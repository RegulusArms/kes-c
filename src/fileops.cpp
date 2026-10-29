#include "fileops.h"

#include "admin.h"
#include "drives.h"
#include "jobs.h"
#include "prefs.h"
#include "atc.h"
#include "proc.h"
#include "undo.h"
#include "stats.h"
#include "tapebuf.h"
#include "util.h"
#include "windows.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QLabel>
#include <QMainWindow>
#include <QRegularExpression>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <sys/statvfs.h>

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <memory>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utime.h>

using namespace util;

static const qint64 CHUNK = 4 * 1024 * 1024;

static qint64 monotonic_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// ---------------------------------------------------------------- Task

Task::Task(const QString &title, Fn fn, bool cancellable) : title(title), cancellable(cancellable), fn(std::move(fn))
{
    static int next_id = 0;
    id = QString::number(++next_id);
}

static thread_local Task *running_task = nullptr;

Task *Task::current() { return running_task; }

qint64 Task::now_ms() { return monotonic_ms(); }

qint64 Task::elapsed_ms() const { return started_ms ? monotonic_ms() - started_ms : 0; }

void Task::cancel()
{
    cancelled = true;
    if (pause_flag)   // a paused job (and its programs) carries on to its cancel
        set_paused(false);
    Q_EMIT cancel_asked();
}

void Task::check()
{
    if (cancelled)
        throw Cancelled();
    if (pause_flag) {
        QMutexLocker l(&mtx);
        while (pause_flag && !cancelled)
            resumed.wait(&mtx, 200);
    }
    if (cancelled)
        throw Cancelled();
}

void Task::set_paused(bool on)
{
    if (on && !pausable())
        return;
    QMutexLocker l(&mtx);
    pause_flag = on;
    for (pid_t pg : programs)
        ::killpg(pg, on ? SIGSTOP : SIGCONT);
    if (!on)
        resumed.wakeAll();
}

void Task::started(pid_t pid)
{
    QMutexLocker l(&mtx);
    programs.insert(pid);
    if (pause_flag)   // started while paused (between two checks): stopped at once
        ::killpg(pid, SIGSTOP);
}

void Task::ended(pid_t pid)
{
    QMutexLocker l(&mtx);
    programs.remove(pid);
}

void Task::log(const QString &line)
{
    QMutexLocker l(&mtx);
    if (head.size() < 300) {
        head << line;
        return;
    }
    tail << line;
    if (tail.size() > 3000) {
        tail.removeFirst();
        ++skipped;
    }
}

void Task::log_output(const QByteArray &chunk)
{
    QByteArray all;
    {
        QMutexLocker l(&mtx);
        all = partial + chunk;
        partial.clear();
    }
    // lines end with \n, or with \r for a tool redrawing its progress line (kept once, the last state of it)
    QList<QByteArray> lines = all.split('\n');
    QByteArray rest = lines.takeLast();
    for (const QByteArray &line : lines) {
        QByteArray shown = line.split('\r').last();
        if (!shown.trimmed().isEmpty())
            log(QString::fromUtf8(shown));
    }
    QMutexLocker l(&mtx);
    partial = rest.size() > 4096 ? rest.right(4096) : rest;
}

QString Task::log_text() const
{
    QMutexLocker l(&mtx);
    QStringList out = head;
    if (skipped)
        out << txn("… %Ln line(s) not kept …", skipped);
    out << tail;
    if (!partial.trimmed().isEmpty())
        out << QString::fromUtf8(partial.split('\r').last());
    return out.join('\n');
}

void Task::report(double done, double total, const QString &t, bool force)
{
    // at most ~12 updates a second, so thousands of tiny files can't flood the UI thread
    qint64 now = monotonic_ms();
    if (now - last < 80 && !force)
        return;
    last = now;
    Q_EMIT progress(total > 0 ? std::min(done / total, 1.0) : -1.0, t);
}

void Task::run()
{
    QVariant res;
    running_task = this;
    proc::set_watcher(this);
    started_ms = monotonic_ms();
    for (const QString &line : intro)
        log(line);
    if (queued_ms && started_ms - queued_ms >= 1000)
        log(waited_for.isEmpty() ? tx("Waited %1").arg(fileops::wait_text(started_ms - queued_ms))
                                 : tx("Waited %1 for: %2").arg(fileops::wait_text(started_ms - queued_ms), waited_for.join("; ")));
    try {
        check();   // cancelled while it waited in the queue: it ends at once
        res = fn(this);
    } catch (const Cancelled &) {
        was_cancelled = true;
        res = QVariant();
    } catch (const std::exception &e) {
        failed = true;
        log(tx("Error: %1").arg(QString::fromStdString(e.what())));
        proc::set_watcher(nullptr);
        running_task = nullptr;
        Q_EMIT error(QString::fromStdString(e.what()));
        return;
    }
    proc::set_watcher(nullptr);
    running_task = nullptr;
    Q_EMIT result(res);
}

// ---------------------------------------------------------------- TaskBoard

TaskBoard *TaskBoard::instance()
{
    static TaskBoard *b = new TaskBoard;
    return b;
}

TaskBoard::TaskBoard()
{
    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, &TaskBoard::publish);
    connect(atc::radio(), &atc::Radio::heard, this, &TaskBoard::on_heard);
    connect(atc::radio(), &atc::Radio::reset, this, [this]() {
        remote.clear();
        Q_EMIT changed();
    });
}

QList<Task *> TaskBoard::keeping() const
{
    QList<Task *> out;
    for (Task *t : local)
        if (t->keeps_running)
            out << t;
    return out;
}

void TaskBoard::add(Task *task)
{
    if (local.contains(task))   // (handed to another window's status bar)
        return;
    local << task;
    stats::peak("tasks at once", local.size());
    connect(task, &Task::progress, this, [this]() {
        schedule(500);
        Q_EMIT changed();
    });
    connect(task, &QThread::finished, this, [this, task]() {
        local.removeAll(task);
        QString log = task->log_text();
        QString state = task->was_cancelled ? "cancelled"
                        : task->failed      ? "failed"
                        : task->had_errors || log.contains("  failed: ") ? "done with errors"   // (the admin helper's lines)
                                                     : "done";
        finished << FinishedTask{task->title, state, log, task->steps, task->elapsed_ms(), QDateTime::currentDateTime(),
                                 task->admin};
        while (finished.size() > 50)
            finished.removeFirst();
        schedule(0);
        Q_EMIT changed();
    });
    schedule(0);
    Q_EMIT changed();
}

void TaskBoard::clear_finished()
{
    finished.clear();
    Q_EMIT changed();
}

void TaskBoard::schedule(int ms)
{
    // report soon (a task started, ended or was cancelled) or within ms (progress); never more often than that
    if (!timer->isActive() || timer->remainingTime() > ms)
        timer->start(ms);
}

void TaskBoard::publish()
{
    QJsonArray list;
    for (Task *t : local)
        list << QJsonObject{{"id", t->id},
                            {"title", t->title},
                            {"text", t->text},
                            {"fraction", t->fraction},
                            {"cancellable", t->cancellable},
                            {"cancelling", bool(t->cancelled)},
                            {"admin", t->admin},
                            {"state", t->queued ? "waiting" : t->paused() ? "paused" : "running"}};
    atc::announce("tasks", {{"tasks", list}}, true);
}

QList<TaskInfo> TaskBoard::others(const QList<Task *> &mine) const
{
    QList<TaskInfo> out;
    QString me = atc::radio()->flight();
    for (Task *t : local)
        if (!mine.contains(t))
            out << TaskInfo{me, t->id, t->title, t->text, t->fraction, t->cancellable, bool(t->cancelled), t->admin, t,
                            t->queued ? "waiting" : t->paused() ? "paused" : "running"};
    for (const QList<TaskInfo> &l : remote)
        out << l;
    return out;
}

void TaskBoard::cancel(const TaskInfo &task)
{
    if (task.local) {
        if (local.contains(task.local))
            task.local->cancel();
        schedule(0);
    } else {
        // the Kestrel running it does the cancelling (its admin session stays its own)
        atc::announce("cancel", {{"flight", task.flight}, {"task", task.id}});
        for (TaskInfo &t : remote[task.flight])
            if (t.id == task.id)
                t.cancelling = true;
    }
    Q_EMIT changed();
}

void TaskBoard::on_heard(const QJsonObject &m)
{
    if (m.value("own").toBool())
        return;
    QString type = m.value("type").toString(), from = m.value("from").toString();
    if (type == "tasks") {
        QList<TaskInfo> l;
        for (const QJsonValue &v : m.value("tasks").toArray()) {
            QJsonObject o = v.toObject();
            l << TaskInfo{from,
                          o.value("id").toString(),
                          o.value("title").toString(),
                          o.value("text").toString(),
                          o.value("fraction").toDouble(-1.0),
                          o.value("cancellable").toBool(),
                          o.value("cancelling").toBool(),
                          o.value("admin").toBool(),
                          nullptr,
                          o.value("state").toString("running")};
        }
        if (l.isEmpty())
            remote.remove(from);
        else
            remote[from] = l;
        Q_EMIT changed();
    } else if (type == "left") {
        if (remote.remove(from))
            Q_EMIT changed();
    } else if (type == "cancel" && m.value("flight").toString() == atc::radio()->flight()) {
        for (Task *t : local)
            if (t->id == m.value("task").toString() && t->cancellable)
                t->cancel();
        schedule(0);
        Q_EMIT changed();
    }
}

// ---------------------------------------------------------------- TaskPanel

PulseBar::PulseBar(QWidget *parent) : QProgressBar(parent)
{
    frame.setInterval(16);
    connect(&frame, &QTimer::timeout, this, qOverload<>(&QWidget::update));
}

void PulseBar::set_busy(bool on)
{
    if (on == pulsing)
        return;
    pulsing = on;
    if (on) {
        clock.start();
        frame.start();
    } else {
        frame.stop();
    }
    update();
}

void PulseBar::paintEvent(QPaintEvent *ev)
{
    if (!pulsing) {
        QProgressBar::paintEvent(ev);
        return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(palette().color(QPalette::Mid));
    p.setBrush(palette().color(QPalette::Base));
    p.drawRoundedRect(r, 3, 3);
    // there and back every 2.4 s, slowing at the ends
    double t = (clock.elapsed() % 2400) / 2400.0;
    double x = 0.5 - 0.5 * std::cos(2 * M_PI * t);
    double w = r.width() * 0.3;
    QRectF block(r.x() + 1.5 + x * (r.width() - 3 - w), r.y() + 1.5, w, r.height() - 3);
    p.setPen(Qt::NoPen);
    p.setBrush(palette().color(QPalette::Highlight));
    p.drawRoundedRect(block, 2, 2);
}

TaskPanel::TaskPanel(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);
    label = new QLabel;
    bar = new PulseBar;
    bar->setFixedWidth(200);
    bar->setMaximumHeight(16);
    bar->setRange(0, 1000);
    stop = new QToolButton;
    stop->setText("✕");
    stop->setAutoRaise(true);
    connect(stop, &QToolButton::clicked, this, &TaskPanel::cancel_first);
    lay->addWidget(label);
    lay->addWidget(bar);
    lay->addWidget(stop);
    hide();
    connect(TaskBoard::instance(), &TaskBoard::changed, this, &TaskPanel::refresh);
}

void TaskPanel::mousePressEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton)
        Q_EMIT clicked();
    QWidget::mousePressEvent(ev);
}

void TaskPanel::add(Task *task)
{
    tasks << task;
    connect(task, &Task::progress, this, [this, task](double f, const QString &t) { on_progress(task, f, t); });
    connect(task, &QThread::finished, this, [this, task]() { on_finished(task); });
    TaskBoard::instance()->add(task);
    refresh();
}

void TaskPanel::on_progress(Task *task, double fraction, const QString &text)
{
    task->fraction = fraction;
    task->text = text;
    if (!tasks.isEmpty() && task == tasks.first())
        refresh();
}

void TaskPanel::on_finished(Task *task)
{
    tasks.removeAll(task);
    refresh();
}

void TaskPanel::cancel_first()
{
    if (!tasks.isEmpty()) {
        tasks.first()->cancel();
        refresh();
        return;
    }
    QList<TaskInfo> others = TaskBoard::instance()->others(tasks);
    if (!others.isEmpty())
        TaskBoard::instance()->cancel(others.first());
}

void TaskPanel::refresh()
{
    QList<TaskInfo> others = TaskBoard::instance()->others(tasks);
    if (tasks.isEmpty() && others.isEmpty()) {
        hide();
        return;
    }
    auto line = [](const TaskInfo &t) {
        QString title = (t.admin ? "🛡 " : "") + t.title;
        return t.text.isEmpty() ? title : tx("%1: %2", "a job and how far it is").arg(title, t.text);
    };
    TaskInfo first;
    bool elsewhere = false;
    if (!tasks.isEmpty()) {
        Task *t = tasks.first();   // the first one running (a waiting one only when none is)
        for (Task *x : tasks)
            if (!x->queued) {
                t = x;
                break;
            }
        first = TaskInfo{QString(), t->id, t->title, t->text, t->fraction, t->cancellable, bool(t->cancelled), false, t,
                         t->queued ? "waiting" : t->paused() ? "paused" : "running"};
    } else {
        first = others.takeFirst();
        elsewhere = true;
    }
    QString shown = (first.admin && !first.local ? "🛡 " : "") + first.title;
    if (elsewhere)
        shown = tx("%1 (in another window)").arg(shown);
    QString text = first.cancelling          ? tx("%1 — cancelling…").arg(shown)
                   : first.state == "paused" ? tx("%1 — paused").arg(shown)
                   : !first.text.isEmpty()   ? tx("%1: %2", "a job and how far it is").arg(shown, first.text)
                                             : shown;
    QStringList more;
    if (tasks.size() > 1)
        more << txn("+%Ln more", tasks.size() - 1);
    if (!others.isEmpty())
        more << txn("+%Ln in other windows", others.size());
    label->setText(label->fontMetrics().elidedText(text, Qt::ElideMiddle, 380) +
                   (more.isEmpty() ? QString() : "  (" + more.join(", ") + ")"));
    QStringList tips;
    for (Task *x : tasks)
        tips << (x->text.isEmpty() ? x->title : tx("%1: %2", "a job and how far it is").arg(x->title, x->text));
    if (elsewhere)
        tips << tx("%1 — in another window").arg(line(first));
    for (const TaskInfo &x : others)
        tips << tx("%1 — in another window").arg(line(x));
    setToolTip(tips.join('\n'));
    bar->set_busy(first.fraction < 0);
    if (first.fraction >= 0)
        bar->setValue(int(first.fraction * 1000));
    setToolTip(toolTip() + "\n\n" + tx("Click for the Jobs window"));
    stop->setVisible(first.cancellable);
    stop->setEnabled(!first.cancelling);
    stop->setToolTip(elsewhere ? tx("Cancel “%1” (in another window)").arg(first.title) : tx("Cancel “%1”").arg(first.title));
    show();
}

namespace fileops {

// The TaskPanel of the main window that `widget` belongs to (or the active/first main window's).
static TaskPanel *panel_for(QWidget *widget)
{
    auto panel_of = [](QWidget *w) -> TaskPanel * {
        return w ? qobject_cast<TaskPanel *>(w->property("task_panel").value<QObject *>()) : nullptr;
    };
    for (QWidget *w = widget; w; w = w->parentWidget())
        if (TaskPanel *p = panel_of(w))
            return p;
    if (TaskPanel *p = panel_of(QApplication::activeWindow()))
        return p;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (TaskPanel *p = panel_of(w))
            return p;
    return nullptr;
}

// A job's failure is told also when the window that started it has closed (with no parent, then)
static void tell_errors(Task *t, QWidget *parent, const QString &title)
{
    QPointer<QWidget> p(parent);
    QObject::connect(t, &Task::error, t, [p, title](const QString &msg) { warn(p, title, msg); });
    t->keeps_running = std::make_unique<QEventLoopLocker>();
}

Task *run_job(QWidget *parent, const QString &title, Task::Fn fn, Done on_done, bool cancellable, bool quiet)
{
    auto *t = new Task(title, std::move(fn), cancellable);
    QObject *ctx = parent ? static_cast<QObject *>(parent) : static_cast<QObject *>(t);
    if (on_done)
        QObject::connect(t, &Task::result, ctx, [on_done](const QVariant &res) { on_done(res); });
    if (!quiet) {
        tell_errors(t, parent, title);
        if (TaskPanel *panel = panel_for(parent))
            panel->add(t);
    }
    QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
    return t;
}

Task *queue_job(QWidget *parent, const QString &title, const QStringList &paths, Task::Fn fn, Done on_done,
                bool cancellable, const QStringList &steps, bool quiet, const QStringList &intro, bool outlives_parent)
{
    auto *t = new Task(title, std::move(fn), cancellable);
    t->paths = paths;
    t->steps = steps;
    t->intro = intro;
    QObject *ctx = parent && !outlives_parent ? static_cast<QObject *>(parent) : static_cast<QObject *>(t);
    if (on_done)
        QObject::connect(t, &Task::result, ctx, [on_done](const QVariant &res) { on_done(res); });
    if (!quiet) {
        tell_errors(t, parent, title);
        if (TaskPanel *panel = panel_for(parent))
            panel->add(t);
    }
    QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    jobs::Queue::instance()->submit(t);
    return t;
}

Task *run_task(QWidget *parent, const QString &title, std::function<QVariant()> fn, Done on_done, bool quiet)
{
    return run_job(parent, title, [fn](Task *) { return fn(); }, std::move(on_done), false, quiet);
}

// ---------------------------------------------------------------- copy / move / delete

// Give the owner rwx on every folder under path that this user owns (so its entries can be deleted).
// Returns true if anything changed.
static bool make_writable(const QString &path)
{
    uid_t uid = ::getuid();
    bool changed = false;
    walk(path, [&](const QString &root, QStringList &dirs, QStringList &) {
        QStringList all{root};
        for (const QString &d : dirs)
            all << join(root, d);
        for (const QString &d : all) {
            struct stat st;
            if (lstat_(d, st) && st.st_uid == uid && !S_ISLNK(st.st_mode) && (st.st_mode & 0700) != 0700) {
                if (::chmod(enc(d).constData(), (st.st_mode & 07777) | 0700) == 0)
                    changed = true;
            }
        }
        return true;
    });
    return changed;
}

// Within a tree, copies and deletes work through open folders, by name, never by path again: each folder is opened
// without following a symlink and must still be the folder that was listed, so a folder swapped for a symlink while
// the job runs (in /tmp, a shared folder, a USB drive) stops the job instead of leading it into the symlink's target.
// The path you chose itself, symlinked folders on the way included, is used as it is. A delete doesn't cross into
// another drive mounted inside the tree.
struct Fd {
    explicit Fd(int fd) : fd(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd &) = delete;
    void reset()
    {
        if (fd >= 0)
            ::close(fd);
        fd = -1;
    }
    int release()
    {
        int f = fd;
        fd = -1;
        return f;
    }
    int fd;
};

static int open_parent(const QString &path)   // the folder a path's last part is in
{
    QString dir = dirname(path);
    int fd = ::open(enc(dir.isEmpty() ? QString("/") : dir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        throw_errno(dir);
    return fd;
}

static bool stat_at(int dirfd, const QString &name, struct stat &st)   // lstat of a name in an open folder
{
    return ::fstatat(dirfd, enc(name).constData(), &st, AT_SYMLINK_NOFOLLOW) == 0;
}

static int open_dir_at(int dirfd, const QString &name, const struct stat &st, const QString &shown)
{
    int fd = ::openat(dirfd, enc(name).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        throw_errno(shown);
    struct stat now;
    if (::fstat(fd, &now) != 0 || now.st_dev != st.st_dev || now.st_ino != st.st_ino) {
        ::close(fd);
        throw Error(tx("%1 changed while it was being worked on").arg(shown));
    }
    return fd;
}

static QStringList names_in(int dirfd, const QString &shown)   // the names in an open folder
{
    int dup_fd = ::fcntl(dirfd, F_DUPFD_CLOEXEC, 0);
    DIR *d = dup_fd >= 0 ? ::fdopendir(dup_fd) : nullptr;
    if (!d) {
        int e = errno;
        if (dup_fd >= 0)
            ::close(dup_fd);
        throw_errno(shown, e);
    }
    ::rewinddir(d);
    QStringList out;
    while (struct dirent *e = ::readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            out << dec(e->d_name);   // as util::listdir: names that aren't valid UTF-8 survive the round trip
    ::closedir(d);
    return out;
}

static void copy_times_mode(int fd, const struct stat &st)   // on a descriptor: never through a symlink
{
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    ::futimens(fd, times);
    ::fchmod(fd, st.st_mode & 07777);
}

// a job as a step in the Jobs window: "Copy photo.jpg → /media/…/Backup"
static QString step_text(const Job &j)
{
    static const QHash<QString, const char *> steps = {{"copy", TX_NOOP("Copy %1 → %2")}, {"move", TX_NOOP("Move %1 → %2")},
                                                       {"merge_copy", TX_NOOP("Copy (merge) %1 → %2")},
                                                       {"merge_move", TX_NOOP("Move (merge) %1 → %2")}};
    if (j.op == "delete")
        return tx("Delete %1").arg(j.src);
    return steps.contains(j.op) ? tx(steps.value(j.op)).arg(basename(j.src), j.dst)
                                : QString("%1 %2 → %3").arg(j.op, basename(j.src), j.dst);
}

// Runs a list of jobs for a Task. Progress is in bytes, or in files when every job is a delete.
class Ops {
public:
    Ops(Task *task, const QList<Job> &jobs) : task(task), jobs(jobs)
    {
        by_count = std::all_of(jobs.begin(), jobs.end(), [](const Job &j) { return j.op == "delete"; });
    }

    QStringList errors;
    QList<Job> denied;      // jobs that failed for lack of permission, to retry as administrator
    QList<Job> completed;   // jobs that succeeded (for undo)
    QStringList partial;    // new folders a copy left part-way (cancelled, or an error inside): undone like a copy
    QList<Job> touched;     // jobs that changed something (done, or begun): a tape among them gets its index written
    int replaced = 0, merged = 0;   // copies that replaced something and merges done: neither can be undone

    QStringList run()
    {
        task->report(0, 0, tx("Counting…"));
        for (const Job &j : jobs) {
            if (j.op == "delete" && !by_count)
                continue;
            if (!(j.op == "move" && same_dev(j.src, j.dst)))
                total += size_of(j.src);
        }
        total = std::max<qint64>(total, 1);
        qint64 started = stats::now_ms();
        // jobs reading a tape go in the order their files lie on the tape (by partition and block), so the drive
        // streams instead of winding back and forth; the others keep their order, after them
        QList<QPair<QPair<qint64, qint64>, int>> keyed;   // (tape position, the job's index: its step)
        for (int i = 0; i < jobs.size(); ++i) {
            const Job &j = jobs[i];
            QPair<qint64, qint64> at{LLONG_MAX, LLONG_MAX};
            if (drives::slow_kind(j.src) == drives::Slow::Tape) {
                at = drives::tape_position(j.src);
                if (at.first < 0)
                    at = {LLONG_MAX - 1, LLONG_MAX - 1};
            }
            keyed << qMakePair(at, i);
        }
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        start_tape_buffer(keyed);
        struct EndBuffer {   // its figures in the log, and its memory given back, however the run ends
            Ops *ops;
            ~EndBuffer()
            {
                if (ops->buffer) {
                    ops->task->log(ops->buffer->summary());
                    ops->buffer.reset();
                }
                if (!ops->quieted.isEmpty()) {   // GNOME's check may look at the tape again
                    drives::unquiet_tape(ops->quieted);
                    ops->quieted.clear();
                }
            }
        } end_buffer{this};
        for (const auto &[at, index] : keyed) {
            const Job &j = jobs[index];
            task->check();
            task->step = index;
            dd_now = j.dd;
            task->log(step_text(j));
            // a copy that replaces something can't be undone: the old item is gone, and the copy is all that's left
            bool replacing = j.op == "copy" && lexists(j.dst);
            bool claim = !j.replace && !j.op.startsWith("merge");   // a merge always goes into an existing folder
            bool fresh = j.op == "copy" && !lexists(j.dst);
            touched << j;
            auto left_part_way = [&]() {
                if (fresh && isdir(j.dst) && !islink(j.dst))
                    partial << j.dst;
            };
            try {
                if (j.op == "delete")
                    remove(j.src);
                else if (j.op == "move" || j.op == "merge_move")
                    move(j.src, j.dst, j.op == "merge_move", claim);
                else
                    copy(j.src, j.dst, j.op == "merge_copy", claim);
                if (replacing) {
                    ++replaced;
                    task->log(tx("  done (replaced what was there)"));
                } else if (j.op == "merge_copy" || j.op == "merge_move") {
                    ++merged;
                    task->log(tx("  done (merged into the folder there)"));
                } else {
                    completed << j;
                    task->log(tx("  done"));
                }
            } catch (const OSError &e) {
                task->log(tx("  failed: %1").arg(e.message()));
                task->had_errors = true;
                if (e.permission()) {
                    // retried as administrator; a new folder already begun is finished by merging into it
                    Job retry = j;
                    if (fresh && isdir(j.dst) && !islink(j.dst))
                        retry.op = "merge_copy";
                    denied << retry;
                } else {
                    left_part_way();
                    errors << basename(j.src) + ": " + e.message();
                }
            } catch (const Error &e) {
                task->log(tx("  failed: %1").arg(e.message()));
                task->had_errors = true;
                left_part_way();
                errors << basename(j.src) + ": " + e.message();
            } catch (const Cancelled &) {
                task->log(tx("  cancelled"));
                left_part_way();
                throw;
            }
        }
        qint64 ms = stats::now_ms() - started;
        if (!by_count && done > 0 && ms > 0)
            stats::sample("copy speed (MB/s)", done / 1e6 / (ms / 1000.0));
        return errors;
    }

private:
    Task *task;
    QList<Job> jobs;
    bool by_count;
    qint64 done = 0, total = 0;
    std::unique_ptr<tapebuf::Buffer> buffer;   // copying onto a tape (tapebuf.h)
    QString quieted;                           // the tape GNOME's low-disk-space check skips while this runs
    QStringList told_pollers;                  // programs asking every drive for its free space, said in the log
    bool dd_now = false;                       // the job being run writes with dd (Job::dd)
    bool told_slow = false;                    // said the source is too slow for the tape to stream
    QElapsedTimer rate_clock;                  // copying onto a tape: since the last rate line in the log
    qint64 rate_done = 0;                      // ...and the bytes written by then

    // Copying onto a tape: every 5 seconds a line in the job's log (the Jobs window shows it as it comes), with the time
    // (to set beside the drive's own logs), how fast the tape took the data in those 5 seconds, the source's read speed,
    // the buffer, and how far the copy is: a drive that slows down shows when and for how long.
    void log_tape_rate()
    {
        if (!buffer)
            return;
        if (!rate_clock.isValid()) {
            rate_clock.start();
            rate_done = done;
            return;
        }
        qint64 ms = rate_clock.elapsed();
        if (ms < 5000)
            return;
        qint64 wrote = done - rate_done;
        qint64 cap = std::max<qint64>(buffer->capacity(), 1);
        QString tape = wrote > 0 ? tx("%1/s").arg(human_size(qint64(double(wrote) * 1000.0 / double(ms))))
                                 : tx("waiting for the buffer");
        double src = buffer->source_rate();
        task->log(tx("  %1  tape %2 · source %3 · buffer %4 of %5 (%6%) · %7 of %8 written")
                      .arg(QTime::currentTime().toString("HH:mm:ss"), tape,
                           src > 0 ? tx("%1/s").arg(human_size(qint64(src))) : QString("—"), human_size(buffer->filled()),
                           human_size(cap), i18n::number(buffer->filled() * 100 / cap), human_size(done),
                           human_size(total)));
        rate_clock.restart();
        rate_done = done;
        check_pollers();   // (one opened meanwhile)
    }

    void report_progress(const QString &name)
    {
        QString text = by_count ? tx("%1 of %2 — %3").arg(group_digits(done), group_digits(total), name)
                                : tx("%1 of %2 — %3").arg(human_size(done), human_size(total), name);
        if (buffer)
            text += " · " + buffer->status();
        task->report(double(done), double(total), text);
    }

    // The regular files under src, in the order copy_at writes them (names_in's order, as it reads them)
    static void plan_files(const QString &src, QStringList &out, Task *task)
    {
        task->check();
        struct stat st;
        if (!lstat_(src, st))
            return;
        if (S_ISREG(st.st_mode)) {
            out << src;
        } else if (S_ISDIR(st.st_mode)) {
            int fd = ::open(enc(src).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0)
                return;
            QStringList names;
            try {
                names = names_in(fd, src);
            } catch (const OSError &) {
            }
            ::close(fd);
            for (const QString &e : names)
                plan_files(join(src, e), out, task);
        }
    }

    // Copies onto a tape go through a RAM buffer read ahead by a thread (tapebuf.h), so the drive keeps streaming:
    // the files of every job writing to a tape from another drive, in the order they'll be written
    void start_tape_buffer(const QList<QPair<QPair<qint64, qint64>, int>> &order)
    {
        QStringList files;
        QString tape;
        for (const auto &entry : order) {
            const Job &j = jobs[entry.second];
            if (j.op == "delete" || (j.op == "move" && same_dev(j.src, j.dst)))
                continue;
            if (drives::slow_kind(j.dst) != drives::Slow::Tape || drives::slow_kind(j.src) == drives::Slow::Tape)
                continue;
            tape = drives::slow_root(j.dst);
            if (!j.dd)   // dd reads each file itself
                plan_files(j.src, files, task);
        }
        if (tape.isEmpty())
            return;
        if (drives::quiet_tape(tape)) {
            quieted = tape;
            task->log(tx("While this job runs, GNOME's low-disk-space check skips this tape: it asks the drive for its free "
                         "space once a minute, and each time the tape stops streaming."));
        }
        check_pollers();
        if (files.isEmpty())
            return;
        tapebuf::Memory mem = tapebuf::memory();
        buffer = std::make_unique<tapebuf::Buffer>(task, files);
        buffer->waiting = [this](const QString &text) {
            task->report(double(done), double(total), text, true);
            log_tape_rate();
        };
        task->log(tx("Tape buffer: up to %1 (memory %2, %3 available)")
                      .arg(human_size(buffer->capacity()), human_size(mem.total), human_size(mem.available)));
        if (int every = drives::ltfs_sync_minutes(drives::ltfs_sync(tape)))
            task->log(tx("Note: LTFS writes this tape's index every %1 minutes while it's mounted (sync_type=%2), and "
                              "the drive stops each time. Mounting it with -o sync_type=unmount avoids that (the index is "
                              "then written when it's unmounted, or with Write Tape Index Now).")
                          .arg(QString::number(every), drives::ltfs_sync(tape)));
    }

    // A file onto a tape written by dd (the user's choice): dd reads the source and writes the destination Kestrel has
    // opened (its claimed name, or a part file), given to it as its input and output, so the claim, the conflict checks
    // and the clean-up of a failed or cancelled write are Kestrel's as for its own copy. 512 KB blocks, LTFS's. Its
    // output shows in the Jobs window as it runs; cancelling stops it, pausing the job pauses it.
    void copy_with_dd(int in_fd, int out_fd, const QString &src, const QString &dst, qint64 size, qint64 &written)
    {
        QStringList argv{"dd", "bs=512K", "iflag=fullblock", "status=progress"};
        task->log(QString("  %1 < “%2” > “%3”").arg(argv.join(' '), src, dst));
        proc::Options o;
        o.in = in_fd;
        o.out = out_fd;
        o.err = proc::PIPE;
        o.c_utf8 = true;   // its progress and errors in English, to read them
        auto p = proc::spawn(argv, o);
        static const QRegularExpression bytes_re("(\\d+) bytes");
        qint64 base = done;
        QByteArray err;
        QElapsedTimer since_look;
        since_look.start();
        auto take = [&](const QByteArray &chunk) {
            task->log_output(chunk);   // the Jobs window shows it live
            err += chunk;
            if (err.size() > 65536)
                err.remove(0, err.size() - 16384);
            QRegularExpressionMatchIterator it = bytes_re.globalMatch(QString::fromLatin1(err.right(256)));
            qint64 n = -1;
            while (it.hasNext())
                n = it.next().captured(1).toLongLong();
            if (n >= 0) {
                written = n;
                done = base + n;
            }
        };
        for (;;) {
            task->check();   // a cancel throws: p goes, and dd with it
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(p->err, &rf);
            struct timeval tv = {0, 200000};
            int r = ::select(p->err + 1, &rf, nullptr, nullptr, &tv);
            if (r > 0) {
                char buf[4096];
                ssize_t n = ::read(p->err, buf, sizeof buf);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    break;
                take(QByteArray(buf, int(n)));
                report_progress(basename(src));
            } else if (p->poll()) {
                break;
            }
            if (since_look.elapsed() >= 5000) {   // (one opened meanwhile)
                since_look.restart();
                check_pollers();
            }
        }
        take(p->read_all_err());
        if (p->wait() != 0) {
            QStringList lines;
            for (const QString &l : QString::fromUtf8(err).split(QRegularExpression("[\\r\\n]"), Qt::SkipEmptyParts))
                if (!l.contains(" bytes ") && !l.contains("records "))
                    lines << l.trimmed();
            throw Error(tx("dd couldn't write “%1”: %2")
                            .arg(basename(dst), lines.isEmpty() ? tx("it failed") : lines.mid(lines.size() - 2).join(" ")));
        }
        written = size;
        done = base + size;
    }

    // A program asking every drive for its free space every few seconds slows the tape: said in the log, once each
    void check_pollers()
    {
        for (const QString &p : drives::free_space_pollers()) {
            if (told_pollers.contains(p))
                continue;
            told_pollers << p;
            task->log(tx("Note: %1 is running. Programs like it ask every drive for its free space every few "
                              "seconds (GNOME System Monitor in its File Systems tab); on a tape each question stops it "
                              "streaming, and it can run at a fraction of its speed for a while after. Close it while "
                              "the tape is written.")
                          .arg(p));
        }
    }

    // bytes under path, or the file count when counting deletes
    qint64 size_of(const QString &path)
    {
        struct stat st;
        if (!lstat_(path, st))
            return 0;
        if (!S_ISDIR(st.st_mode))
            return by_count ? 1 : st.st_size;
        qint64 n = by_count ? 1 : 0;
        walk(path, [&](const QString &root, QStringList &dirs, QStringList &files) {
            task->check();
            if (by_count) {
                n += files.size() + dirs.size();
                return true;
            }
            for (const QString &f : files) {
                struct stat fst;
                if (lstat_(join(root, f), fst))
                    n += fst.st_size;
            }
            return true;
        });
        return n;
    }

    static bool same_dev(const QString &src, const QString &dst)
    {
        struct stat a, b;
        return lstat_(src, a) && stat_(dirname(dst), b) && a.st_dev == b.st_dev;
    }

    void move(const QString &src, const QString &dst, bool merge, bool claim)
    {
        if (!merge && same_dev(src, dst)) {
            struct stat st, dst_st;
            if (claim && lstat_(dst, dst_st))
                throw conflict(dst);
            if (claim) {   // a new name: claimed, never replacing what another program put there meanwhile
                Fd sp(open_parent(src)), dp(open_parent(dst));
                claim_name(sp.fd, basename(src), dp.fd, basename(dst), dst);
            } else if (lstat_(dst, dst_st)) {
                // replaced in one step (swap_in); the old one is deleted once the moved one is in place
                if (lstat_(src, st) && S_ISDIR(dst_st.st_mode) && !S_ISDIR(st.st_mode))
                    throw Error(tx("%1 is a folder").arg(dst));
                Fd sp(open_parent(src)), dp(open_parent(dst));
                swap_in(sp.fd, basename(src), dp.fd, basename(dst), dst);
            } else {
                util::rename(src, dst);
            }
            report_progress(basename(src));
            return;
        }
        copy(src, dst, merge, claim);
        remove(src);
    }

    // Delete a file or tree one entry at a time, so it can report progress and be cancelled.
    // Read-only folders you own (common in extracted Windows archives) are made writable and retried.
    void remove(const QString &path)
    {
        try {
            remove_tree(path);
        } catch (const OSError &) {
            if (!(isdir(path) && !islink(path)) || !make_writable(path))
                throw;
            remove_tree(path);
        }
    }

    // The tree is worked on through open folders, by name (see "Within a tree" above the class)
    void remove_tree(const QString &path)
    {
        struct stat st;
        if (!lstat_(path, st))
            throw_errno(path);
        if (S_ISDIR(st.st_mode)) {
            Fd parent(open_parent(path));
            remove_at(parent.fd, basename(path), st, path, st.st_dev);
        } else if (::unlink(enc(path).constData()) != 0) {
            throw_errno(path);
        }
        count(basename(path));
    }

    // counted: reports progress and stops at a cancel (not when deleting what was just replaced, or a half-made part)
    void remove_at(int dirfd, const QString &name, const struct stat &st, const QString &shown, dev_t dev,
                   bool counted = true)
    {
        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev != dev)
                throw Error(tx("%1 is on another drive (a mount point); not deleting it").arg(shown));
            {
                Fd d(open_dir_at(dirfd, name, st, shown));
                for (const QString &e : names_in(d.fd, shown)) {
                    if (counted)
                        task->check();
                    QString p = join(shown, e);
                    struct stat est;
                    if (!stat_at(d.fd, e, est))
                        throw_errno(p);
                    remove_at(d.fd, e, est, p, dev, counted);
                    if (counted)
                        count(e);
                }
            }
            if (::unlinkat(dirfd, enc(name).constData(), AT_REMOVEDIR) != 0)
                throw_errno(shown);
        } else if (::unlinkat(dirfd, enc(name).constData(), 0) != 0) {   // a file or a symlink: the name itself
            throw_errno(shown);
        }
    }

    void count(const QString &name)
    {
        if (by_count) {
            done += 1;
            report_progress(name);
        }
    }

    // claim: dst must be new (see Job::replace)
    void copy(const QString &src, const QString &dst, bool merge, bool claim)
    {
        struct stat st;
        if (!lstat_(src, st))
            throw_errno(src);
        Fd sp(open_parent(src)), dp(open_parent(dst));
        copy_at(sp.fd, basename(src), st, src, dp.fd, basename(dst), dst, merge, claim);
    }

    // claim: dname must still be free when the copy is put there (the items inside a folder being built never are
    // claimed: only Kestrel writes there)
    void copy_at(int sdir, const QString &sname, const struct stat &st, const QString &src, int ddir,
                 const QString &dname, const QString &dst, bool merge, bool claim = false)
    {
        struct stat dst_st;
        bool dst_exists = stat_at(ddir, dname, dst_st);
        if (claim && dst_exists)
            throw conflict(dst);
        if (dst_exists && S_ISDIR(dst_st.st_mode) && !S_ISDIR(st.st_mode))
            throw Error(tx("%1 is a folder").arg(dst));
        if (S_ISLNK(st.st_mode)) {
            QByteArray target(PATH_MAX, 0);
            ssize_t n = ::readlinkat(sdir, enc(sname).constData(), target.data(), target.size() - 1);
            if (n < 0)
                throw_errno(src);
            target.truncate(n);
            // made under a hidden name, then renamed over dst: replaced in one step, never written through
            QString part;
            for (;;) {
                part = part_name();
                if (::symlinkat(target.constData(), ddir, enc(part).constData()) == 0)
                    break;
                if (errno != EEXIST)
                    throw_errno(dst);
            }
            try {
                commit(ddir, part, dname, dst, claim);
            } catch (...) {
                ::unlinkat(ddir, enc(part).constData(), 0);
                throw;
            }
        } else if (S_ISDIR(st.st_mode)) {
            Fd in(open_dir_at(sdir, sname, st, src));
            auto copy_into = [&](int out) {
                QStringList names = names_in(in.fd, src);
                if (drives::slow_kind(src) == drives::Slow::Tape) {   // in tape order (folders, which have none, last)
                    QHash<QString, QPair<qint64, qint64>> at;
                    for (const QString &e : names) {
                        QPair<qint64, qint64> pos = drives::tape_position(join(src, e));
                        at.insert(e, pos.first < 0 ? qMakePair(LLONG_MAX, LLONG_MAX) : pos);
                    }
                    std::stable_sort(names.begin(), names.end(), [&](const QString &a, const QString &b) { return at[a] < at[b]; });
                }
                for (const QString &e : names) {
                    struct stat est;
                    if (!stat_at(in.fd, e, est))
                        throw_errno(join(src, e));
                    copy_at(in.fd, e, est, join(src, e), out, e, join(dst, e), merge);
                }
                copy_times_mode(out, st);
            };
            if (dst_exists && merge) {   // into the existing folder (each file in it still replaced whole)
                if (!S_ISDIR(dst_st.st_mode))
                    throw Error(tx("%1 exists and isn't a folder").arg(dst));
                Fd out(open_dir_at(ddir, dname, dst_st, dst));
                copy_into(out.fd);
            } else if (!dst_exists && ::mkdirat(ddir, enc(dname).constData(), 0700) == 0) {
                // a new folder (nothing to replace): made under its own name straight away, so each file shows up
                // there as soon as it's complete (each is still written under a hidden name and renamed when whole).
                // Not all-or-nothing: a cancel or an error leaves the files finished so far, whole, and removes only
                // the one being written. The folder gets the source's permissions and times even then.
                struct stat made;
                if (!stat_at(ddir, dname, made))
                    throw_errno(dst);
                Fd out(open_dir_at(ddir, dname, made, dst));
                try {
                    copy_into(out.fd);
                } catch (...) {
                    copy_times_mode(out.fd, st);
                    throw;
                }
            } else {
                if (!dst_exists && errno != EEXIST)
                    throw_errno(dst);
                if (!dst_exists && claim)   // the name was taken meanwhile: never replaced
                    throw conflict(dst);
                // built under a hidden name beside dst and put in its place only when complete (swap_in): a cancel or
                // an error leaves dst as it was, and no half-made copy
                QString part;
                for (;;) {
                    part = part_name();
                    if (::mkdirat(ddir, enc(part).constData(), 0700) == 0)
                        break;
                    if (errno != EEXIST)
                        throw_errno(dst);
                }
                try {
                    struct stat part_st;
                    if (!stat_at(ddir, part, part_st))
                        throw_errno(dst);
                    {
                        Fd out(open_dir_at(ddir, part, part_st, dst));
                        copy_into(out.fd);
                    }
                    if (claim)
                        claim_name(ddir, part, ddir, dname, dst);
                    else
                        swap_in(ddir, part, ddir, dname, dst);
                } catch (...) {
                    discard(ddir, part, join(dirname(dst), part));
                    throw;
                }
            }
        } else if (S_ISREG(st.st_mode)) {
            copy_file_at(sdir, sname, st, src, ddir, dname, dst, claim);
        } else {
            throw Error(tx("%1 isn't a regular file, folder or link").arg(src));
        }
    }

    void copy_file_at(int sdir, const QString &sname, const struct stat &st, const QString &src, int ddir,
                      const QString &dname, const QString &dst, bool claim)
    {
        QString name = basename(src);
        Fd in(::openat(sdir, enc(sname).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        struct stat now;
        if (in.fd < 0)
            throw_errno(src);
        if (::fstat(in.fd, &now) != 0 || now.st_dev != st.st_dev || now.st_ino != st.st_ino)
            throw Error(tx("%1 changed while it was being copied").arg(src));
        // Written to a new file beside the destination and renamed over it once complete: until then an existing file
        // keeps its contents (a cancel or an error removes only the new file), a symlink in the way is replaced (never
        // written through), and another hard link of the old file keeps the old contents.
        //
        // An exception, by the user's choice: a new name on a tape is written under that name straight away. There the
        // part file buys nothing: what a write that fails part-way used stays used whatever the file is called (LTFS
        // never reuses tape), and LTFS has neither RENAME_NOREPLACE nor link(), so a part's claim on the name would be
        // check-then-rename, which O_EXCL on the name itself does better. A cancel or an error deletes the half-written
        // file, so none is left under the real name once the job has ended (a crash or power cut mid-file can leave
        // one). Replacing a file on a tape still goes through a part file: the old one is kept until the new is whole.
        bool to_tape = drives::slow_kind(dst) == drives::Slow::Tape;
        struct stat existing;
        bool direct = to_tape && !stat_at(ddir, dname, existing);
        QString part;
        int part_fd;
        if (direct) {
            part = dname;
            part_fd = ::openat(ddir, enc(dname).constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
            if (part_fd < 0 && errno == EEXIST)   // taken meanwhile: never replaced (as a part's claim would find)
                throw conflict(dst);
            if (part_fd < 0)
                throw_errno(dst);
        } else {
            try {
                part_fd = open_part_at(ddir, QString(), &part);
            } catch (const OSError &e) {
                throw_errno(dst, e.code);   // named as the destination (permission denied there → offer the admin session)
            }
        }
        Fd out(part_fd);
        if (drives::slow_kind(src) == drives::Slow::Tape)   // the first read waits while the drive winds to the file
            task->report(double(done), double(total), tx("Positioning the tape — %1").arg(name), true);
        qint64 written = 0;
        try {
            if (to_tape) {   // room for it first: a write that runs out of tape wastes what it wrote (LTFS never reuses it)
                struct statvfs sv;
                qint64 free = ::fstatvfs(ddir, &sv) == 0 ? qint64(sv.f_bavail) * qint64(sv.f_frsize) : -1;
                if (qEnvironmentVariableIsSet("KESTREL_TEST_TAPE_FREE"))
                    free = qEnvironmentVariable("KESTREL_TEST_TAPE_FREE").toLongLong();
                if (free >= 0 && now.st_size > free)
                    throw Error(tx("Not enough space on the tape for “%1”: it needs %2, the tape has %3 left")
                                    .arg(name, human_size(qint64(now.st_size)), human_size(free)));
            }
            if (to_tape && dd_now) {
                copy_with_dd(in.fd, out.fd, src, dst, qint64(now.st_size), written);
                report_progress(name);
            }
            // from the tape buffer when it has it (read ahead by its thread), else straight from the file
            bool buffered = !dd_now && to_tape && buffer && buffer->begin(src, now.st_dev, now.st_ino);
            std::unique_ptr<char[]> buf(buffered || (to_tape && dd_now) ? nullptr : new char[CHUNK]);
            for (; !(to_tape && dd_now);) {
                task->check();   // a cancel, or a pause (within one chunk; the part file just stays open)
                const char *data = buf.get();
                ssize_t n;
                if (buffered) {
                    n = ssize_t(buffer->next(&data));
                } else {
                    n = ::read(in.fd, buf.get(), CHUNK);
                    if (n < 0 && errno == EINTR)
                        continue;
                    if (n < 0)
                        throw_errno(src);
                }
                if (n == 0)
                    break;
                QElapsedTimer writing;
                writing.start();
                for (ssize_t off = 0; off < n;) {
                    ssize_t w = ::write(out.fd, data + off, size_t(n - off));
                    if (w < 0 && errno == EINTR)
                        continue;
                    if (w < 0)
                        throw_errno(dst);
                    off += w;
                }
                if (buffered)
                    buffer->wrote(n, writing.nsecsElapsed());
                done += n;
                written += n;
                log_tape_rate();
                if (buffer && !told_slow && buffer->source_too_slow()) {
                    told_slow = true;
                    task->log(tx("The source reads at %1/s, slower than the tape's lowest streaming speed: the tape "
                                      "writes in bursts from the buffer, stopping in between. Copying from a faster "
                                      "drive, or making an archive on a fast disk first, would keep it streaming.")
                                  .arg(human_size(qint64(buffer->source_rate()))));
                }
                report_progress(name);
            }
            copy_times_mode(out.fd, st);
            // replacing something: on disk before the rename (a crash leaves the old or the new). Not on a tape: an fsync
            // there writes LTFS's index (the drive stops), and LTFS never overwrites data anyway: until the rename is in
            // the index, the old file is what the tape has
            struct stat old;
            if (!to_tape && stat_at(ddir, dname, old) && ::fsync(out.fd) != 0)
                throw_errno(dst);
            if (::close(out.release()) != 0)
                throw_errno(dst);
            if (!direct)
                commit(ddir, part, dname, dst, claim);
        } catch (...) {
            out.reset();
            ::unlinkat(ddir, enc(part).constData(), 0);   // the part, or (direct) the half-written file under its name
            if (to_tape && written >= 1024LL * 1024 * 1024)
                task->log(tx("  %1 of “%2” had already been written to the tape: LTFS keeps that space used until the "
                                  "tape is reformatted.")
                              .arg(human_size(written), name));
            throw;
        }
    }

    static Error conflict(const QString &dst)
    {
        return Error(tx("“%1” appeared in %2 while this was being done; it was kept as it is, and nothing was put in its place")
                         .arg(basename(dst), dirname(dst)));
    }

    // a finished part (a file or link) to its name in the same folder: claimed (claim_name), or replacing what's
    // there in one rename
    void commit(int dirfd, const QString &part, const QString &name, const QString &shown, bool claim)
    {
        if (claim)
            claim_name(dirfd, part, dirfd, name, shown);
        else if (::renameat(dirfd, enc(part).constData(), dirfd, enc(name).constData()) != 0)
            throw_errno(shown);
    }

    // src (in src_dir) to the name dst (in dst_dir) on one filesystem, which must still be free: something another
    // program put there meanwhile is never replaced (RENAME_NOREPLACE; without it, link() for a file or link, which
    // fails on a taken name, and check-then-rename for a folder). A taken name throws conflict().
    void claim_name(int src_dir, const QString &src, int dst_dir, const QString &dst, const QString &shown)
    {
        QByteArray s = enc(src), d = enc(dst);
        if (util::rename_noreplace(src_dir, s.constData(), dst_dir, d.constData()) == 0)
            return;
        int e = errno;
        if (e == EINVAL) {
            struct stat st, existing;
            bool dir = stat_at(src_dir, src, st) && S_ISDIR(st.st_mode), check_then_rename = dir;
            if (!dir) {
                if (util::link_at(src_dir, s.constData(), dst_dir, d.constData()) == 0) {
                    ::unlinkat(src_dir, s.constData(), 0);
                    return;
                }
                e = errno;
                check_then_rename = util::no_link_support(e);
            }
            // a folder can't be linked, and some file systems link nothing (LTFS tapes: link() fails with EPERM, which
            // isn't a permission problem): check, then rename. Not atomic: a file another program makes under that name
            // between the two could be replaced; the best these file systems allow
            if (check_then_rename) {
                if (stat_at(dst_dir, dst, existing))
                    e = EEXIST;
                else if (::renameat(src_dir, s.constData(), dst_dir, d.constData()) == 0)
                    return;
                else
                    e = errno;
            }
        }
        if (e == EEXIST)
            throw conflict(shown);
        throw_errno(shown, e);
    }

    // src (in src_dir) to dst (in dst_dir) on one filesystem, replacing what's at dst (shown) in one step. A
    // non-folder over a non-folder (or nothing) is one rename. Where a folder is involved, the two are swapped
    // (RENAME_EXCHANGE; on a filesystem without it, the old one is renamed aside first and put back if the second
    // rename fails), and the old one, now out of the way under a hidden name, is then deleted.
    void swap_in(int src_dir, const QString &src, int dst_dir, const QString &dst, const QString &shown)
    {
        struct stat old, nw;
        bool exists = stat_at(dst_dir, dst, old);
        if (!stat_at(src_dir, src, nw))
            throw_errno(shown);
        if (!exists || (!S_ISDIR(old.st_mode) && !S_ISDIR(nw.st_mode))) {
            if (::renameat(src_dir, enc(src).constData(), dst_dir, enc(dst).constData()) != 0)
                throw_errno(shown);
            return;
        }
        if (::renameat2(src_dir, enc(src).constData(), dst_dir, enc(dst).constData(), RENAME_EXCHANGE) == 0) {
            remove_old(src_dir, src, shown);   // the old one, now where the new one was
            return;
        }
        if (errno != EINVAL)
            throw_errno(shown);
        QString aside = part_name();
        if (::renameat(dst_dir, enc(dst).constData(), src_dir, enc(aside).constData()) != 0)
            throw_errno(shown);
        if (::renameat(src_dir, enc(src).constData(), dst_dir, enc(dst).constData()) != 0) {
            int e = errno;
            (void)!::renameat(src_dir, enc(aside).constData(), dst_dir, enc(dst).constData());
            throw_errno(shown, e);
        }
        remove_old(src_dir, aside, shown);
    }

    // what was just replaced: deleted whole (a cancel now would only leave it half-deleted), with the retry for
    // read-only folders
    void remove_old(int dirfd, const QString &name, const QString &shown)
    {
        struct stat st;
        if (!stat_at(dirfd, name, st))
            return;
        QString path = join(dirname(shown), name);
        try {
            try {
                remove_at(dirfd, name, st, path, st.st_dev, false);
            } catch (const OSError &) {
                if (!make_writable(path))
                    throw;
                remove_at(dirfd, name, st, path, st.st_dev, false);
            }
        } catch (const Error &e) {
            throw Error(tx("%1 was replaced, but the old one couldn't be deleted (it's left as %2): %3")
                            .arg(shown, name, e.message()));
        }
    }

    // a half-made part after a failure or cancel: deleted, without letting a second failure hide the first
    void discard(int dirfd, const QString &name, const QString &path)
    {
        try {
            struct stat st;
            if (stat_at(dirfd, name, st))
                remove_at(dirfd, name, st, path, st.st_dev, false);
        } catch (...) {
        }
    }
};

// Offer to redo copy/move/delete jobs that failed with "permission denied" in the admin session.
static void retry_denied_as_admin(QWidget *parent, const QString &title, const QList<Job> &denied,
                                  std::function<void()> on_done)
{
    static const QHash<QString, QString> verbs_of = {{"delete", "deleted"}, {"copy", "copied"}, {"merge_copy", "copied"},
                                                     {"move", "moved"}, {"merge_move", "moved"}};
    // the message for each mix of what couldn't be done (the verbs, sorted, joined with " or ")
    static const QHash<QString, const char *> messages = {
        {"copied", TXN_NOOP("%Ln item(s) couldn't be copied because you don't have permission:\n\n%1%2")},
        {"deleted", TXN_NOOP("%Ln item(s) couldn't be deleted because you don't have permission:\n\n%1%2")},
        {"moved", TXN_NOOP("%Ln item(s) couldn't be moved because you don't have permission:\n\n%1%2")},
        {"copied or deleted", TXN_NOOP("%Ln item(s) couldn't be copied or deleted because you don't have permission:\n\n%1%2")},
        {"copied or moved", TXN_NOOP("%Ln item(s) couldn't be copied or moved because you don't have permission:\n\n%1%2")},
        {"deleted or moved", TXN_NOOP("%Ln item(s) couldn't be deleted or moved because you don't have permission:\n\n%1%2")},
        {"copied or deleted or moved",
         TXN_NOOP("%Ln item(s) couldn't be copied, deleted or moved because you don't have permission:\n\n%1%2")}};
    QSet<QString> vs;
    for (const Job &j : denied)
        vs << verbs_of.value(j.op);
    QStringList verbs(vs.begin(), vs.end());
    verbs.sort();
    QStringList names;
    for (int i = 0; i < std::min<qsizetype>(10, denied.size()); ++i)
        names << "• " + basename(denied[i].src);
    QString more = denied.size() > 10 ? "\n" + txn("…and %Ln more", denied.size() - 10) : QString();
    QString message = txn(messages.value(verbs.join(" or "), messages.value("copied or deleted or moved")), denied.size())
                          .arg(names.join('\n'), more);
    auto work = [denied](Task *task) {
        QStringList errors;
        for (const Job &j : denied) {
            task->check();
            try {
                if (j.op == "delete")
                    admin::session().call(task, "delete", {{"path", j.src}});
                else
                    admin::session().call(task, j.op.contains("copy") ? "copy" : "move",
                                          {{"src", j.src},
                                           {"dst", j.dst},
                                           {"merge", j.op.startsWith("merge")},
                                           {"replace", j.replace}});
            } catch (const admin::AdminError &e) {
                errors << basename(j.src) + ": " + e.message();
            }
        }
        return errors;
    };
    QStringList paths;
    for (const Job &j : denied)
        paths << j.src << (j.dst.isEmpty() ? QString() : dirname(j.dst));
    paths.removeAll(QString());
    admin::retry_as_admin(
        parent, title, message, work,
        [on_done](bool) {
            if (on_done)
                on_done();
        },
        paths);
}

QString wait_text(qint64 ms)
{
    qint64 s = (ms + 500) / 1000;
    if (s < 60)
        return tx("%1 s").arg(s);
    if (s < 3600)
        return s % 60 ? tx("%1 min %2 s").arg(s / 60).arg(s % 60) : tx("%1 min").arg(s / 60);
    return (s / 60) % 60 ? tx("%1 h %2 min").arg(s / 3600).arg((s / 60) % 60) : tx("%1 h").arg(s / 3600);
}

QString transfer_title(const QStringList &sources, const QString &dest_dir, const QString &op, bool dd)
{
    QString to = basename(rstrip(dest_dir, '/'));
    if (to.isEmpty())
        to = dest_dir;
    QString title;
    if (sources.size() == 1) {
        QString name = basename(rstrip(sources.first(), '/'));
        title = op == "copy" ? tx("Copying “%1” to %2").arg(name, to) : tx("Moving “%1” to %2").arg(name, to);
    } else {
        title = op == "copy" ? txn("Copying %Ln item(s) to %1", sources.size()).arg(to)
                             : txn("Moving %Ln item(s) to %1", sources.size()).arg(to);
    }
    return dd ? tx("%1 (with dd)").arg(title) : title;
}

// The folders the jobs write into that aren't there (any more): a destination moved or deleted while the job waited
static QStringList missing_destinations(const QList<Job> &jobs)
{
    QStringList out;
    for (const Job &j : jobs) {
        if (j.dst.isEmpty() || j.op == "delete")
            continue;
        QString dir = dirname(j.dst);
        if (!out.contains(dir) && !isdir(dir))
            out << dir;
    }
    return out;
}

Task *start_ops(QWidget *parent, const QList<Job> &jobs, const QString &title, std::function<void()> on_done,
                const QString &undo_label, const QStringList &intro)
{
    if (jobs.isEmpty())
        return nullptr;
    auto ops = std::make_shared<std::unique_ptr<Ops>>();
    QPointer<QWidget> p(parent);
    auto finished = [ops, p, title, on_done, undo_label](const QVariant &res) {
        QList<Job> denied = *ops ? (*ops)->denied : QList<Job>();
        if (!undo_label.isEmpty() && *ops) {
            QList<QPair<QString, QString>> moves;
            QStringList copies;
            for (const Job &j : (*ops)->completed) {
                if (j.op == "move")
                    moves << qMakePair(j.src, j.dst);
                else if (j.op == "copy")
                    copies << j.dst;
            }
            copies << (*ops)->partial;
            if (!moves.isEmpty())
                undo::record("move", undo_label, moves);
            else if (!copies.isEmpty())
                undo::record_paths("create", undo_label, copies);
            if ((*ops)->replaced || (*ops)->merged) {
                bool merged = (*ops)->merged;
                if (moves.isEmpty() && copies.isEmpty())
                    undo::record("none", merged ? tx("Merge (can't be undone)") : tx("Replace (can't be undone)"), {});
                if (auto *win = qobject_cast<QMainWindow *>(p ? p->window() : nullptr))
                    win->statusBar()->showMessage(merged ? tx("Merge can't be undone: what was replaced is gone.")
                                                         : tx("Replace can't be undone: what was replaced is gone."),
                                                  8000);
            }
        }
        // a tape changed: its index written now (LTFS otherwise writes it only at unmount or on its own timer), so a crash
        // or power cut afterwards doesn't lose what was put on it
        if (*ops) {
            QSet<QString> tapes;
            for (const Job &j : (*ops)->touched) {
                for (const QString &path : {j.dst, j.op.contains("move") || j.op == "delete" ? j.src : QString()})
                    if (drives::slow_kind(path) == drives::Slow::Tape)
                        tapes << drives::slow_root(path);
            }
            for (const QString &root : tapes)
                drives::sync_tape_job(p, root);
            // the remembered sizes of the folders it changed (and those above them) are out of date
            QStringList changed;
            for (const Job &j : (*ops)->touched) {
                if (!j.dst.isEmpty())
                    changed << dirname(j.dst);
                if (j.op.contains("move") || j.op == "delete")
                    changed << j.src;
            }
            dirsizes::drop_with_parents(changed);
        }
        QStringList errors = res.toStringList();
        if (!errors.isEmpty())
            warn(p, title, tx("Some items could not be processed:\n\n%1").arg(errors.mid(0, 20).join('\n')));
        if (!denied.isEmpty() && res.isValid()) {
            retry_denied_as_admin(p, title, denied, on_done);
            return;
        }
        if (on_done)
            on_done();
    };
    auto work = [ops, jobs](Task *task) {
        // checked when its turn comes: one clear failure, not one "No such file or directory" for each item
        QStringList gone = missing_destinations(jobs);
        if (!gone.isEmpty()) {
            bool moving = std::all_of(jobs.begin(), jobs.end(), [](const Job &j) { return j.op.contains("move"); });
            bool waited = task->queued_ms;
            throw Error((moving ? waited ? tx("The folder it was moving into, “%1”, isn't there any more: it was moved or "
                                              "deleted while this job waited. Nothing was moved.")
                                         : tx("The folder it was moving into, “%1”, isn't there any more: it was moved or "
                                              "deleted before this job began. Nothing was moved.")
                         : waited ? tx("The folder it was copying into, “%1”, isn't there any more: it was moved or "
                                       "deleted while this job waited. Nothing was copied.")
                                  : tx("The folder it was copying into, “%1”, isn't there any more: it was moved or "
                                       "deleted before this job began. Nothing was copied."))
                            .arg(gone.first()));
        }
        *ops = std::make_unique<Ops>(task, jobs);
        QStringList errors = (*ops)->run();
        task->log(errors.isEmpty() ? tx("Finished.") : txn("Finished, with %Ln error(s).", errors.size()));
        return QVariant(errors);
    };
    // the drives it reads and writes (its lanes in the job queue); a move within one drive is a rename: no lane
    QStringList paths, steps;
    for (const Job &j : jobs) {
        steps << step_text(j);
        struct stat a, b;
        bool rename_only = j.op == "move" && lstat_(j.src, a) && stat_(dirname(j.dst), b) && a.st_dev == b.st_dev;
        if (!rename_only)
            paths << j.src << (j.dst.isEmpty() ? QString() : dirname(j.dst));
    }
    paths.removeAll(QString());
    // what it finishes (undo, the tape's index, what failed) doesn't need the window: it may have closed meanwhile
    return queue_job(parent, title, paths, work, finished, true, steps, false, intro, true);
}

namespace {
struct Plan {
    QPointer<QWidget> parent;
    bool has_parent;
    QStringList sources;
    QString dest_dir, op, dest_real;
    QString apply_all;   // a conflict's answer, "for all"
    QList<Job> jobs;
    int next = 0;
    std::function<void(const QList<Job> &)> planned;
};
}  // namespace

static void plan_job(Plan &p, const QString &choice, const QString &src, const QString &dst, const QString &name)
{
    if (choice == "skip")
        return;
    if (choice == "both") {
        p.jobs << Job{p.op, src, unique_path(p.dest_dir, name, "num")};
    } else {
        bool merge = isdir(dst) && isdir(src);
        p.jobs << Job{merge ? "merge_" + p.op : p.op, src, dst, true};
    }
}

static void plan_next(std::shared_ptr<Plan> p)
{
    if (p->has_parent && !p->parent)
        return;
    for (; p->next < p->sources.size(); ++p->next) {
        const QString &src = p->sources[p->next];
        QString name = basename(rstrip(src, '/'));
        QString src_parent = dirname(abspath(src));
        if (isdir(src) && !islink(src)) {
            QString sr = realpath(src);
            if (p->dest_real == sr || p->dest_real.startsWith(sr + "/")) {
                warn(p->parent, tx("Cannot transfer"), (p->op == "move" ? tx("Cannot move “%1” into itself.") : tx("Cannot copy “%1” into itself.")).arg(name));
                continue;
            }
        }
        if (realpath(src_parent) == p->dest_real) {
            if (p->op != "move")
                p->jobs << Job{p->op, src, unique_path(p->dest_dir, name)};
            continue;
        }
        QString dst = join(p->dest_dir, name);
        if (!lexists(dst)) {
            p->jobs << Job{p->op, src, dst};
            continue;
        }
        if (!p->apply_all.isEmpty()) {
            plan_job(*p, p->apply_all, src, dst, name);
            continue;
        }
        // ask, and go on with the rest once it's answered
        auto *dlg = new ConflictDialog(p->parent, dst, isdir(dst));
        QObject::connect(dlg, &QDialog::finished, dlg, [p, dlg, src, dst, name]() {
            if (dlg->choice == "cancel")
                return;
            if (dlg->all_box->isChecked())
                p->apply_all = dlg->choice;
            plan_job(*p, dlg->choice, src, dst, name);
            ++p->next;
            plan_next(p);
        });
        show_free(dlg);
        return;
    }
    p->planned(p->jobs);
}

void plan_transfer(QWidget *parent, const QStringList &sources, const QString &dest_dir, const QString &op,
                   std::function<void(const QList<Job> &)> planned)
{
    auto p = std::make_shared<Plan>();
    p->parent = parent;
    p->has_parent = parent != nullptr;
    p->sources = sources;
    p->dest_dir = dest_dir;
    p->op = op;
    p->dest_real = realpath(dest_dir);
    p->planned = std::move(planned);
    if (drives::slow_kind(dest_dir) != drives::Slow::Tape) {
        plan_next(p);
        return;
    }
    // to a tape: many small files are slow to write and waste tape (each costs about as much as a large one), so
    // the user is asked first; counted on a worker (the sources may be large, or slow themselves)
    const int many = 1000;
    run_task(
        parent, tx("Counting files"),
        [sources, many]() {
            int small = 0;
            for (const QString &src : sources) {
                struct stat st;
                if (!lstat_(src, st))
                    continue;
                if (!S_ISDIR(st.st_mode)) {
                    small += st.st_size < (1 << 20);
                    continue;
                }
                walk(src, [&](const QString &root, QStringList &, QStringList &files) {
                    for (const QString &f : files) {
                        struct stat fst;
                        if (lstat_(join(root, f), fst) && fst.st_size < (1 << 20))
                            ++small;
                    }
                    return small <= many;
                });
                if (small > many)
                    break;
            }
            return QVariant(small);
        },
        [p, many](const QVariant &n) {
            if (n.toInt() <= many) {
                plan_next(p);
                return;
            }
            ask_yes(p->parent, tx("Copy to Tape"),
                    tx("This copies more than %1 small files (under 1 MB) to the tape. A tape writes each file as "
                            "slowly as a large one and uses extra space for it, so this will be slow. Putting them in an "
                            "archive first (Compress…) and copying that is much faster.\n\nCopy them anyway?")
                        .arg(many),
                    [p]() { plan_next(p); });
        },
        true);
}

// The jobs that write files onto a tape from another drive (a copy, or a move between drives), by index
static QList<int> tape_writes(const QList<Job> &jobs)
{
    QList<int> out;
    for (int i = 0; i < jobs.size(); ++i) {
        const Job &j = jobs[i];
        if ((j.op != "copy" && j.op != "move") || drives::slow_kind(j.dst) != drives::Slow::Tape ||
            drives::slow_kind(j.src) == drives::Slow::Tape)
            continue;
        struct stat a, b;
        if (j.op == "move" && lstat_(j.src, a) && stat_(dirname(j.dst), b) && a.st_dev == b.st_dev)
            continue;   // a rename on the tape
        out << i;
    }
    return out;
}

static QList<Job> with_dd(QList<Job> jobs, const QList<int> &which, bool dd)
{
    for (int i : which)
        jobs[i].dd = dd;
    return jobs;
}

void choose_tape_writer(QWidget *parent, const QList<Job> &jobs, std::function<void(const QList<Job> &)> go)
{
    QList<int> onto = tape_writes(jobs);
    QString how = settings().value("tape_writer", "ask").toString();
    if (onto.isEmpty() || how != "ask") {
        go(with_dd(jobs, onto, !onto.isEmpty() && how == "dd"));
        return;
    }
    QString tape = basename(rstrip(drives::slow_root(jobs[onto.first()].dst), '/'));
    auto *box = new QMessageBox(QMessageBox::Question, tx("Write to the Tape"),
                                tx("How should Kestrel write to the tape “%1”?").arg(tape), QMessageBox::NoButton,
                                parent);
    box->setInformativeText(tx("Kestrel's copy reads ahead into a buffer in memory, so the tape keeps streaming when the "
                            "source slows down. dd writes each file straight from the source, in 512 KB blocks as LTFS "
                            "stores them; the Jobs window shows its output."));
    QAbstractButton *kestrel = box->addButton(tx("Kestrel's Copy"), QMessageBox::AcceptRole);
    QAbstractButton *dd = box->addButton(tx("dd"), QMessageBox::AcceptRole);
    box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(qobject_cast<QPushButton *>(kestrel));
    auto *always = new QCheckBox(tx("Don't ask again (Preferences → Files → Tapes)"));
    box->setCheckBox(always);
    ask_box(box, [jobs, onto, go, kestrel, dd, always](QAbstractButton *b) {
        if (!b || (b != kestrel && b != dd))
            return;   // cancelled: nothing is written
        if (always->isChecked())
            prefs::set("tape_writer", b == dd ? "dd" : "kestrel");
        go(with_dd(jobs, onto, b == dd));
    });
}

void transfer(QWidget *parent, const QStringList &sources, const QString &dest_dir, const QString &op,
              std::function<void()> on_done, const QString &how, std::function<void()> on_started)
{
    QPointer<QWidget> owner(parent);
    // what started it, when (asked now: the conflict and tape questions may stay open a while)
    QString window = parent ? parent->window()->windowTitle() : QString();
    QStringList from;
    for (const QString &s : sources)
        if (!from.contains(dirname(abspath(s))))
            from << dirname(abspath(s));
    QString by = how.isEmpty() ? QString(APP_NAME) : how, at = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    int n = int(sources.size());
    QString started = window.isEmpty()
                          ? (op == "copy" ? txn("Copy of %Ln item(s), started by %1 at %2", n)
                                          : txn("Move of %Ln item(s), started by %1 at %2", n))
                                .arg(by, at)
                          : (op == "copy" ? txn("Copy of %Ln item(s), started by %1 in the window “%2” at %3", n)
                                          : txn("Move of %Ln item(s), started by %1 in the window “%2” at %3", n))
                                .arg(by, window, at);
    QStringList intro{started, tx("From: %1").arg(from.join(", ")), tx("To: %1").arg(dest_dir)};
    plan_transfer(parent, sources, dest_dir, op, [owner, sources, dest_dir, op, on_done, intro, on_started](const QList<Job> &planned) {
        if (planned.isEmpty())
            return;
        choose_tape_writer(owner, planned, [owner, sources, dest_dir, op, on_done, intro, on_started](const QList<Job> &jobs) {
            bool dd = std::any_of(jobs.begin(), jobs.end(), [](const Job &j) { return j.dd; });
            if (!owner)   // (re-read: the window may have closed while the question was open)
                return;
            start_ops(owner, jobs, transfer_title(sources, dest_dir, op, dd), on_done, op == "copy" ? "Copy" : "Move",
                      intro);
            if (on_started)
                on_started();
        });
    });
}

// ---------------------------------------------------------------- links & shortcuts

// A string value in a .desktop file (the Desktop Entry spec's escapes): a line break can't end the value and start a
// key of its own.
static QString desktop_value(const QString &s)
{
    QString out;
    for (int i = 0; i < s.size(); i++) {
        QChar c = s[i];
        if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\t')
            out += "\\t";
        else if (c == '\r')
            out += "\\r";
        else if (c == ' ' && i == 0)
            out += "\\s";
        else
            out += c;
    }
    return out;
}

// One argument of a .desktop file's Exec: in double quotes, with ", `, $ and \ escaped inside them and % doubled (a
// field code otherwise); desktop_value() is applied to the whole line on top.
static QString exec_arg(const QString &arg)
{
    QString q;
    for (QChar c : arg) {
        if (c == '"' || c == '`' || c == '$' || c == '\\')
            q += '\\';
        q += c;
    }
    return "\"" + q.replace("%", "%%") + "\"";
}

QVariantMap link_plan(const QString &kind, const QString &target, const QString &dest_dir)
{
    QString name = basename(rstrip(target, '/'));
    if (name.isEmpty())
        name = target;
    bool same_dir = dirname(target) == dest_dir;
    if (kind == "sym" || kind == "rel") {
        QString link = unique_path(dest_dir, same_dir ? "Link to " + name : name, "num");
        QString src = kind == "rel" ? relpath(target, dest_dir) : abspath(target);
        return {{"op", "symlink"}, {"target", src}, {"link", link}};
    }
    if (kind == "hard") {
        QString link = unique_path(dest_dir, same_dir ? name + " (hard link)" : name, "num");
        return {{"op", "hardlink"}, {"target", abspath(target)}, {"link", link}};
    }
    // a freedesktop .desktop launcher pointing to target. Every value is escaped (desktop_value), so a name can't add
    // keys of its own; control characters the format has no escape for are refused.
    static const QRegularExpression control("[\\x00-\\x08\\x0b\\x0c\\x0e-\\x1f\\x7f]");
    if (target.contains(control))
        throw Error(tx("A shortcut can't point to “%1”: its name or folder holds control characters.").arg(name));
    bool is_dir = isdir(target);
    QString body;
    if (isfile(target) && util::access(target, X_OK)) {
        // GLib checks that the program exists before it turns %% back into %, so it won't load a launcher whose
        // program's path holds a "%": sh starts that one (the path is its $0, never parsed as a command)
        QString exec = target.contains('%') ? "sh -c " + exec_arg("exec \"$0\"") + " " + exec_arg(target)
                                            : exec_arg(target);
        body = "[Desktop Entry]\nType=Application\nName=" + desktop_value(name) +
               "\nExec=" + desktop_value(exec) + "\nPath=" + desktop_value(dirname(target)) +
               "\nIcon=application-x-executable\nTerminal=false\n";
    } else {
        QString icon = is_dir ? QString("folder") : mime_for(target, is_dir).iconName();
        body = "[Desktop Entry]\nType=Link\nName=" + desktop_value(name) + "\nURL=" + desktop_value(file_uri(target)) +
               "\nIcon=" + desktop_value(icon) + "\n";
    }
    QString path = unique_path(dest_dir, split_ext(name).first + ".desktop", "num");
    return {{"op", "write"}, {"path", path}, {"text", body}, {"mode", 0755}};
}

QString plan_path(const QVariantMap &plan)
{
    QString l = plan.value("link").toString();
    return l.isEmpty() ? plan.value("path").toString() : l;
}

QString make_link(const QVariantMap &plan)
{
    QString op = plan.value("op").toString();
    if (op == "symlink") {
        util::symlink(plan["target"].toString(), plan["link"].toString());
    } else if (op == "hardlink") {
        util::link(plan["target"].toString(), plan["link"].toString());
    } else {
        QString path = plan["path"].toString();
        write_text(path, plan["text"].toString().toUtf8(), true);
        util::chmod(path, mode_t(plan["mode"].toInt()));
        mark_trusted(path);
    }
    return plan_path(plan);
}

// ---------------------------------------------------------------- misc

QStringList trash_contents()
{
    QStringList out;
    for (const QString &root : trash_dirs()) {
        for (const char *sub : {"files", "info", "expunged"}) {
            QString d = join(root, sub);
            if (!isdir(d))
                continue;
            try {
                for (const QString &n : listdir(d))
                    out << join(d, n);
            } catch (const OSError &) {
            }
        }
    }
    return out;
}

bool can_shred() { return which("bleachbit"); }

static QStringList still_there(const QStringList &paths)
{
    QStringList out;
    for (const QString &p : paths) {
        struct stat st;
        if (lstat_(p, st))
            out << p;
    }
    return out;
}

Task *shred(QWidget *parent, const QStringList &paths, const QString &title,
            std::function<void(const QStringList &)> on_done)
{
    auto fn = [paths](Task *task) -> QVariant {
        int total = paths.size();
        task->report(0, total, tx("Starting BleachBit…"));
        proc::Options opts;
        opts.out = opts.err = proc::DEVNULL;   // it lists every file; what's left afterwards is checked instead
        auto p = proc::spawn(QStringList{"bleachbit", "--shred", "--"} + paths, opts);
        try {
            while (!p->wait_for(250)) {
                task->check();
                int n = total - still_there(paths).size();
                task->report(n, total, tx("%1 of %2 shredded").arg(group_digits(n), group_digits(total)));
            }
        } catch (const Cancelled &) {
            p->kill_group();
            throw;
        }
        return still_there(paths);
    };
    return queue_job(parent, title, paths, fn, [on_done](const QVariant &r) {
        if (r.isValid() && on_done)
            on_done(r.toStringList());
    });
}

namespace dirsizes {

static const int MAX_ENTRIES = 5000;
static const qint64 MAX_AGE = 365LL * 24 * 3600;   // older entries are trimmed

QString cache_file() { return join(APP_CACHE(), "folder-sizes.json"); }

static QJsonObject &entries()
{
    // re-read when another Kestrel has written it
    static QJsonObject cached;
    static qint64 read_mtime = -1;
    struct stat st;
    qint64 m = stat_(cache_file(), st) ? qint64(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec : 0;
    if (m != read_mtime) {
        bool ok = false;
        cached = QJsonDocument::fromJson(read_file(cache_file(), &ok)).object();
        read_mtime = m;
    }
    return cached;
}

static void save(QJsonObject &all)
{
    qint64 now = QDateTime::currentSecsSinceEpoch();
    QList<QPair<qint64, QString>> by_age;
    for (auto it = all.begin(); it != all.end();) {
        qint64 at = it.value().toObject().value("at").toInteger();
        if (now - at > MAX_AGE) {
            it = all.erase(it);
        } else {
            by_age << qMakePair(at, it.key());
            ++it;
        }
    }
    if (by_age.size() > MAX_ENTRIES) {   // the oldest go
        std::sort(by_age.begin(), by_age.end());
        for (int i = 0; i < by_age.size() - MAX_ENTRIES; ++i)
            all.remove(by_age[i].second);
    }
    try {
        makedirs(APP_CACHE(), true);
        write_atomic(cache_file(), QJsonDocument(all).toJson(QJsonDocument::Compact));
    } catch (const OSError &) {   // a cache: a failure to write it only means counting again next time
    }
}

std::optional<Entry> get(const QString &folder)
{
    QJsonObject o = entries().value(folder).toObject();
    struct stat st;
    if (o.isEmpty() || !lstat_(folder, st) || !S_ISDIR(st.st_mode) || o.value("dev").toInteger() != qint64(st.st_dev) ||
        o.value("ino").toInteger() != qint64(st.st_ino))
        return std::nullopt;
    Entry e;
    e.stats.size = o.value("size").toInteger();
    e.stats.files = o.value("files").toInteger();
    e.stats.dirs = o.value("dirs").toInteger();
    e.measured = o.value("at").toInteger();
    return e;
}

void put(const QString &folder, const DirStats &s)
{
    struct stat st;
    if (!lstat_(folder, st) || !S_ISDIR(st.st_mode))
        return;
    QJsonObject &all = entries();
    all[folder] = QJsonObject{{"dev", qint64(st.st_dev)}, {"ino", qint64(st.st_ino)}, {"size", s.size},
                              {"files", s.files},         {"dirs", s.dirs},          {"at", QDateTime::currentSecsSinceEpoch()}};
    save(all);
}

void drop_with_parents(const QStringList &paths)
{
    QJsonObject &all = entries();
    if (all.isEmpty())
        return;
    bool changed = false;
    for (const QString &p : paths) {
        for (QString d = rstrip(p, '/'); !d.isEmpty(); d = dirname(d)) {
            if (all.contains(d)) {
                all.remove(d);
                changed = true;
            }
            if (d == "/")
                break;
        }
    }
    if (changed)
        save(all);
}

QString age_text(qint64 measured, qint64 now)
{
    qint64 ago = std::max<qint64>(0, now - measured);
    if (ago < 60)
        return tx("Calculated just now");
    QString when = i18n::date_time(QDateTime::fromSecsSinceEpoch(measured));
    return (ago < 3600            ? txn("Calculated %Ln minute(s) ago (%1)", ago / 60)
            : ago < 86400         ? txn("Calculated %Ln hour(s) ago (%1)", ago / 3600)
            : ago < 30 * 86400    ? txn("Calculated %Ln day(s) ago (%1)", ago / 86400)
            : ago < 365 * 86400LL ? txn("Calculated %Ln month(s) ago (%1)", ago / (30 * 86400))
                                  : txn("Calculated %Ln year(s) ago (%1)", ago / (365 * 86400LL)))
        .arg(when);
}

}  // namespace dirsizes

DirStats dir_stats(const QString &path, std::function<bool()> cancel)
{
    DirStats s;
    QStringList stack{path};
    while (!stack.isEmpty()) {
        if (cancel && cancel())
            break;
        QString d = stack.takeLast();
        DIR *dir = ::opendir(enc(d).constData());
        if (!dir)
            continue;
        while (struct dirent *e = ::readdir(dir)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            QString p = join(d, dec(e->d_name));
            struct stat st;
            if (!lstat_(p, st))
                continue;
            if (S_ISDIR(st.st_mode)) {
                s.dirs += 1;
                stack << p;
            } else {
                s.files += 1;
                s.size += st.st_size;
            }
        }
        ::closedir(dir);
    }
    return s;
}

// ---------------------------------------------------------------- local copies of device files

void fetch_local(QWidget *parent, const QStringList &paths, std::function<void(const QStringList &)> on_done)
{
    struct Item {
        QString path, uri, dst;
    };
    QString root = join(APP_CACHE(), "device-files");
    QList<Item> items;
    for (const QString &p : paths) {
        QString uri = device_uri(p);   // main thread (volume monitor); the copy then talks to gvfs directly
        items << Item{p, uri, join(join(root, md5(uri.isEmpty() ? p : uri)), basename(p))};
    }
    auto fn = [items, root](Task *task) -> QVariant {
        QSet<QString> keep;
        for (const Item &it : items)
            keep << dirname(it.dst);
        // drop the copies nobody has opened for a day
        try {
            for (const QString &n : listdir(root)) {
                QString d = join(root, n);
                struct stat st;
                if (!keep.contains(d) && stat_(d, st) && ::time(nullptr) - st.st_mtime > 86400)
                    rmtree(d);
            }
        } catch (const OSError &) {
        }
        QList<GFile *> srcs;
        QList<qint64> sizes;
        for (const Item &it : items) {
            GFile *f = it.uri.isEmpty() ? g_file_new_for_path(enc(it.path).constData())
                                        : g_file_new_for_uri(it.uri.toUtf8().constData());
            GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
            qint64 size = info ? g_file_info_get_size(info) : -1;
            if (info)
                g_object_unref(info);
            srcs << f;
            sizes << size;
        }
        auto release = [&srcs]() {
            for (GFile *f : srcs)
                g_object_unref(f);
        };
        QStringList out;
        for (int i = 0; i < items.size(); ++i) {
            const Item &it = items[i];
            QString name = basename(it.path);
            struct stat st;
            if (stat_(it.dst, st) && st.st_size == sizes[i]) {   // copied before
                ::utime(enc(dirname(it.dst)).constData(), nullptr);
                out << it.dst;
                continue;
            }
            if (task->cancelled) {
                release();
                throw Cancelled();
            }
            makedirs(dirname(it.dst), true);
            QString part = it.dst + ".part";
            // the device sends the whole file before the copy starts, so a cancel has to interrupt the wait
            GCancellable *cancel = g_cancellable_new();
            std::atomic<bool> finished{false};
            std::thread watch([&]() {
                while (!finished) {
                    if (task->cancelled) {
                        g_cancellable_cancel(cancel);
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
            // busy, not a percentage: the device sends nothing until it has the whole file, then it arrives at once
            task->report(0, 0, name + " — will launch once ready");
            GFile *dst = g_file_new_for_path(enc(part).constData());
            GError *err = nullptr;
            gboolean ok = g_file_copy(srcs[i], dst, G_FILE_COPY_OVERWRITE, cancel, nullptr, nullptr, &err);
            finished = true;
            watch.join();
            g_object_unref(dst);
            g_object_unref(cancel);
            if (!ok) {
                bool cancelled = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
                QString msg = QString::fromUtf8(err->message);
                g_error_free(err);
                ::unlink(enc(part).constData());
                release();
                if (cancelled || task->cancelled)
                    throw Cancelled();
                throw Error(tx("Could not copy %1 from the device: %2").arg(name, msg));
            }
            rename(part, it.dst);
            out << it.dst;
        }
        release();
        return out;
    };
    queue_job(parent, "Loading from device", paths, fn, [on_done](const QVariant &res) {
        if (res.isValid())
            on_done(res.toStringList());
    });
}

}  // namespace fileops

// ---------------------------------------------------------------- conflict dialog

ConflictDialog::ConflictDialog(QWidget *parent, const QString &dst, bool is_dir) : QDialog(parent)
{
    setWindowTitle(tx("File conflict"));
    auto *lay = new QVBoxLayout(this);
    QString kind = is_dir ? "folder" : "file";
    lay->addWidget(new QLabel(tx("A %1 named “%2” already exists in\n%3").arg(kind, basename(dst), dirname(dst))));
    if (is_dir)
        lay->addWidget(new QLabel(tx("Merge puts the contents into the existing folder: files there with the same names are\n"
                                  "replaced. A merge can't be undone.")));
    all_box = new QCheckBox(tx("Apply this action to all conflicts"));
    lay->addWidget(all_box);
    auto *bb = new QDialogButtonBox;
    const QList<QPair<QString, QString>> choices = {
        {is_dir ? "Merge" : "Replace", "replace"}, {"Keep Both", "both"}, {"Skip", "skip"}};
    for (const auto &[label, val] : choices) {
        QPushButton *b = bb->addButton(label, QDialogButtonBox::AcceptRole);
        QString v = val;
        connect(b, &QPushButton::clicked, this, [this, v]() {
            choice = v;
            accept();
        });
    }
    QPushButton *cancel = bb->addButton(QDialogButtonBox::Cancel);
    connect(cancel, &QPushButton::clicked, this, [this]() {
        choice = "cancel";
        accept();
    });
    lay->addWidget(bb);
}
