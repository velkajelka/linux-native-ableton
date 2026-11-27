// The restore in shim/window.cpp editorShown answers the window manager's map of
// an editor, seen through the shim's XNextEvent (shim/pluginclose.cpp), and
// only while that editor is still shown. Editors here are mapped and unmapped
// through the real Xlib calls, not the shim's interposers, as KWin's restore
// and minimise never pass through Live's XMapWindow. Every case fails unless
// the editor's own MapNotify went through the shim's XNextEvent.
//   1. Editor shown on a minimised main window: the main window is mapped.
//   2. A MapNotify drained only after the editor was hidden again (Live busy
//      while the user minimised): the main window stays minimised.
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
using MapFn = int (*)(Display *, Window);
static MapFn realMap, realUnmap;

static bool mapped(Window w)
{
    XWindowAttributes a;
    return XGetWindowAttributes(dpy, w, &a) && a.map_state != IsUnmapped;
}

// The main window as a window manager leaves it on minimise: unmapped, Iconic.
static void minimiseMain(Window main)
{
    realUnmap(dpy, main);
    const long value[2] = {3, 0};
    XChangeProperty(dpy, main, wmState, wmState, 32, PropModeReplace, reinterpret_cast<const unsigned char *>(value), 2);
    XSync(dpy, False);
}

// Runs the shim's XNextEvent until `w`'s MapNotify has gone through it, then
// lets any request the shim made reach the server. False if it never came.
static bool pumpUntilMapNotify(Window w)
{
    QElapsedTimer t;
    t.start();
    bool seen = false;
    while (!seen && t.elapsed() < 2000) {
        while (!seen && XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            seen = ev.type == MapNotify && ev.xmap.window == w;
        }
        QCoreApplication::processEvents();
    }
    XSync(dpy, False);
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
    XSync(dpy, False);
    return seen;
}

static Window newEditor()
{
    Window editor = XCreateWindow(dpy, DefaultRootWindow(dpy), 10, 10, 200, 100, 0, 0, 0, nullptr, 0, nullptr);
    liveOwnPluginWindow(dpy, editor);
    XSelectInput(dpy, editor, StructureNotifyMask);
    XSync(dpy, False);
    return editor;
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
    realMap = reinterpret_cast<MapFn>(dlsym(RTLD_NEXT, "XMapWindow"));
    realUnmap = reinterpret_cast<MapFn>(dlsym(RTLD_NEXT, "XUnmapWindow"));
    if (!realMap || !realUnmap) {
        std::printf("FAIL: no Xlib map calls\n");
        return 1;
    }

    // Case 1: the window manager shows the editor while the main window is minimised.
    Window editor = newEditor();
    Window owner = 0;
    check(XGetTransientForHint(dpy, editor, &owner) && owner == main, "editor is transient for Live's main window");
    minimiseMain(main);
    check(!mapped(main), "set-up: main window minimised (unmapped, Iconic)");
    realMap(dpy, editor);
    check(pumpUntilMapNotify(editor), "case 1: the editor's MapNotify went through the shim's XNextEvent");
    check(mapped(main), "case 1: main window shown again with the editor");

    // Case 2: the editor's MapNotify still queued when the window manager hides
    // the editor and minimises the main window; drained afterwards.
    realUnmap(dpy, editor);
    XSync(dpy, False);
    while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
    }
    realMap(dpy, editor);
    XSync(dpy, False); // the MapNotify is now in the client's queue, not yet handled
    realUnmap(dpy, editor);
    minimiseMain(main);
    check(!mapped(editor) && !mapped(main), "set-up: editor hidden and main window minimised after the queued map");
    check(pumpUntilMapNotify(editor), "case 2: the queued MapNotify went through the shim's XNextEvent");
    check(!mapped(main), "case 2: a MapNotify drained after the editor was hidden leaves the main window minimised");

    XCloseDisplay(dpy);
    std::printf(failures ? "FAIL: %d check(s) failed\n"
                         : "PASS: the restore answers the window manager's map, and only while the editor is shown\n",
                failures);
    return failures ? 1 : 0;
}
