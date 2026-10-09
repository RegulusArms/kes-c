#include "fileops.h"

#include "admin.h"
#include "atc.h"
#include "proc.h"
#include "undo.h"
#include "stats.h"
#include "util.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QRegularExpression>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

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

void Task::check()
{
    if (cancelled)
        throw Cancelled();
}

void Task::report(double done, double total, const QString &t)
{
    // at most ~12 updates a second, so thousands of tiny files can't flood the UI thread
    qint64 now = monotonic_ms();
    if (now - last < 80)
        return;
    last = now;
    Q_EMIT progress(total > 0 ? std::min(done / total, 1.0) : -1.0, t);
}

void Task::run()
{
    QVariant res;
    try {
        res = fn(this);
    } catch (const Cancelled &) {
        was_cancelled = true;
        res = QVariant();
    } catch (const std::exception &e) {
        Q_EMIT error(QString::fromStdString(e.what()));
        return;
    }
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

void TaskBoard::add(Task *task)
{
    local << task;
    stats::peak("tasks at once", local.size());
    connect(task, &Task::progress, this, [this]() {
        schedule(500);
        Q_EMIT changed();
    });
    connect(task, &QThread::finished, this, [this, task]() {
        local.removeAll(task);
        schedule(0);
        Q_EMIT changed();
    });
    schedule(0);
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
        list << QJsonObject{{"id", t->id},           {"title", t->title},
                            {"text", t->text},       {"fraction", t->fraction},
                            {"cancellable", t->cancellable}, {"cancelling", bool(t->cancelled)},
                            {"admin", t->admin}};
    atc::announce("tasks", {{"tasks", list}}, true);
}

QList<TaskInfo> TaskBoard::others(const QList<Task *> &mine) const
{
    QList<TaskInfo> out;
    QString me = atc::radio()->flight();
    for (Task *t : local)
        if (!mine.contains(t))
            out << TaskInfo{me, t->id, t->title, t->text, t->fraction, t->cancellable, bool(t->cancelled), t->admin, t};
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
                          nullptr};
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
    auto line = [](const TaskInfo &t) { return (t.admin ? "🛡 " : "") + t.title + (!t.text.isEmpty() ? ": " + t.text : QString()); };
    TaskInfo first;
    QString where;
    if (!tasks.isEmpty()) {
        Task *t = tasks.first();
        first = TaskInfo{QString(), t->id, t->title, t->text, t->fraction, t->cancellable, bool(t->cancelled), false, t};
    } else {
        first = others.takeFirst();
        where = " (in another window)";
    }
    QString shown = (first.admin && !first.local ? "🛡 " : "") + first.title;
    QString text = shown + where +
                   (first.cancelling ? QString(" — cancelling…") : !first.text.isEmpty() ? ": " + first.text : QString());
    QStringList more;
    if (tasks.size() > 1)
        more << QString("+%1 more").arg(tasks.size() - 1);
    if (!others.isEmpty())
        more << QString("+%1 in other windows").arg(others.size());
    label->setText(label->fontMetrics().elidedText(text, Qt::ElideMiddle, 380) +
                   (more.isEmpty() ? QString() : "  (" + more.join(", ") + ")"));
    QStringList tips;
    for (Task *x : tasks)
        tips << x->title + (!x->text.isEmpty() ? ": " + x->text : QString());
    if (!where.isEmpty())
        tips << line(first) + " — in another window";
    for (const TaskInfo &x : others)
        tips << line(x) + " — in another window";
    setToolTip(tips.join('\n'));
    bar->set_busy(first.fraction < 0);
    if (first.fraction >= 0)
        bar->setValue(int(first.fraction * 1000));
    stop->setVisible(first.cancellable);
    stop->setEnabled(!first.cancelling);
    stop->setToolTip("Cancel " + first.title.toLower() + where);
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

Task *run_job(QWidget *parent, const QString &title, Task::Fn fn, Done on_done, bool cancellable, bool quiet)
{
    auto *t = new Task(title, std::move(fn), cancellable);
    QObject *ctx = parent ? static_cast<QObject *>(parent) : static_cast<QObject *>(t);
    if (on_done)
        QObject::connect(t, &Task::result, ctx, [on_done](const QVariant &res) { on_done(res); });
    if (!quiet) {
        QPointer<QWidget> p(parent);
        QObject::connect(t, &Task::error, ctx, [p, title](const QString &msg) { QMessageBox::warning(p, title, msg); });
        if (TaskPanel *panel = panel_for(parent))
            panel->add(t);
    }
    QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
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
        throw Error(shown + " changed while it was being worked on");
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
    int replaced = 0, merged = 0;   // copies that replaced something and merges done: neither can be undone

    QStringList run()
    {
        task->report(0, 0, "Counting…");
        for (const Job &j : jobs) {
            if (j.op == "delete" && !by_count)
                continue;
            if (!(j.op == "move" && same_dev(j.src, j.dst)))
                total += size_of(j.src);
        }
        total = std::max<qint64>(total, 1);
        qint64 started = stats::now_ms();
        for (const Job &j : jobs) {
            task->check();
            // a copy that replaces something can't be undone: the old item is gone, and the copy is all that's left
            bool replacing = j.op == "copy" && lexists(j.dst);
            try {
                if (j.op == "delete")
                    remove(j.src);
                else if (j.op == "move" || j.op == "merge_move")
                    move(j.src, j.dst, j.op == "merge_move");
                else
                    copy(j.src, j.dst, j.op == "merge_copy");
                if (replacing)
                    ++replaced;
                else if (j.op == "merge_copy" || j.op == "merge_move")
                    ++merged;
                else
                    completed << j;
            } catch (const OSError &e) {
                if (e.permission())
                    denied << j;
                else
                    errors << basename(j.src) + ": " + e.message();
            } catch (const Error &e) {
                errors << basename(j.src) + ": " + e.message();
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

    void report_progress(const QString &name)
    {
        QString text = by_count ? QString("%1 of %2 — %3").arg(group_digits(done), group_digits(total), name)
                                : QString("%1 of %2 — %3").arg(human_size(done), human_size(total), name);
        task->report(double(done), double(total), text);
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

    void move(const QString &src, const QString &dst, bool merge)
    {
        if (!merge && same_dev(src, dst)) {
            struct stat st, dst_st;
            if (lstat_(dst, dst_st)) {
                // replaced in one step (swap_in); the old one is deleted once the moved one is in place
                if (lstat_(src, st) && S_ISDIR(dst_st.st_mode) && !S_ISDIR(st.st_mode))
                    throw Error(dst + " is a folder");
                Fd sp(open_parent(src)), dp(open_parent(dst));
                swap_in(sp.fd, basename(src), dp.fd, basename(dst), dst);
            } else {
                util::rename(src, dst);
            }
            report_progress(basename(src));
            return;
        }
        copy(src, dst, merge);
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
                throw Error(shown + " is on another drive (a mount point); not deleting it");
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

    void copy(const QString &src, const QString &dst, bool merge)
    {
        struct stat st;
        if (!lstat_(src, st))
            throw_errno(src);
        Fd sp(open_parent(src)), dp(open_parent(dst));
        copy_at(sp.fd, basename(src), st, src, dp.fd, basename(dst), dst, merge);
    }

    void copy_at(int sdir, const QString &sname, const struct stat &st, const QString &src, int ddir,
                 const QString &dname, const QString &dst, bool merge)
    {
        struct stat dst_st;
        bool dst_exists = stat_at(ddir, dname, dst_st);
        if (dst_exists && S_ISDIR(dst_st.st_mode) && !S_ISDIR(st.st_mode))
            throw Error(dst + " is a folder");
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
            if (::renameat(ddir, enc(part).constData(), ddir, enc(dname).constData()) != 0) {
                int e = errno;
                ::unlinkat(ddir, enc(part).constData(), 0);
                throw_errno(dst, e);
            }
        } else if (S_ISDIR(st.st_mode)) {
            Fd in(open_dir_at(sdir, sname, st, src));
            auto copy_into = [&](int out) {
                for (const QString &e : names_in(in.fd, src)) {
                    struct stat est;
                    if (!stat_at(in.fd, e, est))
                        throw_errno(join(src, e));
                    copy_at(in.fd, e, est, join(src, e), out, e, join(dst, e), merge);
                }
                copy_times_mode(out, st);
            };
            if (dst_exists && merge) {   // into the existing folder (each file in it still replaced whole)
                if (!S_ISDIR(dst_st.st_mode))
                    throw Error(dst + " exists and isn't a folder");
                Fd out(open_dir_at(ddir, dname, dst_st, dst));
                copy_into(out.fd);
            } else {
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
                    swap_in(ddir, part, ddir, dname, dst);
                } catch (...) {
                    discard(ddir, part, join(dirname(dst), part));
                    throw;
                }
            }
        } else if (S_ISREG(st.st_mode)) {
            copy_file_at(sdir, sname, st, src, ddir, dname, dst);
        } else {
            throw Error(src + " isn't a regular file, folder or link");
        }
    }

    void copy_file_at(int sdir, const QString &sname, const struct stat &st, const QString &src, int ddir,
                      const QString &dname, const QString &dst)
    {
        QString name = basename(src);
        Fd in(::openat(sdir, enc(sname).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        struct stat now;
        if (in.fd < 0)
            throw_errno(src);
        if (::fstat(in.fd, &now) != 0 || now.st_dev != st.st_dev || now.st_ino != st.st_ino)
            throw Error(src + " changed while it was being copied");
        // Written to a new file beside the destination and renamed over it once complete: until then an existing file
        // keeps its contents (a cancel or an error removes only the new file), a symlink in the way is replaced (never
        // written through), and another hard link of the old file keeps the old contents.
        QString part;
        int part_fd;
        try {
            part_fd = open_part_at(ddir, QString(), &part);
        } catch (const OSError &e) {
            throw_errno(dst, e.code);   // named as the destination (permission denied there → offer the admin session)
        }
        Fd out(part_fd);
        try {
            std::unique_ptr<char[]> buf(new char[CHUNK]);
            for (;;) {
                if (task->cancelled)
                    throw Cancelled();
                ssize_t n = ::read(in.fd, buf.get(), CHUNK);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0)
                    throw_errno(src);
                if (n == 0)
                    break;
                for (ssize_t off = 0; off < n;) {
                    ssize_t w = ::write(out.fd, buf.get() + off, size_t(n - off));
                    if (w < 0 && errno == EINTR)
                        continue;
                    if (w < 0)
                        throw_errno(dst);
                    off += w;
                }
                done += n;
                report_progress(name);
            }
            copy_times_mode(out.fd, st);
            struct stat old;   // replacing something: on disk before the rename (a crash leaves the old or the new)
            if (stat_at(ddir, dname, old) && ::fsync(out.fd) != 0)
                throw_errno(dst);
            if (::close(out.release()) != 0)
                throw_errno(dst);
            if (::renameat(ddir, enc(part).constData(), ddir, enc(dname).constData()) != 0)
                throw_errno(dst);
        } catch (...) {
            out.reset();
            ::unlinkat(ddir, enc(part).constData(), 0);
            throw;
        }
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
            throw Error(QString("%1 was replaced, but the old one couldn't be deleted (it's left as %2): %3")
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
    QSet<QString> vs;
    for (const Job &j : denied)
        vs << verbs_of.value(j.op);
    QStringList verbs(vs.begin(), vs.end());
    verbs.sort();
    QStringList names;
    for (int i = 0; i < std::min<qsizetype>(10, denied.size()); ++i)
        names << "• " + basename(denied[i].src);
    QString more = denied.size() > 10 ? QString("\n…and %1 more").arg(denied.size() - 10) : QString();
    QString message = QString("%1 item(s) couldn't be %2 because you don't have permission:\n\n%3%4")
                          .arg(denied.size())
                          .arg(verbs.join(" or "), names.join('\n'), more);
    auto work = [denied](Task *task) {
        QStringList errors;
        for (const Job &j : denied) {
            task->check();
            try {
                if (j.op == "delete")
                    admin::session().call(task, "delete", {{"path", j.src}});
                else
                    admin::session().call(task, j.op.contains("copy") ? "copy" : "move",
                                          {{"src", j.src}, {"dst", j.dst}, {"merge", j.op.startsWith("merge")}});
            } catch (const admin::AdminError &e) {
                errors << basename(j.src) + ": " + e.message();
            }
        }
        return errors;
    };
    admin::retry_as_admin(parent, title, message, work, [on_done](bool) {
        if (on_done)
            on_done();
    });
}

Task *start_ops(QWidget *parent, const QList<Job> &jobs, const QString &title, std::function<void()> on_done,
                const QString &undo_label)
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
            if (!moves.isEmpty())
                undo::record("move", undo_label, moves);
            else if (!copies.isEmpty())
                undo::record_paths("create", undo_label, copies);
            if ((*ops)->replaced || (*ops)->merged) {
                QString what = (*ops)->merged ? "Merge" : "Replace";
                if (moves.isEmpty() && copies.isEmpty())
                    undo::record("none", what + " (can't be undone)", {});
                if (auto *win = qobject_cast<QMainWindow *>(p ? p->window() : nullptr))
                    win->statusBar()->showMessage(what + " can't be undone: what was replaced is gone.", 8000);
            }
        }
        QStringList errors = res.toStringList();
        if (!errors.isEmpty())
            QMessageBox::warning(p, title, "Some items could not be processed:\n\n" + errors.mid(0, 20).join('\n'));
        if (!denied.isEmpty() && res.isValid()) {
            retry_denied_as_admin(p, title, denied, on_done);
            return;
        }
        if (on_done)
            on_done();
    };
    auto work = [ops, jobs](Task *task) {
        *ops = std::make_unique<Ops>(task, jobs);
        return QVariant((*ops)->run());
    };
    return run_job(parent, title, work, finished);
}

std::optional<QList<Job>> plan_transfer(QWidget *parent, const QStringList &sources, const QString &dest_dir,
                                        const QString &op)
{
    QList<Job> jobs;
    QString apply_all;
    QString dest_real = realpath(dest_dir);
    for (const QString &src : sources) {
        QString name = basename(rstrip(src, '/'));
        QString src_parent = dirname(abspath(src));
        if (isdir(src) && !islink(src)) {
            QString sr = realpath(src);
            if (dest_real == sr || dest_real.startsWith(sr + "/")) {
                QMessageBox::warning(parent, "Cannot transfer", QString("Cannot %1 “%2” into itself.").arg(op, name));
                continue;
            }
        }
        if (realpath(src_parent) == dest_real) {
            if (op == "move")
                continue;
            jobs << Job{op, src, unique_path(dest_dir, name)};
            continue;
        }
        QString dst = join(dest_dir, name);
        if (lexists(dst)) {
            QString choice = apply_all;
            if (choice.isEmpty()) {
                ConflictDialog dlg(parent, dst, isdir(dst));
                dlg.exec();
                choice = dlg.choice;
                if (choice == "cancel")
                    return std::nullopt;
                if (dlg.all_box->isChecked())
                    apply_all = choice;
            }
            if (choice == "skip")
                continue;
            if (choice == "both") {
                jobs << Job{op, src, unique_path(dest_dir, name, "num")};
            } else {
                bool merge = isdir(dst) && isdir(src);
                jobs << Job{merge ? "merge_" + op : op, src, dst};
            }
        } else {
            jobs << Job{op, src, dst};
        }
    }
    return jobs;
}

void transfer(QWidget *parent, const QStringList &sources, const QString &dest_dir, const QString &op,
              std::function<void()> on_done)
{
    auto jobs = plan_transfer(parent, sources, dest_dir, op);
    if (jobs && !jobs->isEmpty())
        start_ops(parent, *jobs, op == "copy" ? "Copying" : "Moving", on_done, op == "copy" ? "Copy" : "Move");
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
        throw Error("A shortcut can't point to “" + name + "”: its name or folder holds control characters.");
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
        task->report(0, total, "Starting BleachBit…");
        proc::Options opts;
        opts.out = opts.err = proc::DEVNULL;   // it lists every file; what's left afterwards is checked instead
        auto p = proc::spawn(QStringList{"bleachbit", "--shred", "--"} + paths, opts);
        try {
            while (!p->wait_for(250)) {
                task->check();
                int n = total - still_there(paths).size();
                task->report(n, total, QString("%1 of %2 shredded").arg(group_digits(n), group_digits(total)));
            }
        } catch (const Cancelled &) {
            p->kill_group();
            throw;
        }
        return still_there(paths);
    };
    return run_job(parent, title, fn, [on_done](const QVariant &r) {
        if (r.isValid() && on_done)
            on_done(r.toStringList());
    });
}

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
                throw Error(QString("Could not copy %1 from the device: %2").arg(name, msg));
            }
            rename(part, it.dst);
            out << it.dst;
        }
        release();
        return out;
    };
    run_job(parent, "Loading from device", fn, [on_done](const QVariant &res) {
        if (res.isValid())
            on_done(res.toStringList());
    });
}

}  // namespace fileops

// ---------------------------------------------------------------- conflict dialog

ConflictDialog::ConflictDialog(QWidget *parent, const QString &dst, bool is_dir) : QDialog(parent)
{
    setWindowTitle("File conflict");
    auto *lay = new QVBoxLayout(this);
    QString kind = is_dir ? "folder" : "file";
    lay->addWidget(new QLabel(QString("A %1 named “%2” already exists in\n%3").arg(kind, basename(dst), dirname(dst))));
    if (is_dir)
        lay->addWidget(new QLabel("Merge puts the contents into the existing folder: files there with the same names are\n"
                                  "replaced. A merge can't be undone."));
    all_box = new QCheckBox("Apply this action to all conflicts");
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
