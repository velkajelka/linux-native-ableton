// VST3 plug-in editor windows for Live 11.3.43 (Push 3 1.2.23 build).
//
// Live 11's plug-in code asks a factory (0x33a3970) for an IVst3Window that
// shows a plug-in's IPlugView. The Push 3 build compiled that window as a stub:
// the factory makes an 8-byte object whose methods do nothing, and frees the
// view. The device's wrench button therefore opened no editor. Live 12's Linux
// build has the real window (TVst3Window, factory 0x3e93290 in 12.4.7, vtable
// 0x967c6c0), and Live 11 passes the factory the same arguments (out, the
// {bool, IPlugView *} view holder, a "closed" and a "moved" std::function). The
// factory is replaced here by a port of Live 12's window, method by method,
// together with what the Live 12 shim changes in it (pluginclose.cpp,
// vst3runloop.cpp, window.cpp's liveOwnPluginWindow). Live 12 (12.4.7) does:
//  Attach (0x3e93750): setFrame(frame); position = the saved one, or a global
//    "next editor position" (40, 60); getSize, at least 10 x 10; XCreateWindow on
//    the root window (no attributes); fixed size hints when canResize is
//    kResultFalse;
//    SetTitle; a 50 ms poll (Live's timer list, 0x1d8c3c0); attached(window,
//    "X11EmbedWindowID"), then Show, or Close when attached fails.
//  SetRect (0x3e935e0, the plug-in frame's resizeView): XMoveResizeWindow at the
//    saved position, onSize with the plug-in's rectangle; re-entry returns 0.
//  Close (0x3e93da0): Hide if visible; removed() if attached; setFrame(null);
//    XDestroyWindow; the poll removed.
//  Show / Hide / Raise: XMapWindow / XUnmapWindow / XRaiseWindow.
//  Poll (0x3e942e0): window attributes gone -> "closed"; position changed ->
//    "moved" with the window's root position.
//  Log lines in Log.txt with the same texts and levels (Live 11's logger:
//    0x3eb8e40 entry, 0x3eb3170 enabled, 0x3eb10c0 text, 0x3eb0b70 write), and
//    Live 12's X error labels ("VST3: failed to map platform window", ...).
// The Live 12 shim adds, and so does this file: WM_DELETE_WINDOW (the close
// button calls "closed" instead of KWin killing the connection), the run loop
// for plug-ins (IRunLoop), editor class/transient/icon/no-minimize
// (liveOwnPluginWindow), StaticGravity + USPosition in the hints, hints moved
// along on resizes, sizing to the view's own window before mapping, a 50 ms
// deferred unmap dropped by a map in that time, window-frame resizes passed to
// the plug-in, and the desktop scale through IPlugViewContentScaleSupport.
// Live 12 can step that position by +7, +12 for each new editor when Live asks
// for it (0x3d56530), and its poll translates the point (x, y) of
// XGetWindowAttributes (the position in the parent window) to the root. The Live
// 12 shim turns both off (standalone.cpp's byte patches 0x3d56530 and 0x3e943d8):
// every new editor opens at 40, 60, and the position is the window's origin. Live
// 11 asks for the step on every new editor (0x3447760, the same code as Live 12's
// 0x3f1a5e0: the caller's flag or the Multiple Plug-In Windows option); it is
// ignored here, as in Live 12 with its shim.
// Remaining difference: the window is on its own Xlib connection (Live 12 uses its
// X connection manager, which Live 11 does not have; X errors are logged with
// Live 12's labels by this file's handler).
//
// Live 11's IVst3Window has one method fewer before HasWindow than Live 12's
// (Live 12 moved SetRect from the last slot to slot 2): Live 11 calls
// HasWindow at +0x10, Show at +0x48, Close at +0x30 and Attach at +0x20
// (0x34ac900), where Live 12 uses +0x18, +0x50, +0x38 and +0x28 (0x3f909e0).
// The stub's last slot returns 3 (kNotImplemented): SetRect's tresult.
#include "livebuild.h"
#include <QGuiApplication>
#include <QHash>
#include <QScreen>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>
#include <QWindow>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cfenv>
#include <cstdint>
#include <cstring>
#include <functional>
#include <link.h>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
bool redirectFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *target); // hook.cpp
void *qtRunLoop();                                                                            // vst3runloop.cpp
void liveOwnPluginWindow(Display *dpy, Window editor);                                        // window.cpp

// Not in an anonymous namespace: GCC would then compile calls into the
// plug-in's objects to __cxa_pure_virtual (see vst3runloop.cpp).
namespace vst3win {

using tresult = int32_t;
constexpr tresult kResultOk = 0;
constexpr tresult kResultFalse = 1;
constexpr tresult kInvalidArgument = 2;
constexpr tresult kNoInterface = -1;

// Non-COM (Linux) FUID byte order: four big-endian 32-bit words.
constexpr unsigned char kFUnknownIID[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                            0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
constexpr unsigned char kIPlugFrameIID[16] = {0x36, 0x7F, 0xAF, 0x01, 0xAF, 0xA9, 0x46, 0x93,
                                              0x8D, 0x4D, 0xA2, 0xA0, 0xED, 0x08, 0x82, 0xA3};
constexpr unsigned char kIRunLoopIID[16] = {0x18, 0xC3, 0x53, 0x66, 0x97, 0x76, 0x4F, 0x1A,
                                            0x9C, 0x5B, 0x83, 0x85, 0x7A, 0x87, 0x13, 0x89};
constexpr unsigned char kIPlugViewContentScaleSupportIID[16] = {0x65, 0xED, 0x96, 0x90, 0x8A, 0xC4, 0x45, 0x25,
                                                                0x8A, 0xAD, 0xEF, 0x7A, 0x72, 0xEA, 0x70, 0x3F};

struct ViewRect {
    int32_t left, top, right, bottom;
};

struct FUnknown {
    virtual tresult queryInterface(const char *iid, void **obj) = 0;
    virtual uint32_t addRef() = 0;
    virtual uint32_t release() = 0;
};

struct IPlugView;

struct IPlugFrame : FUnknown {
    virtual tresult resizeView(IPlugView *view, ViewRect *newSize) = 0;
};

struct IPlugView : FUnknown {
    virtual tresult isPlatformTypeSupported(const char *type) = 0;
    virtual tresult attached(void *parent, const char *type) = 0;
    virtual tresult removed() = 0;
    virtual tresult onWheel(float distance) = 0;
    virtual tresult onKeyDown(char16_t key, int16_t keyCode, int16_t modifiers) = 0;
    virtual tresult onKeyUp(char16_t key, int16_t keyCode, int16_t modifiers) = 0;
    virtual tresult getSize(ViewRect *size) = 0;
    virtual tresult onSize(ViewRect *newSize) = 0;
    virtual tresult onFocus(uint8_t state) = 0;
    virtual tresult setFrame(IPlugFrame *frame) = 0;
    virtual tresult canResize() = 0;
    virtual tresult checkSizeConstraint(ViewRect *rect) = 0;
};

struct IPlugViewContentScaleSupport : FUnknown {
    virtual tresult setContentScaleFactor(float factor) = 0;
};

// Live's arguments.
struct Position {
    int32_t x, y; // x == -1: no saved position
};
struct ViewHolder { // made by Live (0x34aca60), owned by the window
    bool flag;
    IPlugView *view;
};

uintptr_t base; // Live's load address

// Live's TString: a pointer to a block {refcount, size in bytes with the
// terminator, ...} with UTF-16 text at +0x10 (0x1954f00 converts it).
QString liveString(const void *title)
{
    auto *block = *static_cast<const char *const *>(title);
    if (!block)
        return QString();
    const int32_t bytes = *reinterpret_cast<const int32_t *>(block + 4);
    const int length = bytes / 2 - 1;
    return length > 0 ? QString::fromUtf16(reinterpret_cast<const char16_t *>(block + 0x10), length) : QString();
}

// A line in Live's Log.txt, as Live 11 writes one (0x34b01b3): level 3 is
// "warning", 4 "error".
enum LogLevel { kWarning = 3, kError = 4 };
void liveLog(LogLevel level, const char *text)
{
    alignas(16) unsigned char entry[256] = {};
    reinterpret_cast<void (*)(void *, int)>(base + 0x3eb8e40)(entry, level);
    if (reinterpret_cast<bool (*)(void *)>(base + 0x3eb3170)(entry))
        reinterpret_cast<void (*)(void *, const char *)>(base + 0x3eb10c0)(entry, text);
    reinterpret_cast<void (*)(void *)>(base + 0x3eb0b70)(entry);
}

// Live's timer list (0x1967030 add, 0x19672a0 remove; Live 12: 0x1d8c3c0,
// 0x1d8c610): entries {function, context}, called every 50 ms from Live's loop.
struct LiveTask {
    void (*fn)(void *context, void *);
    void *context;
};
void addLiveTask(LiveTask task) { reinterpret_cast<void (*)(LiveTask *)>(base + 0x1967030)(&task); }
void removeLiveTask(LiveTask task) { reinterpret_cast<void (*)(LiveTask *)>(base + 0x19672a0)(&task); }

class EditorWindow;

// The plug-in's IPlugFrame (Live 12: TPlugFrame, vtable 0x967c740), reference
// counted; the window drops its reference when it goes.
class PlugFrame final : public IPlugFrame {
public:
    explicit PlugFrame(EditorWindow *owner) : owner(owner) {}
    tresult queryInterface(const char *iid, void **obj) override;
    uint32_t addRef() override { return ++refs; }
    uint32_t release() override
    {
        const uint32_t left = --refs;
        if (!left)
            delete this;
        return left;
    }
    tresult resizeView(IPlugView *view, ViewRect *newSize) override;
    EditorWindow *owner;

private:
    uint32_t refs = 1;
};

// Live 12 saves and restores the floating-point environment around every call
// into the plug-in's view (0x3e94500 and the like: fegetenv/fesetenv).
struct FpGuard {
    fenv_t env;
    FpGuard() { fegetenv(&env); }
    ~FpGuard() { fesetenv(&env); }
};

Display *display;
Atom wmProtocols, wmDeleteWindow;
QHash<Window, EditorWindow *> windows;
const Position nextPosition{40, 60}; // Live 12: 0x9b44628

// Live 12 labels its X requests (0x1fe55c0); an X error on them is logged with
// the label. Here the labels are kept by request serial and logged by an error
// handler for this connection; other connections go to the previous handler.
struct Label {
    unsigned long serial;
    const char *text;
};
Label labels[32];
unsigned nextLabel;
XErrorHandler previousHandler;

void label(const char *text)
{
    labels[nextLabel++ % 32] = {NextRequest(display), text};
}

int onXError(Display *dpy, XErrorEvent *error)
{
    if (dpy != display)
        return previousHandler ? previousHandler(dpy, error) : 0;
    const Label *best = nullptr;
    for (const Label &l : labels)
        if (l.text && l.serial <= error->serial && (!best || l.serial > best->serial))
            best = &l;
    char text[160];
    XGetErrorText(dpy, error->error_code, text, sizeof text);
    menufixTrace("vst3window: X error %d (request %d)\n", error->error_code, error->request_code);
    liveLog(kError, best ? best->text : text);
    return 0;
}

void pumpEvents();

bool openDisplay()
{
    if (display)
        return true;
    display = XOpenDisplay(nullptr);
    if (!display) {
        menufixTrace("vst3window: cannot open the X display\n");
        return false;
    }
    previousHandler = XSetErrorHandler(onXError);
    wmProtocols = XInternAtom(display, "WM_PROTOCOLS", False);
    wmDeleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);
    auto *notifier = new QSocketNotifier(ConnectionNumber(display), QSocketNotifier::Read);
    QObject::connect(notifier, &QSocketNotifier::activated, notifier, [] { pumpEvents(); });
    return true;
}

// Events Xlib already read while waiting for a reply do not wake the socket
// notifier: requests are followed by a pump on the next loop pass.
void flush()
{
    XFlush(display);
    QTimer::singleShot(0, [] { pumpEvents(); });
}

qreal editorScale()
{
    if (QWindow *w = QGuiApplication::focusWindow())
        return w->devicePixelRatio();
    return QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1;
}

// The Live 12 shim's hints for editor windows (pluginclose.cpp's
// XSetWMNormalHints and fitSizeHints): StaticGravity and user position always;
// a fixed size stays fixed at a new size; a range is widened to include it.
void setEditorHints(Window w, XSizeHints *hints)
{
    hints->flags |= PWinGravity | USPosition | PPosition;
    hints->win_gravity = StaticGravity;
    XSetWMNormalHints(display, w, hints);
}

void fitSizeHints(Window w, int width, int height)
{
    XSizeHints *hints = XAllocSizeHints();
    long supplied = 0;
    if (hints && XGetWMNormalHints(display, w, hints, &supplied)) {
        const bool fixed = (hints->flags & PMinSize) && (hints->flags & PMaxSize)
                           && hints->min_width == hints->max_width && hints->min_height == hints->max_height;
        if (fixed) {
            hints->min_width = hints->max_width = width;
            hints->min_height = hints->max_height = height;
        } else {
            if (hints->flags & PMinSize) {
                hints->min_width = qMin(hints->min_width, width);
                hints->min_height = qMin(hints->min_height, height);
            }
            if (hints->flags & PMaxSize) {
                hints->max_width = qMax(hints->max_width, width);
                hints->max_height = qMax(hints->max_height, height);
            }
        }
        setEditorHints(w, hints);
    }
    if (hints)
        XFree(hints);
}

class EditorWindow {
public:
    using Closed = std::function<void()>;
    using Moved = std::function<void(const Position &)>;

    EditorWindow(ViewHolder *holder, const Closed &closed, const Moved &moved)
        : holder(holder), view(holder ? holder->view : nullptr), closed(closed), moved(moved), frame(new PlugFrame(this))
    {
    }

    // IVst3Window, Live 11 order (slots 0 and 1 are the destructors).
    // Live 12: 0x3e934e0.
    virtual ~EditorWindow()
    {
        if (window)
            close();
        if (holder) {
            if (holder->view)
                holder->view->release();
            ::operator delete(holder);
        }
        frame->owner = nullptr;
        frame->release();
    }

    virtual bool hasWindow() { return window != 0; } // 0x3e93730

    virtual bool isVisible() { return visible; } // 0x3e93740

    // Live 12: 0x3e93750.
    virtual void attach(const Position *at, const void *title, bool cascade)
    {
        if (window || !view || !openDisplay())
            return;
        setScale(); // the Live 12 shim does this when Live creates the view
        tresult result;
        {
            FpGuard fp;
            result = view->setFrame(frame);
        }
        if (result != kResultOk)
            liveLog(kWarning, "VST3: setting the plugin editor's resize handler failed");
        // No cascade step (cascade is Live's request; the Live 12 shim disables the step).
        menufixTrace("vst3window: attach, saved position %d, cascade asked %d\n", at && at->x != -1, cascade);
        pos = at && at->x != -1 ? *at : nextPosition;
        ViewRect size{};
        {
            FpGuard fp;
            result = view->getSize(&size);
        }
        if (result != kResultOk)
            liveLog(kError, "VST3: requesting size of VST3 plugin view failed");
        width = qMax(size.right - size.left, 10);
        height = qMax(size.bottom - size.top, 10);
        label("VST3: failed to create platform window");
        window = XCreateWindow(display, DefaultRootWindow(display), pos.x, pos.y, unsigned(width), unsigned(height), 0, 0,
                               0, nullptr, 0, nullptr);
        XSync(display, False); // Live 12 checks its error trap here
        if (!window)
            return;
        windows.insert(window, this);
        // What the Live 12 shim adds to a new editor window (pluginclose.cpp's XCreateWindow).
        XSetWMProtocols(display, window, &wmDeleteWindow, 1);
        XSelectInput(display, window, StructureNotifyMask);
        liveOwnPluginWindow(display, window);
        {
            FpGuard fp;
            result = view->canResize();
        }
        if (result == kResultFalse) {
            XSizeHints *hints = XAllocSizeHints();
            hints->flags = PMinSize | PMaxSize;
            hints->min_width = hints->max_width = width;
            hints->min_height = hints->max_height = height;
            setEditorHints(window, hints);
            XFree(hints);
        }
        setTitle(title);
        addLiveTask({&pollTask, this});
        {
            FpGuard fp;
            result = view->attached(reinterpret_cast<void *>(window), "X11EmbedWindowID");
        }
        if (result == kResultOk) {
            show();
            isAttached = true;
            menufixTrace("vst3window: editor 0x%x attached, %dx", int(window), width);
            menufixTrace("%d\n", height);
        } else {
            liveLog(kError, "VST3: Attaching the plugin editor window failed.");
            close();
        }
    }

    // Live 12: 0x3e93cb0.
    virtual void setTitle(const void *title)
    {
        if (!window)
            return;
        label("VST3: failed to set window title");
        const QByteArray name = title ? liveString(title).toUtf8() : QByteArray();
        XStoreName(display, window, name.constData());
        flush();
    }

    // Live 12: 0x3e93da0.
    virtual void close()
    {
        if (isVisible())
            hide();
        tresult result;
        if (isAttached) {
            isAttached = false;
            FpGuard fp;
            if ((result = view->removed()) != kResultOk)
                liveLog(kWarning, "VST3: removing the plugin editor from the window failed");
        }
        {
            FpGuard fp;
            result = view->setFrame(nullptr);
        }
        if (result != kResultOk)
            liveLog(kWarning, "VST3: resetting the plugin editor resize handler failed");
        if (!window)
            return;
        label("VST3: failed to destroy platform Window");
        windows.remove(window);
        pendingUnmap = false;
        XDestroyWindow(display, window);
        removeLiveTask({&pollTask, this});
        window = 0;
        flush();
    }

    // Live 12: 0x3e93f90.
    virtual void raise()
    {
        if (!window)
            return;
        label("VST3: failed to raise platform Window");
        XRaiseWindow(display, window);
        flush();
    }

    // Live 12: 0x3e94060; the unmap deferred by 50 ms as in the Live 12 shim.
    virtual void hide()
    {
        visible = false;
        if (pendingUnmap)
            return;
        pendingUnmap = true;
        const Window w = window;
        QTimer::singleShot(50, [w] {
            EditorWindow *editor = windows.value(w);
            if (!editor || !editor->pendingUnmap)
                return;
            editor->pendingUnmap = false;
            label("VST3: failed to unmap platform window");
            XUnmapWindow(display, w);
            flush();
        });
    }

    // Live 12: 0x3e94130; sized to the view first, as in the Live 12 shim.
    virtual void show()
    {
        if (!window)
            return;
        visible = true;
        if (pendingUnmap) {
            pendingUnmap = false; // hidden and shown again: still mapped
            flush();
            return;
        }
        fitToView();
        label("VST3: failed to map platform window");
        XMapWindow(display, window);
        flush();
    }

    // Live 12: 0x3e935e0 (slot 2 there), called by the plug-in frame's resizeView.
    virtual tresult setRect(const ViewRect *rect)
    {
        if (resizing)
            return kResultOk;
        if (!window)
            return kResultFalse;
        resizing = true;
        width = rect->right - rect->left;
        height = rect->bottom - rect->top;
        label("VST3: failed to move/resize platform window");
        fitSizeHints(window, width, height);
        XMoveResizeWindow(display, window, pos.x, pos.y, unsigned(width), unsigned(height));
        XFlush(display);
        {
            FpGuard fp;
            view->onSize(const_cast<ViewRect *>(rect));
        }
        resizing = false;
        flush();
        return kResultOk;
    }

    // ConfigureNotify (the Live 12 shim's XNextEvent): a size the window manager
    // gave the window goes to the plug-in 30 ms after the last one.
    void configured(const XConfigureEvent &event)
    {
        wantWidth = event.width;
        wantHeight = event.height;
        if (!framePending && (wantWidth != width || wantHeight != height)) {
            framePending = true;
            const Window w = window;
            QTimer::singleShot(30, [w] {
                if (EditorWindow *editor = windows.value(w))
                    editor->applyFrameResize();
            });
        }
    }

    void closeRequested()
    {
        menufixTrace("vst3window: close requested for 0x%x\n", int(window));
        if (closed)
            closed(); // Live's own path, as the wrench button
    }

    bool pendingUnmap = false;

private:
    // Live 12: 0x3e942e0, every 50 ms.
    static void pollTask(void *context, void *) { static_cast<EditorWindow *>(context)->poll(); }
    void poll()
    {
        label("VST3: failed to get window attributes");
        XWindowAttributes attributes;
        if (!XGetWindowAttributes(display, window, &attributes)) {
            if (hasWindow() && closed)
                closed();
            return;
        }
        label("VST3: failed to translate window coordinates");
        int x = 0, y = 0;
        Window child;
        if (!XTranslateCoordinates(display, window, attributes.root, 0, 0, &x, &y, &child)
            || (x == pos.x && y == pos.y))
            return;
        pos = {x, y};
        if (moved)
            moved(pos);
    }

    void setScale()
    {
        static const bool enabled = qEnvironmentVariable("LIVE_PLUGIN_SCALE") != QLatin1String("0");
        void *support = nullptr;
        if (scaled || !enabled)
            return;
        scaled = true; // once per view, as the Live 12 shim's createView hook
        if (view->queryInterface(reinterpret_cast<const char *>(kIPlugViewContentScaleSupportIID), &support) != kResultOk
            || !support) {
            menufixTrace("vst3window: view has no content scale support\n");
            return;
        }
        const float scale = float(editorScale());
        const tresult r = static_cast<IPlugViewContentScaleSupport *>(support)->setContentScaleFactor(scale);
        static_cast<IPlugViewContentScaleSupport *>(support)->release();
        menufixTrace("vst3window: content scale %d%% -> %d\n", qRound(scale * 100), r);
    }

    // The Live 12 shim's fitToView: the plug-in sizes its view in attached()
    // (u-he at a saved GUI scale) and asks for that size later; the editor takes
    // the size of the view's own window (its first mapped child) before it shows.
    void fitToView()
    {
        XWindowAttributes outer;
        Window root, parent, *kids = nullptr;
        unsigned count = 0;
        if (!XGetWindowAttributes(display, window, &outer) || !XQueryTree(display, window, &root, &parent, &kids, &count))
            return;
        for (unsigned i = 0; i < count; ++i) {
            XWindowAttributes child;
            if (!XGetWindowAttributes(display, kids[i], &child) || child.map_state == IsUnmapped || child.c_class == InputOnly)
                continue;
            if (child.width != outer.width || child.height != outer.height) {
                menufixTrace("vst3window: editor 0x%x fitted to its view, %dx", int(window), child.width);
                menufixTrace("%d\n", child.height);
                width = child.width;
                height = child.height;
                fitSizeHints(window, width, height);
                XResizeWindow(display, window, unsigned(width), unsigned(height));
            }
            break;
        }
        if (kids)
            XFree(kids);
    }

    // The Live 12 shim's applyFrameResize.
    void applyFrameResize()
    {
        framePending = false;
        if (!window || (wantWidth == width && wantHeight == height))
            return;
        ViewRect rect = {0, 0, wantWidth, wantHeight};
        FpGuard fp;
        if (view->canResize() == kResultOk) {
            view->checkSizeConstraint(&rect);
        } else {
            ViewRect own = {};
            if (view->getSize(&own) != kResultOk)
                return;
            rect = own;
        }
        const int w = rect.right - rect.left, h = rect.bottom - rect.top;
        if (w <= 0 || h <= 0)
            return;
        if (w != wantWidth || h != wantHeight) {
            fitSizeHints(window, w, h);
            XResizeWindow(display, window, unsigned(w), unsigned(h));
        }
        width = w;
        height = h;
        rect = {0, 0, w, h};
        view->onSize(&rect);
        flush();
        menufixTrace("vst3window: editor 0x%x resized by its frame to %dx", int(window), w);
        menufixTrace("%d\n", h);
    }

    ViewHolder *holder;
    IPlugView *view;
    Closed closed;
    Moved moved;
    PlugFrame *frame;
    Window window = 0;
    Position pos{0, 0};
    int width = 10, height = 10;
    int wantWidth = 0, wantHeight = 0;
    bool visible = false;
    bool isAttached = false;
    bool resizing = false;
    bool framePending = false;
    bool scaled = false;
};

// Live 12: 0x3e94210 (FUnknown, IPlugFrame); IRunLoop as the Live 12 shim adds it.
tresult PlugFrame::queryInterface(const char *iid, void **obj)
{
    if (!memcmp(iid, kFUnknownIID, 16) || !memcmp(iid, kIPlugFrameIID, 16)) {
        addRef();
        *obj = static_cast<IPlugFrame *>(this);
        return kResultOk;
    }
    if (!memcmp(iid, kIRunLoopIID, 16) && qtRunLoop()) {
        *obj = qtRunLoop();
        menufixTrace("vst3window: run loop handed to the plug-in\n");
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

// Live 12: 0x3e942b0.
tresult PlugFrame::resizeView(IPlugView *, ViewRect *newSize)
{
    if (!newSize)
        return kInvalidArgument;
    if (!owner)
        return kResultFalse;
    return owner->setRect(newSize);
}

void pumpEvents()
{
    while (display && XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);
        EditorWindow *editor = windows.value(event.xany.window);
        if (!editor)
            continue;
        if (event.type == ConfigureNotify && event.xconfigure.window == event.xconfigure.event) {
            editor->configured(event.xconfigure);
        } else if (event.type == ClientMessage && event.xclient.message_type == wmProtocols
                   && Atom(event.xclient.data.l[0]) == wmDeleteWindow) {
            const Window w = event.xany.window;
            // As the Live 12 shim: from the next loop pass, if the window still exists.
            QTimer::singleShot(0, [w] {
                if (EditorWindow *e = windows.value(w))
                    e->closeRequested();
            });
        }
    }
}

// Replaces Live 11's factory (0x33a3970; Live 12: 0x3e93290): out = unique_ptr<IVst3Window>.
void **makeWindow(void **out, ViewHolder **holder, const EditorWindow::Closed *closed, const EditorWindow::Moved *moved)
{
    ViewHolder *taken = *holder;
    *holder = nullptr;
    *out = new EditorWindow(taken, *closed, *moved);
    menufixTrace("vst3window: window for view %d\n", taken && taken->view);
    return out;
}

} // namespace vst3win

void installVst3Window11()
{
    if (liveBuild() != LiveBuild::Live11_3_43)
        return;
    static const unsigned char kPrologue[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x50,
                                              0x48, 0x89, 0xf3, 0x49, 0x89, 0xfe};
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *out) {
            *static_cast<uintptr_t *>(out) = info->dlpi_addr; // first entry = main program
            return 1;
        },
        &vst3win::base);
    const bool ok = redirectFunction(vst3win::base + 0x33a3970, kPrologue, sizeof kPrologue,
                                     reinterpret_cast<void *>(&vst3win::makeWindow));
    menufixTrace("vst3window: factory %d\n", ok);
}
