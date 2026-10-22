#include "app.h"

#include "admin.h"
#include "animate.h"
#include "archive.h"
#include "archive_ui.h"
#include "atc.h"
#include "chooser.h"
#include "dialogs.h"
#include "drives.h"
#include "fileops.h"
#include "hashcheck.h"
#include "fm1.h"
#include "groups.h"
#include "incoming.h"
#include "jobs.h"
#include "jobswin.h"
#include "overview.h"
#include "menus.h"
#include "places.h"
#include "prefswin.h"
#include "prefs.h"
#include "sharing.h"
#include "undo.h"
#include "proc.h"
#include "thumbs.h"
#include "towerwin.h"
#include "stats.h"
#include "util.h"
#include "uwp.h"
#include "viewer.h"
#include "windiag.h"
#include "widgets.h"
#include "windows.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTabBar>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWindow>

#include <csignal>
#include <cstdio>
#include <cstdlib>

using namespace util;

static const int GRID_DEFAULT = 160, LIST_DEFAULT = 28;
static const int CHOOSER_GRID = 96, CHOOSER_LIST = 24;   // a chooser window starts with smaller icons
static const QStringList SORT_COLUMNS = {"Name", "Size", "Type", "Modified"};
QList<MainWindow *> WINDOWS;
ThumbnailManager *g_thumbs = nullptr;

static QIcon icon(const QStringList &names) { return theme_icon(names); }

// ---------------------------------------------------------------- main window

MainWindow::MainWindow(const QStringList &paths, ThumbnailManager *thumbs_, bool chooser_mode_)
    : thumbs(thumbs_), chooser_mode(chooser_mode_)
{
    setAttribute(Qt::WA_DeleteOnClose);
    show_hidden = view_value("show_hidden", false).toBool();
    folder_previews = view_value("folder_previews", true).toBool();
    setWindowTitle(APP_NAME);
    setWindowIcon(app_icon());
    if (chooser_mode)
        resize(960, 620);   // a dialog: smaller than a main window
    else
        resize(1280, 820);

    build_toolbar();
    sidebar = new Sidebar;
    connect(sidebar, &Sidebar::open_path, this, [this](const QString &p, bool nt) { open_location(p, nt); });
    connect(sidebar, &Sidebar::dropped, this, &MainWindow::handle_drop);
    connect(sidebar, &Sidebar::empty_trash_requested, this, &MainWindow::empty_trash);
    connect(sidebar, &Sidebar::shred_trash_requested, this, &MainWindow::empty_trash_with_bleachbit);
    tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    tabs->setTabsClosable(true);
    tabs->setMovable(true);
    tabs->setTabBarAutoHide(true);
    connect(tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::close_tab);
    connect(tabs, &QTabWidget::currentChanged, this, &MainWindow::tab_changed);
    tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs->tabBar(), &QWidget::customContextMenuRequested, this, &MainWindow::tab_menu);
    info = new InfoPanel(thumbs);
    info->folder_previews = folder_previews;
    split = new QSplitter;
    split->addWidget(sidebar);
    split->addWidget(tabs);
    split->addWidget(info);
    split->setStretchFactor(1, 1);
    split->setSizes({220, 900, 300});
    split->setCollapsible(1, false);
    setCentralWidget(split);
    sidebar->setVisible(view_value("sidebar", true).toBool());
    info->setVisible(view_value("info_panel", false).toBool());

    status_label = new QLabel;
    free_label = new QLabel;
    connect(drives::Sizes::instance(), &drives::Sizes::changed, this, [this](const QString &root) {
        if (root == free_root)
            update_free_label();
    });
    // the free space is asked when a folder is opened and when a job has ended (it may have changed it), not on a timer
    connect(TaskBoard::instance(), &TaskBoard::changed, this, [this]() {
        qsizetype n = TaskBoard::instance()->finished.size();
        if (n != finished_seen) {
            finished_seen = n;
            update_free_label();
        }
    });
    zoom_slider = new QSlider(Qt::Horizontal);
    zoom_slider->setFixedWidth(140);
    zoom_slider->setToolTip("Zoom (Ctrl+scroll)");
    zoom_slider->setAccessibleName("Zoom");
    connect(zoom_slider, &QSlider::valueChanged, this, [this](int v) {
        if (pane() && pane()->zoom_value() != v)
            pane()->zoom(0, v);
    });
    thumb_progress = new QProgressBar;
    thumb_progress->setFixedWidth(220);
    thumb_progress->setMaximumHeight(16);
    thumb_progress->setFormat("Thumbnails %v / %m");
    thumb_progress->setToolTip("Generating thumbnails and folder previews in the background");
    QSizePolicy keep = thumb_progress->sizePolicy();
    keep.setRetainSizeWhenHidden(true);   // showing/hiding must not shift the layout
    thumb_progress->setSizePolicy(keep);
    thumb_progress->hide();
    connect(thumbs, &ThumbnailManager::progress, this, &MainWindow::on_thumb_progress);
    task_panel = new TaskPanel;   // file operations running in the background
    connect(task_panel, &TaskPanel::clicked, this, [this]() { JobsWindow::open(this); });
    setProperty("task_panel", QVariant::fromValue<QObject *>(task_panel));
    auto *admin_indicator = new admin::Indicator;   // 🛡 while an admin session is open
    statusBar()->addWidget(status_label, 1);
    statusBar()->addPermanentWidget(admin_indicator);
    statusBar()->addPermanentWidget(task_panel);
    statusBar()->addPermanentWidget(thumb_progress);
    statusBar()->addPermanentWidget(free_label);
    statusBar()->addPermanentWidget(zoom_slider);

    build_actions();
    QVariant geo = view_value("geometry");
    if (geo.isValid())
        restoreGeometry(geo.toByteArray());
    QVariant st = view_value("splitter");
    if (st.isValid())
        split->restoreState(st.toByteArray());
    QStringList start = paths.isEmpty() ? QStringList{homepage()} : paths;
    for (const QString &p : start)
        open_location(p, true);
    if (!tabs->count())   // e.g. the homepage was an unreachable network share
        new_tab(OVERVIEW);
}

void MainWindow::make_chooser(const chooser::Request &req, std::function<void(const chooser::Result &)> done)
{
    // a file chooser window: a normal window with the chooser's bar at the bottom (see chooser.h)
    chooser = new ChooserBar(this, req, std::move(done));
    auto *box = new QWidget;
    auto *lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    takeCentralWidget();
    lay->addWidget(split, 1);
    lay->addWidget(chooser);
    setCentralWidget(box);
    setWindowTitle(req.title.isEmpty() ? chooser::button_text(req) : req.title);
    for (Pane *p : panes()) {
        p->set_type_filter(chooser->type_filter());
        p->overview_page()->set_save_places(chooser->saving());   // a Save window's Overview: its save locations first
    }
    chooser->folder_changed();   // the suggested name made free in the folder it starts in
    if (chooser->name)
        chooser->name->setFocus();
}

// A file chooser window keeps its own view settings, under "chooser/": changing the zoom, view, sort, panels and so on
// there leaves the main windows alone, and the next chooser starts from them. Until changed in a chooser they follow
// the main windows', apart from the size (never the main windows' maximized one) and the smaller icons.
QVariant MainWindow::view_value(const QString &key, const QVariant &def) const
{
    QSettings &s = settings();
    if (chooser_mode && s.contains("chooser/" + key))
        return s.value("chooser/" + key);
    if (key == "grid_size" || key == "list_size") {
        bool grid = key == "grid_size";
        return chooser_mode ? default_zoom(grid) : s.value(key, default_zoom(grid));
    }
    if (chooser_mode && (key == "geometry" || key == "splitter"))
        return QVariant();
    return s.value(key, def);
}

void MainWindow::set_view_value(const QString &key, const QVariant &value)
{
    QString k = chooser_mode ? "chooser/" + key : key;
    settings().setValue(k, value);
    prefs::notify({k});   // an open Preferences window shows it
}

int MainWindow::default_zoom(bool grid) const
{
    if (chooser_mode)
        return grid ? CHOOSER_GRID : CHOOSER_LIST;
    return grid ? GRID_DEFAULT : LIST_DEFAULT;
}

// -- toolbar & actions

void MainWindow::build_toolbar()
{
    auto *tb = new QToolBar;
    tb->setMovable(false);
    tb->setIconSize(QSize(18, 18));
    addToolBar(tb);
    a_back = tb->addAction(icon({"go-previous-symbolic", "go-previous"}), "Back", this, [this]() { pane()->go_back(); });
    a_fwd = tb->addAction(icon({"go-next-symbolic", "go-next"}), "Forward", this, [this]() { pane()->go_forward(); });
    a_up = tb->addAction(icon({"go-up-symbolic", "go-up"}), "Parent Folder", this, [this]() { pane()->go_up(); });
    a_home = tb->addAction(icon({"go-home-symbolic", "go-home"}), "Homepage (Alt+Home)", this, &MainWindow::go_home);
    pathbar = new PathBar;
    connect(pathbar, &PathBar::navigate, this, &MainWindow::navigate);
    tb->addWidget(pathbar);
    preview_box = new QCheckBox("Folder previews");
    preview_box->setToolTip("Show image mosaics on folder icons (Ctrl+Shift+P).\n"
                            "Turn off in large or slow folders to speed things up.");
    preview_box->setChecked(folder_previews);
    connect(preview_box, &QCheckBox::toggled, this, [this](bool on) { set_folder_previews(on); });
    tb->addWidget(preview_box);
    a_search = tb->addAction(icon({"system-search-symbolic", "edit-find"}), "Search (Ctrl+F)", this,
                             [this]() { pane()->start_search(); });
    view_btn = new QToolButton;
    view_btn->setAutoRaise(true);   // its name and tooltip follow the view (sync_view_btn)
    connect(view_btn, &QToolButton::clicked, this, &MainWindow::toggle_view);
    tb->addWidget(view_btn);
    sort_btn = new QToolButton;
    sort_btn->setAutoRaise(true);
    sort_btn->setIcon(icon({"view-sort-ascending-symbolic", "view-sort-ascending"}));
    sort_btn->setToolTip("Sort");
    sort_btn->setAccessibleName("Sort");
    sort_btn->setPopupMode(QToolButton::InstantPopup);
    sort_menu = new QMenu(this);
    connect(sort_menu, &QMenu::aboutToShow, this, &MainWindow::fill_sort_menu);
    sort_btn->setMenu(sort_menu);
    tb->addWidget(sort_btn);
    menu_btn = new QToolButton;
    menu_btn->setAutoRaise(true);
    menu_btn->setIcon(icon({"open-menu-symbolic", "application-menu", "preferences-system"}));
    menu_btn->setToolTip("Menu");
    menu_btn->setAccessibleName("Menu");
    menu_btn->setPopupMode(QToolButton::InstantPopup);
    tb->addWidget(menu_btn);
}

void MainWindow::fill_sort_menu()
{
    sort_menu->clear();
    add_sort_items(sort_menu, pane());
}

void MainWindow::add_sort_items(QMenu *m, Pane *p)
{
    int col = p->tree->header()->sortIndicatorSection();
    Qt::SortOrder order = p->tree->header()->sortIndicatorOrder();
    for (int i = 0; i < SORT_COLUMNS.size(); ++i) {
        QAction *a = m->addAction(SORT_COLUMNS[i], this, [p, i]() { p->sort_by(i); });
        a->setCheckable(true);
        a->setChecked(i == col);
    }
    m->addSeparator();
    for (auto [o, name] : {std::pair{Qt::AscendingOrder, "Ascending"}, std::pair{Qt::DescendingOrder, "Descending"}}) {
        Qt::SortOrder oo = o;
        QAction *a = m->addAction(name, this, [p, oo]() { p->sort_by(p->tree->header()->sortIndicatorSection(), oo); });
        a->setCheckable(true);
        a->setChecked(o == order);
    }
    m->addSeparator();
    QAction *a = m->addAction("Pinned && Starred First", this, [this, p]() { set_pinned_first(!p->pinned_first()); });
    a->setCheckable(true);
    a->setChecked(p->pinned_first());
    QPointer<Pane> pp(p);
    m->addSeparator();
    // grouping: by period or by day (choosing the one that's on turns it off), and which date
    for (auto [mode, name] : {std::pair{groups::PERIOD, "Group by Period"}, std::pair{groups::DAY, "Group by Day"}}) {
        int md = mode;
        QAction *g = m->addAction(name, this, [pp, md]() {
            if (pp)
                pp->set_grouping(pp->grouping() == md ? groups::NONE : md, pp->grouping_by());
        });
        g->setCheckable(true);
        g->setChecked(p->grouping() == mode);
        g->setEnabled(!p->is_overview());
    }
    QMenu *dm = m->addMenu("Date to Group By");
    dm->setEnabled(!p->is_overview());
    for (auto [by, name] : {std::pair{groups::MODIFIED, "Date Modified"}, std::pair{groups::CREATED, "Date Created"},
                            std::pair{groups::TAKEN, "Date Taken (Metadata)"}}) {
        int b = by;
        QAction *d = dm->addAction(name, this, [pp, b]() {
            if (pp)
                pp->set_grouping(pp->grouping(), b);
        });
        d->setCheckable(true);
        d->setChecked(p->grouping_by() == by);
        if (by == groups::TAKEN)
            d->setToolTip("When a photo or video was taken, from its metadata; the date created when it has none");
    }
    dm->setToolTipsVisible(true);
    if (p->grouping() != groups::NONE) {   // also on empty space's right-click menu
        m->addAction("Collapse All Groups", this, [pp]() {
            if (pp)
                pp->set_all_groups_collapsed(true);
        });
        m->addAction("Expand All Groups", this, [pp]() {
            if (pp)
                pp->set_all_groups_collapsed(false);
        });
    }
    m->addSeparator();
    m->addAction("Set as Default", this, [this, pp]() {
        if (pp)
            set_default_sort(pp);
    });
    m->addAction("Apply to Subfolders", this, [this, pp]() {
        if (pp)
            sort_subfolders(pp);
    })->setEnabled(!p->dir().isEmpty() && !p->is_trash());
}

// The sort of folders that haven't been given their own.
void MainWindow::set_default_sort(Pane *p)
{
    set_view_value("sort_col", p->tree->header()->sortIndicatorSection());
    set_view_value("sort_order", int(p->tree->header()->sortIndicatorOrder()));
    set_view_value("sort_group", p->grouping());
    set_view_value("sort_group_by", p->grouping_by());
    for (MainWindow *w : WINDOWS)
        for (Pane *q : w->panes())
            q->apply_folder_sort();
    statusBar()->showMessage("Folders without a sort of their own now use this one", 5000);
}

// Copy the folder's sort to every folder below it (replacing theirs). The walk runs on a thread; nothing is changed
// until it has finished, so a cancel leaves every folder as it was.
void MainWindow::sort_subfolders(Pane *p)
{
    QString root = p->dir();
    if (root.isEmpty())
        return;
    places::FolderSort sort{p->tree->header()->sortIndicatorSection(), int(p->tree->header()->sortIndicatorOrder()),
                            p->grouping(), p->grouping_by()};
    auto work = [root](Task *task) -> QVariant {
        QStringList found;
        walk(
            root,
            [&](const QString &dir, QStringList &dirs, QStringList &) {
                task->check();
                for (const QString &d : dirs)
                    found << join(dir, d);
                return true;
            },
            true, true);
        return found;
    };
    auto done = [this, root, sort](const QVariant &v) {
        if (!v.isValid())
            return;   // cancelled
        QStringList dirs = v.toStringList();
        places::set_folder_sort(dirs + QStringList{root}, sort);
        statusBar()->showMessage(dirs.size() == 1 ? QString("Sort applied to 1 subfolder")
                                                  : QString("Sort applied to %1 subfolders").arg(group_digits(dirs.size())),
                                 5000);
    };
    fileops::run_job(this, "Applying sort to subfolders", work, done);
}

void MainWindow::set_pinned_first(bool on)
{
    set_view_value("pinned_first", on);
    for (Pane *p : panes())
        p->set_pinned_first(on);
}

QAction *MainWindow::act(const QString &text, const QStringList &keys, std::function<void()> fn,
                         const QStringList &icon_names, bool checkable, QMenu *menu)
{
    auto *a = new QAction(text, this);
    QList<QKeySequence> seqs;
    for (const QString &k : keys)
        seqs << QKeySequence(k);
    if (!seqs.isEmpty())
        a->setShortcuts(seqs);
    if (!icon_names.isEmpty())
        a->setIcon(icon(icon_names));
    a->setCheckable(checkable);
    connect(a, &QAction::triggered, this, fn);
    addAction(a);
    if (menu)
        menu->addAction(a);
    return a;
}

void MainWindow::build_actions()
{
    auto *menu = new QMenu(this);
    menu_btn->setMenu(menu);
    auto A = [this](const QString &text, const QStringList &keys, std::function<void()> fn, QMenu *m,
                    const QStringList &icons = {}, bool checkable = false) {
        return act(text, keys, std::move(fn), icons, checkable, m);
    };
    QMenu *fm = menu->addMenu("File");
    A("New Window", {"Ctrl+N"}, [this]() { open_window({pane()->path}); }, fm);
    A("New Tab", {"Ctrl+T"}, [this]() { new_tab(pane()->path); }, fm);
    A("Close Tab", {"Ctrl+W"}, [this]() { close_tab(tabs->currentIndex()); }, fm);
    fm->addSeparator();
    A("New Folder…", {"Ctrl+Shift+N"}, [this]() { new_folder(); }, fm, {"folder-new"});
    A("New Empty File…", {"Ctrl+Alt+N"}, [this]() { new_file(); }, fm, {"document-new"});
    A("Open Terminal Here", {"Ctrl+Alt+T"},
      [this]() {
          if (!cur_dir().isEmpty())
              open_terminal(cur_dir());
      },
      fm, {"utilities-terminal"});
    fm->addSeparator();
    A("Bookmark This Location", {"Ctrl+D"},
      [this]() {
          if (!cur_dir().isEmpty())
              sidebar->add_bookmark(cur_dir());
      },
      fm, {"bookmark-new"});
    A("Properties of This Folder", {},
      [this]() {
          if (!cur_dir().isEmpty())
              properties({cur_dir()});
      },
      fm);
    fm->addSeparator();
    A("Quit", {"Ctrl+Q"}, []() { QApplication::quit(); }, fm);

    QMenu *em = menu->addMenu("Edit");
    a_undo = A("Undo", {"Ctrl+Z"}, [this]() { undo::undo(this); }, em, {"edit-undo"});
    connect(undo::signals_(), &undo::Signals::changed, this, &MainWindow::sync_undo);
    sync_undo();
    em->addSeparator();
    A("Cut", {"Ctrl+X"}, [this]() { clip(true, pane()->selected_paths()); }, em, {"edit-cut"});
    A("Copy", {"Ctrl+C"}, [this]() { clip(false, pane()->selected_paths()); }, em, {"edit-copy"});
    A("Paste", {"Ctrl+V"}, [this]() { paste(); }, em, {"edit-paste"});
    A("Paste as Link", {"Ctrl+Shift+V"}, [this]() { paste(QString(), true); }, em);
    em->addSeparator();
    A("Select All", {"Ctrl+A"}, [this]() { pane()->view()->selectAll(); }, em);
    A("Invert Selection", {"Ctrl+Shift+I"}, [this]() { pane()->invert_selection(); }, em);
    em->addSeparator();
    A("Rename…", {"F2"}, [this]() { rename(pane()->selected_paths()); }, em);
    A("Duplicate", {"Ctrl+Shift+D"}, [this]() { duplicate(pane()->selected_paths()); }, em);
    A("Move to Trash", {"Delete"}, [this]() { trash_paths(pane()->selected_paths()); }, em, {"user-trash"});
    A("Delete Permanently", {"Shift+Delete"}, [this]() { delete_paths(pane()->selected_paths()); }, em);
    A("Copy Path", {"Ctrl+Shift+C"},
      [this]() {
          if (cur_dir().isEmpty())
              return;
          QStringList sel = pane()->selected_paths();
          copy_text(sel.isEmpty() ? QStringList{cur_dir()} : sel);
      },
      em);
    em->addSeparator();
    A("Properties", {"Alt+Return", "Ctrl+I"},
      [this]() {
          QStringList sel = pane()->selected_paths();
          if (sel.isEmpty() && !cur_dir().isEmpty())
              sel = {cur_dir()};
          properties(sel);
      },
      em, {"document-properties"});

    QMenu *vm = menu->addMenu("View");
    A("Grid View", {"Ctrl+1"}, [this]() { set_view("grid"); }, vm);
    A("List View", {"Ctrl+2"}, [this]() { set_view("list"); }, vm);
    vm->addSeparator();
    A("Zoom In", {"Ctrl++", "Ctrl+="}, [this]() { pane()->zoom(1); }, vm);
    A("Zoom Out", {"Ctrl+-"}, [this]() { pane()->zoom(-1); }, vm);
    A("Reset Zoom", {"Ctrl+0"}, [this]() { pane()->zoom(0, default_zoom(pane()->is_grid())); }, vm);
    vm->addSeparator();
    A("Toggle Folder Previews", {"Ctrl+Shift+P"}, [this]() { preview_box->toggle(); }, vm);
    a_hidden = A("Show Hidden Files", {"Ctrl+H"}, [this]() { toggle_hidden(a_hidden->isChecked()); }, vm, {}, true);
    a_hidden->setChecked(show_hidden);
    a_sidebar = A("Sidebar", {"F9"}, [this]() { toggle_panel(sidebar, "sidebar", a_sidebar->isChecked()); }, vm, {}, true);
    a_sidebar->setChecked(sidebar->isVisible());
    a_info = A("Info Panel", {"F3"}, [this]() { toggle_panel(info, "info_panel", a_info->isChecked()); }, vm, {}, true);
    a_info->setChecked(info->isVisible());
    A("Fullscreen", {"F11"},
      [this]() {
          if (isFullScreen())
              showNormal();
          else
              showFullScreen();
      },
      vm);
    A("Reload", {"F5", "Ctrl+R"}, [this]() { pane()->refresh(); }, vm, {"view-refresh"});

    QMenu *gm = menu->addMenu("Go");
    A("Back", {"Alt+Left", "Backspace"}, [this]() { pane()->go_back(); }, gm);
    A("Forward", {"Alt+Right"}, [this]() { pane()->go_forward(); }, gm);
    A("Parent Folder", {"Alt+Up"}, [this]() { pane()->go_up(); }, gm);
    A("Open Selected", {"Alt+Down"}, [this]() { open_paths(pane(), pane()->selected_paths()); }, gm);
    A("Homepage", {"Alt+Home"}, [this]() { go_home(); }, gm);
    A("Home Folder", {}, [this]() { navigate(HOME()); }, gm);
    A("Overview (Drives && Bookmarks)", {}, [this]() { navigate(OVERVIEW); }, gm);
    A("Enter Location…", {"Ctrl+L", "F6"}, [this]() { pathbar->start_edit(); }, gm);
    A("Search", {"Ctrl+F"}, [this]() { pane()->start_search(); }, gm);
    A("Next Tab", {"Ctrl+PgDown", "Ctrl+Tab"},
      [this]() { tabs->setCurrentIndex((tabs->currentIndex() + 1) % tabs->count()); }, gm);
    A("Previous Tab", {"Ctrl+PgUp", "Ctrl+Shift+Tab"},
      [this]() { tabs->setCurrentIndex((tabs->currentIndex() - 1 + tabs->count()) % tabs->count()); }, gm);

    menu->addSeparator();
    A("Preferences…", {"Ctrl+,"}, [this]() { preferences(); }, menu, {"preferences-system"});
    A("Jobs…", {"Ctrl+Shift+J"}, [this]() { JobsWindow::open(this); }, menu);
    a_tower = A("Tower…", {}, [this]() { TowerWindow::open(this); }, menu);
    a_tower->setVisible(settings().value("developer_tools", false).toBool());
    A("Start Admin Session…", {}, [this]() { admin::start_session(this); }, menu, {"security-high", "dialog-password"});
    A("Clear Folder Preview Cache", {}, [this]() { clear_cache(); }, menu);
    A("Delete All Thumbnails…", {}, [this]() { purge_thumbnails(); }, menu, {"edit-clear-all", "edit-delete"});
    A("Keyboard Shortcuts", {"F1"}, [this]() { show_shortcuts(); }, menu);
    A("About", {},
      [this]() {
          QMessageBox *box = tell(this, QMessageBox::NoIcon, QString("About ") + APP_NAME,
                                  QString("<b>%1</b> %2<br>A lightweight image-gallery-oriented file manager.")
                                      .arg(APP_NAME, VERSION));
          box->setIconPixmap(windowIcon().pixmap(64));
      },
      menu);
}

// -- tabs

Pane *MainWindow::pane() const { return qobject_cast<Pane *>(tabs->currentWidget()); }

QList<Pane *> MainWindow::panes() const
{
    QList<Pane *> out;
    for (int i = 0; i < tabs->count(); ++i)
        out << qobject_cast<Pane *>(tabs->widget(i));
    return out;
}

Pane *MainWindow::new_tab(const QString &path, bool activate)
{
    auto *p = new Pane(this, path);
    if (p->path.isEmpty()) {
        p->deleteLater();
        return nullptr;
    }
    int i = tabs->addTab(p, p->title());
    connect(p, &Pane::path_changed, this, [this, p]() { pane_path_changed(p); });
    connect(p, &Pane::selection_changed, this, [this, p]() {
        if (p == pane()) {
            update_status();
            if (chooser)
                chooser->selection_changed();
        }
    });
    if (activate) {
        tabs->setCurrentIndex(i);
        p->view()->setFocus();
    }
    return p;
}

void MainWindow::close_tab(int i)
{
    if (tabs->count() <= 1) {
        close();
        return;
    }
    auto *w = qobject_cast<Pane *>(tabs->widget(i));
    w->stop_search();
    tabs->removeTab(i);
    w->deleteLater();
}

// A copy of tab i beside it, with its history.
void MainWindow::duplicate_tab(int i)
{
    auto *src = qobject_cast<Pane *>(tabs->widget(i));
    Pane *p = src ? new_tab(src->path, false) : nullptr;
    if (!p)
        return;
    p->back_stack = src->back_stack;
    p->fwd_stack = src->fwd_stack;
    tabs->tabBar()->moveTab(tabs->indexOf(p), i + 1);
    tabs->setCurrentWidget(p);
    pane_path_changed(p);   // the back/forward buttons
}

// The menu of tab i (right-click on the tab). The caller deletes it.
QMenu *MainWindow::build_tab_menu(int i)
{
    auto *m = new QMenu(this);
    m->addAction("Duplicate Tab", this, [this, i]() { duplicate_tab(i); });
    if (!chooser_mode)
        m->addAction("Open in New Window", this, [this, i]() {
            if (auto *p = qobject_cast<Pane *>(tabs->widget(i))) {
                open_window({p->path});
                close_tab(i);
            }
        });
    m->addSeparator();
    m->addAction("Close Tab", this, [this, i]() { close_tab(i); });
    return m;
}

void MainWindow::tab_menu(const QPoint &pos)
{
    int i = tabs->tabBar()->tabAt(pos);
    if (i < 0)
        return;
    QMenu *m = build_tab_menu(i);
    m->exec(tabs->tabBar()->mapToGlobal(pos));
    delete m;
}

void MainWindow::tab_changed(int)
{
    if (pane()) {
        pane_path_changed(pane());
        pane()->view()->setFocus();
    }
}

// The free space of the drive shown, from drives::Sizes: statfs can wait on the drive (a phone behind its transfers, a
// tape behind another program's reads), so it's read on a worker and shown when it comes.
void MainWindow::update_free_label()
{
    QString d = pane() ? pane()->dir() : QString();
    QString root = d.isEmpty() || is_device_path(d) ? QString() : drives::mount_root(d);
    drives::Sizes::Info info = root.isEmpty() ? drives::Sizes::Info() : drives::Sizes::instance()->get(root);
    free_label->setText(info.free >= 0 ? human_size(info.free) + " free" : QString());
    free_root = root;
}

void MainWindow::pane_path_changed(Pane *p)
{
    int i = tabs->indexOf(p);
    if (i >= 0) {
        tabs->setTabText(i, p->title());
        tabs->setTabToolTip(i, p->path);
    }
    if (p != pane())
        return;
    pathbar->set_path(p->path);
    sidebar->select_path(p->path);
    setWindowTitle(QString("%1 — %2").arg(p->title(), APP_NAME));
    a_back->setEnabled(!p->back_stack.isEmpty());
    a_fwd->setEnabled(!p->fwd_stack.isEmpty());
    a_up->setEnabled(!p->is_overview() && !p->is_virtual() && p->path != "/");
    sync_view_btn();
    sync_zoom_slider();
    update_free_label();
    update_status();
    if (chooser)   // Save needs a folder, and the names a free one in each: re-checked in each place
        chooser->folder_changed();
}

void MainWindow::navigate(const QString &path) { open_location(path); }

// -- locations: folders, the overview page, network URIs

QString MainWindow::homepage() const
{
    QString hp = settings().value("homepage", "overview").toString();
    if (hp == "overview")
        return OVERVIEW;
    if (hp == "home")
        return HOME();
    return hp;
}

void MainWindow::go_home() { navigate(homepage()); }

QString MainWindow::cur_dir() const { return pane() ? pane()->dir() : QString(); }

void MainWindow::open_location(const QString &target_in, bool new_tab_)
{
    QString target = target_in;
    if (target.isEmpty())
        return;
    if (target.startsWith("file://"))
        target = uri_to_path(target);
    if (is_uri(target)) {
        statusBar()->showMessage(QString("Connecting to %1…").arg(target));
        QPointer<MainWindow> self(this);
        overview::mount_uri(this, target, [self, target, new_tab_](const QString &path, const QString &err) {
            if (!self)
                return;
            self->statusBar()->clearMessage();
            if (!err.isEmpty()) {
                QString scheme = target.section(':', 0, 0);
                QString msg = QString("Could not open %1:\n\n%2").arg(target, err);
                if (overview::is_phone_scheme(scheme))
                    msg += "\n\n" + overview::phone_hint(scheme, target);
                QString title = overview::is_phone_scheme(scheme) ? "Connect to Device" : "Connect to Server";
                warn(self, title, msg);
            } else if (!path.isEmpty()) {
                self->remember_server(target);
                self->sidebar->refresh();
                self->open_location(path, new_tab_);
            }
        });
        return;
    }
    if (new_tab_ || !pane()) {
        new_tab(target);
    } else {
        pane()->set_path(target);
        if (!pane()->is_overview())
            pane()->view()->setFocus();
    }
}

void MainWindow::remember_server(const QString &uri)
{
    QStringList recents;
    for (const QString &u : settings().value("recent_servers").toStringList())
        if (u != uri)
            recents << u;
    settings().setValue("recent_servers", QStringList{uri} + recents.mid(0, 9));
}

// -- view

void MainWindow::set_view(const QString &mode)
{
    set_view_value("view_mode", mode);
    pane()->set_view_mode(mode);
    sync_view_btn();
    sync_zoom_slider();
}

void MainWindow::toggle_view() { set_view(pane()->is_grid() ? "list" : "grid"); }

void MainWindow::sync_view_btn()
{
    bool g = pane()->is_grid();
    view_btn->setIcon(g ? icon({"view-list-symbolic", "view-list-details"})
                        : icon({"view-grid-symbolic", "view-grid", "view-list-icons"}));
    view_btn->setToolTip(g ? "Switch to list view (Ctrl+2)" : "Switch to grid view (Ctrl+1)");
    view_btn->setAccessibleName(g ? "Switch to list view" : "Switch to grid view");
}

void MainWindow::sync_zoom_slider()
{
    Pane *p = pane();
    if (!p)
        return;
    zoom_slider->blockSignals(true);
    if (p->is_grid())
        zoom_slider->setRange(GRID_MIN, GRID_MAX);
    else
        zoom_slider->setRange(LIST_MIN, LIST_MAX);
    zoom_slider->setValue(p->zoom_value());
    zoom_slider->blockSignals(false);
}

void MainWindow::set_folder_previews(bool on, bool save)
{
    // global switch for folder mosaics; off means no directory scanning at all (a chooser's is its own)
    QList<MainWindow *> wins = chooser_mode ? QList<MainWindow *>() : WINDOWS;
    if (!wins.contains(this))
        wins << this;
    for (MainWindow *w : wins) {
        w->folder_previews = on;
        w->info->folder_previews = on;
        if (!w->info->path.isEmpty())
            w->info->show_path(w->info->path);
        w->preview_box->blockSignals(true);
        w->preview_box->setChecked(on);
        w->preview_box->blockSignals(false);
        for (Pane *p : w->panes()) {
            p->apply_folder_previews();
            p->view()->viewport()->update();
        }
    }
    if (save)
        set_view_value("folder_previews", on);
    if (!on)
        thumbs->cancel_pending();
}

// -- recursive preview build

void MainWindow::add_preview_menu(QMenu *menu, const QString &folder)
{
    // Generate: only what isn't made yet. Regenerate: everything made again, the cached copies replaced (for previews
    // that look wrong or old). "Folder previews": just the folders' mosaics (and the few images each shows); without
    // "Recursively", this folder and the folders directly in it (and this folder's files).
    QMenu *pm = menu->addMenu(icon({"view-refresh"}), "Previews");
    menus::tag(pm->menuAction(), "previews");
    menus::tag(pm->addAction("Generate Previews Recursively", this, [this, folder]() { build_previews(folder); }),
               "gen_previews_rec");
    menus::tag(pm->addAction("Generate Folder Previews", this, [this, folder]() { build_previews(folder, true, 1); }),
               "gen_folder_previews");
    menus::tag(pm->addAction("Generate Folder Previews Recursively", this, [this, folder]() { build_previews(folder, true); }),
               "gen_folder_previews_rec");
    pm->addSeparator();
    menus::tag(pm->addAction("Regenerate Previews", this, [this, folder]() { build_previews(folder, false, 1, true); }),
               "regen_previews");
    menus::tag(pm->addAction("Regenerate Previews Recursively", this,
                             [this, folder]() { build_previews(folder, false, -1, true); }),
               "regen_previews_rec");
    menus::tag(pm->addAction("Regenerate Folder Previews", this, [this, folder]() { build_previews(folder, true, 1, true); }),
               "regen_folder_previews");
    menus::tag(pm->addAction("Regenerate Folder Previews Recursively", this,
                             [this, folder]() { build_previews(folder, true, -1, true); }),
               "regen_folder_previews_rec");
}

Task *MainWindow::build_previews(const QString &root, bool folders_only, int depth, bool fresh)
{
    // each build is a job of its own: it waits for its turn on the drive (job queue), shows in the status bar and the
    // Jobs window, and can be paused or stopped like any other
    int size = pane() ? pane()->grid_size : 160;
    auto b = std::make_shared<RecursiveBuilder>(root, size, thumbs, nullptr, 3, folders_only, depth, fresh);
    QString name = basename(rstrip(root, '/'));
    if (name.isEmpty())
        name = root;
    QString what = QString(fresh ? "Regenerating " : "Generating ") + (folders_only ? "folder previews" : "previews") +
                   (depth < 0 ? " recursively" : "");
    auto work = [b](Task *task) -> QVariant {
        bool stopped = b->run(task);
        if (stopped)
            task->was_cancelled = true;
        return QVariantMap{{"done", b->done_count()}, {"total", b->total_count()}, {"stopped", stopped}};
    };
    QPointer<MainWindow> w(this);
    auto done = [w, what, name, fresh](const QVariant &r) {
        QVariantMap m = r.toMap();
        if (fresh) {   // the old pictures are still in memory, here and in the other Kestrels
            g_thumbs->clear_memory();
            for (MainWindow *x : WINDOWS)
                for (Pane *p : x->panes())
                    p->animator->clear();
            atc::announce("thumbs_cleared");
        }
        g_thumbs->failed.clear();   // items that failed earlier may exist on disk now
        repaint_all();
        if (!w)
            return;
        QString text = !r.isValid() || m["stopped"].toBool() ? "Stopped " + what.left(1).toLower() + what.mid(1)
                                                             : "Finished " + what.left(1).toLower() + what.mid(1);
        w->statusBar()->showMessage(QString("%1 in %2: %3 of %4 items")
                                        .arg(text, name, group_digits(m["done"].toInt()), group_digits(m["total"].toInt())),
                                    8000);
    };
    return fileops::queue_job(this, what + ": " + name, {root}, work, done);
}

void MainWindow::on_thumb_progress(int done, int total)
{
    // small batches (a few visible items) finish too fast to be worth showing
    if (total < 4 || done >= total) {
        thumb_progress->hide();
        return;
    }
    thumb_progress->setMaximum(total);
    thumb_progress->setValue(done);
    thumb_progress->show();
}

void MainWindow::toggle_hidden(bool on)
{
    show_hidden = on;
    set_view_value("show_hidden", on);
    for (Pane *p : panes())
        p->apply_hidden();
}

void MainWindow::toggle_panel(QWidget *w, const QString &key, bool on)
{
    w->setVisible(on);
    set_view_value(key, on);
    if (on && w == info)
        update_status();
}

void MainWindow::update_status()
{
    Pane *p = pane();
    if (!p)
        return;
    if (p->is_overview()) {
        status_label->setText("Drives, network locations and bookmarks");
        if (info->isVisible())
            info->show_path(QString());
        return;
    }
    QStringList sel = p->selected_paths();
    int total = p->all_paths().size();
    QString text;
    if (!sel.isEmpty()) {
        qint64 size = 0;
        int dirs = 0;
        // what the view knows first: on a tape busy seeking, a stat would hold the window until the drive gets there
        QHash<QString, Pane::Known> known;
        for (const Pane::Known &k : p->selected_known())
            known.insert(k.path, k);
        for (const QString &s : sel) {
            Pane::Known k = known.value(s);
            bool slow = drives::slow_kind(s) != drives::Slow::None;
            bool dir;
            if (k.kind >= 0 && (!(k.kind & KIND_LINK) || slow))   // (a link elsewhere: followed, as before)
                dir = k.kind & KIND_DIR;
            else
                dir = !slow && isdir(s);
            if (dir) {
                dirs += 1;
            } else if (k.size >= 0) {
                size += k.size;
            } else if (!slow) {
                struct stat st;
                if (lstat_(s, st))
                    size += st.st_size;
            }
        }
        int files = sel.size() - dirs;
        QStringList parts;
        if (dirs)
            parts << QString("%1 folder%2").arg(dirs).arg(dirs != 1 ? "s" : "");
        if (files)
            parts << QString("%1 file%2 (%3)").arg(QString::number(files), files != 1 ? "s" : "", human_size(size));
        text = QString("%1 selected of %2").arg(parts.join(" and "), QString::number(total));
    } else {
        text = QString("%1 item%2").arg(total).arg(total != 1 ? "s" : "");
    }
    if (p->in_search && p->searching())
        text += "  — searching…";
    status_label->setText(text);
    if (info->isVisible())
        info->show_path(sel.size() == 1 ? sel.first() : (sel.isEmpty() ? p->path : QString()));
}

// -- opening files (activation, Quick View, the image viewer): opening.cpp

// -- context menu

void MainWindow::context_menu(Pane *p, QAbstractItemView *view, const QPoint &pos)
{
    QModelIndex idx = view->indexAt(pos);
    QStringList paths;
    if (idx.isValid() && !idx.siblingAtColumn(0).data(PathRole).toString().isEmpty()) {   // not a group's header
        QString x = idx.siblingAtColumn(0).data(PathRole).toString();
        if (!p->selected_paths().contains(x))
            p->select_paths({x});
        paths = p->selected_paths();
    } else {
        view->clearSelection();
    }
    QMenu *m = build_menu(p, paths);
    m->exec(view->viewport()->mapToGlobal(pos));
    m->deleteLater();
}

QMenu *MainWindow::build_menu(Pane *p, const QStringList &paths)
{
    auto *m = new QMenu(this);
    QString cur = p->path;
    // every entry gets its id (menus.h), so the menu can be laid out as the user chose (Preferences → Menus)
    auto T = [](QAction *a, const char *id) {
        menus::tag(a, id);
        return a;
    };
    auto group = [m](const char *id, const std::function<void()> &add) {   // what a helper adds, under one id
        int n = int(m->actions().size());
        add();
        QList<QAction *> acts = m->actions();
        for (int i = n; i < acts.size(); ++i)
            if (!acts[i]->isSeparator())
                menus::tag(acts[i], id);
    };
    if (paths.isEmpty() && p->is_virtual()) {
        m->addAction("Select All", this, [p]() { p->view()->selectAll(); });
        return m;
    }
    if (paths.isEmpty()) {
        T(m->addAction(icon({"folder-new"}), "New Folder…", this, [this]() { new_folder(); }), "new_folder");
        QMenu *nd = m->addMenu(icon({"document-new"}), "New Document");
        T(nd->menuAction(), "new_document");
        nd->addAction("Empty File…", this, [this]() { new_file(); });
        QString tdir = xdg_user_dir("TEMPLATES");
        if (isdir(tdir)) {
            QStringList names;
            try {
                names = listdir(tdir);
            } catch (const OSError &) {
            }
            natural_sort(names);
            for (const QString &t : names) {
                QString full = join(tdir, t);
                nd->addAction(icon_for_path(full), split_ext(t).first, this, [this, full]() { new_file(full); });
            }
        }
        m->addSeparator();
        bool have_files = !read_clipboard().second.isEmpty();
        const QMimeData *md = QGuiApplication::clipboard()->mimeData();
        QAction *pa = T(m->addAction(icon({"edit-paste"}), "Paste", this, [this]() { paste(); }), "paste");
        pa->setEnabled(have_files || (md && md->hasImage()));
        QAction *pl = T(m->addAction("Paste as Link", this, [this]() { paste(QString(), true); }), "paste_link");
        pl->setEnabled(have_files);
        T(m->addAction("Select All", this, [p]() { p->view()->selectAll(); }), "select_all");
        m->addSeparator();
        QMenu *sm = m->addMenu("Sort By");
        T(sm->menuAction(), "sort_by");
        add_sort_items(sm, p);
        QAction *a = T(m->addAction("Show Hidden Files", this, [this]() { a_hidden->trigger(); }), "show_hidden");
        a->setCheckable(true);
        a->setChecked(show_hidden);
        if (p->grouping() != groups::NONE) {
            QPointer<Pane> pp(p);
            T(m->addAction("Collapse All Groups", this, [pp]() {
                  if (pp)
                      pp->set_all_groups_collapsed(true);
              }),
              "collapse_groups");
            T(m->addAction("Expand All Groups", this, [pp]() {
                  if (pp)
                      pp->set_all_groups_collapsed(false);
              }),
              "expand_groups");
        }
        m->addSeparator();
        T(m->addAction(icon({"utilities-terminal"}), "Open in Terminal", this, [cur]() { open_terminal(cur); }),
          "open_terminal");
        T(m->addAction(icon({"bookmark-new"}), "Bookmark This Folder", this, [this, cur]() { sidebar->add_bookmark(cur); }),
          "bookmark_folder");
        add_preview_menu(m, cur);
        if (in_trash(join(cur, "x"))) {
            T(m->addAction(icon({"user-trash"}), "Empty Trash", this, [this]() { empty_trash(); }), "empty_trash");
            if (fileops::can_shred())
                T(m->addAction(icon({"edit-shred", "edit-delete"}), "Empty Trash with BleachBit…", this,
                               [this]() { empty_trash_with_bleachbit(); }),
                  "empty_trash_bleachbit");
        }
        group("scripts", [&]() { sharing::add_scripts_menu(m, {}, cur, [this](const QString &d) { navigate(d); }); });
        m->addSeparator();
        T(m->addAction(icon({"document-properties"}), "Properties", this, [this, cur]() { properties({cur}); }), "properties");
        menus::arrange(m, menus::BACKGROUND);
        return m;
    }

    QString single = paths.size() == 1 ? paths.first() : QString();
    bool is_dir = !single.isEmpty() && isdir(single);
    // only a selection all in the trash gets the trash's menu; a mixed one (a search, Recent, Starred) gets the
    // usual menu, whose Move to Trash trashes the rest and asks before deleting what's already in the trash
    if (std::all_of(paths.begin(), paths.end(), [](const QString &x) { return in_trash(x); })) {
        m->addAction(icon({"edit-undo"}), "Restore", this, [this, paths]() { restore(paths); });
        m->addAction(icon({"edit-delete"}), "Delete Permanently", this, [this, paths]() { delete_paths(paths); });
        if (fileops::can_shred())
            m->addAction(icon({"edit-shred", "edit-delete"}), "Shred with BleachBit…", this,
                         [this, paths]() { shred_paths(paths); });
        m->addSeparator();
        m->addAction(icon({"document-properties"}), "Properties", this, [this, paths]() { properties(paths); });
        return m;
    }

    // which right-click menu this is (Preferences → Menus): a split set selected whole is several items
    QStringList arcs = std::all_of(paths.begin(), paths.end(),
                                   [](const QString &x) { return archive::can_extract(x) && !isdir(x); })
                           ? archive_ui::archives_of(paths)
                           : QStringList();
    menus::Kind kind = paths.size() > 1 ? menus::SEVERAL : is_dir ? menus::FOLDER : arcs.size() == 1 ? menus::ARCHIVE
                                                                                                     : menus::FILE;
    T(m->addAction(icon({"document-open"}), "Open", this, [this, p, paths]() { open_paths(p, paths); }), "open");
    bool all_dirs = std::all_of(paths.begin(), paths.end(), [](const QString &x) { return isdir(x); });
    if (is_dir || all_dirs) {
        T(m->addAction(icon({"tab-new"}), "Open in New Tab", this, [this, paths]() {
            for (const QString &x : paths)
                new_tab(x, false);
        }), "open_new_tab");
        T(m->addAction("Open in New Window", this, [paths]() { open_window(paths); }), "open_new_window");
    }
    if (is_dir) {
        int n = int(m->actions().size());
        sharing::add_open_folder_menu(m, single, [this, paths]() { show_free(new OpenWithDialog(this, paths)); });
        QList<QAction *> acts = m->actions();
        for (int i = n; i < acts.size(); ++i)   // the code editors' "Open in …", then the Open With submenu
            menus::tag(acts[i], acts[i]->menu() ? "open_with" : "open_in_editor");
    }
    QStringList imgs;
    for (const QString &x : paths)
        if (is_image(x))
            imgs << x;
    if (!imgs.isEmpty())
        T(m->addAction(icon({"image-x-generic"}), imgs.size() > 1 ? "View Images" : "View Image", this,
                       [this, p, imgs]() { quick_view(p, imgs); }),
          "view_image");
    if (p->in_search)
        T(m->addAction(icon({"folder-open"}), "Show in Folder", this, [this, paths]() { reveal(paths.first()); }),
          "show_in_folder");
    if (p->group_of_path(paths.value(0)) >= 0) {   // grouped by date: its group collapses from here, not only its header
        QPointer<Pane> pp(p);
        int key = p->group_of_path(paths.value(0));
        T(m->addAction("Collapse This Group", this, [pp, key]() {
              if (pp)
                  pp->collapse_group(key);
          }),
          "collapse_group");
    }
    if (!is_dir && !single.isEmpty()) {
        QMenu *ow = m->addMenu("Open With");
        T(ow->menuAction(), "open_with");
        QList<AppRef> rec = apps_for(single).first;
        for (int i = 0; i < std::min<qsizetype>(8, rec.size()); ++i) {
            AppRef app = rec[i];
            ow->addAction(app_icon(app), app_name(app), this, [this, app, paths]() {
                try {
                    launch_app(app, paths);
                } catch (const Error &e) {
                    warn(this, "Open With", e.message());
                }
            });
        }
        ow->addSeparator();
        ow->addAction("Other Application…", this, [this, paths]() { show_free(new OpenWithDialog(this, paths)); });
    }
    m->addSeparator();
    T(m->addAction(icon({"edit-cut"}), "Cut", this, [this, paths]() { clip(true, paths); }), "cut");
    T(m->addAction(icon({"edit-copy"}), "Copy", this, [this, paths]() { clip(false, paths); }), "copy");
    if (is_dir) {
        QAction *pa = T(m->addAction(icon({"edit-paste"}), "Paste Into Folder", this, [this, single]() { paste(single); }),
                        "paste_into");
        pa->setEnabled(!read_clipboard().second.isEmpty());
    }
    T(m->addAction("Move To…", this, [this, paths]() { transfer_to(paths, "move"); }), "move_to");
    T(m->addAction("Copy To…", this, [this, paths]() { transfer_to(paths, "copy"); }), "copy_to");
    T(m->addAction("Duplicate", this, [this, paths]() { duplicate(paths); }), "duplicate");
    T(m->addAction(icon({"edit-rename"}), !single.isEmpty() ? QString("Rename…") : QString("Rename %1 Items…").arg(paths.size()),
                   this, [this, paths]() { rename(paths); }),
      "rename");
    QMenu *cp = m->addMenu(icon({"edit-copy"}), "Copy Path / Name");
    T(cp->menuAction(), "copy_path");
    T(cp->addAction("Copy Full Path", this, [this, paths]() { copy_text(paths); }), "copy_full_path");
    T(cp->addAction("Copy Name", this, [this, paths]() {
        QStringList names;
        for (const QString &x : paths)
            names << basename(x);
        copy_text(names);
    }), "copy_name");
    T(cp->addAction("Copy URI", this, [this, paths]() {
        QStringList uris;
        for (const QString &x : paths)
            uris << file_uri(x);
        copy_text(uris);
    }), "copy_uri");

    bool starred = std::all_of(paths.begin(), paths.end(), [](const QString &x) { return places::is_starred(x); });
    T(m->addAction(starred ? icon({"non-starred-symbolic", "non-starred"}) : icon({"starred-symbolic", "starred"}),
                   starred ? "Unstar" : "Star", this, [paths, starred]() { places::set_starred(paths, !starred); }),
      "star");
    // pinned in its own folder: sorted to the top there by Pinned & Starred First
    bool pinned = std::all_of(paths.begin(), paths.end(), [](const QString &x) { return places::is_pinned(x); });
    T(m->addAction(icon({"view-pin-symbolic", "view-pin", "pin"}), pinned ? "Unpin" : "Pin", this,
                   [paths, pinned]() { places::set_pinned(paths, !pinned); }),
      "pin");

    QString here = p->dir();   // empty in Starred / Recent: no "… Here" there
    QMenu *lm = m->addMenu(icon({"emblem-symbolic-link", "insert-link"}), "Links && Shortcuts");
    T(lm->menuAction(), "links");
    if (!here.isEmpty()) {
        T(lm->addAction("Create Symbolic Link Here", this, [this, paths, here]() { make_links(paths, here, "sym"); }),
          "link_sym_here");
        T(lm->addAction("Create Relative Symbolic Link Here", this, [this, paths, here]() { make_links(paths, here, "rel"); }),
          "link_rel_here");
    }
    if (!here.isEmpty() && std::all_of(paths.begin(), paths.end(), [](const QString &x) { return isfile(x) && !islink(x); }))
        T(lm->addAction("Create Hard Link Here", this, [this, paths, here]() { make_links(paths, here, "hard"); }),
          "link_hard_here");
    T(lm->addAction("Create Symbolic Link In…", this, [this, paths]() { make_links(paths, QString(), "sym"); }), "link_sym_in");
    lm->addSeparator();
    QString desktop = xdg_user_dir("DESKTOP");
    T(lm->addAction("Send Link to Desktop", this, [this, paths, desktop]() { make_links(paths, desktop, "sym"); }),
      "link_desktop");
    if (!here.isEmpty())
        T(lm->addAction("Create Desktop Shortcut (.desktop) Here", this,
                        [this, paths, here]() { make_links(paths, here, "desktop"); }),
          "desktop_shortcut_here");
    T(lm->addAction("Send Shortcut (.desktop) to Desktop", this,
                    [this, paths, desktop]() { make_links(paths, desktop, "desktop"); }),
      "desktop_shortcut_desktop");
    if (std::any_of(paths.begin(), paths.end(), [](const QString &x) { return islink(x); })) {
        lm->addSeparator();
        T(lm->addAction("Open Link Target Location", this, [this, paths]() { reveal(realpath(paths.first())); }),
          "link_target");
    }
    if (is_dir) {
        lm->addSeparator();
        T(lm->addAction(icon({"bookmark-new"}), "Add to Bookmarks", this, [this, single]() { sidebar->add_bookmark(single); }),
          "add_bookmark");
    }

    m->addSeparator();
    // archives: one, or a split set selected whole (x.part1.rar … x.part3.rar), gets Extract; several get Extract All
    if (arcs.size() == 1) {
        QString one = arcs[0];
        QString missing = archive::missing_extract_tool(one);
        if (!missing.isEmpty()) {
            QAction *a = T(m->addAction(icon({"archive-extract", "package-x-generic"}),
                                        QString("Extract (install %1)").arg(missing)),
                           "extract_here");   // (where Extract Here would be)
            a->setEnabled(false);
        } else {
            T(m->addAction(icon({"archive-extract", "package-x-generic"}), "Extract Here", this,
                           [this, one]() { archive_ui::extract_here(this, one); }),
              "extract_here");
            T(m->addAction(icon({"archive-extract", "package-x-generic"}), "Extract To…", this,
                           [this, one]() { archive_ui::extract_dialog(this, one); }),
              "extract_to");
            if (archive::can_browse(one))
                T(m->addAction(icon({"document-open", "package-x-generic"}), "Open as Archive", this,
                               [this, one]() { archive_ui::browse(this, one); }),
                  "open_as_archive");
            T(m->addAction(icon({"emblem-ok-symbolic", "dialog-information"}), "Test Archive", this,
                           [this, one]() { archive_ui::test_archives(this, {one}); }),
              "test_archive");
        }
    } else if (arcs.size() > 1) {
        T(m->addAction(icon({"archive-extract", "package-x-generic"}), "Extract All Here", this,
                       [this, arcs]() { archive_ui::extract_all_here(this, arcs); }),
          "extract_all_here");
        T(m->addAction(icon({"archive-extract", "package-x-generic"}), "Extract All To…", this,
                       [this, arcs]() { archive_ui::extract_all_dialog(this, arcs); }),
          "extract_all_to");
        T(m->addAction(icon({"emblem-ok-symbolic", "dialog-information"}), "Test Archives", this,
                       [this, arcs]() { archive_ui::test_archives(this, arcs); }),
          "test_archives");
    }
    QString quick = archive_ui::quick_compress_label(paths);
    if (!quick.isEmpty())
        T(m->addAction(icon({"package-x-generic", "archive-insert"}), quick, this,
                       [this, paths]() { archive_ui::quick_compress(this, paths); }),
          "compress_quick");
    T(m->addAction(icon({"package-x-generic", "archive-insert"}), "Compress…", this,
                   [this, paths]() { archive_ui::compress_dialog(this, paths); }),
      "compress");
    T(m->addAction(icon({"security-high", "document-properties"}), "Create Checksum File…", this,
                   [this, paths]() { hashcheck::create_dialog(this, paths); }),
      "checksum_create");
    if (!single.isEmpty() && (is_image(single) || is_video(single)) && uwp::editor_open())
        T(m->addAction(icon({"preferences-desktop-wallpaper", "video-display"}), "Add to Selected UWP Monitor", this,
                       [this, single]() { uwp_add(single); }),
          "uwp_add");
    if (!single.isEmpty() && is_video(single) && uwp::available())
        T(m->addAction(icon({"preferences-desktop-wallpaper"}), uwp::wallpaper_label(), this,
                       [single]() { set_wallpaper(single); }),
          "wallpaper");
    if (!single.isEmpty() && is_image(single)) {
        QMenu *im = m->addMenu(icon({"image-x-generic"}), "Image");
        T(im->menuAction(), "image");
        T(im->addAction(uwp::wallpaper_label(), this, [single]() { set_wallpaper(single); }), "image_wallpaper");
        T(im->addAction("Use as Folder Cover", this, [this, single]() { thumbs->set_cover(dirname(single), single); }),
          "image_cover");
        T(im->addAction("Copy Image to Clipboard", this, [single]() { QGuiApplication::clipboard()->setImage(QImage(single)); }),
          "image_copy");
    }
    QStringList folders;
    for (const QString &x : paths)
        if (isdir(x))
            folders << x;
    if (!folders.isEmpty() && folders.size() == paths.size()) {
        int n = int(m->actions().size());
        folder_style_menu(m, folders);
        QList<QAction *> acts = m->actions();
        for (int i = n; i < acts.size(); ++i)
            menus::tag(acts[i], acts[i]->menu() ? "folder_colour" : "show_image_previews");
    }
    if (is_dir && thumbs->covers.contains(single))
        T(m->addAction("Reset Folder Cover", this, [this, single]() { thumbs->set_cover(single, QString()); }), "reset_cover");
    group("send_to", [&]() { sharing::add_send_to_menu(m, paths); });
    if (is_dir && sharing::can_share())
        T(m->addAction(icon({"folder-remote", "network-workgroup"}), "Network Sharing…", this,
                       [this, single]() { sharing::share_dialog(this, single); }),
          "network_sharing");
    group("scripts", [&]() { sharing::add_scripts_menu(m, paths, here, [this](const QString &d) { navigate(d); }); });
    if (is_dir) {
        T(m->addAction("Regenerate Preview", this, [this, single]() {
            thumbs->invalidate(single);
            atc::announce("folders", {{"paths", QJsonArray{single}}});
        }), "regen_preview");
        add_preview_menu(m, single);
        T(m->addAction(icon({"utilities-terminal"}), "Open in Terminal", this, [single]() { open_terminal(single); }),
          "open_terminal");
    }
    m->addSeparator();
    T(m->addAction(icon({"user-trash"}), "Move to Trash", this, [this, paths]() { trash_paths(paths); }), "trash");
    T(m->addAction(icon({"edit-delete"}), "Delete Permanently…", this, [this, paths]() { delete_paths(paths); }), "delete");
    if (fileops::can_shred())
        T(m->addAction(icon({"edit-shred", "edit-delete"}), "Shred with BleachBit…", this,
                       [this, paths]() { shred_paths(paths); }),
          "shred");
    m->addSeparator();
    T(m->addAction(icon({"document-properties"}), "Properties", this, [this, paths]() { properties(paths); }), "properties");
    menus::arrange(m, kind);
    return m;
}

void MainWindow::folder_style_menu(QMenu *m, const QStringList &folders)
{
    // Folder Colour submenu and the Show Image Previews switch for one or more folders
    ThumbnailManager *t = thumbs;
    QSet<QString> current;
    for (const QString &f : folders)
        current << t->custom_color(f);
    QMenu *cm = m->addMenu(icon({"preferences-color", "applications-graphics"}), "Folder Colour");
    for (const auto &[name, color] : thumbs::FOLDER_COLORS) {
        QString c = color;
        QAction *a = cm->addAction(thumbs::color_swatch(c), name, this, [t, folders, c]() { t->set_folder_color(folders, c); });
        a->setCheckable(true);
        a->setChecked(current == QSet<QString>{c});
    }
    cm->addSeparator();
    QAction *a = cm->addAction(thumbs::color_swatch(t->folder_color), "Default", this,
                               [t, folders]() { t->set_folder_color(folders, QString()); });
    a->setCheckable(true);
    a->setChecked(current == QSet<QString>{QString()});
    bool on = std::all_of(folders.begin(), folders.end(), [t](const QString &f) { return t->previews_for(f); });
    a = m->addAction("Show Image Previews", this, [t, folders, on]() { t->set_folder_previews(folders, !on); });
    a->setCheckable(true);
    a->setChecked(on);
    a->setToolTip("Show a mosaic of the images inside on this folder's icon");
}

// -- file actions (the clipboard, new files, rename, trash, links…): actions.cpp

void MainWindow::uwp_add(const QString &path)
{
    if (uwp::add_to_selected(path))
        statusBar()->showMessage(QString("Sent %1 to the selected UWP monitor").arg(basename(path)), 4000);
    else
        warn(this, "UWP", "Couldn't reach UWP. Is its editor window still open?");
}

void MainWindow::properties(const QStringList &paths, QWidget *parent)
{
    if (paths.isEmpty())
        return;
    auto *d = new PropertiesDialog(parent ? parent : this, paths);   // several can be open, each a window of its own
    connect(d, &QDialog::finished, this, &MainWindow::update_status);
    show_free(d);
}

// -- settings

void MainWindow::preferences()
{
    if (prefs) {   // one at a time: it edits the same settings
        prefs->raise();
        prefs->activateWindow();
        return;
    }
    prefs = new PreferencesDialog(this);   // (changes apply as they're made: prefs.h)
    show_free(prefs);
}

void repaint_all()
{
    for (MainWindow *w : WINDOWS)
        for (Pane *p : w->panes())
            p->view()->viewport()->update();
}

// The main windows' view settings take effect in every main window (a chooser window has its own, "chooser/…")
void apply_settings(const QStringList &keys);
static const bool settings_hooked = (prefs::apply_hook = apply_settings, true);   // prefs::set applies through it

void apply_settings(const QStringList &keys)
{
    bool prefs_wide = false;
    for (const QString &raw : keys) {
        bool chooser = raw.startsWith("chooser/");
        QString key = chooser ? raw.mid(8) : raw;
        QList<MainWindow *> wins;
        for (MainWindow *w : WINDOWS)
            if (w->chooser_mode == chooser)
                wins << w;
        QVariant v = prefs::get(raw);
        if (key == "view_mode") {
            for (MainWindow *w : wins) {
                for (Pane *p : w->panes())
                    p->set_view_mode(v.toString());
                w->sync_view_btn();
                w->sync_zoom_slider();
            }
        } else if (key == "grid_size" || key == "list_size") {
            for (MainWindow *w : wins) {
                for (Pane *p : w->panes())
                    p->set_sizes(w->view_value("grid_size").toInt(), w->view_value("list_size").toInt());
                w->sync_zoom_slider();
            }
        } else if (key == "show_hidden") {   // (applied, not saved again: a reset leaves it unset)
            for (MainWindow *w : wins) {
                QSignalBlocker quiet(w->a_hidden);
                w->a_hidden->setChecked(v.toBool());
                w->show_hidden = v.toBool();
                for (Pane *p : w->panes())
                    p->apply_hidden();
            }
        } else if (key == "sidebar" || key == "info_panel") {
            for (MainWindow *w : wins) {
                QAction *a = key == "sidebar" ? w->a_sidebar : w->a_info;
                QSignalBlocker quiet(a);
                a->setChecked(v.toBool());
                (key == "sidebar" ? static_cast<QWidget *>(w->sidebar) : w->info)->setVisible(v.toBool());
                if (key == "info_panel" && v.toBool())
                    w->update_status();
            }
        } else if (key == "folder_previews") {
            if (!wins.isEmpty())
                wins.first()->set_folder_previews(v.toBool(), false);   // (every main window)
        } else if (key == "pinned_first") {
            for (MainWindow *w : wins)
                for (Pane *p : w->panes())
                    p->set_pinned_first(v.toBool());
        } else if (key.startsWith("sort_")) {
            for (MainWindow *w : WINDOWS)
                for (Pane *p : w->panes())
                    p->apply_folder_sort();
        } else if (key == "search_recursive" || key == "search_contents") {
            for (MainWindow *w : wins)
                for (Pane *p : w->panes())
                    p->set_search_options(prefs::get("search_recursive").toBool(), prefs::get("search_contents").toBool());
        } else if (key == "sidebar_sections" || key == "sidebar_collapsed") {
            for (MainWindow *w : WINDOWS)
                w->sidebar->refresh();
        } else if (raw.startsWith("menus/") || raw.startsWith("tape_")) {
            // read each time a menu is built, or a copy onto a tape starts
        } else if (key == "chooser_start" || key == "chooser_places") {
            // read when a Save window opens; a Save window's Overview follows chooser_places itself (prefs::Notifier)
        } else if (key == "jobs_mode" || key == "jobs_start_paused") {
            // the job queue tells everyone itself (jobs::set_mode, "queue_mode")
        } else {
            prefs_wide = true;   // thumbnails, previews, undo, developer tools…: apply_preferences
        }
    }
    if (prefs_wide)
        apply_preferences();
}

void apply_preferences()
{
    apply_thumb_settings(g_thumbs);
    g_thumbs->clear_memory();
    for (MainWindow *w : WINDOWS)
        for (Pane *p : w->panes()) {
            p->animator->clear();
            p->apply_folder_previews();
            p->view()->viewport()->update();
        }
    Q_EMIT undo::signals_()->changed();   // "Share undo…" may have changed what Ctrl+Z undoes
    for (MainWindow *w : WINDOWS)
        if (w->a_tower)
            w->a_tower->setVisible(settings().value("developer_tools", false).toBool());
}

void MainWindow::clear_cache()
{
    rmtree(join(APP_CACHE(), "folders"));
    thumbs->clear_memory();
    repaint_all();
    atc::announce("thumbs_cleared");
    statusBar()->showMessage("Folder preview cache cleared", 3000);
}

void MainWindow::purge_thumbnails()
{
    auto *box = new QMessageBox(QMessageBox::Warning, "Delete All Thumbnails",
                                QString("Delete every thumbnail and folder preview %1 has made?\n\n"
                                        "• Folder mosaics in %2\n"
                                        "• Image/video thumbnails it wrote to %3\n\n"
                                        "They will be regenerated as you browse.")
                                    .arg(APP_NAME, join(APP_CACHE(), "folders"), THUMB_DIR()),
                                QMessageBox::Cancel, this);
    QPushButton *go = box->addButton("Delete", QMessageBox::DestructiveRole);
    auto *shared = new QCheckBox("Also delete thumbnails made by other apps (GNOME Files, etc.)");
    box->setCheckBox(shared);
    ask_box(box, [this, go, shared](QAbstractButton *b) {
        if (b == go)
            purge_now(shared->isChecked());
    });
}

void MainWindow::purge_now(bool include)
{
    thumbs->cancel_pending();
    fileops::run_task(
        this, "Deleting thumbnails",
        [include]() {
            auto [files, size] = thumbs::purge_thumbnails(include);
            return QVariant(QVariantList{files, size});
        },
        [this](const QVariant &res) {
            QVariantList r = res.toList();
            thumbs->clear_memory();
            repaint_all();
            atc::announce("thumbs_cleared");
            statusBar()->showMessage(QString("Deleted %1 thumbnails (%2)")
                                         .arg(group_digits(r.value(0).toLongLong()), human_size(r.value(1).toLongLong())),
                                     6000);
        });
}

void MainWindow::show_shortcuts()
{
    QString text = R"(
<table cellpadding=3>
<tr><td><b>Enter / double-click</b></td><td>Open</td></tr>
<tr><td><b>Space</b></td><td>Quick view selection</td></tr>
<tr><td><b>Backspace, Alt+Left / Alt+Right</b></td><td>Back / Forward</td></tr>
<tr><td><b>Alt+Up</b></td><td>Parent folder</td></tr>
<tr><td><b>Ctrl+L</b></td><td>Type a location</td></tr>
<tr><td><b>Ctrl+F</b></td><td>Search (filter or recursive)</td></tr>
<tr><td><b>Ctrl+T / Ctrl+W</b></td><td>New tab / close tab (middle-click folder: open in tab)</td></tr>
<tr><td><b>Ctrl+1 / Ctrl+2</b></td><td>Grid / list view</td></tr>
<tr><td><b>Ctrl+scroll, Ctrl+= / Ctrl+-</b></td><td>Zoom thumbnails</td></tr>
<tr><td><b>Ctrl+H</b></td><td>Show hidden files</td></tr>
<tr><td><b>F2</b></td><td>Rename (batch rename with multiple selected)</td></tr>
<tr><td><b>Ctrl+X / C / V</b></td><td>Cut / copy / paste (works with GNOME Files)</td></tr>
<tr><td><b>Ctrl+Shift+V</b></td><td>Paste as symbolic link</td></tr>
<tr><td><b>Delete / Shift+Delete</b></td><td>Trash / delete permanently</td></tr>
<tr><td><b>Alt+Enter</b></td><td>Properties</td></tr>
<tr><td><b>F3 / F9</b></td><td>Info panel / sidebar</td></tr>
<tr><td><b>Ctrl+Left / Ctrl+Right</b></td><td>Grouped by date: collapse / expand the current item's group</td></tr>
<tr><td><b>Drag + Ctrl / Shift / Ctrl+Shift / Alt</b></td><td>Copy / move / link / ask</td></tr>
<tr><td colspan=2><br><b>Image viewer</b></td></tr>
<tr><td><b>←/→, scroll wheel</b></td><td>Previous / next</td></tr>
<tr><td><b>Ctrl+scroll, +/-, 0, 1</b></td><td>Zoom, fit, 100%</td></tr>
<tr><td><b>F / double-click</b></td><td>Fullscreen</td></tr>
<tr><td><b>S</b></td><td>Slideshow</td></tr>
<tr><td><b>R / L / H</b></td><td>Rotate right / left, flip</td></tr>
<tr><td><b>I</b></td><td>Toggle info overlay</td></tr>
<tr><td><b>Delete</b></td><td>Move to trash</td></tr>
</table>)";
    inform(this, "Keyboard Shortcuts", text);
}

void MainWindow::closeEvent(QCloseEvent *ev)
{
    QList<Task *> tasks = task_panel->tasks;
    if (!tasks.isEmpty()) {
        // the worker threads report to this window; closing now would drop their results mid-operation
        QStringList names;
        for (int i = 0; i < std::min<qsizetype>(5, tasks.size()); ++i)
            names << "• " + tasks[i]->title;
        ev->ignore();
        if (close_box) {   // already asked
            close_box->raise();
            close_box->activateWindow();
            return;
        }
        close_box = new QMessageBox(QMessageBox::Question, "Operations Running",
                                    QString("These operations are still running:\n\n%1\n\n"
                                            "Stop them and close the window? Items already processed stay processed.")
                                        .arg(names.join('\n')),
                                    QMessageBox::NoButton, this);
        QPushButton *stop = close_box->addButton("Stop and Close", QMessageBox::DestructiveRole);
        close_box->addButton("Keep Running", QMessageBox::RejectRole);
        ask_box(close_box, [this, stop](QAbstractButton *b) {
            if (b != stop)
                return;
            for (Task *t : task_panel->tasks)
                t->cancel();
            task_panel->refresh();
            centralWidget()->setEnabled(false);   // stay visible (showing "cancelling…") until stopped
            close_when_idle();
        });
        return;
    }
    if (!chooser_mode || !(isMaximized() || isFullScreen()))   // a chooser always opens as a smaller window
        set_view_value("geometry", saveGeometry());
    set_view_value("splitter", split->saveState());
    if (chooser)
        chooser->finish(false);   // closed without choosing: cancelled
    for (Pane *p : panes())
        p->stop_search();
    for (const auto &v : viewers)
        if (v)
            v->close();
    WINDOWS.removeAll(this);
    report_windows();
    QMainWindow::closeEvent(ev);
}

void MainWindow::sync_undo()
{
    QString what = undo::label();
    a_undo->setText(what.isEmpty() ? QString("Undo") : "Undo " + what);
    a_undo->setEnabled(!what.isEmpty());
}

void MainWindow::close_when_idle()
{
    // finish closing once every task has stopped (tasks that can't be cancelled run to the end)
    if (!task_panel->tasks.isEmpty())
        QTimer::singleShot(100, this, &MainWindow::close_when_idle);
    else
        close();
}

// ---------------------------------------------------------------- entry

void apply_thumb_settings(ThumbnailManager *t)
{
    QSettings &s = settings();
    t->folder_count = s.value("folder_count", 4).toInt();
    t->folder_order = s.value("folder_order", "name").toString();
    QString color = s.value("folder_color", "accent").toString();
    t->folder_accent = thumbs::follows_accent(color);
    t->folder_color = t->folder_accent ? accent_color().name() : color;
    t->max_file_mb = s.value("thumb_max_mb", 200).toInt();
    static ThumbnailManager *watched = nullptr;
    if (!watched) {   // runs before the windows' own updates, which then draw folders in the new accent
        watched = t;
        on_palette_change(t, [t]() {
            if (t->folder_accent && t->folder_color != accent_color().name()) {
                t->folder_color = accent_color().name();
                repaint_all();
            }
        });
    }
}

QString location_arg(const QString &arg)
{
    // a command-line argument (as passed by xdg-open, the file chooser or GNOME) as a location for open_location:
    // a local path, OVERVIEW, or a network URI to mount; empty if it can't be opened
    if (arg.startsWith("file:"))
        return uri_to_path(arg);
    QString scheme = is_uri(arg) ? arg.section(':', 0, 0).toLower() : QString();
    if (scheme == "trash")
        return join(TRASH_DIR(), "files");
    if (scheme == "recent" || scheme == "starred")
        return scheme == "recent" ? places::RECENT : places::STARRED;
    if (scheme == "computer" || scheme == "x-nautilus-desktop" || scheme == "other-locations")
        return OVERVIEW;
    if (is_uri(arg))
        return arg;   // smb://, sftp://, … are mounted through gvfs by open_location
    return abspath(expanduser(arg));
}

MainWindow *open_chooser(const chooser::Request &req, std::function<void(const chooser::Result &)> done)
{
    // not in WINDOWS: a chooser isn't a window other Kestrels hand folders to
    QString start = req.current_folder;
    if (req.saving() && !req.replacing && prefs::get("chooser_start").toString() == "overview")
        start = OVERVIEW;   // its own Overview, with the save locations (a file being replaced: its folder)
    if (start != OVERVIEW && !isdir(start))
        start = settings().value("chooser_folder").toString();   // where the last chooser picked something
    if (start != OVERVIEW && !isdir(start))
        start = HOME();
    auto *w = new MainWindow({start}, g_thumbs, true);
    w->make_chooser(req, std::move(done));
    if (quintptr id = chooser::x11_parent(req.parent_window); id && QGuiApplication::platformName() == "xcb") {
        // the app's window (X11): the chooser is its dialog
        w->winId();   // create the native window, to give it a transient parent before it is shown
        if (QWindow *app_window = QWindow::fromWinId(WId(id))) {
            w->windowHandle()->setTransientParent(app_window);
            QObject::connect(w, &QObject::destroyed, app_window, &QObject::deleteLater);
        }
    }
    w->show();
    w->raise();
    w->activateWindow();
    return w;
}

MainWindow *open_window(const QStringList &paths)
{
    auto *w = new MainWindow(paths, g_thumbs);
    WINDOWS << w;
    w->show();
    report_windows();
    return w;
}

// The app-wide objects, and their order. Each is created once and lives until the process exits (none is deleted,
// so none can be used after it's gone, also by a worker thread finishing late):
//   settings()        util.cpp; first use here, right after QApplication, before any thread starts. Used from the UI
//                     thread only. Other Kestrels change the same file: on_atc("settings") re-reads it.
//   g_thumbs          here, after settings() (apply_thumb_settings reads it). Its thread pool runs for the process.
//   TaskBoard         fileops.cpp, on first use (the first task, or the first window's task panel).
//   atc::radio()      atc.cpp, on first use; start() here, after focus tracking and on_atc are connected, so the
//                     first messages from other Kestrels find them. The tower is a separate process.
//   admin::session()  admin.cpp, on first use (a "Retry as Administrator" or the menu); the helper process it starts
//                     ends when its pipe closes, at the latest when this process exits.
//   WINDOWS           app.cpp; a window is added when opened and removed when it closes (choosers aren't in it).
//   last used window  incoming.cpp (window_focused), a QPointer: cleared by Qt when that window is deleted.
// A file-chooser process (--file-chooser) makes settings() and g_thumbs, then only chooser windows.
int kes_main(int argc, char **argv)
{
    prefer_system_environment();   // before GLib or Qt load anything: see util.h
    // LibRaw (RAW image plugin) uses OpenMP; by default each decode spawns one busy-waiting thread per core, which
    // starves the UI. Must be set before the plugin is loaded.
    setenv("OMP_NUM_THREADS", "2", 0);
    setenv("OMP_WAIT_POLICY", "PASSIVE", 0);
    for (int i = 1; i < argc; ++i) {
        if (QByteArray(argv[i]) == "--version") {
            std::printf("%s %s\n", APP_NAME, VERSION);
            return 0;
        }
        if (QByteArray(argv[i]) == "--atc")
            return atc::run_tower(argc, argv);   // the tower: no window (see atc.h)
    }
    std::signal(SIGPIPE, SIG_IGN);   // a tool that exits early must not kill Kestrel (proc::write_pipe also guards)
    QApplication::setApplicationName(APP_ID);
    QApplication::setApplicationVersion(VERSION);
    QApplication::setApplicationDisplayName(APP_NAME);
    QApplication::setDesktopFileName(APP_ID);
    QApplication app(argc, argv);
    windiag::start();   // each window's size and state changes, for developer tools (☰ → Tower… → Windows)
    stats::print_at_quit();   // KESTREL_STATS=1
    qRegisterMetaType<fileops::DirStats>();
    setup_icon_theme();
    follow_gtk_theme();   // the GTK theme's colours where Qt lacks them (Qt < 6.5, or dark style), following changes
    app.setWindowIcon(app_icon());
    migrate_legacy();
    ensure_desktop_entry();
    QImageReader::setAllocationLimit(2048);
    settings();
    g_thumbs = new ThumbnailManager;
    apply_thumb_settings(g_thumbs);
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--file-chooser") {
            // started by D-Bus for the system's file chooser (see chooser.h): only chooser windows
            app.setQuitOnLastWindowClosed(false);   // the service quits after a minute without dialogs
            chooser::serve([](const chooser::Request &req, std::function<void(const chooser::Result &)> done) {
                QPointer<MainWindow> w = open_chooser(req, std::move(done));
                return std::function<void()>([w]() {
                    if (w)
                        w->close();
                });
            });
            return app.exec();
        }
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--tower-monitor") {   // only the Tower window (towerwin.h)
            atc::radio()->start();
            show_free(new TowerWindow);
            return app.exec();
        }
    QStringList paths;
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a.startsWith('-'))
            continue;
        QString loc = location_arg(a);
        if (!loc.isEmpty())
            paths << loc;
    }
    bool service = false;
    for (int i = 1; i < argc; ++i)
        service = service || QByteArray(argv[i]) == "--dbus-service";
    if (!service && !paths.isEmpty() && open_in_tabs() && !atc::hand_off(paths).isEmpty())
        return 0;   // an open Kestrel window took the folders as tabs
    QObject::connect(qApp, &QGuiApplication::focusWindowChanged, qApp, window_focused);
    QObject::connect(atc::radio(), &atc::Radio::heard, qApp, on_atc);
    atc::radio()->start();
    fm1::start(handle_fm1, service ? std::function<void()>([]() { QApplication::quit(); }) : nullptr);
    if (service) {
        // started by D-Bus for a "show in folder" request: no window of our own; quit if none is asked for
        QTimer::singleShot(30000, qApp, []() {
            if (WINDOWS.isEmpty())
                QApplication::quit();
        });
    } else {
        open_window(paths);
    }
    return app.exec();
}
