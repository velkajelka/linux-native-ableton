// Plug-in editor title bars show no Live icon (shim/window.cpp
// liveOwnPluginWindow): on a private X display, a stand-in for Live's main
// window carrying an icon, and an unmapped editor window as Live creates it.
// After liveOwnPluginWindow the editor's _NET_WM_ICON must be one fully
// transparent 1x1 pixel, so the window manager draws no icon in its title bar
// (with no icon at all it would fall back to Live's icon from the class).
#include <QApplication>
#include <QMainWindow>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <vector>

void liveOwnPluginWindow(Display *dpy, Window editor); // window.cpp

// Stand-ins for the parts of the shim window.cpp links against.
void menufixTrace(const char *, int, int, int) {}
void *nextSymbol(const char *name, const char *) { return dlsym(RTLD_NEXT, name); }
void *hookFunction(uintptr_t, const unsigned char *, size_t, void *) { return nullptr; }
bool scaleTransition() { return false; }

static int failures = 0;
static void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "  ok  " : "FAIL:", what);
    if (!ok)
        ++failures;
}

static std::vector<long> icon(Display *dpy, Window w)
{
    std::vector<long> out;
    Atom type;
    int format;
    unsigned long items, rest;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy, w, XInternAtom(dpy, "_NET_WM_ICON", False), 0, 1 << 16, False, XA_CARDINAL, &type,
                           &format, &items, &rest, &data)
            == Success
        && data && format == 32)
        out.assign(reinterpret_cast<long *>(data), reinterpret_cast<long *>(data) + items);
    if (data)
        XFree(data);
    return out;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QMainWindow main;
    main.resize(400, 300);
    main.show();
    main.activateWindow();
    for (int i = 0; i < 20; ++i)
        app.processEvents();

    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::printf("FAIL: no X display\n");
        return 1;
    }
    // Live's icon on its main window: 2x2 opaque red.
    const long liveIcon[] = {2, 2, long(0xffff0000), long(0xffff0000), long(0xffff0000), long(0xffff0000)};
    XChangeProperty(dpy, main.winId(), XInternAtom(dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(liveIcon), 6);
    XSync(dpy, False);

    Window editor = XCreateWindow(dpy, DefaultRootWindow(dpy), 10, 10, 200, 100, 0, 0, 0, nullptr, 0, nullptr);
    liveOwnPluginWindow(dpy, editor);
    XSync(dpy, False);
    Window owner = 0;
    check(XGetTransientForHint(dpy, editor, &owner) && owner == main.winId(), "editor still belongs to Live's main window");
    const std::vector<long> blank = {1, 1, 0};
    const auto got = icon(dpy, editor);
    check(got.size() >= 2 && !(got[0] == 2 && got[1] == 2), "editor does not carry Live's icon");
    check(got == blank, "editor's icon is one transparent 1x1 pixel");

    XCloseDisplay(dpy);
    std::printf(failures ? "FAIL: %d check(s) failed\n" : "PASS: editor title bars show no Live icon\n", failures);
    return failures ? 1 : 0;
}
