// Where plug-in editor windows open. Live creates its editors at the place it
// last knew for that editor and 0,0 when it knows none, and pluginclose.cpp
// asks the window manager to keep the place Live gives, so a plug-in opened
// for the first time came up in the top-left corner of the screen. Here each
// editor is moved, just before Live shows it, to the place its plug-in's
// window was last left (remembered by plug-in name across tracks and runs,
// in GenericDataLocation/Ableton/Window Positions/plugin-positions.ini), else
// centred over Live's main window; either way kept wholly on the screen.
// Positions are root coordinates of the editor's client area, which the
// window manager keeps as they are (StaticGravity, pluginclose.cpp).
#include "editorplacement.h"
#include "editorcolour.h"
#include "livebuild.h"
#include <QApplication>
#include <QHash>
#include <QPoint>
#include <QSettings>
#include <QSize>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

Display *ownDisplay(); // window.cpp

namespace {

struct Placed {
    unsigned token = 0; // renewed by each ConfigureNotify; a pending write checks it
    QPoint last;        // where this file put the editor or last wrote it down
};
QHash<Window, Placed> placed;

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

QSettings &positions()
{
    static QSettings file(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                              + "/Ableton/Window Positions/plugin-positions.ini",
                          QSettings::IniFormat);
    return file;
}

bool remembered(const QString &plugin, QPoint &at)
{
    const QStringList xy = positions().value(plugin).toString().split(QLatin1Char(','));
    bool okX = false, okY = false;
    if (xy.size() == 2)
        at = QPoint(xy[0].toInt(&okX), xy[1].toInt(&okY));
    return okX && okY;
}

// The editor's client area in root coordinates, if it is shown (call under a Trap).
bool shownAt(Display *dpy, Window editor, QPoint &at)
{
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, editor, &a) || a.map_state != IsViewable)
        return false;
    int x = 0, y = 0;
    Window child;
    if (!XTranslateCoordinates(dpy, editor, a.root, 0, 0, &x, &y, &child))
        return false;
    at = QPoint(x, y);
    return true;
}

// The size the editor is shown at: its plug-in view's (the first mapped
// child), which pluginclose.cpp fitToView has just asked for; the window
// manager may not have applied that yet. The editor's own size without one.
QSize shownSize(Display *dpy, Window editor, const XWindowAttributes &outer)
{
    QSize size(outer.width, outer.height);
    Window root, parent, *kids = nullptr;
    unsigned count = 0;
    if (!XQueryTree(dpy, editor, &root, &parent, &kids, &count))
        return size;
    for (unsigned i = 0; i < count; ++i) {
        XWindowAttributes view;
        if (XGetWindowAttributes(dpy, kids[i], &view) && view.map_state != IsUnmapped && view.c_class != InputOnly) {
            size = QSize(view.width, view.height);
            break;
        }
    }
    if (kids)
        XFree(kids);
    return size;
}

// Writes down where the editor is, if it is shown and has moved since this
// file last placed it or wrote it down.
void record(Display *dpy, Window editor)
{
    auto it = placed.find(editor);
    if (it == placed.end())
        return;
    const QString plugin = editorPluginName(editor);
    if (plugin.isEmpty())
        return;
    QPoint at;
    bool shown;
    {
        Trap trap(dpy);
        shown = shownAt(dpy, editor, at) && !trapped;
    }
    if (!shown || at == it->last)
        return;
    it->last = at;
    positions().setValue(plugin, QString("%1,%2").arg(at.x()).arg(at.y()));
    positions().sync();
}

} // namespace

void editorPlacementMapping(Display *dpy, Window editor)
{
    // Live 11 editors keep the place Live 11 gives them.
    if (!guiThread() || liveBuild() == LiveBuild::Live11_3_43)
        return;
    const QString plugin = editorPluginName(editor);
    if (plugin.isEmpty())
        return;
    Trap trap(dpy);
    XWindowAttributes a, rootSize;
    if (!XGetWindowAttributes(dpy, editor, &a) || !XGetWindowAttributes(dpy, a.root, &rootSize) || trapped)
        return;
    const QSize size = shownSize(dpy, editor, a);
    QPoint at;
    if (!remembered(plugin, at)) {
        // Centred over the window the editor belongs to (Live's main window).
        Window owner = 0;
        XWindowAttributes o;
        int ox = 0, oy = 0;
        Window child;
        if (!XGetTransientForHint(dpy, editor, &owner) || !owner || !XGetWindowAttributes(dpy, owner, &o)
            || !XTranslateCoordinates(dpy, owner, a.root, 0, 0, &ox, &oy, &child) || trapped)
            return;
        at = QPoint(ox + (o.width - size.width()) / 2, oy + (o.height - size.height()) / 2);
    }
    // Wholly on the screen; one larger than the screen keeps its top-left at 0,0.
    at.setX(qBound(0, at.x(), qMax(0, rootSize.width - size.width())));
    at.setY(qBound(0, at.y(), qMax(0, rootSize.height - size.height())));
    XMoveWindow(dpy, editor, at.x(), at.y());
    XSizeHints *hints = XAllocSizeHints();
    long supplied = 0;
    if (hints) {
        XGetWMNormalHints(dpy, editor, hints, &supplied);
        hints->flags |= USPosition | PPosition;
        hints->x = at.x();
        hints->y = at.y();
        XSetWMNormalHints(dpy, editor, hints);
        XFree(hints);
    }
    if (trapped)
        return;
    Placed &p = placed[editor];
    ++p.token; // a write pending from before the editor was hidden is stale
    p.last = at;
}

void editorPlacementResizing(Display *dpy, Window editor, int &x, int &y)
{
    if (!guiThread())
        return;
    auto it = placed.constFind(editor);
    if (it == placed.constEnd())
        return;
    // Not shown yet (the window manager has not carried out the map) or
    // hidden: where this file put it or last wrote it down.
    QPoint at = it->last;
    {
        Trap trap(dpy);
        QPoint now;
        if (shownAt(dpy, editor, now) && !trapped)
            at = now;
    }
    x = at.x();
    y = at.y();
}

void editorPlacementConfigured(Window editor)
{
    if (!guiThread())
        return;
    auto it = placed.find(editor);
    if (it == placed.end())
        return;
    const unsigned token = ++it->token;
    QTimer::singleShot(500, [editor, token] {
        auto it = placed.constFind(editor);
        if (it == placed.constEnd() || it->token != token)
            return;
        if (Display *own = ownDisplay())
            record(own, editor);
    });
}

void editorPlacementSave(Display *dpy, Window editor)
{
    if (!guiThread())
        return;
    auto it = placed.find(editor);
    if (it == placed.end())
        return;
    ++it->token; // supersedes a pending write
    record(dpy, editor);
}

void editorPlacementForget(Window editor)
{
    if (guiThread())
        placed.remove(editor);
}
