// Main window and browser panes.
#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QWidget>

class FSModel;
class GridDelegate;
class ImageViewer;
class InfoPanel;
class OverviewPage;
class PathBar;
class RecursiveBuilder;
class SearchModel;
class SearchThread;
class Sidebar;
class TaskPanel;
class ThumbnailManager;
class QAction;
class QCheckBox;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QListView;
class QMenu;
class QProgressBar;
class QSlider;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QToolButton;
class QTreeView;
class QAbstractItemView;
class QAbstractItemModel;

class MainWindow;

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
    QStringList all_paths() const;
    QString current_path() const;
    void invert_selection();
    void sort_by(int col, int order = -1);
    QString title() const;
    bool searching() const;

    MainWindow *win;
    QString path;
    QList<HistoryEntry> back_stack, fwd_stack;
    bool in_search = false;
    QPointer<SearchThread> search_thread;
    QTreeView *tree;
    QListView *grid;
    int grid_size, list_size;

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
    SearchModel *search_model = nullptr;
    GridDelegate *delegate;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(const QStringList &paths, ThumbnailManager *thumbs);

    Pane *pane() const;
    QList<Pane *> panes() const;
    Pane *new_tab(const QString &path, bool activate = true);
    void close_tab(int i);
    void navigate(const QString &path);
    QString homepage() const;
    void go_home();
    QString cur_dir() const;
    void open_location(const QString &target, bool new_tab = false);
    void set_view(const QString &mode);
    void toggle_view();
    void sync_zoom_slider();
    void set_folder_previews(bool on);
    void build_previews(const QString &root);
    void stop_build();
    void toggle_hidden(bool on);
    void update_status();
    void open_paths(Pane *pane, const QStringList &paths, bool new_tab = false);
    bool open_file(const QString &path);
    void quick_view(Pane *pane, const QStringList &paths);
    void open_viewer(Pane *pane, const QStringList &images, int idx);
    void context_menu(Pane *pane, QAbstractItemView *view, const QPoint &pos);
    QMenu *build_menu(Pane *pane, const QStringList &paths);
    void reveal(const QString &path);
    void copy_text(const QStringList &items);
    void clip(bool cut, const QStringList &paths);
    QPair<QString, QStringList> read_clipboard() const;
    void paste(const QString &target = QString(), bool as_link = false);
    void handle_drop(const QStringList &paths, const QString &target);
    void transfer_to(const QStringList &paths, const QString &op);
    void duplicate(const QStringList &paths);
    void new_folder();
    void new_file(const QString &tmpl = QString());
    void rename(const QStringList &paths);
    void trash_paths(const QStringList &paths);
    void delete_paths(const QStringList &paths);
    void restore(const QStringList &paths);
    void empty_trash();
    void make_links(const QStringList &paths, QString dest, const QString &kind);
    void properties(const QStringList &paths, QWidget *parent = nullptr);
    void preferences();
    void clear_cache();
    void purge_thumbnails();
    void show_shortcuts();

    ThumbnailManager *thumbs;
    QTabWidget *tabs;
    QSet<QString> cut_paths;
    bool show_hidden;
    bool folder_previews;
    Sidebar *sidebar;
    InfoPanel *info;
    TaskPanel *task_panel;
    PathBar *pathbar;
    QCheckBox *preview_box;

protected:
    void closeEvent(QCloseEvent *ev) override;

private:
    void build_toolbar();
    void fill_sort_menu();
    QAction *act(const QString &text, const QStringList &keys, std::function<void()> fn,
                 const QStringList &icon_names = {}, bool checkable = false, QMenu *menu = nullptr);
    void build_actions();
    void tab_changed(int i);
    void pane_path_changed(Pane *pane);
    void remember_server(const QString &uri);
    void sync_view_btn();
    void build_progress(int done, int total, bool scanning, const QString &current);
    void build_finished(int done, int total, bool cancelled);
    void on_thumb_progress(int done, int total);
    void toggle_panel(QWidget *w, const QString &key, bool on);
    QStringList open_with_choice(const QStringList &files, const QString &choice);
    void uwp_add(const QString &path);
    void folder_style_menu(QMenu *m, const QStringList &folders);
    void close_when_idle();
    void sync_undo();

    QList<QPointer<ImageViewer>> viewers;
    QSplitter *split;
    QLabel *status_label, *free_label, *build_label;
    QSlider *zoom_slider;
    QProgressBar *thumb_progress, *build_bar;
    RecursiveBuilder *builder = nullptr;
    QString build_root;
    QWidget *build_box;
    QToolButton *build_stop, *view_btn, *sort_btn, *menu_btn;
    QMenu *sort_menu;
    QAction *a_back, *a_fwd, *a_up, *a_home, *a_search, *a_hidden, *a_sidebar, *a_info, *a_undo;
    bool closing = false;
};

QString location_arg(const QString &arg);
void handle_fm1(const QString &method, const QStringList &uris, const QString &startup_id);
MainWindow *open_window(const QStringList &paths);
void apply_thumb_settings(ThumbnailManager *t);
int kes_main(int argc, char **argv);
