// Keys typed in a focused plug-in editor window also fire Live's window-wide
// shortcuts: a QShortcut and a QAction on Live's main window (window context,
// Qt's default), as well as plain key presses. Qt fires window-context
// shortcuts only for its active window, and an editor window is not a Qt
// window, so this checks what tests/plugin-keys.sh does not. Same set-up: the
// editor made on Live's connection through liveOwnPluginWindow, focus given
// to it as the window manager does on a click, keys pressed with XTest, Live's
// connection pumped through the shim's XNextEvent. No window manager runs.
#include "editorcolour.h"
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QShortcut>
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

static Display *live; // Live's connection for plug-in editors

static void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        while (XPending(live)) {
            XEvent ev;
            XNextEvent(live, &ev);
        }
        QCoreApplication::processEvents();
        XSync(live, False);
    }
}

static void focus(Window w)
{
    XSetInputFocus(live, w, RevertToParent, CurrentTime);
    XSync(live, False);
    pump(150);
}

static void press(KeySym sym)
{
    const KeyCode code = XKeysymToKeycode(live, sym);
    XTestFakeKeyEvent(live, code, True, CurrentTime);
    XTestFakeKeyEvent(live, code, False, CurrentTime);
    XSync(live, False);
    pump(300);
}

static void waitMapped(Window w)
{
    QElapsedTimer t;
    t.start();
    XWindowAttributes a;
    while (t.elapsed() < 2000 && XGetWindowAttributes(live, w, &a) && a.map_state != IsViewable)
        pump(20);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QMainWindow main;
    auto *centre = new QWidget;
    centre->setFocusPolicy(Qt::StrongFocus);
    main.setCentralWidget(centre);
    main.setGeometry(700, 100, 400, 300);
    int shortcutFired = 0, actionFired = 0;
    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), &main); // Qt::WindowShortcut
    QObject::connect(space, &QShortcut::activated, [&] { ++shortcutFired; });
    auto *action = new QAction(&main);
    action->setShortcut(QKeySequence(Qt::Key_M));
    main.addAction(action);
    QObject::connect(action, &QAction::triggered, [&] { ++actionFired; });
    main.show();
    centre->setFocus();
    live = XOpenDisplay(nullptr);
    if (!live) {
        std::printf("FAIL: no X display\n");
        return 1;
    }
    waitMapped(main.winId());

    // Control: with Live's main window focused, both fire.
    focus(main.winId());
    press(XK_space);
    press(XK_m);
    check(shortcutFired == 1 && actionFired == 1, "control: shortcuts fire with Live's main window focused");

    const Window editor = XCreateWindow(live, DefaultRootWindow(live), 100, 100, 400, 300, 0, CopyFromParent,
                                        InputOutput, CopyFromParent, 0, nullptr);
    liveOwnPluginWindow(live, editor);
    XMapWindow(live, editor);
    XSync(live, False);
    waitMapped(editor);

    focus(editor);
    press(XK_space);
    check(shortcutFired == 2, "editor focused: Space fires Live's window-wide QShortcut");
    press(XK_m);
    check(actionFired == 2, "editor focused: M fires Live's window-wide QAction");
    Window focused = 0;
    int revert = 0;
    XGetInputFocus(live, &focused, &revert);
    check(focused != main.winId(), "the X focus stays with the editor window");

    // A plug-in focuses its own text box while the focus is outside the editor
    // (in Live's main window): the focus stays in the text box.
    Display *plugin = XOpenDisplay(nullptr);
    const Window textBox = XCreateSimpleWindow(plugin, editor, 10, 10, 100, 20, 0, 0, 0);
    XSelectInput(plugin, textBox, KeyPressMask | KeyReleaseMask);
    XMapWindow(plugin, textBox);
    XSync(plugin, False);
    pump(150);
    focus(main.winId());
    XSetInputFocus(plugin, textBox, RevertToParent, CurrentTime);
    XSync(plugin, False);
    pump(300);
    XGetInputFocus(live, &focused, &revert);
    check(focused == textBox, "a plug-in focusing its own text box from outside the editor keeps the focus");
    XCloseDisplay(plugin);

    // The user then clicks another program's window: Live is no longer the active program.
    Display *other = XOpenDisplay(nullptr);
    const Window elsewhere = XCreateSimpleWindow(other, DefaultRootWindow(other), 0, 500, 200, 100, 0, 0, 0);
    XMapWindow(other, elsewhere);
    XSync(other, False);
    pump(150);
    XSetInputFocus(other, elsewhere, RevertToParent, CurrentTime);
    XSync(other, False);
    pump(300);
    check(QApplication::activeWindow() == nullptr, "focus moved to another program: Live is not left active");
    XCloseDisplay(other);

    XCloseDisplay(live);
    std::printf(failures ? "FAIL: %d check(s) failed\n"
                         : "PASS: keys typed in a plug-in window fire Live's window-wide shortcuts\n",
                failures);
    return failures ? 1 : 0;
}
