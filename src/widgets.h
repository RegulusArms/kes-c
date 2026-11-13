// Models, delegates and widgets used by the main window.
#pragma once

#include "overview.h"
#include "search.h"
#include "fsmodel.h"

#include <QDate>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QHash>
#include <QLabel>
#include <QPointer>
#include <QLineEdit>
#include <QListWidget>
#include <QMutex>
#include <QPainterPath>
#include <QScrollArea>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QThread>
#include <QTimer>
#include <QToolButton>

#include <atomic>
#include <functional>

class Animator;
class ThumbnailManager;
class QVBoxLayout;
class QHBoxLayout;

enum Roles {
    ThumbRole = Qt::UserRole + 50,
    PathRole = Qt::UserRole + 51,
    StatRole = Qt::UserRole + 52,   // search results: (is_dir, mtime, size)
    KindRole = Qt::UserRole + 53,   // bit 1: directory, bit 2: symbolic link
    SizeRole = Qt::UserRole + 54,   // the file's size as the view knows it (no file system call)
    LineRole = Qt::UserRole + 55,   // content search results: the first matching line's number (0: none)
};
enum Kind { KIND_DIR = 1, KIND_LINK = 2 };

// ---------------------------------------------------------------- models

// The dragged items' picture under the pointer, drawn by Kestrel in its own windows. On Wayland the compositor draws a
// drag's picture, and GNOME doesn't show Qt's: so there Kestrel hands it none and draws its own while the drag is
// over any of its windows, following the drag events Qt delivers to them (watched on the whole application). Made for
// the length of a drag (QDrag::exec) by the file views and the image viewer.
class DragGhost : public QObject {
public:
    DragGhost(const QPixmap &pm, const QPoint &hot);
    ~DragGhost() override;

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    QPixmap pm;
    QPoint hot;
    QPointer<QLabel> label;
};

// FSModel, the views' folder model: fsmodel.h

// Between FSModel and the views: keeps the folder model's own order, or with "Pinned & Starred First" on puts the
// pinned and starred items first, each group still in that order. The views' sort() goes on to the folder model.
class PinSortProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit PinSortProxy(FSModel *source);
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    void set_pinned_first(bool on);
    bool pinned_first() const { return first; }
    void set_root(const QString &dir);   // the folder shown: sorted only if something in it is pinned or starred
    void pins_changed();

protected:
    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override;

private:
    bool pinned(const QModelIndex &i) const;   // or starred
    void apply();
    FSModel *fs;
    bool first = false;
    QString root;
    mutable QHash<const void *, QSet<QString>> names;   // per folder (its node): the pinned and starred names in it
};

// Results of a recursive search.
class SearchModel : public QStandardItemModel {
    Q_OBJECT
public:
    explicit SearchModel(ThumbnailManager *thumbs, QObject *parent = nullptr);
    ~SearchModel() override;
    // add result rows; `locations` optionally maps a path to the text for its Location column; `matches` a path to
    // its matching lines ("12:text", content search), shown in the Match column (the first) and its tooltip (all)
    // a file's [st_mode, mtime, size, is a link] (following links), or [] if it isn't there: on a thread (stats, update)
    static QVariantList stat_of(const QString &p);
    // `stats`: path → stat_of(path), as stat'ed where they were found (else stat'ed here; one missing: gone)
    void add_paths(const QStringList &paths, const QHash<QString, QString> &locations = {},
                   const QVariantMap &matches = {}, const QVariantMap &stats = {});
    void clear_results();
    // Following the files: their paths, shown or gone (to be checked on a thread, as update() takes it); update(path →
    // stat_of(path)) drops a row whose file has gone (kept aside: it comes back if
    // the file does, as Undo brings back a trashed one) and refreshes its size and time; moved() follows a rename or a
    // move by Kestrel
    QStringList known_paths() const;
    void update(const QHash<QString, QVariantList> &found);
    void moved(const QString &from, const QString &to);
    int generation = 0;   // bumps with every clear_results (a check from before is stale)
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<QString, QStandardItem *> rows;
    QHash<QString, QList<QStandardItem *>> gone;   // rows whose file has gone, taken out of the model
    int thumb_size = 128;
    bool folder_previews = true;

private:
    void thumb_ready(const QString &path);
    ThumbnailManager *thumbs;
};

class SearchThread : public QThread {
    Q_OBJECT
public:
    // recursive: the subfolders too (else root's own items); an empty query matches every name (filters only)
    SearchThread(const QString &root, const QString &query, bool hidden, QObject *parent = nullptr,
                 bool contents = false, bool recursive = true, const SearchFilter &filter = {},
                 const ContentSearch &how = {});
    std::atomic<bool> stop{false};
    QString engine;   // a content search: what it used ("index", "ripgrep", "grep", "index, then grep"…)
    QString error;    // why it found nothing it should have (a bad regular expression, …)

Q_SIGNALS:
    void found(const QStringList &paths, const QVariantMap &matches, const QVariantMap &stats);   // (stats: add_paths)

protected:
    void run() override;

private:
    void send(const QStringList &batch, const QVariantMap &matches = {});
    bool match(const QString &name) const;
    bool keep(const QString &path) const;   // the filter
    int run_contents();   // the files found with the index; -1 if it couldn't be asked
    void run_read();      // ripgrep / grep
    QString root;
    QString query, raw_query;
    bool hidden;
    bool wildcard;
    bool contents;
    bool recursive;
    SearchFilter filter;
    ContentSearch how;
    QDate today;
};

bool can_search_contents();   // the desktop's search index (localsearch) is installed

// ---------------------------------------------------------------- grid delegate

class GridDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    GridDelegate(QWidget *pane, ThumbnailManager *thumbs, std::function<bool(const QString &)> is_cut,
                 QObject *parent = nullptr);
    QSize cell_size() const;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    // What counts as the item in its cell (option.rect; State_Selected: its name is drawn bold): the icon as drawn (a
    // thumbnail's own size, for a wide or tall image less than the icon's square) and the name's lines as wrapped,
    // each centred. The highlight (selected, hovered, a drag's target) fills it, and the grid's hit tests ask it
    // (Pane's grid: indexAt, the rubber band), so the rest of the cell is the folder's empty space. A group header:
    // its whole line.
    QPainterPath shape(const QStyleOptionViewItem &option, const QModelIndex &index) const;
    int icon_size = 128;
    Animator *animator = nullptr;   // GIF / WebM playing in place of thumbnails
    // grouped (groups.h): the grid has no fixed cells (a header takes a line of its own), so each item is given this size
    // and each header this width
    QSize cell;
    int header_width = 0;
    // A name wrapped into its lines (at most two: the first broken at a natural boundary if it can be, never inside a
    // character; the rest elided), with each line's width. Kept per font, width and text (widgets.cpp says why);
    // names_measured counts the names actually measured (not found kept).
    struct Wrapped {
        QStringList lines;
        QList<int> widths;
    };
    const Wrapped &wrap(const QFont &font, const QString &text, int width) const;
    mutable int names_measured = 0;

private:
    struct Layout {
        QRect r, icon_rect, text_rect;   // the cell less its margin; the icon's square; the name's lines
        QRectF drawn;                    // the picture as drawn: a thumbnail's own size, else the icon's square
        QPixmap pm;                      // the thumbnail or animation frame (null: the theme's icon, in icon_rect)
        QPixmap anim;                    // (the frame, when one plays)
        QFont font;
        QStringList lines;
        QList<int> widths;   // each line's (horizontalAdvance)
    };
    // paint: the animation's frame too (asking for it starts it playing); a hit test goes by the thumbnail
    Layout layout(const QStyleOptionViewItem &option, const QModelIndex &index, bool paint) const;
    QPainterPath shape(const Layout &l) const;
    QStringList lines(const QFontMetrics &fm, const QString &text, int width) const;
    mutable QHash<QString, Wrapped> wrapped;
    static void play_badge(QPainter *p, const QRectF &rect);
    static void star_badge(QPainter *p, const QRectF &rect);
    static void pin_badge(QPainter *p, const QRectF &rect);
    QWidget *pane;
    ThumbnailManager *thumbs;
    std::function<bool(const QString &)> is_cut;
};

// The headers of grouped views (groups.h): an arrow (pointing right when collapsed), the group's name and count, and a line.
int group_header_height(const QFontMetrics &fm);
void paint_group_header(QPainter *p, const QRect &rect, const QModelIndex &index, const QStyleOptionViewItem &option);

// The list view's: headers drawn as above, everything else as usual.
class ListDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

// ---------------------------------------------------------------- path bar

class Crumbs : public QWidget {
    Q_OBJECT
Q_SIGNALS:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent *) override { Q_EMIT clicked(); }
};

class PathBar : public QWidget {
    Q_OBJECT
public:
    explicit PathBar(QWidget *parent = nullptr);
    void start_edit();
    void cancel_edit();
    void set_path(const QString &path);
    QString path;

Q_SIGNALS:
    void navigate(const QString &path);

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;

private:
    void commit();
    void fit(bool force = false);
    Crumbs *crumb_box;
    QHBoxLayout *crumb_lay;
    QLineEdit *edit;
    QToolButton *edit_btn;
    QToolButton *overflow;
    QList<QToolButton *> buttons;
    bool fitting = false;
    int fit_width = -1;
};

// ---------------------------------------------------------------- sidebar

// The sidebar's sections and their saved order (settings: sidebar_sections, sidebar_collapsed, sidebar_places_order,
// sidebar_devices_order; the bookmarks' order is the GTK bookmarks file's), and the places hidden (sidebar_hidden).
namespace sidebar {
extern const QStringList SECTIONS;   // "places", "bookmarks", "devices": the default order
// every entry Places can have, in its natural order: (id, name; TX_NOOP); sidebar_hidden lists the ids of hidden ones
extern const QList<QPair<QString, QString>> PLACES;
QStringList section_order(const QStringList &saved);   // the saved order, without unknown ids, plus any missing
// keys in the saved order (those present), then the rest in their natural order
QStringList ordered(const QStringList &keys, const QStringList &saved);
// keys with `key` moved to insertion point `index` (0 = first, keys.size() = last, counted before the move)
QStringList moved(QStringList keys, const QString &key, int index);
}  // namespace sidebar

class Sidebar : public QListWidget {
    Q_OBJECT
public:
    explicit Sidebar(QWidget *parent = nullptr);
    ~Sidebar() override;
    void refresh();
    void select_path(const QString &path);
    void add_bookmark(const QString &path);
    // rearranging (what drag and drop does): an entry within its section, or a whole section, to insertion point
    // `index` (counted before the move); click a header to collapse/expand it
    void move_entry(const QString &section, const QString &key, int index);
    void move_section(const QString &section, int index);
    void toggle_section(const QString &section);
    QStringList shown_sections() const;                   // in order
    QStringList entry_keys(const QString &section) const;   // in order; empty when collapsed
    void forget_server(const QString &uri);   // takes a recent server out of the Mounts section (and the Overview)
    static void hide_place(const QString &id);   // Hide from Sidebar: an id in sidebar::PLACES
    // Files dragged over an entry: the row that would take them is highlighted (drop_row, -1: none) with what a drop
    // does said under it (drop_hint: "Move to Documents", or why it can't). Resting on one for SPRING_MS opens it.
    static const int SPRING_MS = 500;
    int drop_row() const { return drop_ok ? drop_on : -1; }
    QString drop_hint() const { return hint; }

Q_SIGNALS:
    void open_path(const QString &path, bool new_tab);
    void dropped(const QStringList &sources, const QString &target, bool copy_only, int mods);   // mods: keys held
    void dropped_members(const QByteArray &members, const QString &target);   // archive::MEMBERS_MIME's data
    void empty_trash_requested();
    void find_duplicates(const QString &root);   // a drive's Find Duplicates…
    void disk_usage(const QString &root);        // a drive's Disk Usage…
    void shred_trash_requested();   // Empty Trash with BleachBit

protected:
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void dragEnterEvent(QDragEnterEvent *ev) override;
    void dragMoveEvent(QDragMoveEvent *ev) override;
    void dragLeaveEvent(QDragLeaveEvent *ev) override;
    void dropEvent(QDropEvent *ev) override;
    void paintEvent(QPaintEvent *ev) override;

private:
    struct Entry {
        Entry() = default;
        Entry(const QString &label, const QString &path, const QIcon &icon, const QString &kind = "place",
              const QVariant &extra = QVariant(), const QString &key = QString(), const QString &tip = QString())
            : label(label), path(path), icon(icon), kind(kind), extra(extra), key(key), tip(tip)
        {
        }
        QString label, path;
        QIcon icon;
        QString kind = "place";
        QVariant extra;
        QString key, tip;   // key: what the saved order lists (the path; "phone:<name>" for a phone)
        bool dim = false;   // greyed (a recent server that isn't connected)
    };
    struct Drop {
        int row = -1;            // insertion row (count() = after the last)
        QString section, key;    // what moves (key empty: the whole section)
        int index = -1;          // for move_entry / move_section
    };
    struct Mount {
        QString name, root, dev;
        qint64 total;
        bool operator==(const Mount &o) const
        {
            return name == o.name && root == o.root && dev == o.dev && total == o.total;
        }
    };
    void header(const QString &section, const QString &title, bool collapsed);
    void add(const Entry &e, const QString &section);
    QList<Entry> in_order(const QList<Entry> &entries, const QString &setting) const;
    int section_row(const QString &section) const;   // its header's row, -1 if not shown
    int section_end(const QString &section) const;   // the row after its last entry
    Drop drop_for(const QPoint &pos, const QString &section, const QString &key) const;
    void save_order(const QString &setting, const QStringList &keys);
    struct UserMount {   // the Mounts section's own mounts (drives::user_mount), not those Devices lists
        QString root, fs, source;
        bool operator==(const UserMount &o) const { return root == o.root && fs == o.fs && source == o.source; }
    };
    QList<Mount> mount_list() const;
    QList<UserMount> user_mount_list(const QList<Mount> &devices) const;
    void check_mounts();
    void unmount_share(int i);
    void clicked(QListWidgetItem *it);
    void menu(const QPoint &pos);
    void unmount(const QString &path);
    void eject_phone(int i);
    static void monitor_changed(void *, void *, void *self);
    void edit_bookmark(const QString &path);
    void remove_bookmark(int i);
    void move_bookmark(int i, int d);
    QList<Mount> mounts;
    QList<UserMount> user_mounts;
    QList<DriveInfo> phones;   // phones and cameras (GIO)
    QList<DriveInfo> shares;   // network shares mounted through GVFS (GIO)
    QTimer phone_timer;   // a change to the drives (GIO, MountWatch): the list made again, once, shortly after
    // ...and checked again a little later (check_mounts), for the steps of a mount that come after the refresh with no
    // event of their own (udev's by-label link, a mount that settles a moment later)
    QTimer settle_timer;
    int settle_step = 0;
    void drives_changed();
    GVolumeMonitor *monitor = nullptr;
    QPoint press_pos;
    QPersistentModelIndex press_index;
    int drop_line = -1;   // y of the drop indicator while rearranging, -1 = none
    // What a drop of files onto an entry does. what: "folder" (into path), "trash", "star", "connect" (a phone or
    // network place that isn't mounted: path is its URI), "ramdisk" (a RAM disk that isn't in memory: path is its
    // folder, id its id), "" (none: reason says why, or nothing for a header)
    struct Target {
        QString what, path, name, id, reason;
        qint64 dev = -1;   // the folder's drive (st_dev), -1 unknown
    };
    Target target_for(QListWidgetItem *it, const QMimeData *md);   // this drag's, worked out once per entry
    QString op_for(const Target &t, Qt::KeyboardModifiers mods, bool copy_only) const;   // move, copy, link, ask
    QString hint_for(const Target &t, const QString &op) const;
    void end_drop();   // the highlight and hint gone, the spring stopped
    QHash<QString, Target> targets;   // this drag's targets, by entry: a slow drive is asked once, not on every move
    QStringList drag_paths;           // the files dragged (local ones)
    qint64 drag_dev = -1;             // the first one's drive
    int drop_on = -1;                 // the row under the files dragged
    bool drop_ok = false;             // ...and it takes them: highlighted (otherwise only the hint, saying why not)
    QString hint;
    QTimer spring;                    // resting on an entry: opened in the pane
    QString spring_path;
};

// ---------------------------------------------------------------- info panel

class InfoPanel : public QScrollArea {
    Q_OBJECT
public:
    explicit InfoPanel(ThumbnailManager *thumbs, QWidget *parent = nullptr);
    void show_path(const QString &path);
    QString path;
    bool folder_previews = true;

protected:
    void resizeEvent(QResizeEvent *ev) override;

private:
    void set_preview();
    void clear();
    void row(const QString &k, const QString &v);
    void refresh();
    void add_details(const QList<QPair<QString, QString>> &basic, const QList<QPair<QString, QString>> &ai);
    ThumbnailManager *thumbs;
    int gen = 0;   // the details, read on a worker, are for the path shown when they were asked
    // the item, as the worker read it (nothing is asked from the window's thread: a busy drive, or a tape seeking,
    // answers only when it gets there)
    bool cached = false, cached_dir = false;
    qint64 cached_mtime = 0, cached_size = 0;
    QLabel *preview;
    QLabel *title;
    QFormLayout *form;
    QVBoxLayout *extra;
    QTimer timer;
};
