// Plug-in editor title bars follow the editor through the shim's own X
// hooks (shim/pluginclose.cpp interposers -> shim/editorcolour.cpp): the
// test program links pluginclose.cpp, so its XMapWindow, XResizeWindow,
// XNextEvent and XDestroyWindow calls go through the same interposers Live's
// do. A frame resize comes from another X client through the real Xlib call,
// as a window manager's does, and reaches the hooks only as a ConfigureNotify.
// Expected colours are the literals painted here.
#include "editorcolour.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMainWindow>
#include <QThread>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

struct Editor {
    Window editor = 0, view = 0;
};

// An editor (top-level) with one mapped child (the plug-in's view); not
// mapped yet. X keeps no drawing made on a hidden window: paint after the map.
static Editor makeEditor(int x, int y, int width, int height)
{
    Editor e;
    e.editor = XCreateWindow(dpy, DefaultRootWindow(dpy), x, y, unsigned(width), unsigned(height), 0, 0, 0,
                             nullptr, 0, nullptr);
    e.view = XCreateSimpleWindow(dpy, e.editor, 0, 0, unsigned(width), unsigned(height), 0, 0, 0);
    XMapWindow(dpy, e.view);
    return e;
}

static void paint(Window w, unsigned long rgb, int x, int y, int width, int height)
{
    GC gc = XCreateGC(dpy, w, 0, nullptr);
    XSetForeground(dpy, gc, rgb);
    XFillRectangle(dpy, w, gc, x, y, unsigned(width), unsigned(height));
    XFreeGC(dpy, gc);
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

static QString schemeOf(Window w)
{
    Atom type;
    int format;
    unsigned long items, rest;
    unsigned char *data = nullptr;
    QString path;
    if (XGetWindowProperty(dpy, w, XInternAtom(dpy, "_KDE_NET_WM_COLOR_SCHEME", False), 0, 4096, False, XA_STRING,
                           &type, &format, &items, &rest, &data)
            == Success
        && data && format == 8)
        path = QString::fromLocal8Bit(reinterpret_cast<const char *>(data), int(items));
    if (data)
        XFree(data);
    return path;
}

static QString wmValue(const QString &path, const char *key)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    bool inWm = false;
    for (const QByteArray &raw : f.readAll().split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.startsWith('['))
            inWm = line == "[WM]";
        else if (inWm && line.startsWith(QByteArray(key) + '='))
            return QString::fromLatin1(line.mid(int(std::strlen(key)) + 1));
    }
    return {};
}

static void expectBand(Window w, const char *band, const char *what)
{
    const QString got = wmValue(schemeOf(w), "activeBackground");
    check(got == band, std::string(what) + ": title bar " + band + " (got " + got.toStdString() + ")");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QMainWindow main; // Live's main window, for liveOwnPluginWindow
    main.resize(300, 200);
    main.show();
    main.activateWindow();
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 200)
        app.processEvents();

    dpy = XOpenDisplay(nullptr);
    Display *other = XOpenDisplay(nullptr); // the window manager's connection
    if (!dpy || !other) {
        std::printf("FAIL: no X display\n");
        return 1;
    }
    XSetErrorHandler([](Display *, XErrorEvent *) {
        ++escapedErrors;
        return 0;
    });
    // The real Xlib call, past this program's interposer, for the other client.
    const auto realResize = reinterpret_cast<decltype(&XResizeWindow)>(dlsym(RTLD_NEXT, "XResizeWindow"));

    // 1. Shown blank, resized at once (Live sizes editors as it opens them),
    // drawn only after the resize read: the late read after the map still
    // finds the colour.
    Editor late = makeEditor(0, 0, 500, 160);
    liveOwnPluginWindow(dpy, late.editor);
    XMapWindow(dpy, late.editor);
    XSync(dpy, False);
    pump(50);
    XResizeWindow(dpy, late.editor, 520, 170);
    XResizeWindow(dpy, late.view, 520, 170);
    XSync(dpy, False);
    pump(600); // the resize read (400 ms) sees black
    check(schemeOf(late.editor).isEmpty(), "late painter: nothing while it shows black");
    paint(late.view, 0x2a5070, 0, 0, 520, 170);
    pump(1200); // past the 1500 ms read after the map
    expectBand(late.editor, "42,80,112", "late painter, read after the map");

    // 2. A frame resize by another client (the window manager): the editor
    // hears of it only as a ConfigureNotify; the plug-in redraws in a new colour.
    Editor framed = makeEditor(600, 0, 400, 150);
    liveOwnPluginWindow(dpy, framed.editor);
    XSelectInput(dpy, framed.editor, StructureNotifyMask);
    XMapWindow(dpy, framed.editor);
    XSync(dpy, False);
    paint(framed.view, 0x335533, 0, 0, 400, 150);
    pump(1700);
    expectBand(framed.editor, "51,85,51", "framed, after the map");
    realResize(other, framed.editor, 450, 170);
    XSync(other, False);
    pump(50);
    XResizeWindow(dpy, framed.view, 450, 170);
    paint(framed.view, 0x553333, 0, 0, 450, 170);
    pump(1000);
    expectBand(framed.editor, "85,51,51", "framed, after the frame resize");

    // 3. A closed editor's scheme is no longer kept: once it is destroyed,
    // the next editor's scheme replaces its file.
    Editor closed = makeEditor(0, 300, 300, 100);
    liveOwnPluginWindow(dpy, closed.editor);
    XMapWindow(dpy, closed.editor);
    XSync(dpy, False);
    paint(closed.view, 0x404080, 0, 0, 300, 100);
    pump(1700);
    const QString closedScheme = schemeOf(closed.editor);
    check(QFile::exists(closedScheme), "closed: its scheme was written");
    XDestroyWindow(dpy, closed.editor);
    XSync(dpy, False);
    Editor next = makeEditor(320, 300, 300, 100);
    liveOwnPluginWindow(dpy, next.editor);
    XMapWindow(dpy, next.editor);
    XSync(dpy, False);
    paint(next.view, 0x804040, 0, 0, 300, 100);
    pump(1700);
    expectBand(next.editor, "128,64,64", "next editor");
    check(!closedScheme.isEmpty() && !QFile::exists(closedScheme), "closed: its scheme file is gone");
    check(QFile::exists(schemeOf(late.editor)) && QFile::exists(schemeOf(framed.editor)),
          "open editors keep their schemes");

    // 4. A black header with a coloured logo over a quarter of its top is
    // drawn: its title bar is black, with light text.
    Editor black = makeEditor(640, 300, 400, 100);
    XMapWindow(dpy, black.editor);
    XSync(dpy, False);
    paint(black.view, 0x000000, 0, 0, 400, 100);
    paint(black.view, 0xc08040, 0, 0, 100, 30);
    check(editorColourApply(dpy, black.editor), "black header with a logo: a colour is applied");
    expectBand(black.editor, "0,0,0", "black header with a logo");
    check(wmValue(schemeOf(black.editor), "activeForeground") == "230,230,230", "black header: light title text");

    // 5. A plug-in that frames its view with a thin near-black outline (as
    // Serum 2 does, colours measured from its window): a 2-pixel line along
    // the top and 6-pixel sides, its slate header below. The title bar takes
    // the header, not the outline.
    Editor framedView = makeEditor(0, 420, 600, 120);
    XMapWindow(dpy, framedView.editor);
    XSync(dpy, False);
    paint(framedView.view, 0x191d21, 0, 0, 600, 120);
    paint(framedView.view, 0x010101, 0, 0, 600, 2);
    paint(framedView.view, 0x010101, 0, 0, 6, 120);
    paint(framedView.view, 0x010101, 594, 0, 6, 120);
    check(editorColourApply(dpy, framedView.editor), "outlined view: a colour is applied");
    expectBand(framedView.editor, "25,29,33", "outlined view, header under a thin outline");

    // 6. An editor with only its top 8 rows on the screen (the screen is 800
    // high): those rows are read.
    Editor low = makeEditor(700, 792, 300, 120);
    XMapWindow(dpy, low.editor);
    XSync(dpy, False);
    paint(low.view, 0x3a6b4c, 0, 0, 300, 120);
    check(editorColourApply(dpy, low.editor), "8 rows on screen: a colour is applied");
    expectBand(low.editor, "58,107,76", "8 rows on screen");

    XSync(dpy, False);
    check(escapedErrors == 0, "no X error reached the process's error handler (" + std::to_string(escapedErrors) + ")");
    XCloseDisplay(other);
    XCloseDisplay(dpy);
    std::printf(failures ? "FAIL: %d check(s) failed\n" : "PASS: title bars follow editors through the shim's X hooks\n",
                failures);
    return failures ? 1 : 0;
}
