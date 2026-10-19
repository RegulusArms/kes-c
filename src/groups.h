// Grouping by date: a folder's items (or the search results, Trash, Starred, Recent) under headers for their dates.
// Group by Period: Today, Yesterday, Earlier this week, Last week, Earlier this month, Last month, Earlier this year,
// then one per year. Group by Day: one header per date. The date is when an item was modified, created, or taken (its
// metadata, see Dates). The groups go newest first (oldest first when sorted by Modified, ascending); inside each group
// the items keep the order of the chosen sort (name, size, type…). A header is a row of its own, clicked to collapse
// or expand its group.
#pragma once

#include <QAbstractProxyModel>
#include <QDate>
#include <QHash>
#include <QMutex>
#include <QPersistentModelIndex>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

#include <functional>

namespace groups {

enum Mode { NONE = 0, PERIOD = 1, DAY = 2 };            // saved in sorts.json: keep the numbers
enum DateBy { MODIFIED = 0, CREATED = 1, TAKEN = 2 };    // likewise

// The group of a date, relative to `today`: a smaller key is newer. Weeks start on first_day (QLocale's).
int key(const QDate &date, const QDate &today, Qt::DayOfWeek first_day);
QString title(int key, const QDate &today);
int day_key(const QDate &date);   // Group by Day: one key per date, smaller is newer
QString day_title(int key, const QDate &today);

// The dates items are grouped by, other than modified (which the models have). Created: the file's birth time
// (statx), or its modified date where the file system keeps none. Taken: the date in a photo's or video's metadata
// (metadata::taken), or its created date when it has none. Both are cached by path and modification time; taken dates
// are read on worker threads: until one is read, the created date stands in, and ready() says when more have come.
class Dates : public QObject {
    Q_OBJECT
public:
    static Dates *instance();
    QDate created(const QString &path, qint64 mtime);
    QDate taken(const QString &path, qint64 mtime, bool *pending = nullptr);

Q_SIGNALS:
    void ready();   // taken dates have come in (at most every 300 ms)

private:
    Dates();
    struct Entry {
        qint64 mtime;
        QDate date;
    };
    QHash<QString, Entry> born, shot;
    QSet<QString> asked;
    QStringList queue;
    QMutex lock;   // queue and reading
    QThreadPool pool;
    QTimer notify;
    int reading = 0;   // workers running
    void read_some();
};

}  // namespace groups

enum {
    GroupRole = Qt::UserRole + 60,         // on a header row: its group's key (the items have none)
    GroupFoldedRole = Qt::UserRole + 61,   // on a header row: its group is collapsed
};

class DateGroupProxy : public QAbstractProxyModel {
    Q_OBJECT
public:
    explicit DateGroupProxy(QObject *parent = nullptr);
    // group the children of `root` in `source`; date_of(a source index in column 0) is when it was last modified
    void set_source(QAbstractItemModel *source, const QModelIndex &root,
                    std::function<QDate(const QModelIndex &)> date_of);
    QModelIndex source_root() const { return root; }
    void set_order(int column, Qt::SortOrder order);   // the sort shown: picks the order of the groups
    void set_mode(int mode);   // groups::PERIOD or DAY
    void set_watch_dates(bool on);   // regroup as taken dates come in (grouping by them)
    bool is_header(const QModelIndex &index) const;
    void toggle(const QModelIndex &header);   // collapse or expand its group
    bool collapsed(int key) const { return folded.contains(key); }
    QList<int> header_rows() const;
    QList<int> source_rows() const { return in_order; }   // in the groups' order, also those in collapsed groups
    void reveal(const QModelIndex &source);   // expand the group of a source index (to select it)

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &) const override { return QModelIndex(); }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override { return !parent.isValid(); }
    QModelIndex mapToSource(const QModelIndex &index) const override;
    QModelIndex mapFromSource(const QModelIndex &source) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QVariant headerData(int section, Qt::Orientation o, int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    QModelIndex sibling(int row, int column, const QModelIndex &index) const override;
    // Dragging and dropping go to the source as well. (Its items' data: the source isn't QAbstractProxyModel's
    // sourceModel(), so its defaults would ask an empty model, and a drag from a grouped view carried no files.)
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    Qt::DropActions supportedDropActions() const override;
    Qt::DropActions supportedDragActions() const override;
    bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                         const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;

private:
    struct Row {
        int key;
        int src;   // the source row; -1 for the group's header
    };
    void before();   // the source is about to change: the views keep their places (persistent indexes)
    void after();    // ...and has: regroup
    void regroup();  // before() + after() for a change of our own
    void build();
    void data_changed(const QModelIndex &a, const QModelIndex &b, const QList<int> &roles);
    int key_of(int source_row, Qt::DayOfWeek first_day) const;
    QAbstractItemModel *src = nullptr;
    QPersistentModelIndex root;
    bool rooted = false;        // root was a folder (not the model's top): if it goes, nothing is shown
    std::function<QDate(const QModelIndex &)> date_of;
    QList<Row> rows;
    QList<int> to_row;          // source row -> our row (-1: in a collapsed group)
    QList<int> keys;            // source row -> its group
    QList<int> in_order;        // the source rows in the groups' order
    QHash<int, int> counts;     // group -> its items
    QHash<int, int> header_at;  // group -> its header's row
    QSet<int> folded;
    bool oldest_first = false;
    int mode = groups::PERIOD;
    QMetaObject::Connection dates_link;
    QDate today;
    int changing = 0;           // before() calls waiting for their after()
    QModelIndexList saved_from;
    QList<QPair<int, QPersistentModelIndex>> saved_to;   // a header's key (-1: an item), or the item in the source
    QList<QMetaObject::Connection> links;
    QTimer midnight;
};
