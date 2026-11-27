// A plug-in editor shown again while Live's main window is minimised brings
// the main window back (shim/window.cpp editorShown, called from the
// XNextEvent interposer in shim/pluginclose.cpp). A dock click on minimised
// Live is passed on to the editor, its transient, and the window manager
// restores only that window. Links shim/pluginclose.cpp, so the MapNotify goes
// through the shim's XNextEvent as Live's does. No window manager runs here:
// "minimised" is the main window unmapped with WM_STATE IconicState, as a
// window manager leaves it, and the shim's answer is a map of that window.
#include "editorcolour.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QMainWindow>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
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

static Display *dpy;
static Atom wmState;

static bool mapped(Window w)
{
    XWindowAttributes a;
    return XGetWindowAttributes(dpy, w, &a) && a.map_state != IsUnmapped;
}

// The main window as a window manager leaves it: unmapped, WM_STATE `state`.
static void leaveMain(Window main, long state)
{
    XUnmapWindow(dpy, main);
    const long value[2] = {state, 0};
    XChangeProperty(dpy, main, wmState, wmState, 32, PropModeReplace, reinterpret_cast<const unsigned char *>(value), 2);
    XSync(dpy, False);
}

// Maps `w` and runs the shim's XNextEvent until its MapNotify has gone
// through, then lets any request the shim made reach the server.
static void showAndPump(Window w)
{
    XSelectInput(dpy, w, StructureNotifyMask);
    XMapWindow(dpy, w);
    QElapsedTimer t;
    t.start();
    bool seen = false;
    while (!seen && t.elapsed() < 2000) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            seen = seen || (ev.type == MapNotify && ev.xmap.window == w);
        }
        QCoreApplication::processEvents();
    }
    XSync(dpy, False);
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
    XSync(dpy, False);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QMainWindow live;
    live.resize(600, 400);
    live.show();
    live.activateWindow();
    for (int i = 0; i < 20; ++i)
        app.processEvents();
    const Window main = live.winId();

    dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::printf("FAIL: no X display\n");
        return 1;
    }
    wmState = XInternAtom(dpy, "WM_STATE", False);
    constexpr long kNormal = 1, kIconic = 3;

    // Case 1: an editor shown while the main window is minimised.
    Window editor = XCreateWindow(dpy, DefaultRootWindow(dpy), 10, 10, 200, 100, 0, 0, 0, nullptr, 0, nullptr);
    liveOwnPluginWindow(dpy, editor);
    XSync(dpy, False);
    Window owner = 0;
    check(XGetTransientForHint(dpy, editor, &owner) && owner == main, "editor is transient for Live's main window");
    leaveMain(main, kIconic);
    check(!mapped(main), "set-up: main window minimised (unmapped, Iconic)");
    showAndPump(editor);
    check(mapped(editor), "editor shown");
    check(mapped(main), "main window shown again with the editor");

    // Case 2: the main window not minimised (Normal): left as it is.
    XUnmapWindow(dpy, editor);
    XSync(dpy, False);
    leaveMain(main, kNormal);
    showAndPump(editor);
    check(!mapped(main), "main window in Normal state is not mapped by the shim");

    // Case 3: a window the shim does not track as an editor, transient for a
    // minimised main window: left as it is.
    Window other = XCreateWindow(dpy, DefaultRootWindow(dpy), 10, 10, 200, 100, 0, 0, 0, nullptr, 0, nullptr);
    XSetTransientForHint(dpy, other, main);
    leaveMain(main, kIconic);
    showAndPump(other);
    check(!mapped(main), "an untracked window shown does not map the main window");

    XCloseDisplay(dpy);
    std::printf(failures ? "FAIL: %d check(s) failed\n" : "PASS: an editor shown on minimised Live brings Live back\n",
                failures);
    return failures ? 1 : 0;
}
