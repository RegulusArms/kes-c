#include "compare.h"

#include "app.h"
#include "drives.h"
#include "util.h"
#include "windows.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace util;

namespace compare {

static const char *PARTIAL_DIR = ".kes-rsync-partial";

bool same_time(qint64 a, qint64 b) { return std::abs(a - b) <= SLACK; }

// Kestrel's and rsync's unfinished pieces: not part of either folder
static bool ignored(const QString &name)
{
    return name == PARTIAL_DIR || (name.startsWith(".kes-") && name.endsWith(".part"));
}

bool same_bytes(const QString &a, const QString &b, Task *task)
{
    QFile fa(a), fb(b);
    if (!fa.open(QIODevice::ReadOnly))
        throw OSError(EACCES, tx("Can't read %1").arg(a));
    if (!fb.open(QIODevice::ReadOnly))
        throw OSError(EACCES, tx("Can't read %1").arg(b));
    if (fa.size() != fb.size())
        return false;
    const qint64 CHUNK = 1 << 20;
    for (;;) {
        if (task)
            task->check();
        QByteArray x = fa.read(CHUNK), y = fb.read(CHUNK);
        if (x != y)
            return false;
        if (x.isEmpty())
            return true;
    }
}

static void diff_dir(const QString &left, const QString &right, const QString &rel, bool deep, Task *task,
                     QList<Item> &out)
{
    QString l = rel.isEmpty() ? left : join(left, rel), r = rel.isEmpty() ? right : join(right, rel);
    QStringList names;
    for (const QString &side : {l, r}) {
        try {
            for (const QString &n : listdir(side))
                if (!ignored(n) && !names.contains(n))
                    names << n;
        } catch (const OSError &) {
        }
    }
    std::sort(names.begin(), names.end());
    for (const QString &name : std::as_const(names)) {
        if (task)
            task->check();
        QString sub = rel.isEmpty() ? name : rel + "/" + name;
        struct stat ls, rs;
        bool hl = lstat_(join(l, name), ls), hr = lstat_(join(r, name), rs);
        Item it;
        it.rel = sub;
        if (hl) {
            it.left_dir = S_ISDIR(ls.st_mode);
            it.left_size = it.left_dir ? 0 : qint64(ls.st_size);
            it.left_mtime = ls.st_mtime;
        }
        if (hr) {
            it.right_dir = S_ISDIR(rs.st_mode);
            it.right_size = it.right_dir ? 0 : qint64(rs.st_size);
            it.right_mtime = rs.st_mtime;
        }
        if (hl && !hr) {
            it.kind = ONLY_LEFT;
        } else if (!hl && hr) {
            it.kind = ONLY_RIGHT;
        } else if (it.left_dir && it.right_dir) {   // both folders: what's in them
            diff_dir(left, right, sub, deep, task, out);
            continue;
        } else if (it.left_dir != it.right_dir || S_ISLNK(ls.st_mode) != S_ISLNK(rs.st_mode)) {
            it.kind = DIFFERENT;
        } else if (S_ISLNK(ls.st_mode)) {   // links: by where they point
            it.kind = readlink(join(l, name)) == readlink(join(r, name)) ? SAME : DIFFERENT;
        } else {
            if (task)
                task->report(0, 0, tx("Comparing %1").arg(sub));
            bool size = ls.st_size == rs.st_size, time = same_time(ls.st_mtime, rs.st_mtime);
            auto bytes_same = [&]() {
                try {
                    return same_bytes(join(l, name), join(r, name), task);
                } catch (const OSError &) {
                    return false;
                }
            };
            if (size && time)
                it.kind = deep && !bytes_same() ? DIFFERENT : SAME;
            else if (!time)
                it.kind = deep && size && bytes_same() ? SAME : ls.st_mtime > rs.st_mtime ? LEFT_NEWER : RIGHT_NEWER;
            else
                it.kind = DIFFERENT;
        }
        out << it;
    }
}

QList<Item> diff(const QString &left, const QString &right, bool deep, Task *task)
{
    QList<Item> out;
    diff_dir(left, right, QString(), deep, task, out);
    return out;
}

QString kind_text(Kind k, const QString &left_name, const QString &right_name)
{
    switch (k) {
    case SAME:
        return tx("The same");
    case ONLY_LEFT:
        return tx("Only in %1").arg(left_name);
    case ONLY_RIGHT:
        return tx("Only in %1").arg(right_name);
    case LEFT_NEWER:
        return tx("Newer in %1").arg(left_name);
    case RIGHT_NEWER:
        return tx("Newer in %1").arg(right_name);
    case DIFFERENT:
        return tx("Different contents");
    }
    return QString();
}

QList<Step> plan(const QList<Item> &items, bool deletes)
{
    QList<Step> out, gone;
    for (const Item &it : items) {
        switch (it.kind) {
        case SAME:
            break;
        case ONLY_LEFT:
            out << Step{Step::COPY, it.rel, it.left_dir, it.left_dir ? 0 : it.left_size, false};
            break;
        case LEFT_NEWER:
        case RIGHT_NEWER:
        case DIFFERENT:
            out << Step{Step::REPLACE, it.rel, it.left_dir, it.left_dir ? 0 : it.left_size, it.kind == RIGHT_NEWER};
            break;
        case ONLY_RIGHT:
            if (deletes)
                gone << Step{Step::DELETE, it.rel, it.right_dir, it.right_dir ? 0 : it.right_size, false};
            break;
        }
    }
    return out + gone;   // the copies first: stopped half-way, nothing has been deleted before what replaces it is in
}

QList<fileops::Job> jobs_for(const QString &source, const QString &dest, const QList<Step> &steps)
{
    QList<fileops::Job> out;
    for (const Step &s : steps) {
        QString src = join(source, s.rel), dst = join(dest, s.rel);
        if (s.op == Step::DELETE)
            out << fileops::Job{"delete", dst, QString()};
        else
            out << fileops::Job{"copy", src, dst, s.op == Step::REPLACE};
    }
    return out;
}

bool have_rsync() { return which("rsync"); }

QStringList rsync_argv(const QString &source, const QString &dest, bool deletes)
{
    // -a: what's inside, links as links, dates and permissions; no --inplace: each file is written under a
    // temporary name and renamed over the old one when complete (an interrupted one waits in the partial folder)
    QStringList argv{"rsync", "-a", QString("--modify-window=%1").arg(SLACK), QString("--partial-dir=") + PARTIAL_DIR,
                     "--info=progress2", "--no-inc-recursive"};
    if (deletes)
        argv << "--delete";
    argv << "--" << rstrip(source, '/') + "/" << rstrip(dest, '/') + "/";
    return argv;
}

void run_rsync(Task *task, const QString &source, const QString &dest, bool deletes)
{
    QStringList argv = rsync_argv(source, dest, deletes);
    task->log("  " + argv.join(' '));
    proc::Options o;
    o.c_utf8 = true;   // its messages in English, to read them
    auto p = proc::spawn(argv, o);
    static const QRegularExpression pct("(\\d+)%");
    QByteArray out, err;
    auto take = [&](int fd, QByteArray &into) {
        char buf[8192];
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0)
            return false;
        QByteArray chunk(buf, int(n));
        task->log_output(chunk);
        into += chunk;
        if (into.size() > 65536)
            into.remove(0, into.size() - 16384);
        if (&into == &out) {
            QRegularExpressionMatchIterator it = pct.globalMatch(QString::fromLatin1(out.right(200)));
            int last = -1;
            while (it.hasNext())
                last = it.next().captured(1).toInt();
            if (last >= 0)
                task->report(last, 100, tx("rsync: %1%").arg(last));
        }
        return true;
    };
    bool out_open = true, err_open = true;
    while (out_open || err_open) {
        task->check();   // a stop throws: p goes, and rsync with it (what it was writing waits in the partial folder)
        fd_set rf;
        FD_ZERO(&rf);
        int top = 0;
        if (out_open) {
            FD_SET(p->out, &rf);
            top = std::max(top, p->out);
        }
        if (err_open) {
            FD_SET(p->err, &rf);
            top = std::max(top, p->err);
        }
        struct timeval tv = {0, 200000};
        int r = ::select(top + 1, &rf, nullptr, nullptr, &tv);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            continue;
        if (out_open && FD_ISSET(p->out, &rf))
            out_open = take(p->out, out);
        if (err_open && FD_ISSET(p->err, &rf))
            err_open = take(p->err, err);
    }
    int rc = p->wait();
    if (rc != 0 && rc != 24) {   // 24: files vanished on the source meanwhile (the rest is done)
        QStringList lines = QString::fromUtf8(err).split('\n', Qt::SkipEmptyParts);
        throw Error(tx("rsync stopped with an error (%1): %2")
                        .arg(QString::number(rc), lines.isEmpty() ? tx("no message") : lines.mid(lines.size() - 3).join(" ")));
    }
}

// ---- saved pairs

QString pairs_file() { return join(CONFIG_DIR(), "sync-pairs.json"); }

QList<Pair> pairs()
{
    QList<Pair> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(read_file(pairs_file())).array()) {
        QJsonObject o = v.toObject();
        Pair p;
        p.name = o.value("name").toString();
        p.source = o.value("source").toString();
        p.dest = o.value("dest").toString();
        p.deletes = o.value("deletes").toBool();
        p.rsync = o.value("rsync").toBool(true);
        p.source_uuid = o.value("source_uuid").toString();
        p.source_rel = o.value("source_rel").toString();
        p.dest_uuid = o.value("dest_uuid").toString();
        p.dest_rel = o.value("dest_rel").toString();
        if (!p.source.isEmpty() && !p.dest.isEmpty())
            out << p;
    }
    return out;
}

void save_pairs(const QList<Pair> &list)
{
    QJsonArray a;
    for (const Pair &p : list)
        a << QJsonObject{{"name", p.name},           {"source", p.source},         {"dest", p.dest},
                         {"deletes", p.deletes},     {"rsync", p.rsync},           {"source_uuid", p.source_uuid},
                         {"source_rel", p.source_rel}, {"dest_uuid", p.dest_uuid}, {"dest_rel", p.dest_rel}};
    makedirs(CONFIG_DIR(), true);
    write_atomic(pairs_file(), QJsonDocument(a).toJson());
}

static QString by_uuid_dir() { return qEnvironmentVariable("KESTREL_TEST_BY_UUID", "/dev/disk/by-uuid"); }

QString fs_uuid(const QString &path, QString *rel)
{
    QString root = drives::mount_root(path), source;
    for (const drives::Mount &m : drives::mounts())
        if (m.root == root)
            source = m.source;   // (the last mounted there is the one seen)
    if (!source.startsWith("/dev/"))
        return QString();
    QString dev = realpath(source);
    QStringList names;
    try {
        names = listdir(by_uuid_dir());
    } catch (const OSError &) {
        return QString();
    }
    for (const QString &name : names)
        if (realpath(join(by_uuid_dir(), name)) == dev) {
            if (rel)
                *rel = relpath(path, root);
            return name;
        }
    return QString();
}

QString resolve(const QString &path, const QString &uuid, const QString &rel)
{
    if (uuid.isEmpty())
        return path;
    if (isdir(path) && fs_uuid(path) == uuid)
        return path;
    for (const drives::Mount &m : drives::mounts()) {
        if (!m.source.startsWith("/dev/") || fs_uuid(m.root) != uuid)
            continue;
        QString p = rel.isEmpty() || rel == "." ? m.root : join(m.root, rel);
        if (isdir(p))
            return p;
    }
    return path;
}

Pair make_pair(const QString &name, const QString &source, const QString &dest, bool deletes, bool rsync)
{
    Pair p{name, source, dest, deletes, rsync, {}, {}, {}, {}};
    p.source_uuid = fs_uuid(source, &p.source_rel);
    p.dest_uuid = fs_uuid(dest, &p.dest_rel);
    return p;
}

void open_compare(MainWindow *win, const QString &left)
{
    QPointer<MainWindow> w(win);
    choose_folder(win, tx("Compare “%1” With").arg(basename(left)), dirname(left), [w, left](const QString &right) {
        if (!w)
            return;
        auto *cw = new CompareWindow(w, left, right);
        show_free(cw);
        cw->run();
    });
}

void open_sync(MainWindow *win, const QString &source) { show_free(new SyncWindow(win, source, QString())); }

}  // namespace compare

using namespace compare;

static QString side_text(bool there, bool dir, qint64 size, qint64 mtime)
{
    if (!there)
        return "—";
    return (dir ? tx("Folder") : human_size(size)) + " · " + i18n::date_time(QDateTime::fromSecsSinceEpoch(mtime));
}

// ---------------------------------------------------------------- Compare With…

CompareWindow::CompareWindow(MainWindow *win, const QString &left, const QString &right) : QDialog(win), win(win)
{
    setWindowTitle(tx("Compare Folders"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(960, 600);
    auto *lay = new QVBoxLayout(this);
    auto *grid = new QGridLayout;
    left_edit = new QLineEdit(left);
    left_edit->setAccessibleName(tx("Left folder"));
    right_edit = new QLineEdit(right);
    right_edit->setAccessibleName(tx("Right folder"));
    int row = 0;
    for (QLineEdit *e : {left_edit, right_edit}) {
        grid->addWidget(new QLabel(e == left_edit ? tx("Left:") : tx("Right:")), row, 0);
        grid->addWidget(e, row, 1);
        auto *choose = new QPushButton(tx("Choose…"));
        connect(choose, &QPushButton::clicked, this, [this, e]() {
            QPointer<QLineEdit> edit(e);
            choose_folder(this, tx("Compare"), e->text(), [edit](const QString &d) {
                if (edit)
                    edit->setText(d);
            });
        });
        grid->addWidget(choose, row, 2);
        ++row;
    }
    auto *swap = new QPushButton(tx("⇅ Swap"));
    connect(swap, &QPushButton::clicked, this, [this]() {
        QString l = left_edit->text();
        left_edit->setText(right_edit->text());
        right_edit->setText(l);
        run();
    });
    grid->addWidget(swap, 0, 3, 2, 1);
    lay->addLayout(grid);

    auto *opts = new QHBoxLayout;
    deep = new QCheckBox(tx("Compare contents (slower: reads the files)"));
    deep->setToolTip(tx("Without it, files of the same size and date count as the same"));
    opts->addWidget(deep);
    opts->addStretch(1);
    compare_btn = new QPushButton(tx("Compare"));
    stop_btn = new QPushButton(tx("Stop"));
    stop_btn->setEnabled(false);
    connect(compare_btn, &QPushButton::clicked, this, &CompareWindow::run);
    connect(stop_btn, &QPushButton::clicked, this, [this]() {
        if (task)
            task->cancel();
    });
    opts->addWidget(compare_btn);
    opts->addWidget(stop_btn);
    lay->addLayout(opts);

    auto *show = new QHBoxLayout;
    show->addWidget(new QLabel(tx("Show:")));
    const QList<QPair<QString, bool>> kinds = {{tx("The same"), false},   {tx("Only on the left"), true}, {tx("Only on the right"), true},
                                               {tx("Newer on the left"), true}, {tx("Newer on the right"), true},
                                               {tx("Different contents"), true}};
    for (const auto &[label, on] : kinds) {   // in Kind's order
        auto *b = new QCheckBox(label);
        b->setChecked(on);
        connect(b, &QCheckBox::toggled, this, &CompareWindow::fill);
        shows << b;
        show->addWidget(b);
    }
    show->addStretch(1);
    lay->addLayout(show);

    bar = new QProgressBar;
    bar->setRange(0, 0);
    bar->hide();
    lay->addWidget(bar);
    summary = new QLabel;
    summary->setWordWrap(true);
    lay->addWidget(summary);
    tree = new QTreeWidget;
    tree->setAccessibleName(tx("Differences"));
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setHeaderLabels({tx("Item"), tx("Difference"), tx("Left"), tx("Right")});
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->setColumnWidth(1, 170);
    tree->setColumnWidth(2, 190);
    tree->setColumnWidth(3, 190);
    connect(tree, &QTreeWidget::itemDoubleClicked, this, [this]() {
        QList<Item> sel = selected();
        if (!sel.isEmpty())
            open_side(sel.first().left_size < 0);
    });
    lay->addWidget(tree, 1);

    auto *acts = new QHBoxLayout;
    auto add = [&](const QString &text, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(text);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        acts->addWidget(b);
    };
    add(tx("Open Left"), tx("Open the left folder's copy"), [this]() { open_side(false); });
    add(tx("Open Right"), tx("Open the right folder's copy"), [this]() { open_side(true); });
    add(tx("Copy to Right →"), tx("Copy the left's items to the right (replacing what's there, asking first)"),
        [this]() { copy_across(true); });
    add(tx("← Copy to Left"), tx("Copy the right's items to the left (replacing what's there, asking first)"),
        [this]() { copy_across(false); });
    add(tx("Trash on Left"), tx("Move the left's items to the trash"), [this]() { trash_side(false); });
    add(tx("Trash on Right"), tx("Move the right's items to the trash"), [this]() { trash_side(true); });
    acts->addStretch(1);
    auto *close = new QPushButton(tx("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    acts->addWidget(close);
    lay->addLayout(acts);
}

CompareWindow::~CompareWindow()
{
    if (task)
        task->cancel();
}

void CompareWindow::closeEvent(QCloseEvent *ev)
{
    if (task)
        task->cancel();
    QDialog::closeEvent(ev);
}

void CompareWindow::run()
{
    if (task)
        task->cancel();
    QString l = left_edit->text(), r = right_edit->text();
    if (!isdir(l) || !isdir(r)) {
        summary->setText(tx("Choose two folders to compare."));
        return;
    }
    items.clear();
    fill();
    compare_btn->setEnabled(false);
    stop_btn->setEnabled(true);
    bar->show();
    summary->setText(tx("Comparing…"));
    auto found = std::make_shared<QList<Item>>();
    bool d = deep->isChecked();
    QPointer<CompareWindow> self(this);
    QWidget *parent = win ? static_cast<QWidget *>(win) : this;
    task = fileops::queue_job(
        parent, tx("Comparing %1 and %2").arg(basename(l), basename(r)), {l, r},
        [l, r, d, found](Task *t) {
            *found = diff(l, r, d, t);
            return QVariant(true);
        },
        [self, found](const QVariant &res) {
            if (!self)
                return;
            self->task = nullptr;
            self->compare_btn->setEnabled(true);
            self->stop_btn->setEnabled(false);
            self->bar->hide();
            self->items = res.isValid() ? *found : QList<Item>();
            self->fill();
            if (!res.isValid())
                self->summary->setText(tx("Stopped."));
            Q_EMIT self->compared();
        },
        true, {}, false, {}, true);
}

void CompareWindow::fill()
{
    QString ln = basename(left_edit->text()), rn = basename(right_edit->text());
    if (ln == rn) {
        ln = tx("the left");
        rn = tx("the right");
    }
    tree->clear();
    QHash<Kind, int> counts;
    for (int i = 0; i < items.size(); ++i) {
        const Item &it = items[i];
        counts[it.kind] += 1;
        if (!shows.value(it.kind) || !shows[it.kind]->isChecked())
            continue;
        auto *row = new QTreeWidgetItem(tree);
        row->setData(0, Qt::UserRole, i);
        row->setText(0, it.rel + ((it.left_dir || it.right_dir) && it.kind != DIFFERENT ? "/" : ""));
        row->setText(1, kind_text(it.kind, ln, rn));
        row->setText(2, side_text(it.left_size >= 0, it.left_dir, it.left_size, it.left_mtime));
        row->setText(3, side_text(it.right_size >= 0, it.right_dir, it.right_size, it.right_mtime));
    }
    if (task)
        return;
    int differ = int(items.size()) - counts.value(SAME);
    summary->setText(items.isEmpty() ? tx("Both folders are empty.")
                     : differ == 0   ? txn("The folders are the same (%Ln item(s)).", items.size())
                                     : txn("%Ln difference(s): %1 only on the left, %2 only on the right, %3 newer on the "
                                           "left, %4 newer on the right, %5 with other contents. %6 the same.",
                                           differ)
                                           .arg(group_digits(counts.value(ONLY_LEFT)), group_digits(counts.value(ONLY_RIGHT)),
                                                group_digits(counts.value(LEFT_NEWER)), group_digits(counts.value(RIGHT_NEWER)),
                                                group_digits(counts.value(DIFFERENT)), group_digits(counts.value(SAME))));
}

QList<Item> CompareWindow::selected() const
{
    QList<Item> out;
    for (QTreeWidgetItem *row : tree->selectedItems())
        out << items.value(row->data(0, Qt::UserRole).toInt());
    return out;
}

void CompareWindow::open_side(bool right)
{
    if (!win || !win->pane())
        return;
    QString base = right ? right_edit->text() : left_edit->text();
    QStringList paths;
    for (const Item &it : selected())
        if ((right ? it.right_size : it.left_size) >= 0)
            paths << join(base, it.rel);
    if (!paths.isEmpty())
        win->open_paths(win->pane(), paths);
}

void CompareWindow::copy_across(bool to_right)
{
    if (!win)
        return;
    QString from = to_right ? left_edit->text() : right_edit->text(), to = to_right ? right_edit->text() : left_edit->text();
    QList<fileops::Job> jobs;
    int replacing = 0;
    for (const Item &it : selected()) {
        bool there = (to_right ? it.left_size : it.right_size) >= 0, other = (to_right ? it.right_size : it.left_size) >= 0;
        if (!there || it.kind == SAME)
            continue;
        jobs << fileops::Job{"copy", join(from, it.rel), join(to, it.rel), other};
        replacing += other;
    }
    if (jobs.isEmpty())
        return;
    QPointer<CompareWindow> self(this);
    QPointer<MainWindow> w(win);
    auto go = [self, w, jobs, to]() {
        if (!self || !w)
            return;
        fileops::start_ops(w, jobs, tx("Copying to %1").arg(basename(to)), [self]() {
            if (self)
                self->run();
        }, tx("Copy"));
    };
    if (replacing)
        ask_yes(this, tx("Copy Across"),
                tx("Replace %1 item(s) in “%2” with the copies from “%3”? What's replaced can't be brought back.")
                    .arg(QString::number(replacing), basename(to), basename(from)),
                go);
    else
        go();
}

void CompareWindow::trash_side(bool right)
{
    if (!win)
        return;
    QString base = right ? right_edit->text() : left_edit->text();
    QStringList paths;
    QSet<QString> rels;
    for (const Item &it : selected())
        if ((right ? it.right_size : it.left_size) >= 0) {
            paths << join(base, it.rel);
            rels << it.rel;
        }
    if (paths.isEmpty())
        return;
    win->trash_paths(paths);
    // that side's copy is gone: what's left is on the other side only
    for (int i = int(items.size()) - 1; i >= 0; --i) {
        Item &it = items[i];
        if (!rels.contains(it.rel))
            continue;
        if ((right ? it.left_size : it.right_size) < 0) {
            items.removeAt(i);
            continue;
        }
        (right ? it.right_size : it.left_size) = -1;
        (right ? it.right_dir : it.left_dir) = false;
        it.kind = right ? ONLY_LEFT : ONLY_RIGHT;
    }
    fill();
}

// ---------------------------------------------------------------- Sync To…

SyncWindow::SyncWindow(MainWindow *win, const QString &source, const QString &dest) : QDialog(win), win(win)
{
    setWindowTitle(tx("Sync Folders"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(820, 600);
    auto *lay = new QVBoxLayout(this);

    auto *saved_row = new QHBoxLayout;
    saved_row->addWidget(new QLabel(tx("Saved:")));
    saved = new QComboBox;
    saved->setAccessibleName(tx("Saved sync pairs"));
    saved->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    saved_row->addWidget(saved, 1);
    auto *run = new QPushButton(tx("Run"));
    run->setToolTip(tx("Preview the saved pair and sync it (asking first when it deletes)"));
    connect(run, &QPushButton::clicked, this, [this]() { run_pair(saved->currentIndex()); });
    auto *remove = new QPushButton(tx("Remove"));
    connect(remove, &QPushButton::clicked, this, [this]() {
        QList<Pair> l = pairs();
        int i = saved->currentIndex();
        if (i < 0 || i >= l.size())
            return;
        l.removeAt(i);
        try {
            save_pairs(l);
        } catch (const OSError &e) {
            warn(this, tx("Sync Folders"), e.message());
        }
        fill_saved();
    });
    saved_row->addWidget(run);
    saved_row->addWidget(remove);
    lay->addLayout(saved_row);

    auto *grid = new QGridLayout;
    source_edit = new QLineEdit(source);
    source_edit->setAccessibleName(tx("Source folder"));
    dest_edit = new QLineEdit(dest);
    dest_edit->setAccessibleName(tx("Destination folder"));
    int row = 0;
    for (QLineEdit *e : {source_edit, dest_edit}) {
        grid->addWidget(new QLabel(e == source_edit ? tx("From:") : tx("To:")), row, 0);
        grid->addWidget(e, row, 1);
        auto *choose = new QPushButton(tx("Choose…"));
        connect(choose, &QPushButton::clicked, this, [this, e]() {
            QPointer<QLineEdit> edit(e);
            choose_folder(this, e == source_edit ? tx("Sync From") : tx("Sync To"), e->text().isEmpty() ? HOME() : e->text(),
                          [edit](const QString &d) {
                              if (edit)
                                  edit->setText(d);
                          });
        });
        grid->addWidget(choose, row, 2);
        connect(e, &QLineEdit::textChanged, this, [this]() {   // what the preview showed is for other folders
            steps.clear();
            list->clear();
            summary->setText(tx("Preview to see what will be done."));
            sync_btn->setEnabled(false);
        });
        ++row;
    }
    lay->addLayout(grid);
    deletes = new QCheckBox(tx("Also delete what's on the destination but not in the source (asks again)"));
    deletes->setToolTip(tx("Off: the destination keeps files you've deleted from the source"));
    lay->addWidget(deletes);
    auto *how = new QHBoxLayout;
    how->addWidget(new QLabel(tx("Copy with:")));
    use_rsync = new QRadioButton(tx("rsync (resumes interrupted copies, only sends what changed)"));
    use_kestrel = new QRadioButton(tx("Kestrel's own copy"));
    auto *group = new QButtonGroup(this);
    group->addButton(use_rsync);
    group->addButton(use_kestrel);
    bool rs = have_rsync();
    use_rsync->setEnabled(rs);
    if (!rs)
        use_rsync->setToolTip(tx("rsync isn't installed"));
    (rs ? use_rsync : use_kestrel)->setChecked(true);
    how->addWidget(use_rsync);
    how->addWidget(use_kestrel);
    how->addStretch(1);
    lay->addLayout(how);
    connect(deletes, &QCheckBox::toggled, this, [this]() {
        if (!planned_source.isEmpty())
            preview();
    });

    auto *pv = new QHBoxLayout;
    preview_btn = new QPushButton(tx("Preview"));
    connect(preview_btn, &QPushButton::clicked, this, [this]() { preview(); });
    auto *save = new QPushButton(tx("Save Pair…"));
    save->setToolTip(tx("Keep these folders and choices, to sync them again in one click"));
    connect(save, &QPushButton::clicked, this, [this]() {
        QPointer<SyncWindow> self(this);
        ask_text(this, tx("Save Sync Pair"), tx("Name:"),
                 basename(source_edit->text()) + " → " + basename(dest_edit->text()), [self](const QString &name) {
                     if (self && !name.trimmed().isEmpty())
                         self->save_pair(name.trimmed());
                 });
    });
    pv->addWidget(preview_btn);
    pv->addWidget(save);
    pv->addStretch(1);
    lay->addLayout(pv);
    summary = new QLabel(tx("Preview to see what will be done."));
    summary->setWordWrap(true);
    lay->addWidget(summary);
    list = new QTreeWidget;
    list->setAccessibleName(tx("What the sync will do"));
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setHeaderLabels({tx("Will"), tx("Item"), tx("Size")});
    list->header()->setStretchLastSection(false);
    list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    list->setColumnWidth(0, 200);
    list->setColumnWidth(2, 90);
    lay->addWidget(list, 1);
    bar = new QProgressBar;
    bar->setRange(0, 1000);
    bar->hide();
    lay->addWidget(bar);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    sync_btn = bb->addButton(tx("Sync Now"), QDialogButtonBox::AcceptRole);
    sync_btn->setEnabled(false);
    connect(sync_btn, &QPushButton::clicked, this, &SyncWindow::sync_now);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
    lay->addWidget(bb);
    fill_saved();
}

SyncWindow::~SyncWindow()
{
    if (task && task->title.startsWith("Comparing"))   // a preview isn't needed any more; a sync goes on
        task->cancel();
}

void SyncWindow::closeEvent(QCloseEvent *ev)
{
    if (task && task->title.startsWith("Comparing"))
        task->cancel();
    QDialog::closeEvent(ev);
}

void SyncWindow::fill_saved()
{
    saved->clear();
    for (const Pair &p : pairs())
        saved->addItem(p.deletes ? tx("%1  (%2 → %3, deleting)").arg(p.name, p.source, p.dest)
                                 : QString("%1  (%2 → %3)").arg(p.name, p.source, p.dest));
    if (!saved->count())
        saved->addItem(tx("None yet: choose the folders, then Save Pair…"));
    saved->setEnabled(!pairs().isEmpty());
}

void SyncWindow::save_pair(const QString &name)
{
    QList<Pair> l = pairs();
    Pair p = make_pair(name, source_edit->text(), dest_edit->text(), deletes->isChecked(), use_rsync->isChecked());
    bool replaced = false;
    for (Pair &x : l)
        if (x.name == name) {
            x = p;
            replaced = true;
        }
    if (!replaced)
        l << p;
    try {
        save_pairs(l);
    } catch (const OSError &e) {
        warn(this, tx("Save Sync Pair"), e.message());
        return;
    }
    fill_saved();
    for (int i = 0; i < l.size(); ++i)
        if (l[i].name == name)
            saved->setCurrentIndex(i);
}

void SyncWindow::run_pair(int i)
{
    QList<Pair> l = pairs();
    if (i < 0 || i >= l.size())
        return;
    const Pair &p = l[i];
    source_edit->setText(resolve(p.source, p.source_uuid, p.source_rel));
    dest_edit->setText(resolve(p.dest, p.dest_uuid, p.dest_rel));
    deletes->setChecked(p.deletes);
    (p.rsync && use_rsync->isEnabled() ? use_rsync : use_kestrel)->setChecked(true);
    preview(true);
}

void SyncWindow::preview(bool then_run)
{
    if (task)
        return;
    QString src = rstrip(source_edit->text(), '/'), dst = rstrip(dest_edit->text(), '/');
    if (!isdir(src) || !isdir(dst)) {
        summary->setText(!isdir(src) ? tx("The source folder isn't there (a drive not connected?).")
                                     : tx("The destination folder isn't there (a drive not connected?)."));
        return;
    }
    if (src == dst || dst.startsWith(src + "/") || src.startsWith(dst + "/")) {
        summary->setText(tx("Choose two separate folders: one can't be inside the other."));
        return;
    }
    bool del = deletes->isChecked();
    preview_btn->setEnabled(false);
    sync_btn->setEnabled(false);
    bar->setRange(0, 0);
    bar->show();
    summary->setText(tx("Comparing…"));
    auto found = std::make_shared<QList<Item>>();
    QPointer<SyncWindow> self(this);
    QWidget *parent = win ? static_cast<QWidget *>(win) : this;
    task = fileops::queue_job(
        parent, tx("Comparing %1 and %2").arg(basename(src), basename(dst)), {src, dst},
        [src, dst, found](Task *t) {
            *found = diff(src, dst, false, t);
            return QVariant(true);
        },
        [self, found, src, dst, del, then_run](const QVariant &res) {
            if (!self)
                return;
            self->task = nullptr;
            self->preview_btn->setEnabled(true);
            self->bar->hide();
            if (!res.isValid()) {
                self->summary->setText(tx("Stopped."));
                return;
            }
            self->steps = plan(*found, del);
            self->planned_source = src;
            self->planned_dest = dst;
            self->planned_deletes = del;
            self->list->clear();
            int copy = 0, replace = 0, remove = 0, newer = 0;
            qint64 bytes = 0;
            for (const Step &s : std::as_const(self->steps)) {
                QString what = s.op == Step::COPY      ? tx("Copy")
                               : s.op == Step::REPLACE ? (s.dest_newer ? tx("Replace (the destination's is newer)") : tx("Replace"))
                                                       : tx("Delete from the destination");
                (s.op == Step::COPY ? copy : s.op == Step::REPLACE ? replace : remove) += 1;
                newer += s.dest_newer;
                if (s.op != Step::DELETE)
                    bytes += s.bytes;
                auto *row = new QTreeWidgetItem(self->list, {what, s.rel + (s.dir ? "/" : ""), s.dir ? QString() : human_size(s.bytes)});
                row->setToolTip(1, join(s.op == Step::DELETE ? dst : src, s.rel));
            }
            QString plan = tx("%1 to copy, %2 to replace").arg(group_digits(copy), group_digits(replace));
            if (del)
                plan = tx("%1, %2 to delete").arg(plan, group_digits(remove));
            if (newer)
                plan = tx("%1; %2 replaced where the destination's copy is newer").arg(plan, group_digits(newer));
            self->summary->setText(self->steps.isEmpty()
                                       ? tx("Nothing to do: “%1” already matches “%2”.").arg(basename(dst), basename(src))
                                       : tx("%1 (%2 of files, besides whole folders).").arg(plan, human_size(bytes)));
            self->sync_btn->setEnabled(!self->steps.isEmpty());
            Q_EMIT self->previewed();
            if (then_run && !self->steps.isEmpty())
                self->sync_now();
        },
        true, {}, false, {}, true);
}

void SyncWindow::sync_now()
{
    if (task || steps.isEmpty())
        return;
    if (planned_source != rstrip(source_edit->text(), '/') || planned_dest != rstrip(dest_edit->text(), '/') ||
        planned_deletes != deletes->isChecked()) {
        preview(true);
        return;
    }
    int remove = 0;
    for (const Step &s : std::as_const(steps))
        remove += s.op == Step::DELETE;
    QString src = planned_source, dst = planned_dest;
    QList<Step> st = steps;
    bool rs = use_rsync->isChecked() && have_rsync();
    if (!remove) {
        start(src, dst, st, false, rs);
        return;
    }
    QPointer<SyncWindow> self(this);
    ask_yes(this, tx("Delete on the Destination"),
            txn("The sync will also delete %Ln item(s) in “%1” that aren't in “%2”. They're deleted for good, not "
                "moved to the trash.\n\nDelete them and sync?",
                remove)
                .arg(dst, src),
            [self, src, dst, st, rs]() {
                if (self)
                    self->start(src, dst, st, true, rs);
            });
}

void SyncWindow::start(const QString &source, const QString &dest, const QList<Step> &st, bool del, bool rsync)
{
    if (!win)
        return;
    QPointer<SyncWindow> self(this);
    QString title = rsync ? tx("Syncing %1 to %2 (with rsync)").arg(basename(source), basename(dest))
                          : tx("Syncing %1 to %2").arg(basename(source), basename(dest));
    auto after = [self]() {
        if (!self)
            return;
        self->task = nullptr;
        self->bar->hide();
        self->preview();   // what's left to do: nothing, unless something failed
        Q_EMIT self->synced();
    };
    sync_btn->setEnabled(false);
    bar->setRange(0, 1000);
    bar->setValue(0);
    bar->show();
    if (rsync) {
        task = fileops::queue_job(
            win, title, {source, dest},
            [source, dest, del](Task *t) {
                run_rsync(t, source, dest, del);
                return QVariant(true);
            },
            [after](const QVariant &) { after(); }, true, {}, false, {}, true);
    } else {
        task = fileops::start_ops(win, jobs_for(source, dest, st), title, after);
    }
    if (task)
        connect(task, &Task::progress, this, [this](double f, const QString &) { bar->setValue(f < 0 ? 0 : int(f * 1000)); });
}
