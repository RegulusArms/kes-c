#include "windiag.h"

#include <QApplication>
#include <QDateTime>
#include <QEvent>
#include <QHash>
#include <QLayout>
#include <QMoveEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QScreen>
#include <QWidget>
#include <QWindow>

namespace windiag {

namespace {

struct Record {
    qint64 since = 0;     // first seen (ms since the epoch)
    QStringList recent;   // the latest changes, oldest first
    bool screen_watched = false;
};

QString size_text(const QSize &s) { return QString("%1×%2").arg(s.width()).arg(s.height()); }
QString rect_text(const QRect &r) { return QString("%1×%2 at %3,%4").arg(r.width()).arg(r.height()).arg(r.x()).arg(r.y()); }

QString states_text(Qt::WindowStates s)
{
    QStringList out;
    if (s & Qt::WindowMinimized)
        out << "minimized";
    if (s & Qt::WindowMaximized)
        out << "maximized";
    if (s & Qt::WindowFullScreen)
        out << "full screen";
    if (s & Qt::WindowActive)
        out << "active";
    return out.isEmpty() ? QString("normal") : out.join(", ");
}

class Recorder : public QObject {
public:
    QHash<QWidget *, Record> windows;

    void note(QWidget *w, const QString &what)
    {
        Record &r = windows[w];
        if (!r.since) {
            r.since = QDateTime::currentMSecsSinceEpoch();
            connect(w, &QObject::destroyed, this, [this, w]() { windows.remove(w); });
        }
        if (!r.screen_watched && w->windowHandle()) {   // which screen it's on (Qt's idea of it)
            r.screen_watched = true;
            QPointer<QWidget> pw(w);
            connect(w->windowHandle(), &QWindow::screenChanged, this, [this, pw](QScreen *s) {
                if (pw)
                    note(pw, "screen → " + (s ? s->name() : QString("none")));
            });
        }
        r.recent << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") + "  " + what;
        while (r.recent.size() > 80)
            r.recent.removeFirst();
    }

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        switch (ev->type()) {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::WindowStateChange:
        case QEvent::Show:
        case QEvent::Hide:
            break;
        default:
            return false;
        }
        if (!obj->isWidgetType())
            return false;
        auto *w = static_cast<QWidget *>(obj);
        if (!w->isWindow())
            return false;
        switch (ev->type()) {
        case QEvent::Resize: {
            auto *re = static_cast<QResizeEvent *>(ev);
            note(w, QString("resize %1 (was %2), minimum %3").arg(size_text(re->size()), size_text(re->oldSize()),
                                                                  size_text(w->minimumSize())));
            break;
        }
        case QEvent::Move: {
            auto *me = static_cast<QMoveEvent *>(ev);
            note(w, QString("move to %1,%2 (from %3,%4)").arg(me->pos().x()).arg(me->pos().y()).arg(me->oldPos().x()).arg(me->oldPos().y()));
            break;
        }
        case QEvent::WindowStateChange:
            note(w, "state → " + states_text(w->windowState()));
            break;
        case QEvent::Show:
            note(w, "shown");
            break;
        case QEvent::Hide:
            note(w, "hidden");
            break;
        default:
            break;
        }
        return false;
    }
};

Recorder *recorder = nullptr;

}  // namespace

void start()
{
    if (recorder || !qApp)
        return;
    recorder = new Recorder;
    recorder->setParent(qApp);
    qApp->installEventFilter(recorder);
}

QString report()
{
    QStringList out;
    out << QString("Kestrel's windows, %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    out << QString("Qt %1 on %2 (session %3, QT_WAYLAND_DECORATION %4, QT_QPA_PLATFORM %5)")
               .arg(QString::fromLatin1(qVersion()), QGuiApplication::platformName(),
                    qEnvironmentVariable("XDG_SESSION_TYPE", "?"), qEnvironmentVariable("QT_WAYLAND_DECORATION", "unset"),
                    qEnvironmentVariable("QT_QPA_PLATFORM", "unset"));
    out << QString("Mouse buttons down (as Qt sees it): %1").arg(int(QGuiApplication::mouseButtons()));
    for (QScreen *s : QGuiApplication::screens())
        out << QString("Screen %1: %2, scale %3%4")
                   .arg(s->name(), rect_text(s->geometry()), QString::number(s->devicePixelRatio()),
                        s == QGuiApplication::primaryScreen() ? QString(", primary") : QString());
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (!w->isVisible() && !w->inherits("QMainWindow"))
            continue;
        out << "";
        QString age;
        if (recorder && recorder->windows.contains(w))
            age = QString(", seen for %1 min").arg((now - recorder->windows[w].since) / 60000);
        out << QString("== %1 “%2”%3%4").arg(QString::fromLatin1(w->metaObject()->className()), w->windowTitle(), age,
                                             w->isVisible() ? QString() : QString(" (hidden)"));
        out << QString("  widget: %1, frame %2").arg(rect_text(w->geometry()), rect_text(w->frameGeometry()));
        out << QString("  minimum %1 (hint %2, layout %3), maximum %4, size hint %5")
                   .arg(size_text(w->minimumSize()), size_text(w->minimumSizeHint()),
                        w->layout() ? size_text(w->layout()->minimumSize()) : QString("none"), size_text(w->maximumSize()),
                        size_text(w->sizeHint()));
        if (QWindow *h = w->windowHandle()) {
            QMargins m = h->frameMargins();
            out << QString("  window: %1, border margins %2,%3,%4,%5 (left, top, right, bottom)")
                       .arg(rect_text(h->geometry()))
                       .arg(m.left())
                       .arg(m.top())
                       .arg(m.right())
                       .arg(m.bottom());
            out << QString("  asks for minimum %1, maximum %2; state %3; visibility %4; screen %5")
                       .arg(size_text(h->minimumSize()), size_text(h->maximumSize()), states_text(h->windowStates()))
                       .arg(int(h->visibility()))
                       .arg(h->screen() ? h->screen()->name() + " " + rect_text(h->screen()->geometry()) : QString("none"));
        }
        // children asking for more room than the window has (they can make it refuse a smaller size)
        QStringList wide;
        for (QWidget *c : w->findChildren<QWidget *>()) {
            if (!c->isVisibleTo(w))
                continue;
            QSize min = c->minimumSizeHint().expandedTo(c->minimumSize());
            if (min.width() > w->width() || min.height() > w->height())
                wide << QString("%1 “%2” %3").arg(QString::fromLatin1(c->metaObject()->className()), c->objectName(),
                                                    size_text(min));
        }
        if (!wide.isEmpty())
            out << "  children wanting more room than the window: " + wide.mid(0, 8).join("; ");
        if (recorder && recorder->windows.contains(w)) {
            out << "  recent changes:";
            for (const QString &r : recorder->windows[w].recent)
                out << "    " + r;
        }
    }
    return out.join('\n') + '\n';
}

}  // namespace windiag
