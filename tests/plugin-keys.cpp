// Keys pressed while a plug-in editor window has the keyboard reach Live's
// main window unless the plug-in itself took the keyboard (a text box). Live's
// editor windows live on Live's own Xlib connection; this test makes one there
// the way Live does (a top-level window owned through liveOwnPluginWindow),
// gives it the X input focus as the window manager does when it is clicked,
// presses Space with XTest and pumps that connection through the shim's
// XNextEvent, as Live's own event pump does, and the Qt loop. Live's main
// window stands in as a QMainWindow whose focus widget counts Space presses.
// No window manager runs: focus and pointer are set explicitly per case.
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

// Live's view: counts the Space presses Qt delivers to it.
class LiveView : public QWidget {
public:
    int spaces = 0;

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Space)
            ++spaces;
        QWidget::keyPressEvent(e);
    }
};

static Display *live;   // Live's connection for plug-in editors
static Display *plugin; // the plug-in's own connection (its view, a text box)
static int pluginSpaces = 0; // Space presses the plug-in's windows received

// Pumps both connections (Live's through the shim's XNextEvent) and Qt for `ms`.
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

// The window manager's part: focus to `w` (a click on it), pointer at (x, y).
static void focusAndPoint(Display *dpy, Window w, int x, int y)
{
    XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0, 0, 0, 0, x, y);
    XSetInputFocus(dpy, w, RevertToParent, CurrentTime);
    XSync(dpy, False);
    pump(150);
}

static void pressSpace(Display *dpy)
{
    const KeyCode space = XKeysymToKeycode(dpy, XK_space);
    XTestFakeKeyEvent(dpy, space, True, CurrentTime);
    XTestFakeKeyEvent(dpy, space, False, CurrentTime);
    XSync(dpy, False);
    pump(300);
}

static void waitMapped(Display *dpy, Window w)
{
    QElapsedTimer t;
    t.start();
    XWindowAttributes a;
    while (t.elapsed() < 2000 && XGetWindowAttributes(dpy, w, &a) && a.map_state != IsViewable)
        pump(20);
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

    // Control: with Live's main window focused, Space reaches Live's view.
    focusAndPoint(live, main.winId(), 800, 200);
    pressSpace(live);
    check(view->spaces == 1, "control: Space in Live's main window reaches Live (" + std::to_string(view->spaces) + ")");

    // The editor window, made on Live's connection as Live makes it, 400x300 at (100, 100),
    // with the plug-in's view as a child on the plug-in's connection over its left half.
    const Window editor = XCreateWindow(live, DefaultRootWindow(live), 100, 100, 400, 300, 0, CopyFromParent,
                                        InputOutput, CopyFromParent, 0, nullptr);
    liveOwnPluginWindow(live, editor);
    XMapWindow(live, editor);
    XSync(live, False);
    waitMapped(live, editor);
    const Window pluginView = XCreateSimpleWindow(plugin, editor, 0, 0, 200, 300, 0, 0, 0);
    XSelectInput(plugin, pluginView, ExposureMask);
    XMapWindow(plugin, pluginView);
    XSync(plugin, False);
    waitMapped(plugin, pluginView);

    // Case 1: the editor focused, pointer over a part of it with no plug-in window.
    int before = view->spaces;
    focusAndPoint(live, editor, 450, 250);
    pressSpace(live);
    check(view->spaces == before + 1, "editor focused, pointer on the editor: Space reaches Live");

    // Case 2: the editor focused, pointer over the plug-in's view, which takes no keys.
    before = view->spaces;
    focusAndPoint(live, editor, 150, 250);
    pressSpace(live);
    check(view->spaces == before + 1, "editor focused, pointer on a plug-in view that takes no keys: Space reaches Live");

    // Case 3: the plug-in's view takes keys, the editor focused, pointer over the view
    // (a native plug-in under the pointer that does not use Space).
    XSelectInput(plugin, pluginView, ExposureMask | KeyPressMask | KeyReleaseMask);
    XSync(plugin, False);
    before = view->spaces;
    focusAndPoint(live, editor, 150, 250);
    pressSpace(live);
    check(view->spaces == before + 1, "editor focused, pointer on a plug-in view that selects keys: Space reaches Live");

    // Case 4: the plug-in took the keyboard itself (a text box): the plug-in gets Space, Live does not.
    before = view->spaces;
    const int pluginBefore = pluginSpaces;
    focusAndPoint(plugin, pluginView, 150, 250);
    pressSpace(live);
    check(pluginSpaces == pluginBefore + 1, "plug-in focused its own window: Space reaches the plug-in");
    check(view->spaces == before, "plug-in focused its own window: Space does not reach Live");

    // Case 5: a key the plug-in did not use, handed back to the editor window as a
    // synthetic KeyPress/KeyRelease (as a bridge does for a key its plug-in left unused).
    focusAndPoint(plugin, pluginView, 150, 250);
    before = view->spaces;
    XKeyEvent key = {};
    key.type = KeyPress;
    key.display = plugin;
    key.window = editor;
    key.root = DefaultRootWindow(plugin);
    key.subwindow = None;
    key.time = CurrentTime;
    key.x = key.y = 10;
    key.x_root = key.y_root = 110;
    key.same_screen = True;
    key.keycode = XKeysymToKeycode(plugin, XK_space);
    XSendEvent(plugin, editor, False, KeyPressMask, reinterpret_cast<XEvent *>(&key));
    key.type = KeyRelease;
    XSendEvent(plugin, editor, False, KeyReleaseMask, reinterpret_cast<XEvent *>(&key));
    XSync(plugin, False);
    pump(300);
    check(view->spaces == before + 1, "a key handed back to the editor window by the plug-in reaches Live");

    XCloseDisplay(plugin);
    XCloseDisplay(live);
    std::printf(failures ? "FAIL: %d check(s) failed\n" : "PASS: keys the plug-in does not use reach Live\n", failures);
    return failures ? 1 : 0;
}
