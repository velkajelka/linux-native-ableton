// Mouse input for a dialog opened over another dialog (LIVE_MODAL_REFRESH=0 turns this off).
//
// Qt keeps a cached "blocked by a modal window" flag per window and drops every mouse
// event (hover and clicks) for a window whose flag is set; keys go to the focus window
// and still work. When Live opens a modal box while another modal one is up (at start a
// trial's "Saving and exporting were successfully activated" over "Retrieving
// authorization from ableton.com..."), the new box keeps the flag it got while it was
// still blocked by the older one: Qt's own check (isWindowBlocked) says it is free, but
// its OK button never sees the pointer, so only Enter closed it.
//
// Each time a modal window is shown or hidden, the flag of every visible window is
// computed again (QGuiApplicationPrivate::updateBlockedStatus, the call Qt makes itself
// when a window's modality changes), after Qt has finished handling that event.
#include <QGuiApplication>
#include <QTimer>
#include <QWindow>
#include <dlfcn.h>
#include <cstdlib>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

namespace {

using UpdateBlocked = void (*)(QWindow *);

UpdateBlocked updateBlocked()
{
    static const auto fn = reinterpret_cast<UpdateBlocked>(
        dlvsym(RTLD_DEFAULT, "_ZN22QGuiApplicationPrivate19updateBlockedStatusEP7QWindow", "Qt_5_PRIVATE_API"));
    return fn;
}

void refreshAll()
{
    int n = 0;
    for (QWindow *w : QGuiApplication::allWindows())
        if (w->isVisible()) {
            updateBlocked()(w);
            ++n;
        }
    static int traced = 0;
    if (++traced <= 5)
        menufixTrace("modalrefresh: blocked state of %d window(s) computed again\n", n);
}

class ModalRefresh : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if ((ev->type() == QEvent::Show || ev->type() == QEvent::Hide) && obj->isWindowType()
            && static_cast<QWindow *>(obj)->modality() != Qt::NonModal && !pending) {
            pending = true;
            QTimer::singleShot(0, this, [this] {
                pending = false;
                refreshAll();
            });
        }
        return false;
    }

private:
    bool pending = false;
};

} // namespace

void installModalRefresh()
{
    if (const char *off = getenv("LIVE_MODAL_REFRESH"); off && *off == '0')
        return;
    if (!updateBlocked()) {
        menufixTrace("modalrefresh: no updateBlockedStatus in this Qt, left alone\n");
        return;
    }
    qApp->installEventFilter(new ModalRefresh);
}
