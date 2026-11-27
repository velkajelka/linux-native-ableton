// Plug-in editor title bars take the plug-in's colour (shim/editorcolour.cpp):
// on a private X display, editor windows whose plug-in view (a child window)
// is painted with known colours. Checks the _KDE_NET_WM_COLOR_SCHEME property
// each editor gets and the title bar colours in the scheme file it names.
// Expected colours are the literals painted here.
#include "chrome.h"
#include "editorcolour.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMainWindow>
#include <QStandardPaths>
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

// Stand-ins for the parts of the shim window.cpp links against.
void menufixTrace(const char *, int, int, int) {}
void *nextSymbol(const char *name, const char *) { return dlsym(RTLD_NEXT, name); }
void *hookFunction(uintptr_t, const unsigned char *, size_t, void *) { return nullptr; }
bool scaleTransition() { return false; }

static int failures = 0;
static int escapedErrors = 0;
static void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "  ok  " : "FAIL:", what.c_str());
    if (!ok)
        ++failures;
}

static Display *dpy;

// An editor (top-level, like Live's) with one mapped child (the plug-in's
// view) at depth `depth`; not mapped yet. X keeps no drawing made while a
// window is hidden, so the cases paint after the map, as plug-ins do.
struct Editor {
    Window editor = 0, view = 0;
};

static Editor makeEditor(int x, int y, int width, int height, int depth = 24)
{
    Editor e;
    e.editor = XCreateWindow(dpy, DefaultRootWindow(dpy), x, y, unsigned(width), unsigned(height), 0, 0, 0,
                             nullptr, 0, nullptr);
    if (depth == 24) {
        e.view = XCreateSimpleWindow(dpy, e.editor, 0, 0, unsigned(width), unsigned(height), 0, 0, 0);
    } else {
        XVisualInfo info;
        if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), depth, TrueColor, &info))
            return e;
        XSetWindowAttributes a{};
        a.colormap = XCreateColormap(dpy, DefaultRootWindow(dpy), info.visual, AllocNone);
        a.border_pixel = 0;
        a.background_pixel = 0;
        e.view = XCreateWindow(dpy, e.editor, 0, 0, unsigned(width), unsigned(height), 0, depth, InputOutput,
                               info.visual, CWColormap | CWBorderPixel | CWBackPixel, &a);
    }
    XMapWindow(dpy, e.view);
    return e;
}

static void paint(Window w, int depth, unsigned long rgb, int x, int y, int width, int height)
{
    GC gc = XCreateGC(dpy, w, 0, nullptr);
    XSetForeground(dpy, gc, depth == 32 ? (0xff000000UL | rgb) : rgb);
    XFillRectangle(dpy, w, gc, x, y, unsigned(width), unsigned(height));
    XFreeGC(dpy, gc);
}

static void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
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

// "[WM] <key>" from the scheme file at `path`, as written ("r,g,b").
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

static void expectScheme(Window w, const char *band, const char *text, const char *what)
{
    const QString path = schemeOf(w);
    check(!path.isEmpty() && QFile::exists(path), std::string(what) + ": the editor names an existing scheme");
    check(wmValue(path, "activeBackground") == band && wmValue(path, "inactiveBackground") == band,
          std::string(what) + ": title bar " + band + " (got " + wmValue(path, "activeBackground").toStdString() + ")");
    check(wmValue(path, "activeForeground") == text,
          std::string(what) + ": title text " + text + " (got " + wmValue(path, "activeForeground").toStdString() + ")");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QMainWindow main; // Live's main window, for liveOwnPluginWindow
    main.resize(300, 200);
    main.show();
    main.activateWindow();
    pump(200);

    dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::printf("FAIL: no X display\n");
        return 1;
    }
    // Errors the shim does not catch land here (the default handler would
    // end the process, as it would end Live).
    XSetErrorHandler([](Display *, XErrorEvent *) {
        ++escapedErrors;
        return 0;
    });
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Ableton/Window Colors";
    // Live's own scheme, written as Live's theme code writes it.
    const QString liveScheme = chrome::writeColorScheme(dir, QColor("#2b2b2b"), QColor("#dddddd"));
    check(!liveScheme.isEmpty(), "Live's own scheme is written");

    // 1. Dark plug-in with a light logo over its left quarter.
    Editor dark = makeEditor(0, 0, 600, 200);
    XMapWindow(dpy, dark.editor);
    XSync(dpy, False);
    paint(dark.view, 24, 0x131114, 0, 0, 600, 200);
    paint(dark.view, 24, 0xc0c0c0, 0, 0, 150, 30);
    XSync(dpy, False);
    pump(100);
    check(editorColourApply(dpy, dark.editor), "dark: a colour is applied");
    expectScheme(dark.editor, "19,17,20", "230,230,230", "dark with logo");

    // 2. Light plug-in.
    Editor light = makeEditor(620, 0, 400, 150);
    XMapWindow(dpy, light.editor);
    XSync(dpy, False);
    paint(light.view, 24, 0xe9e6df, 0, 0, 400, 150);
    XSync(dpy, False);
    pump(100);
    check(editorColourApply(dpy, light.editor), "light: a colour is applied");
    expectScheme(light.editor, "233,230,223", "26,26,26", "light");

    // 3. Plug-in view in a 32-bit (ARGB) window.
    Editor argb = makeEditor(0, 220, 400, 150, 32);
    check(argb.view != 0, "argb: a 32-bit visual exists");
    if (argb.view) {
        XMapWindow(dpy, argb.editor);
        XSync(dpy, False);
        paint(argb.view, 32, 0x336699, 0, 0, 400, 150);
        XSync(dpy, False);
        pump(100);
        check(editorColourApply(dpy, argb.editor), "argb: a colour is applied");
        expectScheme(argb.editor, "51,102,153", "230,230,230", "argb");
    }

    // 4. Not yet painted: all black.
    Editor blank = makeEditor(420, 220, 300, 120);
    XMapWindow(dpy, blank.editor);
    XSync(dpy, False);
    paint(blank.view, 24, 0x000000, 0, 0, 300, 120);
    XSync(dpy, False);
    pump(100);
    check(!editorColourApply(dpy, blank.editor), "unpainted: nothing applied");
    check(schemeOf(blank.editor).isEmpty(), "unpainted: no property (KDE's default title bar)");

    // 5. Never mapped.
    Editor hidden = makeEditor(740, 220, 300, 120);
    paint(hidden.view, 24, 0x445566, 0, 0, 300, 120);
    XSync(dpy, False);
    check(!editorColourApply(dpy, hidden.editor), "unmapped: nothing applied");
    check(schemeOf(hidden.editor).isEmpty(), "unmapped: no property");

    // 6. Already destroyed.
    Editor gone = makeEditor(0, 400, 200, 100);
    XMapWindow(dpy, gone.editor);
    XSync(dpy, False);
    const Window goneId = gone.editor;
    XDestroyWindow(dpy, gone.editor);
    XSync(dpy, False);
    check(!editorColourApply(dpy, goneId), "destroyed: nothing applied");

    // 7. Through the hooks: owned by Live, read after the map, read again
    // after a resize.
    Editor hooked = makeEditor(220, 400, 500, 160);
    liveOwnPluginWindow(dpy, hooked.editor);
    XSync(dpy, False);
    Window owner = 0;
    check(XGetTransientForHint(dpy, hooked.editor, &owner) && owner == main.winId(),
          "hooked: editor is transient for Live's main window");
    XClassHint hint{};
    const bool gotClass = XGetClassHint(dpy, hooked.editor, &hint);
    check(gotClass && hint.res_name && std::strcmp(hint.res_name, "ableton-live-plugin") == 0,
          "hooked: editor keeps the ableton-live-plugin instance name");
    if (gotClass) {
        XFree(hint.res_name);
        XFree(hint.res_class);
    }
    XMapWindow(dpy, hooked.editor);
    XSync(dpy, False);
    paint(hooked.view, 24, 0x204060, 0, 0, 500, 160);
    XSync(dpy, False);
    editorColourMapped(hooked.editor);
    check(schemeOf(hooked.editor).isEmpty(), "hooked: nothing before the plug-in has had time to draw");
    pump(2000);
    expectScheme(hooked.editor, "32,64,96", "230,230,230", "hooked, after the map");
    XResizeWindow(dpy, hooked.editor, 560, 180);
    XResizeWindow(dpy, hooked.view, 560, 180);
    paint(hooked.view, 24, 0x604020, 0, 0, 560, 180);
    XSync(dpy, False);
    editorColourResized(hooked.editor);
    pump(1000);
    expectScheme(hooked.editor, "96,64,32", "230,230,230", "hooked, after a resize");

    // 8. A forgotten editor's pending read does nothing.
    Editor closed = makeEditor(740, 400, 200, 100);
    liveOwnPluginWindow(dpy, closed.editor);
    XMapWindow(dpy, closed.editor);
    XSync(dpy, False);
    paint(closed.view, 24, 0x806040, 0, 0, 200, 100);
    XSync(dpy, False);
    editorColourMapped(closed.editor);
    editorColourForget(closed.editor);
    pump(2000);
    check(schemeOf(closed.editor).isEmpty(), "forgotten: its pending read sets nothing");

    // 9. Scheme files: those in use stay, Live's own stays.
    check(QFile::exists(schemeOf(dark.editor)), "files: the first editor's scheme is still there");
    check(QFile::exists(schemeOf(light.editor)), "files: the second editor's scheme is still there");
    check(QFile::exists(liveScheme), "files: Live's own scheme is still there");

    XSync(dpy, False);
    check(escapedErrors == 0, "no X error reached the process's error handler (" + std::to_string(escapedErrors) + ")");
    XCloseDisplay(dpy);
    std::printf(failures ? "FAIL: %d check(s) failed\n" : "PASS: editor title bars take the plug-in's colour\n",
                failures);
    return failures ? 1 : 0;
}
