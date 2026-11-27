// Mouse wheel / trackpad scroll over knobs, sliders and number boxes (LIVE_WHEEL_VALUES=0 turns it
// off).
//
// Live's TQtView::wheelEvent already forwards every wheel event to
// AViewEventHandler::DispatchMouseWheelEvent, which hit-tests the view tree and
// asks each view whether it wants the wheel: view vtable slot 102
// (bool WantsWheel(point, delta, axis), base: return false) and then calls slot
// 104 (void OnWheel(point, delta, axis), base: nothing), but only views with a flag
// bit set (see kWheelHitFlag) are asked. Only scrollers
// (AScrollerView) implement them, so a wheel over a knob, fader or number box did
// nothing. The classes below get both slots and the wheel becomes a vertical drag, so values,
// undo, automation and modulation behave as for a mouse drag: through the class's own OnDrag
// (slot 96) where it has one (knobs, number boxes, most sliders), else through a synthetic
// press, move and release (the mixer's volume pack, vertical sliders such as the clip Gain).
// About 3 px per notch, a fifth of that with Ctrl or Shift held. Fractions of a pixel are
// carried over so touchpads scroll smoothly.
//
// Addresses are Live 12.4.7's (vtables found by RTTI); other builds are left alone.
#include "livebuild.h"
#include <QApplication>
#include <QCursor>
#include <QMouseEvent>
#include <QTimer>
#include <QWidget>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <link.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

namespace {

constexpr int kSlotDrag = 96, kSlotWants = 102, kSlotWheel = 104;
constexpr int kSlotHit =
    70; // bool Hit(point, hit type): called while the pointer moves over the view
constexpr uintptr_t kBaseWants = 0x20ab4d0, kBaseWheel = 0x20ab550,
                    kBaseHit = 0x20aad80; // AView defaults
// A view only joins the wheel hit list (hit type 4) when bit 9 of its flags word
// (offset 0x40; bit 8 is the mouse hit type) is set. Scrollers set it themselves; the
// value controls get it here, the first time the pointer is over them, which is always
// before they are scrolled.
constexpr int kFlags = 0x40;
constexpr uint32_t kWheelHitFlag = 0x200;
constexpr uintptr_t kBaseDrag = 0x20ab460; // AView default OnDrag (does nothing)
constexpr int kSlotValueDrag = 108;
constexpr uintptr_t kBaseValueDrag = 0x2198140; // AValueControl default of kSlotValueDrag
constexpr double kPixelsPerDelta = 0.04, kFinePixelsPerDelta = 0.008;

uintptr_t base;

// Itanium RTTI: the vtable's slot -1 is the type_info, whose second word is the mangled name.
const char *className(void **vt) {
  auto **ti = static_cast<void **>(vt[-1]);
  return ti ? static_cast<const char *>(ti[1]) : nullptr;
}

// Knobs ("Poti"), sliders, number boxes and the mixer's volume pack; not text fields, buttons or
// dialogs.
bool wantedClass(const char *name) {
  const std::string n(name);
  static const char *const want[] = {"Slider", "Poti", "EditField", "VolumePack", "Fader"};
  static const char *const skip[] = {"Dialog",  "Converter", "Title",      "SingleLine", "History",
                                     "Time",    "Peak",      "Button",     "Member",     "WeakPtr",
                                     "Manager", "Displayer", "Remoteable", "Check"};
  for (const char *s : skip)
    if (n.find(s) != std::string::npos)
      return false;
  for (const char *w : want)
    if (n.find(w) != std::string::npos)
      return true;
  return false;
}

using HitFn = bool (*)(void *, uint64_t, int);
HitFn originalHit;

bool hitAndMarkWheel(void *self, uint64_t point, int type) {
  const bool hit = originalHit(self, point, type);
  if (hit)
    *reinterpret_cast<uint32_t *>(static_cast<char *>(self) + kFlags) |= kWheelHitFlag;
  return hit;
}

int wantsWheel(void *, uint64_t, int delta, int) { return delta != 0; }

// Controls without a usable OnDrag follow mouse events instead: a synthetic press, move and
// release on the view under the pointer. Live reads the pointer itself during a drag, so
// wheelSynthPointer() (pointerwarp.cpp) reports a virtual pointer while it runs.
bool synthActive;
QPoint synthPointer;

void synthDrag(int dy) {
  if (synthActive)
    return;
  const QPoint global = QCursor::pos();
  QWidget *view = QApplication::widgetAt(global);
  if (!view)
    return;
  const Qt::KeyboardModifiers mods = QGuiApplication::keyboardModifiers();
  const QPointF local = view->mapFromGlobal(global);
  auto send = [&](QEvent::Type type, const QPointF &at, Qt::MouseButtons buttons) {
    QMouseEvent ev(type, at, view->mapToGlobal(at.toPoint()), Qt::LeftButton, buttons, mods);
    QCoreApplication::sendEvent(view, &ev);
  };
  synthActive = true;
  synthPointer = global;
  send(QEvent::MouseButtonPress, local, Qt::LeftButton);
  synthPointer = global + QPoint(0, dy);
  send(QEvent::MouseMove, local + QPointF(0, dy), Qt::LeftButton);
  send(QEvent::MouseButtonRelease, local + QPointF(0, dy), Qt::NoButton);
  synthActive = false;
}

// Whole pixels of drag for a wheel delta; the fraction stays in carry.
int wheelPixels(double &carry, int delta) {
  const bool fine =
      QGuiApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
  carry += -delta * (fine ? kFinePixelsPerDelta : kPixelsPerDelta);
  const int dy = int(carry);
  carry -= dy;
  return dy;
}

void applyWheelSynth(void *, uint64_t, int delta, int) {
  static double carry;
  const int dy = wheelPixels(carry, delta);
  if (!dy)
    return;
  QTimer::singleShot(0, [dy] { synthDrag(dy); }); // not from inside Live's wheel dispatch
}

void applyWheel(void *self, uint64_t, int delta, int) {
  static void *last;
  static double carry;
  if (self != last) {
    last = self;
    carry = 0;
  }
  const int dy = wheelPixels(carry, delta);
  if (!dy)
    return;
  void **vt = *reinterpret_cast<void ***>(self);
  using DragFn = void (*)(void *, uint64_t, int, int);
  reinterpret_cast<DragFn>(vt[kSlotDrag])(self, uint64_t(uint32_t(dy)) << 32, 0, 1);
}

} // namespace

// The pointer Live's drag code reads while a wheel drag runs (nullptr: only ask whether one does).
bool wheelSynthPointer(QPoint *position) {
  if (!synthActive)
    return false;
  if (position)
    *position = synthPointer;
  return true;
}

void installWheelValues() {
  const char *off = getenv("LIVE_WHEEL_VALUES");
  if (off && *off == '0')
    return;
  if (liveBuild() != LiveBuild::Live12_4_7)
    return;
  dl_iterate_phdr(
      [](dl_phdr_info *info, size_t, void *out) {
        *static_cast<uintptr_t *>(out) = info->dlpi_addr; // first entry = main program
        return 1;
      },
      &base);
  int patched = 0, seen = 0;
  const long page = sysconf(_SC_PAGESIZE);
  struct Scan {
    std::vector<void **> found;
  } scan;
  // Every vtable of a view class holds AView's default hit function in slot 70:
  // find them by scanning the program's data for that pointer.
  dl_iterate_phdr(
      [](dl_phdr_info *info, size_t, void *out) {
        auto *s = static_cast<Scan *>(out);
        for (int i = 0; i < info->dlpi_phnum; ++i) {
          const ElfW(Phdr) &ph = info->dlpi_phdr[i];
          if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_R) || (ph.p_flags & PF_X))
            continue;
          auto *lo = reinterpret_cast<uintptr_t *>(info->dlpi_addr + ph.p_vaddr);
          auto *hi = lo + ph.p_filesz / 8;
          for (uintptr_t *p = lo + kSlotHit + 2; p < hi; ++p)
            if (*p == base + kBaseHit && p[-kSlotHit - 1] && !p[-kSlotHit - 2])
              s->found.push_back(reinterpret_cast<void **>(p - kSlotHit));
        }
        return 1; // the main program only
      },
      &scan);
  for (void **vt : scan.found) {
    const char *name = className(vt);
    if (!name || !wantedClass(name))
      continue;
    ++seen;
    // AModulatedVerticalSlider (the clip Gain slider) has an OnDrag, but it only works inside a
    // press: called without one it does nothing. Vertical sliders take the synthetic-mouse path.
    const bool dragSlot = reinterpret_cast<uintptr_t>(vt[kSlotDrag]) != base + kBaseDrag &&
                          !strstr(name, "VerticalSlider");
    if (reinterpret_cast<uintptr_t>(vt[kSlotDrag]) == base + kBaseDrag &&
        reinterpret_cast<uintptr_t>(vt[kSlotValueDrag]) == base + kBaseValueDrag)
      continue; // neither drag path
    if (reinterpret_cast<uintptr_t>(vt[kSlotWants]) != base + kBaseWants ||
        reinterpret_cast<uintptr_t>(vt[kSlotWheel]) != base + kBaseWheel)
      continue; // already handles the wheel (scrollers) or another layout
    void **first = &vt[kSlotHit];
    const uintptr_t lo = reinterpret_cast<uintptr_t>(first) & ~uintptr_t(page - 1);
    const uintptr_t hi =
        (reinterpret_cast<uintptr_t>(&vt[kSlotWheel + 1]) + page - 1) & ~uintptr_t(page - 1);
    if (mprotect(reinterpret_cast<void *>(lo), hi - lo, PROT_READ | PROT_WRITE))
      continue;
    originalHit = reinterpret_cast<HitFn>(base + kBaseHit);
    vt[kSlotHit] = reinterpret_cast<void *>(&hitAndMarkWheel);
    vt[kSlotWants] = reinterpret_cast<void *>(&wantsWheel);
    vt[kSlotWheel] = reinterpret_cast<void *>(dragSlot ? &applyWheel : &applyWheelSynth);
    ++patched;
  }
  menufixTrace("wheelvalues: %d patched of %d candidate classes\n", patched, seen);
}
