// MainWindow's file actions: the clipboard (cut, copy, paste), drops, Move To / Copy To, duplicate, new folders and
// files, rename, Move to Trash, delete, restore from the trash, empty the trash, and links. The window itself (tabs,
// menus, opening files) is in app.cpp.
#include "app.h"

#include "admin.h"
#include "dialogs.h"
#include "fileops.h"
#include "undo.h"
#include "util.h"
#include "widgets.h"

#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>

using namespace util;

void MainWindow::reveal(const QString &path)
{
    Pane *p = new_tab(dirname(path));
    if (p)
        p->select_later(path);
}

void MainWindow::copy_text(const QStringList &items) { QGuiApplication::clipboard()->setText(items.join('\n')); }

void MainWindow::clip(bool cut, const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    auto *md = new QMimeData;
    md->setUrls(url_list(paths));
    QStringList uris;
    for (const QString &p : paths)
        uris << file_uri(p);
    md->setData("x-special/gnome-copied-files", ((cut ? "cut" : "copy") + QString("\n") + uris.join('\n')).toUtf8());
    md->setData("application/x-kde-cutselection", cut ? "1" : "0");
    md->setText(paths.join('\n'));
    QGuiApplication::clipboard()->setMimeData(md);
    cut_paths = cut ? QSet<QString>(paths.begin(), paths.end()) : QSet<QString>();
    for (Pane *p : panes())
        p->view()->viewport()->update();
    statusBar()->showMessage(QString("%1 item(s) %2").arg(QString::number(paths.size()), cut ? "cut" : "copied"), 3000);
}

QPair<QString, QStringList> MainWindow::read_clipboard() const
{
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    if (!md)
        return {"copy", {}};
    if (md->hasFormat("x-special/gnome-copied-files")) {
        QStringList lines = QString::fromUtf8(md->data("x-special/gnome-copied-files")).split('\n');
        if (!lines.isEmpty() && !(lines.size() == 1 && lines[0].isEmpty())) {
            QString op = lines.first().trimmed();
            QStringList paths;
            for (const QString &u : lines.mid(1)) {
                if (u.trimmed().isEmpty())
                    continue;
                QString p = uri_to_path(u.trimmed());
                if (!p.isEmpty())
                    paths << p;
            }
            return {op == "cut" ? "cut" : "copy", paths};
        }
    }
    if (md->hasUrls()) {
        bool cut = md->data("application/x-kde-cutselection") == "1";
        QStringList paths;
        for (const QUrl &u : md->urls())
            if (u.isLocalFile())
                paths << u.toLocalFile();
        return {cut ? "cut" : "copy", paths};
    }
    if (md->hasText()) {
        QStringList paths;
        for (const QString &l : md->text().split('\n'))
            if (l.trimmed().startsWith('/'))
                paths << l.trimmed();
        if (!paths.isEmpty() && std::all_of(paths.begin(), paths.end(), [](const QString &p) { return exists(p); }))
            return {"copy", paths};
    }
    return {"copy", {}};
}

void MainWindow::paste(const QString &target_in, bool as_link)
{
    QString target = target_in.isEmpty() ? cur_dir() : target_in;
    if (target.isEmpty())
        return;
    auto [op, paths] = read_clipboard();
    if (paths.isEmpty()) {
        const QMimeData *md = QGuiApplication::clipboard()->mimeData();
        if (md && md->hasImage()) {
            QString dst = unique_path(target, "Pasted image.png", "num");
            if (QGuiApplication::clipboard()->image().save(dst, "PNG"))
                undo::record_paths("create", "Paste", {dst});
            pane()->select_later(dst);
        }
        return;
    }
    if (as_link) {
        make_links(paths, target, "sym");
        return;
    }
    if (op == "cut") {
        QPointer<MainWindow> self(this);
        fileops::transfer(this, paths, target, "move", [self]() {
            if (self)
                self->cut_paths.clear();
            QGuiApplication::clipboard()->clear();
        });
    } else {
        fileops::transfer(this, paths, target, "copy");
    }
}

void MainWindow::handle_drop(const QStringList &paths, const QString &target)
{
    if (paths.isEmpty())
        return;
    Qt::KeyboardModifiers mods = QGuiApplication::keyboardModifiers();
    bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier, alt = mods & Qt::AltModifier;
    if (alt) {
        QMenu m(this);
        m.addAction("Move Here", this, [this, paths, target]() { fileops::transfer(this, paths, target, "move"); });
        m.addAction("Copy Here", this, [this, paths, target]() { fileops::transfer(this, paths, target, "copy"); });
        m.addAction("Link Here", this, [this, paths, target]() { make_links(paths, target, "sym"); });
        m.exec(QCursor::pos());
        return;
    }
    if (ctrl && shift) {
        make_links(paths, target, "sym");
        return;
    }
    QString op;
    if (ctrl) {
        op = "copy";
    } else if (shift) {
        op = "move";
    } else {
        struct stat a, b;
        bool same = lstat_(paths.first(), a) && stat_(target, b) && a.st_dev == b.st_dev;
        op = same ? "move" : "copy";
    }
    fileops::transfer(this, paths, target, op);
}

void MainWindow::transfer_to(const QStringList &paths, const QString &op)
{
    QString d = dialogs::choose_dir(this, op == "move" ? "Move To" : "Copy To", cur_dir().isEmpty() ? HOME() : cur_dir());
    if (!d.isEmpty())
        fileops::transfer(this, paths, d, op);
}

void MainWindow::duplicate(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    QList<fileops::Job> jobs;
    for (const QString &p : paths)
        jobs << fileops::Job{"copy", p, unique_path(dirname(p), basename(p))};
    fileops::start_ops(this, jobs, "Duplicating", nullptr, "Duplicate");
}

void MainWindow::new_folder()
{
    QString cur = cur_dir();
    if (cur.isEmpty())
        return;
    QString def = basename(unique_path(cur, "New Folder", "num"));
    bool ok = false;
    QString name = QInputDialog::getText(this, "New Folder", "Folder name:", QLineEdit::Normal, def, &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    QString p = join(cur, name.trimmed());
    try {
        makedirs(p);
        undo::record_paths("create", "New Folder", {p});
        pane()->select_later(p);
    } catch (const OSError &e) {
        if (e.permission()) {
            QPointer<MainWindow> self(this);
            admin::retry_as_admin(
                this, "New Folder", QString("You don't have permission to create folders in “%1”.").arg(cur),
                [p](Task *task) {
                    admin::session().call(task, "mkdir", {{"path", p}});
                    return QStringList();
                },
                [self, p](bool ok2) {
                    if (ok2 && self && self->pane())
                        self->pane()->select_later(p);
                });
        } else {
            QMessageBox::warning(this, "New Folder", e.message());
        }
    }
}

void MainWindow::new_file(const QString &tmpl)
{
    QString cur = cur_dir();
    if (cur.isEmpty())
        return;
    QString base = tmpl.isEmpty() ? QString("Untitled.txt") : basename(tmpl);
    QString def = basename(unique_path(cur, base, "num"));
    bool ok = false;
    QString name = QInputDialog::getText(this, "New File", "File name:", QLineEdit::Normal, def, &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    QString p = join(cur, name.trimmed());
    if (lexists(p)) {
        QMessageBox::warning(this, "New File", "A file with that name already exists.");
        return;
    }
    try {
        if (!tmpl.isEmpty())
            copyfile(tmpl, p);
        else
            write_text(p, QByteArray(), true);
        undo::record_paths("create", "New File", {p});
        pane()->select_later(p);
    } catch (const OSError &e) {
        if (e.permission()) {
            QPointer<MainWindow> self(this);
            admin::retry_as_admin(
                this, "New File", QString("You don't have permission to create files in “%1”.").arg(cur),
                [p, tmpl](Task *task) {
                    if (!tmpl.isEmpty())
                        admin::session().call(task, "copyfile", {{"src", tmpl}, {"dst", p}});
                    else
                        admin::session().call(task, "touch", {{"path", p}});
                    return QStringList();
                },
                [self, p](bool ok2) {
                    if (ok2 && self && self->pane())
                        self->pane()->select_later(p);
                });
        } else {
            QMessageBox::warning(this, "New File", e.message());
        }
    }
}

void MainWindow::rename(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    if (paths.size() > 1) {
        BatchRenameDialog(this, paths).exec();
        return;
    }
    QString nw = dialogs::ask_rename(this, paths.first());
    if (nw.isEmpty())
        return;
    QString target = join(dirname(paths.first()), nw);
    QPointer<MainWindow> self(this);
    auto renamed = [self, target](bool ok) {
        if (ok && self && self->pane()) {
            Pane *p = self->pane();
            p->select_later(target);
            QTimer::singleShot(150, p, &Pane::try_select);
        }
    };
    auto res = dialogs::do_rename(paths.first(), nw);
    if (res.denied) {
        QString src = paths.first();
        admin::retry_as_admin(this, "Rename", QString("You don't have permission to rename “%1”.").arg(basename(src)),
                              [src, target](Task *task) {
                                  admin::session().call(task, "rename", {{"src", src}, {"dst", target}});
                                  return QStringList();
                              },
                              renamed);
        return;
    }
    if (!res.error.isEmpty())
        QMessageBox::warning(this, "Rename", res.error);
    else {
        undo::record("rename", "Rename", {qMakePair(paths.first(), target)});
        renamed(true);
    }
}

void MainWindow::trash_paths(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    if (in_trash(paths.first())) {
        delete_paths(paths);
        return;
    }
    auto trashed = std::make_shared<QStringList>();
    auto work = [paths, trashed](Task *task) -> QVariant {
        QVariantList failed;
        for (int i = 0; i < paths.size(); ++i) {
            if (task->cancelled)
                break;   // items already moved stay in the trash (and can be undone)
            task->report(i, paths.size(),
                         QString("%1 of %2 — %3").arg(group_digits(i), group_digits(paths.size()), basename(paths[i])));
            try {
                util::trash(paths[i]);
                *trashed << paths[i];
            } catch (const OSError &e) {
                failed << QVariant(QStringList{paths[i], e.message()});
            }
        }
        return task->cancelled ? QVariant() : QVariant(failed);
    };
    auto done = [this, paths, trashed](const QVariant &res) {
        undo::record_paths("trash", "Move to Trash", *trashed);
        sidebar->refresh();
        QVariantList failed = res.toList();
        if (!failed.isEmpty()) {
            auto r = QMessageBox::question(this, "Cannot move to trash",
                                           QString("%1 item(s) could not be moved to the trash:\n%2\n\nDelete them permanently?")
                                               .arg(QString::number(failed.size()), failed.first().toStringList().value(1)));
            if (r == QMessageBox::Yes) {
                QList<fileops::Job> jobs;
                for (const QVariant &f : failed)
                    jobs << fileops::Job{"delete", f.toStringList().value(0), QString()};
                fileops::start_ops(this, jobs, "Deleting");
            }
        } else if (res.isValid()) {
            statusBar()->showMessage(QString("Moved %1 item(s) to the trash").arg(group_digits(paths.size())), 4000);
        } else {
            statusBar()->showMessage("Move to trash cancelled; items already moved stay in the trash", 6000);
        }
    };
    fileops::run_job(this, "Moving to trash", work, done);
}

void MainWindow::delete_paths(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    QString what = paths.size() == 1 ? QString("“%1”").arg(basename(paths.first()))
                                     : QString("these %1 items").arg(paths.size());
    QMessageBox box(QMessageBox::Warning, "Delete Permanently",
                    QString("Permanently delete %1?\n\nThis cannot be undone.").arg(what), QMessageBox::Cancel, this);
    QPushButton *del = box.addButton("Delete", QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != del)
        return;
    QPointer<MainWindow> self(this);
    auto done = [self, paths]() {
        for (const QString &p : paths) {
            QString info_path = trash_info_path(p);
            if (!info_path.isEmpty())
                ::unlink(enc(info_path).constData());
        }
        if (self)
            self->sidebar->refresh();
    };
    QList<fileops::Job> jobs;
    for (const QString &p : paths)
        jobs << fileops::Job{"delete", p, QString()};
    fileops::start_ops(this, jobs, "Deleting", done);
}

void MainWindow::restore(const QStringList &paths)
{
    auto work = [paths](Task *task) -> QVariant {
        QStringList errors;
        for (int i = 0; i < paths.size(); ++i) {
            const QString &p = paths[i];
            task->check();
            task->report(i, paths.size(), QString("%1 of %2 — %3").arg(group_digits(i), group_digits(paths.size()), basename(p)));
            QString orig = trash_original_path(p);
            if (orig.isEmpty()) {
                errors << basename(p) + ": original location unknown";
                continue;
            }
            QString dest = !lexists(orig) ? orig : unique_path(dirname(orig), basename(orig), "num");
            try {
                makedirs(dirname(dest), true);
                QString info_path = trash_info_path(p);
                util::move(p, dest);
                if (!info_path.isEmpty() && exists(info_path))
                    util::unlink(info_path);
            } catch (const OSError &e) {
                errors << basename(p) + ": " + e.message();
            }
        }
        return errors;
    };
    auto done = [this](const QVariant &res) {
        sidebar->refresh();
        QStringList errors = res.toStringList();
        if (!errors.isEmpty())
            QMessageBox::warning(this, "Restore", errors.mid(0, 20).join('\n'));
    };
    fileops::run_job(this, "Restoring", work, done);
}

void MainWindow::empty_trash()
{
    auto r = QMessageBox::warning(this, "Empty Trash", "Permanently delete all items in the trash?",
                                  QMessageBox::Yes | QMessageBox::Cancel);
    if (r != QMessageBox::Yes)
        return;
    QList<fileops::Job> jobs;
    for (const QString &p : fileops::trash_contents())
        jobs << fileops::Job{"delete", p, QString()};
    QPointer<MainWindow> self(this);
    fileops::start_ops(this, jobs, "Emptying trash", [self]() {
        if (self)
            self->sidebar->refresh();
    });
}

// -- shredding with BleachBit (only offered when it's installed: fileops::can_shred)

static const char *SHRED_NOTE = "BleachBit overwrites them and then deletes them, so they can't be recovered, not even "
                                "from the trash. On SSDs and some file systems, overwriting can't guarantee that every "
                                "old copy of the data is gone.";

static bool confirm_shred(QWidget *parent, const QString &title, const QString &question, const QString &button)
{
    QMessageBox box(QMessageBox::Warning, title, question, QMessageBox::Cancel, parent);
    box.setInformativeText(SHRED_NOTE);
    QPushButton *yes = box.addButton(button, QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == yes;
}

void MainWindow::shred_paths(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    QString what = paths.size() == 1 ? "“" + basename(paths.first()) + "”" : QString("%1 items").arg(paths.size());
    if (!confirm_shred(this, "Shred with BleachBit", "Shred " + what + "?", "Shred"))
        return;
    QStringList targets = paths;
    for (const QString &p : paths) {   // an item in the trash: its record of where it came from too
        QString info = trash_info_path(p);
        struct stat st;
        if (!info.isEmpty() && lstat_(info, st))
            targets << info;
    }
    run_shred(targets, "Shredding with BleachBit");
}

void MainWindow::empty_trash_with_bleachbit()
{
    QStringList items = fileops::trash_contents();
    if (items.isEmpty()) {
        QMessageBox::information(this, "Empty Trash with BleachBit", "The trash is empty.");
        return;
    }
    if (!confirm_shred(this, "Empty Trash with BleachBit",
                       "Shred everything in the trash with BleachBit, on every drive?", "Empty Trash"))
        return;
    run_shred(items, "Emptying trash with BleachBit");
}

void MainWindow::run_shred(const QStringList &targets, const QString &title)
{
    QPointer<MainWindow> self(this);
    fileops::shred(this, targets, title, [self](const QStringList &left) {
        if (!self)
            return;
        self->sidebar->refresh();
        QStringList names;
        for (const QString &p : left)
            if (!p.endsWith(".trashinfo"))
                names << p;
        if (names.isEmpty())
            return;
        QString list = names.mid(0, 10).join("\n");
        if (names.size() > 10)
            list += QString("\n… and %1 more").arg(names.size() - 10);
        QMessageBox::warning(self, "Shred with BleachBit",
                             "BleachBit couldn't shred these, so they're still there (you may not have permission to "
                             "change them):\n\n" + list);
    });
}

void MainWindow::make_links(const QStringList &paths, QString dest, const QString &kind)
{
    if (dest.isEmpty()) {
        dest = dialogs::choose_dir(this, "Create Links In", cur_dir().isEmpty() ? HOME() : cur_dir());
        if (dest.isEmpty())
            return;
    }
    QStringList made, errors;
    QList<QVariantMap> denied;
    try {
        makedirs(dest, true);
    } catch (const OSError &e) {
        QMessageBox::warning(this, "Create Link", e.message());
        return;
    }
    for (const QString &p : paths) {
        QVariantMap plan = fileops::link_plan(kind, p, dest);
        try {
            made << fileops::make_link(plan);
        } catch (const OSError &e) {
            if (e.permission())
                denied << plan;
            else
                errors << basename(p) + ": " + e.message();
        }
    }
    if (!errors.isEmpty())
        QMessageBox::warning(this, "Create Link", errors.join('\n'));
    undo::record_paths("create", "Create Link", made);
    QPointer<MainWindow> self(this);
    auto finish = [self, made, denied, dest](bool) {
        if (!self)
            return;
        QStringList done = made;
        for (const QVariantMap &pl : denied)
            if (lexists(fileops::plan_path(pl)))
                done << fileops::plan_path(pl);
        if (!done.isEmpty()) {
            self->statusBar()->showMessage(QString("Created %1 link(s) in %2").arg(QString::number(done.size()), dest), 4000);
            if (dest == self->cur_dir())
                self->pane()->select_later(done.first());
        }
    };
    if (!denied.isEmpty()) {
        auto work = [denied](Task *task) {
            QStringList errs;
            for (const QVariantMap &pl : denied) {
                QVariantMap args = pl;
                QString op = args.take("op").toString();
                try {
                    admin::session().call(task, op, args);
                } catch (const admin::AdminError &e) {
                    errs << basename(fileops::plan_path(pl)) + ": " + e.message();
                }
            }
            return errs;
        };
        admin::retry_as_admin(this, "Create Link", QString("You don't have permission to create links in “%1”.").arg(dest),
                              work, finish);
    } else {
        finish(true);
    }
}

