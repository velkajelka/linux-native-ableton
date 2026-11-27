// When title bar colours are read after an editor is shown
// (shim/editorcolour.cpp): a read is an XGetImage of the editor's view, and
// this program counts them per view by standing in for XGetImage (it links
// editorcolour.cpp, so the shim's reads come here first). It links
// shim/pluginclose.cpp, so its XStoreName, XMapWindow and XUnmapWindow calls
// go through the shim's interposers as Live's do. The bounds:
// reads after a map run for at most 1.5 s and number at most 30.
#include "editorcolour.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
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

static QHash<Drawable, int> reads; // XGetImage calls per drawable

extern "C" XImage *XGetImage(Display *d, Drawable w, int x, int y, unsigned width, unsigned height,
                             unsigned long planes, int format)
{
    static auto real = reinterpret_cast<decltype(&XGetImage)>(dlsym(RTLD_NEXT, "XGetImage"));
    ++reads[w];
    return real(d, w, x, y, width, height, planes, format);
}

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

// A tracked, named editor as Live makes one, with one mapped child (the
// plug-in's view); not shown. X keeps no drawing made on a hidden window.
static Editor openEditor(int x, int y, int width, int height, const char *title)
{
    Editor e;
    e.editor = XCreateWindow(dpy, DefaultRootWindow(dpy), x, y, unsigned(width), unsigned(height), 0, 0, 0,
                             nullptr, 0, nullptr);
    e.view = XCreateSimpleWindow(dpy, e.editor, 0, 0, unsigned(width), unsigned(height), 0, 0, 0);
    XMapWindow(dpy, e.view);
    liveOwnPluginWindow(dpy, e.editor);
    XStoreName(dpy, e.editor, title);
    return e;
}

static void show(const Editor &e)
{
    XMapWindow(dpy, e.editor);
    XSync(dpy, False);
}

static void paint(Window w, unsigned long rgb, int width, int height)
{
    GC gc = XCreateGC(dpy, w, 0, nullptr);
    XSetForeground(dpy, gc, rgb);
    XFillRectangle(dpy, w, gc, 0, 0, unsigned(width), unsigned(height));
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
    if (!dpy) {
        std::printf("FAIL: no X display\n");
        return 1;
    }
    XSetErrorHandler([](Display *, XErrorEvent *) {
        ++escapedErrors;
        return 0;
    });

    // 1. A short startup frame: a splash drawn as soon as the editor is
    // shown, the plug-in's own view 120 ms later at the same size. By 350 ms
    // the title bar has the view's colour, and an editor closed before
    // 1.5 s leaves the view's colour remembered, not the splash's.
    {
        Editor e = openEditor(0, 0, 400, 120, "Flashplug/1-Audio");
        show(e);
        paint(e.view, 0xc8c8c8, 400, 120);
        pump(120);
        paint(e.view, 0x3a2a5c, 400, 120);
        pump(230);
        expectBand(e.editor, "58,42,92", "short splash: the view's colour by 350 ms");
        XDestroyWindow(dpy, e.editor);
        XSync(dpy, False);
        pump(50);
        Editor again = openEditor(0, 0, 400, 120, "Flashplug/2-Bass");
        expectBand(again.editor, "58,42,92", "short splash, closed before 1.5 s: the view's colour is remembered");
        XDestroyWindow(dpy, again.editor);
        XSync(dpy, False);
        pump(1700); // let its own timers run out
    }

    // 2. A blank editor with the GUI loop running: at most 30 reads after the map.
    {
        Editor e = openEditor(420, 0, 300, 100, "Blankplug/1-Audio");
        show(e);
        pump(2000);
        const int n = reads.value(e.view) + reads.value(e.editor);
        check(n >= 2 && n <= 30, "blank editor: 30 reads at most after the map (" + std::to_string(n) + ")");
    }

    // 3. A blank editor while Live's GUI thread is busy past the 1.5 s
    // deadline (loading a set): once the loop runs again, only the late read
    // may still read.
    {
        Editor e = openEditor(740, 0, 300, 100, "Busyplug/1-Audio");
        show(e);
        QThread::msleep(1800); // no events handled
        const int before = reads.value(e.view) + reads.value(e.editor);
        pump(1500);
        const int after = reads.value(e.view) + reads.value(e.editor) - before;
        check(after <= 1, "busy GUI thread: one read at most once the deadline has passed (" + std::to_string(after) + ")");
    }

    // 4. Hidden and shown again while the first map's reads are still due:
    // only the second map's reads run, so no more than one map's worth.
    {
        Editor e = openEditor(0, 300, 300, 100, "Hideplug/1-Audio");
        show(e);
        pump(150);
        XUnmapWindow(dpy, e.editor); // deferred 50 ms by the shim
        XSync(dpy, False);
        pump(150);
        const int before = reads.value(e.view) + reads.value(e.editor);
        show(e);
        pump(2500);
        const int after = reads.value(e.view) + reads.value(e.editor) - before;
        check(after <= 30, "shown again: one map's reads at most after the re-show (" + std::to_string(after) + ")");
    }

    XSync(dpy, False);
    check(escapedErrors == 0, "no X error reached the process's error handler (" + std::to_string(escapedErrors) + ")");
    XCloseDisplay(dpy);
    std::printf(failures ? "FAIL: %d check(s) failed\n" : "PASS: colour reads after a map are timed and bounded\n",
                failures);
    return failures ? 1 : 0;
}
