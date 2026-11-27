// Window chrome that follows Live's theme (see chrome.h).
#include "chrome.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
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
