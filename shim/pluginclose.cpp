// Window-manager close for plug-in editor windows in the Push 3 Live build.
//
// Live creates plug-in editor windows on its own Xlib connection but never
// sets WM_PROTOCOLS, and registers no X event handlers on that connection.
// Clicking the window's close button therefore makes KWin kill the whole
// client connection (XKillClient), and Live exits from _XIOError on its next
// XSync.
//
// Fix:
//  1. Hook Live's "editor open" property observer (0x3f132a0, reached through
//     the thunk at 0x3f2b9a0) to know which property is being applied while
//     the editor window is created.
//  2. Interpose XCreateWindow: top-level windows created during that call get
//     WM_DELETE_WINDOW and are remembered as window -> property.
//  3. Interpose XNextEvent: a WM_DELETE_WINDOW for such a window sets the
//     property to false through Live's own setter (0x1e924c0), the same path
//     as the device's wrench button. Deferred to the next Qt loop pass.
//  4. Interpose XDestroyWindow to forget closed windows.
//
// Plug-in editor resizing: when a plug-in asks for another size
// (IPlugFrame::resizeView, e.g. u-he's GUI scale), Live calls
// XMoveResizeWindow on the editor (0x3e93692) but leaves the WM_NORMAL_HINTS
// it set at creation, min = max = the first size. The window manager clamps
// the request to those hints, so the window keeps its old size and the
// plug-in's view sits in a corner of it with stale pixels around it. The
// resize interposers below move the size hints along before resizing. Live
// also passes the window's client position, which a reparenting window
// manager reads as the frame position (default NorthWest gravity), so every
// resize moved the editor down by the title bar. Editor windows get
// StaticGravity in their hints, as Qt's own windows have, so client
// coordinates mean client coordinates.
//
// Opening: Live creates the editor at the size from IPlugView::getSize, calls
// attached(), where the plug-in creates its view at its real size (u-he at a
// saved GUI scale: 1080x540 in a 1200x600 window), maps the window, and only
// ~130 ms later gets the plug-in's resizeView. The window appeared at the
// first size and then changed. The XMapWindow interposer sizes the editor to
// the plug-in's view just before the map, so it shows once at its final size;
// the later resizeView is then a no-op. Live also hides and re-shows an
// editor within a few ms when it re-selects the device (after a drop); on
// KWin each unmap/map plays the close and open animations, so the editor
// looked opened twice. An editor's unmap is deferred by 50 ms and dropped
// together with a map that arrives in that time. Editor hints carry
// USPosition/PPosition, so KWin keeps Live's position instead of centring the
// transient window.
// Resizing an editor from its window frame: Live gets no event for it (it
// selects none on its plug-in connection), so a resizable plug-in (Pro-Q 3,
// through yabridge) kept drawing at its old size in a bigger, partly black
// window. Windows Live answers WM_SIZE with IPlugView::checkSizeConstraint and
// onSize. Here editor windows get StructureNotify on Live's connection, and a
// ConfigureNotify whose size is not the one Live or this file last gave the
// window is passed to the plug-in the same way (the view comes from Live's
// editor creation, vst3runloop.cpp): the size it accepts is applied to the
// window, then onSize. A plug-in that cannot resize gets its own size back.
// Addresses are for Live 12.4.7 (Push 3 2.4.7); the hook is skipped if the
// thunk does not hold the expected bytes.
#include <QApplication>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp
void liveOwnPluginWindow(Display *dpy, Window editor); // window.cpp
void *hookFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *replacement); // hook.cpp

// VST3 IPlugView (pluginterfaces/gui/iplugview.h), only what is called here.
// Not in the anonymous namespace: GCC would then know that nothing in this
// program implements it and compile the calls to __cxa_pure_virtual (Live
// aborted on the first resize from the window frame).
namespace pluginview {

struct ViewRect {
    int32_t left, top, right, bottom;
};
struct IPlugView {
    virtual int32_t queryInterface(const char *iid, void **obj) = 0;
    virtual uint32_t addRef() = 0;
    virtual uint32_t release() = 0;
    virtual int32_t isPlatformTypeSupported(const char *type) = 0;
    virtual int32_t attached(void *parent, const char *type) = 0;
    virtual int32_t removed() = 0;
    virtual int32_t onWheel(float distance) = 0;
    virtual int32_t onKeyDown(char16_t key, int16_t keyCode, int16_t modifiers) = 0;
    virtual int32_t onKeyUp(char16_t key, int16_t keyCode, int16_t modifiers) = 0;
    virtual int32_t getSize(ViewRect *size) = 0;
    virtual int32_t onSize(ViewRect *newSize) = 0;
    virtual int32_t onFocus(uint8_t state) = 0;
    virtual int32_t setFrame(void *frame) = 0;
    virtual int32_t canResize() = 0;
    virtual int32_t checkSizeConstraint(ViewRect *rect) = 0;
};
constexpr int32_t kResultTrue = 0;

} // namespace pluginview
using pluginview::IPlugView;
using pluginview::ViewRect;
using pluginview::kResultTrue;

namespace {

// Defined below; the layout hook uses it to tell Live's calls from this file's.
bool calledFromLive(void *ret);

// Max for Live device views: the native X children of Live's window that the
// shim repositions. They have to disappear when the device view is not shown
// (Clip view) - Live itself never hides them for that case, but it does hide
// them when a track's devices change, so unmapping is a state it handles.
QSet<Window> maxDeviceViews;
Window maxDeviceParent = 0; // Live's window, parent of the Max device hosts
bool contentCheckRequested = false; // reconcile the Max content after a layout
bool maxDeviceViewsHidden;

bool managerEnabled()
{
    static const bool on = [] {
        const char *value = getenv("LIVE_M4L_MANAGER");
        return !value || strcmp(value, "0") != 0;
    }();
    return on;
}

qint64 monotonicMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// A snapshot of this process's readable mappings. The manager dereferences
// pointers Live owns (the chain view, its node list, the views), and Live can
// free any of them between passes. Every one of those pointers is checked
// against this snapshot before it is touched, so a stale pointer can only
// stop the manager for a pass, never crash the process.
struct AddressRanges {
    QVector<quintptr> starts;
    QVector<quintptr> ends;
    bool loaded = false;

    // Do not use QFile here: /proc files report size 0, so QFile::atEnd() is
    // already true before the first read and the whole file looks empty.
    // fopen/read behaves like a stream and is what the rest of the shim uses
    // for /proc as well.
    bool load()
    {
        starts.clear();
        ends.clear();
        FILE *maps = fopen("/proc/self/maps", "r");
        if (!maps) {
            loaded = false;
            return false;
        }
        char line[512];
        while (fgets(line, sizeof line, maps)) {
            unsigned long long start = 0, end = 0;
            char perms[8] = {};
            if (sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3)
                continue;
            // Only mappings that can be read are useful here.
            if (perms[0] != 'r' || end <= start)
                continue;
            starts.append(quintptr(start));
            ends.append(quintptr(end));
        }
        fclose(maps);
        loaded = !starts.isEmpty();
        return loaded;
    }

    bool contains(const void *p, size_t len) const
    {
        // Without /proc the checks are skipped and the manager keeps working
        // exactly as it did before the guards existed.
        if (!loaded)
            return true;
        const quintptr a = reinterpret_cast<quintptr>(p);
        if (!a || a + len < a)
            return false;
        // The mappings are sorted by start and do not overlap.
        int lo = 0, hi = starts.size() - 1;
        while (lo <= hi) {
            const int mid = lo + (hi - lo) / 2;
            if (a < starts.at(mid))
                hi = mid - 1;
            else if (a >= ends.at(mid))
                lo = mid + 1;
            else
                return a + len <= ends.at(mid);
        }
        return false;
    }
};

struct LayoutInfo {
    int x1, y1, x2, y2;     // full (unclipped) rect, Live's units
    int vx1, vy1, vx2, vy2; // visible (clipped) rect
    qint64 when;
};
QHash<unsigned long, LayoutInfo> lastLayout; // full rect per host, from Live
void *chainView = nullptr;                   // the chain view object
int viewportLeft = 8;
int viewportRight = 2469;
int viewportParentWidth = 0;    // Live window width when the right edge was learned
int viewportPanelUnits = -1;    // window right minus chain right (side panel), units
double lastWindowUnits = 0.0;   // window width in chain units, as of the last pass
double pendingPanelUnits = -1.0;
int panelAgreements = 0;
bool viewportRightUpdated = false;
qint64 viewportRightWhen = 0;
int viewportTop = 1446;
int viewportBottom = 1615;
// Live's UI zoom is a scale between the chain view's node rectangles and the
// rectangles the layout routine works in. At 100 % zoom it is 1; at 105 % the
// same device has a node width of 441 but a laid-out width of 463.
// Chain right edge, as pixels from the window's right edge. Set once when a
// side panel's width is known, it makes the boundary independent of which
// clips Live happens to emit (and of the order they arrive in).
int rightInsetPixels()
{
    static const int pixels = [] {
        const char *value = getenv("LIVE_M4L_RIGHT_INSET");
        const int n = value ? atoi(value) : 0;
        return n > 0 ? n : -1;
    }();
    return pixels;
}

double managerZoom = 1.0;
double managerOffsetX = 0.0;
double managerOffsetY = 0.0;
qint64 managerWhen = 0;
int mappingNodeY = 0;
QSet<Window> managerHidden;

struct Placement {
    int vx1, vy1, vx2, vy2;
    int fx1, fy1, fx2, fy2;
};
QHash<unsigned long, Placement> lastPlacement;
QSet<unsigned long> nudgedHosts;

// Last laid-out width per host, to tell a translation (info view, instrument
// inserted at the left) from a zoom change, which rescales every view.
QHash<unsigned long, int> lastLayoutW;
double pendingShiftX = 0.0;
double pendingShiftY = 0.0;
int originShiftPasses = 0;

using LayoutFn = void (*)(void *, const int *, const int *);
LayoutFn originalLayout;

void hookedLayout(void *window, const int *visible, const int *full)
{
    // Only Live's own layout calls are authoritative: the manager calls this
    // routine too when it places a view, and a rect it computed from a guess
    // must not become the reference it is checked against.
    if (visible && full && calledFromLive(__builtin_return_address(0))) {
        const qint64 now = monotonicMs();
        lastLayout[(unsigned long)window] = {full[0],    full[1],    full[2],    full[3],
                                             visible[0], visible[1], visible[2], visible[3],
                                             now};
        if (visible[0] > full[0])
            viewportLeft = visible[0];
        // Only a view with something visible says where the chain ends. A
        // view that is entirely outside reports a zero-width rect whose x is
        // its own clamped position, and taking that as the boundary made the
        // right edge unpredictable (and let hosts sit under the side panel).
        if (rightInsetPixels() <= 0 && visible[2] < full[2] && visible[2] > visible[0]
            && visible[3] > visible[1]) {
            // Only a clip that implies a plausible side panel is the chain's
            // edge; a view Live clamped during a transition reports a point or
            // its own position and must not shrink the viewport.
            const bool plausible = lastWindowUnits <= 0.0
                || (lastWindowUnits - double(visible[2]) >= 0.0
                    && lastWindowUnits - double(visible[2]) < lastWindowUnits / 3.0);
            if (plausible) {
            viewportRight = visible[2];
            viewportRightWhen = now;
            viewportRightUpdated = true;
            // The side panel's width is learned as an inset from the window,
            // measured against the window size in effect during this clip. A
            // single clip can be a transient clamp (Live's 800 px startup
            // window produced one), and believing it shrinks the chain and
            // hides the leftmost views, so an inset is adopted only when two
            // consecutive clips agree. A real panel edge repeats whenever a
            // device straddles it.
            if (lastWindowUnits > 0.0) {
                const double panel = lastWindowUnits - double(viewportRight);
                if (panel >= 0.0 && panel < lastWindowUnits / 3.0) {
                    if (qAbs(panel - pendingPanelUnits) <= 2.0) {
                        if (++panelAgreements >= 2)
                            viewportPanelUnits = qRound(panel);
                    } else {
                        pendingPanelUnits = panel;
                        panelAgreements = 1;
                    }
                }
            }
            }
            menufixTrace("maxviews: right from clip win %x vis ", int((unsigned long)window));
            menufixTrace("%d..%d full ", visible[0], visible[2]);
            menufixTrace("%d..%d\n", full[0], full[2]);
        }
        viewportTop = full[1];
        viewportBottom = full[3];
    }
    if (originalLayout)
        originalLayout(window, visible, full);
}

struct MaxViewRect {
    int32_t x1, y1, x2, y2;
};
using ViewRectFn = MaxViewRect (*)(void *);
ViewRectFn viewRectFn;

using ChainLayoutFn = void (*)(void *, void *, void *);
ChainLayoutFn originalChainLayout;

Display *managerDisplay()
{
    static Display *dpy = XOpenDisplay(nullptr);
    return dpy;
}

void hookedChainLayout(void *self, void *arg2, void *arg3)
{
    chainView = self;
    if (self) {
        // Register the device hosts before Live lays them out. XMoveResizeWindow
        // uses this set to scale Live's units to pixels even when a device is
        // small enough that its height no longer identifies it.
        static void *parentFor = nullptr;
        if (self != parentFor) {
            parentFor = self;
            maxDeviceParent = 0;
        }
        void *node = *reinterpret_cast<void **>(static_cast<char *>(self) + 0x28);
        for (int i = 0; node && i < 128; ++i) {
            const unsigned long win = *reinterpret_cast<unsigned long *>(static_cast<char *>(node) + 0x10);
            if (win) {
                maxDeviceViews.insert(win);
                if (!maxDeviceParent) {
                    Window root, parent, *children = nullptr;
                    unsigned count = 0;
                    if (Display *dpy = managerDisplay();
                        dpy && XQueryTree(dpy, win, &root, &parent, &children, &count))
                        maxDeviceParent = parent;
                    if (children)
                        XFree(children);
                }
            }
            node = *reinterpret_cast<void **>(node);
        }
        contentCheckRequested = true;
    }
    if (originalChainLayout)
        originalChainLayout(self, arg2, arg3);
}

using ObserverFn = void (*)(void *controller, void *prop, int changeType);
using SetBoolFn = void (*)(void *prop, bool value);

constexpr uintptr_t kThunk = 0x3f2b9a0;    // jmp 0x3f132a0, padded to 0x3f2b9b0
constexpr uintptr_t kObserver = 0x3f132a0; // editor-open property observer
constexpr uintptr_t kSetBool = 0x1e924c0;  // bool property setter (prop, value)

uintptr_t base;
ObserverFn observer;
SetBoolFn setBool;
void *currentProp;                    // set while the observer runs
QHash<Window, void *> windowProps;    // plug-in editor window -> property
QHash<Window, Display *> pendingUnmaps; // editor unmaps deferred by XUnmapWindow
Atom wmProtocols, wmDeleteWindow;


struct Editor {
    IPlugView *view = nullptr; // Live's view for the window; Live releases it when it closes the editor
    Display *dpy = nullptr;
    int width = 0, height = 0;    // the size Live or this file last gave the window
    int wantWidth = 0, wantHeight = 0; // the window manager's latest size
    bool pending = false;
};
QHash<Window, Editor> editors;
// The view Live created last, for the editor window that follows. No
// reference is taken: not every view Live creates gets a window, and some
// plug-ins destroy theirs regardless of references.
IPlugView *createdView;

void rememberSize(Window w, unsigned width, unsigned height)
{
    auto it = editors.find(w);
    if (it != editors.end()) {
        it->width = int(width);
        it->height = int(height);
    }
}

// The window manager resized an editor: let the plug-in take that size (or
// the nearest it accepts), as Windows Live does on WM_SIZE.
void applyFrameResize(Window w)
{
    auto it = editors.find(w);
    if (it == editors.end())
        return;
    it->pending = false;
    menufixTrace("pluginclose: frame resize to %dx%d, view %d\n", it->wantWidth, it->wantHeight, it->view != nullptr);
    if (!it->view)
        return;
    if (it->wantWidth == it->width && it->wantHeight == it->height)
        return;
    IPlugView *view = it->view;
    ViewRect rect = {0, 0, it->wantWidth, it->wantHeight};
    if (view->canResize() == kResultTrue) {
        view->checkSizeConstraint(&rect);
    } else {
        ViewRect own = {};
        if (view->getSize(&own) != kResultTrue)
            return;
        rect = own;
    }
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
        return;
    Display *dpy = it->dpy;
    if (width != it->wantWidth || height != it->wantHeight)
        XResizeWindow(dpy, w, unsigned(width), unsigned(height)); // also records the size
    else
        rememberSize(w, unsigned(width), unsigned(height));
    rect = {0, 0, width, height};
    const int32_t sized = view->onSize(&rect);
    menufixTrace("pluginclose: onSize %dx%d -> %d\n", width, height, sized);
    XFlush(dpy);
    menufixTrace("pluginclose: editor 0x%x resized by its frame to %dx", int(w), width);
    menufixTrace("%d\n", height);
}

void observerHook(void *controller, void *prop, int changeType)
{
    void *saved = currentProp;
    currentProp = prop;
    observer(controller, prop, changeType);
    currentProp = saved;
}

uintptr_t mainExecutableBase();

// The chain view's list nodes (next at +0, view at +8, window at +0x10).
constexpr size_t kNodeSize = 0x18;
// The view rect getter reads +0x30, +0x38 and follows its +0x60 list.
constexpr size_t kViewSize = 0x68;

// The getter walks the view's +0x60 list without checking anything. Walk the
// same list here first so a stale or corrupt view cannot send the getter into
// unmapped memory. Real chains are a few dozen nodes; the cap only stops a
// cycle.
bool viewLooksValid(const void *view, const AddressRanges &ranges)
{
    const void *node = view;
    for (int i = 0; node && i < 4096; ++i) {
        if (!ranges.contains(node, kViewSize))
            return false;
        node = *reinterpret_cast<void *const *>(static_cast<const char *>(node) + 0x60);
    }
    return !node;
}

bool plausibleRect(const MaxViewRect &r)
{
    constexpr qint64 limit = 1 << 20;
    return qAbs(qint64(r.x1)) < limit && qAbs(qint64(r.y1)) < limit
           && qAbs(qint64(r.x2)) < limit && qAbs(qint64(r.y2)) < limit
           && r.x2 >= r.x1 && r.y2 >= r.y1;
}

void installMaxViewHooks()
{
    if (!managerEnabled())
        return;
    const uintptr_t fn = mainExecutableBase() + 0x2002150;
    const unsigned char expected[13] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                        0x54, 0x53, 0x48, 0x83, 0xec, 0x40};
    unsigned char prologue[sizeof expected];
    memcpy(prologue, reinterpret_cast<void *>(fn), sizeof prologue);
    if (memcmp(prologue, expected, sizeof expected) != 0) {
        menufixTrace("maxviews: layout prologue mismatch\n");
        return;
    }
    originalLayout = reinterpret_cast<LayoutFn>(
        hookFunction(fn, prologue, sizeof prologue, reinterpret_cast<void *>(&hookedLayout)));
    menufixTrace("maxviews: layout hook %d\n", int(bool(originalLayout)));
}

void installChainViewHook()
{
    if (!managerEnabled())
        return;

    // Validate the rect getter before hooking anything: on another build the
    // chain layout prologue can match while this address means something
    // else, and calling it at 30 Hz would be worse than doing nothing.
    const uintptr_t rectFn = mainExecutableBase() + 0x20ab930;
    const unsigned char rectExpected[8] = {0x31, 0xf6, 0x31, 0xc9, 0x48, 0x89, 0xf8, 0x66};
    unsigned char rectPrologue[sizeof rectExpected];
    memcpy(rectPrologue, reinterpret_cast<void *>(rectFn), sizeof rectPrologue);
    if (memcmp(rectPrologue, rectExpected, sizeof rectExpected) != 0) {
        menufixTrace("maxviews: rect getter mismatch, manager off\n");
        return;
    }

    const uintptr_t fn = mainExecutableBase() + 0x2074fd0;
    const unsigned char expected[17] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                        0x54, 0x53, 0x48, 0x81, 0xec, 0x18, 0x01, 0x00, 0x00};
    unsigned char prologue[sizeof expected];
    memcpy(prologue, reinterpret_cast<void *>(fn), sizeof prologue);
    if (memcmp(prologue, expected, sizeof expected) != 0) {
        menufixTrace("maxviews: chain prologue mismatch\n");
        return;
    }
    originalChainLayout = reinterpret_cast<ChainLayoutFn>(
        hookFunction(fn, prologue, sizeof prologue, reinterpret_cast<void *>(&hookedChainLayout)));
    viewRectFn = reinterpret_cast<ViewRectFn>(rectFn);
    menufixTrace("maxviews: chain hook %d\n", int(bool(originalChainLayout)));
}

// Live sometimes leaves the Max runtime's content window at the size of an
// older zoom: the host is correct but the content is either smaller than the
// host (black border) or larger (clipped). When a device is fully visible the
// content should fill its host, so put it back if it does not.
void repairMaxContent(Display *dpy, Window host)
{
    if (!maxDeviceViews.contains(host))
        return; // destroyed or no longer one of ours
    Window root, parent, *children = nullptr;
    unsigned count = 0;
    if (!XQueryTree(dpy, host, &root, &parent, &children, &count))
        return;
    Window content = 0;
    int contentWidth = 0, contentHeight = 0, contentX = 0, contentY = 0;
    long bestArea = -1;
    for (unsigned i = 0; i < count; ++i) {
        XWindowAttributes attrs;
        if (!XGetWindowAttributes(dpy, children[i], &attrs))
            continue;
        const long area = long(attrs.width) * attrs.height;
        if (area > bestArea) {
            bestArea = area;
            content = children[i];
            contentWidth = attrs.width;
            contentHeight = attrs.height;
            contentX = attrs.x;
            contentY = attrs.y;
        }
    }
    XWindowAttributes hostAttrs;
    if (content && XGetWindowAttributes(dpy, host, &hostAttrs)
        && hostAttrs.width > 0 && hostAttrs.height > 0
        && (contentWidth != hostAttrs.width || contentHeight != hostAttrs.height
            || contentX != 0 || contentY != 0)) {
        XMoveResizeWindow(dpy, content, 0, 0, unsigned(hostAttrs.width), unsigned(hostAttrs.height));
    }
    if (children)
        XFree(children);
}

// The Max runtime can leave the content unpainted when a hidden host is
// shown again (for example after a fullscreen change). A one-pixel resize and
// restore sends the configure event it needs, once per visibility change,
// without recurring layout calls.
bool nudgeMaxContent(Display *dpy, Window host)
{
    if (!maxDeviceViews.contains(host))
        return false;
    Window root, parent, *children = nullptr;
    unsigned count = 0;
    if (!XQueryTree(dpy, host, &root, &parent, &children, &count))
        return false;
    Window content = 0;
    int width = 0, height = 0, contentX = 0, contentY = 0;
    long bestArea = -1;
    for (unsigned i = 0; i < count; ++i) {
        XWindowAttributes attrs;
        if (!XGetWindowAttributes(dpy, children[i], &attrs))
            continue;
        const long area = long(attrs.width) * attrs.height;
        if (area > bestArea) {
            bestArea = area;
            content = children[i];
            width = attrs.width;
            height = attrs.height;
            contentX = attrs.x;
            contentY = attrs.y;
        }
    }
    if (children)
        XFree(children);
    if (!content || width <= 1 || height <= 1)
        return false;
    // Keep the content where the runtime put it: a view clipped at the chain
    // edge carries an offset that must survive the nudge. Grow by one pixel
    // rather than shrink: the transient column then falls outside the host and
    // is clipped, whereas shrinking left its outer column unpainted and showed
    // as a hairline at the device's edge.
    XMoveResizeWindow(dpy, content, contentX, contentY, unsigned(width + 1), unsigned(height));
    XSync(dpy, False);
    XMoveResizeWindow(dpy, content, contentX, contentY, unsigned(width), unsigned(height));
    XFlush(dpy);
    return true;
}

// A view Live placed at the chain's edge because it is (or was) out of view
// can carry a clamped full rect: x1 is the viewport edge, not the device's
// true screen position. Only a rect whose visible part starts at its full
// left edge is a reliable reference for the zoom and offset.
bool layoutUsable(const LayoutInfo &l, qint64 now)
{
    if (l.vx2 <= l.vx1 || l.vx1 != l.x1 || l.x2 <= viewportLeft
        || l.vy2 <= viewportTop || l.vy1 >= viewportBottom)
        return false;
    // A recent right edge catches a view Live clamped to the edge; an old one
    // may belong to an earlier window size and must not reject valid views.
    if (now - viewportRightWhen < 5000 && l.x1 >= viewportRight)
        return false;
    return true;
}

// Keeps every Max device view on its device. Live gives us the unclipped
// device rect through the chain view's list (viewRectFn); the mapping from
// that rect to the layout routine's units, screen = zoom * node + offset,
// is taken from the views Live laid out in the latest pass. Each pass places
// or hides all views from that mapping, so views Live stops updating are
// handled too.
void manageMaxDeviceViews()
{
    if (!managerEnabled() || maxDeviceViewsHidden)
        return;
    if (!chainView || !viewRectFn || !originalLayout)
        return;
    Display *dpy = managerDisplay();
    if (!dpy)
        return;

    AddressRanges ranges;
    if (!ranges.load()) {
        // No /proc/self/maps: run without the pointer guards (the behaviour
        // that predates them) instead of disabling the manager entirely.
        static bool logged = false;
        if (!logged) {
            logged = true;
            menufixTrace("maxviews: no /proc/self/maps, pointer checks skipped\n");
        }
    }
    if (!ranges.contains(chainView, 0x30)) {
        menufixTrace("maxviews: chain view %x gone\n", int(reinterpret_cast<quintptr>(chainView)));
        chainView = nullptr; // Live freed it (set closed, chain view gone)
        return;
    }

    struct Node {
        unsigned long win;
        MaxViewRect rect;
    };
    Node nodes[128];
    int count = 0;
    QVector<unsigned long> untrusted; // views we cannot place this pass
    void *node = *reinterpret_cast<void **>(static_cast<char *>(chainView) + 0x28);
    while (node && count < 128) {
        if (!ranges.contains(node, kNodeSize)) {
            menufixTrace("maxviews: node %x gone, chain dropped\n", int(reinterpret_cast<quintptr>(node)));
            chainView = nullptr; // stale list; wait for the next chain layout
            lastLayoutW.clear();
            originShiftPasses = 0;
            return;
        }
        void *next = *reinterpret_cast<void **>(node);
        void *view = *reinterpret_cast<void **>(static_cast<char *>(node) + 0x08);
        const unsigned long win = *reinterpret_cast<unsigned long *>(static_cast<char *>(node) + 0x10);
        if (view && win) {
            maxDeviceViews.insert(win); // Clip-view hiding must know it too
            if (viewLooksValid(view, ranges)) {
                const MaxViewRect rect = viewRectFn(view);
                if (plausibleRect(rect)) {
                    nodes[count].win = win;
                    nodes[count].rect = rect;
                    ++count;
                    node = next;
                    continue;
                }
            }
            // Cannot trust this view's rectangle. Hiding is safer than
            // leaving a stale Max window over another device.
            menufixTrace("maxviews: untrusted view %x win %x, hiding\n",
                         int(reinterpret_cast<quintptr>(view)), int(win));
            untrusted.append(win);
        }
        node = next;
    }

    if (!count) {
        // The track has no Max devices. Live leaves the previous track's
        // windows alone when the device list changes, so hide them here.
        for (Window w : maxDeviceViews) {
            managerHidden.insert(w);
            XUnmapWindow(dpy, w);
        }
        XFlush(dpy);
        return;
    }

    for (unsigned long w : untrusted) {
        managerHidden.insert(w);
        XUnmapWindow(dpy, w);
    }

    const qint64 now = monotonicMs();

    // The chain can move on screen without its devices moving within it: the
    // info view opens, an instrument is inserted at the left, the detail area
    // changes. Compare each view's implied offset (laid-out x minus zoomed
    // node x) with the learned one. Scrolling leaves that unchanged, so the
    // mapping stays frozen there; a translation moves every view by the same
    // delta; a zoom change rescales them, caught by the width check and left
    // to the y re-learn below. The shift is applied to the offset directly,
    // never by re-learning zoom from whatever layouts are current: a
    // vertically clipped view would otherwise move every host by its clipped y.
    QVector<QPair<double, double>> shifts;
    for (int i = 0; i < count; ++i) {
        const auto layout = lastLayout.find(nodes[i].win);
        if (layout == lastLayout.end() || now - layout->when > 200 || !layoutUsable(*layout, now))
            continue;
        if (!managerWhen)
            break; // no mapping learned yet; the block below learns it
        const int width = layout->x2 - layout->x1;
        auto seenWidth = lastLayoutW.find(nodes[i].win);
        if (seenWidth == lastLayoutW.end()) {
            lastLayoutW.insert(nodes[i].win, width);
            continue;
        }
        if (qAbs(seenWidth.value() - width) > 1) {
            *seenWidth = width; // zoom change, not a translation
            continue;
        }
        const double dx = double(layout->x1) - managerZoom * double(nodes[i].rect.x1) - managerOffsetX;
        const double dy = double(layout->y1) - managerZoom * double(nodes[i].rect.y1) - managerOffsetY;
        if (qAbs(dx) > 3.0 || qAbs(dy) > 3.0)
            shifts.append({dx, dy});
    }
    double shiftX = 0.0, shiftY = 0.0;
    int agreeing = 0;
    if (!shifts.isEmpty()) {
        QVector<double> xs, ys;
        for (const QPair<double, double> &s : shifts) {
            xs.append(s.first);
            ys.append(s.second);
        }
        std::sort(xs.begin(), xs.end());
        std::sort(ys.begin(), ys.end());
        shiftX = xs.at(xs.size() / 2);
        shiftY = ys.at(ys.size() / 2);
        for (const QPair<double, double> &s : shifts)
            if (qAbs(s.first - shiftX) <= 2.0 && qAbs(s.second - shiftY) <= 2.0)
                ++agreeing;
    }
    if (agreeing >= 2
        || (agreeing == 1 && originShiftPasses >= 1 && qAbs(shiftX - pendingShiftX) <= 2.0
            && qAbs(shiftY - pendingShiftY) <= 2.0)) {
        managerOffsetX += shiftX;
        managerOffsetY += shiftY;
        lastPlacement.clear();
        originShiftPasses = 0;
        menufixTrace("maxviews: chain moved, offset by %d,%d\n", int(shiftX), int(shiftY));
    } else if (agreeing) {
        pendingShiftX = shiftX;
        pendingShiftY = shiftY;
        ++originShiftPasses;
    } else {
        originShiftPasses = 0;
    }

    // The chain viewport's right edge follows the Live window's right edge.
    // A fullscreen/narrow change otherwise leaves the old bound in place and
    // hides every Max host, because they are suddenly outside it.
    if (maxDeviceParent) {
        XWindowAttributes parentAttrs;
        if (XGetWindowAttributes(dpy, maxDeviceParent, &parentAttrs)) {
            const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
            const int windowUnits = qRound(double(parentAttrs.width) / dpr);
            lastWindowUnits = windowUnits;
            if (viewportRightUpdated) {
                viewportRightUpdated = false;
                viewportParentWidth = parentAttrs.width;
            } else if (viewportParentWidth && parentAttrs.width != viewportParentWidth) {
                // A window resize keeps the side panel's width: the chain ends
                // that many units before the window. Without a learned inset,
                // start from the window edge; it only costs the frame the
                // resize happens on, never the whole chain.
                viewportRight = viewportPanelUnits >= 0 ? windowUnits - viewportPanelUnits : windowUnits - 9;
                viewportParentWidth = parentAttrs.width;
                nudgedHosts.clear();
                lastPlacement.clear();
            } else {
                viewportParentWidth = parentAttrs.width;
            }
            if (rightInsetPixels() > 0) {
                viewportRight = qRound((double(parentAttrs.width) - rightInsetPixels()) / dpr);
                viewportRightWhen = 0;
            } else if (viewportRightWhen == 0) {
                viewportRight = windowUnits - 9;
                static qint64 lastFallback = 0;
                if (now - lastFallback > 1000) {
                    lastFallback = now;
                    menufixTrace("maxviews: fallback width %d dpr ", parentAttrs.width);
                    menufixTrace("%d/1000 right %d\n", int(dpr * 1000), viewportRight);
                }
            }
        }
    }

    qint64 newest = -1;
    for (int i = 0; i < count; ++i) {
        auto it = lastLayout.find(nodes[i].win);
        if (it != lastLayout.end() && now - it->when < 5000 && layoutUsable(*it, now)
            && it->when > newest)
            newest = it->when;
    }
    if (managerWhen && qAbs(nodes[0].rect.y1 - mappingNodeY) > 2) {
        managerWhen = 0; // zoom/window/detail-area change
        lastPlacement.clear();
    }
    if (!managerWhen) {
        if (newest < 0)
            return; // no trusted layout for the first/zoom mapping
        // Live's zoom scales the device width between the chain view's node
        // rect and the rect the layout routine expects. The unclipped width
        // of a view Live just laid out gives that scale directly.
        double fullWidth = 0.0, nodeWidth = 0.0;
        for (int i = 0; i < count; ++i) {
            auto it = lastLayout.find(nodes[i].win);
            if (it == lastLayout.end() || newest - it->when > 5 || now - it->when >= 5000
                || !layoutUsable(*it, now))
                continue;
            const double nw = double(nodes[i].rect.x2 - nodes[i].rect.x1);
            if (nw >= 20.0) {
                fullWidth += double(it->x2 - it->x1);
                nodeWidth += nw;
            }
        }
        if (nodeWidth > 0.0) {
            const double zoom = fullWidth / nodeWidth;
            if (zoom > 0.25 && zoom < 4.0)
                managerZoom = zoom;
        }

        // Offsets from the same layout pass: screen = zoom * node + offset.
        double offsetX = 0.0, offsetY = 0.0;
        int fits = 0;
        for (int i = 0; i < count; ++i) {
            auto it = lastLayout.find(nodes[i].win);
            if (it == lastLayout.end() || newest - it->when > 5 || now - it->when >= 5000
                || !layoutUsable(*it, now))
                continue;
            offsetX += double(it->x1) - managerZoom * double(nodes[i].rect.x1);
            offsetY += double(it->y1) - managerZoom * double(nodes[i].rect.y1);
            ++fits;
        }
        if (fits) {
            managerOffsetX = offsetX / fits;
            managerOffsetY = offsetY / fits;
        }
        managerWhen = now;
        mappingNodeY = nodes[0].rect.y1;
    }

    // Reconcile the Max runtime's content window after a layout (startup,
    // track change, zoom change), and periodically in case it resizes itself.
    // Time-based only: animations trigger chain layouts, and reacting to every
    // one made the content repair fight the runtime.
    static qint64 lastContentCheck = 0;
    const bool checkContent = now - lastContentCheck >= 1000;
    if (checkContent) {
        contentCheckRequested = false;
        lastContentCheck = now;
    }

    const int left = viewportLeft;
    const int right = viewportRight > viewportLeft ? viewportRight : 2469;
    {
        static qint64 lastSnapshot = 0;
        if (now - lastSnapshot > 1000) {
            lastSnapshot = now;
            menufixTrace("maxviews: boundary %d..%d x ", left, right);
            menufixTrace("%d..%d zoom ", viewportTop, viewportBottom);
            menufixTrace("%d/1000 off %d\n", int(managerZoom * 1000), int(managerOffsetX));
            for (int i = 0; i < count && i < 8; ++i) {
                menufixTrace("maxviews: view %x node ", int(nodes[i].win));
                menufixTrace("%d..%d screen ", nodes[i].rect.x1, nodes[i].rect.x2);
                menufixTrace("%d..%d\n", qRound(managerZoom * nodes[i].rect.x1 + managerOffsetX),
                             qRound(managerZoom * nodes[i].rect.x2 + managerOffsetX));
            }
        }
    }
    const int top = viewportTop;
    const int bottom = viewportBottom;
    // At 100 % the chain has no vertical scroll: the node y is the screen y.
    // A stale vertical offset here previously hid every host after a window
    // or fullscreen change.
    const double offsetY = qAbs(managerZoom - 1.0) < 0.05 ? 0.0 : managerOffsetY;
    for (int i = 0; i < count; ++i) {
        const int sx1 = qRound(managerZoom * nodes[i].rect.x1 + managerOffsetX);
        const int sy1 = qRound(managerZoom * nodes[i].rect.y1 + offsetY);
        const int sx2 = qRound(managerZoom * nodes[i].rect.x2 + managerOffsetX);
        const int sy2 = qRound(managerZoom * nodes[i].rect.y2 + offsetY);
        const int ix1 = qMax(sx1, left), iy1 = qMax(sy1, top);
        const int ix2 = qMin(sx2, right), iy2 = qMin(sy2, bottom);
        // A view Live clipped at the viewport edge is not a valid thing to
        // show: it leaves a sliver of the Max UI over whatever is at the
        // edge. Hide it once less than a fifth of the device is visible,
        // as the old scroll heuristic did, instead of showing the sliver
        // until the next refresh.
        const int fullWidth = sx2 - sx1;
        const int fullHeight = sy2 - sy1;
        // Hide only when nothing of the view is inside the chain. Live's layout
        // routine already sizes a host to its visible rect, so a partially
        // visible view shows exactly its visible part, frame and view together.
        // The old "less than a fifth" rule existed for when placement was not
        // trustworthy; it left an empty frame strip at the edge.
        // A few pixels are a hairline, not a partial view: hiding them keeps
        // the frame edge from showing a 1 px seam next to its neighbour.
        const bool nothingVisible = fullWidth <= 0 || fullHeight <= 0
            || ix2 - ix1 <= 3 || iy2 - iy1 <= 3;
        if (nothingVisible) {
            managerHidden.insert(nodes[i].win);
            XUnmapWindow(dpy, nodes[i].win);
            lastPlacement.remove(nodes[i].win);
            nudgedHosts.remove(nodes[i].win);
        } else {
            managerHidden.remove(nodes[i].win);
            int visible[4] = {ix1, iy1, ix2, iy2};
            int full[4] = {sx1, sy1, sx2, sy2};
            const Placement placement = {ix1, iy1, ix2, iy2, sx1, sy1, sx2, sy2};
            const auto placed = lastPlacement.constFind(nodes[i].win);
            const bool needLayout = placed == lastPlacement.constEnd()
                || placed->vx1 != ix1 || placed->vy1 != iy1
                || placed->vx2 != ix2 || placed->vy2 != iy2
                || placed->fx1 != sx1 || placed->fy1 != sy1
                || placed->fx2 != sx2 || placed->fy2 != sy2;
            if (needLayout) {
                originalLayout(reinterpret_cast<void *>(nodes[i].win), visible, full);
                lastPlacement.insert(nodes[i].win, placement);
            }
            // A device that is not clipped should have its content filling the
            // host exactly. Partially visible devices keep Live's larger,
            // offset content for clipping.
            if (checkContent && ix1 == sx1 && iy1 == sy1 && ix2 == sx2 && iy2 == sy2)
                repairMaxContent(dpy, nodes[i].win);
            // A host that has just become visible (fully or clipped at the
            // chain's edge) needs one configure event to make the runtime
            // paint; otherwise Live's placeholder stays grey until the view is
            // scrolled away and back.
            if (!nudgedHosts.contains(nodes[i].win)) {
                if (nudgeMaxContent(dpy, nodes[i].win))
                    nudgedHosts.insert(nodes[i].win);
            }
        }
    }
    XFlush(dpy);
}

uintptr_t mainExecutableBase()
{
    uintptr_t b = 0;
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *out) {
            *static_cast<uintptr_t *>(out) = info->dlpi_addr;
            return 1;
        },
        &b);
    return b;
}

template <typename Fn>
Fn next(const char *name)
{
    return reinterpret_cast<Fn>(nextSymbol(name, nullptr));
}

// Does this return address belong to Live itself (not to a library it loads)?
// The Max runtime applies its own scale to the windows it creates, so only
// Live's requests are in Live's unscaled units and need converting.
bool calledFromLive(void *ret)
{
    Dl_info info;
    if (!dladdr(ret, &info) || !info.dli_fbase)
        return false;
    uintptr_t liveBase = 0;
    dl_iterate_phdr(
        [](dl_phdr_info *entry, size_t, void *out) {
            *static_cast<uintptr_t *>(out) = entry->dlpi_addr; // first entry = Live
            return 1;
        },
        &liveBase);
    return reinterpret_cast<uintptr_t>(info.dli_fbase) == liveBase;
}

// The Max runtime (libMaxPlugLib.so) draws its UI at JUCE's own scale, which on
// X11 stays 100 %, while Live's interface runs at the window's scale factor.
// Live hands the device views a rectangle in its units, so at 125 % every Max
// view filled only four fifths of its frame. The runtime's global scale brings
// its own window sizing, painting and font metrics to Live's scale; the X
// interposer below then leaves the runtime's requests alone (they are already
// scaled) and only converts the ones coming from Live.
void followMaxRuntimeScale()
{
    const char *env = getenv("LIVE_MAX_SCALE");
    if (env && !strcmp(env, "0"))
        return;
    auto getDesktop = reinterpret_cast<void *(*)()>(
        dlsym(RTLD_DEFAULT, "_ZN4juce7Desktop11getInstanceEv"));
    auto setScale = reinterpret_cast<void (*)(void *, float)>(
        dlsym(RTLD_DEFAULT, "_ZN4juce7Desktop20setGlobalScaleFactorEf"));
    if (!getDesktop || !setScale)
        return; // the Max runtime is not loaded yet
    void *desktop = getDesktop();
    if (!desktop || !qApp)
        return;
    const qreal ratio = qApp->devicePixelRatio();
    if (ratio <= 0)
        return;
    float current = 1.0f;
    memcpy(&current, static_cast<char *>(desktop) + 0x120, sizeof current);
    if (qAbs(current - float(ratio)) < 0.001f)
        return;
    setScale(desktop, float(ratio));
    menufixTrace("maxscale: runtime scale %d/1000\n", int(ratio * 1000));
}

} // namespace

void pluginViewCreated(void *view)
{
    createdView = static_cast<IPlugView *>(view);
}

void installPluginCloseHook()
{
    auto *scaleTimer = new QTimer(QCoreApplication::instance());
    QObject::connect(scaleTimer, &QTimer::timeout, [] { followMaxRuntimeScale(); });
    scaleTimer->start(500);
    base = mainExecutableBase();
    installMaxViewHooks();
    installChainViewHook();
    if (managerEnabled()) {
        auto *viewManager = new QTimer(qApp);
        QObject::connect(viewManager, &QTimer::timeout, [] { manageMaxDeviceViews(); });
        viewManager->start(33);
    }
    auto *thunk = reinterpret_cast<unsigned char *>(base + kThunk);
    // e9 rel32 to the observer, then padding up to 0x3f2b9b0.
    int32_t rel;
    memcpy(&rel, thunk + 1, 4);
    if (thunk[0] != 0xe9 || base + kThunk + 5 + rel != base + kObserver) {
        menufixTrace("pluginclose: thunk mismatch, not hooked\n");
        return;
    }
    observer = reinterpret_cast<ObserverFn>(base + kObserver);
    setBool = reinterpret_cast<SetBoolFn>(base + kSetBool);

    // jmp qword [rip+0] ; .quad observerHook  (14 bytes, fits before 0x3f2b9b0)
    unsigned char patch[14] = {0xff, 0x25, 0, 0, 0, 0};
    const uintptr_t target = reinterpret_cast<uintptr_t>(&observerHook);
    memcpy(patch + 6, &target, 8);
    const long page = sysconf(_SC_PAGESIZE);
    auto *pageStart = reinterpret_cast<void *>(uintptr_t(thunk) & ~uintptr_t(page - 1));
    const size_t len = (uintptr_t(thunk) + sizeof patch > uintptr_t(pageStart) + page) ? 2 * page : page;
    if (mprotect(pageStart, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        menufixTrace("pluginclose: mprotect failed\n");
        return;
    }
    memcpy(thunk, patch, sizeof patch);
    mprotect(pageStart, len, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char *>(thunk), reinterpret_cast<char *>(thunk) + sizeof patch);
    menufixTrace("pluginclose: observer hooked\n");
}

extern "C" Window XCreateWindow(Display *dpy, Window parent, int x, int y, unsigned width,
                                unsigned height, unsigned border, int depth, unsigned cls,
                                Visual *visual, unsigned long mask, XSetWindowAttributes *attrs)
{
    static auto real = next<decltype(&XCreateWindow)>("XCreateWindow");
    const Window w = real(dpy, parent, x, y, width, height, border, depth, cls, visual, mask, attrs);
    if (w && currentProp && parent == DefaultRootWindow(dpy)) {
        if (!wmDeleteWindow) {
            wmProtocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
            wmDeleteWindow = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        }
        XSetWMProtocols(dpy, w, &wmDeleteWindow, 1);
        windowProps.insert(w, currentProp);
        Editor editor;
        editor.view = createdView;
        createdView = nullptr;
        editor.dpy = dpy;
        editor.width = int(width);
        editor.height = int(height);
        editors.insert(w, editor);
        XWindowAttributes attributes;
        if (XGetWindowAttributes(dpy, w, &attributes))
            XSelectInput(dpy, w, attributes.your_event_mask | StructureNotifyMask);
        menufixTrace("pluginclose: editor window 0x%x\n", int(w));
        liveOwnPluginWindow(dpy, w);
    }
    return w;
}

namespace {

// Keeps a fixed-size editor fixed at the new size, and widens the limits of a
// resizable one so the new size fits.
void fitSizeHints(Display *dpy, Window w, unsigned width, unsigned height)
{
    XSizeHints *hints = XAllocSizeHints();
    long supplied = 0;
    if (hints && XGetWMNormalHints(dpy, w, hints, &supplied)) {
        hints->flags |= PWinGravity | USPosition | PPosition;
        hints->win_gravity = StaticGravity;
        const bool fixed = (hints->flags & PMinSize) && (hints->flags & PMaxSize)
                           && hints->min_width == hints->max_width && hints->min_height == hints->max_height;
        if (fixed) {
            hints->min_width = hints->max_width = int(width);
            hints->min_height = hints->max_height = int(height);
        } else {
            if (hints->flags & PMinSize) {
                hints->min_width = qMin(hints->min_width, int(width));
                hints->min_height = qMin(hints->min_height, int(height));
            }
            if (hints->flags & PMaxSize) {
                hints->max_width = qMax(hints->max_width, int(width));
                hints->max_height = qMax(hints->max_height, int(height));
            }
        }
        XSetWMNormalHints(dpy, w, hints);
        menufixTrace("pluginclose: editor 0x%x resized to %dx", int(w), int(width));
        menufixTrace("%d\n", int(height));
    }
    if (hints)
        XFree(hints);
}

// Sizes the editor to the plug-in's view (its first mapped child window),
// which the plug-in sized in attached() before Live maps the editor.
void fitToView(Display *dpy, Window w)
{
    XWindowAttributes outer;
    Window root, parent, *kids = nullptr;
    unsigned count = 0;
    if (!XGetWindowAttributes(dpy, w, &outer) || !XQueryTree(dpy, w, &root, &parent, &kids, &count))
        return;
    for (unsigned i = 0; i < count; ++i) {
        XWindowAttributes view;
        if (!XGetWindowAttributes(dpy, kids[i], &view) || view.map_state == IsUnmapped)
            continue;
        if (view.width != outer.width || view.height != outer.height) {
            menufixTrace("pluginclose: editor 0x%x fitted to its view, ", int(w));
            XResizeWindow(dpy, w, unsigned(view.width), unsigned(view.height));
        }
        break;
    }
    if (kids)
        XFree(kids);
}

} // namespace

extern "C" int XMoveResizeWindow(Display *dpy, Window w, int x, int y, unsigned width, unsigned height)
{
    static auto real = next<decltype(&XMoveResizeWindow)>("XMoveResizeWindow");
    // Max for Live device windows. The Max runtime creates them as native X
    // children of Live's window (juce::Desktop and LinuxComponentPeer live in
    // libMaxPlugLib.so). Live hands their rectangle over in its own units
    // (zoom x scale, the units the rest of its views use), but an X window
    // needs pixels: unscaled, every Max device sat above and left of its slot
    // by (ratio - 1) times its position. followMaxRuntimeScale() puts the
    // runtime's own scale at Live's, so its painting fills the scaled window.
    if (!windowProps.contains(w)) {
        // With the runtime at Live's scale, its own window requests are
        // already in pixels; only Live's are in its unscaled units.
        const bool liveUnits = calledFromLive(__builtin_return_address(0));
        // Height identifies a Max view at normal zoom, but a low enough zoom
        // shrinks the device below that threshold. The chain view's node list
        // is authoritative; the parent check covers a window Live lays out
        // before the chain hook has registered it.
        bool maxView = height >= 100 || maxDeviceViews.contains(w);
        if (!maxView && liveUnits && maxDeviceParent) {
            Window root, parent, *children = nullptr;
            unsigned count = 0;
            if (XQueryTree(dpy, w, &root, &parent, &children, &count))
                maxView = parent == maxDeviceParent;
            if (children)
                XFree(children);
        }
        if (maxView) {
            maxDeviceViews.insert(w);
            if (const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
                liveUnits && ratio != 1.0) {
                x = qRound(x * ratio);
                y = qRound(y * ratio);
                width = unsigned(qRound(width * ratio));
                height = unsigned(qRound(height * ratio));
            }
        }
    }
    if (!windowProps.contains(w))
        return real(dpy, w, x, y, width, height);
    fitSizeHints(dpy, w, width, height);
    rememberSize(w, width, height);
    return real(dpy, w, x, y, width, height);
}

extern "C" int XMapWindow(Display *dpy, Window w)
{
    static auto real = next<decltype(&XMapWindow)>("XMapWindow");
    if (pendingUnmaps.remove(w)) {
        menufixTrace("pluginclose: editor 0x%x hidden and shown again, kept mapped\n", int(w));
        return 1;
    }
    if (windowProps.contains(w))
        fitToView(dpy, w);
    else if (managerHidden.contains(w) || (maxDeviceViewsHidden && maxDeviceViews.contains(w)))
        return 1; // scrolled out of the chain, or the device view is not shown
    return real(dpy, w);
}

extern "C" int XUnmapWindow(Display *dpy, Window w)
{
    static auto real = next<decltype(&XUnmapWindow)>("XUnmapWindow");
    if (!windowProps.contains(w))
        return real(dpy, w);
    pendingUnmaps.insert(w, dpy);
    QTimer::singleShot(50, [w] {
        if (Display *d = pendingUnmaps.take(w)) {
            real(d, w);
            XFlush(d);
        }
    });
    return 1;
}

extern "C" void XSetWMNormalHints(Display *dpy, Window w, XSizeHints *hints)
{
    static auto real = next<decltype(&XSetWMNormalHints)>("XSetWMNormalHints");
    if (hints && (windowProps.contains(w) || currentProp)) {
        hints->flags |= PWinGravity | USPosition | PPosition;
        hints->win_gravity = StaticGravity;
    }
    real(dpy, w, hints);
}

extern "C" int XResizeWindow(Display *dpy, Window w, unsigned width, unsigned height)
{
    static auto real = next<decltype(&XResizeWindow)>("XResizeWindow");
    if (!windowProps.contains(w))
        return real(dpy, w, width, height);
    fitSizeHints(dpy, w, width, height);
    rememberSize(w, width, height);
    return real(dpy, w, width, height);
}

extern "C" int XDestroyWindow(Display *dpy, Window w)
{
    static auto real = next<decltype(&XDestroyWindow)>("XDestroyWindow");
    windowProps.remove(w);
    pendingUnmaps.remove(w);
    editors.remove(w);
    maxDeviceViews.remove(w);
    managerHidden.remove(w);
    lastLayout.remove((unsigned long)w);
    lastPlacement.remove((unsigned long)w);
    nudgedHosts.remove((unsigned long)w);
    lastLayoutW.remove((unsigned long)w);
    return real(dpy, w);
}

extern "C" int XNextEvent(Display *dpy, XEvent *ev)
{
    static auto real = next<decltype(&XNextEvent)>("XNextEvent");
    const int r = real(dpy, ev);
    if (ev->type == ConfigureNotify && ev->xconfigure.window == ev->xconfigure.event) {
        auto it = editors.find(ev->xconfigure.window);
        if (it != editors.end()) {
            it->wantWidth = ev->xconfigure.width;
            it->wantHeight = ev->xconfigure.height;
            // One onSize per pause in a drag, from the next loop pass.
            if (!it->pending && (it->wantWidth != it->width || it->wantHeight != it->height)) {
                it->pending = true;
                const Window w = ev->xconfigure.window;
                QTimer::singleShot(30, [w] { applyFrameResize(w); });
            }
        }
    }
    if (ev->type == ClientMessage && wmDeleteWindow && ev->xclient.message_type == wmProtocols
        && Atom(ev->xclient.data.l[0]) == wmDeleteWindow && windowProps.contains(ev->xclient.window)) {
        const Window w = ev->xclient.window;
        menufixTrace("pluginclose: close requested for 0x%x\n", int(w));
        // Live is inside its own event pump here; close on the next loop pass,
        // and only if the window still belongs to an open editor by then.
        QTimer::singleShot(0, [w] {
            if (void *prop = windowProps.value(w))
                setBool(prop, false);
        });
    }
    return r;
}

// Called when Live switches between Clip view and Device view: the Max device
// views are native windows Live does not hide for that, so the shim does.
void setMaxDeviceViewsHidden(bool hidden)
{
    if (maxDeviceViewsHidden == hidden)
        return;
    maxDeviceViewsHidden = hidden;
    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy)
        return;
    for (Window w : maxDeviceViews) {
        if (hidden)
            XUnmapWindow(dpy, w);
        else if (!managerHidden.contains(w))
            XMapWindow(dpy, w); // Live does not map them again by itself
    }
    XFlush(dpy);
    XCloseDisplay(dpy);
    menufixTrace(hidden ? "pluginclose: device views hidden (%d)\n" : "pluginclose: device views shown (%d)\n",
                 int(maxDeviceViews.size()));
}


// Called on a timer: hide the Max device views whenever Live is showing the
// clip view instead of the device view.


