// Window chrome that follows Live's theme (see chrome.h).
#include "chrome.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHash>
#include <QImage>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QStyleFactory>
#include <QStyleOptionMenuItem>
#include <QWidget>
#include <QWindow>
#include <cmath>

namespace chrome {

namespace {

double luminance(const QColor &c)
{
    auto channel = [](int v) {
        const double x = v / 255.0;
        return x <= 0.03928 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) + 0.0722 * channel(c.blue());
}

// Sizes in Live's units (scaled by MenuLook::unit), as Live's own menus have them.
constexpr double kFrame = 1;        // frame line
constexpr double kVMargin = 1;      // above the first row and below the last
constexpr double kTickColumn = 16;  // inner edge to the label; the tick sits in it
constexpr double kTick = 8;         // tick size
constexpr double kShortcutGap = 20; // label to shortcut
constexpr double kArrowColumn = 8;  // submenu arrow column
constexpr double kRowRight = 5;     // shortcut or arrow column to the frame
constexpr double kRowHeight = 16;   // smallest row height
constexpr double kSeparator = 10;   // separator row height

} // namespace

double contrast(const QColor &a, const QColor &b)
{
    const double la = luminance(a), lb = luminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

QColor extreme(const QColor &back)
{
    return contrast(Qt::black, back) >= contrast(Qt::white, back) ? QColor(Qt::black) : QColor(Qt::white);
}

QColor withContrast(const QColor &c, const QColor &back, const QColor &towards, double ratio)
{
    for (int i = 0; i <= 20; ++i) {
        const QColor m = mix(c, towards, i / 20.0);
        if (contrast(m, back) >= ratio)
            return m;
    }
    return towards;
}

MenuLook menuLook(const QColor &surface, const QColor &text, const QColor &disabled, int bandHeight)
{
    MenuLook look;
    look.unit = qMax(1, bandHeight) / 24.0;
    look.fontPixels = qRound(bandHeight * 12.0 / 24);
    look.surface = surface;
    look.text = withContrast(text, surface, extreme(surface), 7.0);
    look.disabled = withContrast(disabled, surface, look.text, 3.5);
    look.shortcut = withContrast(mix(look.text, surface, 0.4), surface, look.text, 4.5);
    look.hover = mix(surface, look.text, 0.13);
    return look;
}

MenuStyle::MenuStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

int MenuStyle::px(double units) const
{
    return qRound(units * look.unit);
}

int MenuStyle::pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const
{
    switch (metric) {
    case PM_MenuPanelWidth:
        return qMax(1, px(kFrame));
    case PM_MenuHMargin:
        return 0;
    case PM_MenuVMargin:
        return px(kVMargin);
    case PM_SubMenuOverlap:
        return 0; // a submenu's frame starts where its parent's ends
    default:
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

void MenuStyle::drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                              const QWidget *widget) const
{
    if (element == PE_FrameMenu)
        return; // the panel draws the frame
    if (element != PE_PanelMenu)
        return QProxyStyle::drawPrimitive(element, option, painter, widget);
    const QRect r = option->rect;
    const int f = qMax(1, px(kFrame));
    painter->fillRect(r, look.text);
    painter->fillRect(r.adjusted(f, f, -f, -f), look.surface);
}

void MenuStyle::drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                            const QWidget *widget) const
{
    if (element == CE_MenuEmptyArea)
        return; // the panel drew it
    const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
    if (element != CE_MenuItem || !item)
        return QProxyStyle::drawControl(element, option, painter, widget);
    const QRect r = item->rect;
    painter->save();
    if (item->menuItemType == QStyleOptionMenuItem::Separator) {
        const int line = qMax(1, px(kFrame));
        painter->fillRect(QRect(r.left(), r.top() + (r.height() - line) / 2, r.width(), line), look.text);
        painter->restore();
        return;
    }
    const bool enabled = item->state & State_Enabled;
    if (enabled && (item->state & State_Selected))
        painter->fillRect(r, look.hover);
    painter->setRenderHint(QPainter::Antialiasing);
    const QColor text = enabled ? look.text : look.disabled;
    const QColor dim = enabled ? look.shortcut : look.disabled;
    const int tickLeft = r.left(), tickWidth = px(kTickColumn);
    if (item->checkType != QStyleOptionMenuItem::NotCheckable && item->checked) {
        const double size = px(kTick);
        const QRectF box(tickLeft + (tickWidth - size) / 2, r.top() + (r.height() - size) / 2, size, size);
        QPainterPath tick;
        tick.moveTo(box.left(), box.top() + box.height() * 0.55);
        tick.lineTo(box.left() + box.width() * 0.38, box.bottom() - box.height() * 0.12);
        tick.lineTo(box.right(), box.top() + box.height() * 0.15);
        painter->setPen(QPen(text, qMax(1.2, 1.3 * look.unit), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(tick);
    }
    QString label = item->text;
    QString shortcut;
    const int tab = label.indexOf('\t');
    if (tab >= 0) {
        shortcut = label.mid(tab + 1);
        label.truncate(tab);
    }
    const bool submenu = item->menuItemType == QStyleOptionMenuItem::SubMenu;
    const int textLeft = tickLeft + tickWidth;
    const int textRight = r.right() + 1 - px(kRowRight) - (submenu ? px(kArrowColumn) : 0);
    const QRect textRect(textLeft, r.top(), qMax(0, textRight - textLeft), r.height());
    painter->setFont(item->font);
    painter->setPen(text);
    painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine | Qt::TextHideMnemonic, label);
    if (!shortcut.isEmpty()) {
        painter->setPen(dim);
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignRight | Qt::TextSingleLine, shortcut);
    }
    if (submenu) {
        const double x = textRight + px(kArrowColumn) / 2.0, y = r.top() + r.height() / 2.0, half = px(3);
        QPainterPath arrow;
        arrow.moveTo(x - half / 2, y - half);
        arrow.lineTo(x + half / 2, y);
        arrow.lineTo(x - half / 2, y + half);
        painter->setPen(QPen(dim, qMax(1.2, 1.3 * look.unit), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(arrow);
    }
    painter->restore();
}

QSize MenuStyle::sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size,
                                  const QWidget *widget) const
{
    const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
    if (type != CT_MenuItem || !item)
        return QProxyStyle::sizeFromContents(type, option, size, widget);
    if (item->menuItemType == QStyleOptionMenuItem::Separator)
        return QSize(px(2), px(kSeparator));
    // The menu adds the widest shortcut to this. Rows are a fixed height, as
    // Live's, whatever line height the font reports (it differs between font
    // engines, and the text is centred in the row).
    const int arrow = item->menuItemType == QStyleOptionMenuItem::SubMenu ? px(kArrowColumn) : 0;
    return QSize(px(kTickColumn) + size.width() + px(kShortcutGap) + arrow + px(kRowRight), px(kRowHeight));
}

namespace {

// Qt places a submenu by its first row, level with the row that opens it, but
// only when it already had rows; one whose rows are added as it opens comes
// in at that row's top instead, and is moved up here.
class SubmenuPlacement : public QObject
{
public:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() != QEvent::Show)
            return false;
        auto *menu = static_cast<QMenu *>(o);
        auto *opener = qobject_cast<QMenu *>(menu->parentWidget());
        QAction *row = opener ? opener->activeAction() : nullptr;
        if (!row || row->menu() != menu || menu->actions().isEmpty())
            return false;
        const int rowTop = opener->mapToGlobal(opener->actionGeometry(row).topLeft()).y();
        if (menu->y() == rowTop)
            menu->move(menu->x(), rowTop - menu->actionGeometry(menu->actions().first()).top());
        return false;
    }
};

} // namespace

QString loadFontFamily(const QString &path)
{
    const int id = QFontDatabase::addApplicationFont(path);
    return id < 0 ? QString() : QFontDatabase::applicationFontFamilies(id).value(0);
}

void placeSubmenus(QMenu *menu)
{
    static auto *filter = new SubmenuPlacement;
    menu->installEventFilter(filter);
}

QString colorSchemeText(const QColor &band, const QColor &text)
{
    const auto rgb = [](const QColor &c) { return QStringLiteral("%1,%2,%3").arg(c.red()).arg(c.green()).arg(c.blue()); };
    QString s = QStringLiteral("[General]\nName=Ableton Live\n");
    for (const char *group : {"Colors:Header", "Colors:Window"})
        s += QStringLiteral("\n[%1]\nBackgroundNormal=%2\nBackgroundAlternate=%2\nForegroundNormal=%3\n")
                 .arg(QLatin1String(group), rgb(band), rgb(text));
    s += QStringLiteral("\n[WM]\nactiveBackground=%1\nactiveBlend=%1\nactiveForeground=%2\n"
                        "inactiveBackground=%1\ninactiveBlend=%1\ninactiveForeground=%2\n")
             .arg(rgb(band), rgb(text));
    return s;
}

QString writeColorScheme(const QString &dir, const QColor &band, const QColor &text, const QString &prefix,
                         const QStringList &keep)
{
    static QString current, previous; // "live" paths written by this process
    const bool live = prefix == QLatin1String("live");
    QDir folder(dir);
    if (!folder.mkpath(QStringLiteral(".")))
        return {};
    const QString path = folder.absoluteFilePath(
        QStringLiteral("%1-%2-%3.colors").arg(prefix, band.name().mid(1), text.name().mid(1)));
    if (!QFile::exists(path)) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(colorSchemeText(band, text).toUtf8()) < 0 || !file.commit())
            return {};
    }
    if (live && path != current) {
        previous = current;
        current = path;
    }
    for (const QFileInfo &f : folder.entryInfoList({prefix + QStringLiteral("-*.colors")}, QDir::Files)) {
        const QString other = f.absoluteFilePath();
        const bool kept = live ? (other == current || other == previous) : (other == path || keep.contains(other));
        if (!kept)
            QFile::remove(other);
    }
    return path;
}

QColor dominantColour(const QImage &image)
{
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    QHash<QRgb, int> counts;
    QRgb best = 0;
    int bestCount = 0;
    for (int y = 0; y < rgb.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); ++x) {
            const QRgb c = line[x] | 0xff000000u;
            const int n = ++counts[c];
            if (n > bestCount) {
                best = c;
                bestCount = n;
            }
        }
    }
    return bestCount ? QColor(best) : QColor();
}

QColor titleTextFor(const QColor &band)
{
    const QColor dark(0x1a, 0x1a, 0x1a), light(0xe6, 0xe6, 0xe6);
    return contrast(dark, band) > contrast(light, band) ? dark : light;
}

QSize deviceSize(const QSize &logical, qreal ratio)
{
    return logical * ratio;
}

QSize deviceSize(const QWidget *view)
{
    // QWidget::devicePixelRatioF() is cut to 1/65536 steps (1.325 reads as
    // 1.3249969), a pixel short of the backing store at some sizes.
    const QWindow *window = view->window()->windowHandle();
    return deviceSize(view->size(), window ? window->devicePixelRatio() : view->devicePixelRatioF());
}

void fillUncovered(QPainter &painter, const QSize &view, const QImage &image)
{
    const int w = image.width(), h = image.height();
    if (image.isNull() || (view.width() <= w && view.height() <= h))
        return;
    painter.save();
    // Device pixels.
    const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1;
    painter.setWorldTransform(QTransform::fromScale(1 / ratio, 1 / ratio));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setRenderHint(QPainter::Antialiasing, false);
    if (view.width() > w)
        painter.drawImage(QRect(w, 0, view.width() - w, h), image, QRect(w - 1, 0, 1, h));
    if (view.height() > h)
        painter.drawImage(QRect(0, h, w, view.height() - h), image, QRect(0, h - 1, w, 1));
    if (view.width() > w && view.height() > h)
        painter.drawImage(QRect(w, h, view.width() - w, view.height() - h), image, QRect(w - 1, h - 1, 1, 1));
    painter.restore();
}

} // namespace chrome
