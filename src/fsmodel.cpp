// The file views' folder model (fsmodel.h): listing by bytes on a thread, inotify, sorting, filters, drag and drop.
#include "fsmodel.h"

#include "archive.h"
#include "i18n.h"
#include "thumbs.h"
#include "util.h"
#include "widgets.h"

#include <QCollator>
#include <QMimeData>
#include <QMutex>
#include <QSocketNotifier>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace util;
using fsmodel::Entry;

namespace fsmodel {

QString type_of(const QString &name, bool dir)
{
    if (dir)
        return tx("Folder");
    int dot = name.lastIndexOf('.');
    QString suf = dot >= 0 ? name.mid(dot + 1).toLower() : QString();
    static QMutex mutex;
    static QHash<QString, QString> types;   // by extension
    QMutexLocker lock(&mutex);
    auto it = types.constFind(suf);
    if (it != types.constEnd())
        return *it;
    QString t = mime_for(name, 0).comment();
    if (t.isEmpty())
        t = tx("File");
    types.insert(suf, t);
    return t;
}

// The lister's threads post their results to the model through this: the model clears it as it's destroyed, so a
// thread that finishes later posts nothing (and a result posted before is dropped with the model's events).
struct Link {
    std::mutex m;
    FSModel *model = nullptr;
    std::atomic<int> gen{0};   // the folder it shows now: a listing for another one stops
};

template <typename F> static void post(const std::shared_ptr<Link> &link, F f)
{
    std::lock_guard<std::mutex> g(link->m);
    if (FSModel *m = link->model)
        QMetaObject::invokeMethod(m, [m, f]() { f(m); }, Qt::QueuedConnection);
}

static QCollator collator()
{
    QCollator c;   // (the default locale's, as QFileSystemModel's)
    c.setNumericMode(true);
    c.setCaseSensitivity(Qt::CaseInsensitive);
    return c;
}

// One item, looked at in its folder (dirfd); nullopt when it isn't there (or can't be looked at)
static std::optional<Entry> look_at(int dirfd, const QByteArray &raw, QCollator &coll, int *err = nullptr)
{
    struct stat st;
    if (::fstatat(dirfd, raw.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (err)
            *err = errno;
        return std::nullopt;
    }
    Entry e;
    e.raw = raw;
    e.name = dec(raw.constData());
    e.link = S_ISLNK(st.st_mode);
    struct stat target;
    if (e.link && ::fstatat(dirfd, raw.constData(), &target, 0) == 0)
        st = target;
    else if (e.link)
        e.special = true;   // (pointing at nothing)
    e.dir = S_ISDIR(st.st_mode);
    e.special = e.special || (!e.dir && !S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode));
    e.size = st.st_size;
    e.mtime = st.st_mtime;
    e.type = type_of(e.name, e.dir);
    e.key = coll.sortKey(e.name);
    e.type_key = coll.sortKey(e.type);
    return e;
}

// The folder's items, in batches (small first, so the view shows something at once; then bigger, so it isn't sorted
// again for every few hundred)
static void list_folder(std::shared_ptr<Link> link, int gen, QByteArray dir)
{
    QCollator coll = collator();
    QList<Entry> batch;
    int want = 256;
    int fd = ::open(dir.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (DIR *d = fd >= 0 ? ::fdopendir(fd) : nullptr) {
        while (struct dirent *de = ::readdir(d)) {
            if (!std::strcmp(de->d_name, ".") || !std::strcmp(de->d_name, ".."))
                continue;
            if (std::optional<Entry> e = look_at(::dirfd(d), QByteArray(de->d_name), coll))
                batch << std::move(*e);
            if (batch.size() >= want) {
                post(link, [gen, batch](FSModel *m) { m->take_listed(gen, batch, false); });
                batch.clear();
                want = std::min(want * 2, 16384);
                std::lock_guard<std::mutex> g(link->m);
                if (!link->model || link->gen != gen)   // (the model has gone, or shows another folder now)
                    break;
            }
        }
        ::closedir(d);
    } else if (fd >= 0) {
        ::close(fd);
    }
    post(link, [gen, batch](FSModel *m) { m->take_listed(gen, batch, true); });
}

// Names inotify said changed: each one as it is now, or gone (only when it's really not there: a drive that answers
// with an error for a moment keeps its items)
static void check_names(std::shared_ptr<Link> link, int gen, QByteArray dir, QList<QByteArray> names)
{
    QCollator coll = collator();
    QList<QByteArray> gone;
    QList<Entry> found;
    int fd = ::open(dir.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (const QByteArray &n : names) {
        int err = 0;
        std::optional<Entry> e = fd >= 0 ? look_at(fd, n, coll, &err) : std::nullopt;
        if (e)
            found << std::move(*e);
        else if (fd < 0 || err == ENOENT || err == ENOTDIR)
            gone << n;
    }
    if (fd >= 0)
        ::close(fd);
    post(link, [gen, gone, found](FSModel *m) { m->take_checked(gen, gone, found); });
}

// One inotify instance for every model (each one's would count against the user's limit of instances), its watches
// shared by the models showing the same folder
class Hub : public QObject {
public:
    static Hub *instance()
    {
        static Hub *hub = new Hub;
        return hub;
    }

    int add(FSModel *m, const QByteArray &dir)
    {
        if (fd < 0)
            return -1;
        int wd = ::inotify_add_watch(fd, dir.constData(),
                                     IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_ATTRIB |
                                         IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR);
        if (wd >= 0)
            watchers[wd] << m;
        return wd;
    }

    void remove(FSModel *m, int wd)
    {
        auto it = watchers.find(wd);
        if (it == watchers.end())
            return;
        it->removeAll(m);
        if (it->isEmpty()) {
            watchers.erase(it);
            ::inotify_rm_watch(fd, wd);
        }
    }

private:
    Hub()
    {
        fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd >= 0) {
            auto *n = new QSocketNotifier(fd, QSocketNotifier::Read, this);
            connect(n, &QSocketNotifier::activated, this, &Hub::read_events);
        }
    }

    void read_events()
    {
        struct Got {
            QList<QByteArray> names;
            bool overflow = false, gone = false;
        };
        QHash<int, Got> got;
        alignas(struct inotify_event) char buf[64 * 1024];
        for (;;) {
            ssize_t n = ::read(fd, buf, sizeof buf);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            for (char *p = buf; p < buf + n;) {
                auto *ev = reinterpret_cast<struct inotify_event *>(p);
                p += sizeof(struct inotify_event) + ev->len;
                if (ev->mask & IN_Q_OVERFLOW) {   // (events were lost: every folder is listed again)
                    for (int wd : watchers.keys())
                        got[wd].overflow = true;
                    continue;
                }
                if (!watchers.contains(ev->wd))
                    continue;
                if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF))
                    got[ev->wd].gone = true;
                else if (ev->len)
                    got[ev->wd].names << QByteArray(ev->name, int(::strnlen(ev->name, ev->len)));
            }
        }
        for (auto it = got.begin(); it != got.end(); ++it)
            for (FSModel *m : watchers.value(it.key()))   // (a copy: a model may change the watches)
                m->take_events(it->names, it->overflow, it->gone);
    }

    int fd = -1;
    QHash<int, QList<FSModel *>> watchers;
};

}  // namespace fsmodel

using fsmodel::Hub;

// A name with its stray bytes (dec()'s U+DC80…U+DCFF) shown as U+FFFD
static QString shown_name(const QString &name)
{
    QString out = name;
    for (QChar &c : out)
        if (c.unicode() >= 0xDC80 && c.unicode() <= 0xDCFF)
            c = QChar(0xFFFD);
    return out;
}

// A file:// URL with the path's bytes (QUrl::fromLocalFile would take the text, losing a stray byte)
static QUrl url_of(const QString &path)
{
    return QUrl::fromEncoded("file://" + enc(path).toPercentEncoding("/"));
}

FSModel::FSModel(ThumbnailManager *thumbs, QObject *parent) : QAbstractItemModel(parent), thumbs(thumbs)
{
    link = std::make_shared<fsmodel::Link>();
    link->model = this;
    change_timer.setSingleShot(true);
    connect(&change_timer, &QTimer::timeout, this, &FSModel::check_changed);
    connect(thumbs, &ThumbnailManager::updated, this, &FSModel::thumb_ready);
}

FSModel::~FSModel()
{
    {
        std::lock_guard<std::mutex> g(link->m);
        link->model = nullptr;
    }
    if (watch >= 0)
        Hub::instance()->remove(this, watch);
}

// ---------------------------------------------------------------- the folder

QModelIndex FSModel::setRootPath(const QString &path)
{
    QString p = path.size() > 1 && path.endsWith('/') ? rstrip(path, '/') : path;
    if (p.isEmpty())
        p = "/";
    if (p == root)
        return index(0, 0, QModelIndex());
    beginResetModel();
    root = p;
    items.clear();
    rows.clear();
    changed.clear();
    checking = false;   // (a look at the old folder's changes still on its way is dropped: another gen)
    done = false;
    link->gen = ++gen;
    endResetModel();
    if (watch >= 0)
        Hub::instance()->remove(this, watch);
    watch = Hub::instance()->add(this, enc(root));   // (before listing: a change while it lists isn't missed)
    list(false);
    return index(0, 0, QModelIndex());
}

void FSModel::list(bool again)
{
    relisting = again;
    seen.clear();
    std::thread(fsmodel::list_folder, link, gen, enc(root)).detach();
}

void FSModel::take_listed(int g, const QList<Entry> &batch, bool last)
{
    if (g != gen)
        return;
    QVector<Entry *> added;
    for (const Entry &e : batch) {
        if (relisting)
            seen << e.name;
        auto it = items.find(e.name);
        if (it == items.end()) {
            auto made = std::make_shared<Entry>(e);
            items.insert(e.name, made);
            if (shown(*made))
                added << made.get();
        } else {
            int row = (*it)->row;
            **it = e;
            (*it)->row = row;
        }
    }
    add_shown(added);
    if (!last)
        return;
    if (relisting) {   // what wasn't listed again has gone
        relisting = false;
        QList<std::shared_ptr<Entry>> gone;   // (kept until their rows are removed)
        for (auto it = items.begin(); it != items.end();) {
            if (seen.contains(it.key())) {
                ++it;
            } else {
                gone << *it;
                it = items.erase(it);
            }
        }
        seen.clear();
        set_shown(sorted_shown());
        if (!rows.isEmpty())   // (the items listed again may have changed)
            Q_EMIT dataChanged(index(0, 0, index(0, 0)), index(int(rows.size()) - 1, COLUMNS - 1, index(0, 0)));
    } else {
        resort();   // (items listed again kept their place)
    }
    bool first = !done;
    done = true;
    if (!changed.isEmpty())
        check_changed();
    if (first)
        Q_EMIT directoryLoaded(root);
}

void FSModel::take_events(const QList<QByteArray> &names, bool overflow, bool gone)
{
    if (gone) {   // the folder itself was deleted or moved away: nothing to show from it
        if (watch >= 0)
            Hub::instance()->remove(this, watch);
        watch = -1;
        auto old = std::move(items);   // (kept until their rows are removed)
        items.clear();
        set_shown({});
        return;
    }
    if (overflow) {
        changed.clear();
        if (done)
            list(true);
        return;
    }
    for (const QByteArray &n : names)
        changed << n;
    if (!change_timer.isActive())
        change_timer.start(150);   // (changes coming one after another, as a copy writes, looked at together)
}

void FSModel::check_changed()
{
    if (!done || checking || changed.isEmpty()) {   // (after the listing; one look at a time)
        if (checking && !changed.isEmpty() && !change_timer.isActive())
            change_timer.start(150);
        return;
    }
    QList<QByteArray> names(changed.begin(), changed.end());
    changed.clear();
    checking = true;
    std::thread(fsmodel::check_names, link, gen, enc(root), names).detach();
}

void FSModel::take_checked(int g, const QList<QByteArray> &gone, const QList<Entry> &found)
{
    if (g != gen)
        return;
    checking = false;
    bool removed = false, moved = false;
    QList<std::shared_ptr<Entry>> dropped;   // (kept until their rows are removed)
    for (const QByteArray &raw : gone) {
        auto it = items.find(dec(raw.constData()));
        if (it != items.end()) {
            removed = removed || (*it)->row >= 0;
            dropped << *it;
            items.erase(it);
        }
    }
    QVector<Entry *> added;
    for (const Entry &e : found) {
        auto it = items.find(e.name);
        if (it == items.end()) {
            auto made = std::make_shared<Entry>(e);
            items.insert(e.name, made);
            if (shown(*made))
                added << made.get();
            continue;
        }
        Entry &old = **it;
        bool was = old.row >= 0;
        if (shown(e) != was) {
            moved = true;   // (now shown or hidden: set_shown below)
        } else if (was && (old.size != e.size || old.mtime != e.mtime || old.dir != e.dir || old.link != e.link)) {
            moved = moved || sort_column != NAME || old.dir != e.dir;
            QModelIndex a = index(old.row, 0, index(0, 0)), b = index(old.row, COLUMNS - 1, index(0, 0));
            int row = old.row;
            old = e;
            old.row = row;
            Q_EMIT dataChanged(a, b);
            continue;
        }
        int row = old.row;
        old = e;
        old.row = row;
    }
    if (removed || moved)
        set_shown(sorted_shown());
    add_shown(added);
    if (!changed.isEmpty() && !change_timer.isActive())
        change_timer.start(150);
}

// ---------------------------------------------------------------- filters and order

bool FSModel::shown(const Entry &e) const
{
    if (!(filters & QDir::Hidden) && e.name.startsWith('.'))
        return false;
    if (!(filters & QDir::System) && e.special)
        return false;
    if (e.dir ? !(filters & (QDir::Dirs | QDir::AllDirs)) : !(filters & QDir::Files))
        return false;
    if (patterns.isEmpty() || (e.dir && (filters & QDir::AllDirs)))
        return true;
    return std::any_of(patterns.begin(), patterns.end(), [&e](const QRegularExpression &re) { return re.match(e.name).hasMatch(); });
}

void FSModel::setFilter(QDir::Filters f)
{
    if (f == filters)
        return;
    filters = f;
    set_shown(sorted_shown());
}

void FSModel::setNameFilters(const QStringList &list)
{
    if (list == globs)
        return;
    globs = list;
    patterns.clear();
    for (const QString &g : list)
        patterns << QRegularExpression(QRegularExpression::wildcardToRegularExpression(g),   // (anchored: the whole name)
                                       QRegularExpression::CaseInsensitiveOption);
    set_shown(sorted_shown());
}

bool FSModel::less(const Entry *a, const Entry *b) const
{
    auto by_name = [](const Entry *x, const Entry *y) {
        int c = x->key && y->key ? x->key->compare(*y->key) : 0;
        return c ? c < 0 : x->raw < y->raw;
    };
    switch (sort_column) {
    case SIZE:
        if (a->dir != b->dir)
            return a->dir;
        if (a->size != b->size)
            return a->size < b->size;
        return by_name(a, b);
    case TYPE: {
        int c = a->type_key && b->type_key ? a->type_key->compare(*b->type_key) : 0;
        return c ? c < 0 : by_name(a, b);
    }
    case MODIFIED:
        if (a->mtime != b->mtime)
            return a->mtime < b->mtime;
        return by_name(a, b);
    default:
        if (a->dir != b->dir)
            return a->dir;
        return by_name(a, b);
    }
}

QVector<Entry *> FSModel::sorted_shown() const
{
    QVector<Entry *> out;
    out.reserve(items.size());
    for (const auto &e : items)
        if (shown(*e))
            out << e.get();
    std::sort(out.begin(), out.end(), [this](const Entry *a, const Entry *b) { return less(a, b); });
    if (sort_order == Qt::DescendingOrder)
        std::reverse(out.begin(), out.end());
    return out;
}

void FSModel::renumber()
{
    for (int i = 0; i < rows.size(); ++i)
        rows[i]->row = i;
}

void FSModel::set_shown(const QVector<Entry *> &list)
{
    if (root.isEmpty())
        return;
    QModelIndex top = index(0, 0);
    QSet<Entry *> keep(list.begin(), list.end());
    for (int r = int(rows.size()) - 1; r >= 0;) {   // what goes, in runs from the end
        if (keep.contains(rows[r])) {
            --r;
            continue;
        }
        int hi = r;
        while (r >= 0 && !keep.contains(rows[r]))
            --r;
        beginRemoveRows(top, r + 1, hi);
        for (int i = r + 1; i <= hi; ++i)
            rows[i]->row = -1;
        rows.remove(r + 1, hi - r);
        endRemoveRows();
    }
    // what stays must be in list's order (a subsequence of it), else it's sorted first (an item whose size or date
    // changed, sorted by those); then what comes in goes where list has it
    renumber();
    for (int i = 0, j = 0; i < rows.size(); ++i, ++j) {
        while (j < list.size() && list[j] != rows[i])
            ++j;
        if (j >= list.size()) {
            resort();
            break;
        }
    }
    QSet<Entry *> have(rows.begin(), rows.end());
    for (int r = 0; r < list.size();) {
        if (have.contains(list[r])) {
            ++r;
            continue;
        }
        int lo = r;
        while (r < list.size() && !have.contains(list[r]))
            ++r;
        beginInsertRows(top, lo, r - 1);
        rows.insert(lo, r - lo, nullptr);
        for (int i = lo; i < r; ++i)
            rows[i] = list[i];
        endInsertRows();
    }
    renumber();
}

void FSModel::add_shown(const QVector<Entry *> &added)
{
    if (added.isEmpty() || root.isEmpty())
        return;
    if (added.size() <= 16 || rows.isEmpty()) {   // each where it goes
        set_shown(sorted_shown());
        return;
    }
    // many (a listing's batch): at the end, then sorted, as QFileSystemModel does (inserting each where it goes
    // would be a run of rows for nearly every one)
    QModelIndex top = index(0, 0);
    beginInsertRows(top, int(rows.size()), int(rows.size() + added.size()) - 1);
    rows += added;
    endInsertRows();
    renumber();
    resort();
}

void FSModel::resort()
{
    if (rows.isEmpty())
        return;
    QVector<Entry *> order = rows;
    std::sort(order.begin(), order.end(), [this](const Entry *a, const Entry *b) { return less(a, b); });
    if (sort_order == Qt::DescendingOrder)
        std::reverse(order.begin(), order.end());
    if (order == rows)
        return;
    Q_EMIT layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    QModelIndexList from = persistentIndexList(), to;
    QVector<Entry *> was;
    for (const QModelIndex &i : from)
        was << (i.isValid() && i.internalId() == 1 && i.row() < rows.size() ? rows[i.row()] : nullptr);
    rows = order;
    renumber();
    for (int k = 0; k < from.size(); ++k)
        to << (was[k] ? createIndex(was[k]->row, from[k].column(), quintptr(1)) : from[k]);
    changePersistentIndexList(from, to);
    Q_EMIT layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void FSModel::sort(int column, Qt::SortOrder order)
{
    if (column < 0 || column >= COLUMNS)
        column = NAME;
    if (column == sort_column && order == sort_order)
        return;
    sort_column = column;
    sort_order = order;
    resort();
}

// ---------------------------------------------------------------- the model

// The folder is row 0 at the top (internal id 0); its items are its children (internal id 1)
QModelIndex FSModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column < 0 || column >= COLUMNS || root.isEmpty())
        return QModelIndex();
    if (!parent.isValid())
        return row == 0 ? createIndex(0, column, quintptr(0)) : QModelIndex();
    if (parent.internalId() != 0 || parent.column() != 0 || row < 0 || row >= rows.size())
        return QModelIndex();
    return createIndex(row, column, quintptr(1));
}

QModelIndex FSModel::index(const QString &path, int column) const
{
    if (root.isEmpty())
        return QModelIndex();
    QString p = path.size() > 1 && path.endsWith('/') ? rstrip(path, '/') : path;
    if (p == root)
        return index(0, column, QModelIndex());
    if (dirname(p) != root)
        return QModelIndex();
    auto it = items.constFind(basename(p));
    return it != items.constEnd() && (*it)->row >= 0 ? createIndex((*it)->row, column, quintptr(1)) : QModelIndex();
}

QModelIndex FSModel::parent(const QModelIndex &child) const
{
    return child.isValid() && child.internalId() == 1 ? createIndex(0, 0, quintptr(0)) : QModelIndex();
}

int FSModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return root.isEmpty() ? 0 : 1;
    return parent.internalId() == 0 && parent.column() == 0 ? int(rows.size()) : 0;
}

int FSModel::columnCount(const QModelIndex &parent) const { return parent.column() > 0 ? 0 : COLUMNS; }

Entry *FSModel::entry_at(const QModelIndex &i) const
{
    return i.isValid() && i.internalId() == 1 && i.row() < rows.size() ? rows[i.row()] : nullptr;
}

QString FSModel::filePath(const QModelIndex &i) const
{
    if (i.isValid() && i.internalId() == 0)
        return root;
    Entry *e = entry_at(i);
    return e ? join(root, e->name) : QString();
}

QString FSModel::fileName(const QModelIndex &i) const
{
    if (i.isValid() && i.internalId() == 0)
        return basename(root);
    Entry *e = entry_at(i);
    return e ? e->name : QString();
}

QDateTime FSModel::lastModified(const QModelIndex &i) const
{
    Entry *e = entry_at(i);
    return e ? QDateTime::fromSecsSinceEpoch(e->mtime) : QDateTime();
}

qint64 FSModel::size(const QModelIndex &i) const
{
    Entry *e = entry_at(i);
    return e ? e->size : 0;
}

bool FSModel::isDir(const QModelIndex &i) const
{
    Entry *e = entry_at(i);
    return e ? e->dir : (i.isValid() && i.internalId() == 0);
}

QVariant FSModel::data(const QModelIndex &i, int role) const
{
    if (!i.isValid())
        return QVariant();
    if (i.internalId() == 0) {   // the folder itself
        if (i.column() != 0)
            return QVariant();
        if (role == PathRole)
            return root;
        if (role == KindRole)
            return KIND_DIR;
        if (role == Qt::DisplayRole)
            return root == "/" ? root : shown_name(basename(root));
        return QVariant();
    }
    Entry *e = entry_at(i);
    if (!e)
        return QVariant();
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        switch (i.column()) {
        case NAME:
            return shown_name(e->name);
        case SIZE:
            return e->dir ? QString() : human_size(e->size);
        case TYPE:
            return e->type;
        case MODIFIED:
            return i18n::date_time(QDateTime::fromSecsSinceEpoch(e->mtime));
        }
        return QVariant();
    case Qt::TextAlignmentRole:
        return i.column() == SIZE ? QVariant(int(Qt::AlignTrailing | Qt::AlignVCenter))
                                  : QVariant(int(Qt::AlignLeading | Qt::AlignVCenter));
    default:
        break;
    }
    if (i.column() != NAME)
        return QVariant();
    QString path = join(root, e->name);
    switch (role) {
    case ThumbRole: {
        QPixmap pm = thumb(*e, path);
        return pm.isNull() ? QVariant() : QVariant(pm);
    }
    case Qt::DecorationRole: {
        QPixmap pm = thumb(*e, path);
        return pm.isNull() ? QVariant(plain_icon(*e, path)) : QVariant(QIcon(pm));
    }
    case PathRole:
        return path;
    case KindRole:
        return (e->dir ? KIND_DIR : 0) | (e->link ? KIND_LINK : 0);
    case SizeRole:
        return e->size;
    default:
        return QVariant();
    }
}

QVariant FSModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return QVariant();
    if (role == Qt::TextAlignmentRole)
        return int(Qt::AlignLeading | Qt::AlignVCenter);
    if (role != Qt::DisplayRole)
        return QVariant();
    switch (section) {
    case NAME:
        return tx("Name");
    case SIZE:
        return tx("Size");
    case TYPE:
        return tx("Type");
    case MODIFIED:
        return tx("Date Modified");
    }
    return QVariant();
}

Qt::ItemFlags FSModel::flags(const QModelIndex &i) const
{
    if (!i.isValid())
        return Qt::ItemIsDropEnabled;
    if (i.internalId() == 0)
        return Qt::ItemIsEnabled | Qt::ItemIsDropEnabled;
    Entry *e = entry_at(i);
    if (!e)
        return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled | Qt::ItemNeverHasChildren;
    return e->dir ? f | Qt::ItemIsDropEnabled : f;
}

// ---------------------------------------------------------------- pictures

QPixmap FSModel::thumb(const Entry &e, const QString &path) const
{
    if (e.dir)
        return thumbs->folder_pixmap(path, e.mtime, thumb_size, folder_previews, this);
    if (!thumbs::can_thumbnail(path))
        return QPixmap();
    return thumbs->get(path, e.mtime, false, thumb_size, e.size, this);
}

QIcon FSModel::plain_icon(const Entry &e, const QString &path) const
{
    if (e.dir)
        return icon_for_path(path, 1);
    int dot = e.name.lastIndexOf('.');
    QString suf = dot >= 0 ? e.name.mid(dot + 1).toLower() : QString();
    auto it = suffix_icons.constFind(suf);
    if (it != suffix_icons.constEnd())
        return *it;
    QIcon icon = icon_for_path(path, 0);
    suffix_icons.insert(suf, icon);
    return icon;
}

void FSModel::thumb_ready(const QString &path)
{
    if (dirname(path) != root)
        return;
    QModelIndex idx = index(path);
    if (idx.isValid())
        Q_EMIT dataChanged(idx, idx, {Qt::DecorationRole, ThumbRole});
}

// ---------------------------------------------------------------- drag and drop

QStringList FSModel::mimeTypes() const { return {"text/uri-list", archive::MEMBERS_MIME}; }

QMimeData *FSModel::mimeData(const QModelIndexList &indexes) const
{
    QList<QUrl> urls;
    for (const QModelIndex &i : indexes)
        if (i.column() == NAME)
            if (QString p = filePath(i); !p.isEmpty())
                urls << url_of(p);
    auto *data = new QMimeData;
    data->setUrls(urls);
    return data;
}

Qt::DropActions FSModel::supportedDropActions() const { return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction; }

bool FSModel::dropMimeData(const QMimeData *data, Qt::DropAction, int, int, const QModelIndex &parent)
{
    QString target = parent.isValid() ? filePath(parent) : root;
    if (!isdir(target))
        target = dirname(target);
    // from Open as Archive: extracted straight into the folder (checked first: the file list it also offers would
    // extract them to a temporary folder)
    if (data->hasFormat(archive::MEMBERS_MIME) && members_handler) {
        auto handler = members_handler;
        auto *copy = new QMimeData;   // the drag's data goes when the drag ends
        copy->setData(archive::MEMBERS_MIME, data->data(archive::MEMBERS_MIME));
        QTimer::singleShot(0, this, [handler, copy, target]() {
            handler(copy, target);
            delete copy;
        });
        return true;
    }
    if (!data->hasUrls() || !drop_handler)
        return false;
    QStringList paths;
    for (const QUrl &u : data->urls())
        if (u.isLocalFile())
            paths << uri_to_path(QString::fromLatin1(u.toEncoded()));   // (by its bytes: toLocalFile() loses a stray one)
    auto handler = drop_handler;
    bool copy_only = data->hasFormat(COPY_ONLY_MIME);
    QTimer::singleShot(0, this, [handler, paths, target, copy_only]() { handler(paths, target, copy_only); });
    return true;
}
