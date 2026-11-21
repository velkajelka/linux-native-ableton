// Max for Live device views: hidden when Live does not show them, placed and
// drawn at the window's scale.
//
// A Max device draws its interface in an X window of its own (the Max
// runtime's JUCE peer), inside a host window that Live's X11 platform-view
// code places (Live+0x2002150, "x11: failed to move/resize platform view"):
// it unmaps the host for an empty visible rect, otherwise sizes the host to
// the visible rect, offsets the content by (full - visible) and maps it.
//
// Live calls that code from its platform-view pass (Live+0x2074fd0), which
// runs on every frame but places a view only when its rect changed and it is
// on screen. On Windows and macOS the views are children of the device
// chain's own native view, so hiding or scrolling the chain hides them too;
// on Linux everything is one window and nothing did:
//   - in Clip view and with the detail area collapsed the views stayed on
//     screen (over the clip editor or the mixer);
//   - a device scrolled out of the chain in one step kept its last, partly
//     visible host: a sliver of its interface over the next device.
// Live knows the view is not visible: its visible-rect function
// (Live+0x20a74d0, called by the pass for the views it places) returns an
// empty rect for exactly these views. After each pass, a host whose view's
// visible rect is empty is unmapped; Live maps it again when it places it.
//
// Live's rects are in its own units (the UI zoom is already applied), not
// pixels: at a 125 % desktop scale every view sat at 4/5 of its position and
// size. Rects from the platform-view code are scaled to pixels here, and the
// Max runtime's JUCE scale (juce::Desktop::setGlobalScaleFactor, exported by
// libMaxPlugLib.so) is set to the same factor so it draws to fill them. A
// scale change while Live runs (scalechange.cpp) re-places the views; the
// views that already exist take the new scale only when they are shown again
// (checked: Clip view and back fixes them, a JUCE screen-change notice does
// not), so 400 ms after the change every placed host is hidden and shown once.
//
// Live 11.3.43 has the same X11 platform-view layer (APlatformViewHostRegistry):
// place-view 0x1ba0600 (same strings, same arguments: host window, visible rect,
// full rect; content child first, then the host, then XMapWindow; reached through
// the jmp thunk 0x1b91ea0), pass 0x1c037c0 ("APlatformViewHostRegistry::
// SyncPlatformViews"; same arguments, list of views at registry +0x28 with next
// +0, view +8, host window +0x10; rects in Live's units: zoom from 0x1c588e0,
// = zoom * window scale virtual +0x5a8, 1.0 by default, like Live 12's 0x20c40e0)
// and visible rect 0x1c36a30 (same code as 0x20a74d0: parent +0x60, rect +0x30).
// One difference: Live 11's place-view has no empty-rect case. Live 12's unmaps
// the host when the visible (or full) rect is empty; Live 11's sizes it to the
// empty rect (XMoveResizeWindow with width or height 0: BadValue, the host keeps
// its old size) and maps it, so a device scrolled out of the chain left its
// last sliver on screen even while Live placed it. On Live 11 place-view is
// hooked to do what Live 12's does: unmap the host for an empty rect, through
// Live's own X connection (0x8195548 -> object, Display * at +0x20, as
// place-view loads it at 0x1ba064e).
//
// Every Live address is checked against its expected bytes first; on another
// build nothing here is active. LIVE_MAX_VIEWS=0 turns all of it off.
#include "livebuild.h"
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QHash>
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <link.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
bool maxWheelInstall(); // maxwheel.cpp
void *hookFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *replacement); // hook.cpp

namespace {

uintptr_t kPlaceView = 0x2002150, kPlaceViewEnd = 0x20024c0; // X11 platform view: set bounds
uintptr_t kViewPass = 0x2074fd0;                              // platform-view pass
uintptr_t kVisibleRect = 0x20a74d0;                           // view's visible rect (own coordinates)
constexpr unsigned char kPlaceViewPrologue[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40};
constexpr unsigned char kViewPassPrologue[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                               0x54, 0x53, 0x48, 0x81, 0xec, 0x18, 0x01, 0x00, 0x00};
constexpr unsigned char kVisibleRectPrologue[] = {0x53, 0x48, 0x89, 0xfb, 0x48, 0x39, 0xfe, 0x74, 0x70};

// Live 11.3.43 (see the header): place-view (size 0x46c), pass, visible rect, and
// Live's X connection with the instruction in place-view that loads it.
constexpr uintptr_t kPlaceView11 = 0x1ba0600, kPlaceViewEnd11 = 0x1ba0a6c;
constexpr uintptr_t kViewPass11 = 0x1c037c0;
constexpr uintptr_t kVisibleRect11 = 0x1c36a30;
constexpr uintptr_t kConnection11 = 0x8195548, kConnectionLoad11 = 0x1ba064e;
constexpr unsigned char kPlaceViewPrologue11[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                                  0x54, 0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00};
constexpr unsigned char kViewPassPrologue11[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                                 0x54, 0x53, 0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00};
constexpr unsigned char kVisibleRectPrologue11[] = {0x53, 0x48, 0x89, 0xfb, 0x48, 0x39, 0xfe, 0x74, 0x5f};
constexpr unsigned char kConnectionLoadBytes11[] = {0x48, 0x8b, 0x05, 0xf3, 0x4e, 0x5f, 0x06}; // mov rax, [rip + 0x65f4ef3]

struct Rect {
    int32_t x1, y1, x2, y2;
};
using PassFn = void (*)(void *, void *, void *);
using VisibleRectFn = Rect (*)(void *view, int);
using MoveResizeFn = int (*)(Display *, Window, int, int, unsigned, unsigned);
using PlaceFn = void (*)(Window host, const Rect *visible, const Rect *full);

uintptr_t base;
bool active;
PassFn originalPass;
VisibleRectFn visibleRect;
PlaceFn originalPlace; // Live 11 only

struct Placed {
    Display *dpy;
    int x, y;
    unsigned width, height; // Live's units
    Window host;            // for a content window: its host (the code places the content first)
};
Window lastContent;
QHash<Window, Placed> placed;    // host and content windows, as Live last placed them
QHash<Window, Display *> mapped; // hosts Live mapped
QHash<Window, Display *> hidden; // hosts unmapped here
double ratio = 1.0;              // pixels per Live unit applied to the placements
double passRatio = 0;            // the scale the last pass saw (0: none yet)
float juceScale = 0;             // the Max runtime's scale as set here (0: not yet)
QElapsedTimer sinceScaleChange;  // a Max view re-applies the scale when it is shown again

bool fromPlaceView(void *ret)
{
    const uintptr_t r = reinterpret_cast<uintptr_t>(ret);
    return active && r >= base + kPlaceView && r < base + kPlaceViewEnd;
}

double currentRatio()
{
    const double r = qApp ? qApp->devicePixelRatio() : 1.0;
    return r > 0 ? r : 1.0;
}

// Live's rect in its units -> pixels, edge by edge (neighbouring views stay flush).
void toPixels(const Placed &p, int &x, int &y, unsigned &width, unsigned &height)
{
    x = int(std::lround(p.x * ratio));
    y = int(std::lround(p.y * ratio));
    width = unsigned(std::max(1L, std::lround((p.x + int(p.width)) * ratio) - x));
    height = unsigned(std::max(1L, std::lround((p.y + int(p.height)) * ratio) - y));
}

MoveResizeFn realMoveResize()
{
    static auto fn = reinterpret_cast<MoveResizeFn>(dlsym(RTLD_NEXT, "XMoveResizeWindow"));
    return fn;
}

// The Max runtime draws at JUCE's scale; it is loaded with the first Max device.
void followJuceScale()
{
    if (juceScale == float(ratio))
        return;
    static void *(*getDesktop)() = nullptr;
    static void (*setScale)(void *, float) = nullptr;
    if (!setScale) {
        getDesktop = reinterpret_cast<void *(*)()>(dlsym(RTLD_DEFAULT, "_ZN4juce7Desktop11getInstanceEv"));
        setScale = reinterpret_cast<void (*)(void *, float)>(dlsym(RTLD_DEFAULT, "_ZN4juce7Desktop20setGlobalScaleFactorEf"));
        if (!getDesktop || !setScale)
            return;
    }
    if (void *desktop = getDesktop()) {
        // Live 11's Max runtime (Max 8.5.8) applies JUCE's global scale twice:
        // with 1.25 its interface came out 1.56 times its size (an LFO's Map
        // box 112 px instead of 72 x 1.25 = 90) and was cut off by its frame.
        // It gets the square root, which draws it at the desktop's scale, as
        // Live 12's Max 9 draws with the factor itself.
        setScale(desktop, liveBuild() == LiveBuild::Live11_3_43 ? float(std::sqrt(ratio)) : float(ratio));
        juceScale = float(ratio);
        menufixTrace("maxviews: Max runtime scale %d/100\n", int(std::lround(ratio * 100)));
    }
}

void pass(void *registry, void *view, void *region)
{
    originalPass(registry, view, region);
    if (!registry || placed.isEmpty())
        return; // no Max device has been placed yet
    if (liveBuild() == LiveBuild::Live12_4_7)
        maxWheelInstall(); // wheel over dials and sliders (the Max runtime is loaded by now)
    const double now = currentRatio();
    const bool changed = passRatio != 0 && now != passRatio;
    passRatio = now;
    if (changed) {
        ratio = now;
        followJuceScale(); // desktop scale changed: Live's units stay, the pixels do not
        // A content window the Max runtime destroyed is still listed (it does not
        // call XDestroyWindow through this process's symbols): ignore BadWindow.
        Display *d = placed.cbegin()->dpy;
        XSync(d, False);
        auto previous = XSetErrorHandler([](Display *, XErrorEvent *) { return 0; });
        for (auto it = placed.cbegin(); it != placed.cend(); ++it) {
            int x, y;
            unsigned width, height;
            toPixels(*it, x, y, width, height);
            realMoveResize()(it->dpy, it.key(), x, y, width, height);
        }
        XSync(d, False);
        XSetErrorHandler(previous);
        menufixTrace("maxviews: %d windows re-placed at %d/100\n", int(placed.size()), int(std::lround(ratio * 100)));
        sinceScaleChange.start();
    }
    // Views that existed before the change keep drawing at the old scale (JUCE gives
    // the new one only to new views, and Max re-reads it when a view is shown). Once
    // Live and Max have settled, hide and show every placed host for one frame.
    if (sinceScaleChange.isValid() && sinceScaleChange.elapsed() > 400) {
        sinceScaleChange.invalidate();
        for (auto it = mapped.cbegin(); it != mapped.cend(); ++it) {
            XUnmapWindow(it.value(), it.key());
            XMapWindow(it.value(), it.key());
        }
        if (!mapped.isEmpty())
            XFlush(mapped.cbegin().value());
        menufixTrace("maxviews: %d views shown again for the new scale\n", int(mapped.size()));
    }
    followJuceScale();
    // The registry's list of platform views: next at +0, view at +8, host window at +0x10.
    Display *flush = nullptr;
    int n = 0;
    for (void *node = *reinterpret_cast<void **>(static_cast<char *>(registry) + 0x28); node && n < 512;
         node = *reinterpret_cast<void **>(node), ++n) {
        void *v = *reinterpret_cast<void **>(static_cast<char *>(node) + 0x08);
        const Window host = *reinterpret_cast<Window *>(static_cast<char *>(node) + 0x10);
        if (!v || !host)
            continue;
        const Rect r = visibleRect(v, 0);
        const bool visible = r.x2 > r.x1 && r.y2 > r.y1;
        if (!visible) {
            if (Display *d = mapped.take(host)) {
                XUnmapWindow(d, host);
                hidden.insert(host, d);
                flush = d;
            }
        } else if (Display *d = hidden.value(host)) {
            // Live shows it again without placing it (its rect did not change).
            const auto p = placed.constFind(host);
            if (p != placed.cend() && p->width == unsigned(r.x2 - r.x1) && p->height == unsigned(r.y2 - r.y1)) {
                hidden.remove(host);
                XMapWindow(d, host);
                mapped.insert(host, d);
                flush = d;
            }
        }
    }
    if (flush)
        XFlush(flush);
}

// Live 11's place-view with Live 12's empty-rect case (Live 12 0x2002150: either
// rect empty -> XUnmapWindow(host), nothing else). The unmap is not made from
// place-view's code, so its bookkeeping (maxViewUnmapped) is done here.
void place11(Window host, const Rect *visible, const Rect *full)
{
    if (visible->x1 < visible->x2 && visible->y1 < visible->y2 && full->x1 < full->x2 && full->y1 < full->y2) {
        originalPlace(host, visible, full);
        return;
    }
    void *connection = *reinterpret_cast<void **>(base + kConnection11);
    Display *d = connection ? *reinterpret_cast<Display **>(static_cast<char *>(connection) + 0x20) : nullptr;
    if (!d)
        return;
    XUnmapWindow(d, host);
    mapped.remove(host);
    hidden.remove(host);
}

__attribute__((constructor)) void installMaxViews()
{
    const bool live11 = liveBuild() == LiveBuild::Live11_3_43;
    if (!live11 && liveBuild() != LiveBuild::Live12_4_7)
        return; // not ported to this Live build
    const char *off = getenv("LIVE_MAX_VIEWS");
    if (off && !strcmp(off, "0"))
        return;
    dl_iterate_phdr([](dl_phdr_info *info, size_t, void *out) { *static_cast<uintptr_t *>(out) = info->dlpi_addr; return 1; }, &base);
    const unsigned char *placeViewPrologue = kPlaceViewPrologue, *viewPassPrologue = kViewPassPrologue,
                        *visibleRectPrologue = kVisibleRectPrologue;
    size_t placeViewLen = sizeof kPlaceViewPrologue, viewPassLen = sizeof kViewPassPrologue,
           visibleRectLen = sizeof kVisibleRectPrologue;
    if (live11) {
        kPlaceView = kPlaceView11;
        kPlaceViewEnd = kPlaceViewEnd11;
        kViewPass = kViewPass11;
        kVisibleRect = kVisibleRect11;
        placeViewPrologue = kPlaceViewPrologue11;
        placeViewLen = sizeof kPlaceViewPrologue11;
        viewPassPrologue = kViewPassPrologue11;
        viewPassLen = sizeof kViewPassPrologue11;
        visibleRectPrologue = kVisibleRectPrologue11;
        visibleRectLen = sizeof kVisibleRectPrologue11;
    }
    if (memcmp(reinterpret_cast<void *>(base + kPlaceView), placeViewPrologue, placeViewLen)
        || memcmp(reinterpret_cast<void *>(base + kVisibleRect), visibleRectPrologue, visibleRectLen)
        || memcmp(reinterpret_cast<void *>(base + kViewPass), viewPassPrologue, viewPassLen)
        || (live11 && memcmp(reinterpret_cast<void *>(base + kConnectionLoad11), kConnectionLoadBytes11,
                             sizeof kConnectionLoadBytes11))) {
        menufixTrace("maxviews: not this Live build, off\n");
        return;
    }
    visibleRect = reinterpret_cast<VisibleRectFn>(base + kVisibleRect);
    if (live11) {
        originalPlace = reinterpret_cast<PlaceFn>(
            hookFunction(base + kPlaceView, placeViewPrologue, placeViewLen, reinterpret_cast<void *>(&place11)));
        if (!originalPlace) {
            menufixTrace("maxviews: place-view hook failed, off\n");
            return;
        }
    }
    originalPass = reinterpret_cast<PassFn>(
        hookFunction(base + kViewPass, viewPassPrologue, viewPassLen, reinterpret_cast<void *>(&pass)));
    active = originalPass != nullptr;
    menufixTrace("maxviews: hooks %d\n", int(active));
}

} // namespace

// From pluginclose.cpp's X interposers. Returns false when the call is not Live's platform-view code.
bool maxViewMoveResize(Display *dpy, Window w, int &x, int &y, unsigned &width, unsigned &height, void *ret)
{
    if (!fromPlaceView(ret))
        return false;
    ratio = currentRatio();
    const Placed p{dpy, x, y, width, height, 0};
    placed.insert(w, p);
    lastContent = w;
    toPixels(p, x, y, width, height);
    return true;
}

void maxViewMapped(Display *dpy, Window w, void *ret)
{
    if (!fromPlaceView(ret))
        return;
    hidden.remove(w);
    mapped.insert(w, dpy);
    // The content window placed just before belongs to this host.
    if (lastContent && lastContent != w) {
        auto it = placed.find(lastContent);
        if (it != placed.end() && !it->host)
            it->host = w;
    }
    lastContent = 0;
}

void maxViewUnmapped(Window w, void *ret)
{
    if (!fromPlaceView(ret))
        return;
    mapped.remove(w);
    hidden.remove(w);
}

// The platform view was destroyed (or its window is gone): forget it.
void maxViewForget(Window w)
{
    if (placed.remove(w))
        for (auto it = placed.begin(); it != placed.end();)
            it = it->host == w ? placed.erase(it) : std::next(it);
    mapped.remove(w);
    hidden.remove(w);
}
