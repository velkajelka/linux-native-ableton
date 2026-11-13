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
#include <QSet>
#include <QTimer>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <climits>
#include <cstdarg>
#include <cstdint>
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

// Max for Live device views: the native X children of Live's window that the
// shim repositions. They have to disappear when the device view is not shown
// (Clip view) - Live itself never hides them for that case, but it does hide
// them when a track's devices change, so unmapping is a state it handles.
QSet<Window> maxDeviceViews;
bool maxDeviceViewsHidden;

// Live clips a Max view that scrolls out of the device chain to its visible
// slice: the host window shrinks to a sliver at the viewport edge while the
// content window shifts inside it, and it is never unmapped. On Windows the
// chain window clips that sliver away; here the host is a child of Live's
// window, so it stays on screen over whatever is at the edge. These track the
// windows currently hidden for that reason and their full (unclipped) size.
QSet<Window> scrollHidden;
QHash<Window, int> maxDeviceWidth;
QHash<Window, int> maxDeviceHeight;
QHash<Window, int> lastLogicalWidth;
int chainLeft = INT_MAX; // viewport left edge, logical, seen while clipping
int chainRight = 0;      // viewport right edge, logical

// Rect diagnostics: LIVE_TRACE_MAXRECTS=1 logs the eight values Live passes to
// the Max device view's setDeviceRects while the chain scrolls, to find out
// whether the unclipped device rectangle is available.
bool rectTraceEnabled()
{
    static const bool on = getenv("LIVE_TRACE_MAXRECTS") != nullptr;
    return on;
}

void rectTrace(const char *fmt, ...)
{
    if (!rectTraceEnabled())
        return;
    const char *path = getenv("MENUFIX_DEBUG");
    if (!path)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "recttrace %ld.%06ld ", long(ts.tv_sec), long(ts.tv_nsec / 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

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

struct LayoutInfo {
    int x1, y1, x2, y2;
    qint64 when;
};
QHash<unsigned long, LayoutInfo> lastLayout; // full rect per host, from Live
void *chainView = nullptr;                   // the chain view object
int viewportLeft = 8;
int viewportRight = 2469;
int viewportTop = 1446;
int viewportBottom = 1615;
int managerK = 0;
qint64 managerKWhen = 0;
QSet<Window> managerHidden;

bool layoutTraceEnabled()
{
    static const bool on = getenv("LIVE_TRACE_MAXLAYOUT") != nullptr;
    return on;
}

void layoutTrace(const char *fmt, ...)
{
    if (!layoutTraceEnabled())
        return;
    const char *path = getenv("MENUFIX_DEBUG");
    if (!path)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "layouttrace %ld.%06ld ", long(ts.tv_sec), long(ts.tv_nsec / 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Max device layout: arg1 window, arg2 visible (clipped) rect, arg3 full
// (unclipped) device rect, each as x1,y1,x2,y2. Live computes both in its
// caller; the caller address is the key to where the chain scroll lives.
using LayoutFn = void (*)(void *, const int *, const int *);
LayoutFn originalLayout;

void hookedLayout(void *window, const int *visible, const int *full)
{
    if (visible && full) {
        const qint64 now = monotonicMs();
        lastLayout[(unsigned long)window] = {full[0], full[1], full[2], full[3], now};
        if (visible[0] > full[0])
            viewportLeft = visible[0];
        if (visible[2] < full[2])
            viewportRight = visible[2];
        viewportTop = full[1];
        viewportBottom = full[3];
    }
    if (layoutTraceEnabled() && visible && full) {
        layoutTrace("win=0x%lx visible=%d,%d,%d,%d full=%d,%d,%d,%d caller=0x%lx",
                    (unsigned long)window, visible[0], visible[1], visible[2], visible[3], full[0], full[1],
                    full[2], full[3], (unsigned long)__builtin_return_address(0));
    }
    if (originalLayout)
        originalLayout(window, visible, full);
}

bool chainTraceEnabled()
{
    static const bool on = getenv("LIVE_TRACE_CHAINVIEW") != nullptr;
    return on;
}

void chainTrace(const char *fmt, ...)
{
    if (!chainTraceEnabled())
        return;
    const char *path = getenv("MENUFIX_DEBUG");
    if (!path)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "chaintrace %ld.%06ld ", long(ts.tv_sec), long(ts.tv_nsec / 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Device-chain layout routine (Live+0x2074fd0): walks the view list at
// this+0x28 and computes the full/visible rectangles. Its fields are where
// the chain scroll must live.
struct MaxViewRect {
    int32_t x1, y1, x2, y2;
};
using ViewRectFn = MaxViewRect (*)(void *);
ViewRectFn viewRectFn;

using ChainLayoutFn = void (*)(void *, void *, void *);
ChainLayoutFn originalChainLayout;

void hookedChainLayout(void *self, void *arg2, void *arg3)
{
    chainView = self;
    if (chainTraceEnabled() && self) {
        // Log only the fields that change, comparing against the previous pass.
        constexpr int kWords = 0x40; // 0x200 bytes
        static void *previousSelf = nullptr;
        static uint64_t previous[kWords];
        const auto *words = reinterpret_cast<const uint64_t *>(self);
        if (self != previousSelf) {
            chainTrace("self=0x%lx a2=0x%lx a3=0x%lx", (unsigned long)self, (unsigned long)arg2,
                       (unsigned long)arg3);
            for (int i = 0; i < kWords; ++i)
                previous[i] = words[i];
            previousSelf = self;
        } else {
            for (int i = 0; i < kWords; ++i) {
                const uint64_t value = words[i];
                if (value != previous[i]) {
                    chainTrace("+%03x %016lx -> %016lx", i * 8, (unsigned long)previous[i],
                               (unsigned long)value);
                    previous[i] = value;
                }
            }
        }

        // Throttled: ask Live for every view's rectangle, including views
        // that are no longer laid out. Nodes: +0 next, +8 view, +0x10 window.
        static struct timespec lastRects {};
        struct timespec now {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const long elapsed = (now.tv_sec - lastRects.tv_sec) * 1000
                             + (now.tv_nsec - lastRects.tv_nsec) / 1000000;
        if (elapsed >= 150 && viewRectFn) {
            lastRects = now;
            void *node = *reinterpret_cast<void **>(static_cast<char *>(self) + 0x28);
            for (int i = 0; node && i < 32; ++i) {
                void *next = *reinterpret_cast<void **>(node);
                void *view = *reinterpret_cast<void **>(static_cast<char *>(node) + 0x08);
                const unsigned long window =
                    *reinterpret_cast<unsigned long *>(static_cast<char *>(node) + 0x10);
                MaxViewRect rect{};
                if (view)
                    rect = viewRectFn(view);
                chainTrace("node=0x%lx view=0x%lx win=0x%lx rect=%d,%d,%d,%d",
                           (unsigned long)node, (unsigned long)view, window, rect.x1, rect.y1, rect.x2,
                           rect.y2);
                node = next;
            }
        }
    }
    if (originalChainLayout)
        originalChainLayout(self, arg2, arg3);
}

using SetRectsFn = void (*)(void *, int, int, int, int, int, int, int, int);
SetRectsFn originalViewRects;
SetRectsFn originalWindowRects;

using GetArrayFn = long (*)(void *, void *, long, long *);
GetArrayFn originalGetArray;
using GetSysWindFn = unsigned long (*)(void *);
GetSysWindFn livewindGetSysWind;

long hookedGetArray(void *object, void *symbol, long maxlen, long *values)
{
    if (!originalGetArray)
        return 0; // the trampoline is installed just after the patch; do not call null
    const long count = originalGetArray(object, symbol, maxlen, values);
    if (!rectTraceEnabled() || !symbol || count <= 0)
        return count;
    const char *name = *reinterpret_cast<const char **>(symbol);
    if (!name || !strstr(name, "devicerect"))
        return count;
    unsigned long window = 0;
    if (livewindGetSysWind)
        window = livewindGetSysWind(object);
    rectTrace("get object=0x%lx window=0x%lx %s n=%ld %ld %ld %ld %ld %ld %ld %ld %ld",
              (unsigned long)object, window, name, count, count > 0 ? values[0] : 0, count > 1 ? values[1] : 0,
              count > 2 ? values[2] : 0, count > 3 ? values[3] : 0, count > 4 ? values[4] : 0,
              count > 5 ? values[5] : 0, count > 6 ? values[6] : 0, count > 7 ? values[7] : 0);
    return count;
}

void hookedViewRects(void *self, int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8)
{
    rectTrace("view self=0x%lx %d %d %d %d %d %d %d %d", (unsigned long)self, a1, a2, a3, a4, a5, a6, a7,
              a8);
    if (originalViewRects)
        originalViewRects(self, a1, a2, a3, a4, a5, a6, a7, a8);
}

void hookedWindowRects(void *self, int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8)
{
    rectTrace("window self=0x%lx %d %d %d %d %d %d %d %d", (unsigned long)self, a1, a2, a3, a4, a5, a6, a7,
              a8);
    if (originalWindowRects)
        originalWindowRects(self, a1, a2, a3, a4, a5, a6, a7, a8);
}

// Patches a Max runtime wrapper whose entry is a tail jump (the view variant
// first zeroes x and y) and returns a trampoline that reproduces it. The usual
// hookFunction cannot be used: it copies the prologue, which here contains a
// position-dependent jump.
void *hookTailJump(uintptr_t fn, void *replacement, bool zeroXY)
{
    auto *code = reinterpret_cast<unsigned char *>(fn);
    int32_t rel = 0;
    size_t jmpAt = 0;
    if (zeroXY) {
        if (code[0] != 0x31 || code[1] != 0xf6 || code[2] != 0x31 || code[3] != 0xd2 || code[4] != 0xe9) {
            menufixTrace("recttrace: view prologue mismatch\n");
            return nullptr;
        }
        memcpy(&rel, code + 5, 4);
        jmpAt = 9;
    } else {
        if (code[0] != 0xe9) {
            menufixTrace("recttrace: window prologue mismatch\n");
            return nullptr;
        }
        memcpy(&rel, code + 1, 4);
        jmpAt = 5;
    }
    const uintptr_t target = fn + jmpAt + rel;
    const size_t page = sysconf(_SC_PAGESIZE);
    void *mem = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return nullptr;
    auto *trampoline = static_cast<unsigned char *>(mem);
    size_t n = 0;
    if (zeroXY) {
        trampoline[n++] = 0x31; // xor esi, esi
        trampoline[n++] = 0xf6;
        trampoline[n++] = 0x31; // xor edx, edx
        trampoline[n++] = 0xd2;
    }
    trampoline[n++] = 0x48; // movabs rax, target
    trampoline[n++] = 0xb8;
    memcpy(trampoline + n, &target, 8);
    n += 8;
    trampoline[n++] = 0xff; // jmp rax
    trampoline[n++] = 0xe0;
    mprotect(mem, page, PROT_READ | PROT_EXEC);

    unsigned char patch[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    const uintptr_t to = reinterpret_cast<uintptr_t>(replacement);
    memcpy(patch + 2, &to, 8);
    const uintptr_t pageBase = fn & ~(page - 1);
    const size_t span = ((fn + sizeof patch + page - 1) & ~(page - 1)) - pageBase;
    if (mprotect(reinterpret_cast<void *>(pageBase), span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(mem, page);
        return nullptr;
    }
    memcpy(code, patch, sizeof patch);
    __builtin___clear_cache(reinterpret_cast<char *>(code), reinterpret_cast<char *>(code) + sizeof patch);
    mprotect(reinterpret_cast<void *>(pageBase), span, PROT_READ | PROT_EXEC);
    return mem;
}

void installMaxRectTrace()
{
    if (!rectTraceEnabled())
        return;
    auto *timer = new QTimer(QCoreApplication::instance());
    QObject::connect(timer, &QTimer::timeout, [timer] {
        if (!originalViewRects) {
            if (void *symbol = dlsym(RTLD_DEFAULT, "_ZN23LiveDeviceViewComponent14setDeviceRectsEiiiiiiii"))
                originalViewRects = reinterpret_cast<SetRectsFn>(hookTailJump(
                    reinterpret_cast<uintptr_t>(symbol), reinterpret_cast<void *>(&hookedViewRects), true));
        }
        if (!originalWindowRects) {
            if (void *symbol = dlsym(RTLD_DEFAULT, "_ZN25LiveDeviceWindowComponent14setDeviceRectsEiiiiiiii"))
                originalWindowRects = reinterpret_cast<SetRectsFn>(hookTailJump(
                    reinterpret_cast<uintptr_t>(symbol), reinterpret_cast<void *>(&hookedWindowRects), false));
        }
        if (!originalGetArray) {
            if (void *symbol = dlsym(RTLD_DEFAULT, "object_attr_getlong_array")) {
                unsigned char prologue[29]; // through the second movq; instruction boundary
                memcpy(prologue, symbol, sizeof prologue);
                originalGetArray = reinterpret_cast<GetArrayFn>(hookFunction(
                    reinterpret_cast<uintptr_t>(symbol), prologue, sizeof prologue,
                    reinterpret_cast<void *>(&hookedGetArray)));
            }
        }
        if (!livewindGetSysWind)
            livewindGetSysWind = reinterpret_cast<GetSysWindFn>(dlsym(RTLD_DEFAULT, "livewind_getsyswind"));
        if (originalGetArray) {
            menufixTrace("recttrace: view %d window %d get %d\n", int(bool(originalViewRects)),
                         int(bool(originalWindowRects)), int(bool(originalGetArray)));
            timer->deleteLater();
        }
    });
    timer->start(500);
}

// Scroll diagnostics: LIVE_TRACE_MAXWINDOWS=1 logs every move/map/unmap of the
// Max device windows to MENUFIX_DEBUG, with timestamps, so Live's behaviour
// during horizontal scrolling can be measured.
bool scrollTraceEnabled()
{
    static const bool on = getenv("LIVE_TRACE_MAXWINDOWS") != nullptr;
    return on;
}

void scrollTrace(const char *fmt, ...)
{
    if (!scrollTraceEnabled())
        return;
    const char *path = getenv("MENUFIX_DEBUG");
    if (!path)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "maxscroll %ld.%06ld ", long(ts.tv_sec), long(ts.tv_nsec / 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
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

void installMaxLayoutTrace()
{
    if (!layoutTraceEnabled() && !managerEnabled())
        return;
    const uintptr_t fn = mainExecutableBase() + 0x2002150;
    const unsigned char expected[13] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                        0x54, 0x53, 0x48, 0x83, 0xec, 0x40};
    unsigned char prologue[sizeof expected];
    memcpy(prologue, reinterpret_cast<void *>(fn), sizeof prologue);
    if (memcmp(prologue, expected, sizeof expected) != 0) {
        menufixTrace("layouttrace: prologue mismatch\n");
        return;
    }
    originalLayout = reinterpret_cast<LayoutFn>(
        hookFunction(fn, prologue, sizeof prologue, reinterpret_cast<void *>(&hookedLayout)));
    menufixTrace("layouttrace: hook %d\n", int(bool(originalLayout)));
}

void installChainLayoutTrace()
{
    if (!chainTraceEnabled() && !managerEnabled())
        return;
    const uintptr_t fn = mainExecutableBase() + 0x2074fd0;
    const unsigned char expected[17] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                        0x54, 0x53, 0x48, 0x81, 0xec, 0x18, 0x01, 0x00, 0x00};
    unsigned char prologue[sizeof expected];
    memcpy(prologue, reinterpret_cast<void *>(fn), sizeof prologue);
    if (memcmp(prologue, expected, sizeof expected) != 0) {
        menufixTrace("chaintrace: prologue mismatch\n");
        return;
    }
    originalChainLayout = reinterpret_cast<ChainLayoutFn>(
        hookFunction(fn, prologue, sizeof prologue, reinterpret_cast<void *>(&hookedChainLayout)));
    viewRectFn = reinterpret_cast<ViewRectFn>(mainExecutableBase() + 0x20ab930);
    menufixTrace("chaintrace: hook %d\n", int(bool(originalChainLayout)));
}

// Keeps every Max device view on its device. Live gives us the unclipped
// device rect through the chain view's list (viewRectFn); the screen mapping
// K = full_rect - node_rect is taken from the last view Live laid out. Each
// pass places or hides all views from that mapping, so views Live stops
// updating are handled too.
void manageMaxDeviceViews()
{
    if (!managerEnabled() || maxDeviceViewsHidden)
        return;
    if (!chainView || !viewRectFn || !originalLayout)
        return;
    static Display *dpy = XOpenDisplay(nullptr);
    if (!dpy)
        return;
    struct Node {
        unsigned long win;
        MaxViewRect rect;
    };
    Node nodes[64];
    int count = 0;
    void *node = *reinterpret_cast<void **>(static_cast<char *>(chainView) + 0x28);
    while (node && count < 64) {
        void *next = *reinterpret_cast<void **>(node);
        void *view = *reinterpret_cast<void **>(static_cast<char *>(node) + 0x08);
        const unsigned long win = *reinterpret_cast<unsigned long *>(static_cast<char *>(node) + 0x10);
        if (view && win) {
            nodes[count].win = win;
            nodes[count].rect = viewRectFn(view);
            ++count;
        }
        node = next;
    }
    if (!count)
        return;

    const qint64 now = monotonicMs();
    int k = 0;
    qint64 best = -1;
    for (int i = 0; i < count; ++i) {
        auto it = lastLayout.find(nodes[i].win);
        if (it != lastLayout.end() && now - it->when < 5000 && it->when > best) {
            k = it->x1 - nodes[i].rect.x1;
            best = it->when;
        }
    }
    if (best < 0) {
        if (managerKWhen && now - managerKWhen < 5000)
            k = managerK;
        else
            return;
    } else {
        managerK = k;
        managerKWhen = now;
    }

    const int left = viewportLeft;
    const int right = viewportRight > viewportLeft ? viewportRight : 2469;
    const int top = viewportTop;
    const int bottom = viewportBottom;
    for (int i = 0; i < count; ++i) {
        const int sx1 = nodes[i].rect.x1 + k, sy1 = nodes[i].rect.y1;
        const int sx2 = nodes[i].rect.x2 + k, sy2 = nodes[i].rect.y2;
        const int ix1 = qMax(sx1, left), iy1 = qMax(sy1, top);
        const int ix2 = qMin(sx2, right), iy2 = qMin(sy2, bottom);
        if (ix1 >= ix2 || iy1 >= iy2) {
            managerHidden.insert(nodes[i].win);
            XUnmapWindow(dpy, nodes[i].win);
        } else {
            managerHidden.remove(nodes[i].win);
            int visible[4] = {ix1, iy1, ix2, iy2};
            int full[4] = {sx1, sy1, sx2, sy2};
            originalLayout(reinterpret_cast<void *>(nodes[i].win), visible, full);
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

const char *moduleOf(void *address)
{
    Dl_info info;
    return dladdr(address, &info) && info.dli_fname ? info.dli_fname : "?";
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
    installMaxRectTrace();
    base = mainExecutableBase();
    installMaxLayoutTrace();
    installChainLayoutTrace();
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
    const int inX = x, inY = y;
    const unsigned inWidth = width, inHeight = height;
    bool mapAfter = false, unmapAfter = false;
    // Max for Live device windows. The Max runtime creates them as native X
    // children of Live's window (juce::Desktop and LinuxComponentPeer live in
    // libMaxPlugLib.so). Live hands their rectangle over in its own units
    // (zoom x scale, the units the rest of its views use), but an X window
    // needs pixels: unscaled, every Max device sat above and left of its slot
    // by (ratio - 1) times its position. followMaxRuntimeScale() puts the
    // runtime's own scale at Live's, so its painting fills the scaled window.
    if (!windowProps.contains(w) && height >= 100) {
        maxDeviceViews.insert(w);
        // With the runtime at Live's scale, its own window requests are
        // already in pixels; only Live's are in its unscaled units.
        const bool liveUnits = calledFromLive(__builtin_return_address(0));
        if (getenv("LIVE_LOG_MAXCALLER")) {
            if (const char *path = getenv("MENUFIX_DEBUG")) {
                if (FILE *f = fopen(path, "a")) {
                    fprintf(f, "maxscale: caller %s window 0x%x %d,%d %ux%u\n",
                            moduleOf(__builtin_return_address(0)), unsigned(w), x, y, width, height);
                    fclose(f);
                }
            }
        }
        if (const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
            liveUnits && ratio != 1.0) {
            x = qRound(x * ratio);
            y = qRound(y * ratio);
            width = unsigned(qRound(width * ratio));
            height = unsigned(qRound(height * ratio));
        }
        if (!managerEnabled()) {
        // Track the full (unclipped) size this window has had. Live shrinks
        // the host to the visible slice while scrolling; once that slice is a
        // small fraction of the device, hide it instead of leaving a sliver
        // over the device at the viewport edge. Scrolling back grows it again.
        const int fullWidth = qMax(maxDeviceWidth.value(w, 1), int(inWidth));
        const int fullHeight = qMax(maxDeviceHeight.value(w, 1), int(inHeight));
        maxDeviceWidth[w] = fullWidth;
        maxDeviceHeight[w] = fullHeight;
        const bool mostlyOut = int(inWidth) * 5 < fullWidth || int(inHeight) * 5 < fullHeight;

        // Live also has a layout bug when the chain jumps: it moves a clipped
        // view straight back to its base slot at full size, with the content at
        // 0, as if the chain were not scrolled. That leaves the window over
        // other devices. A real scroll first grows the view at the viewport
        // edge; only then does it move to its slot. Treat a clipped -> full
        // jump that skips the edge as stale and keep the window hidden until a
        // clipped or edge state arrives again.
        bool staleLayout = false;
        if (inY != 0) { // host windows; the content child sits at y = 0
            const int logicalWidth = int(inWidth);
            const int logicalX = inX;
            const bool clipped = logicalWidth < fullWidth;
            bool validFull = false;
            if (clipped) {
                // A clipped host sits at a viewport edge: the left one is at
                // the chain's small left margin, the right one further in.
                if (logicalX <= 64) {
                    if (logicalX < chainLeft)
                        chainLeft = logicalX;
                } else {
                    const int right = logicalX + logicalWidth;
                    if (right > chainRight)
                        chainRight = right;
                }
            } else {
                const bool atLeft = chainLeft != INT_MAX && logicalX <= chainLeft + 1;
                const bool atRight = chainRight > 0 && logicalX + fullWidth >= chainRight - 1;
                validFull = atLeft || atRight;
            }
            const bool wasClipped = lastLogicalWidth.value(w, fullWidth) < fullWidth;
            staleLayout = logicalWidth == fullWidth && !validFull && (wasClipped || scrollHidden.contains(w));
            lastLogicalWidth[w] = logicalWidth;
        }

        if (mostlyOut || staleLayout) {
            if (!scrollHidden.contains(w)) {
                scrollHidden.insert(w);
                unmapAfter = true;
                scrollTrace("scroll-hide w=0x%x %ux%u of %dx%d%s", unsigned(w), inWidth, inHeight, fullWidth,
                            fullHeight, staleLayout ? " stale" : "");
            }
        } else if (scrollHidden.remove(w)) {
            mapAfter = true;
            scrollTrace("scroll-show w=0x%x %ux%u of %dx%d", unsigned(w), inWidth, inHeight, fullWidth,
                        fullHeight);
        }        }

    }
    if (scrollTraceEnabled() && !windowProps.contains(w)) {
        XWindowAttributes attributes{};
        const bool mapped = XGetWindowAttributes(dpy, w, &attributes) && attributes.map_state != IsUnmapped;
        scrollTrace("move w=0x%x max=%d mapped=%d in=%d,%d %ux%u out=%d,%d %ux%u dpr=%.3f caller=%s",
                    unsigned(w), int(maxDeviceViews.contains(w)), int(mapped), inX, inY, inWidth, inHeight,
                    x, y, width, height, qApp ? double(qApp->devicePixelRatio()) : 1.0,
                    moduleOf(__builtin_return_address(0)));
    }
    if (!windowProps.contains(w)) {
        const int result = real(dpy, w, x, y, width, height);
        if (unmapAfter && !maxDeviceViewsHidden)
            XUnmapWindow(dpy, w);
        if (mapAfter && !maxDeviceViewsHidden)
            XMapWindow(dpy, w);
        return result;
    }
    fitSizeHints(dpy, w, width, height);
    rememberSize(w, width, height);
    return real(dpy, w, x, y, width, height);
}

extern "C" int XMapWindow(Display *dpy, Window w)
{
    static auto real = next<decltype(&XMapWindow)>("XMapWindow");
    if (scrollTraceEnabled() && !windowProps.contains(w))
        scrollTrace("map w=0x%x max=%d hidden=%d", unsigned(w), int(maxDeviceViews.contains(w)),
                    int(maxDeviceViewsHidden));
    if (pendingUnmaps.remove(w)) {
        menufixTrace("pluginclose: editor 0x%x hidden and shown again, kept mapped\n", int(w));
        return 1;
    }
    if (windowProps.contains(w))
        fitToView(dpy, w);
    else if (scrollHidden.contains(w) || managerHidden.contains(w)
             || (maxDeviceViewsHidden && maxDeviceViews.contains(w)))
        return 1; // scrolled out of the chain, or the device view is not shown
    return real(dpy, w);
}

extern "C" int XUnmapWindow(Display *dpy, Window w)
{
    static auto real = next<decltype(&XUnmapWindow)>("XUnmapWindow");
    if (scrollTraceEnabled() && !windowProps.contains(w))
        scrollTrace("unmap w=0x%x max=%d", unsigned(w), int(maxDeviceViews.contains(w)));
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
    scrollHidden.remove(w);
    managerHidden.remove(w);
    lastLayout.remove((unsigned long)w);
    maxDeviceWidth.remove(w);
    maxDeviceHeight.remove(w);
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
        else if (!scrollHidden.contains(w) && !managerHidden.contains(w))
            XMapWindow(dpy, w); // Live does not map them again by itself
    }
    XFlush(dpy);
    XCloseDisplay(dpy);
    menufixTrace(hidden ? "pluginclose: device views hidden (%d)\n" : "pluginclose: device views shown (%d)\n",
                 int(maxDeviceViews.size()));
}


// Called on a timer: hide the Max device views whenever Live is showing the
// clip view instead of the device view.


