// MainWindow's file actions: the clipboard (cut, copy, paste), drops, Move To / Copy To, duplicate, new folders and
// files, rename, Move to Trash, delete, restore from the trash, empty the trash, and links. The window itself (tabs,
// menus, opening files) is in app.cpp.
#include "app.h"

#include "admin.h"
#include "dialogs.h"
#include "fileops.h"
#include "metadata.h"
#include "prefs.h"
#include "undo.h"
#include "util.h"
#include "widgets.h"
#include "windows.h"

#include <QClipboard>
#include <QCursor>
#include <QFile>
#include <QGuiApplication>
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
    statusBar()->showMessage(cut ? txn("%Ln item(s) cut", paths.size()) : txn("%Ln item(s) copied", paths.size()), 3000);
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
            QString dst = unique_path(target, tx("Pasted image") + ".png", "num");
            QImage img = QGuiApplication::clipboard()->image();
            try {   // made whole under a hidden name, then given the new name (write_parts)
                write_parts(dst, [&](int fd) {
                    QFile f;
                    if (!f.open(fd, QIODevice::WriteOnly, QFileDevice::DontCloseHandle) || !img.save(&f, "PNG"))
                        throw OSError(EIO, tx("Couldn't save the pasted image"));
                }, true);
                undo::record_paths("create", TX_NOOP("Paste"), {dst});
                pane()->select_later(dst);
            } catch (const OSError &e) {
                warn(this, tx("Paste"), e.message());
            }
        }
        return;
    }
    if (as_link) {
        make_links(paths, target, "sym");
        return;
    }
    QString how = target_in.isEmpty() ? tx("Paste") : tx("Paste Into Folder");
    if (op == "cut") {
        QPointer<MainWindow> self(this);
        fileops::transfer(this, paths, target, "move", [self]() {
            if (self)
                self->cut_paths.clear();
            QGuiApplication::clipboard()->clear();
        }, how);
    } else {
        // copied files are forgotten once pasted, so an old copy isn't pasted again by mistake (Paste Into Folder is a
        // click away from Move To…); not when the clipboard holds something else by then, or paths copied as text
        QPointer<MainWindow> self(this);
        fileops::transfer(this, paths, target, "copy", nullptr, how, [self, paths]() {
            const QMimeData *md = QGuiApplication::clipboard()->mimeData();
            bool files = md && (md->hasFormat("x-special/gnome-copied-files") || md->hasUrls());
            if (self && files && self->read_clipboard() == qMakePair(QString("copy"), paths))
                QGuiApplication::clipboard()->clear();
        });
    }
}

void MainWindow::handle_drop(const QStringList &paths, const QString &target, bool copy_only)
{
    if (paths.isEmpty())
        return;
    if (target == join(TRASH_DIR(), "files")) {   // onto the Trash (its tab, its place in the sidebar): trashed properly
        trash_paths(paths);
        return;
    }
    Qt::KeyboardModifiers mods = QGuiApplication::keyboardModifiers();
    bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier, alt = mods & Qt::AltModifier;
    if (alt) {
        QMenu m(this);
        if (!copy_only)
            m.addAction(tx("Move Here"), this,
                        [this, paths, target]() { fileops::transfer(this, paths, target, "move", nullptr, tx("Drop (Move Here)")); });
        m.addAction(tx("Copy Here"), this,
                    [this, paths, target]() { fileops::transfer(this, paths, target, "copy", nullptr, tx("Drop (Copy Here)")); });
        m.addAction(tx("Link Here"), this, [this, paths, target]() { make_links(paths, target, "sym"); });
        m.exec(QCursor::pos());
        return;
    }
    if (ctrl && shift) {
        make_links(paths, target, "sym");
        return;
    }
    QString op;
    if (ctrl || copy_only) {
        op = "copy";
    } else if (shift) {
        op = "move";
    } else {
        struct stat a, b;
        bool same = lstat_(paths.first(), a) && stat_(target, b) && a.st_dev == b.st_dev;
        op = same ? "move" : "copy";
    }
    fileops::transfer(this, paths, target, op, nullptr, tx("Drop"));
}

void MainWindow::transfer_to(const QStringList &paths, const QString &op)
{
    choose_folder(this, op == "move" ? "Move To" : "Copy To", cur_dir().isEmpty() ? HOME() : cur_dir(),
                  [this, paths, op](const QString &d) {
                      fileops::transfer(this, paths, d, op, nullptr, op == "move" ? "Move To…" : "Copy To…");
                  });
}

void MainWindow::duplicate(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    QList<fileops::Job> jobs;
    for (const QString &p : paths)
        jobs << fileops::Job{"copy", p, unique_path(dirname(p), basename(p))};
    fileops::start_ops(this, jobs, tx("Duplicating"), nullptr, tx("Duplicate"));
}

void MainWindow::new_folder()
{
    QString cur = cur_dir();
    if (cur.isEmpty())
        return;
    QString def = basename(unique_path(cur, tx("New Folder"), "num"));
    ask_text(this, tx("New Folder"), tx("Folder name:"), def, [this, cur](const QString &name) { make_folder(cur, name); });
}

void MainWindow::make_folder(const QString &cur, const QString &name)
{
    if (name.trimmed().isEmpty())
        return;
    QString p = join(cur, name.trimmed());
    try {
        makedirs(p);
        undo::record_paths("create", TX_NOOP("New Folder"), {p});
        pane()->select_later(p);
    } catch (const OSError &e) {
        if (e.permission()) {
            QPointer<MainWindow> self(this);
            admin::retry_as_admin(
                this, tx("New Folder"), tx("You don't have permission to create folders in “%1”.").arg(cur),
                [p](Task *task) {
                    admin::session().call(task, "mkdir", {{"path", p}});
                    return QStringList();
                },
                [self, p](bool ok2) {
                    if (ok2 && self && self->pane())
                        self->pane()->select_later(p);
                });
        } else {
            warn(this, tx("New Folder"), e.message());
        }
    }
}

void MainWindow::new_folder_with(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    // the folder they were selected in: the one shown, or theirs in search results and lists
    QString dir = dirname(paths.first());
    QString shown = cur_dir();
    if (!shown.isEmpty() && std::all_of(paths.begin(), paths.end(), [&](const QString &p) { return dirname(p) == shown; }))
        dir = shown;
    QString def = basename(unique_path(dir, tx("New Folder"), "num"));
    QPointer<MainWindow> self(this);
    ask_text(this, tx("New Folder with Selection"),
             paths.size() == 1 ? tx("Put “%1” in a new folder named:").arg(basename(paths.first()))
                               : txn("Put the %Ln selected item(s) in a new folder named:", paths.size()),
             def, [self, paths, dir](const QString &name) {
                 if (self)
                     self->move_into_new(paths, dir, name);
             });
}

void MainWindow::move_into_new(const QStringList &paths, const QString &dir, const QString &name_in)
{
    QString name = name_in.trimmed();
    auto again = [this, paths, dir, name](const QString &why) {   // the name window again, the name kept
        QPointer<MainWindow> self(this);
        ask_text(this, tx("New Folder with Selection"), why, name, [self, paths, dir](const QString &n) {
            if (self)
                self->move_into_new(paths, dir, n);
        });
    };
    if (name.isEmpty())
        return;
    if (name.contains('/') || name == "." || name == "..") {
        again(tx("“%1” can't be a folder's name. Another name:").arg(name));
        return;
    }
    // what's still there (the window stayed open meanwhile: items may have been moved or deleted)
    QStringList items, gone;
    QString folder = join(dir, name);
    for (const QString &p : paths)
        (lexists(p) && p != folder ? items : gone) << p;
    if (items.isEmpty()) {
        warn(this, tx("New Folder with Selection"), tx("None of the selected items is there any more."));
        return;
    }
    // the name claimed without replacing anything: mkdir fails on a name that's taken
    if (::mkdir(enc(folder).constData(), 0777) != 0) {
        int err = errno;
        if (err == EEXIST) {
            again(tx("There's already something named “%1” in “%2”. Another name:").arg(name, basename(dir)));
            return;
        }
        if (err == EACCES || err == EPERM) {
            QPointer<MainWindow> self(this);
            admin::retry_as_admin(
                this, tx("New Folder with Selection"),
                tx("You don't have permission to create folders in “%1”.").arg(dir),
                [folder](Task *task) {
                    admin::session().call(task, "mkdir", {{"path", folder}});
                    return QStringList();
                },
                [self, items, gone, folder](bool ok) {
                    if (ok && self)
                        self->start_move_into(items, gone, folder);
                });
            return;
        }
        warn(this, tx("New Folder with Selection"), folder + ": " + QString::fromLocal8Bit(strerror(err)));
        return;
    }
    start_move_into(items, gone, folder);
}

void MainWindow::start_move_into(const QStringList &items, const QStringList &gone, const QString &folder)
{
    QString name = basename(folder);
    QList<fileops::Job> jobs;
    for (const QString &p : items)
        jobs << fileops::Job{"move", p, join(folder, basename(rstrip(p, '/')))};
    QStringList intro{txn("New Folder with Selection in the window “%1”: %Ln item(s) into the new folder “%2”", items.size())
                          .arg(windowTitle(), folder),
                      tx("Each item is moved whole; if this stops part-way, what was moved stays in the folder and the "
                         "rest stays where it was.")};
    if (!gone.isEmpty())
        intro << tx("Left out (moved or deleted meanwhile): %1").arg(gone.join(", "));
    QPointer<MainWindow> self(this);
    fileops::start_ops(
        this, jobs,
        items.size() == 1 ? tx("Moving “%1” into %2").arg(basename(items.first()), name) : txn("Moving %Ln item(s) into %1", items.size()).arg(name),
        [self, jobs, folder]() {
            // undone as one: moved back, and the folder removed once it's empty
            QList<QPair<QString, QString>> undo_items{{folder, QString()}};
            for (const fileops::Job &j : jobs)
                if (lexists(j.dst) && !lexists(j.src))
                    undo_items << qMakePair(j.src, j.dst);
            undo::record("move_into", TX_NOOP("New Folder with Selection"), undo_items);
            if (self && self->pane())
                self->pane()->select_later(folder);
        },
        QString(), intro);
    if (!gone.isEmpty())
        inform(this, tx("New Folder with Selection"),
               tx("%1 of the selected items weren't there any more and were left out:\n\n%2")
                   .arg(QString::number(gone.size()), gone.mid(0, 10).join('\n')));
}

void MainWindow::new_file(const QString &tmpl)
{
    QString cur = cur_dir();
    if (cur.isEmpty())
        return;
    QString base = tmpl.isEmpty() ? tx("Untitled") + ".txt" : basename(tmpl);
    QString def = basename(unique_path(cur, base, "num"));
    ask_text(this, tx("New File"), tx("File name:"), def, [this, cur, tmpl](const QString &name) { make_file(cur, name, tmpl); });
}

void MainWindow::make_file(const QString &cur, const QString &name, const QString &tmpl)
{
    if (name.trimmed().isEmpty())
        return;
    QString p = join(cur, name.trimmed());
    if (lexists(p)) {
        warn(this, tx("New File"), tx("A file with that name already exists."));
        return;
    }
    try {
        if (!tmpl.isEmpty())
            copyfile(tmpl, p, true);
        else
            write_text(p, QByteArray(), true);
        undo::record_paths("create", TX_NOOP("New File"), {p});
        pane()->select_later(p);
    } catch (const OSError &e) {
        if (e.permission()) {
            QPointer<MainWindow> self(this);
            admin::retry_as_admin(
                this, tx("New File"), tx("You don't have permission to create files in “%1”.").arg(cur),
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
            warn(this, tx("New File"), e.message());
        }
    }
}

void MainWindow::rename(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    if (paths.size() > 1) {
        show_free(new BatchRenameDialog(this, paths));
        return;
    }
    QString src = paths.first();
    dialogs::ask_rename(this, src, [this, src](const QString &nw) { rename_to(src, nw); });
}

void MainWindow::rename_to(const QString &src, const QString &nw)
{
    QStringList paths{src};
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
        admin::retry_as_admin(this, tx("Rename"), tx("You don't have permission to rename “%1”.").arg(basename(src)),
                              [src, target](Task *task) {
                                  admin::session().call(task, "rename", {{"src", src}, {"dst", target}});
                                  return QStringList();
                              },
                              renamed);
        return;
    }
    if (!res.error.isEmpty())
        warn(this, tx("Rename"), res.error);
    else {
        undo::record("rename", TX_NOOP("Rename"), {qMakePair(paths.first(), target)});
        renamed(true);
    }
}

void MainWindow::trash_paths(const QStringList &selected)
{
    // what's already in the trash can only be deleted permanently (delete_paths asks first); the rest goes to the
    // trash. A selection can hold both (a search, Recent, Starred).
    QStringList paths, trashed_already;
    for (const QString &p : selected)
        (in_trash(p) ? trashed_already : paths) << p;
    delete_paths(trashed_already);
    if (paths.isEmpty())
        return;
    auto trashed = std::make_shared<QStringList>();
    auto work = [paths, trashed](Task *task) -> QVariant {
        QVariantList failed;
        for (int i = 0; i < paths.size(); ++i) {
            if (task->cancelled)
                break;   // items already moved stay in the trash (and can be undone)
            task->report(i, paths.size(), tx("%1 of %2 — %3").arg(group_digits(i), group_digits(paths.size()), basename(paths[i])));
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
        undo::record_paths("trash", TX_NOOP("Move to Trash"), *trashed);
        fileops::dirsizes::drop_with_parents(*trashed);   // the folders they left are smaller now
        sidebar->refresh();
        QVariantList failed = res.toList();
        if (!failed.isEmpty()) {
            ask_yes(this, tx("Cannot move to trash"),
                    txn("%Ln item(s) could not be moved to the trash:\n%1\n\nDelete them permanently?", failed.size())
                        .arg(failed.first().toStringList().value(1)),
                    [this, failed]() {
                        QList<fileops::Job> jobs;
                        for (const QVariant &f : failed)
                            jobs << fileops::Job{"delete", f.toStringList().value(0), QString()};
                        fileops::start_ops(this, jobs, tx("Deleting"));
                    });
        } else if (res.isValid()) {
            statusBar()->showMessage(txn("Moved %Ln item(s) to the trash", paths.size()), 4000);
        } else {
            statusBar()->showMessage(tx("Move to trash cancelled; items already moved stay in the trash"), 6000);
        }
    };
    fileops::run_job(this, tx("Moving to trash"), work, done);
}

void MainWindow::delete_paths(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    QString question = paths.size() == 1
                           ? tx("Permanently delete “%1”?\n\nThis cannot be undone.").arg(basename(paths.first()))
                           : txn("Permanently delete these %Ln item(s)?\n\nThis cannot be undone.", paths.size());
    auto *box = new QMessageBox(QMessageBox::Warning, tx("Delete Permanently"), question, QMessageBox::Cancel, this);
    QPushButton *del = box->addButton(tx("Delete"), QMessageBox::DestructiveRole);
    box->setDefaultButton(QMessageBox::Cancel);
    ask_box(box, [this, del, paths](QAbstractButton *b) {
        if (b == del)
            delete_now(paths);
    });
}

void MainWindow::delete_now(const QStringList &paths)
{
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
    fileops::start_ops(this, jobs, tx("Deleting"), done);
}

void MainWindow::wipe_metadata(const QStringList &paths)
{
    QStringList imgs;
    for (const QString &p : paths)
        if (metadata::can_wipe(p))
            imgs << p;
    if (imgs.isEmpty())
        return;
    bool copy = prefs::get("metadata_wipe").toString() != "original";
    bool keep = prefs::get("metadata_wipe_keep_basic").toBool();
    QPointer<MainWindow> self(this);
    auto start = [self, imgs, copy, keep]() {
        if (!self)
            return;
        auto work = [imgs, copy, keep](Task *task) -> QVariant {
            QStringList made;
            QVariantList failed;
            for (int i = 0; i < imgs.size(); ++i) {
                if (task->cancelled)
                    break;   // the images done so far stay done
                task->step = i;
                task->report(i, imgs.size(), tx("%1 of %2 — %3").arg(group_digits(i), group_digits(imgs.size()), basename(imgs[i])));
                try {
                    made << metadata::wipe(imgs[i], copy, keep);
                    task->log((copy ? tx("Copied without metadata: %1") : tx("Wiped: %1")).arg(made.last()));
                } catch (const Error &e) {
                    failed << QVariant(QStringList{imgs[i], e.message()});
                    task->log(tx("Failed: %1: %2").arg(imgs[i], e.message()));
                }
            }
            task->was_cancelled = task->cancelled;
            return QVariantMap{{"made", made}, {"failed", failed}};
        };
        auto done = [self, copy](const QVariant &res) {
            QVariantMap m = res.toMap();
            QStringList made = m.value("made").toStringList();
            if (copy)
                undo::record_paths("create", TX_NOOP("Wipe Metadata"), made);   // undone: the copies go to the trash
            if (!self)
                return;
            if (copy && self->pane())
                for (const QString &p : made)
                    self->pane()->select_later(p);
            QVariantList failed = m.value("failed").toList();
            if (!failed.isEmpty()) {
                QStringList lines;
                for (const QVariant &f : failed.mid(0, 10))
                    lines << f.toStringList().value(1);
                warn(self, tx("Wipe Metadata"),
                     txn("%Ln image(s) couldn't be wiped:\n\n%1", failed.size()).arg(lines.join('\n')));
            } else if (res.isValid()) {
                self->statusBar()->showMessage(
                    copy ? txn("Made %Ln copy(ies) without metadata", made.size())
                         : txn("Wiped the metadata of %Ln image(s)", made.size()),
                    4000);
            }
        };
        QStringList steps;
        for (const QString &p : imgs)
            steps << basename(p);
        QStringList intro{copy ? txn("Wipe Metadata: %Ln image(s), each copied beside it without its metadata", imgs.size())
                               : txn("Wipe Metadata: %Ln image(s), wiped in place (no backup)", imgs.size()),
                          tx("Each image is written whole under a hidden name and then renamed into place; if this stops "
                             "part-way, the images done so far stay done and the rest are as they were.")};
        QString title = imgs.size() == 1 ? tx("Wiping the metadata of “%1”").arg(basename(imgs.first()))
                                         : txn("Wiping the metadata of %Ln image(s)", imgs.size());
        fileops::queue_job(self, title, imgs, work, done, true, imgs.size() > 1 ? steps : QStringList(), false, intro, true);
    };
    if (copy) {
        start();
        return;
    }
    // the originals changed with no copy kept: asked first
    QString question =
        imgs.size() == 1 ? tx("Wipe all metadata (camera details, GPS location, dates, comments, image-generation "
                              "prompts…) from “%1”?\n\nNo backup is kept, so this can't be undone.")
                               .arg(basename(imgs.first()))
                         : txn("Wipe all metadata (camera details, GPS location, dates, comments, image-generation "
                               "prompts…) from these %Ln image(s)?\n\nNo backup is kept, so this can't be undone.",
                               imgs.size());
    auto *box = new QMessageBox(QMessageBox::Warning, tx("Wipe Metadata"), question, QMessageBox::Cancel, this);
    box->setInformativeText(tx("Preferences → Images & Videos can keep the originals and make copies instead."));
    QPushButton *ok = box->addButton(tx("Wipe"), QMessageBox::DestructiveRole);
    box->setDefaultButton(QMessageBox::Cancel);
    ask_box(box, [ok, start](QAbstractButton *b) {
        if (b == ok)
            start();
    });
}

void MainWindow::restore(const QStringList &paths)
{
    auto work = [paths](Task *task) -> QVariant {
        QStringList errors;
        for (int i = 0; i < paths.size(); ++i) {
            const QString &p = paths[i];
            task->check();
            task->report(i, paths.size(), tx("%1 of %2 — %3").arg(group_digits(i), group_digits(paths.size()), basename(p)));
            QString orig = trash_original_path(p);
            if (orig.isEmpty()) {
                errors << tx("%1: original location unknown").arg(basename(p));
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
            warn(this, tx("Restore"), errors.mid(0, 20).join('\n'));
    };
    fileops::run_job(this, tx("Restoring"), work, done);
}

void MainWindow::empty_trash()
{
    ask(this, QMessageBox::Warning, tx("Empty Trash"), tx("Permanently delete all items in the trash?"),
        QMessageBox::Yes | QMessageBox::Cancel, [this](QMessageBox::StandardButton b) {
            if (b != QMessageBox::Yes)
                return;
            QList<fileops::Job> jobs;
            for (const QString &p : fileops::trash_contents())
                jobs << fileops::Job{"delete", p, QString()};
            QPointer<MainWindow> self(this);
            fileops::start_ops(this, jobs, tx("Emptying trash"), [self]() {
                if (self)
                    self->sidebar->refresh();
            });
        });
}

// -- shredding with BleachBit (only offered when it's installed: fileops::can_shred)

static const char *SHRED_NOTE = TX_NOOP("BleachBit overwrites them and then deletes them, so they can't be recovered, not even "
                                "from the trash. On SSDs and some file systems, overwriting can't guarantee that every "
                                "old copy of the data is gone.");

static void confirm_shred(QWidget *parent, const QString &title, const QString &question, const QString &button,
                          std::function<void()> yes)
{
    auto *box = new QMessageBox(QMessageBox::Warning, title, question, QMessageBox::Cancel, parent);
    box->setInformativeText(tx(SHRED_NOTE));
    QPushButton *ok = box->addButton(button, QMessageBox::DestructiveRole);
    box->setDefaultButton(QMessageBox::Cancel);
    ask_box(box, [ok, yes](QAbstractButton *b) {
        if (b == ok)
            yes();
    });
}

void MainWindow::shred_paths(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    QString question = paths.size() == 1 ? tx("Shred “%1”?").arg(basename(paths.first())) : txn("Shred %Ln item(s)?", paths.size());
    confirm_shred(this, tx("Shred with BleachBit"), question, tx("Shred"), [this, paths]() {
        QStringList targets = paths;
        for (const QString &p : paths) {   // an item in the trash: its record of where it came from too
            QString info = trash_info_path(p);
            struct stat st;
            if (!info.isEmpty() && lstat_(info, st))
                targets << info;
        }
        run_shred(targets, tx("Shredding with BleachBit"));
    });
}

void MainWindow::empty_trash_with_bleachbit()
{
    QStringList items = fileops::trash_contents();
    if (items.isEmpty()) {
        inform(this, tx("Empty Trash with BleachBit"), tx("The trash is empty."));
        return;
    }
    confirm_shred(this, tx("Empty Trash with BleachBit"), tx("Shred everything in the trash with BleachBit, on every drive?"),
                  tx("Empty Trash"), [this]() { run_shred(fileops::trash_contents(), tx("Emptying trash with BleachBit")); });
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
            list += "\n" + txn("… and %Ln more", names.size() - 10);
        warn(self, tx("Shred with BleachBit"),
             tx("BleachBit couldn't shred these, so they're still there (you may not have permission to "
                "change them):\n\n%1").arg(list));
    });
}

void MainWindow::make_links(const QStringList &paths, QString dest, const QString &kind)
{
    if (dest.isEmpty()) {
        choose_folder(this, tx("Create Links In"), cur_dir().isEmpty() ? HOME() : cur_dir(),
                      [this, paths, kind](const QString &d) { make_links(paths, d, kind); });
        return;
    }
    QStringList made, errors;
    QList<QVariantMap> denied;
    try {
        makedirs(dest, true);
    } catch (const OSError &e) {
        warn(this, tx("Create Link"), e.message());
        return;
    }
    for (const QString &p : paths) {
        QVariantMap plan;
        try {
            plan = fileops::link_plan(kind, p, dest);
            made << fileops::make_link(plan);
        } catch (const OSError &e) {
            if (e.permission())
                denied << plan;
            else
                errors << basename(p) + ": " + e.message();
        } catch (const Error &e) {   // a name a shortcut can't hold
            errors << e.message();
        }
    }
    if (!errors.isEmpty())
        warn(this, tx("Create Link"), errors.join('\n'));
    undo::record_paths("create", TX_NOOP("Create Link"), made);
    QPointer<MainWindow> self(this);
    auto finish = [self, made, denied, dest](bool) {
        if (!self)
            return;
        QStringList done = made;
        for (const QVariantMap &pl : denied)
            if (lexists(fileops::plan_path(pl)))
                done << fileops::plan_path(pl);
        if (!done.isEmpty()) {
            self->statusBar()->showMessage(txn("Created %Ln link(s) in %1", done.size()).arg(dest), 4000);
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
        admin::retry_as_admin(this, tx("Create Link"), tx("You don't have permission to create links in “%1”.").arg(dest),
                              work, finish);
    } else {
        finish(true);
    }
}

