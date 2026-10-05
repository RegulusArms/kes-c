// Models, delegates and widgets used by the main window.
#pragma once

#include "overview.h"

#include <QAbstractFileIconProvider>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMutex>
#include <QScrollArea>
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
};
enum Kind { KIND_DIR = 1, KIND_LINK = 2 };

// ---------------------------------------------------------------- models

// Type/icon lookup without touching the disk.
//
// The default provider sniffs file contents (opens every file) to name its type, and QFileSystemModel calls it on
// the UI thread - thousands of opens on a slow disk freezes the window. Icons are drawn by FSModel::data(), so
// return none here.
class FastIconProvider : public QAbstractFileIconProvider {
public:
    QIcon icon(IconType) const override { return QIcon(); }
    QIcon icon(const QFileInfo &) const override { return QIcon(); }
    QString type(const QFileInfo &fi) const override;

private:
    mutable QHash<QString, QString> types;
    mutable QMutex mutex;
};

// QFileSystemModel that serves thumbnails and folder previews.
class FSModel : public QFileSystemModel {
    Q_OBJECT
public:
    explicit FSModel(ThumbnailManager *thumbs, QObject *parent = nullptr);
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;

    int thumb_size = 128;
    bool folder_previews = true;
    std::function<void(const QStringList &, const QString &)> drop_handler;

private:
    QPixmap thumb(const QModelIndex &index) const;
    QIcon plain_icon(const QModelIndex &index) const;
    void thumb_ready(const QString &path);
    ThumbnailManager *thumbs;
    mutable QHash<QString, QIcon> suffix_icons;
};

// Results of a recursive search.
class SearchModel : public QStandardItemModel {
    Q_OBJECT
public:
    explicit SearchModel(ThumbnailManager *thumbs, QObject *parent = nullptr);
    // add result rows; `locations` optionally maps a path to the text for its Location column
    void add_paths(const QStringList &paths, const QHash<QString, QString> &locations = {});
    void clear_results();
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<QString, QStandardItem *> rows;
    int thumb_size = 128;
    bool folder_previews = true;

private:
    void thumb_ready(const QString &path);
    ThumbnailManager *thumbs;
};

class SearchThread : public QThread {
    Q_OBJECT
public:
    SearchThread(const QString &root, const QString &query, bool hidden, QObject *parent = nullptr,
                 bool contents = false);
    std::atomic<bool> stop{false};

Q_SIGNALS:
    void found(const QStringList &paths);

protected:
    void run() override;

private:
    bool match(const QString &name) const;
    void run_contents();
    QString root;
    QString query, raw_query;
    bool hidden;
    bool wildcard;
    bool contents;
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
    int icon_size = 128;
    Animator *animator = nullptr;   // GIF / WebM playing in place of thumbnails

private:
    QStringList lines(const QFontMetrics &fm, const QString &text, int width) const;
    static void play_badge(QPainter *p, const QRectF &rect);
    static void star_badge(QPainter *p, const QRectF &rect);
    QWidget *pane;
    ThumbnailManager *thumbs;
    std::function<bool(const QString &)> is_cut;
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
// sidebar_devices_order; the bookmarks' order is the GTK bookmarks file's).
namespace sidebar {
extern const QStringList SECTIONS;   // "places", "bookmarks", "devices": the default order
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

Q_SIGNALS:
    void open_path(const QString &path, bool new_tab);
    void dropped(const QStringList &sources, const QString &target);
    void empty_trash_requested();

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
    QList<Mount> mount_list() const;
    void check_mounts();
    void clicked(QListWidgetItem *it);
    void menu(const QPoint &pos);
    void unmount(const QString &path);
    void eject_phone(int i);
    static void monitor_changed(void *, void *, void *self);
    void edit_bookmark(const QString &path);
    void remove_bookmark(int i);
    void move_bookmark(int i, int d);
    QList<Mount> mounts;
    QList<DriveInfo> phones;   // phones and cameras (GIO)
    QTimer timer, phone_timer;
    GVolumeMonitor *monitor = nullptr;
    QPoint press_pos;
    QPersistentModelIndex press_index;
    int drop_line = -1;   // y of the drop indicator while rearranging, -1 = none
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
    ThumbnailManager *thumbs;
    QLabel *preview;
    QLabel *title;
    QFormLayout *form;
    QVBoxLayout *extra;
    QTimer timer;
};
