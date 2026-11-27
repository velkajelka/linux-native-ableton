// Plug-in windows open centred over Live's main window the first time, then
// where they were last left (shim/editorplacement.cpp). Runs three times
// against one data folder (tests/editor-placement.sh), standing for three Live
// sessions: argv[1] is "first", "second" or "third". It links
// shim/pluginclose.cpp, so its XStoreName, XMapWindow, XUnmapWindow,
// XMoveResizeWindow, XNextEvent and XDestroyWindow calls go through the
// shim's interposers as Live's do. The display has no window manager, so X
// positions are exact; a move by the window manager is stood in for by
// another X client's move. Expected positions are worked out from the
// literal geometries here (screen 1280x800).
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

// A tracked, named editor as Live makes one, at Live's position (x, y), with
// one mapped child (the plug-in's view); not shown.
static Editor openEditor(int x, int y, int width, int height, const char *title)
{
    Editor e;
    e.editor = XCreateWindow(dpy, DefaultRootWindow(dpy), x, y, unsigned(width), unsigned(height), 0, 0, 0,
                             nullptr, 0, nullptr);
    e.view = XCreateSimpleWindow(dpy, e.editor, 0, 0, unsigned(width), unsigned(height), 0, 0, 0);
    XMapWindow(dpy, e.view);
    XSelectInput(dpy, e.editor, StructureNotifyMask); // as Live's editors get (pluginclose.cpp)
    liveOwnPluginWindow(dpy, e.editor);
    XStoreName(dpy, e.editor, title);
    return e;
}

static void show(const Editor &e)
{
    XMapWindow(dpy, e.editor);
    XSync(dpy, False);
}

// Runs the GUI loop and hands this connection's events to the (interposed)
// XNextEvent, as Live's event pump does.
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

// The window manager moves the editor (another client, the real Xlib call).
static void wmMove(const Editor &e, int x, int y)
{
    static auto realMove = reinterpret_cast<decltype(&XMoveWindow)>(dlsym(RTLD_NEXT, "XMoveWindow"));
    realMove(other, e.editor, x, y);
    XSync(other, False);
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

static void close(const Editor &e)
{
    XDestroyWindow(dpy, e.editor);
    XSync(dpy, False);
}

int main(int argc, char **argv)
{
    const std::string run = argc > 1 ? argv[1] : "";
    QApplication app(argc, argv);
    // Live's main window: client area 600x400 at 200,150.
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

    if (run == "first") {
        // 1. Never opened before, created by Live at 0,0: centred over the
        // main window: 200 + (600 - 400) / 2, 150 + (400 - 200) / 2.
        Editor e = openEditor(0, 0, 400, 200, "Placeplug/1-Audio");
        show(e);
        pump(100);
        expectAt(e, 300, 250, "new plug-in: centred over Live");

        // 3. The plug-in resizes its window; Live's call carries the place it
        // created the editor at.
        XMoveResizeWindow(dpy, e.editor, 0, 0, 460, 230);
        pump(100);
        expectAt(e, 300, 250, "plug-in resize: stays where it is");
        expectSize(e, 460, 230, "plug-in resize: the new size");

        // 4. Moved, then hidden and shown again at once, before the move was
        // written down: not moved back. (These editors are not made through
        // Live's editor-open path, so the shim does not merge the hide away.)
        wmMove(e, 500, 100);
        pump(50);
        XUnmapWindow(dpy, e.editor);
        XMapWindow(dpy, e.editor);
        pump(100);
        expectAt(e, 500, 100, "hidden and shown at once: stays where it is");

        // 2. Moved and closed; the same plug-in on another track opens there.
        wmMove(e, 700, 300);
        pump(800);
        close(e);
        Editor again = openEditor(0, 0, 400, 200, "Placeplug/2-Bass");
        show(again);
        pump(100);
        expectAt(again, 700, 300, "same plug-in, another track: where it was left");
        close(again);

        // A plug-in left partly off the screen (300x200 at 1200,750).
        Editor edge = openEditor(0, 0, 300, 200, "Edgeplug/1-Audio");
        show(edge);
        pump(100);
        wmMove(edge, 1200, 750);
        pump(800);
        close(edge);

        // Moved and closed within 100 ms: the move is still kept.
        Editor quick = openEditor(0, 0, 300, 200, "Quickplug/1-Audio");
        show(quick);
        pump(100);
        wmMove(quick, 100, 500);
        pump(100);
        close(quick);
        pump(100);
    } else if (run == "second") {
        Editor e = openEditor(0, 0, 400, 200, "Placeplug/3-Keys");
        show(e);
        pump(100);
        expectAt(e, 700, 300, "next session: where it was left");
        Editor edge = openEditor(0, 0, 300, 200, "Edgeplug/2-Bass");
        show(edge);
        pump(100);
        expectAt(edge, 980, 600, "left partly off the screen: moved fully onto it (1280-300, 800-200)");
        Editor quick = openEditor(0, 0, 300, 200, "Quickplug/2-Bass");
        show(quick);
        pump(100);
        expectAt(quick, 100, 500, "moved just before closing: kept");
        Editor fresh = openEditor(40, 40, 200, 100, "Freshplug/1-Audio");
        show(fresh);
        pump(100);
        expectAt(fresh, 400, 300, "another new plug-in: centred over Live (200 + 200, 150 + 150)");
        // Moved and left open when Live quits (nothing closes it).
        wmMove(fresh, 500, 450);
        pump(800);
    } else if (run == "third") {
        Editor e = openEditor(0, 0, 400, 200, "Placeplug/4-Audio");
        show(e);
        pump(100);
        expectAt(e, 700, 300, "third session: still where it was left");
        Editor fresh = openEditor(0, 0, 200, 100, "Freshplug/2-Bass");
        show(fresh);
        pump(100);
        expectAt(fresh, 500, 450, "moved and left open when Live quit: where it was left");
    } else {
        std::printf("FAIL: usage: editor-placement first|second|third\n");
        return 2;
    }

    XSync(dpy, False);
    check(escapedErrors == 0, "no X error reached the process's error handler (" + std::to_string(escapedErrors) + ")");
    XCloseDisplay(other);
    XCloseDisplay(dpy);
    if (failures)
        std::printf("FAIL: %d check(s) failed (%s session)\n", failures, run.c_str());
    else
        std::printf("PASS: %s session\n", run.c_str());
    return failures ? 1 : 0;
}
