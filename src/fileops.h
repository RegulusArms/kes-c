// File operations: threaded copy/move/delete with progress, the status-bar task panel, conflicts, links.
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QProgressBar>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <QWidget>

#include <atomic>
#include <functional>
#include <optional>

class QCheckBox;
class QLabel;
class QToolButton;

// A background job shown in its window's status bar (TaskPanel).
//
// fn(task) runs on the thread. It may call task->report(done, total, text) for progress (total 0 = busy, no
// percentage) and task->check() to stop early when the user cancels. Its return value goes to on_done; a
// Cancelled exception counts as a normal finish with an invalid QVariant and was_cancelled set.
class Task : public QThread {
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

    void cancel() { cancelled = true; }
    void check();   // throws Cancelled
    void report(double done, double total = 0, const QString &text = QString());

Q_SIGNALS:
    void progress(double fraction, const QString &text);   // fraction 0..1 or -1 for busy
    void result(const QVariant &res);
    void error(const QString &msg);

protected:
    void run() override;

private:
    Fn fn;
    qint64 last = 0;
};

// A running task as the shared task list knows it: one of ours (local), or another Kestrel's.
struct TaskInfo {
    QString flight, id, title, text;
    double fraction = -1.0;
    bool cancellable = false, cancelling = false, admin = false;
    Task *local = nullptr;
};

// Every running task: this Kestrel's (all its windows) and, through the tower (atc.h), the other Kestrels'. Ours are
// reported to the others whenever one starts or ends, and at most twice a second while they make progress.
class TaskBoard : public QObject {
    Q_OBJECT
public:
    static TaskBoard *instance();
    void add(Task *task);
    QList<TaskInfo> others(const QList<Task *> &mine) const;   // all but `mine`: our other windows', then other Kestrels'
    void cancel(const TaskInfo &task);   // another Kestrel's task: asks it to cancel

Q_SIGNALS:
    void changed();

private:
    TaskBoard();
    void schedule(int ms);
    void publish();
    void on_heard(const QJsonObject &msg);
    QList<Task *> local;
    QMap<QString, QList<TaskInfo>> remote;   // flight -> its tasks
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
// Run fn() (no arguments) on a thread, shown as a busy item in the status bar unless quiet.
Task *run_task(QWidget *parent, const QString &title, std::function<QVariant()> fn, Done on_done = nullptr,
               bool quiet = false);

// op is "copy", "move", "merge_copy", "merge_move" or "delete"
struct Job {
    QString op, src, dst;
    // the user chose Replace or Merge for a dst that was there. Otherwise dst must be new: one that's there when the job
    // starts, or appears while it runs (another program saving a file), is kept and the job fails with a conflict.
    bool replace = false;
};

// Copy/move/delete jobs on a thread, with progress and Cancel in the status bar. Jobs that fail for lack of
// permission can be retried as administrator.
// With undo_label, the moves and copies that succeed (also when cancelled part-way) can be undone with Ctrl+Z; merges
// into existing folders and copies that replaced something can't (what was replaced is gone): the status bar says so,
// and when nothing else in the job can be undone an entry that can't be is recorded, so Ctrl+Z doesn't undo the action
// before it instead.
Task *start_ops(QWidget *parent, const QList<Job> &jobs, const QString &title, std::function<void()> on_done = nullptr,
                const QString &undo_label = QString());

// Resolve conflicts interactively; the job list for start_ops (nullopt if cancelled).
std::optional<QList<Job>> plan_transfer(QWidget *parent, const QStringList &sources, const QString &dest_dir,
                                        const QString &op);
void transfer(QWidget *parent, const QStringList &sources, const QString &dest_dir, const QString &op,
              std::function<void()> on_done = nullptr);

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
