// Shift+vertical wheel to horizontal wheel on Live 12.4.7 native views.
// Remap before delivery: some Live scrollers move vertically even when Qt marks
// their event ignored. Delivering both axes would move the Arrangement
// diagonally.
#include "livebuild.h"
#include <QCoreApplication>
#include <QObject>
#include <QWheelEvent>
#include <cstring>
#include <typeinfo>

namespace {
class HorizontalFallback final : public QObject {
  bool sending = false;
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (sending || event->type() != QEvent::Wheel ||
        std::strcmp(typeid(*watched).name(), "7TQtView") != 0)
      return false;
    auto *wheel = static_cast<QWheelEvent *>(event);
    if (wheel->modifiers() != Qt::ShiftModifier || wheel->angleDelta().x() ||
        wheel->pixelDelta().x() || (!wheel->angleDelta().y() && !wheel->pixelDelta().y()))
      return false;
    QWheelEvent horizontal(wheel->position(), wheel->globalPosition(),
                           QPoint(wheel->pixelDelta().y(), 0), QPoint(wheel->angleDelta().y(), 0),
                           wheel->buttons(), wheel->modifiers(), wheel->phase(), wheel->inverted(),
                           wheel->source());
    horizontal.setAccepted(false);
    sending = true;
    QCoreApplication::sendEvent(watched, &horizontal);
    sending = false;
    return true;
  }
};
} // namespace

void installHorizontalWheelFallback() {
  if (liveBuild() != LiveBuild::Live12_4_7)
    return;
  static HorizontalFallback filter;
  QCoreApplication::instance()->installEventFilter(&filter);
}

Q_COREAPP_STARTUP_FUNCTION(installHorizontalWheelFallback)
