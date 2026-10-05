#include "fileops.h"

#include "admin.h"
#include "atc.h"
#include "undo.h"
#include "util.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cerrno>
#include <cmath>
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
        for (const Job &j : jobs) {
            task->check();
            try {
                if (j.op == "delete")
                    remove(j.src);
                else if (j.op == "move" || j.op == "merge_move")
                    move(j.src, j.dst, j.op == "merge_move");
                else
                    copy(j.src, j.dst, j.op == "merge_copy");
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
            if (lexists(dst))
                remove(dst);
            util::rename(src, dst);
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

    void remove_tree(const QString &path)
    {
        if (isdir(path) && !islink(path)) {
            walk(path, [&](const QString &root, QStringList &dirs, QStringList &files) {
                for (const QString &name : files + dirs) {
                    task->check();
                    QString p = join(root, name);
                    if (dirs.contains(name) && !islink(p))
                        util::rmdir(p);
                    else
                        util::unlink(p);
                    count(name);
                }
                return true;
            }, false);
            util::rmdir(path);
        } else {
            util::unlink(path);
        }
        count(basename(path));
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
        if (islink(src)) {
            if (lexists(dst))
                util::unlink(dst);
            util::symlink(util::readlink(src), dst);
        } else if (isdir(src)) {
            if (exists(dst) && !merge)
                remove(dst);
            makedirs(dst, true);
            for (const QString &name : listdir(src))
                copy(join(src, name), join(dst, name), merge);
            copystat(src, dst, false);
        } else {
            copy_file(src, dst);
        }
    }

    void copy_file(const QString &src, const QString &dst)
    {
        QString name = basename(src);
        int in = ::open(enc(src).constData(), O_RDONLY | O_CLOEXEC);
        if (in < 0)
            throw_errno(src);
        int out = ::open(enc(dst).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (out < 0) {
            int e = errno;
            ::close(in);
            throw_errno(dst, e);
        }
        std::unique_ptr<char[]> buf(new char[CHUNK]);
        auto fail = [&](const QString &p) {
            int e = errno;
            ::close(in);
            ::close(out);
            throw_errno(p, e);
        };
        for (;;) {
            if (task->cancelled) {
                ::close(in);
                ::close(out);
                ::unlink(enc(dst).constData());
                throw Cancelled();
            }
            ssize_t n = ::read(in, buf.get(), CHUNK);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                fail(src);
            if (n == 0)
                break;
            for (ssize_t off = 0; off < n;) {
                ssize_t w = ::write(out, buf.get() + off, size_t(n - off));
                if (w < 0 && errno == EINTR)
                    continue;
                if (w < 0)
                    fail(dst);
                off += w;
            }
            done += n;
            report_progress(name);
        }
        ::close(in);
        if (::close(out) != 0)
            throw_errno(dst);
        copystat(src, dst);
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
    // a freedesktop .desktop launcher pointing to target
    bool is_dir = isdir(target);
    QString body;
    if (isfile(target) && util::access(target, X_OK)) {
        body = QString("[Desktop Entry]\nType=Application\nName=%1\nExec=\"%2\"\nPath=%3\n"
                       "Icon=application-x-executable\nTerminal=false\n")
                   .arg(name, target, dirname(target));
    } else {
        QString icon = is_dir ? QString("folder") : mime_for(target, is_dir).iconName();
        body = QString("[Desktop Entry]\nType=Link\nName=%1\nURL=%2\nIcon=%3\n").arg(name, file_uri(target), icon);
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
