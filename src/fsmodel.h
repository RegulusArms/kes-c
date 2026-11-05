// The folder model of the file views: one folder's items, listed by their names' bytes (util.h dec()), so a folder
// whose own name, or a name in it, isn't valid UTF-8 lists and opens like any other. (Qt's QFileSystemModel, used
// before, decodes names lossily and opens folders by that text: such a folder listed nothing.)
//
// Shaped as QFileSystemModel is for the views: one top-level row, the folder (index(rootPath()), the views' root),
// whose children are its items, in four columns (name, size, type, date modified). It's listed on a thread of its own
// (a slow or busy drive doesn't hold up the window), in batches, then directoryLoaded(); changes in the folder come
// through inotify (its own: QFileSystemWatcher takes paths as text too) and only the names that changed are looked at
// again, also on a thread. Sorted as QFileSystemModel sorts: folders first, names in natural order (numbers by value,
// case ignored), the whole list mirrored for descending.
#pragma once

#include <QAbstractItemModel>
#include <QCollatorSortKey>
#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QIcon>
#include <QPixmap>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

class ThumbnailManager;

namespace fsmodel {

// One item of the folder, as listed (on the lister's thread)
struct Entry {
    QByteArray raw;      // its name's bytes
    QString name;        // dec(raw): the name Kestrel's paths use
    QString type;        // "Folder", "PNG image"… (the Type column)
    std::optional<QCollatorSortKey> key, type_key;   // for sorting
    qint64 size = 0, mtime = 0;   // a symlink's: what it points to (st_mtime, seconds)
    bool dir = false;     // a folder (also a symlink to one)
    bool link = false;    // a symlink
    bool special = false; // not a file or folder: a device, a FIFO, a socket, or a symlink to nothing
    int row = -1;         // its row in the view (-1: hidden by the filters)
};

// The Type column's text for a name: "Folder", else its MIME type's description by the name (cached by extension;
// any thread)
QString type_of(const QString &name, bool dir);

struct Link;   // how the lister's threads reach the model (it may be gone when they finish)

}  // namespace fsmodel

class FSModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit FSModel(ThumbnailManager *thumbs, QObject *parent = nullptr);
    ~FSModel() override;

    enum Column { NAME, SIZE, TYPE, MODIFIED, COLUMNS };

    // Shows this folder (listed anew unless it's the one shown): returns its index, the views' root
    QModelIndex setRootPath(const QString &path);
    QString rootPath() const { return root; }
    // The folder's index, or one of its items'; invalid for anything else (or an item hidden by the filters)
    QModelIndex index(const QString &path, int column = 0) const;
    QString filePath(const QModelIndex &index) const;
    QString fileName(const QModelIndex &index) const;
    QDateTime lastModified(const QModelIndex &index) const;
    qint64 size(const QModelIndex &index) const;
    bool isDir(const QModelIndex &index) const;
    bool loaded() const { return done; }   // the folder has been listed
    // As QFileSystemModel's: AllEntries (Dirs, Files), Hidden (names starting with "."), System (devices, FIFOs,
    // sockets, broken symlinks), AllDirs (folders aren't hidden by the name filters)
    void setFilter(QDir::Filters filters);
    QDir::Filters filter() const { return filters; }
    // Wildcards ("*.png"), case ignored; items that match none are hidden (QFileSystemModel's nameFilterDisables off)
    void setNameFilters(const QStringList &globs);
    QStringList nameFilters() const { return globs; }

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    QStringList mimeTypes() const override;   // files, and items dragged out of Open as Archive (archive::MEMBERS_MIME)
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;
    Qt::DropActions supportedDropActions() const override;

    int thumb_size = 128;
    bool folder_previews = true;
    std::function<void(const QStringList &, const QString &, bool)> drop_handler;   // (paths, folder, copy only)
    // items dragged out of Open as Archive, dropped into target (ArchiveWindow::drop_members)
    std::function<void(const QMimeData *, const QString &)> members_handler;

    // (the lister's and the watcher's results: UI thread, only for the folder they were asked for)
    void take_listed(int gen, const QList<fsmodel::Entry> &batch, bool last);
    void take_checked(int gen, const QList<QByteArray> &names, const QList<fsmodel::Entry> &found);
    void take_events(const QList<QByteArray> &names, bool overflow, bool gone);   // inotify's, for this folder

Q_SIGNALS:
    void directoryLoaded(const QString &path);

private:
    using Entry = fsmodel::Entry;
    bool shown(const Entry &e) const;          // by the filters
    bool less(const Entry *a, const Entry *b) const;   // ascending, by the sort column
    QVector<Entry *> sorted_shown() const;     // every item the filters let through, in the order shown
    void set_shown(const QVector<Entry *> &list);   // rows removed and inserted to make it so (no reset)
    void add_shown(const QVector<Entry *> &added);  // newly listed items that show
    void resort();                             // the same items in the sort order (layoutChanged)
    void renumber();
    void list(bool again);                     // (again: the folder as it is now, dropping what has gone)
    void check_changed();
    Entry *entry_at(const QModelIndex &index) const;
    QPixmap thumb(const Entry &e, const QString &path) const;
    QIcon plain_icon(const Entry &e, const QString &path) const;
    void thumb_ready(const QString &path);

    ThumbnailManager *thumbs;
    std::shared_ptr<fsmodel::Link> link;
    QString root;
    QHash<QString, std::shared_ptr<Entry>> items;   // every item listed, shown or not, by name
    QVector<Entry *> rows;                          // the items shown, in order
    QSet<QString> seen;                             // listed again (list(true)) so far: what's not in it has gone
    bool relisting = false, done = false;
    int gen = 0;                                    // which folder (and listing) results are for
    int watch = -1;                                 // its inotify watch
    QSet<QByteArray> changed;                       // names inotify said changed, to look at again
    bool checking = false;                          // a look at them is on its way
    QTimer change_timer;
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
    QStringList globs;
    QList<QRegularExpression> patterns;
    int sort_column = NAME;
    Qt::SortOrder sort_order = Qt::AscendingOrder;
    mutable QHash<QString, QIcon> suffix_icons;
};
