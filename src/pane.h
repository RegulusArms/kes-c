// A browser pane: one tab's view of a folder (grid or list), the overview page, the combined trash, Starred and Recent,
// search, history and selection.
#pragma once

#include "places.h"

#include <QPointer>
#include <QWidget>

class Animator;
class DateGroupProxy;
class FSModel;
class GridDelegate;
class MainWindow;
class OverviewPage;
class SearchModel;
class SearchThread;
class PinSortProxy;
class ThumbnailManager;
class QAbstractItemModel;
class QModelIndex;
class QAbstractItemView;
class QCheckBox;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QListView;
class QStackedWidget;
class QTimer;
class QTreeView;

// zoom limits: icon size in the grid, row height in the list
constexpr int GRID_MIN = 48, GRID_MAX = 320;
constexpr int LIST_MIN = 16, LIST_MAX = 128;

class Pane : public QWidget {
    Q_OBJECT
public:
    Pane(MainWindow *win, const QString &path);
    ~Pane() override;

    struct HistoryEntry {
        QString path, focus;
        bool has_search = false;
        QString search_text;
        bool search_recursive = false;
    };

    bool is_grid() const;
    QAbstractItemView *view() const;
    bool is_overview() const;
    bool is_trash() const;
    bool is_virtual() const;   // Starred or Recent: files from anywhere, not a folder
    bool is_listing() const;   // a list of files from many folders (Trash, Starred, Recent) in the results model
    QString dir() const;   // the current folder, or empty on the overview page
    void set_view_mode(const QString &mode);
    void apply_folder_previews();
    void zoom(int step, int absolute = -1);
    int zoom_value() const;
    void set_sizes(int grid, int list);   // both zooms (Preferences); saved by whoever changed them
    void set_search_options(bool subfolders, bool contents);   // the search bar's switches (Preferences)
    void update_grid_size();
    bool set_path(const QString &path, bool record = true, const QString &select = QString());
    void show_trash();
    void try_select();
    void select_later(const QString &path);
    void select_paths(const QStringList &paths);
    void scroll_to_current(const QString &path);
    void go_back();
    void go_forward();
    void go_up();
    void refresh();
    void apply_hidden();
    void start_search();
    void close_search(bool refocus = true, bool navigating = false);
    void stop_search();
    QStringList selected_paths() const;
    // the selected items as the view knows them (kind: KindRole's bits, -1 if it doesn't know; size: -1 likewise),
    // without asking the file system: on a busy tape, a stat waits until the drive gets there
    struct Known {
        QString path;
        int kind = -1;
        qint64 size = -1;
    };
    QList<Known> selected_known() const;
    QStringList all_paths() const;
    QString current_path() const;
    void invert_selection();
    void sort_by(int col, int order = -1);   // for this folder (or place) only
    void apply_folder_sort();   // show the folder's own sort, or the default when it has none
    int grouping() const { return group_mode; }     // groups::Mode
    int grouping_by() const { return group_by; }   // groups::DateBy
    void set_grouping(int mode, int by);   // for this folder (or place) only, like its sort
    void set_pinned_first(bool on);   // pinned and starred items first
    bool pinned_first() const;
    QString title() const;
    bool searching() const;
    void set_type_filter(const QStringList &globs);   // chooser: show folders and only these files

    MainWindow *win;
    QString path;
    QList<HistoryEntry> back_stack, fwd_stack;
    bool in_search = false;
    QPointer<SearchThread> search_thread;
    QTreeView *tree;
    QListView *grid;
    int grid_size, list_size;
    Animator *animator;   // GIF / WebM playing in the grid

Q_SIGNALS:
    void path_changed();
    void selection_changed();

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void setup_common(QAbstractItemView *v);
    void setup_grid();
    void setup_tree();
    FSModel *make_model();
    void attach(QAbstractItemModel *model);
    void set_root(const QModelIndex &root);   // the folder shown, in the attached model
    void apply_grouping(int mode, int by);
    void span_headers();
    QModelIndex source_index_for(const QString &p) const;   // in the attached model
    places::FolderSort folder_sort(const QString &p) const;   // its own, or the default
    void fill_trash(const QVariant &items, int gen);
    void add_trash_rows(QStringList paths, const QHash<QString, QString> &locations, int gen, const QStringList &sel);
    void trash_changed();
    void dir_loaded(const QString &p);
    QModelIndex index_for(const QString &p) const;
    HistoryEntry snapshot(const QString &dest = QString(), bool search = true) const;
    void restore(const HistoryEntry &entry);
    void record_search();
    void unrecord_search();
    void do_search();
    void update_empty();
    void sort_changed(int col, Qt::SortOrder order);
    void clicked(const QModelIndex &idx);
    void double_clicked(const QModelIndex &idx);
    void use_model_root();
    void starred_changed();

    ThumbnailManager *thumbs;
    QAbstractItemModel *attached = nullptr;   // the folder's (proxy) or the results' (search_model)
    bool grouped = false;   // group_mode isn't groups::NONE
    int group_mode = 0, group_by = 0;
    bool applying_sort = false;     // apply_folder_sort is setting the header: not a sort to remember
    bool search_recorded = false;   // the folder as it was before the active search is on back_stack
    QString pending_select;
    int trash_gen = 0;              // bumps on every combined-trash reload, so stale loads are dropped
    QFileSystemWatcher *trash_watch = nullptr;
    bool trash_reload = false;
    QWidget *search_bar;
    QLineEdit *search_edit;
    QCheckBox *search_sub;
    QCheckBox *search_contents;
    QTimer *search_timer;
    QStackedWidget *stack;
    QAbstractItemView *mode_view;   // grid or tree; stays set while the overview page is shown
    OverviewPage *overview;
    QLabel *empty;
    FSModel *model = nullptr;
    DateGroupProxy *grouper = nullptr;   // between the model attached and the views while grouped by date (groups.h)
    PinSortProxy *proxy = nullptr;   // between model and the views (PinSortProxy)
    SearchModel *search_model = nullptr;
    GridDelegate *delegate;
    QStringList type_filters;   // chooser: the chosen file type's globs
    qint64 listing_since = -1;  // KESTREL_STATS: when the folder now shown started loading
};
