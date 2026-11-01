#include "diskusage.h"

#include "app.h"
#include "drives.h"
#include "fileops.h"
#include "jobs.h"
#include "util.h"
#include "windows.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFileIconProvider>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStyle>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <functional>

using namespace util;

namespace diskusage {

QString Node::path() const
{
    if (rest)
        return parent ? parent->path() : name;
    QStringList parts;
    const Node *n = this;
    for (; n->parent; n = n->parent)
        parts.prepend(n->name);
    QString p = n->name;
    for (const QString &part : std::as_const(parts))
        p = join(p, part);
    return p;
}

// biggest first; the smaller files last of those of one size; then by name
static bool bigger(const std::unique_ptr<Node> &a, const std::unique_ptr<Node> &b)
{
    if (a->size != b->size)
        return a->size > b->size;
    if (a->rest != b->rest)
        return b->rest;
    return a->name < b->name;
}

namespace {

struct DirCloser {
    void operator()(DIR *d) const { ::closedir(d); }
};

struct Counter {
    Task *task;
    dev_t dev;
    qint64 expected;
    QStringList *unreadable;
    qint64 seen = 0, bytes = 0;

    void report(const QString &where)
    {
        if (!task || !task->due())
            return;
        QString text = txn("%1 in %Ln file(s) so far — %2", seen).arg(human_size(bytes), where);
        if (expected > 0)
            task->report(std::min<double>(bytes, expected * 0.99), double(expected), text);
        else
            task->report(0, 0, text);
    }

    // n's contents, from fd (an open folder: taken over)
    void folder(int fd, Node *n, const QString &path)
    {
        if (task)
            task->check();
        std::unique_ptr<DIR, DirCloser> d(::fdopendir(fd));
        if (!d) {
            ::close(fd);
            if (unreadable)
                *unreadable << path;
            return;
        }
        struct File {
            QString name;
            qint64 size;
            quint64 ino;
        };
        std::vector<File> files;
        QList<QByteArray> subdirs;
        int dfd = ::dirfd(d.get());
        while (struct dirent *e = ::readdir(d.get())) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            struct stat st;
            if (::fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
                continue;   // (gone meanwhile: Properties leaves it out too)
            if (++seen % 256 == 0) {
                if (task)
                    task->check();
                report(path);
            }
            if (S_ISDIR(st.st_mode)) {
                n->dirs += 1;
                if (st.st_dev != dev)
                    n->complete = false;   // another drive: not gone into (Properties would count it)
                else
                    subdirs << QByteArray(e->d_name);
            } else {
                n->files += 1;
                n->size += st.st_size;
                bytes += st.st_size;
                files.push_back(File{dec(e->d_name), qint64(st.st_size), quint64(st.st_ino)});
            }
        }
        for (const QByteArray &name : std::as_const(subdirs)) {
            auto child = std::make_unique<Node>();
            child->name = dec(name.constData());
            child->dir = true;
            child->parent = n;
            struct stat st;
            if (::fstatat(dfd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0)
                child->ino = st.st_ino;
            int cfd = ::openat(dfd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            QString cpath = join(path, child->name);
            if (cfd < 0) {
                if (unreadable)
                    *unreadable << cpath;
            } else {
                folder(cfd, child.get(), cpath);
            }
            n->size += child->size;
            n->files += child->files;
            n->dirs += child->dirs;
            n->complete = n->complete && child->complete;
            if (child->size < SMALL_FOLDER && !child->kids.empty()) {   // its totals are enough
                child->kids.clear();
                child->pruned = true;
            }
            n->kids.push_back(std::move(child));
        }
        // the biggest files one by one (those worth a block), the rest together
        std::sort(files.begin(), files.end(), [](const File &a, const File &b) {
            return a.size != b.size ? a.size > b.size : a.name < b.name;
        });
        qint64 least = std::max<qint64>(1, n->size / 500);
        qint64 rest_size = 0, rest_n = 0;
        for (size_t i = 0; i < files.size(); ++i) {
            if (int(i) < FILES_KEPT && files[i].size >= least) {
                auto f = std::make_unique<Node>();
                f->name = files[i].name;
                f->size = files[i].size;
                f->files = 1;
                f->ino = files[i].ino;
                f->parent = n;
                n->kids.push_back(std::move(f));
            } else {
                rest_size += files[i].size;
                rest_n += 1;
            }
        }
        if (rest_n) {
            auto r = std::make_unique<Node>();
            r->rest = true;
            r->size = rest_size;
            r->files = rest_n;
            r->parent = n;
            n->kids.push_back(std::move(r));
        }
        std::sort(n->kids.begin(), n->kids.end(), bigger);
    }
};

}  // namespace

Tree count(const QString &root, Task *task, qint64 expected, QStringList *unreadable)
{
    auto t = std::make_shared<Node>();
    t->name = root;
    t->dir = true;
    struct stat st;
    if (!lstat_(root, st) || !S_ISDIR(st.st_mode))
        throw OSError(ENOTDIR, errno_text(ENOTDIR, root));
    t->ino = st.st_ino;
    int fd = ::open(enc(root).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        throw_errno(root);
    Counter c{task, st.st_dev, expected, unreadable};
    c.folder(fd, t.get(), root);
    return t;
}

Tree remembered(const QString &root, qint64 *oldest, int depth)
{
    QString prefix = root == "/" ? "/" : root + "/";
    QHash<QString, QStringList> below;   // folder -> the remembered folders directly in it
    for (const QString &f : fileops::dirsizes::folders())
        if (f.startsWith(prefix) && f.size() > prefix.size())
            below[dirname(f)] << f;
    qint64 first = 0;
    std::function<std::unique_ptr<Node>(const QString &, int)> build = [&](const QString &path, int left) {
        auto e = fileops::dirsizes::get(path);
        auto n = std::make_unique<Node>();
        n->name = path == root ? root : basename(path);
        n->dir = n->remembered = true;
        if (left > 0)
            for (const QString &k : below.value(path))
                if (auto kid = build(k, left - 1)) {
                    kid->parent = n.get();
                    n->kids.push_back(std::move(kid));
                }
        qint64 in_kids = 0, kid_files = 0, kid_dirs = 0;
        for (const auto &k : n->kids) {
            in_kids += k->size;
            kid_files += k->files;
            kid_dirs += k->dirs + 1;
        }
        if (e) {
            n->size = e->stats.size;
            n->files = e->stats.files;
            n->dirs = e->stats.dirs;
            first = first ? std::min(first, e->measured) : e->measured;
        } else if (path == root && !n->kids.empty()) {   // only parts of it remembered
            n->size = in_kids;
            n->files = kid_files;
            n->dirs = kid_dirs;
        } else {
            return std::unique_ptr<Node>();
        }
        if (!n->kids.empty() && n->size > in_kids) {   // its files and the folders not remembered
            auto r = std::make_unique<Node>();
            r->rest = r->remembered = true;
            r->size = n->size - in_kids;
            r->files = std::max<qint64>(0, n->files - kid_files);
            r->parent = n.get();
            n->kids.push_back(std::move(r));
        }
        std::sort(n->kids.begin(), n->kids.end(), bigger);
        return n;
    };
    std::unique_ptr<Node> top = build(root, depth);
    if (!top)
        return nullptr;
    if (oldest)
        *oldest = first;
    return Tree(top.release());
}

QList<QPair<QString, const Node *>> worth_remembering(const Node &root, int max)
{
    QList<const Node *> first, others;
    if (root.complete)
        first << &root;
    for (const auto &k : root.kids)
        if (k->dir && k->complete && !k->remembered)
            first << k.get();
    std::function<void(const Node &)> walk = [&](const Node &n) {
        for (const auto &k : n.kids)
            if (k->dir) {
                if (k->complete && !k->remembered)
                    others << k.get();
                walk(*k);
            }
    };
    for (const auto &k : root.kids)
        if (k->dir)
            walk(*k);
    std::sort(others.begin(), others.end(), [](const Node *a, const Node *b) { return a->size > b->size; });
    QList<QPair<QString, const Node *>> out;
    for (const Node *n : first + others) {
        if (out.size() >= max)
            break;
        out << qMakePair(n->path(), n);
    }
    return out;
}

void remove(Node *n)
{
    Node *p = n ? n->parent : nullptr;
    if (!p)
        return;
    qint64 files = n->files, dirs = n->dirs + (n->dir ? 1 : 0);
    for (Node *a = p; a; a = a->parent) {
        a->size -= n->size;
        a->files -= files;
        a->dirs -= dirs;
    }
    auto it = std::find_if(p->kids.begin(), p->kids.end(), [n](const std::unique_ptr<Node> &k) { return k.get() == n; });
    if (it != p->kids.end())
        p->kids.erase(it);   // (n is gone now)
    for (Node *a = p; a->parent; a = a->parent)   // the folders above it may be smaller than their neighbours now
        std::sort(a->parent->kids.begin(), a->parent->kids.end(), bigger);
}

Node *find(Node *root, const QString &path)
{
    if (!root)
        return nullptr;
    if (path == root->name)
        return root;
    QString prefix = root->name == "/" ? "/" : root->name + "/";
    if (!path.startsWith(prefix))
        return nullptr;
    Node *n = root;
    for (const QString &part : path.mid(prefix.size()).split('/', Qt::SkipEmptyParts)) {
        Node *next = nullptr;
        for (const auto &k : n->kids)
            if (!k->rest && k->name == part) {
                next = k.get();
                break;
            }
        if (!next)
            return nullptr;
        n = next;
    }
    return n;
}

QList<QRectF> squarify(const QList<double> &sizes, const QRectF &area)
{
    QList<QRectF> out(sizes.size());
    double total = 0;
    for (double s : sizes)
        total += s;
    if (total <= 0 || area.width() <= 0 || area.height() <= 0)
        return out;
    double scale = area.width() * area.height() / total;
    QRectF r = area;
    int n = int(sizes.size()), i = 0;
    while (i < n) {
        double side = std::min(r.width(), r.height());
        if (side <= 0)
            break;
        // a row along the short side, grown while that keeps its blocks squarer
        int j = i;
        double sum = 0, worst = std::numeric_limits<double>::infinity();
        while (j < n) {
            double s = sum + sizes[j] * scale;
            double big = sizes[i] * scale, small = std::max(sizes[j] * scale, 1e-9);
            double w = std::max(side * side * big / (s * s), s * s / (side * side * small));
            if (j > i && w > worst)
                break;
            worst = w;
            sum = s;
            ++j;
        }
        double thick = sum / side;
        if (j == n)   // the last row fills what's left (no gap from rounding)
            thick = r.width() >= r.height() ? r.width() : r.height();
        double at = 0;
        for (int k = i; k < j; ++k) {
            double len = sum > 0 ? side * (sizes[k] * scale / sum) : 0;
            if (r.width() >= r.height())   // a column at the left
                out[k] = QRectF(r.left(), r.top() + at, thick, len);
            else   // a row at the top
                out[k] = QRectF(r.left() + at, r.top(), len, thick);
            at += len;
        }
        if (r.width() >= r.height())
            r.setLeft(r.left() + thick);
        else
            r.setTop(r.top() + thick);
        i = j;
    }
    return out;
}

bool count_asked_first(const QString &path) { return drives::slow_kind(path) != drives::Slow::None; }

QIcon icon()
{
    QIcon i = theme_icon({"office-chart-pie", "view-statistics", "drive-harddisk-symbolic", "drive-harddisk"});
    return i.isNull() ? QApplication::style()->standardIcon(QStyle::SP_DriveHDIcon) : i;
}

void open_window(MainWindow *win, const QString &folder)
{
    QString root = rstrip(folder, '/').isEmpty() ? "/" : rstrip(folder, '/');
    auto *w = new DiskUsageWindow(win, root);
    show_free(w);
    qint64 oldest = 0;
    if (Tree mem = remembered(root, &oldest)) {   // shown at once, while it counts
        w->tree = mem;
        w->go(mem.get());
        w->age->setText(tx("%1, as Properties remembers").arg(
            fileops::dirsizes::age_text(oldest, QDateTime::currentSecsSinceEpoch())));
    }
    drives::Slow slow = drives::slow_kind(root);
    if (slow == drives::Slow::None) {
        w->recount();
        return;
    }
    w->status->setText(slow == drives::Slow::Tape
                           ? tx("This folder is on a tape: counting reads every folder in it, which keeps the tape "
                                "busy for a long time. Count Now counts it anyway.")
                           : tx("This folder is on a floppy disk: counting reads every folder in it, which is slow. "
                                "Count Now counts it anyway."));
    w->recount_btn->setText(tx("Count Now"));
}

}  // namespace diskusage

using namespace diskusage;

// ---------------------------------------------------------------- the map

static QString node_name(const Node *n)
{
    if (!n->rest)
        return n->name;
    return n->remembered ? tx("Other files and folders (not remembered one by one)")
                         : txn("%Ln smaller file(s)", n->files);
}

static QString share(qint64 part, qint64 whole)
{
    double pct = whole > 0 ? part * 100.0 / whole : 0;
    return i18n::decimal(pct, pct < 10 ? 1 : 0);
}

UsageMap::UsageMap(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tx("Disk usage map"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void UsageMap::set_shown(Node *n)
{
    shown = n;
    hover = nullptr;
    relayout();
    update();
}

static int header_h(const QFontMetrics &fm) { return fm.height() + 4; }

void UsageMap::relayout()
{
    blocks.clear();
    if (shown && shown->size > 0)
        lay(shown, QRectF(rect()).adjusted(1, 1, -1, -1), 0, 0);
}

void UsageMap::lay(Node *n, const QRectF &area, int depth, int hue)
{
    QList<double> sizes;
    QList<Node *> kids;
    for (const auto &k : n->kids)
        if (k->size > 0) {
            sizes << double(k->size);
            kids << k.get();
        }
    QList<QRectF> rects = squarify(sizes, area);
    int head = header_h(fontMetrics());
    for (int i = 0; i < kids.size(); ++i) {
        int h = depth == 0 ? int(i * 137.508) % 360 : hue;   // each top block its own colour, kept below it
        blocks << Block{kids[i], rects[i], depth, h};
        const QRectF &r = rects[i];
        if (kids[i]->can_enter() && depth < 5 && r.width() > 48 && r.height() > head + 24)
            lay(kids[i], r.adjusted(3, head, -3, -3), depth + 1, h);
    }
}

void UsageMap::resizeEvent(QResizeEvent *ev)
{
    QWidget::resizeEvent(ev);
    relayout();
}

static QColor block_color(const Node *n, int depth, int hue)
{
    if (n->rest)
        return QColor(190, 190, 190);
    if (!n->dir)
        return QColor::fromHsv(hue, 45, 245);
    return QColor::fromHsv(hue, std::max(40, 130 - depth * 18), std::min(250, 205 + depth * 9));
}

void UsageMap::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Base));
    if (blocks.isEmpty()) {
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                   !shown ? tx("Nothing counted yet") : tx("Nothing here takes up space"));
        return;
    }
    p.fillRect(QRectF(rect()).adjusted(1, 1, -1, -1), QColor(170, 170, 170));   // (blocks too small to draw)
    QFontMetrics fm = fontMetrics();
    int head = header_h(fm);
    QSet<const Node *> with_inner;   // folders drawn with their contents inside: their name goes in a strip at the top
    for (const Block &b : std::as_const(blocks))
        if (b.node->parent && b.node->parent != shown)
            with_inner << b.node->parent;
    for (const Block &b : std::as_const(blocks)) {
        QRectF r = b.rect.adjusted(0.5, 0.5, -0.5, -0.5);
        if (r.width() < 1 || r.height() < 1)
            continue;
        QColor c = block_color(b.node, b.depth, b.hue);
        if (b.node == hover)
            c = c.lighter(110);
        p.setPen(QPen(c.darker(150), 1));
        p.setBrush(c);
        if (b.node->rest)
            p.setBrush(QBrush(c, Qt::Dense5Pattern));
        p.drawRect(r);
        QString label = node_name(b.node), size = human_size(b.node->size);
        p.setPen(QColor(32, 32, 32));
        QRectF text_r = r.adjusted(4, 2, -4, -2);
        if (with_inner.contains(b.node)) {   // the name (shortened first), then the size
            int room = int(text_r.width()) - fm.horizontalAdvance("  " + size);
            QString line = room >= fm.horizontalAdvance("M…M") ? fm.elidedText(label, Qt::ElideMiddle, room) + "  " + size
                                                               : fm.elidedText(label, Qt::ElideRight, int(text_r.width()));
            if (text_r.width() > 24)
                p.drawText(QRectF(text_r.left(), r.top(), text_r.width(), head), Qt::AlignLeft | Qt::AlignVCenter, line);
        } else if (text_r.width() > 30 && text_r.height() >= fm.height()) {
            QString name = fm.elidedText(label, Qt::ElideMiddle, int(text_r.width()));
            if (text_r.height() >= 2 * fm.height() + 2)
                p.drawText(text_r, Qt::AlignCenter, name + "\n" + fm.elidedText(size, Qt::ElideRight, int(text_r.width())));
            else
                p.drawText(text_r, Qt::AlignCenter, name);
        }
    }
    if (selected) {
        QRectF r = rect_of(selected);
        if (!r.isEmpty()) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(palette().color(QPalette::Highlight), 3));
            p.drawRect(r.adjusted(1.5, 1.5, -1.5, -1.5));
        }
    }
}

Node *UsageMap::node_at(const QPointF &pos) const
{
    for (int i = int(blocks.size()) - 1; i >= 0; --i)   // inner blocks come after the ones they're in
        if (blocks[i].rect.contains(pos))
            return blocks[i].node;
    return nullptr;
}

QRectF UsageMap::rect_of(const Node *n) const
{
    for (const Block &b : blocks)
        if (b.node == n)
            return b.rect;
    return QRectF();
}

QString UsageMap::describe(const Node *n) const
{
    if (!n)
        return QString();
    QStringList lines;
    lines << node_name(n);
    if (n->dir)
        lines << tx("%1 in %2").arg(human_size(n->size), txn("%Ln file(s)", n->files));
    else
        lines << human_size(n->size);
    const Node *in = n == shown ? n->parent : shown;
    if (in && in->size > 0)
        lines << tx("%1% of “%2”").arg(share(n->size, in->size), node_name(in));
    const Node *top = n;
    while (top->parent)
        top = top->parent;
    if (top != in && top != n && top->size > 0)
        lines << tx("%1% of everything counted").arg(share(n->size, top->size));
    if (n->pruned)
        lines << tx("Small: its contents aren't mapped one by one");
    else if (n->remembered && n->dir && !n->can_enter())
        lines << tx("Its contents aren't remembered: Recalculate to map them");
    return lines.join("\n");
}

bool UsageMap::event(QEvent *ev)
{
    if (ev->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(ev);
        Node *n = node_at(he->pos());
        if (n)
            QToolTip::showText(he->globalPos(), describe(n), this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(ev);
}

void UsageMap::mouseMoveEvent(QMouseEvent *ev)
{
    Node *n = node_at(ev->position());
    if (n != hover) {
        hover = n;
        update();
        Q_EMIT hovered(n);
    }
}

void UsageMap::leaveEvent(QEvent *ev)
{
    QWidget::leaveEvent(ev);
    if (hover) {
        hover = nullptr;
        update();
    }
    Q_EMIT hovered(nullptr);
}

void UsageMap::mousePressEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::BackButton) {
        Q_EMIT back();
        return;
    }
    QWidget::mousePressEvent(ev);
}

void UsageMap::mouseReleaseEvent(QMouseEvent *ev)
{
    if (ev->button() != Qt::LeftButton)
        return;
    Node *n = node_at(ev->position());
    if (!n)
        return;
    if (n->can_enter()) {
        Q_EMIT enter(n);
    } else {
        selected = n;
        update();
        Q_EMIT picked(n);
    }
}

void UsageMap::mouseDoubleClickEvent(QMouseEvent *ev)
{
    Node *n = ev->button() == Qt::LeftButton ? node_at(ev->position()) : nullptr;
    if (n && !n->can_enter() && !n->rest)
        Q_EMIT activated(n);
}

void UsageMap::contextMenuEvent(QContextMenuEvent *ev)
{
    if (Node *n = node_at(ev->pos())) {
        selected = n;
        update();
        Q_EMIT picked(n);
        Q_EMIT menu(n, ev->globalPos());
    }
}

// ---------------------------------------------------------------- the window

enum { NodeRole = Qt::UserRole + 50 };

static Node *node_of(QTreeWidgetItem *it)
{
    return it ? reinterpret_cast<Node *>(it->data(0, NodeRole).value<quintptr>()) : nullptr;
}

DiskUsageWindow::DiskUsageWindow(MainWindow *win, const QString &root_) : QDialog(win), win(win), root(root_)
{
    QString name = basename(root).isEmpty() ? root : basename(root);
    setWindowTitle(tx("Disk Usage — %1").arg(name));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1000, 650);
    auto *lay = new QVBoxLayout(this);

    auto *top = new QHBoxLayout;
    back_btn = new QPushButton(theme_icon({"go-up", "go-previous"}), tx("Back"));
    back_btn->setToolTip(tx("Up to the folder this one is in (Backspace)"));
    back_btn->setEnabled(false);
    connect(back_btn, &QPushButton::clicked, this, &DiskUsageWindow::up);
    top->addWidget(back_btn);
    where = new QLabel;
    where->setTextFormat(Qt::RichText);
    where->setWordWrap(true);
    where->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    connect(where, &QLabel::linkActivated, this, [this](const QString &link) {
        Node *n = map->shown;
        for (int up = link.toInt(); n && up > 0; --up)
            n = n->parent;
        if (n)
            go(n);
    });
    top->addWidget(where, 1);
    recount_btn = new QPushButton(theme_icon(QString("view-refresh")), tx("Recalculate"));
    recount_btn->setToolTip(tx("Count everything in it again"));
    connect(recount_btn, &QPushButton::clicked, this, &DiskUsageWindow::recount);
    pause_btn = new QPushButton(tx("Pause"));
    pause_btn->setEnabled(false);
    connect(pause_btn, &QPushButton::clicked, this, [this]() {
        if (!task)
            return;
        bool paused = false;
        for (const jobs::Job &j : jobs::Queue::instance()->jobs())
            if (j.local == task)
                paused = j.state == "paused";
        jobs::Queue::instance()->control(QString(), task->id, paused ? "resume" : "pause");
        sync_pause();
    });
    connect(jobs::Queue::instance(), &jobs::Queue::changed, this, &DiskUsageWindow::sync_pause);
    stop_btn = new QPushButton(tx("Stop"));
    stop_btn->setEnabled(false);
    connect(stop_btn, &QPushButton::clicked, this, [this]() {
        if (task)
            task->cancel();
    });
    for (QPushButton *b : {recount_btn, pause_btn, stop_btn})
        top->addWidget(b);
    lay->addLayout(top);

    bar = new QProgressBar;
    bar->setRange(0, 1000);
    bar->setAccessibleName(tx("Progress"));
    bar->hide();
    lay->addWidget(bar);
    status = new QLabel;
    status->setWordWrap(true);
    lay->addWidget(status);
    age = new QLabel;
    age->setForegroundRole(QPalette::PlaceholderText);
    lay->addWidget(age);

    auto *split = new QSplitter;
    map = new UsageMap;
    split->addWidget(map);
    list = new QTreeWidget;
    list->setAccessibleName(tx("What's in the folder shown"));
    list->setRootIsDecorated(false);
    list->setHeaderLabels({tx("Name"), tx("Size"), tx("Share")});
    list->header()->setStretchLastSection(false);
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    split->addWidget(list);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setSizes({700, 300});
    list->setMinimumWidth(220);
    lay->addWidget(split, 1);
    info = new QLabel;
    info->setTextFormat(Qt::PlainText);
    info->setWordWrap(true);
    info->setMinimumHeight(info->fontMetrics().height() * 3);
    info->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    lay->addWidget(info);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
    lay->addWidget(bb);

    connect(map, &UsageMap::enter, this, &DiskUsageWindow::go);
    connect(map, &UsageMap::back, this, &DiskUsageWindow::up);
    connect(map, &UsageMap::hovered, this, [this](Node *n) {
        info->setText(map->describe(n ? n : map->selected ? map->selected : map->shown));
    });
    connect(map, &UsageMap::picked, this, [this](Node *n) {
        info->setText(map->describe(n));
        for (int i = 0; i < list->topLevelItemCount(); ++i)
            if (node_of(list->topLevelItem(i)) == n)
                list->setCurrentItem(list->topLevelItem(i));
    });
    connect(map, &UsageMap::activated, this, [this](Node *n) {
        if (this->win)
            this->win->open_file(n->path());
    });
    connect(map, &UsageMap::menu, this, [this](Node *n, const QPoint &at) {
        if (QMenu *m = item_menu(n))
            m->popup(at);
    });
    connect(list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
        if (Node *n = node_of(it)) {
            map->selected = n;
            map->update();
            info->setText(map->describe(n));
        }
    });
    connect(list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        Node *n = node_of(it);
        if (!n || n->rest)
            return;
        if (n->can_enter())
            go(n);
        else if (this->win)
            this->win->open_file(n->path());
    });
    connect(list, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (QMenu *m = item_menu(node_of(list->itemAt(pos))))
            m->popup(list->viewport()->mapToGlobal(pos));
    });
    for (const QKeySequence &k : {QKeySequence(Qt::Key_Backspace), QKeySequence(Qt::ALT | Qt::Key_Left)})
        connect(new QShortcut(k, this), &QShortcut::activated, this, &DiskUsageWindow::up);
    refresh();
}

DiskUsageWindow::~DiskUsageWindow()
{
    if (task)
        task->cancel();
}

void DiskUsageWindow::closeEvent(QCloseEvent *ev)
{
    if (task)
        task->cancel();
    QDialog::closeEvent(ev);
}

void DiskUsageWindow::sync_pause()
{
    bool paused = false;
    if (task)
        for (const jobs::Job &j : jobs::Queue::instance()->jobs())
            if (j.local == task)
                paused = j.state == "paused";
    pause_btn->setText(paused ? tx("Resume") : tx("Pause"));
    pause_btn->setEnabled(task != nullptr);
}

void DiskUsageWindow::recount()
{
    if (task)
        return;
    // how much there is to count, for a percentage: what Properties remembers, or a whole drive's used space
    qint64 expected = 0;
    if (auto e = fileops::dirsizes::get(root)) {
        expected = e->stats.size;
    } else {
        struct stat st, pst;
        struct statvfs vfs;
        if (lstat_(root, st) && (root == "/" || (lstat_(dirname(root), pst) && pst.st_dev != st.st_dev)) &&
            ::statvfs(enc(root).constData(), &vfs) == 0)
            expected = qint64(vfs.f_blocks - vfs.f_bfree) * qint64(vfs.f_frsize);
    }
    recount_btn->setEnabled(false);
    recount_btn->setText(tx("Recalculate"));
    stop_btn->setEnabled(true);
    bar->setValue(0);
    bar->setRange(0, expected > 0 ? 1000 : 0);
    bar->show();
    status->setText(tx("Waiting for the drive…"));
    auto counted = std::make_shared<Tree>();
    auto keep = std::make_shared<QList<fileops::dirsizes::Counted>>();
    auto unreadable = std::make_shared<QStringList>();
    QString r = root;
    QPointer<DiskUsageWindow> self(this);
    QWidget *parent = win ? static_cast<QWidget *>(win) : this;
    QString name = basename(r).isEmpty() ? r : basename(r);
    task = fileops::queue_job(
        parent, tx("Counting what fills “%1”").arg(name), {r},
        [r, expected, counted, keep, unreadable](Task *t) {
            t->log(tx("Counting %1 (not other drives mounted in it)").arg(r));
            Tree tr = count(r, t, expected, unreadable.get());
            t->log(tx("%1 in %2 and %3")
                       .arg(human_size(tr->size), txn("%Ln file(s)", tr->files), txn("%Ln folder(s)", tr->dirs)));
            for (const QString &u : std::as_const(*unreadable))
                t->log(tx("Couldn't be read, counted as empty: %1").arg(u));
            int shown = 0;
            for (const auto &k : tr->kids)
                if (!k->rest && shown++ < 10)
                    t->log(tx("  %1: %2").arg(k->name, human_size(k->size)));
            struct stat st;
            quint64 dev = lstat_(r, st) ? quint64(st.st_dev) : 0;
            for (const auto &[path, n] : worth_remembering(*tr))
                *keep << fileops::dirsizes::Counted{path, dev, n->ino, fileops::DirStats{n->size, n->files, n->dirs}};
            t->log(txn("%Ln folder size(s) remembered for Properties", keep->size()));
            *counted = tr;
            return QVariant(true);
        },
        [self, counted, keep, unreadable](const QVariant &res) {
            if (!self)
                return;
            self->task = nullptr;
            self->recount_btn->setEnabled(true);
            self->stop_btn->setEnabled(false);
            self->sync_pause();
            self->bar->hide();
            if (res.isValid() && *counted) {
                fileops::dirsizes::put_many(*keep);   // for Properties (and the next Disk Usage)
                self->show_tree(*counted);
                self->age->setText(fileops::dirsizes::age_text(QDateTime::currentSecsSinceEpoch(),
                                                               QDateTime::currentSecsSinceEpoch()));
                self->status->setText(unreadable->isEmpty()
                                          ? QString()
                                          : txn("%Ln folder(s) couldn't be read and count as empty, such as %1",
                                                unreadable->size())
                                                .arg(unreadable->first()));
            } else {
                self->status->setText(self->tree ? tx("Stopped: the sizes shown are from before.") : tx("Stopped."));
            }
            Q_EMIT self->finished_count();
        },
        true, {}, false, {}, true);
    connect(task, &Task::progress, this, [this](double f, const QString &text) {
        bar->setRange(0, f < 0 ? 0 : 1000);
        bar->setValue(f < 0 ? 0 : int(f * 1000));
        status->setText(text);
    });
    sync_pause();
}

void DiskUsageWindow::show_tree(const Tree &t)
{
    // the folder shown stays shown, if it's in the new tree
    QString at = map->shown ? map->shown->path() : root;
    tree = t;
    map->selected = nullptr;
    Node *n = diskusage::find(tree.get(), at);
    while (n && !n->can_enter() && n->parent)
        n = n->parent;
    go(n ? n : tree.get());
}

void DiskUsageWindow::go(Node *n)
{
    if (!n)
        return;
    map->selected = nullptr;
    map->set_shown(n);
    refresh();
}

void DiskUsageWindow::up()
{
    if (map->shown && map->shown->parent)
        go(map->shown->parent);
}

void DiskUsageWindow::refresh()
{
    Node *shown = map->shown;
    back_btn->setEnabled(shown && shown->parent);
    // the path: each folder above a link up to it
    QStringList parts;
    int up = 0;
    for (Node *n = shown; n; n = n->parent, ++up) {
        QString text = n->name.toHtmlEscaped();
        parts.prepend(up == 0 ? "<b>" + text + "</b>" : "<a href=\"" + QString::number(up) + "\">" + text + "</a>");
    }
    where->setText(parts.isEmpty() ? root.toHtmlEscaped() : parts.join(" › "));
    list->clear();
    if (shown) {
        QFileIconProvider icons;
        QIcon folder = icons.icon(QFileIconProvider::Folder), file = icons.icon(QFileIconProvider::File);
        for (const auto &k : shown->kids) {
            auto *it = new QTreeWidgetItem(list);
            it->setText(0, node_name(k.get()));
            it->setIcon(0, k->dir ? folder : file);
            it->setText(1, human_size(k->size));
            it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            it->setText(2, tx("%1%").arg(share(k->size, shown->size)));
            it->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            it->setData(0, NodeRole, QVariant::fromValue(quintptr(k.get())));
            it->setToolTip(0, map->describe(k.get()));
        }
    }
    info->setText(map->describe(shown));
}

QMenu *DiskUsageWindow::item_menu(Node *n)
{
    if (!n || n->rest)
        return nullptr;
    auto *m = new QMenu(this);
    connect(m, &QMenu::aboutToHide, m, &QObject::deleteLater);
    QString path = n->path();
    QPointer<DiskUsageWindow> self(this);
    if (n->can_enter())
        m->addAction(tx("Go Into"), this, [this, path]() {
            if (Node *x = diskusage::find(tree.get(), path))
                go(x);
        });
    QAction *open = m->addAction(theme_icon(QString("document-open")), tx("Open"), this, [self, path]() {
        if (!self || !self->win)
            return;
        if (isdir(path))
            self->win->navigate(path);
        else
            self->win->open_file(path);
        self->win->raise();
        self->win->activateWindow();
    });
    QAction *show = m->addAction(theme_icon({"folder-open", "system-file-manager"}), tx("Show in Folder"), this, [self, path]() {
        if (!self || !self->win)
            return;
        self->win->reveal(path);
        self->win->raise();
        self->win->activateWindow();
    });
    m->addSeparator();
    QAction *trash_act = m->addAction(theme_icon(QString("user-trash")), tx("Move to Trash"), this, [this, path]() {
        if (Node *x = diskusage::find(tree.get(), path))
            trash(x);
    });
    QAction *props = m->addAction(theme_icon(QString("document-properties")), tx("Properties"), this, [self, path]() {
        if (self && self->win)
            self->win->properties({path});
    });
    bool can = win != nullptr && n->parent;
    for (QAction *a : {open, show, trash_act, props})
        a->setEnabled(can);
    return m;
}

void DiskUsageWindow::trash(Node *n)
{
    if (!win || !n || n->rest || !n->parent)
        return;
    QString path = n->path();
    QPointer<DiskUsageWindow> self(this);
    win->trash_paths({path}, [self, path](const QStringList &trashed) {
        if (!self || !trashed.contains(path))
            return;
        Node *x = diskusage::find(self->tree.get(), path);
        if (!x || !x->parent)
            return;
        Node *shown = self->map->shown;
        for (Node *a = shown; a; a = a->parent)
            if (a == x) {   // it was shown, or a folder in it: the folder it was in is shown instead
                shown = x->parent;
                break;
            }
        self->map->selected = nullptr;
        remove(x);
        self->map->set_shown(shown);
        self->refresh();
        self->status->setText(tx("Moved “%1” to the trash. Undo in a Kestrel window brings it back (Recalculate "
                                 "then counts it again).").arg(basename(path)));
    });
}
