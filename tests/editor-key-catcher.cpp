// The key catcher an editor window gets (shim/window.cpp) is never taken for
// the plug-in's view. Live makes the editor window first (the shim then adds
// the catcher) and the plug-in adds its view afterwards, so the catcher comes
// first among the editor's children; the shim's placement finds the view to
// centre a new plug-in window by its size (shim/editorplacement.cpp). Links
// shim/pluginclose.cpp, so XStoreName, XMapWindow and XNextEvent go through
// the shim's interposers as Live's do. No window manager runs; screen
// 1280x800, Live's main window 600x400 at 200,150.
#include "editorcolour.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QThread>
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

static void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QMainWindow main;
    main.setGeometry(200, 150, 600, 400);
    main.show();
    main.activateWindow();
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 300)
        app.processEvents();
    dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::printf("FAIL: no X display\n");
        return 1;
    }

    // Live makes the editor window, 400x200 at 0,0; the shim owns it (and adds the catcher).
    const Window editor = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, 400, 200, 0, 0, 0, nullptr, 0, nullptr);
    XSelectInput(dpy, editor, StructureNotifyMask); // as Live's editors get (pluginclose.cpp)
    liveOwnPluginWindow(dpy, editor);
    Window root, parent, *kids = nullptr;
    unsigned count = 0;
    XQueryTree(dpy, editor, &root, &parent, &kids, &count);
    check(count == 1, "set-up: the editor has one child (the catcher) before the plug-in's view ("
                          + std::to_string(count) + ")");
    if (kids)
        XFree(kids);

    // The plug-in then adds its view, 200x100, and Live names and shows the window.
    const Window view = XCreateSimpleWindow(dpy, editor, 0, 0, 200, 100, 0, 0, 0);
    XMapWindow(dpy, view);
    XStoreName(dpy, editor, "Catcherplug/1-Audio");
    XMapWindow(dpy, editor);
    XSync(dpy, False);
    pump(200);

    // Centred over Live by the view's size: 200 + (600 - 200) / 2, 150 + (400 - 100) / 2.
    int x = -1, y = -1;
    Window child;
    XTranslateCoordinates(dpy, editor, DefaultRootWindow(dpy), 0, 0, &x, &y, &child);
    check(x == 400 && y == 300, "new plug-in window centred by its view's size, not the catcher's: at 400,300 (got "
                                    + std::to_string(x) + "," + std::to_string(y) + ")");

    XCloseDisplay(dpy);
    std::printf(failures ? "FAIL: %d check(s) failed\n" : "PASS: the key catcher is never taken for the plug-in's view\n",
                failures);
    return failures ? 1 : 0;
}
