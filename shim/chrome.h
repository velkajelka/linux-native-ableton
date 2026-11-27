// Window chrome: the title bar colour scheme (plug-in editors) and the window
// edge fill. No Live dependencies, so
// tests/theme-chrome.sh builds and checks it on its own.
#pragma once
#include <QColor>
#include <QString>
#include <QStringList>

class QImage;
class QPainter;
class QSize;
class QWidget;

namespace chrome {

// WCAG contrast ratio, 1 (same) to 21 (black on white).
double contrast(const QColor &a, const QColor &b);
QColor mix(const QColor &a, const QColor &b, double t);
// Black or white, whichever stands out more on `back`.
QColor extreme(const QColor &back);
// `c`, moved towards `towards` just far enough to reach `ratio` against `back`.
QColor withContrast(const QColor &c, const QColor &back, const QColor &towards, double ratio);

// A KDE colour scheme whose window title bar takes `band` with `text` on it:
// [Colors:Header], [Colors:Window] and [WM].
QString colorSchemeText(const QColor &band, const QColor &text);

// Writes the scheme into `dir` as `<prefix>-<band>-<text>.colors` and returns
// its absolute path (empty when it cannot be written). The window manager
// keeps a scheme by its path, so each colour set has its own file. For
// "live", of the files written here the current and the previous one are
// kept; for another prefix, the files with that prefix in `keep` and the one
// written now.
QString writeColorScheme(const QString &dir, const QColor &band, const QColor &text,
                         const QString &prefix = QStringLiteral("live"), const QStringList &keep = {});

// The most common colour in `image` (invalid when it is empty).
QColor dominantColour(const QImage &image);
// Title text for a title bar in `band`: a near-black or a near-white,
// whichever stands out more.
QColor titleTextFor(const QColor &band);

// A widget's size in device pixels at `ratio`, rounded as Qt sizes its
// backing store.
QSize deviceSize(const QSize &logical, qreal ratio);
// The size of `view` in device pixels, as its window's backing store has it.
QSize deviceSize(const QWidget *view);

// Live's frame image can be smaller than the view it is drawn into at a
// fractional scale. Fills the strips of the view (`view`, device pixels) right
// of and below `image` (drawn at the origin, one image pixel per device pixel)
// with the image's last column and row. Draws nothing when the image covers
// the view.
void fillUncovered(QPainter &painter, const QSize &view, const QImage &image);

} // namespace chrome
