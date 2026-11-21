// Device sizes of real widgets at a fractional scale, run offscreen at
// QT_SCALE_FACTOR=1.325 by tests/chrome-scale.sh. The expected size is measured from the window's backing store image, and the
// literal products are worked by hand: 1300 x 1.325 = 1722.5 -> 1723,
// 1301 x 1.325 = 1723.825 -> 1724, 800 x 1.325 = 1060.
#include "chrome.h"
#include <QApplication>
#include <QBackingStore>
#include <QWidget>
#include <QWindow>
#include <qpa/qplatformbackingstore.h>
#include <cstdio>

static int failures;

static void check(const QString &what, bool ok)
{
    printf("%s %s\n", ok ? "  ok  " : "FAIL:", qPrintable(what));
    if (!ok)
        ++failures;
}

static QString text(const QSize &s)
{
    return QString("%1x%2").arg(s.width()).arg(s.height());
}

static void sized(int width, int height, const QSize &expected)
{
    QWidget top;
    top.resize(width, height);
    top.show();
    QApplication::processEvents();
    top.repaint();
    QApplication::processEvents();
    const QSize computed = chrome::deviceSize(&top);
    QSize stored;
    if (QBackingStore *store = top.backingStore())
        if (store->handle())
            if (QPaintDevice *device = store->handle()->paintDevice())
                stored = QSize(device->width(), device->height());
    const QString n = QString("%1x%2 at %3: ").arg(width).arg(height).arg(top.windowHandle()->devicePixelRatio());
    check(n + "device size " + text(computed) + ", expected " + text(expected), computed == expected);
    if (!stored.isEmpty())
        check(n + "device size " + text(computed) + ", backing store " + text(stored), computed == stored);
    else
        check(n + "backing store image readable", false);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (QGuiApplication::primaryScreen() && qAbs(QWindow().devicePixelRatio() - 1.325) > 1e-9) {
        printf("FAIL: the run is not at scale 1.325 (QT_SCALE_FACTOR)\n");
        return 2;
    }
    sized(1300, 800, QSize(1723, 1060));
    sized(1301, 800, QSize(1724, 1060));
    if (failures) {
        printf("FAIL: %d checks\n", failures);
        return 1;
    }
    printf("PASS: device sizes of real widgets at 1.325 match their backing store\n");
    return 0;
}
