#include "dupes.h"

#include "app.h"
#include "fileops.h"
#include "hashcheck.h"
#include "prefs.h"
#include "thumbs.h"
#include "util.h"
#include "windows.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileIconProvider>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <sys/stat.h>

using namespace util;

namespace dupes {

static const qint64 HEAD = 64 * 1024;   // read first: most files of the same size differ there

static bool hidden_name(const QString &name) { return name.startsWith('.'); }

// the first HEAD bytes' hash; empty if it can't be read
static QByteArray head_hash(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    QByteArray data = f.read(HEAD);
    if (data.isNull() && f.error() != QFile::NoError)
        return QByteArray();
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QList<Group> find(const QStringList &roots, const Options &opts, Task *task, QStringList *unreadable)
{
    auto check = [task]() {
        if (task)
            task->check();
    };
    // 1. every regular file, by size (links are never followed: a symlink isn't a copy)
    QHash<qint64, QList<File>> by_size;
    QSet<QString> seen_paths;
    QSet<QPair<quint64, quint64>> seen_inodes;
    int found = 0;
    auto add = [&](const QString &path) {
        struct stat st;
        if (!lstat_(path, st) || !S_ISREG(st.st_mode) || st.st_size < std::max<qint64>(opts.min_size, 1))
            return;
        if (seen_paths.contains(path))
            return;
        seen_paths << path;
        auto inode = qMakePair(quint64(st.st_dev), quint64(st.st_ino));
        if (opts.skip_hardlinks && seen_inodes.contains(inode))
            return;
        seen_inodes << inode;
        by_size[st.st_size] << File{path, qint64(st.st_size), qint64(st.st_mtime), inode.first, inode.second};
        if (++found % 500 == 0 && task)
            task->report(0, 0, QString("Looking through the files: %1 found").arg(group_digits(found)));
    };
    for (const QString &root : roots) {
        struct stat rst;
        if (!lstat_(root, rst))
            continue;
        if (!S_ISDIR(rst.st_mode)) {
            add(root);
            continue;
        }
        walk(root, [&](const QString &dir, QStringList &dirs, QStringList &files) {
            check();
            if (!opts.recursive && dir != root) {
                dirs.clear();
                return true;
            }
            for (int i = int(dirs.size()) - 1; i >= 0; --i) {   // not hidden ones (unless asked), not other drives
                struct stat st;
                QString d = join(dir, dirs[i]);
                if ((!opts.hidden && hidden_name(dirs[i])) || !opts.recursive || !lstat_(d, st) ||
                    !S_ISDIR(st.st_mode) || st.st_dev != rst.st_dev)
                    dirs.removeAt(i);
            }
            std::sort(files.begin(), files.end());   // (of several names of one file, the same one each time)
            for (const QString &name : std::as_const(files))
                if (opts.hidden || !hidden_name(name))
                    add(join(dir, name));
            return true;
        });
    }
    // 2. same size: the first 64 KB, then the whole file
    QList<QList<File>> candidates;
    for (auto it = by_size.begin(); it != by_size.end(); ++it)
        if (it->size() > 1)
            candidates << *it;
    int to_read = 0;
    for (const auto &c : candidates)
        to_read += int(c.size());
    int read = 0;
    QList<QList<File>> same_head;
    for (const QList<File> &c : std::as_const(candidates)) {
        QHash<QByteArray, QList<File>> by_head;
        for (const File &f : c) {
            check();
            if (task)
                task->report(read, to_read * 2, QString("Comparing the beginnings of %1 files").arg(group_digits(to_read)));
            ++read;
            QByteArray h = head_hash(f.path);
            if (h.isEmpty()) {
                if (unreadable)
                    *unreadable << f.path;
                continue;
            }
            by_head[h] << f;
        }
        for (const QList<File> &g : std::as_const(by_head))
            if (g.size() > 1)
                same_head << g;
    }
    qint64 bytes = 0, done = 0;
    for (const auto &g : same_head)
        for (const File &f : g)
            bytes += f.size > HEAD ? f.size : 0;
    QList<Group> out;
    for (const QList<File> &g : std::as_const(same_head)) {
        QHash<QString, QList<File>> by_hash;
        if (g.first().size <= HEAD) {   // all of it read already
            by_hash.insert(QString(), g);
        } else {
            for (const File &f : g) {
                check();
                try {
                    qint64 before = done;
                    QString h = hashcheck::hash_file(
                        f.path, "sha256",
                        [&](qint64 n, qint64) {
                            if (task)
                                task->report(before + n, std::max<qint64>(bytes, 1),
                                             "Comparing the files of the same size: " + basename(f.path));
                        },
                        check);
                    by_hash[h] << f;
                } catch (const OSError &) {
                    if (unreadable)
                        *unreadable << f.path;
                }
                done += f.size;
            }
        }
        for (QList<File> files : std::as_const(by_hash)) {
            if (files.size() < 2)
                continue;
            std::sort(files.begin(), files.end(), [](const File &a, const File &b) {
                return a.mtime != b.mtime ? a.mtime < b.mtime : a.path < b.path;
            });
            out << Group{files.first().size, files};
        }
    }
    std::sort(out.begin(), out.end(), [](const Group &a, const Group &b) {
        qint64 wa = freed({a}), wb = freed({b});
        return wa != wb ? wa > wb : a.files.first().path < b.files.first().path;
    });
    return out;
}

qint64 freed(const Group &g, const QList<bool> &keep)
{
    // the inodes kept stay; every other inode goes (several names of one inode: once)
    QSet<QPair<quint64, quint64>> kept, gone;
    for (int i = 0; i < g.files.size(); ++i)
        (keep.value(i) ? kept : gone) << qMakePair(g.files[i].dev, g.files[i].ino);
    gone -= kept;
    return kept.isEmpty() ? 0 : qint64(gone.size()) * g.size;
}

qint64 freed(const QList<Group> &groups)
{
    qint64 n = 0;
    for (const Group &g : groups) {
        QList<bool> keep(g.files.size(), false);
        keep[0] = true;
        n += freed(g, keep);
    }
    return n;
}

QList<bool> keep_oldest(const Group &g)
{
    QList<bool> keep(g.files.size(), false);
    int best = 0;
    for (int i = 1; i < g.files.size(); ++i)
        if (g.files[i].mtime < g.files[best].mtime)
            best = i;
    keep[best] = true;
    return keep;
}

QList<bool> keep_newest(const Group &g)
{
    QList<bool> keep(g.files.size(), false);
    int best = 0;
    for (int i = 1; i < g.files.size(); ++i)
        if (g.files[i].mtime > g.files[best].mtime)
            best = i;
    keep[best] = true;
    return keep;
}

QList<bool> keep_in(const Group &g, const QString &folder)
{
    QString f = rstrip(folder, '/');
    QList<bool> keep;
    for (const File &x : g.files)
        keep << x.path.startsWith(f + "/");
    if (!keep.contains(true))   // none there: this group stays as it is
        keep.fill(true);
    return keep;
}

bool changed(const File &f)
{
    struct stat st;
    return !lstat_(f.path, st) || !S_ISREG(st.st_mode) || st.st_size != f.size || st.st_mtime != f.mtime ||
           quint64(st.st_dev) != f.dev || quint64(st.st_ino) != f.ino;
}

QStringList link_over(const File &keep, const QList<File> &others, QStringList *replaced)
{
    QStringList errors;
    if (changed(keep)) {
        for (const File &o : others)
            errors << o.path + ": the copy kept (" + keep.path + ") changed since the search";
        return errors;
    }
    for (const File &o : others) {
        if (o.dev != keep.dev) {
            errors << o.path + ": on another drive than the copy kept";
            continue;
        }
        if (o.ino == keep.ino)   // already the same file
            continue;
        if (changed(o)) {
            errors << o.path + ": changed since the search";
            continue;
        }
        // the link under a part name beside it, renamed over it: the file is itself or the link, never missing
        QString part = join(dirname(o.path), part_name());
        try {
            util::link(keep.path, part);
        } catch (const OSError &e) {
            errors << o.path + ": " + e.message();
            continue;
        }
        try {
            util::rename(part, o.path);
            if (replaced)
                *replaced << o.path;
        } catch (const OSError &e) {
            ::unlink(enc(part).constData());
            errors << o.path + ": " + e.message();
        }
    }
    return errors;
}

void open_window(MainWindow *win, const QStringList &roots)
{
    auto *w = new DupesWindow(win, roots);
    show_free(w);
    w->search();
}

}  // namespace dupes

// ---------------------------------------------------------------- the window

using namespace dupes;

enum { DupGroupRole = Qt::UserRole + 40, DupFileRole };

DupesWindow::DupesWindow(MainWindow *win, const QStringList &roots) : QDialog(win), win(win), roots(roots)
{
    QStringList shown;
    for (const QString &r : roots)
        shown << (r.startsWith(HOME() + "/") ? "~" + r.mid(HOME().size()) : r);
    setWindowTitle("Find Duplicates — " + (roots.size() == 1 ? basename(roots.first()) : QString("%1 folders").arg(roots.size())));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 600);
    auto *lay = new QVBoxLayout(this);
    auto *where = new QLabel;
    where->setWordWrap(true);
    where->setTextFormat(Qt::PlainText);
    where->setText("Files with the same contents in: " + shown.join(", "));
    lay->addWidget(where);

    auto *opts = new QHBoxLayout;
    opts->addWidget(new QLabel("Files of at least"));
    min_kb = new QSpinBox;
    min_kb->setRange(0, 100000000);
    min_kb->setSuffix(" KB");
    min_kb->setAccessibleName("Smallest file size");
    min_kb->setValue(prefs::get("dupes_min_kb").toInt());
    opts->addWidget(min_kb);
    recursive = new QCheckBox("Look in subfolders");
    recursive->setChecked(prefs::get("dupes_recursive").toBool());
    hidden = new QCheckBox("Include hidden files");
    hidden->setChecked(prefs::get("dupes_hidden").toBool());
    skip_links = new QCheckBox("Skip hard links");
    skip_links->setToolTip("Hard links to one file are that file, not copies of it: they take no extra space");
    skip_links->setChecked(prefs::get("dupes_skip_hardlinks").toBool());
    for (QCheckBox *b : {recursive, hidden, skip_links})
        opts->addWidget(b);
    opts->addStretch(1);
    search_btn = new QPushButton("Search");
    stop_btn = new QPushButton("Stop");
    stop_btn->setEnabled(false);
    connect(search_btn, &QPushButton::clicked, this, &DupesWindow::search);
    connect(stop_btn, &QPushButton::clicked, this, [this]() {
        if (task)
            task->cancel();
    });
    opts->addWidget(search_btn);
    opts->addWidget(stop_btn);
    lay->addLayout(opts);

    bar = new QProgressBar;
    bar->setRange(0, 1000);
    bar->setAccessibleName("Progress");
    bar->hide();
    lay->addWidget(bar);
    status = new QLabel;
    status->setWordWrap(true);
    lay->addWidget(status);
    summary = new QLabel;
    summary->setWordWrap(true);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(summary);

    tree = new QTreeWidget;
    tree->setAccessibleName("Duplicates");
    tree->setHeaderLabels({"Keep", "Name", "Folder", "Modified", "Size"});
    tree->setIconSize(QSize(48, 48));
    tree->setUniformRowHeights(false);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    tree->setColumnWidth(0, 60);
    tree->setColumnWidth(3, 140);
    tree->setColumnWidth(4, 90);
    connect(tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *it, int col) {
        if (filling || col != 0 || !it->data(0, DupFileRole).isValid())
            return;
        int g = it->data(0, DupGroupRole).toInt(), f = it->data(0, DupFileRole).toInt();
        if (g < keep.size() && f < keep[g].size())
            keep[g][f] = it->checkState(0) == Qt::Checked;
        update_summary();
    });
    connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        QString path = it->data(1, Qt::UserRole).toString();
        if (!path.isEmpty() && this->win)
            this->win->reveal(path);
    });
    lay->addWidget(tree, 1);

    auto *helpers = new QHBoxLayout;
    helpers->addWidget(new QLabel("Keep in each group:"));
    auto *oldest = new QPushButton("The Oldest");
    connect(oldest, &QPushButton::clicked, this, [this]() { set_keep(keep_oldest); });
    auto *newest = new QPushButton("The Newest");
    connect(newest, &QPushButton::clicked, this, [this]() { set_keep(keep_newest); });
    auto *in_folder = new QPushButton("Those in a Folder…");
    in_folder->setToolTip("Keep the files in a folder you choose (and below it); a group with none there stays as it is");
    connect(in_folder, &QPushButton::clicked, this, [this]() {
        QPointer<DupesWindow> self(this);
        choose_folder(this, "Keep the Files In", this->roots.value(0, HOME()), [self](const QString &dir) {
            if (self)
                self->set_keep([dir](const Group &g) { return keep_in(g, dir); });
        });
    });
    auto *all = new QPushButton("All");
    connect(all, &QPushButton::clicked, this, [this]() {
        set_keep([](const Group &g) { return QList<bool>(g.files.size(), true); });
    });
    for (QPushButton *b : {oldest, newest, in_folder, all})
        helpers->addWidget(b);
    helpers->addStretch(1);
    lay->addLayout(helpers);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    trash_btn = bb->addButton("Move the Others to Trash", QDialogButtonBox::ActionRole);
    trash_btn->setToolTip("The files not kept go to the trash (Undo brings them back)");
    link_btn = bb->addButton("Replace the Others with Hard Links…", QDialogButtonBox::ActionRole);
    link_btn->setToolTip("Each file not kept becomes a hard link to the one kept: the same file under both names, "
                         "taking its space once. Only on the same drive.");
    connect(trash_btn, &QPushButton::clicked, this, &DupesWindow::trash_others);
    connect(link_btn, &QPushButton::clicked, this, &DupesWindow::link_others);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
    lay->addWidget(bb);
    if (ThumbnailManager *t = ThumbnailManager::instance())
        connect(t, &ThumbnailManager::updated, this, &DupesWindow::icon_ready);
    update_summary();
}

DupesWindow::~DupesWindow()
{
    if (task)
        task->cancel();
}

void DupesWindow::closeEvent(QCloseEvent *ev)
{
    if (task)
        task->cancel();
    QDialog::closeEvent(ev);
}

void DupesWindow::search()
{
    if (task)
        return;
    Options o;
    o.min_size = std::max<qint64>(1, qint64(min_kb->value()) * 1024);
    o.hidden = hidden->isChecked();
    o.skip_hardlinks = skip_links->isChecked();
    o.recursive = recursive->isChecked();
    prefs::set_many({{"dupes_min_kb", min_kb->value()}, {"dupes_hidden", o.hidden},
                     {"dupes_skip_hardlinks", o.skip_hardlinks}, {"dupes_recursive", o.recursive}});
    groups.clear();
    keep.clear();
    show_results();
    search_btn->setEnabled(false);
    stop_btn->setEnabled(true);
    bar->setValue(0);
    bar->show();
    status->setText("Waiting for the drive…");
    auto found = std::make_shared<QList<Group>>();
    auto unreadable = std::make_shared<QStringList>();
    QStringList r = roots;
    QPointer<DupesWindow> self(this);
    QWidget *parent = win ? static_cast<QWidget *>(win) : this;
    task = fileops::queue_job(
        parent, "Finding duplicates in " + (r.size() == 1 ? basename(r.first()) : QString("%1 folders").arg(r.size())), r,
        [r, o, found, unreadable](Task *t) {
            *found = dupes::find(r, o, t, unreadable.get());
            return QVariant(true);
        },
        [self, found, unreadable](const QVariant &res) {
            if (!self)
                return;
            self->task = nullptr;
            self->search_btn->setEnabled(true);
            self->stop_btn->setEnabled(false);
            self->bar->hide();
            if (res.isValid()) {
                self->groups = *found;
                for (const Group &g : std::as_const(self->groups))
                    self->keep << QList<bool>(g.files.size(), true);
                self->status->setText(unreadable->isEmpty() ? QString()
                                                            : QString("%1 file(s) couldn't be read and were left out, such as %2")
                                                                  .arg(QString::number(unreadable->size()), unreadable->first()));
            } else {
                self->status->setText("Stopped.");
            }
            self->show_results();
            Q_EMIT self->finished_search();
        },
        true, {}, false, {}, true);
    connect(task, &Task::progress, this, [this](double f, const QString &text) {
        bar->setValue(f < 0 ? 0 : int(f * 1000));
        bar->setRange(0, f < 0 ? 0 : 1000);
        status->setText(text);
    });
}

void DupesWindow::show_results()
{
    filling = true;
    tree->clear();
    QFileIconProvider icons;
    ThumbnailManager *t = ThumbnailManager::instance();
    for (int gi = 0; gi < groups.size(); ++gi) {
        const Group &g = groups[gi];
        auto *gr = new QTreeWidgetItem(tree);
        gr->setText(0, QString("%1 identical files, %2 each").arg(QString::number(g.files.size()), human_size(g.size)));
        gr->setFirstColumnSpanned(true);
        QFont f = gr->font(0);
        f.setBold(true);
        gr->setFont(0, f);
        for (int fi = 0; fi < g.files.size(); ++fi) {
            const File &x = g.files[fi];
            auto *it = new QTreeWidgetItem(gr);
            it->setData(0, DupGroupRole, gi);
            it->setData(0, DupFileRole, fi);
            it->setCheckState(0, keep.value(gi).value(fi, true) ? Qt::Checked : Qt::Unchecked);
            it->setText(1, basename(x.path));
            it->setData(1, Qt::UserRole, x.path);
            it->setToolTip(1, x.path);
            it->setText(2, dirname(x.path));
            it->setText(3, fmt_time(x.mtime, "%Y-%m-%d %H:%M"));
            it->setText(4, human_size(x.size));
            QIcon thumb = t ? t->get_icon(x.path, x.mtime, false, 48, x.size, this) : QIcon();
            it->setIcon(1, thumb.isNull() ? icons.icon(QFileInfo(x.path)) : thumb);
        }
        gr->setExpanded(true);
    }
    filling = false;
    update_summary();
}

void DupesWindow::icon_ready(const QString &path)
{
    ThumbnailManager *t = ThumbnailManager::instance();
    for (int gi = 0; gi < tree->topLevelItemCount(); ++gi) {
        QTreeWidgetItem *gr = tree->topLevelItem(gi);
        for (int i = 0; i < gr->childCount(); ++i) {
            QTreeWidgetItem *it = gr->child(i);
            if (it->data(1, Qt::UserRole).toString() != path)
                continue;
            const File &x = groups[it->data(0, DupGroupRole).toInt()].files[it->data(0, DupFileRole).toInt()];
            QIcon thumb = t->get_icon(x.path, x.mtime, false, 48, x.size, this);
            if (!thumb.isNull())
                it->setIcon(1, thumb);
        }
    }
}

void DupesWindow::update_summary()
{
    if (groups.isEmpty()) {
        summary->setText(searching() ? QString() : QString("No files with the same contents were found."));
    } else {
        int copies = 0, marked = 0;
        qint64 going = 0;
        for (int gi = 0; gi < groups.size(); ++gi) {
            copies += int(groups[gi].files.size()) - 1;
            if (keep[gi].contains(true)) {
                marked += int(keep[gi].count(false));
                going += freed(groups[gi], keep[gi]);
            }
        }
        summary->setText(QString("<b>%1 can be freed</b>: %2 group(s) of identical files, %3 copies more than needed. "
                                 "Not kept: %4 file(s), freeing %5.")
                             .arg(human_size(freed(groups)), QString::number(groups.size()), QString::number(copies),
                                  QString::number(marked), human_size(going)));
    }
    bool any = false;
    for (int gi = 0; gi < keep.size(); ++gi)
        any = any || (keep[gi].contains(true) && keep[gi].contains(false));
    trash_btn->setEnabled(any && !searching());
    link_btn->setEnabled(any && !searching());
}

void DupesWindow::set_keep(const std::function<QList<bool>(const Group &)> &rule)
{
    for (int gi = 0; gi < groups.size(); ++gi)
        keep[gi] = rule(groups[gi]);
    filling = true;
    for (int gi = 0; gi < tree->topLevelItemCount(); ++gi) {
        QTreeWidgetItem *gr = tree->topLevelItem(gi);
        for (int i = 0; i < gr->childCount(); ++i) {
            QTreeWidgetItem *it = gr->child(i);
            int g = it->data(0, DupGroupRole).toInt(), f = it->data(0, DupFileRole).toInt();
            it->setCheckState(0, keep[g][f] ? Qt::Checked : Qt::Unchecked);
        }
    }
    filling = false;
    update_summary();
}

void DupesWindow::drop_files(const QSet<QString> &gone)
{
    QList<Group> g2;
    QList<QList<bool>> k2;
    for (int gi = 0; gi < groups.size(); ++gi) {
        Group g{groups[gi].size, {}};
        QList<bool> k;
        for (int fi = 0; fi < groups[gi].files.size(); ++fi)
            if (!gone.contains(groups[gi].files[fi].path)) {
                g.files << groups[gi].files[fi];
                k << keep[gi][fi];
            }
        if (g.files.size() > 1) {
            g2 << g;
            k2 << k;
        }
    }
    groups = g2;
    keep = k2;
    show_results();
}

void DupesWindow::trash_others()
{
    if (!win)
        return;
    QStringList paths, changed_files;
    int no_keeper = 0;
    for (int gi = 0; gi < groups.size(); ++gi) {
        if (!keep[gi].contains(true)) {   // never every copy
            no_keeper += 1;
            continue;
        }
        for (int fi = 0; fi < groups[gi].files.size(); ++fi) {
            if (keep[gi][fi])
                continue;
            const File &f = groups[gi].files[fi];
            (changed(f) ? changed_files : paths) << f.path;
        }
    }
    if (!paths.isEmpty())
        win->trash_paths(paths);
    QStringList notes;
    if (no_keeper)
        notes << QString("%1 group(s) with no file kept were left as they are: keep at least one in each.").arg(no_keeper);
    if (!changed_files.isEmpty())
        notes << QString("%1 file(s) changed since the search and were left as they are, such as %2.")
                     .arg(QString::number(changed_files.size()), changed_files.first());
    if (!notes.isEmpty())
        inform(this, "Move the Others to Trash", notes.join("\n\n"));
    QSet<QString> gone(paths.begin(), paths.end());
    drop_files(gone);
}

void DupesWindow::link_others()
{
    QList<QPair<File, QList<File>>> work;
    int n = 0, no_keeper = 0;
    QStringList all;
    for (int gi = 0; gi < groups.size(); ++gi) {
        int k = int(keep[gi].indexOf(true));
        if (k < 0) {
            no_keeper += 1;
            continue;
        }
        QList<File> others;
        for (int fi = 0; fi < groups[gi].files.size(); ++fi)
            if (!keep[gi][fi])
                others << groups[gi].files[fi];
        if (others.isEmpty())
            continue;
        n += int(others.size());
        work << qMakePair(groups[gi].files[k], others);
        all << groups[gi].files[k].path;
    }
    if (work.isEmpty())
        return;
    QPointer<DupesWindow> self(this);
    ask_yes(this, "Replace with Hard Links",
            QString("Replace %1 file(s) with hard links to the copy kept in each group?\n\n"
                    "Each then is the same file as the one kept, under its own name: one date, one set of permissions, "
                    "and a change to it shows under every name. Files on another drive than the one kept are left as "
                    "they are.%2\n\nThis can't be undone.")
                .arg(QString::number(n), no_keeper ? QString("\n\nGroups with no file kept are left as they are.") : QString()),
            [self, work, all]() {
                if (!self || !self->win)
                    return;
                auto replaced = std::make_shared<QStringList>();
                fileops::queue_job(
                    self->win, "Replacing duplicates with hard links", all,
                    [work, replaced](Task *t) {
                        QStringList errors;
                        for (int i = 0; i < work.size(); ++i) {
                            t->check();
                            t->report(i, work.size(), basename(work[i].first.path));
                            errors << link_over(work[i].first, work[i].second, replaced.get());
                        }
                        return QVariant(errors);
                    },
                    [self, replaced](const QVariant &res) {
                        if (!self)
                            return;
                        QStringList errors = res.toStringList();
                        if (!errors.isEmpty())
                            warn(self, "Replace with Hard Links",
                                 QString("%1 file(s) were left as they are:\n%2")
                                     .arg(QString::number(errors.size()), errors.mid(0, 10).join("\n")));
                        self->drop_files(QSet<QString>(replaced->begin(), replaced->end()));
                    },
                    true, {}, false, {}, true);
            });
}
