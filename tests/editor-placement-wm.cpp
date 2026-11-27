// Plug-in window placement where timing matters (shim/editorplacement.cpp):
// the size the window is centred by, a plug-in resize while the window
// manager has not shown the editor yet, and a pending write for a window that
// another client destroyed. Links shim/pluginclose.cpp, so its XStoreName,
// XMapWindow, XMoveResizeWindow and XNextEvent calls go through the shim's
// interposers as Live's do. A stand-in window manager on a second connection
// can redirect map and configure requests and carry them out later, in order.
// Expected positions are worked out from the literal geometries here
// (screen 1280x800, Live's main window client area 600x400 at 200,150).
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
static int escapedErrors = 0;
static void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "  ok  " : "FAIL:", what.c_str());
    if (!ok)
        ++failures;
}

static Display *dpy;
static Display *other; // the window manager's connection

struct Editor {
    Window editor = 0, view = 0;
};

// A tracked, named editor as Live makes one, at Live's position (x, y), whose
// plug-in view (its one mapped child) is viewWidth x viewHeight; not shown.
static Editor openEditor(int x, int y, int width, int height, int viewWidth, int viewHeight, const char *title)
{
    Editor e;
    e.editor = XCreateWindow(dpy, DefaultRootWindow(dpy), x, y, unsigned(width), unsigned(height), 0, 0, 0,
                             nullptr, 0, nullptr);
    e.view = XCreateSimpleWindow(dpy, e.editor, 0, 0, unsigned(viewWidth), unsigned(viewHeight), 0, 0, 0);
    XMapWindow(dpy, e.view);
    XSelectInput(dpy, e.editor, StructureNotifyMask);
    liveOwnPluginWindow(dpy, e.editor);
    XStoreName(dpy, e.editor, title);
    return e;
}

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

// The window manager's own calls (the real Xlib ones, not the shim's).
template <typename F> static F real(const char *name) { return reinterpret_cast<F>(dlsym(RTLD_NEXT, name)); }

static void redirect(bool on)
{
    XSelectInput(other, DefaultRootWindow(other), on ? SubstructureRedirectMask : NoEventMask);
    XSync(other, False);
}

// Carries out the redirected requests, in the order they were made.
static void windowManagerCatchesUp()
{
    static auto realMap = real<decltype(&XMapWindow)>("XMapWindow");
    XSync(dpy, False);
    XSync(other, False);
    while (XPending(other)) {
        XEvent ev;
        XNextEvent(other, &ev);
        if (ev.type == MapRequest) {
            realMap(other, ev.xmaprequest.window);
        } else if (ev.type == ConfigureRequest) {
            const XConfigureRequestEvent &r = ev.xconfigurerequest;
            XWindowChanges c{};
            c.x = r.x;
            c.y = r.y;
            c.width = r.width;
            c.height = r.height;
            XConfigureWindow(other, r.window, unsigned(r.value_mask) & (CWX | CWY | CWWidth | CWHeight), &c);
        }
        XSync(other, False);
    }
}

static void expectAt(const Editor &e, int x, int y, const std::string &what)
{
    XSync(dpy, False);
    int rx = -1, ry = -1;
    Window child;
    XTranslateCoordinates(dpy, e.editor, DefaultRootWindow(dpy), 0, 0, &rx, &ry, &child);
    check(rx == x && ry == y, what + ": at " + std::to_string(x) + "," + std::to_string(y) + " (got "
                                  + std::to_string(rx) + "," + std::to_string(ry) + ")");
}

static void expectSize(const Editor &e, unsigned width, unsigned height, const std::string &what)
{
    XWindowAttributes a{};
    XGetWindowAttributes(dpy, e.editor, &a);
    check(unsigned(a.width) == width && unsigned(a.height) == height,
          what + ": " + std::to_string(width) + "x" + std::to_string(height) + " (got " + std::to_string(a.width)
              + "x" + std::to_string(a.height) + ")");
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
    other = XOpenDisplay(nullptr);
    if (!dpy || !other) {
        std::printf("FAIL: no X display\n");
        return 1;
    }
    XSetErrorHandler([](Display *, XErrorEvent *) {
        ++escapedErrors;
        return 0;
    });
    {
        int mx = -1, my = -1;
        Window child;
        XTranslateCoordinates(dpy, Window(main.winId()), DefaultRootWindow(dpy), 0, 0, &mx, &my, &child);
        check(mx == 200 && my == 150, "set-up: Live's main window at 200,150 (got " + std::to_string(mx) + ","
                                          + std::to_string(my) + ")");
    }

    // 1. Live's window 400x200, the plug-in's view 300x150 (Live resizes its
    // window to the view just before the map, which the window manager may
    // not have done yet): centred by the view, 200 + (600 - 300) / 2,
    // 150 + (400 - 150) / 2.
    {
        Editor e = openEditor(0, 0, 400, 200, 300, 150, "Viewplug/1-Audio");
        XMapWindow(dpy, e.editor);
        pump(100);
        expectAt(e, 350, 275, "first open: centred by the plug-in's view size");
        XDestroyWindow(dpy, e.editor);
    }

    // 2. The plug-in resizes its window after Live asked for the map but
    // before the window manager has shown it; Live's call carries 0,0.
    {
        Editor e = openEditor(0, 0, 400, 200, 400, 200, "Holdplug/1-Audio");
        redirect(true);
        XMapWindow(dpy, e.editor);
        XMoveResizeWindow(dpy, e.editor, 0, 0, 460, 230);
        windowManagerCatchesUp();
        redirect(false);
        pump(100);
        expectAt(e, 300, 250, "resized before the window manager showed it: where it was placed");
        expectSize(e, 460, 230, "resized before the window manager showed it: the new size");
        XDestroyWindow(dpy, e.editor);
    }

    // 3. Moved (a write is due 500 ms later), then destroyed by another
    // client, so the shim never hears of it: the write finds no window, and
    // no X error reaches the process's handler.
    {
        static auto realMove = real<decltype(&XMoveWindow)>("XMoveWindow");
        static auto realDestroy = real<decltype(&XDestroyWindow)>("XDestroyWindow");
        Editor e = openEditor(0, 0, 300, 200, 300, 200, "Goneplug/1-Audio");
        XMapWindow(dpy, e.editor);
        pump(100);
        realMove(other, e.editor, 500, 300);
        XSync(other, False);
        pump(50);
        realDestroy(other, e.editor);
        XSync(other, False);
        pump(800);
    }

    XSync(dpy, False);
    check(escapedErrors == 0, "no X error reached the process's error handler (" + std::to_string(escapedErrors) + ")");
    XCloseDisplay(other);
    XCloseDisplay(dpy);
    if (failures)
        std::printf("FAIL: %d check(s) failed\n", failures);
    else
        std::printf("PASS: placement under a window manager's timing\n");
    return failures ? 1 : 0;
}
