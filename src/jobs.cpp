#include "jobs.h"

#include "atc.h"
#include "drives.h"
#include "fileops.h"
#include "util.h"

#include <QJsonDocument>
#include <QSet>
#include <QSettings>

using namespace util;

namespace jobs {

QStringList to_start(const QList<Entry> &queue, const QString &mode)
{
    QStringList out;
    QSet<QString> busy;
    bool running = false;
    for (const Entry &e : queue)
        if (e.running) {
            running = true;
            for (const QString &d : e.drives)
                busy << d;
        }
    for (const Entry &e : queue) {
        if (e.running)
            continue;
        if (e.forced) {   // "start now": whatever the others use
            out << e.key;
            running = true;
            for (const QString &d : e.drives)
                busy << d;
            continue;
        }
        if (e.paused)   // waiting, paused: it neither starts nor holds up the jobs after it
            continue;
        if (mode == "one") {
            if (!running) {
                out << e.key;
                running = true;
            }
            continue;
        }
        bool free = true;
        for (const QString &d : e.drives)
            free = free && !busy.contains(d);
        if (free)
            out << e.key;
        for (const QString &d : e.drives)   // a waiting job keeps its place: the later ones in its lanes wait behind it
            busy << d;
    }
    return out;
}

QString mode() { return settings().value("jobs_mode", "drive").toString() == "one" ? "one" : "drive"; }

void set_mode(const QString &m)
{
    settings().setValue("jobs_mode", m == "one" ? "one" : "drive");
    atc::announce("queue_mode", {{"mode", mode()}});
    Queue::instance()->control(QString(), QString(), "decide");
}

// ---------------------------------------------------------------- Queue

Queue *Queue::instance()
{
    static Queue *q = new Queue;
    return q;
}

Queue::Queue()
{
    connect(atc::radio(), &atc::Radio::heard, this, &Queue::on_heard);
    connect(atc::radio(), &atc::Radio::reset, this, [this]() {   // a (new) tower: it learns our jobs again
        shared.clear();
        atc::announce("queue_mode", {{"mode", mode()}});
        for (const Local &l : local)
            ask(l, l.running);
        Q_EMIT changed();
    });
    fallback.setInterval(1000);   // waiting jobs while there's no tower: decided here
    connect(&fallback, &QTimer::timeout, this, [this]() {
        if (local.isEmpty())
            fallback.stop();
        else if (!via_tower())
            decide();
    });
}

bool Queue::via_tower() const { return atc::radio()->tower_up(); }

bool Queue::start_paused() const { return settings().value("jobs_start_paused", false).toBool(); }

void Queue::set_start_paused(bool on) { settings().setValue("jobs_start_paused", on); }

Queue::Local *Queue::find(Task *t)
{
    for (Local &l : local)
        if (l.task == t)
            return &l;
    return nullptr;
}

void Queue::submit(Task *t)
{
    QStringList ids = drives::drive_ids(t->paths);   // on the UI thread: each mount looked up once, not each item
    if (ids.isEmpty()) {   // nothing to wait for
        t->start();
        return;
    }
    // for its log, if it has to wait: the jobs ahead of it in its lanes (every Kestrel's), or all of them one at a time
    bool one = mode() == "one";
    for (const Job &j : jobs())
        if (one || std::any_of(j.drives.begin(), j.drives.end(), [&ids](const QString &d) { return ids.contains(d); }))
            t->waited_for << j.title;
    t->queued_ms = Task::now_ms();
    Local l;
    l.task = t;
    l.drives = ids;
    l.paused = start_paused();
    t->queued = true;
    t->text = l.paused ? "Paused (not started)" : "Waiting for its turn (its drive is in use)…";
    local << l;
    connect(t, &QThread::finished, this, [this, t]() { finished(t); });
    connect(t, &Task::cancel_asked, this, [this, t]() {   // cancelled while waiting: its thread starts only to end
        if (t->queued)
            start(t);
    });
    if (via_tower())
        ask(l, false);
    else
        decide();
    if (!fallback.isActive())
        fallback.start();
    Q_EMIT changed();
}

void Queue::ask(const Local &l, bool running)
{
    if (!l.task)
        return;
    QJsonObject req{{"id", l.task->id},          {"title", l.task->title},
                    {"drives", QJsonArray::fromStringList(l.drives)},
                    {"paused", l.paused},        {"running", running},
                    {"admin", l.task->admin}};
    atc::radio()->call("JobAsk", QString::fromUtf8(QJsonDocument(req).toJson(QJsonDocument::Compact)));
}

void Queue::start(Task *t)
{
    Local *l = find(t);
    if (!l || l->running || !t)
        return;
    l->running = true;
    t->queued = false;
    t->text.clear();
    t->start();
    Q_EMIT changed();
}

void Queue::finished(Task *t)
{
    QString id;
    for (int i = 0; i < local.size(); ++i)
        if (local[i].task == t || !local[i].task) {
            if (local[i].task == t)
                id = t->id;
            local.removeAt(i--);
        }
    if (via_tower()) {
        if (!id.isEmpty())
            atc::radio()->call("JobDone", QString::fromUtf8(QJsonDocument(QJsonObject{{"id", id}}).toJson(QJsonDocument::Compact)));
    } else {
        decide();
    }
    Q_EMIT changed();
}

void Queue::decide()
{
    QList<Entry> q;
    for (const Local &l : local)
        if (l.task)
            q << Entry{l.task->id, l.drives, l.running, l.paused, l.forced};
    for (const QString &key : to_start(q, mode()))
        for (const Local &l : local)
            if (l.task && l.task->id == key) {
                start(l.task);
                break;
            }
}

QList<Job> Queue::jobs() const
{
    QString me = atc::radio()->flight();
    auto state_of = [](const Local &l) {
        if (l.task->queued)
            return QString(l.paused ? "paused" : "waiting");
        return QString(l.task->paused() ? "paused" : "running");
    };
    QList<Job> out;
    QSet<QString> listed;
    if (via_tower()) {
        for (Job j : shared) {
            if (j.flight == me) {
                for (const Local &l : local)
                    if (l.task && l.task->id == j.id) {
                        j.local = l.task;
                        j.state = state_of(l);
                    }
                if (!j.local)
                    continue;   // gone here already (the tower hears it next)
                listed << j.id;
            }
            out << j;
        }
    }
    for (const Local &l : local)   // ours: all of them without a tower, else those it hasn't listed yet
        if (l.task && !listed.contains(l.task->id))
            out << Job{me, l.task->id, l.task->title, state_of(l), l.drives, l.task->admin, l.task};
    return out;
}

void Queue::apply(Task *t, const QString &action)
{
    Local *l = find(t);
    if (!l || !t)
        return;
    if (action == "pause" || action == "resume") {
        bool on = action == "pause";
        if (t->queued) {
            l->paused = on;
            t->text = on ? "Paused (not started)" : "Waiting for its turn (its drive is in use)…";
        } else {
            t->set_paused(on);
        }
    } else if (action == "cancel") {
        if (t->cancellable || t->queued)
            t->cancel();   // a waiting one: its thread starts only to end at once (cancel_asked)
    } else if (action == "start_now") {
        l->forced = true;
        if (t->queued)
            start(t);
    }
    Q_EMIT changed();
}

void Queue::control(const QString &flight_in, const QString &id, const QString &action)
{
    if (action == "decide") {   // the mode changed
        if (!via_tower())
            decide();
        Q_EMIT changed();
        return;
    }
    QString me = atc::radio()->flight(), flight = flight_in.isEmpty() ? me : flight_in;
    if (flight == me || me.isEmpty()) {
        for (const Local &l : local)
            if (l.task && l.task->id == id) {
                if (action == "up" || action == "down") {
                    int i = int(&l - local.constData()), j = action == "up" ? i - 1 : i + 1;
                    if (j >= 0 && j < local.size())
                        local.swapItemsAt(i, j);
                } else {
                    apply(l.task, action);
                }
                break;
            }
    }
    if (via_tower())   // the tower keeps the order (and starts what's free); another Kestrel's job: its Kestrel acts
        atc::announce("job_control", {{"flight", flight}, {"task", id}, {"action", action}});
    else
        decide();
    Q_EMIT changed();
}

void Queue::pause_all(bool on)
{
    for (const Job &j : jobs())
        if ((on && j.state != "paused") || (!on && j.state == "paused"))
            control(j.flight, j.id, on ? "pause" : "resume");
}

bool Queue::busy(const QString &path) const
{
    QStringList ids = drives::drive_ids(path);
    for (const Job &j : jobs()) {
        if (j.state == "waiting" || (j.local && j.local->queued))
            continue;
        for (const QString &d : j.drives)
            if (ids.contains(d))
                return true;
    }
    return false;
}

void Queue::on_heard(const QJsonObject &m)
{
    if (m.value("own").toBool())
        return;
    QString type = m.value("type").toString(), me = atc::radio()->flight();
    if (type == "lane_open" && m.value("flight").toString() == me) {
        for (const Local &l : local)
            if (l.task && l.task->id == m.value("task").toString()) {
                start(l.task);
                break;
            }
    } else if (type == "queue") {
        shared.clear();
        for (const QJsonValue &v : m.value("jobs").toArray()) {
            QJsonObject o = v.toObject();
            QStringList drives;
            for (const QJsonValue &d : o.value("drives").toArray())
                drives << d.toString();
            QString state = o.value("state").toString();
            if (o.value("paused").toBool())
                state = "paused";
            shared << Job{o.value("flight").toString(), o.value("id").toString(), o.value("title").toString(), state, drives,
                          o.value("admin").toBool(), nullptr};
        }
        Q_EMIT changed();
    } else if (type == "job_control" && m.value("flight").toString() == me) {
        QString id = m.value("task").toString(), action = m.value("action").toString();
        for (const Local &l : local)
            if (l.task && l.task->id == id) {
                if (action != "up" && action != "down")
                    apply(l.task, action);
                break;
            }
    }
}

}  // namespace jobs
