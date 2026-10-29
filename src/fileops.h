// File operations: threaded copy/move/delete with progress, the status-bar task panel, conflicts, links.
#pragma once

#include "proc.h"

#include <QDateTime>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoopLocker>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QPointer>
#include <QProgressBar>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <QWaitCondition>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QCheckBox;
class QLabel;
class QToolButton;

// A background job shown in its window's status bar (TaskPanel) and the Jobs window.
//
// fn(task) runs on the thread. It may call task->report(done, total, text) for progress (total 0 = busy, no
// percentage) and task->check() to stop early when the user cancels (and to wait there while it's paused). Its return
// value goes to on_done; a Cancelled exception counts as a normal finish with an invalid QVariant and was_cancelled set.
//
// A job that reads or writes drives names its paths: it then waits in the job queue (jobs.h) for its lanes before its
// thread starts (fileops::queue_job). Its steps and log are shown in the Jobs window.
class Task : public QThread, public proc::Watcher {
    Q_OBJECT
public:
    using Fn = std::function<QVariant(Task *)>;
    Task(const QString &title, Fn fn, bool cancellable = true);

    QString title;
    QString id;           // unique in this process (for the shared task list)
    bool admin = false;   // runs in this window's admin session
    bool cancellable;
    std::atomic<bool> cancelled{false};
    bool was_cancelled = false;
    double fraction = -1.0;
    QString text;
    QStringList paths;    // what it reads and writes: the drives it needs lanes on; empty: starts at once
    bool queued = false;  // waiting in the job queue, its thread not started yet
    QStringList steps;    // what it's going to do, for the Jobs window (e.g. each item of a copy)
    std::atomic<int> step{-1};   // the step it's on
    qint64 started_ms = 0;       // when its thread started (the job queue let it run)
    qint64 queued_ms = 0;        // when it went into the job queue to wait (0: it started at once)
    QStringList waited_for;      // the jobs ahead of it on its drives then (titles), for its log
    QStringList intro;           // its log's first lines: what started it, when, from where to where
    // A job the user sees (not a quiet helper task) keeps Kestrel running until it ends, also once every Kestrel
    // window is closed (the Jobs window shows it then); released when it's deleted.
    std::unique_ptr<QEventLoopLocker> keeps_running;
    bool failed = false;         // ended with an error
    std::atomic<bool> had_errors{false};   // some of its items failed (it ends "done with errors")
    qint64 elapsed_ms() const;   // since it started; 0 while it waits
    static qint64 now_ms();      // the clock of started_ms and queued_ms (monotonic)

    void cancel();
    void check();   // throws Cancelled; waits here while paused
    void set_paused(bool on);    // also pauses (SIGSTOP) and continues (SIGCONT) the programs it runs
    bool paused() const { return pause_flag; }
    bool pausable() const { return !admin; }   // the admin helper runs as root: Kestrel can't stop it
    void log(const QString &line);              // a line of what it did (capped: the start and the end are kept)
    void log_output(const QByteArray &chunk);   // a program's output, as it comes
    QString log_text() const;
    static Task *current();   // the job running on this thread, if any
    // at most ~12 a second; `now`: always (a state the user must see at once, such as waiting for a tape to wind)
    void report(double done, double total = 0, const QString &text = QString(), bool now = false);

Q_SIGNALS:
    void progress(double fraction, const QString &text);   // fraction 0..1 or -1 for busy
    void cancel_asked();   // cancel() was called (a job still waiting in the queue then ends at once)
    void result(const QVariant &res);
    void error(const QString &msg);

protected:
    void run() override;
    void started(pid_t pid) override;
    void ended(pid_t pid) override;

private:
    Fn fn;
    qint64 last = 0;
    std::atomic<bool> pause_flag{false};
    mutable QMutex mtx;          // the log and the programs
    QWaitCondition resumed;
    QSet<pid_t> programs;
    QStringList head, tail;      // the log: its first lines, and its latest
    int skipped = 0;
    QByteArray partial;          // a program's output line not finished yet
};

// A running task as the shared task list knows it: one of ours (local), or another Kestrel's.
struct TaskInfo {
    QString flight, id, title, text;
    double fraction = -1.0;
    bool cancellable = false, cancelling = false, admin = false;
    Task *local = nullptr;
    QString state;   // "waiting", "running", "paused"
    qint64 elapsed = 0;   // ms since it started (0 while it waits)
};

// A job of this Kestrel that has ended, for the Jobs window (kept until cleared; the newest 50)
struct FinishedTask {
    QString title, state;   // "done", "done with errors", "failed", "cancelled"
    QString log;
    QStringList steps;
    qint64 took_ms = 0;
    QDateTime ended;
    bool admin = false;
    QString id;       // the task's (its log is asked for by it)
    QString flight;   // another Kestrel's (remote_finished: without its log and steps, asked for with ask_detail)
};

// Another Kestrel's job, as the Jobs window shows it: its steps around where it is, and its log (ask_detail)
struct JobDetail {
    QStringList steps;    // steps_from… of steps_total
    int steps_from = 0, steps_total = 0, step = -1;
    QString log;
    qint64 at = 0;        // when it came (ms since the epoch)
};

// Every running task: this Kestrel's (all its windows) and, through the tower (atc.h), the other Kestrels'. Ours are
// reported to the others whenever one starts or ends, and at most twice a second while they make progress.
class TaskBoard : public QObject {
    Q_OBJECT
public:
    static TaskBoard *instance();
    void add(Task *task);
    QList<TaskInfo> others(const QList<Task *> &mine) const;   // all but `mine`: our other windows', then other Kestrels'
    QList<TaskInfo> all() const { return others({}); }        // every job running or waiting: ours, then the others'
    void cancel(const TaskInfo &task);   // another Kestrel's task: asks it to cancel
    QList<Task *> mine() const { return local; }   // this Kestrel's, every window's
    // the jobs that keep this Kestrel running (keeps_running), running or waiting
    QList<Task *> keeping() const;
    // Finished jobs stay until Clear Finished (here or in another Kestrel), however many, also a closed Kestrel's
    QList<FinishedTask> finished;        // ours that have ended, newest last
    QMap<QString, QList<FinishedTask>> remote_finished;   // flight -> the other Kestrels' (no logs: ask_detail)
    void clear_finished();               // every Kestrel's
    bool has_left(const QString &flight) const { return left.contains(flight); }   // closed: its logs went with it
    // Another Kestrel's job or finished job: ask it for its steps and log; detail() has them once it answers
    void ask_detail(const QString &flight, const QString &task);
    JobDetail detail(const QString &flight, const QString &task) const { return details.value(flight + "/" + task); }
    void pause(const TaskInfo &task, bool on);   // also another Kestrel's (a job outside the queue: jobs.h has its own)

Q_SIGNALS:
    void changed();

private:
    TaskBoard();
    void schedule(int ms);
    void publish();
    void on_heard(const QJsonObject &msg);
    QList<Task *> local;
    QMap<QString, QList<TaskInfo>> remote;   // flight -> its tasks
    QHash<QString, JobDetail> details;       // "flight/task" -> what it answered
    QSet<QString> left;                      // flights that have closed (their finished jobs are kept)
    void publish_finished();
    QTimer *timer;
};

// Status-bar widget: the running tasks' title, status and progress, with a cancel button. This window's tasks come
// first; tasks running in other windows (and other Kestrels) are counted after them, or shown when there are none here.
// The task panel's bar. Busy (no percentage): a block glides back and forth, so a long wait doesn't look stuck
// (styles draw busy bars differently, some barely moving).
class PulseBar : public QProgressBar {
public:
    explicit PulseBar(QWidget *parent = nullptr);
    void set_busy(bool on);
    bool busy() const { return pulsing; }

protected:
    void paintEvent(QPaintEvent *ev) override;

private:
    bool pulsing = false;
    QTimer frame;
    QElapsedTimer clock;
};

class TaskPanel : public QWidget {
    Q_OBJECT
public:
    explicit TaskPanel(QWidget *parent = nullptr);
    void add(Task *task);
    QList<Task *> tasks;
    void refresh();

Q_SIGNALS:
    void clicked();   // opens the Jobs window

protected:
    void mousePressEvent(QMouseEvent *ev) override;

private:
    void on_progress(Task *task, double fraction, const QString &text);
    void on_finished(Task *task);
    void cancel_first();
    QLabel *label;
    PulseBar *bar;
    QToolButton *stop;
};

namespace fileops {

using Done = std::function<void(const QVariant &)>;

// Run fn(task) on a thread, shown in the status bar unless quiet. on_done(result) runs on the UI thread (also
// after a cancel, with an invalid QVariant) while `parent` exists. Errors are shown in a message box unless quiet.
Task *run_job(QWidget *parent, const QString &title, Task::Fn fn, Done on_done = nullptr, bool cancellable = true,
              bool quiet = false);
// The same for a job that reads or writes drives: it waits in the job queue (jobs.h) for lanes on the drives of
// `paths` before its thread starts, so it doesn't compete with other jobs on them (in any Kestrel). `steps`: what it's
// going to do, listed in the Jobs window (it sets task->step as it goes).
// intro: the first lines of its log (Task::intro).
// outlives_parent: on_done runs also when parent has gone meanwhile (it must then not need it).
// Jobs don't belong to a window: closing it doesn't stop them, and their errors are shown without it.
Task *queue_job(QWidget *parent, const QString &title, const QStringList &paths, Task::Fn fn, Done on_done = nullptr,
                bool cancellable = true, const QStringList &steps = QStringList(), bool quiet = false,
                const QStringList &intro = QStringList(), bool outlives_parent = false);
// Run fn() (no arguments) on a thread, shown as a busy item in the status bar unless quiet.
Task *run_task(QWidget *parent, const QString &title, std::function<QVariant()> fn, Done on_done = nullptr,
               bool quiet = false);

// op is "copy", "move", "merge_copy", "merge_move" or "delete"
struct Job {
    QString op, src, dst;
    // the user chose Replace or Merge for a dst that was there. Otherwise dst must be new: one that's there when the job
    // starts, or appears while it runs (another program saving a file), is kept and the job fails with a conflict.
    bool replace = false;
    bool dd = false;   // onto a tape: each file written by dd (the user's choice) instead of Kestrel's buffered copy
};

// Copy/move/delete jobs on a thread, with progress and Cancel in the status bar. Jobs that fail for lack of
// permission can be retried as administrator.
// With undo_label, the moves and copies that succeed (also when cancelled part-way) can be undone with Ctrl+Z; merges
// into existing folders and copies that replaced something can't (what was replaced is gone): the status bar says so,
// and when nothing else in the job can be undone an entry that can't be is recorded, so Ctrl+Z doesn't undo the action
// before it instead.
// A copy or move whose destination folder isn't there when its turn comes (moved or deleted while it waited) fails
// at once, with one message, before anything is written. intro: its log's first lines (Task::intro).
Task *start_ops(QWidget *parent, const QList<Job> &jobs, const QString &title, std::function<void()> on_done = nullptr,
                const QString &undo_label = QString(), const QStringList &intro = QStringList());

// Resolve conflicts interactively (a window for each, answered one after another); planned(jobs) then runs with the
// job list for start_ops, unless the user cancels or parent is closed meanwhile.
void plan_transfer(QWidget *parent, const QStringList &sources, const QString &dest_dir, const QString &op,
                   std::function<void(const QList<Job> &)> planned);
// Jobs writing files onto a tape: by Kestrel's buffered copy or by dd (Job::dd), as Preferences say (tape_writer), or as
// asked in a free window; go(jobs) runs with the choice made (not at all when the question is cancelled).
void choose_tape_writer(QWidget *parent, const QList<Job> &jobs, std::function<void(const QList<Job> &)> go);
// Copy or move sources into dest_dir (plan_transfer, choose_tape_writer, start_ops). how: what the user did ("Paste
// Into Folder", "Drop", "Move To…"), for the job's log, which also names the window, the time, and from where to where.
// on_started: once the job is made (not when planning was cancelled).
void transfer(QWidget *parent, const QStringList &sources, const QString &dest_dir, const QString &op,
              std::function<void()> on_done = nullptr, const QString &how = QString(),
              std::function<void()> on_started = nullptr);
// The job's title: "Copying “a.txt” to Photos", "Moving 3 items to Backup (with dd)"
QString transfer_title(const QStringList &sources, const QString &dest_dir, const QString &op, bool dd);
// "45 s", "2 min 5 s", "1 h 3 min"
QString wait_text(qint64 ms);

// Copy files from a device (needs_local_copy) into ~/.cache/kestrel-explorer/device-files, as a busy task ("will
// launch once ready") with Cancel, then call on_done(local paths) on the UI thread (not after a cancel or an error). A copy is reused while its
// size matches the file's; copies not opened for a day are deleted.
void fetch_local(QWidget *parent, const QStringList &paths, std::function<void(const QStringList &)> on_done);

// What creating a link of `kind` ("sym", "rel", "hard" or "desktop") to target in dest_dir means, as an
// admin-helper request ({"op": "symlink" | "hardlink" | "write", ...}). The name is made unique.
QVariantMap link_plan(const QString &kind, const QString &target, const QString &dest_dir);
QString plan_path(const QVariantMap &plan);
QString make_link(const QVariantMap &plan);   // as the current user; raises OSError

struct DirStats {
    qint64 size = 0, files = 0, dirs = 0;
};
// (total bytes, file count, dir count) recursively, not following symlinks
DirStats dir_stats(const QString &path, std::function<bool()> cancel = nullptr);

// Folder sizes worked out before (Properties), so they show at once: ~/.cache/kestrel-explorer/folder-sizes.json,
// keyed by path and checked against the folder's device and inode (another folder later under the same name isn't
// given them). Stored after a finished count only. Kestrel's own copies, moves and deletes drop the entries of the
// folders they change and of the folders above them. UI thread.
namespace dirsizes {
struct Entry {
    DirStats stats;
    qint64 measured = 0;   // when (seconds since the epoch)
};
QString cache_file();
std::optional<Entry> get(const QString &folder);
void put(const QString &folder, const DirStats &stats);
void drop_with_parents(const QStringList &paths);   // each path's own entry and every folder's above it
QString age_text(qint64 measured, qint64 now);      // "Calculated 3 days ago (2026-10-21 14:05)", "Calculated just now"
}  // namespace dirsizes

// What emptying the trash removes: every entry in the trash folders on every drive (files, info and expunged)
QStringList trash_contents();
// Shredding with BleachBit (`bleachbit --shred`): files and folders are overwritten, then deleted, so they can't be
// recovered. A background task; BleachBit reports success either way, so on_done gets the paths still there
// afterwards (none: all shredded). Not called after a cancel.
bool can_shred();   // BleachBit is installed
Task *shred(QWidget *parent, const QStringList &paths, const QString &title,
            std::function<void(const QStringList &left)> on_done = nullptr);

}  // namespace fileops

class ConflictDialog : public QDialog {
    Q_OBJECT
public:
    ConflictDialog(QWidget *parent, const QString &dst, bool is_dir);
    QString choice = "skip";
    QCheckBox *all_box;
};

Q_DECLARE_METATYPE(fileops::DirStats)
