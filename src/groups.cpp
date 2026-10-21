#include "groups.h"

#include "drives.h"
#include "metadata.h"
#include "util.h"

#include <QDateTime>
#include <QLocale>
#include <QMap>

#include <fcntl.h>
#include <sys/stat.h>

namespace groups {

static const int FUTURE = 0, TODAY = 1, YESTERDAY = 2, THIS_WEEK = 3, LAST_WEEK = 4, THIS_MONTH = 5, LAST_MONTH = 6,
                 THIS_YEAR = 7, YEARS = 100, NO_DATE = 100000;   // YEARS + n: n years ago

// Each group takes the dates not taken by a newer one, so the groups never overlap and stay in date order (a week
// that began last month is still "Last week" or "Earlier this week").
int key(const QDate &date, const QDate &today, Qt::DayOfWeek first_day)
{
    if (!date.isValid())
        return NO_DATE;
    if (date > today)
        return FUTURE;
    if (date == today)
        return TODAY;
    if (date == today.addDays(-1))
        return YESTERDAY;
    QDate week = today.addDays(-((today.dayOfWeek() - first_day + 7) % 7));
    if (date >= week)
        return THIS_WEEK;
    if (date >= week.addDays(-7))
        return LAST_WEEK;
    QDate month(today.year(), today.month(), 1);
    if (date >= month)
        return THIS_MONTH;
    if (date >= month.addMonths(-1))
        return LAST_MONTH;
    if (date.year() == today.year())
        return THIS_YEAR;
    return YEARS + today.year() - date.year();
}

QString title(int key, const QDate &today)
{
    switch (key) {
    case FUTURE:
        return "In the future";
    case TODAY:
        return "Today";
    case YESTERDAY:
        return "Yesterday";
    case THIS_WEEK:
        return "Earlier this week";
    case LAST_WEEK:
        return "Last week";
    case THIS_MONTH:
        return "Earlier this month";
    case LAST_MONTH:
        return "Last month";
    case THIS_YEAR:
        return "Earlier this year";
    case NO_DATE:
        return "No date";
    default:
        return QString::number(today.year() - (key - YEARS));
    }
}

static const int DAY_BASE = 100000000;   // day keys: DAY_BASE - the Julian day, so newer is smaller

int day_key(const QDate &date)
{
    if (!date.isValid() || date.toJulianDay() <= 0 || date.toJulianDay() >= DAY_BASE)
        return NO_DATE + DAY_BASE;
    return int(DAY_BASE - date.toJulianDay());
}

QString day_title(int key, const QDate &today)
{
    if (key >= NO_DATE + DAY_BASE)
        return "No date";
    QDate d = QDate::fromJulianDay(DAY_BASE - key);
    QString text = QLocale().toString(d, QLocale::LongFormat);
    if (d == today)
        return "Today, " + text;
    if (d == today.addDays(-1))
        return "Yesterday, " + text;
    return text;
}

// ---------------------------------------------------------------- Dates

Dates *Dates::instance()
{
    static Dates *d = new Dates;
    return d;
}

Dates::Dates()
{
    pool.setMaxThreadCount(2);
    notify.setSingleShot(true);
    notify.setInterval(300);
    connect(&notify, &QTimer::timeout, this, &Dates::ready);
}

QDate Dates::created(const QString &path, qint64 mtime)
{
    auto it = born.constFind(path);
    if (it != born.constEnd() && it->mtime == mtime)
        return it->date;
    struct statx sx;
    QDate d;
    if (::statx(AT_FDCWD, path.toUtf8().constData(), 0, STATX_BTIME | STATX_MTIME, &sx) == 0)
        d = QDateTime::fromSecsSinceEpoch(sx.stx_mask & STATX_BTIME ? sx.stx_btime.tv_sec : sx.stx_mtime.tv_sec).date();
    if (born.size() > 200000)
        born.clear();
    born.insert(path, Entry{mtime, d});
    return d;
}

QDate Dates::taken(const QString &path, qint64 mtime, bool *pending)
{
    if (pending)
        *pending = false;
    if (!metadata::may_have_taken(path) || !drives::reads_allowed(path))   // a tape or floppy: not read
        return created(path, mtime);
    auto it = shot.constFind(path);
    if (it != shot.constEnd() && it->mtime == mtime)
        return it->date.isValid() ? it->date : created(path, mtime);
    if (pending)
        *pending = true;
    if (!asked.contains(path)) {
        asked.insert(path);
        QMutexLocker l(&lock);
        queue << path;
        if (reading < pool.maxThreadCount()) {
            ++reading;
            pool.start([this]() { read_some(); });
        }
    }
    return created(path, mtime);
}

// on a worker: read the queue's files, handing the dates back in batches
void Dates::read_some()
{
    for (;;) {
        QStringList batch;
        {
            QMutexLocker l(&lock);
            batch = queue.mid(0, 50);
            queue.remove(0, batch.size());
            if (batch.isEmpty()) {
                --reading;   // under the lock: a path queued from now on starts another worker
                return;
            }
        }
        QList<std::tuple<QString, qint64, QDate>> got;
        for (const QString &p : batch) {
            struct stat st;
            qint64 mtime = util::stat_(p, st) ? qint64(st.st_mtime) : 0;
            got.append({p, mtime, metadata::taken(p).date()});
        }
        QMetaObject::invokeMethod(
            this,
            [this, got]() {
                if (shot.size() > 200000)
                    shot.clear();
                for (const auto &[p, mtime, date] : got) {
                    shot.insert(p, Entry{mtime, date});
                    asked.remove(p);
                }
                if (!notify.isActive())
                    notify.start();
            },
            Qt::QueuedConnection);
    }
}

}  // namespace groups

DateGroupProxy::DateGroupProxy(QObject *parent) : QAbstractProxyModel(parent)
{
    midnight.setSingleShot(true);
    connect(&midnight, &QTimer::timeout, this, [this]() { regroup(); });   // Today becomes Yesterday
}

void DateGroupProxy::set_source(QAbstractItemModel *source, const QModelIndex &root_index,
                                std::function<QDate(const QModelIndex &)> date_fn)
{
    beginResetModel();
    for (const auto &c : links)
        disconnect(c);
    links.clear();
    src = source;
    root = root_index;
    rooted = root_index.isValid();
    date_of = std::move(date_fn);
    folded.clear();
    changing = 0;
    if (src) {
        auto under_root = [this](const QModelIndex &parent) { return parent == root; };
        links << connect(src, &QAbstractItemModel::rowsAboutToBeInserted, this,
                         [this, under_root](const QModelIndex &p) { if (under_root(p)) before(); });
        links << connect(src, &QAbstractItemModel::rowsInserted, this,
                         [this, under_root](const QModelIndex &p) { if (under_root(p)) after(); });
        // any removal or move: it may take the folder itself
        links << connect(src, &QAbstractItemModel::rowsAboutToBeRemoved, this, [this]() { before(); });
        links << connect(src, &QAbstractItemModel::rowsRemoved, this, [this]() { after(); });
        links << connect(src, &QAbstractItemModel::rowsAboutToBeMoved, this, [this]() { before(); });
        links << connect(src, &QAbstractItemModel::rowsMoved, this, [this]() { after(); });
        links << connect(src, &QAbstractItemModel::layoutAboutToBeChanged, this, [this]() { before(); });
        links << connect(src, &QAbstractItemModel::layoutChanged, this, [this]() { after(); });
        links << connect(src, &QAbstractItemModel::modelAboutToBeReset, this, [this]() { beginResetModel(); });
        links << connect(src, &QAbstractItemModel::modelReset, this, [this]() {
            changing = 0;
            build();
            endResetModel();
        });
        links << connect(src, &QAbstractItemModel::dataChanged, this, &DateGroupProxy::data_changed);
        links << connect(src, &QAbstractItemModel::headerDataChanged, this, &QAbstractItemModel::headerDataChanged);
        links << connect(src, &QObject::destroyed, this, [this]() {
            beginResetModel();
            src = nullptr;
            changing = 0;
            build();
            endResetModel();
        });
    }
    build();
    endResetModel();
}

void DateGroupProxy::set_order(int column, Qt::SortOrder order)
{
    bool old = oldest_first;
    oldest_first = column == 3 && order == Qt::AscendingOrder;   // by Modified, oldest first
    if (old != oldest_first && src)
        regroup();
}

void DateGroupProxy::set_mode(int m)
{
    if (m == mode)
        return;
    mode = m;
    if (src)
        regroup();
}

void DateGroupProxy::set_watch_dates(bool on)
{
    disconnect(dates_link);
    if (on)
        dates_link = connect(groups::Dates::instance(), &groups::Dates::ready, this, [this]() {
            if (src && !changing)
                regroup();
        });
}

int DateGroupProxy::key_of(int r, Qt::DayOfWeek first_day) const
{
    QDate d = date_of ? date_of(src->index(r, 0, root)) : QDate();
    return mode == groups::DAY ? groups::day_key(d) : groups::key(d, today, first_day);
}

void DateGroupProxy::build()
{
    today = QDate::currentDate();
    rows.clear();
    to_row.clear();
    keys.clear();
    in_order.clear();
    counts.clear();
    header_at.clear();
    int n = src && (root.isValid() || !rooted) ? src->rowCount(root) : 0;   // the folder may be gone
    Qt::DayOfWeek first_day = QLocale().firstDayOfWeek();
    QMap<int, QList<int>> members;   // group -> its source rows, in the source's order
    keys.resize(n);
    to_row.fill(-1, n);
    for (int r = 0; r < n; ++r) {
        int k = key_of(r, first_day);
        keys[r] = k;
        members[k] << r;
    }
    QList<int> order = members.keys();
    if (oldest_first)
        std::reverse(order.begin(), order.end());
    for (int k : order) {
        const QList<int> &m = members[k];
        counts.insert(k, int(m.size()));
        header_at.insert(k, int(rows.size()));
        rows << Row{k, -1};
        in_order << m;
        if (folded.contains(k))
            continue;
        for (int r : m) {
            to_row[r] = int(rows.size());
            rows << Row{k, r};
        }
        rows << Row{k, FOOTER};   // collapses the group from its end
    }
    if (!midnight.isActive()) {
        QDateTime next(today.addDays(1), QTime(0, 0, 1));
        midnight.start(int(std::clamp<qint64>(QDateTime::currentDateTime().msecsTo(next), 1000, 86400000)));
    }
}

void DateGroupProxy::before()
{
    if (changing++ > 0)
        return;
    Q_EMIT layoutAboutToBeChanged();
    saved_from = persistentIndexList();
    saved_to.clear();
    for (const QModelIndex &i : std::as_const(saved_from)) {
        if (i.row() >= rows.size())
            saved_to << qMakePair(-2, QPersistentModelIndex());
        else if (rows[i.row()].src < 0)
            saved_to << qMakePair(rows[i.row()].key, QPersistentModelIndex());
        else
            saved_to << qMakePair(-1, QPersistentModelIndex(src->index(rows[i.row()].src, i.column(), root)));
    }
}

void DateGroupProxy::after()
{
    if (changing == 0 || --changing > 0)
        return;
    build();
    QModelIndexList to;
    for (int i = 0; i < saved_from.size(); ++i) {
        const auto &[k, item] = saved_to[i];
        int col = saved_from[i].column();
        if (k >= 0)
            to << (header_at.contains(k) ? index(header_at.value(k), col) : QModelIndex());
        else if (k == -1)
            to << mapFromSource(item);
        else
            to << QModelIndex();
    }
    changePersistentIndexList(saved_from, to);
    saved_from.clear();
    saved_to.clear();
    Q_EMIT layoutChanged();
}

void DateGroupProxy::regroup()
{
    before();
    after();
}

void DateGroupProxy::data_changed(const QModelIndex &a, const QModelIndex &b, const QList<int> &roles)
{
    if (changing || !a.isValid() || a.parent() != root)
        return;
    // the date can only change with the text (a thumbnail arriving doesn't move anything)
    if (roles.isEmpty() || roles.contains(Qt::DisplayRole)) {
        Qt::DayOfWeek first_day = QLocale().firstDayOfWeek();
        for (int r = a.row(); r <= b.row() && r < keys.size(); ++r)
            if (key_of(r, first_day) != keys[r]) {
                regroup();
                return;
            }
    }
    int lo = INT_MAX, hi = -1;
    for (int r = a.row(); r <= b.row() && r < to_row.size(); ++r)
        if (to_row[r] >= 0) {
            lo = std::min(lo, to_row[r]);
            hi = std::max(hi, to_row[r]);
        }
    if (hi >= 0)
        Q_EMIT dataChanged(index(lo, a.column()), index(hi, b.column()), roles);
}

bool DateGroupProxy::is_header(const QModelIndex &i) const
{
    return i.isValid() && i.model() == this && i.row() < rows.size() && rows[i.row()].src < 0;
}

bool DateGroupProxy::is_footer(const QModelIndex &i) const
{
    return is_header(i) && rows[i.row()].src == FOOTER;
}

void DateGroupProxy::toggle(const QModelIndex &header)
{
    if (!is_header(header))
        return;
    int k = rows[header.row()].key;
    set_collapsed(k, is_footer(header) || !folded.contains(k));
}

int DateGroupProxy::group_of(const QModelIndex &i) const
{
    return i.isValid() && i.model() == this && i.row() < rows.size() ? rows[i.row()].key : -1;
}

QModelIndex DateGroupProxy::header_index(int key) const
{
    return header_at.contains(key) ? index(header_at.value(key), 0) : QModelIndex();
}

void DateGroupProxy::set_collapsed(int key, bool on)
{
    if (!header_at.contains(key) || on == folded.contains(key))
        return;
    if (on)
        folded.insert(key);
    else
        folded.remove(key);
    regroup();
}

void DateGroupProxy::set_all_collapsed(bool on)
{
    QSet<int> now = on ? QSet<int>(header_at.keyBegin(), header_at.keyEnd()) : QSet<int>();
    if (now == folded)
        return;
    folded = now;
    regroup();
}

QList<int> DateGroupProxy::group_keys() const
{
    QList<int> out;
    for (const Row &r : rows)
        if (r.src == HEADER)
            out << r.key;
    return out;
}

void DateGroupProxy::reveal(const QModelIndex &s)
{
    if (src && s.isValid() && s.model() == src && s.parent() == root && s.row() < keys.size() && folded.remove(keys[s.row()]))
        regroup();
}

QList<int> DateGroupProxy::header_rows() const
{
    QList<int> out;
    for (int r = 0; r < rows.size(); ++r)
        if (rows[r].src < 0)
            out << r;
    return out;
}

QModelIndex DateGroupProxy::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid() || row < 0 || row >= rows.size() || column < 0 || column >= columnCount())
        return QModelIndex();
    return createIndex(row, column);
}

QModelIndex DateGroupProxy::sibling(int row, int column, const QModelIndex &) const { return index(row, column); }

int DateGroupProxy::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(rows.size()); }

int DateGroupProxy::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() || !src ? 0 : src->columnCount(root);
}

// The view's root (an invalid index) is the folder: what's fetched, and where a drop on empty space goes.
QModelIndex DateGroupProxy::mapToSource(const QModelIndex &i) const
{
    if (!src)
        return QModelIndex();
    if (!i.isValid())
        return root;
    if (i.model() != this || i.row() >= rows.size() || rows[i.row()].src < 0)
        return QModelIndex();
    return src->index(rows[i.row()].src, i.column(), root);
}

QModelIndex DateGroupProxy::mapFromSource(const QModelIndex &s) const
{
    if (!src || !s.isValid() || s.model() != src || s.parent() != root)
        return QModelIndex();
    int r = to_row.value(s.row(), -1);
    return r < 0 ? QModelIndex() : index(r, s.column());
}

QVariant DateGroupProxy::data(const QModelIndex &i, int role) const
{
    if (!i.isValid() || i.row() >= rows.size())
        return QVariant();
    const Row &row = rows[i.row()];
    if (row.src >= 0)
        return src->data(src->index(row.src, i.column(), root), role);
    if (role == GroupRole)
        return row.key;
    if (role == GroupFoldedRole)
        return folded.contains(row.key);
    if (role == GroupFooterRole)
        return row.src == FOOTER;
    QString title = mode == groups::DAY ? groups::day_title(row.key, today) : groups::title(row.key, today);
    QString count = util::group_digits(counts.value(row.key));
    if (i.column() == 0 && row.src == FOOTER && (role == Qt::DisplayRole || role == Qt::AccessibleTextRole))
        return "Collapse " + title + " (" + count + ")";
    if (i.column() == 0 && row.src == FOOTER && role == Qt::ToolTipRole)
        return QString("Collapse this group (also Ctrl+Left on any of its items)");
    if (i.column() == 0 && (role == Qt::DisplayRole || role == Qt::AccessibleTextRole))
        return title + " (" + count + ")";
    if (i.column() == 0 && role == Qt::AccessibleDescriptionRole)
        return row.src == FOOTER ? QString("Collapses the group") : folded.contains(row.key) ? QString("Collapsed group") : QString("Group");
    return QVariant();
}

Qt::ItemFlags DateGroupProxy::flags(const QModelIndex &i) const
{
    if (is_header(i))
        return Qt::ItemIsEnabled;   // not selected, dragged or dropped on
    QModelIndex s = mapToSource(i);
    return s.isValid() ? src->flags(s) : Qt::NoItemFlags;
}

QVariant DateGroupProxy::headerData(int section, Qt::Orientation o, int role) const
{
    return src ? src->headerData(section, o, role) : QVariant();
}

void DateGroupProxy::sort(int column, Qt::SortOrder order)
{
    set_order(column, order);
    if (src)
        src->sort(column, order);
}

QStringList DateGroupProxy::mimeTypes() const { return src ? src->mimeTypes() : QStringList(); }

QMimeData *DateGroupProxy::mimeData(const QModelIndexList &indexes) const
{
    if (!src)
        return nullptr;
    QModelIndexList items;   // the headers aren't dragged
    for (const QModelIndex &i : indexes)
        if (i.isValid() && !is_header(i))
            items << mapToSource(i);
    return items.isEmpty() ? nullptr : src->mimeData(items);
}

Qt::DropActions DateGroupProxy::supportedDropActions() const
{
    return src ? src->supportedDropActions() : Qt::IgnoreAction;
}

Qt::DropActions DateGroupProxy::supportedDragActions() const
{
    return src ? src->supportedDragActions() : Qt::IgnoreAction;
}

bool DateGroupProxy::canDropMimeData(const QMimeData *data, Qt::DropAction action, int, int,
                                     const QModelIndex &parent) const
{
    QModelIndex s = is_header(parent) ? QModelIndex(root) : mapToSource(parent);
    return src && src->canDropMimeData(data, action, -1, -1, s);
}

bool DateGroupProxy::dropMimeData(const QMimeData *data, Qt::DropAction action, int, int, const QModelIndex &parent)
{
    QModelIndex s = is_header(parent) ? QModelIndex(root) : mapToSource(parent);
    return src && src->dropMimeData(data, action, -1, -1, s);
}
