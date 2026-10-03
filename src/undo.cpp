#include "undo.h"

#include "app.h"
#include "fileops.h"
#include "util.h"
#include "widgets.h"

#include <QMessageBox>
#include <QPointer>
#include <QStatusBar>

using namespace util;

namespace undo {

static const int MAX = 50;

struct Op {
    QString kind, label;
    QList<QPair<QString, QString>> items;
};
static QList<Op> stack;   // newest last

Signals *signals_()
{
    static Signals *s = new Signals;
    return s;
}

void record(const QString &kind, const QString &label, const QList<QPair<QString, QString>> &items)
{
    if (items.isEmpty())
        return;
    stack << Op{kind, label, items};
    while (stack.size() > MAX)
        stack.removeFirst();
    Q_EMIT signals_()->changed();
}

void record_paths(const QString &kind, const QString &label, const QStringList &paths)
{
    QList<QPair<QString, QString>> items;
    for (const QString &p : paths)
        items << qMakePair(p, QString());
    record(kind, label, items);
}

QString label() { return stack.isEmpty() ? QString() : stack.last().label; }

// {original path: the most recently trashed copy of it in a trash files/ folder}
static QHash<QString, QString> trashed_index()
{
    QHash<QString, QPair<QString, QString>> newest;   // orig -> (date, path)
    for (const auto &[path, orig] : trashed_items()) {
        if (orig.isEmpty())
            continue;
        QString date;
        bool ok = false;
        for (const QByteArray &line : read_file(trash_info_path(path), &ok).split('\n'))
            if (line.startsWith("DeletionDate="))
                date = QString::fromUtf8(line.mid(13)).trimmed();
        if (!newest.contains(orig) || date >= newest[orig].first)
            newest[orig] = qMakePair(date, path);
    }
    QHash<QString, QString> out;
    for (auto it = newest.begin(); it != newest.end(); ++it)
        out.insert(it.key(), it.value().second);
    return out;
}

void undo(MainWindow *win)
{
    if (stack.isEmpty())
        return;
    Op op = stack.takeLast();
    Q_EMIT signals_()->changed();
    QString title = "Undo " + op.label;
    QPointer<MainWindow> w(win);
    auto done = [w, title, op](const QVariant &res) {
        if (!w)
            return;
        w->sidebar->refresh();
        QStringList errors = res.toStringList();
        if (!errors.isEmpty())
            QMessageBox::warning(w, title, "Some items couldn't be put back:\n\n" + errors.mid(0, 20).join('\n'));
        else if (res.isValid())
            w->statusBar()->showMessage("Undid: " + op.label, 4000);
    };

    if (op.kind == "move") {   // move each item back where it came from
        QStringList errors;
        QList<fileops::Job> jobs;
        for (const auto &[src, dst] : op.items) {
            if (!lexists(dst))
                errors << QString("%1: no longer at %2").arg(basename(dst), dirname(dst));
            else if (lexists(src))
                errors << QString("%1: something new is already at %2").arg(basename(src), src);
            else
                jobs << fileops::Job{"move", dst, src};
        }
        if (!errors.isEmpty())
            QMessageBox::warning(win, title, "Some items couldn't be put back:\n\n" + errors.mid(0, 20).join('\n'));
        if (!jobs.isEmpty())
            fileops::start_ops(win, jobs, title, [done]() { done(QStringList()); });
        return;
    }

    auto work = [op](Task *task) -> QVariant {
        QStringList errors;
        if (op.kind == "rename") {
            for (int i = op.items.size() - 1; i >= 0; --i) {
                task->check();
                const auto &[old_path, new_path] = op.items[i];
                if (!lexists(new_path)) {
                    errors << basename(new_path) + ": no longer exists";
                } else if (lexists(old_path)) {
                    errors << basename(old_path) + ": that name is taken again";
                } else {
                    try {
                        util::rename(new_path, old_path);
                    } catch (const OSError &e) {
                        errors << basename(new_path) + ": " + e.message();
                    }
                }
            }
        } else if (op.kind == "trash") {   // restore from the trash
            QHash<QString, QString> index = trashed_index();
            for (int i = 0; i < op.items.size(); ++i) {
                task->check();
                QString orig = op.items[i].first;
                task->report(i, op.items.size(), basename(orig));
                QString trashed = index.value(orig);
                if (trashed.isEmpty()) {
                    errors << basename(orig) + ": not in the trash any more";
                    continue;
                }
                if (lexists(orig)) {
                    errors << QString("%1: something new is already at %2").arg(basename(orig), orig);
                    continue;
                }
                try {
                    makedirs(dirname(orig), true);
                    QString info = trash_info_path(trashed);
                    util::move(trashed, orig);
                    if (!info.isEmpty() && exists(info))
                        util::unlink(info);
                } catch (const OSError &e) {
                    errors << basename(orig) + ": " + e.message();
                }
            }
        } else if (op.kind == "create") {   // move what was created to the trash
            for (int i = 0; i < op.items.size(); ++i) {
                task->check();
                QString p = op.items[i].first;
                task->report(i, op.items.size(), basename(p));
                if (!lexists(p))
                    continue;
                try {
                    util::trash(p);
                } catch (const OSError &e) {
                    errors << basename(p) + ": " + e.message();
                }
            }
        }
        return errors;
    };
    fileops::run_job(win, title, work, done);
}

}  // namespace undo
