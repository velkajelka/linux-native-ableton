// Window chrome that follows Live's theme: the menu bar's drop-down menus, the
// title bar colour scheme and the window edge fill. No Live dependencies, so
// tests/theme-chrome.sh builds and checks it on its own.
#pragma once
#include <QColor>
#include <QProxyStyle>
#include <QString>

class QImage;
class QMenu;
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

// Colours and scale of the drop-down menus.
struct MenuLook {
    QColor surface{0x22, 0x22, 0x22}, text{Qt::white}, shortcut{0xa0, 0xa0, 0xa0}, disabled{0x80, 0x80, 0x80};
    QColor hover{0x3c, 0x3c, 0x3c};
    double unit = 1;     // Live's band height / 24: sizes below are in these units
    int fontPixels = 12; // menu text size in the fallback font
};

// The look for Live's menu colour, text and disabled-text colours and its band
// height in pixels (24 at 100 %). Text gets at least 7:1 against the menu,
// shortcuts 4.5:1 and disabled text 3.5:1.
MenuLook menuLook(const QColor &surface, const QColor &text, const QColor &disabled, int bandHeight);

// Drop-down menus as Live draws its own: a square, opaque body with a 1-unit
// frame in the text colour, 16-unit rows, a square hover plate across the
// row, a 16-unit tick column every row has, the label, a dimmer shortcut after
// the tab ending 5 units from the frame, the submenu arrow, and separators of
// one line in the frame colour across the menu.
class MenuStyle : public QProxyStyle
{
public:
    MenuStyle();
    MenuLook look;

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr,
                    const QWidget *widget = nullptr) const override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget = nullptr) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size,
                           const QWidget *widget = nullptr) const override;

private:
    int px(double units) const;
};

// Adds the font file at `path` to the application's fonts and returns its
// family; empty when the file is missing or not a font (the caller keeps its
// font then). Needs the QGuiApplication.
QString loadFontFamily(const QString &path);

// Places a submenu as it is shown with its first row level with the row that
// opens it, also when the submenu gets its rows only as it opens (Qt then
// places it by that row's top).
void placeSubmenus(QMenu *menu);

// A KDE colour scheme whose window title bar takes `band` with `text` on it:
// [Colors:Header], [Colors:Window] and [WM].
QString colorSchemeText(const QColor &band, const QColor &text);

// Writes the scheme into `dir` under a name made of the two colours and
// returns its absolute path (empty when it cannot be written). The window
// manager keeps a scheme by its path, so each colour set has its own file. Of
// the files written here, the current and the previous one are kept.
QString writeColorScheme(const QString &dir, const QColor &band, const QColor &text);

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
