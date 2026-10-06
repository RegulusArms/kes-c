// The main window (its file actions: actions.cpp; opening files: opening.cpp). The browser pane: pane.h.
#pragma once

#include "pane.h"

#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QWidget>

class Animator;
class FSModel;
class PreferencesDialog;
class GridDelegate;
class ImageViewer;
class InfoPanel;
class OverviewPage;
class PathBar;
class RecursiveBuilder;
class SearchModel;
class SearchThread;
class Sidebar;
class ChooserBar;
namespace chooser {
struct Request;
struct Result;
}  // namespace chooser
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

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(const QStringList &paths, ThumbnailManager *thumbs, bool chooser_mode = false);
    void make_chooser(const chooser::Request &req, std::function<void(const chooser::Result &)> done);

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
    ChooserBar *chooser = nullptr;   // a file chooser window (see chooser.h)
    bool chooser_mode = false;       // a chooser window: has its own view settings (view_value)
    // the window's and its tabs' view settings (size, zoom, view, sort, panels, hidden files, folder previews, search
    // options); a chooser window keeps its own (see view_value in app.cpp)
    QVariant view_value(const QString &key, const QVariant &def = QVariant()) const;
    void set_view_value(const QString &key, const QVariant &value);
    int default_zoom(bool grid) const;

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
    void open_videos(const QStringList &videos);
    QStringList open_with_choice(const QStringList &files, const QString &choice);
    void uwp_add(const QString &path);
    void folder_style_menu(QMenu *m, const QStringList &folders);
    void close_when_idle();
    void sync_undo();
    void preferences_saved();

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
    QPointer<PreferencesDialog> prefs;   // the open Preferences window
};

QString location_arg(const QString &arg);
// The app's state and helpers that incoming.cpp shares
extern QList<MainWindow *> WINDOWS;   // this process's windows (file choosers aren't in it)
extern ThumbnailManager *g_thumbs;
void repaint_all();
void apply_preferences();   // after Preferences change, here or in another Kestrel
MainWindow *open_window(const QStringList &paths);
MainWindow *open_chooser(const chooser::Request &req, std::function<void(const chooser::Result &)> done);
void apply_thumb_settings(ThumbnailManager *t);
int kes_main(int argc, char **argv);
