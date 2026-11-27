// Plug-in title bars without the wait (shim/editorcolour.cpp): each
// plug-in's colour is remembered by name (the editor's title up to the first
// "/", as Live names editors with XStoreName) and given to its next editor
// before that editor is shown; a plug-in seen for the first time gets its
// colour as soon as it has drawn. The program runs three times against one
// data folder (tests/editor-colour-instant.sh), standing for three Live
// sessions: argv[1] is "first", "second" or "third". It links
// shim/pluginclose.cpp, so its XStoreName and XMapWindow calls go through the
// shim's interposers as Live's do. Expected colours are the literals painted.
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


// A tracked, named editor as Live makes one; not mapped.
static Editor openEditor(int x, int y, int width, int height, const char *title)
{
    Editor e = makeEditor(x, y, width, height);
    liveOwnPluginWindow(dpy, e.editor);
    XStoreName(dpy, e.editor, title);
    return e;
}

static void show(const Editor &e)
{
    XMapWindow(dpy, e.editor);
    XSync(dpy, False);
}

int main(int argc, char **argv)
{
    const std::string run = argc > 1 ? argv[1] : "";
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

    if (run == "first") {
        // A plug-in that draws as soon as it is shown.
        Editor known = openEditor(0, 0, 400, 120, "Testplug/1-Audio");
        show(known);
        paint(known.view, 0x2e5a7a, 0, 0, 400, 120);
        pump(250);
        expectBand(known.editor, "46,90,122", "first session: drawn at once, coloured within 250 ms");

        // A plug-in seen for the first time that draws 120 ms after it is shown.
        Editor fresh = openEditor(420, 0, 400, 120, "Freshplug/1-Bass");
        show(fresh);
        pump(120);
        check(schemeOf(fresh.editor).isEmpty(), "first-time plug-in: nothing before it draws");
        paint(fresh.view, 0x6a3d8c, 0, 0, 400, 120);
        pump(120);
        expectBand(fresh.editor, "106,61,140", "first-time plug-in: coloured within 120 ms of drawing");

        // A splash first, the real view 800 ms later.
        Editor splash = openEditor(840, 0, 400, 120, "Splashplug/1-Keys");
        show(splash);
        paint(splash.view, 0xd0d0d0, 0, 0, 400, 120);
        pump(800);
        paint(splash.view, 0x224466, 0, 0, 400, 120);
        pump(900);
        expectBand(splash.editor, "34,68,102", "splash then view: ends on the view's colour");

        // Never drawn.
        Editor blank = openEditor(0, 200, 300, 100, "Blankplug/1-Audio");
        show(blank);

        // A plug-in that will draw another colour next session.
        Editor change = openEditor(320, 200, 300, 100, "Changeplug/1-Audio");
        show(change);
        paint(change.view, 0x884422, 0, 0, 300, 100);
        pump(1700);
        expectBand(change.editor, "136,68,34", "changing plug-in, first session");
        check(schemeOf(blank.editor).isEmpty(), "never drawn: no colour");
    } else if (run == "second") {
        // Remembered plug-ins: coloured before the editor is shown, with no
        // GUI loop pass and nothing drawn, also on another track.
        Editor known = openEditor(0, 0, 400, 120, "Testplug/2-Bass");
        expectBand(known.editor, "46,90,122", "second session: remembered colour before the editor is shown");
        show(known);
        Editor splash = openEditor(840, 0, 400, 120, "Splashplug/2-Keys");
        expectBand(splash.editor, "34,68,102", "second session: the view's colour is remembered, not the splash");
        Editor blank = openEditor(0, 200, 300, 100, "Blankplug/2-Audio");
        show(blank);
        check(schemeOf(blank.editor).isEmpty(), "second session: nothing remembered for a plug-in never drawn");

        // The plug-in now draws another colour: what it draws wins and is remembered.
        Editor change = openEditor(320, 200, 300, 100, "Changeplug/2-Audio");
        expectBand(change.editor, "136,68,34", "changing plug-in: last session's colour at first");
        show(change);
        paint(change.view, 0x228844, 0, 0, 300, 100);
        pump(250);
        expectBand(change.editor, "34,136,68", "changing plug-in: the colour it draws now replaces it");
        pump(1500);
    } else if (run == "third") {
        Editor change = openEditor(320, 200, 300, 100, "Changeplug/3-Audio");
        expectBand(change.editor, "34,136,68", "third session: the updated colour is remembered");
    } else {
        std::printf("FAIL: usage: editor-colour-instant first|second|third\n");
        return 2;
    }

    XSync(dpy, False);
    check(escapedErrors == 0, "no X error reached the process's error handler (" + std::to_string(escapedErrors) + ")");
    XCloseDisplay(dpy);
    if (failures)
        std::printf("FAIL: %d check(s) failed (%s session)\n", failures, run.c_str());
    else
        std::printf("PASS: %s session\n", run.c_str());
    return failures ? 1 : 0;
}
