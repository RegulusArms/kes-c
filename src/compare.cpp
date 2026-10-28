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
        throw OSError(EACCES, "Can't read " + a);
    if (!fb.open(QIODevice::ReadOnly))
        throw OSError(EACCES, "Can't read " + b);
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
                task->report(0, 0, "Comparing " + sub);
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
        return "The same";
    case ONLY_LEFT:
        return "Only in " + left_name;
    case ONLY_RIGHT:
        return "Only in " + right_name;
    case LEFT_NEWER:
        return "Newer in " + left_name;
    case RIGHT_NEWER:
        return "Newer in " + right_name;
    case DIFFERENT:
        return "Different contents";
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
                task->report(last, 100, QString("rsync: %1%").arg(last));
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
        throw Error(QString("rsync stopped with an error (%1): %2")
                        .arg(QString::number(rc), lines.isEmpty() ? QString("no message") : lines.mid(lines.size() - 3).join(" ")));
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
    choose_folder(win, "Compare “" + basename(left) + "” With", dirname(left), [w, left](const QString &right) {
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
    return (dir ? QString("Folder") : human_size(size)) + " · " + fmt_time(mtime, "%Y-%m-%d %H:%M");
}

// ---------------------------------------------------------------- Compare With…

CompareWindow::CompareWindow(MainWindow *win, const QString &left, const QString &right) : QDialog(win), win(win)
{
    setWindowTitle("Compare Folders");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(960, 600);
    auto *lay = new QVBoxLayout(this);
    auto *grid = new QGridLayout;
    left_edit = new QLineEdit(left);
    left_edit->setAccessibleName("Left folder");
    right_edit = new QLineEdit(right);
    right_edit->setAccessibleName("Right folder");
    int row = 0;
    for (QLineEdit *e : {left_edit, right_edit}) {
        grid->addWidget(new QLabel(e == left_edit ? "Left:" : "Right:"), row, 0);
        grid->addWidget(e, row, 1);
        auto *choose = new QPushButton("Choose…");
        connect(choose, &QPushButton::clicked, this, [this, e]() {
            QPointer<QLineEdit> edit(e);
            choose_folder(this, "Compare", e->text(), [edit](const QString &d) {
                if (edit)
                    edit->setText(d);
            });
        });
        grid->addWidget(choose, row, 2);
        ++row;
    }
    auto *swap = new QPushButton("⇅ Swap");
    connect(swap, &QPushButton::clicked, this, [this]() {
        QString l = left_edit->text();
        left_edit->setText(right_edit->text());
        right_edit->setText(l);
        run();
    });
    grid->addWidget(swap, 0, 3, 2, 1);
    lay->addLayout(grid);

    auto *opts = new QHBoxLayout;
    deep = new QCheckBox("Compare contents (slower: reads the files)");
    deep->setToolTip("Without it, files of the same size and date count as the same");
    opts->addWidget(deep);
    opts->addStretch(1);
    compare_btn = new QPushButton("Compare");
    stop_btn = new QPushButton("Stop");
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
    show->addWidget(new QLabel("Show:"));
    const QList<QPair<QString, bool>> kinds = {{"The same", false},   {"Only on the left", true}, {"Only on the right", true},
                                               {"Newer on the left", true}, {"Newer on the right", true},
                                               {"Different contents", true}};
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
    tree->setAccessibleName("Differences");
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setHeaderLabels({"Item", "Difference", "Left", "Right"});
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
    add("Open Left", "Open the left folder's copy", [this]() { open_side(false); });
    add("Open Right", "Open the right folder's copy", [this]() { open_side(true); });
    add("Copy to Right →", "Copy the left's items to the right (replacing what's there, asking first)",
        [this]() { copy_across(true); });
    add("← Copy to Left", "Copy the right's items to the left (replacing what's there, asking first)",
        [this]() { copy_across(false); });
    add("Trash on Left", "Move the left's items to the trash", [this]() { trash_side(false); });
    add("Trash on Right", "Move the right's items to the trash", [this]() { trash_side(true); });
    acts->addStretch(1);
    auto *close = new QPushButton("Close");
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
        summary->setText("Choose two folders to compare.");
        return;
    }
    items.clear();
    fill();
    compare_btn->setEnabled(false);
    stop_btn->setEnabled(true);
    bar->show();
    summary->setText("Comparing…");
    auto found = std::make_shared<QList<Item>>();
    bool d = deep->isChecked();
    QPointer<CompareWindow> self(this);
    QWidget *parent = win ? static_cast<QWidget *>(win) : this;
    task = fileops::queue_job(
        parent, "Comparing " + basename(l) + " and " + basename(r), {l, r},
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
                self->summary->setText("Stopped.");
            Q_EMIT self->compared();
        },
        true, {}, false, {}, true);
}

void CompareWindow::fill()
{
    QString ln = basename(left_edit->text()), rn = basename(right_edit->text());
    if (ln == rn) {
        ln = "the left";
        rn = "the right";
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
    summary->setText(items.isEmpty() ? QString("Both folders are empty.")
                     : differ == 0   ? QString("The folders are the same (%1 items).").arg(items.size())
                                     : QString("%1 difference(s): %2 only on the left, %3 only on the right, %4 newer on the "
                                               "left, %5 newer on the right, %6 with other contents. %7 the same.")
                                           .arg(QString::number(differ), QString::number(counts.value(ONLY_LEFT)),
                                                QString::number(counts.value(ONLY_RIGHT)), QString::number(counts.value(LEFT_NEWER)),
                                                QString::number(counts.value(RIGHT_NEWER)), QString::number(counts.value(DIFFERENT)),
                                                QString::number(counts.value(SAME))));
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
        fileops::start_ops(w, jobs, "Copying to " + basename(to), [self]() {
            if (self)
                self->run();
        }, "Copy");
    };
    if (replacing)
        ask_yes(this, "Copy Across",
                QString("Replace %1 item(s) in “%2” with the copies from “%3”? What's replaced can't be brought back.")
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
    setWindowTitle("Sync Folders");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(820, 600);
    auto *lay = new QVBoxLayout(this);

    auto *saved_row = new QHBoxLayout;
    saved_row->addWidget(new QLabel("Saved:"));
    saved = new QComboBox;
    saved->setAccessibleName("Saved sync pairs");
    saved->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    saved_row->addWidget(saved, 1);
    auto *run = new QPushButton("Run");
    run->setToolTip("Preview the saved pair and sync it (asking first when it deletes)");
    connect(run, &QPushButton::clicked, this, [this]() { run_pair(saved->currentIndex()); });
    auto *remove = new QPushButton("Remove");
    connect(remove, &QPushButton::clicked, this, [this]() {
        QList<Pair> l = pairs();
        int i = saved->currentIndex();
        if (i < 0 || i >= l.size())
            return;
        l.removeAt(i);
        try {
            save_pairs(l);
        } catch (const OSError &e) {
            warn(this, "Sync Folders", e.message());
        }
        fill_saved();
    });
    saved_row->addWidget(run);
    saved_row->addWidget(remove);
    lay->addLayout(saved_row);

    auto *grid = new QGridLayout;
    source_edit = new QLineEdit(source);
    source_edit->setAccessibleName("Source folder");
    dest_edit = new QLineEdit(dest);
    dest_edit->setAccessibleName("Destination folder");
    int row = 0;
    for (QLineEdit *e : {source_edit, dest_edit}) {
        grid->addWidget(new QLabel(e == source_edit ? "From:" : "To:"), row, 0);
        grid->addWidget(e, row, 1);
        auto *choose = new QPushButton("Choose…");
        connect(choose, &QPushButton::clicked, this, [this, e]() {
            QPointer<QLineEdit> edit(e);
            choose_folder(this, e == source_edit ? "Sync From" : "Sync To", e->text().isEmpty() ? HOME() : e->text(),
                          [edit](const QString &d) {
                              if (edit)
                                  edit->setText(d);
                          });
        });
        grid->addWidget(choose, row, 2);
        connect(e, &QLineEdit::textChanged, this, [this]() {   // what the preview showed is for other folders
            steps.clear();
            list->clear();
            summary->setText("Preview to see what will be done.");
            sync_btn->setEnabled(false);
        });
        ++row;
    }
    lay->addLayout(grid);
    deletes = new QCheckBox("Also delete what's on the destination but not in the source (asks again)");
    deletes->setToolTip("Off: the destination keeps files you've deleted from the source");
    lay->addWidget(deletes);
    auto *how = new QHBoxLayout;
    how->addWidget(new QLabel("Copy with:"));
    use_rsync = new QRadioButton("rsync (resumes interrupted copies, only sends what changed)");
    use_kestrel = new QRadioButton("Kestrel's own copy");
    auto *group = new QButtonGroup(this);
    group->addButton(use_rsync);
    group->addButton(use_kestrel);
    bool rs = have_rsync();
    use_rsync->setEnabled(rs);
    if (!rs)
        use_rsync->setToolTip("rsync isn't installed");
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
    preview_btn = new QPushButton("Preview");
    connect(preview_btn, &QPushButton::clicked, this, [this]() { preview(); });
    auto *save = new QPushButton("Save Pair…");
    save->setToolTip("Keep these folders and choices, to sync them again in one click");
    connect(save, &QPushButton::clicked, this, [this]() {
        QPointer<SyncWindow> self(this);
        ask_text(this, "Save Sync Pair", "Name:",
                 basename(source_edit->text()) + " → " + basename(dest_edit->text()), [self](const QString &name) {
                     if (self && !name.trimmed().isEmpty())
                         self->save_pair(name.trimmed());
                 });
    });
    pv->addWidget(preview_btn);
    pv->addWidget(save);
    pv->addStretch(1);
    lay->addLayout(pv);
    summary = new QLabel("Preview to see what will be done.");
    summary->setWordWrap(true);
    lay->addWidget(summary);
    list = new QTreeWidget;
    list->setAccessibleName("What the sync will do");
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setHeaderLabels({"Will", "Item", "Size"});
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
    sync_btn = bb->addButton("Sync Now", QDialogButtonBox::AcceptRole);
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
        saved->addItem(p.name + "  (" + p.source + " → " + p.dest + (p.deletes ? ", deleting" : "") + ")");
    if (!saved->count())
        saved->addItem("None yet: choose the folders, then Save Pair…");
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
        warn(this, "Save Sync Pair", e.message());
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
        summary->setText(!isdir(src) ? "The source folder isn't there (a drive not connected?)."
                                     : "The destination folder isn't there (a drive not connected?).");
        return;
    }
    if (src == dst || dst.startsWith(src + "/") || src.startsWith(dst + "/")) {
        summary->setText("Choose two separate folders: one can't be inside the other.");
        return;
    }
    bool del = deletes->isChecked();
    preview_btn->setEnabled(false);
    sync_btn->setEnabled(false);
    bar->setRange(0, 0);
    bar->show();
    summary->setText("Comparing…");
    auto found = std::make_shared<QList<Item>>();
    QPointer<SyncWindow> self(this);
    QWidget *parent = win ? static_cast<QWidget *>(win) : this;
    task = fileops::queue_job(
        parent, "Comparing " + basename(src) + " and " + basename(dst), {src, dst},
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
                self->summary->setText("Stopped.");
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
                QString what = s.op == Step::COPY      ? QString("Copy")
                               : s.op == Step::REPLACE ? QString(s.dest_newer ? "Replace (the destination's is newer)" : "Replace")
                                                       : QString("Delete from the destination");
                (s.op == Step::COPY ? copy : s.op == Step::REPLACE ? replace : remove) += 1;
                newer += s.dest_newer;
                if (s.op != Step::DELETE)
                    bytes += s.bytes;
                auto *row = new QTreeWidgetItem(self->list, {what, s.rel + (s.dir ? "/" : ""), s.dir ? QString() : human_size(s.bytes)});
                row->setToolTip(1, join(s.op == Step::DELETE ? dst : src, s.rel));
            }
            self->summary->setText(
                self->steps.isEmpty()
                    ? QString("Nothing to do: “%1” already matches “%2”.").arg(basename(dst), basename(src))
                    : QString("%1 to copy, %2 to replace%3%4 (%5 of files, besides whole folders).")
                          .arg(QString::number(copy), QString::number(replace),
                               del ? QString(", %1 to delete").arg(remove) : QString(),
                               newer ? QString("; %1 replaced where the destination's copy is newer").arg(newer) : QString(),
                               human_size(bytes)));
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
    ask_yes(this, "Delete on the Destination",
            QString("The sync will also delete %1 item(s) in “%2” that aren't in “%3”. They're deleted for good, not "
                    "moved to the trash.\n\nDelete them and sync?")
                .arg(QString::number(remove), dst, src),
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
    QString title = "Syncing " + basename(source) + " to " + basename(dest) + (rsync ? " (with rsync)" : "");
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
