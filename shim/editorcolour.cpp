// Plug-in editor title bars in the plug-in's own colour. Live's plug-in
// editors are plain X windows with the plug-in's view inside; KDE draws their
// title bars in its default colours. A little after an editor is shown or
// resized, the colour along the top edge of the plug-in's view is read back
// from the X server and the editor is given a KDE colour scheme whose title
// bar is that colour (the property _KDE_NET_WM_COLOR_SCHEME names the scheme
// file, as window.cpp does for Live's own windows).
//
// The read targets the innermost window under the editor's top centre (the
// plug-in's own drawing surface), at that window's depth and visual: X does
// not define the contents of child windows of another depth. A top edge that
// is all black reads as not drawn yet, and a read that fails (the window went
// away, is not shown) changes nothing: the editor keeps its last colour, or
// KDE's default. Every X call here is made under an error trap, so a window
// destroyed between a timer and its read cannot end Live.
#include "editorcolour.h"
#include "chrome.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

Display *ownDisplay(); // window.cpp
void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

namespace {

struct Tracked {
    unsigned generation = 0; // a new one per editor window: timers of an earlier one do nothing
    unsigned resizes = 0;    // the latest size change: only the read after the last one runs
    int width = 0, height = 0; // the size the latest ConfigureNotify gave
    QString plugin;            // the plug-in's name, from the editor's title
    bool found = false;        // a colour was read since the editor was last shown
    unsigned shown = 0;        // the latest map: reads after an earlier map do nothing
    QElapsedTimer mapped;      // since the latest map: polls stop at kLateRead
};
QHash<Window, Tracked> tracked;
QHash<Window, QString> applied; // the scheme file each editor names
unsigned generations;

// After the map: a read every kPoll ms until the first colour, until kLateRead
// ms; one at kConfirmRead if a poll found a colour before it (a startup frame
// replaced at once) and one at kLateRead (a plug-in showing a splash first).
constexpr int kPoll = 50, kConfirmRead = 300, kLateRead = 1500;
constexpr int kResizeRead = 400;                   // ms after the last size change
// Rows read from the top of the view: deep enough that a thin outline some
// plug-ins draw around their view (Serum 2: 2 rows, near black) is outvoted
// by the header under it.
constexpr int kBandRows = 16;

bool guiThread()
{
    return qApp && QThread::currentThread() == qApp->thread();
}

bool trapped;

// X errors on the calls made while one of these lives set `trapped` instead
// of reaching the process's handler (which would end Live).
struct Trap {
    Display *dpy;
    XErrorHandler previous;
    explicit Trap(Display *d) : dpy(d)
    {
        XSync(dpy, False);
        trapped = false;
        previous = XSetErrorHandler([](Display *, XErrorEvent *) {
            trapped = true;
            return 0;
        });
    }
    ~Trap()
    {
        XSync(dpy, False);
        XSetErrorHandler(previous);
    }
};

QString colourFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Ableton/Window Colors";
}

// Colours read before, by plug-in name ("#rrggbb"), kept across sessions.
QSettings &remembered()
{
    static QSettings file(colourFolder() + "/plugin-colours.ini", QSettings::IniFormat);
    return file;
}

QColor rememberedColour(const QString &plugin)
{
    return plugin.isEmpty() ? QColor() : QColor(remembered().value(plugin).toString());
}

void remember(const QString &plugin, const QColor &band)
{
    if (plugin.isEmpty() || rememberedColour(plugin) == band)
        return;
    remembered().setValue(plugin, band.name());
    remembered().sync();
}

// Gives `editor` a title bar in `band` (call under a Trap).
bool giveColour(Display *dpy, Window editor, const QColor &band)
{
    const QString path = chrome::writeColorScheme(colourFolder(), band, chrome::titleTextFor(band),
                                                  QStringLiteral("plugin"), applied.values());
    if (path.isEmpty())
        return false;
    if (applied.value(editor) != path) {
        const QByteArray name = QFile::encodeName(path);
        XChangeProperty(dpy, editor, XInternAtom(dpy, "_KDE_NET_WM_COLOR_SCHEME", False), XA_STRING, 8,
                        PropModeReplace, reinterpret_cast<const unsigned char *>(name.constData()), name.size());
        XSync(dpy, False);
    }
    if (trapped)
        return false;
    applied.insert(editor, path);
    return true;
}

// The remembered colour of the editor's plug-in, if there is one.
void giveRemembered(Display *dpy, Window editor)
{
    auto it = tracked.constFind(editor);
    if (it == tracked.constEnd())
        return;
    const QColor band = rememberedColour(it->plugin);
    if (!band.isValid())
        return;
    bool done;
    {
        Trap trap(dpy);
        done = giveColour(dpy, editor, band);
    }
    if (done)
        menufixTrace("editorcolour: editor 0x%x remembered title bar %06x\n", int(editor), int(band.rgb() & 0xffffff));
}

// One 8-bit channel of `pixel` under `mask`.
int channel(unsigned long pixel, unsigned long mask)
{
    if (!mask)
        return 0;
    const int shift = __builtin_ctzl(mask);
    const unsigned long max = mask >> shift;
    return int(((pixel & mask) >> shift) * 255 / max);
}

// The colour along the top edge of `editor`'s plug-in view; invalid when it
// cannot be read. Call under the error trap.
QColor readTopEdge(Display *dpy, Window editor)
{
    XWindowAttributes outer;
    if (!XGetWindowAttributes(dpy, editor, &outer) || trapped || outer.map_state != IsViewable || outer.width < 1)
        return {};
    // The innermost viewable window under the top centre, and its offset
    // from the editor.
    Window target = editor;
    XWindowAttributes at = outer;
    int ox = 0, oy = 0;
    const int px = outer.width / 2;
    for (int depth = 0; depth < 32; ++depth) {
        Window root, parent, *kids = nullptr;
        unsigned count = 0;
        if (!XQueryTree(dpy, target, &root, &parent, &kids, &count) || trapped)
            return {};
        Window next = 0;
        XWindowAttributes nextAt;
        for (unsigned i = count; i-- > 0;) { // top of the stack first
            XWindowAttributes a;
            if (!XGetWindowAttributes(dpy, kids[i], &a) || trapped)
                continue;
            if (a.map_state != IsViewable || a.c_class == InputOnly)
                continue;
            const int x = ox + a.x + a.border_width, y = oy + a.y + a.border_width;
            if (px >= x && px < x + a.width && 0 >= y && 0 < y + a.height) {
                next = kids[i];
                nextAt = a;
                break;
            }
        }
        if (kids)
            XFree(kids);
        if (!next)
            break;
        ox += nextAt.x + nextAt.border_width;
        oy += nextAt.y + nextAt.border_width;
        target = next;
        at = nextAt;
    }
    if (!at.visual || (at.visual->c_class != TrueColor && at.visual->c_class != DirectColor))
        return {};
    // The target's top kBandRows rows inside the editor, and on the screen (X
    // refuses a read of any part outside it).
    int rootX = 0, rootY = 0;
    Window child;
    if (!XTranslateCoordinates(dpy, target, at.root, 0, 0, &rootX, &rootY, &child) || trapped)
        return {};
    // The root's current size (the Screen fields keep the size at connection
    // time; a screen added since makes the root larger).
    Window rootReturn;
    int rx, ry;
    unsigned rootWidth, rootHeight, border, rootDepth;
    if (!XGetGeometry(dpy, at.root, &rootReturn, &rx, &ry, &rootWidth, &rootHeight, &border, &rootDepth) || trapped)
        return {};
    const int screenWidth = int(rootWidth), screenHeight = int(rootHeight);
    const int top = -oy;
    // Rows from the editor's top edge: inside the view, the editor and the screen.
    const int rows = std::min({kBandRows, at.height - top, outer.height, screenHeight - (rootY + top)});
    const int left = std::max({0, -ox, -rootX});
    const int right = std::min({at.width, outer.width - ox, screenWidth - rootX});
    if (rows < 1 || right <= left || rootY + top < 0)
        return {};
    XImage *image = XGetImage(dpy, target, left, top, unsigned(right - left), unsigned(rows), AllPlanes, ZPixmap);
    if (!image || trapped) {
        if (image)
            XDestroyImage(image);
        return {};
    }
    QImage strip(right - left, rows, QImage::Format_RGB32);
    bool drawn = false; // any pixel not black: all black is a view not drawn yet
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < right - left; ++x) {
            const unsigned long p = XGetPixel(image, x, y);
            const QRgb c = qRgb(channel(p, at.visual->red_mask), channel(p, at.visual->green_mask),
                                channel(p, at.visual->blue_mask));
            drawn = drawn || (c & 0xffffff) != 0;
            strip.setPixel(x, y, c);
        }
    XDestroyImage(image);
    return drawn ? chrome::dominantColour(strip) : QColor();
}

// The editor is still the same window and was not shown again since `shown`.
bool sameShow(Window editor, unsigned generation, unsigned shown)
{
    auto it = tracked.constFind(editor);
    return it != tracked.constEnd() && it->generation == generation && it->shown == shown;
}

// A read `ms` after a map, if the editor was not shown again since; with
// `onlyIfFound`, only if a poll has already found a colour (else the polls
// are still reading).
void readAfterMap(Window editor, int ms, unsigned generation, unsigned shown, bool onlyIfFound)
{
    QTimer::singleShot(ms, qApp, [editor, generation, shown, onlyIfFound] {
        if (!sameShow(editor, generation, shown) || (onlyIfFound && !tracked.value(editor).found))
            return;
        if (Display *dpy = ownDisplay())
            editorColourApply(dpy, editor);
    });
}

// A read kResizeRead ms after a size change, if no later one came.
void readAfterResize(Window editor, unsigned generation, unsigned resizes)
{
    QTimer::singleShot(kResizeRead, qApp, [editor, generation, resizes] {
        auto it = tracked.constFind(editor);
        if (it == tracked.constEnd() || it->generation != generation || it->resizes != resizes)
            return;
        if (Display *dpy = ownDisplay())
            editorColourApply(dpy, editor);
    });
}

// Reads every kPoll ms until the editor's first colour since it was shown,
// until kLateRead ms after the map (the late read takes over there), however
// late Live's GUI thread runs the timers.
void pollLater(Window editor, unsigned generation, unsigned shown)
{
    QTimer::singleShot(kPoll, qApp, [editor, generation, shown] {
        if (!sameShow(editor, generation, shown))
            return;
        const Tracked &t = tracked[editor];
        if (t.found || t.mapped.elapsed() >= kLateRead)
            return;
        Display *dpy = ownDisplay();
        if (dpy && editorColourApply(dpy, editor))
            return;
        if (tracked.value(editor).mapped.elapsed() + kPoll < kLateRead)
            pollLater(editor, generation, shown);
    });
}

} // namespace

bool editorColourApply(Display *dpy, Window editor)
{
    QColor band;
    bool done = false;
    {
        Trap trap(dpy);
        band = readTopEdge(dpy, editor);
        XSync(dpy, False);
        if (band.isValid() && !trapped)
            done = giveColour(dpy, editor, band);
    }
    if (!done)
        return false;
    auto it = tracked.find(editor);
    if (it != tracked.end()) {
        it->found = true;
        remember(it->plugin, band);
    }
    menufixTrace("editorcolour: editor 0x%x title bar %06x\n", int(editor), int(band.rgb() & 0xffffff));
    return true;
}

void editorColourTrack(Window editor)
{
    if (!guiThread())
        return;
    Tracked fresh;
    fresh.generation = ++generations;
    tracked.insert(editor, fresh);
    applied.remove(editor);
}

void editorColourNamed(Window editor, const char *title)
{
    if (!guiThread() || !title)
        return;
    auto it = tracked.find(editor);
    if (it == tracked.end())
        return;
    it->plugin = QString::fromUtf8(title).section(QLatin1Char('/'), 0, 0).trimmed();
    menufixTrace("editorcolour: editor 0x%x named\n", int(editor));
    // The editor exists on the server by now: liveOwnPluginWindow (window.cpp)
    // made a round trip on Live's connection after creating it.
    if (Display *own = ownDisplay())
        giveRemembered(own, editor);
}

void editorColourMapped(Window editor)
{
    if (!guiThread())
        return;
    auto it = tracked.find(editor);
    if (it == tracked.end())
        return;
    it->found = false;
    it->mapped.start();
    const unsigned generation = it->generation, shown = ++it->shown;
    if (!applied.contains(editor))
        if (Display *own = ownDisplay())
            giveRemembered(own, editor);
    pollLater(editor, generation, shown);
    readAfterMap(editor, kConfirmRead, generation, shown, true);
    readAfterMap(editor, kLateRead, generation, shown, false);
}

void editorColourResized(Window editor)
{
    if (!guiThread())
        return;
    auto it = tracked.find(editor);
    if (it == tracked.end())
        return;
    ++it->resizes;
    readAfterResize(editor, it->generation, it->resizes);
}

void editorColourConfigured(Window editor, int width, int height)
{
    if (!guiThread())
        return;
    auto it = tracked.find(editor);
    if (it == tracked.end() || (it->width == width && it->height == height))
        return;
    it->width = width;
    it->height = height;
    editorColourResized(editor);
}

QString editorPluginName(Window editor)
{
    return guiThread() ? tracked.value(editor).plugin : QString();
}

bool editorTracked(Window editor)
{
    return guiThread() && tracked.contains(editor);
}

void editorColourForget(Window editor)
{
    if (!guiThread())
        return;
    tracked.remove(editor);
    applied.remove(editor);
}
