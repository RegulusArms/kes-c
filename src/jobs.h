// The job queue: one queue for every Kestrel, so no two jobs hit the same drive at once, whichever window or Kestrel
// started them.
//
// A job that reads or writes drives (a copy, a move, an extraction…; fileops::queue_job) names its paths, which become
// the drives it uses (drives::drive_ids: the physical disk, the same in every Kestrel). It waits in the queue, its
// thread not started, until its lanes are free:
// - by drive (the default): jobs whose drives don't overlap run at the same time; a job waits while a running job uses
//   one of its drives, or an earlier waiting one will (so jobs keep their order within a lane);
// - one at a time: a single job runs, the next starts when it's done.
// A job can also be started now (skipping the queue), paused while waiting, moved up or down, or cancelled.
//
// With the tower up (atc.h), the tower keeps the one queue: each Kestrel asks it for lanes (JobAsk), runs its job when
// told its lane is open ("lane_open"), and says when it's finished (JobDone); controls from any Jobs window go through
// it ("job_control"), and it tells everyone the queue ("queue"). A Kestrel that leaves frees its lanes; a new tower
// rebuilds the queue as the Kestrels check in again. Without a tower, each Kestrel queues its own jobs.
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

class Task;

namespace jobs {

// the scheduler, shared by the tower and the fallback: the keys of the waiting jobs that may start now
struct Entry {
    QString key;
    QStringList drives;
    bool running = false, paused = false, forced = false;
};
QStringList to_start(const QList<Entry> &queue, const QString &mode);

QString mode();   // "drive" (by drive, the default) or "one" (one at a time)
void set_mode(const QString &mode);

// a job of any Kestrel, as the queue knows it
struct Job {
    QString flight, id, title, state;   // state: "waiting", "running", "paused"
    QStringList drives;
    bool admin = false;
    Task *local = nullptr;   // ours
};

class Queue : public QObject {
    Q_OBJECT
public:
    static Queue *instance();
    // A job naming its paths (task->paths): started when its lanes are free. Without drives (nothing it can be told
    // apart by, or a move within one drive) it starts at once.
    void submit(Task *task);
    QList<Job> jobs() const;   // the whole queue, in order: every Kestrel's (through the tower), else ours
    // "pause", "resume", "start_now", "up", "down", "cancel" a job of any Kestrel
    void control(const QString &flight, const QString &id, const QString &action);
    void pause_all(bool on);
    bool busy(const QString &path) const;   // a running job uses the drive path is on
    bool start_paused() const;
    void set_start_paused(bool on);

Q_SIGNALS:
    void changed();

private:
    Queue();
    struct Local {
        QPointer<Task> task;
        QStringList drives;
        bool running = false, paused = false, forced = false;
    };
    QList<Local> local;      // our jobs, in order (decided here only without a tower)
    QList<Job> shared;       // the tower's queue: every Kestrel's
    QTimer fallback;         // waiting jobs and no tower: decide here
    bool via_tower() const;
    Local *find(Task *t);
    void ask(const Local &l, bool running);
    void start(Task *t);
    void finished(Task *t);
    void decide();
    void on_heard(const QJsonObject &m);
    void apply(Task *t, const QString &action);   // a control on one of ours
};

}  // namespace jobs
