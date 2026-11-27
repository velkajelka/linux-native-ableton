// Focus cases of the editor key routing (shim/window.cpp):
//   1. A bridged plug-in (an embedded client window: a child of the editor
//      whose own child carries WM_STATE, as yabridge embeds Wine's window)
//      is given the keyboard by focusing the editor window while the pointer
//      is over the plug-in. The shim leaves that focus alone, so keys reach
//      the plug-in, not Live; with the pointer outside, they reach Live.
//   2. A key forwarded to Live as pressed, then released after a plug-in
//      window took the focus: Live still gets the release.
// Same set-up as tests/plugin-keys.sh: editor made on Live's connection
// through liveOwnPluginWindow, plug-in windows on their own connection, keys
// pressed with XTest, Live's connection pumped through the shim's XNextEvent.
#include "editorcolour.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMainWindow>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <string>

void liveOwnPluginWindow(Display *dpy, Window editor); // window.cpp

// Stand-ins for the parts of the shim window.cpp and pluginclose.cpp link against.
void menufixTrace(const char *, int, int, int) {}
void *nextSymbol(const char *name, const char *) { return dlsym(RTLD_NEXT, name); }
void *hookFunction(uintptr_t, const unsigned char *, size_t, void *) { return nullptr; }
bool scaleTransition() { return false; }
bool maxViewMoveResize(Display *, Window, int &, int &, unsigned &, unsigned &, void *) { return false; }
void maxViewMapped(Display *, Window, void *) {}
void maxViewUnmapped(Window, void *) {}
void maxViewForget(Window) {}

static int failures = 0;
static void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "  ok  " : "FAIL:", what.c_str());
    if (!ok)
        ++failures;
}

// Live's view: counts presses and releases of Space and Z.
class LiveView : public QWidget {
public:
    int spaces = 0, zDown = 0, zUp = 0;

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Space)
            ++spaces;
        if (e->key() == Qt::Key_Z && !e->isAutoRepeat())
            ++zDown;
    }
    void keyReleaseEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Z && !e->isAutoRepeat())
            ++zUp;
    }
};

static Display *live, *plugin;
static int pluginSpaces = 0;

static void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        while (XPending(live)) {
            XEvent ev;
            XNextEvent(live, &ev);
        }
        while (XPending(plugin)) {
            XEvent ev;
            XNextEvent(plugin, &ev);
            if (ev.type == KeyPress && XLookupKeysym(&ev.xkey, 0) == XK_space)
                ++pluginSpaces;
        }
        QCoreApplication::processEvents();
        XSync(live, False);
    }
}

static void point(int x, int y)
{
    XWarpPointer(live, None, DefaultRootWindow(live), 0, 0, 0, 0, x, y);
    XSync(live, False);
    pump(100);
}

static void key(KeySym sym, bool down)
{
    XTestFakeKeyEvent(live, XKeysymToKeycode(live, sym), down ? True : False, CurrentTime);
    XSync(live, False);
    pump(250);
}

static Window focusNow()
{
    Window w = 0;
    int revert = 0;
    XGetInputFocus(live, &w, &revert);
    return w;
}

static void waitMapped(Display *dpy, Window w)
{
    QElapsedTimer t;
    t.start();
    XWindowAttributes a;
    while (t.elapsed() < 2000 && XGetWindowAttributes(dpy, w, &a) && a.map_state != IsViewable)
        pump(20);
}

static Window makeEditor(int x, int y)
{
    const Window editor = XCreateWindow(live, DefaultRootWindow(live), x, y, 400, 300, 0, CopyFromParent,
                                        InputOutput, CopyFromParent, 0, nullptr);
    liveOwnPluginWindow(live, editor);
    XMapWindow(live, editor);
    XSync(live, False);
    waitMapped(live, editor);
    return editor;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QMainWindow main;
    auto *view = new LiveView;
    view->setFocusPolicy(Qt::StrongFocus);
    main.setCentralWidget(view);
    main.setGeometry(700, 100, 400, 300);
    main.show();
    view->setFocus();
    live = XOpenDisplay(nullptr);
    plugin = XOpenDisplay(nullptr);
    if (!live || !plugin) {
        std::printf("FAIL: no X display\n");
        return 1;
    }
    waitMapped(live, main.winId());

    // Case 1: a bridged plug-in. The bridge's wrapper fills the editor and takes keys;
    // inside it the embedded client window, with WM_STATE Normal.
    const Window bridged = makeEditor(100, 100);
    const Window wrapper = XCreateSimpleWindow(plugin, bridged, 0, 0, 400, 300, 0, 0, 0);
    XSelectInput(plugin, wrapper, KeyPressMask | KeyReleaseMask);
    const Window client = XCreateSimpleWindow(plugin, wrapper, 0, 0, 400, 300, 0, 0, 0);
    const Atom wmState = XInternAtom(plugin, "WM_STATE", False);
    const long normal[2] = {1, 0};
    XChangeProperty(plugin, client, wmState, wmState, 32, PropModeReplace, reinterpret_cast<const unsigned char *>(normal), 2);
    XMapWindow(plugin, client);
    XMapWindow(plugin, wrapper);
    XSync(plugin, False);
    waitMapped(plugin, client);

    // Pointer over the plug-in; the bridge focuses the editor window.
    point(300, 250);
    XSetInputFocus(plugin, bridged, RevertToParent, CurrentTime);
    XSync(plugin, False);
    pump(200);
    check(focusNow() == bridged, "bridged plug-in: the focus it gave the editor window is left there");
    int before = view->spaces, pluginBefore = pluginSpaces;
    key(XK_space, true);
    key(XK_space, false);
    check(pluginSpaces == pluginBefore + 1, "bridged plug-in, pointer over it: Space reaches the plug-in");
    check(view->spaces == before, "bridged plug-in, pointer over it: Space does not reach Live");

    // Pointer moved outside the plug-in; the editor window keeps the focus.
    point(50, 600);
    before = view->spaces;
    key(XK_space, true);
    key(XK_space, false);
    check(view->spaces == before + 1, "bridged plug-in, pointer outside it: Space reaches Live");
    XDestroyWindow(live, bridged);
    XSync(live, False);
    pump(100);

    // Case 2: a native editor, its view a plug-in window that can take the focus.
    const Window native = makeEditor(100, 100);
    const Window pluginView = XCreateSimpleWindow(plugin, native, 0, 0, 400, 300, 0, 0, 0);
    XSelectInput(plugin, pluginView, KeyPressMask | KeyReleaseMask);
    XMapWindow(plugin, pluginView);
    XSync(plugin, False);
    waitMapped(plugin, pluginView);
    point(300, 250);
    XSetInputFocus(live, native, RevertToParent, CurrentTime); // the window manager, on a click
    XSync(live, False);
    pump(200);
    key(XK_z, true); // a computer MIDI keyboard note held
    check(view->zDown == 1 && view->zUp == 0, "native plug-in: Z pressed in the editor reaches Live as pressed");
    XSetInputFocus(plugin, pluginView, RevertToParent, CurrentTime); // the plug-in takes the focus
    XSync(plugin, False);
    pump(200);
    key(XK_z, false);
    check(view->zDown == 1 && view->zUp == 1,
          "native plug-in took the focus while Z was held: Live gets the release (down " + std::to_string(view->zDown)
              + ", up " + std::to_string(view->zUp) + ")");

    XCloseDisplay(plugin);
    XCloseDisplay(live);
    std::printf(failures ? "FAIL: %d check(s) failed\n" : "PASS: the key routing keeps focus where it belongs and no key held\n",
                failures);
    return failures ? 1 : 0;
}
