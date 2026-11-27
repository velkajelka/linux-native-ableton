// LIVE_WHEEL_TRACE=1 (with MENUFIX_DEBUG=<file>): logs how scroll reaches Live.
//
// Three points on the path, one line each, with a shared millisecond clock:
//   xi2   a native XInput2 event Qt is about to read: XI_Motion with valuators
//         (smooth scroll deltas arrive as valuators of XI_Motion), or button
//         4-7 press (legacy wheel). flags bit 16 = pointer emulated.
//   qt    the QWheelEvent Qt built from it: position in the receiver, global
//         position, angleDelta (120 per notch), pixelDelta (touchpads), source,
//         phase, inverted flag, and the receiver's class and window.
//   under the widget Qt's own idea of the widget under the pointer, to compare
//         with the control that actually changed.
// Reads only; nothing is changed. Off unless LIVE_WHEEL_TRACE=1.
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QWheelEvent>
#include <QWidget>
#include <X11/Xlib.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cxxabi.h>
#include <dlfcn.h>
#include <link.h>
#include <typeinfo>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

namespace {

QElapsedTimer clockMs;

void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void line(const char *fmt, ...) {
  const char *path = getenv("MENUFIX_DEBUG");
  if (!path)
    return;
  FILE *f = fopen(path, "a");
  if (!f)
    return;
  fprintf(f, "wheel %6lld ", static_cast<long long>(clockMs.elapsed()));
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fclose(f);
}

struct XiTrace : QAbstractNativeEventFilter {
  uint8_t xinput = 0;
  bool nativeEventFilter(const QByteArray &, void *message, long *) override {
    auto *generic = static_cast<xcb_ge_generic_event_t *>(message);
    if ((generic->response_type & 0x7f) != XCB_GE_GENERIC || generic->extension != xinput)
      return false;
    const uint16_t type = generic->event_type;
    if (type != XCB_INPUT_MOTION && type != XCB_INPUT_BUTTON_PRESS)
      return false;
    auto *e = static_cast<xcb_input_motion_event_t *>(message);
    if (type == XCB_INPUT_BUTTON_PRESS) {
      if (e->detail < 4 || e->detail > 7)
        return false; // only the legacy wheel buttons
    } else if (e->valuators_len == 0) {
      return false; // plain motion with no valuators
    }
    line("xi2 %s detail %u flags 0x%x dev %u src %u event %u pos %.2f,%.2f root %.2f,%.2f valmask "
         "%u\n",
         type == XCB_INPUT_MOTION ? "motion" : "button", e->detail, e->flags, e->deviceid,
         e->sourceid, e->event, e->event_x / 65536.0, e->event_y / 65536.0, e->root_x / 65536.0,
         e->root_y / 65536.0, e->valuators_len);
    return false;
  }
};

struct QtTrace : QObject {
  // RTTI name and vtable of the receiver, once per class, plus which vtable slots still
  // point at QWidget::wheelEvent (an unchanged slot means the class never overrides it).
  static void describeOnce(QObject *o) {
    static QSet<QByteArray> seen;
    const char *mangled = typeid(*o).name();
    if (seen.contains(mangled))
      return;
    seen.insert(mangled);
    int status = 0;
    char *name = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    uintptr_t base = 0;
    dl_iterate_phdr(
        [](dl_phdr_info *i, size_t, void *out) {
          *static_cast<uintptr_t *>(out) = i->dlpi_addr;
          return 1;
        },
        &base);
    void **vt = *reinterpret_cast<void ***>(o);
    void *qwheel = dlsym(RTLD_DEFAULT, "_ZN7QWidget10wheelEventEP11QWheelEvent");
    QByteArray hits;
    for (int k = 0; k < 120; ++k)
      if (vt[k] == qwheel)
        hits += QByteArray::number(k) + ' ';
    line("class %s vtable main+0x%lx QWidget::wheelEvent %p in slots: %s\n", name ? name : mangled,
         reinterpret_cast<uintptr_t>(vt) - base, qwheel, hits.constData());
    free(name);
  }
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() != QEvent::Wheel)
      return false;
    auto *w = static_cast<QWheelEvent *>(event);
    QWidget *under = QApplication::widgetAt(QCursor::pos());
    describeOnce(watched);
    line("qt   wheel on %s(%s) pos %.1f,%.1f global %.1f,%.1f angle %d,%d pixel %d,%d src %d phase "
         "%d inv %d "
         "buttons 0x%x mods 0x%x | cursor %d,%d under %s\n",
         watched->metaObject()->className(), qPrintable(watched->objectName()), w->position().x(),
         w->position().y(), w->globalPosition().x(), w->globalPosition().y(), w->angleDelta().x(),
         w->angleDelta().y(), w->pixelDelta().x(), w->pixelDelta().y(), int(w->source()),
         int(w->phase()), int(w->inverted()), int(w->buttons()), int(w->modifiers()),
         QCursor::pos().x(), QCursor::pos().y(), under ? under->metaObject()->className() : "none");
    return false;
  }
};

} // namespace

void installWheelTrace() {
  const char *on = getenv("LIVE_WHEEL_TRACE");
  if (!on || *on != '1' || !getenv("MENUFIX_DEBUG"))
    return;
  clockMs.start();
  static XiTrace xi;
  static QtTrace qt;
  int opcode, event, error;
  Display *dpy = XOpenDisplay(nullptr);
  if (dpy && XQueryExtension(dpy, "XInputExtension", &opcode, &event, &error)) {
    xi.xinput = uint8_t(opcode);
    QCoreApplication::instance()->installNativeEventFilter(&xi);
  }
  if (dpy)
    XCloseDisplay(dpy);
  QCoreApplication::instance()->installEventFilter(&qt);
  line("trace installed (xinput opcode %d)\n", xi.xinput);
}
