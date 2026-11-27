// Regression checks for shim/chrome.cpp, run offscreen by
// tests/chrome-regressions.sh. Expected values are literals or come from
// the parent menu's own geometry, never from the code under test.
#include "chrome.h"
#include <QApplication>
#include <QMenu>
#include <QMenuBar>
#include <cstdio>

using namespace chrome;

static int failures;

static void check(const QString &what, bool ok)
{
    printf("%s %s\n", ok ? "  ok  " : "FAIL:", qPrintable(what));
    if (!ok)
        ++failures;
}

// #5: Qt sizes a backing store as the logical size times the ratio, rounded
// to nearest (801 * 1.325 = 1061.3 -> 1061; 800 * 1.325 = 1060 exactly).
static void deviceSizes()
{
    check("801x600 at 1.325 is 1061x795 device pixels", deviceSize(QSize(801, 600), 1.325) == QSize(1061, 795));
    check("800x600 at 1.325 is 1060x795 device pixels", deviceSize(QSize(800, 600), 1.325) == QSize(1060, 795));
    check("803x601 at 1.325 is 1064x796 device pixels", deviceSize(QSize(803, 601), 1.325) == QSize(1064, 796));
    check("100x50 at 1 is 100x50", deviceSize(QSize(100, 50), 1) == QSize(100, 50));
}

// A drop-down as the shim builds one: the shared style, submenus placed.
static void setUp(QMenu &menu, MenuStyle &style)
{
    menu.setStyle(&style);
    placeSubmenus(&menu);
}

static int globalTop(const QMenu &menu, QAction *a)
{
    return menu.mapToGlobal(menu.actionGeometry(a).topLeft()).y();
}

// #2: a submenu whose rows are added as it opens has its first row level
// with the row that opened it, the first time and the next.
static void submenuPlacement(MenuStyle &style, QMenuBar &bar)
{
    QMenu parent("Parent", &bar);
    setUp(parent, style);
    parent.addAction("Alpha");
    QMenu child("Child", &parent);
    setUp(child, style);
    QObject::connect(&child, &QMenu::aboutToShow, &child, [&child] {
        child.clear();
        child.addAction("One");
        child.addAction("Two");
    });
    QAction *sub = parent.addMenu(&child);
    parent.addAction("Omega");
    parent.popup(QPoint(150, 140));
    QApplication::processEvents();
    for (const char *when : {"first", "second"}) {
        parent.setActiveAction(sub);
        QApplication::processEvents();
        if (!child.isVisible() || child.actions().isEmpty()) {
            check(QString("%1 open: submenu shown").arg(when), false);
            return;
        }
        const int row = globalTop(parent, sub), first = globalTop(child, child.actions().first());
        check(QString("%1 open: submenu's first row at y %2, its row at y %3").arg(when).arg(first).arg(row),
              qAbs(first - row) <= 1);
        child.hide();
        QApplication::processEvents();
    }
    parent.hide();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    deviceSizes();
    QMenuBar bar;
    bar.setStyleSheet("QMenuBar { background: #3c3c3c; color: #e0e0e0; }");
    bar.setGeometry(100, 100, 400, 30);
    bar.show();
    MenuStyle style;
    style.look = menuLook(QColor(0x20, 0x20, 0x20), Qt::white, Qt::gray, 24);
    submenuPlacement(style, bar);
    if (failures) {
        printf("FAIL: %d checks\n", failures);
        return 1;
    }
    printf("PASS: device sizes and submenu placement\n");
    return 0;
}
