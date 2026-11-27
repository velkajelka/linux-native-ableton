// Checks for shim/chrome.cpp, run offscreen by tests/theme-chrome.sh: the
// drop-down menus, the title bar scheme and the edge fill. Every expected
// value is a literal (Live's own menu, measured:
// a square body with one 1-unit frame in the text colour, 16-unit rows, the
// label 16 units inside the frame, shortcuts ending about 5 units from it,
// hover 13 % of the text colour over the menu colour), or is measured in the
// rendered image.
#include "chrome.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QImage>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QTextStream>
#include <cmath>
#include <cstdio>

using namespace chrome;

static int failures;

static void check(const QString &what, bool ok)
{
    printf("%s %s\n", ok ? "  ok  " : "FAIL:", qPrintable(what));
    if (!ok)
        ++failures;
}

static bool near(QRgb a, const QColor &b, int tolerance)
{
    return qAbs(qRed(a) - b.red()) <= tolerance && qAbs(qGreen(a) - b.green()) <= tolerance
           && qAbs(qBlue(a) - b.blue()) <= tolerance;
}

static double lum(QRgb c)
{
    auto ch = [](int v) {
        const double x = v / 255.0;
        return x <= 0.03928 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * ch(qRed(c)) + 0.7152 * ch(qGreen(c)) + 0.0722 * ch(qBlue(c));
}

static double ratio(QRgb a, QRgb b)
{
    const double la = lum(a), lb = lum(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

struct Theme {
    const char *name;
    QColor surface, text, disabled, hover; // hover: worked out by hand from the mockup's 13 %
};

struct Rendered {
    QImage image; // ARGB32, not premultiplied
    QRect body;   // measured: the outermost fully opaque pixels along the middle row and column
    QRect row0, row1, row3;
};

// A menu as the shim builds one: a child of a menu bar that has a QMenuBar-only
// style sheet, with the shared style. Row 0 has a shortcut, row 1 is hovered
// (`hover` 1; 0 hovers row 0 instead), row 2 is a separator, row 3 is ticked
// (ticks) or plain. In `family` at 10 px when given (Live's menu font, as the
// shim sets it), else the default font at 12 px; `pixels` overrides the size.
static Rendered render(MenuStyle &style, QMenuBar &bar, bool ticks, int hover = 1, const QString &family = QString(),
                       int pixels = 0)
{
    QMenu menu("Test", &bar);
    menu.setStyle(&style);
    QFont font = menu.font();
    if (!family.isEmpty())
        font.setFamily(family);
    font.setPixelSize(pixels ? pixels : family.isEmpty() ? 12 : 10);
    menu.setFont(font);
    QAction *a0 = menu.addAction("HH\tCtrl+Shift+F9");
    QAction *a1 = menu.addAction("HHHH");
    menu.addSeparator();
    QAction *a3 = menu.addAction(ticks ? "Ticked" : "Plain");
    if (ticks) {
        a3->setCheckable(true);
        a3->setChecked(true);
    }
    menu.popup(QPoint(200, 200));
    QApplication::processEvents();
    menu.setActiveAction(hover ? a1 : a0);
    QApplication::processEvents();
    Rendered r;
    r.image = menu.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    r.row0 = menu.actionGeometry(a0);
    r.row1 = menu.actionGeometry(a1);
    r.row3 = menu.actionGeometry(a3);
    menu.hide();
    const QImage &img = r.image;
    const int midY = r.row1.center().y(), midX = img.width() / 2;
    int left = -1, right = -1, top = -1, bottom = -1;
    for (int x = 0; x < img.width(); ++x)
        if (qAlpha(img.pixel(x, midY)) == 255) {
            if (left < 0)
                left = x;
            right = x;
        }
    for (int y = 0; y < img.height(); ++y)
        if (qAlpha(img.pixel(midX, y)) == 255) {
            if (top < 0)
                top = y;
            bottom = y;
        }
    r.body = QRect(QPoint(left, top), QPoint(right, bottom));
    return r;
}

// Highest contrast against `back` of any pixel in `area` of the image.
static double strongest(const QImage &img, const QRect &area, const QColor &back)
{
    double best = 1;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            best = qMax(best, ratio(img.pixel(x, y), back.rgb()));
    return best;
}

static bool hasColour(const QImage &img, const QRect &area, const QColor &c, int tolerance)
{
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x)
            if (near(img.pixel(x, y), c, tolerance))
                return true;
    return false;
}

// x of the first pixel in `row` (from the body's inner edge) that differs from `back`.
static int firstInk(const QImage &img, const QRect &body, const QRect &row, const QColor &back)
{
    for (int x = body.left() + 1; x < body.right(); ++x)
        for (int y = row.top(); y <= row.bottom(); ++y)
            if (!near(img.pixel(x, y), back, 40))
                return x;
    return -1;
}

// x of the last pixel in `row` (towards the body's inner edge) that differs from `back`.
static int lastInk(const QImage &img, const QRect &body, const QRect &row, const QColor &back)
{
    for (int x = body.right() - 1; x > body.left(); --x)
        for (int y = row.top(); y <= row.bottom(); ++y)
            if (!near(img.pixel(x, y), back, 40))
                return x;
    return -1;
}

static void menus()
{
    const Theme themes[] = {
        {"dark", QColor(0x22, 0x22, 0x22), QColor(0xea, 0xea, 0xea), QColor(0x80, 0x80, 0x80), QColor(0x3c, 0x3c, 0x3c)},
        {"light", QColor(0xdf, 0xdf, 0xdf), QColor(0x12, 0x12, 0x12), QColor(0x70, 0x70, 0x70), QColor(0xc4, 0xc4, 0xc4)},
    };
    QMenuBar bar;
    bar.setStyleSheet("QMenuBar { background: #3c3c3c; color: #e0e0e0; }");
    for (const Theme &t : themes) {
        MenuStyle style;
        style.look = menuLook(t.surface, t.text, t.disabled, 24);
        const Rendered r = render(style, bar, true);
        const QImage &img = r.image;
        const QRect b = r.body;
        const QString n = QString(t.name) + ": ";
        if (b.width() < 50 || b.height() < 50) {
            check(n + "menu body found in the rendered image", false);
            continue;
        }
        // Square, opaque, nothing outside the body.
        check(n + QString("body fills the window (%1x%2 at %3,%4 in %5x%6)")
                      .arg(b.width()).arg(b.height()).arg(b.left()).arg(b.top()).arg(img.width()).arg(img.height()),
              b == img.rect());
        bool corners = true;
        for (const QPoint &c : {b.topLeft(), b.topRight(), b.bottomLeft(), b.bottomRight()})
            corners = corners && qAlpha(img.pixel(c)) == 255 && near(img.pixel(c), t.text, 2);
        check(n + "corner pixels opaque, in the frame colour (square)", corners);
        // One 1-unit frame in the text colour, the menu colour right inside it.
        const int y0 = r.row0.center().y(), y1 = r.row1.center().y();
        check(n + "frame in the text colour on all four sides",
              near(img.pixel(b.left(), y0), t.text, 2) && near(img.pixel(b.right(), y0), t.text, 2)
                  && near(img.pixel(b.center().x(), b.top()), t.text, 2)
                  && near(img.pixel(b.center().x(), b.bottom()), t.text, 2));
        check(n + "menu colour right inside the frame (one line)",
              near(img.pixel(b.left() + 1, y0), t.surface, 1) && near(img.pixel(b.right() - 1, y0), t.surface, 1));
        // Separator, one line in the frame colour across the full inner width.
        int sepLines = 0;
        bool sepFull = false;
        for (int y = r.row1.bottom() + 1; y < r.row3.top(); ++y) {
            bool all = true;
            for (int x = b.left() + 1; x < b.right(); ++x)
                all = all && near(img.pixel(x, y), t.text, 2);
            if (all) {
                ++sepLines;
                sepFull = true;
            }
        }
        check(n + QString("separator: one line in the frame colour across the inner width (%1 lines)").arg(sepLines),
              sepFull && sepLines == 1);
        // Hover plate square across the full inner width of its row.
        check(n + "hover plate colour " + t.hover.name(), near(img.pixel(b.center().x(), r.row1.top() + 2), t.hover, 2));
        check(n + "hover plate from the left frame to the right frame, its full row height",
              near(img.pixel(b.left() + 1, r.row1.top()), t.hover, 2) && near(img.pixel(b.right() - 1, r.row1.top()), t.hover, 2)
                  && near(img.pixel(b.left() + 1, r.row1.bottom()), t.hover, 2)
                  && near(img.pixel(b.right() - 1, r.row1.bottom()), t.hover, 2)
                  && !near(img.pixel(b.left() + 1, r.row1.top() - 1), t.hover, 2)
                  && !near(img.pixel(b.left() + 1, r.row1.bottom() + 1), t.hover, 2));
        // 16-unit rows.
        check(n + QString("rows 16 px (%1, %2, %3)").arg(r.row0.height()).arg(r.row1.height()).arg(r.row3.height()),
              r.row0.height() == 16 && r.row1.height() == 16 && r.row3.height() == 16);
        // Text colour.
        const QRect labelArea(b.left() + 1, r.row1.top(), b.width() / 3, r.row1.height());
        check(n + "label text in " + t.text.name(), hasColour(img, labelArea, t.text, 30));
        // Shortcut dimmer than its label (row 0: short label, long shortcut).
        const QRect label0(b.left() + 1, r.row0.top(), b.width() / 3, r.row0.height());
        const QRect shortcut0(b.right() - b.width() / 3, r.row0.top(), b.width() / 3 - 1, r.row0.height());
        const double l = strongest(img, label0, t.surface), s = strongest(img, shortcut0, t.surface);
        check(n + QString("shortcut dimmer than label (contrast %1 vs %2)").arg(s, 0, 'f', 1).arg(l, 0, 'f', 1),
              s > 1.5 && s * 1.3 < l);
        // The shortcut ends about 5 units from the frame.
        const int end0 = lastInk(img, b, r.row0, t.surface);
        check(n + QString("shortcut ends %1 px from the frame (about 5)").arg(b.right() - end0), b.right() - end0 >= 4 && b.right() - end0 <= 7);
        // The same label x with and without ticks, 16 units inside the frame.
        const Rendered plain = render(style, bar, false);
        const int xTicks = firstInk(img, b, r.row1, t.hover);
        const int xPlain = firstInk(plain.image, plain.body, plain.row1, t.hover);
        check(n + QString("label x %1 with ticks, %2 without (16 to 18)").arg(xTicks - b.left()).arg(xPlain - plain.body.left()),
              xTicks - b.left() == xPlain - plain.body.left() && xTicks - b.left() >= 16 && xTicks - b.left() <= 18);
        const int xTick = firstInk(img, b, r.row3, t.surface);
        check(n + "tick drawn in the column before the label", xTick > b.left() + 1 && xTick < xTicks);
        // Moving the hover changes neither the menu's size nor its rows.
        const Rendered other = render(style, bar, true, 0);
        check(n + "same size and rows with the hover on another row",
              other.image.size() == img.size() && other.row0 == r.row0 && other.row1 == r.row1 && other.row3 == r.row3
                  && near(other.image.pixel(b.left() + 1, r.row0.top()), t.hover, 2)
                  && near(other.image.pixel(b.left() + 1, r.row1.top()), t.surface, 1));
        // In one open menu, moving the selection to the next row repaints
        // both rows at once (no plate left behind or on its way) and keeps
        // every size; grabbed straight after the move, before any event runs.
        {
            QMenu menu("Test", &bar);
            menu.setStyle(&style);
            QFont font = menu.font();
            font.setPixelSize(12);
            menu.setFont(font);
            QAction *m0 = menu.addAction("HH\tCtrl+Shift+F9");
            QAction *m1 = menu.addAction("HHHH");
            menu.popup(QPoint(200, 200));
            QApplication::processEvents();
            menu.setActiveAction(m0);
            QApplication::processEvents();
            const QImage before = menu.grab().toImage().convertToFormat(QImage::Format_ARGB32);
            const QRect g0 = menu.actionGeometry(m0), g1 = menu.actionGeometry(m1);
            menu.setActiveAction(m1);
            const QImage after = menu.grab().toImage().convertToFormat(QImage::Format_ARGB32);
            const int x0 = 1, x1 = after.width() - 2;
            check(n + "selection moved in one open menu: same sizes, old row plain, new row's plate across its full row",
                  after.size() == before.size() && menu.actionGeometry(m0) == g0 && menu.actionGeometry(m1) == g1
                      && near(before.pixel(x0, g0.top()), t.hover, 2)
                      && near(after.pixel(x0, g0.top()), t.surface, 1) && near(after.pixel(x1, g0.bottom()), t.surface, 1)
                      && near(after.pixel(x0, g1.top()), t.hover, 2) && near(after.pixel(x1, g1.bottom()), t.hover, 2));
            menu.hide();
        }
    }
    // Rows stay 16 units in a font whose line is taller than that (a
    // font's line height differs between font engines).
    {
        QFont probe = QApplication::font();
        int tall = 0;
        for (int p = 12; p <= 30 && !tall; ++p) {
            probe.setPixelSize(p);
            if (QFontMetrics(probe).height() > 16)
                tall = p;
        }
        check(QString("a default font size with a line taller than 16 px (%1 px)").arg(tall), tall > 0);
        MenuStyle style;
        style.look = menuLook(QColor(0xdf, 0xdf, 0xdf), QColor(0x12, 0x12, 0x12), QColor(0x70, 0x70, 0x70), 24);
        const Rendered r = render(style, bar, true, 1, QString(), tall);
        check(QString("rows 16 px in a %1 px font (%2, %3, %4)").arg(tall).arg(r.row0.height()).arg(r.row1.height()).arg(r.row3.height()),
              r.row0.height() == 16 && r.row1.height() == 16 && r.row3.height() == 16);
    }
}

// Live's menu font loads from the installed Live (`liveFont`, read in
// place, empty when no Live is installed); a missing file or one that is not a
// font gives no family, so the menus keep their font.
static void fonts(const QString &tmp, const QString &liveFont)
{
    check("missing font file: no family", loadFontFamily(tmp + "/no-such-font.otf").isEmpty());
    QFile bad(tmp + "/not-a-font.otf");
    bad.open(QIODevice::WriteOnly);
    bad.write("this is not a font\n");
    bad.close();
    check("a file that is not a font: no family", loadFontFamily(bad.fileName()).isEmpty());
    if (liveFont.isEmpty() || !QFile::exists(liveFont)) {
        printf("  skip no installed Live font\n");
        return;
    }
    const QString family = loadFontFamily(liveFont);
    check("Live's menu font loads as \"" + family + "\" (Ableton Sans Small)", family == "Ableton Sans Small");
    // Rows stay 16 units in Live's menu font (its line height is taller).
    QMenuBar bar;
    bar.setStyleSheet("QMenuBar { background: #3c3c3c; color: #e0e0e0; }");
    MenuStyle style;
    style.look = menuLook(QColor(0xdf, 0xdf, 0xdf), QColor(0x12, 0x12, 0x12), QColor(0x70, 0x70, 0x70), 24);
    const Rendered r = render(style, bar, true, 1, family);
    check(QString("rows 16 px in Live's menu font (%1, %2, %3)").arg(r.row0.height()).arg(r.row1.height()).arg(r.row3.height()),
          r.row0.height() == 16 && r.row1.height() == 16 && r.row3.height() == 16);
}

static QMap<QString, QMap<QString, QString>> readIni(const QString &path)
{
    QMap<QString, QMap<QString, QString>> groups;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return groups;
    QString group;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith('[') && line.endsWith(']'))
            group = line.mid(1, line.size() - 2);
        else if (line.contains('='))
            groups[group][line.section('=', 0, 0)] = line.section('=', 1);
    }
    return groups;
}

static void schemes(const QString &tmp)
{
    const QString dir = tmp + "/Window Colors";
    QDir().mkpath(dir);
    QFile other(dir + "/other.colors");
    other.open(QIODevice::WriteOnly);
    other.close();
    const QString p1 = writeColorScheme(dir, QColor(0x3c, 0x3c, 0x3c), QColor(0xe0, 0xe0, 0xe0));
    check("scheme written as an absolute path in the folder", QFileInfo(p1).isAbsolute() && QFileInfo(p1).dir() == QDir(dir) && QFile::exists(p1));
    const auto ini = readIni(p1);
    bool entries = true;
    for (const char *g : {"Colors:Header", "Colors:Window"})
        entries = entries && ini[g]["BackgroundNormal"] == "60,60,60" && ini[g]["ForegroundNormal"] == "224,224,224";
    entries = entries && ini["WM"]["activeBackground"] == "60,60,60" && ini["WM"]["activeForeground"] == "224,224,224"
              && ini["WM"]["inactiveBackground"] == "60,60,60" && ini["WM"]["inactiveForeground"] == "224,224,224";
    check("Header, Window and WM entries carry the band and text colours", entries);
    check("same colours, same file", writeColorScheme(dir, QColor(0x3c, 0x3c, 0x3c), QColor(0xe0, 0xe0, 0xe0)) == p1);
    const QString p2 = writeColorScheme(dir, QColor(0x11, 0x22, 0x33), QColor(0xff, 0xee, 0xdd));
    const QString p3 = writeColorScheme(dir, QColor(0x44, 0x55, 0x66), QColor(0, 0, 0));
    check("a new colour set gets a new file name", !p2.isEmpty() && !p3.isEmpty() && p2 != p1 && p3 != p2 && p3 != p1);
    QStringList left = QDir(dir).entryList({"*.colors"}, QDir::Files);
    left.removeAll("other.colors");
    check(QString("at most two scheme files left (%1), the newest two").arg(left.join(", ")),
          left.size() == 2 && left.contains(QFileInfo(p2).fileName()) && left.contains(QFileInfo(p3).fileName()));
    check("other files in the folder are kept", QFile::exists(dir + "/other.colors"));
    check("unwritable folder: no path", writeColorScheme("/proc/no-such-folder", Qt::black, Qt::white).isEmpty());
}

static void edges()
{
    QImage frame(10, 8, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 10; ++x)
            frame.setPixel(x, y, qRgb(x * 20, y * 30, 100));
    const QRgb background = qRgb(0xef, 0xef, 0xef);
    for (const double dpr : {1.0, 1.325}) {
        const QString n = QString("scale %1: ").arg(dpr);
        // Gap: the view is 12x10 device pixels, Live's frame 10x8.
        QImage canvas(12, 10, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(background);
        canvas.setDevicePixelRatio(dpr);
        QImage drawn = frame;
        drawn.setDevicePixelRatio(dpr);
        {
            QPainter p(&canvas);
            p.drawImage(QPointF(0, 0), drawn);
            fillUncovered(p, QSize(12, 10), frame);
        }
        bool right = true, below = true, corner = true;
        for (int y = 0; y < 8; ++y)
            for (int x = 10; x < 12; ++x)
                right = right && canvas.pixel(x, y) == frame.pixel(9, y);
        for (int y = 8; y < 10; ++y)
            for (int x = 0; x < 10; ++x)
                below = below && canvas.pixel(x, y) == frame.pixel(x, 7);
        for (int y = 8; y < 10; ++y)
            for (int x = 10; x < 12; ++x)
                corner = corner && canvas.pixel(x, y) == frame.pixel(9, 7);
        check(n + "strip right of the frame repeats its last column", right);
        check(n + "strip below the frame repeats its last row", below);
        check(n + "corner takes the frame's corner pixel", corner);
        // No gap: nothing is drawn.
        for (const QSize view : {QSize(10, 8), QSize(9, 7)}) {
            QImage untouched(12, 10, QImage::Format_ARGB32_Premultiplied);
            untouched.fill(background);
            untouched.setDevicePixelRatio(dpr);
            {
                QPainter p(&untouched);
                fillUncovered(p, view, frame);
            }
            bool same = true;
            for (int y = 0; y < 10; ++y)
                for (int x = 0; x < 12; ++x)
                    same = same && untouched.pixel(x, y) == background;
            check(n + QString("view %1x%2 covered by the frame: nothing drawn").arg(view.width()).arg(view.height()), same);
        }
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (argc < 2) {
        printf("usage: theme-chrome <temp folder> [Live's menu font file]\n");
        return 2;
    }
    menus();
    schemes(argv[1]);
    fonts(argv[1], argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());
    edges();
    if (failures) {
        printf("FAIL: %d checks\n", failures);
        return 1;
    }
    printf("PASS: menus, menu font, title bar scheme and edge fill behave as specified\n");
    return 0;
}
