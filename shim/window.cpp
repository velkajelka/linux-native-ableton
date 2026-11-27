// Window-manager integration for AbletonLive12's top-level windows.
//
// Windows Live is a native Win32 app: its windows carry the app icon, and it
// answers WM_GETMINMAXINFO, so the shell knows the minimum window size before
// a resize starts. The Push build's Qt windows have neither: no _NET_WM_ICON,
// WM_CLASS "AbletonLive12", and no minimum size in WM_NORMAL_HINTS. Live keeps
// its minimum (about 800x620) by resizing the window back after the window
// manager made it smaller, so every interactive resize below the limit
// fights the window manager.
//
// This file sets the app icon (exported from the Windows build by
// tools/make_linux_assets.py), names the window class, and publishes Live's
// own minimum: after every frame of a window, the minimum Live clamps the
// window to (AWindow's virtual at +0x200, in Live's units; the clamp is
// 0x20c3de0) is converted to pixels as Live converts it (x zoom x the
// window's scale, virtual +0x588) and set as the window's minimum size, so
// the window manager stops at it. A minimum is never larger than the
// screen's work area.
//
// Zoom changes: Live's SetZoom (0x20c4110) keeps a window's size by
// converting it to Live's units and back, and resizes the window to the
// result, which is off by a rounding error (1920x1173 for 1172 at 130 %), even
// when the window is maximized. Repeated zoom changes made a normal window
// creep, and a maximized one could be pushed past the work area. During
// SetZoom such a resize is dropped: the window keeps its pixel size, as on
// Windows. Any resize Live asks for is limited to the work area of the
// window's screen (with the window frame), so a window Live restores or
// enlarges never ends up under a panel or past the screen edge. The work area
// is read in X pixels (KWin on Wayland reports it in scaled units), and a
// maximized or full-screen window is left at the window manager's size.
//
// Plug-in editor windows: Windows Live creates them as windows owned by the
// main window, so they stay above it and minimize with it. The Push build
// creates them as plain top-level X windows on its own Xlib connection, so
// clicking Live's main window buries the editor behind it. The editor
// windows (seen by pluginclose.cpp's XCreateWindow interposer) are made
// transient for Live's active main window, with Live's window class and icon.
//
// Title bar colours: KDE's window manager draws a window's title bar from the
// colour scheme named by the window's _KDE_NET_WM_COLOR_SCHEME property (the
// absolute path of a .colors file; type STRING), read again when the property
// changes. applyTitleBarColours writes a scheme with Live's band colour into
// Live's data folder and names it on Live's windows, so the title bar and
// Live's top band read as one surface; it runs again on every theme change.
//
// Splash screen zoom (Live 12.4.7): the splash screen always opened at 100 %
// whatever Zoom the main window had. The shim records the main window's Zoom
// in Live's config folder (LinuxSplashZoom.txt, splashzoom.cpp) whenever it
// changes, and the splash opens at that Zoom at the next start. Every other
// window opens as before.
#include "chrome.h"
#include "editorcolour.h"
#include "livebuild.h"
#include "splashzoom.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QIcon>
#include <QMainWindow>
#include <QPointer>
#include <QScreen>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <link.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp
void *hookFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *replacement); // hook.cpp
bool scaleTransition(); // scalechange.cpp

namespace {

bool isLiveMainWindow(QWidget *w)
{
    return w && w->isWindow() && w->windowType() == Qt::Window && w->inherits("QMainWindow");
}

} // namespace

// The shim's own connection to the X server (GUI thread only; also used by
// editorcolour.cpp).
Display *ownDisplay()
{
    static Display *dpy = XOpenDisplay(nullptr);
    return dpy;
}

namespace {

QByteArray schemePath; // the title bar colour scheme in use, empty before the first

// Windows with a title bar.
bool decorated(QWidget *w)
{
    return w->isWindow() && (w->windowType() == Qt::Window || w->windowType() == Qt::Dialog);
}

void setColorScheme(QWidget *w)
{
    Display *dpy = ownDisplay();
    if (!dpy || schemePath.isEmpty() || !w->internalWinId())
        return;
    XChangeProperty(dpy, w->internalWinId(), XInternAtom(dpy, "_KDE_NET_WM_COLOR_SCHEME", False), XA_STRING, 8,
                    PropModeReplace, reinterpret_cast<const unsigned char *>(schemePath.constData()), schemePath.size());
}

// The Live main window whose view is being painted (see liveWindowPainted).
QPointer<QWidget> paintingWindow;

class WindowFilter : public QObject
{
public:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Show && o->isWidgetType() && !schemePath.isEmpty()) {
            auto *w = static_cast<QWidget *>(o);
            // Once Qt has sent its own requests for the new window (the
            // window manager reads the property again when it changes).
            if (decorated(w))
                QTimer::singleShot(0, w, [w] {
                    setColorScheme(w);
                    XFlush(ownDisplay());
                });
        } else if (e->type() == QEvent::Paint && o->isWidgetType()) {
            QWidget *window = static_cast<QWidget *>(o)->window();
            if (isLiveMainWindow(window) && static_cast<QMainWindow *>(window)->centralWidget() == o)
                paintingWindow = window;
        }
        return false;
    }
};

void setAppIcon()
{
    const QDir dir(QCoreApplication::applicationDirPath() + "/products/live/Live/AppLive/Linux/Icons");
    QIcon icon;
    for (const QString &f : dir.entryList({"live_*.png"}))
        icon.addFile(dir.filePath(f));
    if (!icon.isNull())
        QApplication::setWindowIcon(icon);
}

// Motif hints (_MOTIF_WM_HINTS): flags, functions, decorations, input mode,
// status. Removes the minimize function and keeps everything else, merged
// with whatever the window already has. Mutter, Xfwm, Openbox and most other
// window managers then show no minimize button; KWin ignores the bit, so on
// KDE editors keep the button and minimize with Live's main window.
void setNoMinimize(Display *dpy, Window window)
{
    constexpr unsigned long kHintsFunctions = 1, kFuncAll = 1, kFuncMinimize = 8;
    const Atom atom = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    unsigned long hints[5] = {};
    Atom type;
    int format;
    unsigned long items, rest;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy, window, atom, 0, 5, False, atom, &type, &format, &items, &rest, &data) == Success
        && data && format == 32)
        memcpy(hints, data, std::min<unsigned long>(items, 5) * sizeof(long));
    if (data)
        XFree(data);
    if (!(hints[0] & kHintsFunctions))
        hints[1] = kFuncAll; // no function hint yet: all functions allowed
    // With MWM_FUNC_ALL set the listed functions are the ones removed.
    hints[1] = (hints[1] & kFuncAll) ? hints[1] | kFuncMinimize : hints[1] & ~kFuncMinimize;
    hints[0] |= kHintsFunctions;
    XChangeProperty(dpy, window, atom, atom, 32, PropModeReplace, reinterpret_cast<unsigned char *>(hints), 5);
}

// Root window property `name` (32-bit cardinals), empty when missing.
std::vector<long> rootCardinals(Display *dpy, const char *name)
{
    std::vector<long> values;
    Atom type;
    int format;
    unsigned long items, rest;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), XInternAtom(dpy, name, False), 0, 1024, False, XA_CARDINAL,
                           &type, &format, &items, &rest, &data)
            == Success
        && data && format == 32)
        values.assign(reinterpret_cast<long *>(data), reinterpret_cast<long *>(data) + items);
    if (data)
        XFree(data);
    return values;
}

// The work area of `screen` in X pixels.
//
// KWin on Wayland writes _NET_WORKAREA and _NET_DESKTOP_GEOMETRY in its own
// scaled units, not in X pixels, when Xwayland clients scale themselves
// (Plasma's default): a 1920x1080 screen at 125 % has the work area 0,0
// 1536x864. Qt takes it as pixels, so its availableGeometry() was 1/1.25 of the
// screen. After the desktop scale was changed while Live ran, maximizing Live
// gave a 1920x1045 window with Live drawn at 1536x836 in its corner and the
// rest black: Live asks for its window size again and again, and each request
// was cut to that work area. The work area is scaled back by the ratio of the
// root window's size to _NET_DESKTOP_GEOMETRY (1 elsewhere: X11 sessions,
// Plasma's legacy scaling, other window managers).
QRect nativeWorkArea(QScreen *screen)
{
    // Qt 5 keeps a screen's top-left in pixels and scales only its size.
    const QRect screenRect(screen->geometry().topLeft(), screen->geometry().size() * screen->devicePixelRatio());
    Display *dpy = ownDisplay();
    if (!dpy)
        return screenRect;
    const std::vector<long> area = rootCardinals(dpy, "_NET_WORKAREA"); // x, y, w, h per desktop
    const std::vector<long> current = rootCardinals(dpy, "_NET_CURRENT_DESKTOP");
    const size_t at = current.empty() ? 0 : size_t(current[0]) * 4;
    if (area.size() < at + 4)
        return screenRect;
    qreal units = 1;
    const std::vector<long> desktop = rootCardinals(dpy, "_NET_DESKTOP_GEOMETRY");
    Window root;
    int x, y;
    unsigned int rootWidth, rootHeight, border, depth;
    if (desktop.size() >= 2 && desktop[0] > 0
        && XGetGeometry(dpy, DefaultRootWindow(dpy), &root, &x, &y, &rootWidth, &rootHeight, &border, &depth))
        units = qreal(rootWidth) / desktop[0];
    static int tracedUnits = 100;
    if (qRound(units * 100) != tracedUnits) {
        tracedUnits = qRound(units * 100);
        menufixTrace("window: work area reported in units of %d/100 px\n", tracedUnits);
    }
    const QRect work(qRound(area[at] * units), qRound(area[at + 1] * units), qRound(area[at + 2] * units),
                     qRound(area[at + 3] * units));
    const QRect fit = work & screenRect;
    return fit.isEmpty() ? screenRect : fit;
}

// Largest size of `w` (without its frame) that fits its screen's work area.
QSize workAreaSize(QWidget *w)
{
    QScreen *screen = w->screen();
    if (!screen)
        return {QWIDGETSIZE_MAX, QWIDGETSIZE_MAX};
    const QSize frame = w->isVisible() ? w->frameGeometry().size() - w->size() : QSize();
    return nativeWorkArea(screen).size() / w->devicePixelRatioF() - frame;
}

// Live 11.3.43: SetZoom 0x1c58910 (same arguments; zoom state at AWindow +
// 0xa0, zoom at + 0xdc, scale virtual + 0x5a8, minimum size the same + 0x200).
constexpr uintptr_t kSetZoom = 0x20c4110; // (window zoom state *, float zoom)
constexpr uintptr_t kSetZoom11 = 0x1c58910;
const unsigned char kSetZoomPrologue[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                          0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x68};
const unsigned char kSetZoomPrologue11[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                            0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x58};
using SetZoomFn = void (*)(void *, float);

// AWindow (a window's view manager): minimum size in Live's units, packed
// w | h << 32 (the maximum is the next slot); the window's scale (Live's
// units to pixels before zoom); the zoom at zoom state (+0xf0) + 0xd4.
constexpr size_t kMinimumSizeSlot = 0x200;
size_t kScaleSlot = 0x588;
size_t kZoomState = 0xf0;
size_t kZoom = 0xd4;
SetZoomFn liveSetZoom;
int zoomChanging; // depth of SetZoom calls on the GUI thread

void setZoom(void *state, float zoom)
{
    const bool gui = qApp && QThread::currentThread() == qApp->thread();
    if (gui)
        ++zoomChanging;
    liveSetZoom(state, zoom);
    if (gui)
        --zoomChanging;
}

uintptr_t mainBase()
{
    uintptr_t b = 0;
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *out) {
            *static_cast<uintptr_t *>(out) = info->dlpi_addr; // first entry = main program
            return 1;
        },
        &b);
    return b;
}

// Live 12.4.7 only; tests/splash-zoom.sh checks both addresses against the
// installed Live.
constexpr uintptr_t kCreateWindow = 0x20e4770;
const unsigned char kCreateWindowPrologue[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                               0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28};
constexpr uintptr_t kSplashCreateReturn = 0x21586e9;
CreateWindowFn liveCreateWindow;

// The recorded Zoom's file (splashzoom.cpp); empty: nothing is read or written.
const std::string &splashZoomPath()
{
    static const std::string path = splashZoomFile(getenv("HOME"), getenv("XDG_CONFIG_HOME"));
    return path;
}

void *createWindow(void *view, int a, void *parameters, int b, void *zoomSource, int c, ZoomRange range)
{
    static const uintptr_t splashReturn = mainBase() + kSplashCreateReturn;
    const bool splash = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) == splashReturn;
    int pinned = 0;
    void *window = createWithSplashZoom(liveCreateWindow, splash, splash ? splashZoomPath().c_str() : nullptr, view, a,
                                        parameters, b, zoomSource, c, range, &pinned);
    if (pinned)
        menufixTrace("window: splash zoom %d %%\n", pinned);
    return window;
}

// The main window whose Zoom is recorded: the first framed one painted (a
// second main window can have its own Zoom), replaced by the next one painted
// once it is closed or hidden, so a window shown only during startup cannot
// keep the place. recordedZoom is the last value read or written.
QPointer<QWidget> zoomRecordWindow;
int recordedZoom = -1;

void recordSplashZoom(QWidget *w, float zoom)
{
    if (liveBuild() != LiveBuild::Live12_4_7 || splashZoomPath().empty()
        || (w->windowFlags() & Qt::FramelessWindowHint))
        return;
    if (!zoomRecordWindow || !zoomRecordWindow->isVisible())
        zoomRecordWindow = w;
    if (zoomRecordWindow != w)
        return;
    const int percent = qRound(zoom * 100);
    if (percent < 50 || percent > 400 || percent == recordedZoom)
        return;
    if (recordedZoom < 0 && readZoomPercent(splashZoomPath().c_str()) == percent) {
        recordedZoom = percent; // already recorded by an earlier start
        return;
    }
    // Remembered even if the write fails, so a read-only folder is not retried every frame.
    recordedZoom = percent;
    const bool ok = writeZoomPercent(splashZoomPath().c_str(), percent);
    menufixTrace("window: recorded zoom %d %% from a %d px wide window (%d)\n", percent, w->width(), ok);
}

} // namespace

// Live calls QWidget::resize through its PLT with symbol version Qt_5; the
// version script (shim/qt5.map) gives this definition that version, so the
// preload wins the lookup. The real one is fetched with dlvsym.
extern "C" void _ZN7QWidget6resizeERK5QSize(QWidget *w, const QSize &size)
{
    using ResizeFn = void (*)(QWidget *, const QSize &);
    static auto real = reinterpret_cast<ResizeFn>(nextSymbol("_ZN7QWidget6resizeERK5QSize", "Qt_5"));
    if (!isLiveMainWindow(w))
        return real(w, size);
    if (scaleTransition())
        return; // Live's answer to a window the window manager scaled; scalechange.cpp restores it
    if (zoomChanging) {
        const QSize delta = size - w->size();
        if ((w->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))
            || (qAbs(delta.width()) <= 2 && qAbs(delta.height()) <= 2))
            return; // the zoom change's rounding error; keep the window as it is
    }
    // A maximized or full-screen window has the size the window manager gave it.
    const QSize fit = (w->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))
        ? size
        : size.boundedTo(workAreaSize(w));
    if (fit != size)
        menufixTrace("window: %dx%d limited to the work area\n", size.width(), size.height());
    real(w, fit);
}

// Called after Live has painted a frame of a window's view (from the paint
// handlers in nativemenu.cpp and glsurface.cpp), with the window's view
// manager (Live's AWindow).
void liveWindowPainted(void *viewManager)
{
    QWidget *w = paintingWindow;
    paintingWindow = nullptr;
    if (!w || !viewManager)
        return;
    auto *vtable = *reinterpret_cast<void ***>(viewManager);
    const uint64_t minimum = reinterpret_cast<uint64_t (*)(void *)>(vtable[kMinimumSizeSlot / 8])(viewManager);
    const float scale = reinterpret_cast<float (*)(void *)>(vtable[kScaleSlot / 8])(viewManager);
    const float zoom = *reinterpret_cast<float *>(*reinterpret_cast<char **>(static_cast<char *>(viewManager) + kZoomState) + kZoom);
    recordSplashZoom(w, zoom);
    // Live 11's menu bar is a QMainWindow menu bar above Live's view (Live 12's
    // view fills the window): the window's minimum includes it.
    QSize extra(0, 0); // a default QSize is (-1, -1)
    if (liveBuild() == LiveBuild::Live11_3_43)
        if (QWidget *view = static_cast<QMainWindow *>(w)->centralWidget())
            extra = (w->size() - view->size()).expandedTo(QSize(0, 0));
    const QSize size = (QSize(qRound(int32_t(minimum) * zoom * scale), qRound(int32_t(minimum >> 32) * zoom * scale)) + extra)
                           .boundedTo(workAreaSize(w));
    if (size.isValid() && size != w->minimumSize()) {
        menufixTrace("window: minimum size %dx%d\n", size.width(), size.height());
        w->setMinimumSize(size);
    }
}

// Title bars of Live's windows in `band` with `text` (see the top of this
// file). A failed write leaves the title bars as they are.
void applyTitleBarColours(const QColor &band, const QColor &text)
{
    static QColor lastBand, lastText;
    if (band == lastBand && text == lastText)
        return;
    Display *dpy = ownDisplay();
    if (!dpy)
        return;
    const QString path = chrome::writeColorScheme(
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Ableton/Window Colors", band, text);
    if (path.isEmpty())
        return;
    lastBand = band;
    lastText = text;
    schemePath = QFile::encodeName(path);
    for (QWidget *w : QApplication::topLevelWidgets())
        if (decorated(w) && w->isVisible())
            setColorScheme(w);
    XFlush(dpy);
    menufixTrace("window: title bar colours %06x\n", int(band.rgb() & 0xffffff));
}

// An editor shown again while the window it belongs to is minimised. A dock
// or task bar click on minimised Live is passed on to the editor (its
// transient), and the window manager restores that window alone, leaving
// Live's main window minimised. Mapping the main window returns it from
// Iconic to Normal state (ICCCM 4.1.4); the window manager then restores it,
// with its editors.
// Keys typed in a plug-in editor window go to Live, as with Live's editor
// windows on Windows and Mac, unless the plug-in has taken the keyboard
// itself (a text box: its own window holds the X focus, which is left alone).
// The editor is a plain X window on Live's editor connection, where nothing
// takes keys, so a key would be lost there or swallowed by a plug-in window
// under the pointer. Each editor gets a 1x1 InputOnly child (the key catcher);
// focus given to the editor top-level moves to it, so keys arrive at the
// catcher whatever lies under the pointer. Each key seen there (or sent back
// to the editor by a plug-in bridge) is sent on to Live's main window, which
// Qt is told is active, so Live's window-wide shortcuts fire too. Window ids
// only; GUI thread only.
namespace {
struct EditorKeys {
    Display *dpy;
    Window catcher;
    Window owner;
    QHash<unsigned, XKeyEvent> down; // keys passed on to Live as pressed, by keycode
};
QHash<Window, EditorKeys> editorKeys;

bool onGuiThread() { return qApp && QThread::currentThread() == qApp->thread(); }

// The tracked editor `w` is, or whose catcher it is; 0 if neither.
Window keysEditor(Window w)
{
    if (editorKeys.contains(w))
        return w;
    for (auto it = editorKeys.cbegin(); it != editorKeys.cend(); ++it)
        if (it->catcher == w)
            return it.key();
    return 0;
}

// Whether the X focus window `w` is one of Live's Qt windows or lies inside a
// tracked editor (the catcher, a plug-in window).
bool focusInsideLive(Display *dpy, Window w)
{
    for (QWindow *q : QGuiApplication::allWindows())
        if (q->handle() && q->winId() == w)
            return true;
    for (int depth = 0; w && w != PointerRoot && depth < 16; ++depth) {
        if (editorKeys.contains(w))
            return true;
        Window root, parent, *kids = nullptr;
        unsigned count = 0;
        if (!XQueryTree(dpy, w, &root, &parent, &kids, &count))
            return false;
        if (kids)
            XFree(kids);
        if (parent == root)
            return false;
        w = parent;
    }
    return false;
}
// Whether a plug-in bridge has embedded a client window in the editor (a child
// of the editor whose own child carries WM_STATE, as yabridge embeds Wine's
// window). Such a bridge gives its plug-in the keyboard by focusing the editor
// window while the pointer is over the plug-in, so that focus stays put.
bool hostsEmbeddedClient(Display *dpy, Window editor, Window catcher)
{
    const Atom wmState = XInternAtom(dpy, "WM_STATE", False);
    Window root, parent, *kids = nullptr;
    unsigned count = 0;
    if (!XQueryTree(dpy, editor, &root, &parent, &kids, &count))
        return false;
    bool found = false;
    for (unsigned i = 0; i < count && !found; ++i) {
        if (kids[i] == catcher)
            continue;
        Window *inner = nullptr;
        unsigned innerCount = 0;
        if (!XQueryTree(dpy, kids[i], &root, &parent, &inner, &innerCount))
            continue;
        for (unsigned j = 0; j < innerCount && !found; ++j) {
            int propCount = 0;
            Atom *props = XListProperties(dpy, inner[j], &propCount);
            for (int k = 0; k < propCount; ++k)
                found = found || props[k] == wmState;
            if (props)
                XFree(props);
        }
        if (inner)
            XFree(inner);
    }
    if (kids)
        XFree(kids);
    return found;
}

// Live gets a release for every key it was given as pressed and has not seen
// released: the release went elsewhere once the catcher lost the focus.
void releaseHeld(EditorKeys &keys)
{
    for (XKeyEvent key : std::as_const(keys.down)) {
        key.type = KeyRelease;
        XSendEvent(keys.dpy, keys.owner, False, KeyReleaseMask, reinterpret_cast<XEvent *>(&key));
    }
    if (!keys.down.isEmpty())
        XFlush(keys.dpy);
    keys.down.clear();
}
} // namespace

void editorKeysTrack(Display *dpy, Window editor, Window owner)
{
    XWindowAttributes a;
    if (!onGuiThread() || editorKeys.contains(editor) || !XGetWindowAttributes(dpy, editor, &a))
        return;
    XSelectInput(dpy, editor, a.your_event_mask | KeyPressMask | KeyReleaseMask | FocusChangeMask);
    // The real calls: the shim's XCreateWindow and XMapWindow are for editors.
    static auto createWindow = reinterpret_cast<decltype(&XCreateWindow)>(nextSymbol("XCreateWindow", nullptr));
    static auto mapWindow = reinterpret_cast<int (*)(Display *, Window)>(nextSymbol("XMapWindow", nullptr));
    if (!createWindow || !mapWindow)
        return;
    XSetWindowAttributes attrs = {};
    attrs.event_mask = KeyPressMask | KeyReleaseMask | FocusChangeMask;
    const Window catcher = createWindow(dpy, editor, -1, -1, 1, 1, 0, 0, InputOnly, CopyFromParent, CWEventMask, &attrs);
    if (!catcher)
        return;
    mapWindow(dpy, catcher);
    editorKeys.insert(editor, {dpy, catcher, owner, {}});
}

void editorKeysForget(Window editor)
{
    if (!onGuiThread())
        return;
    auto it = editorKeys.find(editor);
    if (it == editorKeys.end())
        return;
    releaseHeld(*it);
    editorKeys.erase(it); // the catcher goes with its parent
}

void editorKeysEvent(Display *dpy, const XEvent &ev)
{
    if (!onGuiThread() || editorKeys.isEmpty())
        return;
    if (ev.type == FocusIn && ev.xfocus.window == ev.xany.window) {
        auto it = editorKeys.constFind(ev.xfocus.window);
        if (it == editorKeys.cend())
            return;
        Window focus = 0;
        int revert = 0;
        XGetInputFocus(dpy, &focus, &revert);
        // The top-level itself, not a plug-in window inside it, and no bridge
        // that keys its plug-in through this focus.
        if (focus == ev.xfocus.window && !hostsEmbeddedClient(dpy, ev.xfocus.window, it->catcher))
            XSetInputFocus(dpy, it->catcher, RevertToParent, CurrentTime);
        return;
    }
    if (ev.type == FocusOut) {
        const Window editor = keysEditor(ev.xfocus.window);
        if (editor && editor != ev.xfocus.window) // the catcher lost the focus
            QTimer::singleShot(0, qApp, [editor] {
                auto it = editorKeys.find(editor);
                if (it != editorKeys.end())
                    releaseHeld(*it);
            });
    }
    if (ev.type == FocusOut && ev.xfocus.detail != NotifyInferior && ev.xfocus.mode == NotifyNormal
        && editorKeys.contains(ev.xfocus.window)) {
        // Focus left the editor: if it went outside Live, Live is no longer
        // active (it may have been made so for a key typed in the editor).
        QTimer::singleShot(0, qApp, [dpy] {
            Window focus = 0;
            int revert = 0;
            XGetInputFocus(dpy, &focus, &revert);
            if (QApplication::activeWindow() && !focusInsideLive(dpy, focus))
                QApplication::setActiveWindow(nullptr);
        });
        return;
    }
    if (ev.type != KeyPress && ev.type != KeyRelease)
        return;
    const Window editor = keysEditor(ev.xkey.window);
    if (!editor)
        return;
    // Out of Live's event pump, in order (equal delays run in order).
    XEvent copy = ev;
    QTimer::singleShot(0, qApp, [dpy, editor, copy]() mutable {
        auto it = editorKeys.find(editor);
        if (it == editorKeys.end())
            return;
        const Window owner = it->owner;
        if (QWidget *main = QWidget::find(WId(owner)))
            if (QApplication::activeWindow() != main->window())
                QApplication::setActiveWindow(main->window());
        copy.xkey.window = owner;
        copy.xkey.subwindow = None;
        if (copy.type == KeyPress)
            it->down.insert(copy.xkey.keycode, copy.xkey);
        else
            it->down.remove(copy.xkey.keycode);
        XSendEvent(dpy, owner, False, copy.type == KeyPress ? KeyPressMask : KeyReleaseMask, &copy);
        XFlush(dpy);
    });
}

void editorShown(Display *dpy, Window editor)
{
    if (!qApp || QThread::currentThread() != qApp->thread() || !editorTracked(editor))
        return;
    Window owner = 0;
    if (!XGetTransientForHint(dpy, editor, &owner) || !owner)
        return;
    constexpr long kIconicState = 3;
    const Atom wmState = XInternAtom(dpy, "WM_STATE", False);
    Atom type;
    int format;
    unsigned long items, rest;
    unsigned char *data = nullptr;
    const bool iconic = XGetWindowProperty(dpy, owner, wmState, 0, 2, False, wmState, &type, &format, &items, &rest,
                                           &data)
                            == Success
        && data && format == 32 && items >= 1 && reinterpret_cast<long *>(data)[0] == kIconicState;
    if (data)
        XFree(data);
    if (!iconic)
        return;
    // A MapNotify handled late (Live busy) after the window manager has hidden
    // the editor again with the main window: leave both minimised.
    XWindowAttributes now;
    if (!XGetWindowAttributes(dpy, editor, &now) || now.map_state == IsUnmapped)
        return;
    // The real call: the shim's XMapWindow is for editors and device views.
    static auto mapWindow = reinterpret_cast<int (*)(Display *, Window)>(nextSymbol("XMapWindow", nullptr));
    if (mapWindow) {
        mapWindow(dpy, owner);
        XFlush(dpy);
        menufixTrace("window: editor 0x%x shown on minimised 0x%x, main window restored\n", int(editor), int(owner));
    }
}

void liveOwnPluginWindow(Display *dpy, Window editor)
{
    if (!qApp || QThread::currentThread() != qApp->thread())
        return;
    QWidget *owner = QApplication::activeWindow();
    if (!isLiveMainWindow(owner)) {
        owner = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (isLiveMainWindow(w) && w->isVisible()) {
                owner = w;
                break;
            }
    }
    // Own instance name, so a window rule can match editors only; same class
    // for grouping.
    XClassHint hint = {const_cast<char *>("ableton-live-plugin"),
                        const_cast<char *>(liveBuild() == LiveBuild::Live11_3_43 ? "Ableton Live 11" : "Ableton Live 12")};
    XSetClassHint(dpy, editor, &hint);
    setNoMinimize(dpy, editor);
    editorColourTrack(editor);
    // No icon in the title bar: one transparent pixel. With no icon at all the
    // window manager would show Live's, found through the class.
    const long blankIcon[] = {1, 1, 0};
    XChangeProperty(dpy, editor, XInternAtom(dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(blankIcon), 3);
    if (!owner)
        return;
    const Window ownerId = owner->winId();
    XSetTransientForHint(dpy, editor, ownerId);
    editorKeysTrack(dpy, editor, ownerId);
    menufixTrace("window: plug-in editor 0x%x owned by 0x%x\n", int(editor), int(ownerId));
}

namespace {

// Diagnostics for Qt warnings about Live's windows ("QWidget::setMinimumSize:
// ... Negative sizes"): with MENUFIX_DEBUG set, each one is written there with
// the call stack as offsets into AbletonLive12.
QtMessageHandler previousHandler;

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const char *path = getenv("MENUFIX_DEBUG");
    if (path && type == QtWarningMsg && message.startsWith("QWidget::")) {
        if (FILE *f = fopen(path, "a")) {
            uintptr_t base = 0;
            dl_iterate_phdr([](dl_phdr_info *i, size_t, void *o) { *static_cast<uintptr_t *>(o) = i->dlpi_addr; return 1; },
                            &base);
            void *frames[24];
            const int n = backtrace(frames, 24);
            fprintf(f, "qt: %s\n  stack:", message.toLocal8Bit().constData());
            for (int i = 1; i < n; ++i) {
                Dl_info info;
                if (dladdr(frames[i], &info) && info.dli_fname && !strstr(info.dli_fname, "Live"))
                    fprintf(f, " %s+%lx", strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname,
                            uintptr_t(frames[i]) - uintptr_t(info.dli_fbase));
                else
                    fprintf(f, " Live+%lx", uintptr_t(frames[i]) - base);
            }
            fprintf(f, "\n");
            fclose(f);
        }
    }
    if (previousHandler)
        previousHandler(type, context, message);
    else
        fprintf(stderr, "%s\n", message.toLocal8Bit().constData());
}

} // namespace

void installWindowIntegration()
{
    previousHandler = qInstallMessageHandler(messageHandler);
    // WM_CLASS: Qt takes the instance from RESOURCE_NAME and the class from the
    // application name; both are read when the first window is created.
    // Live 11 gets its own instance name (its desktop entry's StartupWMClass), so
    // the desktop does not group its windows under Live 12's entry and icon.
    if (!qEnvironmentVariableIsSet("RESOURCE_NAME"))
        qputenv("RESOURCE_NAME", liveBuild() == LiveBuild::Live11_3_43 ? "ableton-live-11" : "ableton-live");
    QCoreApplication::setApplicationName(liveBuild() == LiveBuild::Live11_3_43 ? "Ableton Live 11" : "Ableton Live 12");
    setAppIcon();
    qApp->installEventFilter(new WindowFilter);
    if (liveBuild() == LiveBuild::Live12_4_7) {
        liveSetZoom = reinterpret_cast<SetZoomFn>(hookFunction(mainBase() + kSetZoom, kSetZoomPrologue,
                                                           sizeof kSetZoomPrologue, reinterpret_cast<void *>(&setZoom)));
        liveCreateWindow = reinterpret_cast<CreateWindowFn>(hookFunction(mainBase() + kCreateWindow, kCreateWindowPrologue,
                                                                     sizeof kCreateWindowPrologue,
                                                                     reinterpret_cast<void *>(&createWindow)));
        menufixTrace("window: create hook %d\n", liveCreateWindow != nullptr);
    }
    if (liveBuild() == LiveBuild::Live11_3_43) {
        kScaleSlot = 0x5a8;
        kZoomState = 0xa0;
        kZoom = 0xdc;
        liveSetZoom = reinterpret_cast<SetZoomFn>(hookFunction(mainBase() + kSetZoom11, kSetZoomPrologue11,
                                                           sizeof kSetZoomPrologue11, reinterpret_cast<void *>(&setZoom)));
    }
    menufixTrace("window: zoom hook %d\n", liveSetZoom != nullptr);
}
