#include "app.h"

#include "admin.h"
#include "animate.h"
#include "archive.h"
#include "archive_ui.h"
#include "atc.h"
#include "chooser.h"
#include "dialogs.h"
#include "fileops.h"
#include "hashcheck.h"
#include "fm1.h"
#include "incoming.h"
#include "overview.h"
#include "places.h"
#include "sharing.h"
#include "undo.h"
#include "proc.h"
#include "thumbs.h"
#include "stats.h"
#include "util.h"
#include "uwp.h"
#include "viewer.h"
#include "widgets.h"

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
#include <QSplitter>
#include <QStatusBar>
#include <QStorageInfo>
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
    build_box = new QWidget;
    auto *bl = new QHBoxLayout(build_box);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(4);
    build_label = new QLabel;
    build_label->setMaximumWidth(260);
    build_bar = new QProgressBar;
    build_bar->setFixedWidth(200);
    build_bar->setMaximumHeight(16);
    build_stop = new QToolButton;
    build_stop->setText("✕");
    build_stop->setAutoRaise(true);
    build_stop->setToolTip("Stop building previews");
    connect(build_stop, &QToolButton::clicked, this, &MainWindow::stop_build);
    bl->addWidget(build_label);
    bl->addWidget(build_bar);
    bl->addWidget(build_stop);
    build_box->hide();
    task_panel = new TaskPanel;   // file operations running in the background
    setProperty("task_panel", QVariant::fromValue<QObject *>(task_panel));
    auto *admin_indicator = new admin::Indicator;   // 🛡 while an admin session is open
    statusBar()->addWidget(status_label, 1);
    statusBar()->addPermanentWidget(admin_indicator);
    statusBar()->addPermanentWidget(task_panel);
    statusBar()->addPermanentWidget(build_box);
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
    for (Pane *p : panes())
        p->set_type_filter(chooser->type_filter());
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
    settings().setValue(chooser_mode ? "chooser/" + key : key, value);
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
    connect(preview_box, &QCheckBox::toggled, this, &MainWindow::set_folder_previews);
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
    QMenu *m = sort_menu;
    m->clear();
    Pane *p = pane();
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
    A("Start Admin Session…", {}, [this]() { admin::start_session(this); }, menu, {"security-high", "dialog-password"});
    A("Clear Folder Preview Cache", {}, [this]() { clear_cache(); }, menu);
    A("Delete All Thumbnails…", {}, [this]() { purge_thumbnails(); }, menu, {"edit-clear-all", "edit-delete"});
    A("Keyboard Shortcuts", {"F1"}, [this]() { show_shortcuts(); }, menu);
    A("About", {},
      [this]() {
          QMessageBox::about(this, APP_NAME,
                             QString("<b>%1</b> %2<br>A lightweight image-gallery-oriented file manager.")
                                 .arg(APP_NAME, VERSION));
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

void MainWindow::tab_changed(int)
{
    if (pane()) {
        pane_path_changed(pane());
        pane()->view()->setFocus();
    }
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
    QString d = p->dir();
    QStorageInfo vol;
    if (!d.isEmpty() && !is_device_path(d))   // statfs on a phone waits behind its file transfers
        vol = QStorageInfo(d);
    free_label->setText(!d.isEmpty() && vol.isValid() ? human_size(vol.bytesAvailable()) + " free" : QString());
    update_status();
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
                QMessageBox::warning(self, title, msg);
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

void MainWindow::set_folder_previews(bool on)
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
    set_view_value("folder_previews", on);
    if (!on)
        thumbs->cancel_pending();
}

// -- recursive preview build

void MainWindow::build_previews(const QString &root)
{
    if (builder) {
        QMessageBox::information(this, "Generate Previews",
                                 "A preview build is already running. Stop it first (✕ in the status bar).");
        return;
    }
    int size = pane() ? pane()->grid_size : 160;
    auto *b = new RecursiveBuilder(root, size, thumbs);
    connect(b, &RecursiveBuilder::progress, this, &MainWindow::build_progress);
    connect(b, &RecursiveBuilder::finished_build, this, &MainWindow::build_finished);
    connect(b, &QThread::finished, b, &QObject::deleteLater);
    builder = b;
    build_root = root;
    QString name = basename(rstrip(root, '/'));
    build_label->setText("Previews: " + (name.isEmpty() ? root : name));
    build_label->setToolTip(root);
    build_bar->setRange(0, 0);   // busy while scanning
    build_bar->setFormat("Scanning…");
    build_box->show();
    b->start();
}

void MainWindow::build_progress(int done, int total, bool scanning, const QString &current)
{
    if (total == 0) {
        build_bar->setRange(0, 0);
    } else {
        build_bar->setRange(0, total);
        build_bar->setValue(done);
        // total keeps growing while the folder tree is still being walked
        build_bar->setFormat(scanning ? QString("%1 / %2+ scanning…").arg(group_digits(done), group_digits(total))
                                      : QString("%v / %m  (%p%)"));
    }
    build_bar->setToolTip(current);
}

void MainWindow::stop_build()
{
    if (builder) {
        builder->cancel();
        build_label->setText("Stopping…");
        build_stop->setEnabled(false);
    }
}

void MainWindow::build_finished(int done, int total, bool cancelled)
{
    builder = nullptr;
    build_box->hide();
    build_stop->setEnabled(true);
    thumbs->failed.clear();   // items that failed earlier may exist on disk now
    for (MainWindow *w : WINDOWS)
        for (Pane *p : w->panes())
            p->view()->viewport()->update();
    statusBar()->showMessage(QString("%1 building previews: %2 of %3 items")
                                 .arg(cancelled ? "Stopped" : "Finished", group_digits(done), group_digits(total)),
                             8000);
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
        for (const QString &s : sel) {
            if (isdir(s)) {
                dirs += 1;
            } else {
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
    if (idx.isValid()) {
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
    if (paths.isEmpty() && p->is_virtual()) {
        m->addAction("Select All", this, [p]() { p->view()->selectAll(); });
        return m;
    }
    if (paths.isEmpty()) {
        m->addAction(icon({"folder-new"}), "New Folder…", this, [this]() { new_folder(); });
        QMenu *nd = m->addMenu(icon({"document-new"}), "New Document");
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
        QAction *pa = m->addAction(icon({"edit-paste"}), "Paste", this, [this]() { paste(); });
        pa->setEnabled(have_files || (md && md->hasImage()));
        QAction *pl = m->addAction("Paste as Link", this, [this]() { paste(QString(), true); });
        pl->setEnabled(have_files);
        m->addAction("Select All", this, [p]() { p->view()->selectAll(); });
        m->addSeparator();
        QMenu *sm = m->addMenu("Sort By");
        for (int i = 0; i < SORT_COLUMNS.size(); ++i)
            sm->addAction(SORT_COLUMNS[i], this, [p, i]() { p->sort_by(i); });
        sm->addSeparator();
        sm->addAction("Ascending", this,
                      [p]() { p->sort_by(p->tree->header()->sortIndicatorSection(), Qt::AscendingOrder); });
        sm->addAction("Descending", this,
                      [p]() { p->sort_by(p->tree->header()->sortIndicatorSection(), Qt::DescendingOrder); });
        QAction *a = m->addAction("Show Hidden Files", this, [this]() { a_hidden->trigger(); });
        a->setCheckable(true);
        a->setChecked(show_hidden);
        m->addSeparator();
        m->addAction(icon({"utilities-terminal"}), "Open in Terminal", this, [cur]() { open_terminal(cur); });
        m->addAction(icon({"bookmark-new"}), "Bookmark This Folder", this, [this, cur]() { sidebar->add_bookmark(cur); });
        m->addAction(icon({"view-refresh"}), "Generate Previews Recursively", this, [this, cur]() { build_previews(cur); });
        if (in_trash(join(cur, "x"))) {
            m->addAction(icon({"user-trash"}), "Empty Trash", this, [this]() { empty_trash(); });
            if (fileops::can_shred())
                m->addAction(icon({"edit-shred", "edit-delete"}), "Empty Trash with BleachBit…", this,
                             [this]() { empty_trash_with_bleachbit(); });
        }
        sharing::add_scripts_menu(m, {}, cur, [this](const QString &d) { navigate(d); });
        m->addSeparator();
        m->addAction(icon({"document-properties"}), "Properties", this, [this, cur]() { properties({cur}); });
        return m;
    }

    QString single = paths.size() == 1 ? paths.first() : QString();
    bool is_dir = !single.isEmpty() && isdir(single);
    if (in_trash(paths.first())) {
        m->addAction(icon({"edit-undo"}), "Restore", this, [this, paths]() { restore(paths); });
        m->addAction(icon({"edit-delete"}), "Delete Permanently", this, [this, paths]() { delete_paths(paths); });
        if (fileops::can_shred())
            m->addAction(icon({"edit-shred", "edit-delete"}), "Shred with BleachBit…", this,
                         [this, paths]() { shred_paths(paths); });
        m->addSeparator();
        m->addAction(icon({"document-properties"}), "Properties", this, [this, paths]() { properties(paths); });
        return m;
    }

    m->addAction(icon({"document-open"}), "Open", this, [this, p, paths]() { open_paths(p, paths); });
    bool all_dirs = std::all_of(paths.begin(), paths.end(), [](const QString &x) { return isdir(x); });
    if (is_dir || all_dirs) {
        m->addAction(icon({"tab-new"}), "Open in New Tab", this, [this, paths]() {
            for (const QString &x : paths)
                new_tab(x, false);
        });
        m->addAction("Open in New Window", this, [paths]() { open_window(paths); });
    }
    if (is_dir) {
        sharing::add_open_folder_menu(m, single, [this, paths]() { OpenWithDialog(this, paths).exec(); });
    }
    QStringList imgs;
    for (const QString &x : paths)
        if (is_image(x))
            imgs << x;
    if (!imgs.isEmpty())
        m->addAction(icon({"image-x-generic"}), imgs.size() > 1 ? "View Images" : "View Image", this,
                     [this, p, imgs]() { quick_view(p, imgs); });
    if (p->in_search)
        m->addAction(icon({"folder-open"}), "Show in Folder", this, [this, paths]() { reveal(paths.first()); });
    if (!is_dir && !single.isEmpty()) {
        QMenu *ow = m->addMenu("Open With");
        QList<AppRef> rec = apps_for(single).first;
        for (int i = 0; i < std::min<qsizetype>(8, rec.size()); ++i) {
            AppRef app = rec[i];
            ow->addAction(app_icon(app), app_name(app), this, [this, app, paths]() {
                try {
                    launch_app(app, paths);
                } catch (const Error &e) {
                    QMessageBox::warning(this, "Open With", e.message());
                }
            });
        }
        ow->addSeparator();
        ow->addAction("Other Application…", this, [this, paths]() { OpenWithDialog(this, paths).exec(); });
    }
    m->addSeparator();
    m->addAction(icon({"edit-cut"}), "Cut", this, [this, paths]() { clip(true, paths); });
    m->addAction(icon({"edit-copy"}), "Copy", this, [this, paths]() { clip(false, paths); });
    if (is_dir) {
        QAction *pa = m->addAction(icon({"edit-paste"}), "Paste Into Folder", this, [this, single]() { paste(single); });
        pa->setEnabled(!read_clipboard().second.isEmpty());
    }
    m->addAction("Move To…", this, [this, paths]() { transfer_to(paths, "move"); });
    m->addAction("Copy To…", this, [this, paths]() { transfer_to(paths, "copy"); });
    m->addAction("Duplicate", this, [this, paths]() { duplicate(paths); });
    m->addAction(icon({"edit-rename"}), !single.isEmpty() ? QString("Rename…") : QString("Rename %1 Items…").arg(paths.size()),
                 this, [this, paths]() { rename(paths); });
    QMenu *cp = m->addMenu(icon({"edit-copy"}), "Copy Path / Name");
    cp->addAction("Copy Full Path", this, [this, paths]() { copy_text(paths); });
    cp->addAction("Copy Name", this, [this, paths]() {
        QStringList names;
        for (const QString &x : paths)
            names << basename(x);
        copy_text(names);
    });
    cp->addAction("Copy URI", this, [this, paths]() {
        QStringList uris;
        for (const QString &x : paths)
            uris << file_uri(x);
        copy_text(uris);
    });

    bool starred = std::all_of(paths.begin(), paths.end(), [](const QString &x) { return places::is_starred(x); });
    m->addAction(starred ? icon({"non-starred-symbolic", "non-starred"}) : icon({"starred-symbolic", "starred"}),
                 starred ? "Unstar" : "Star", this, [paths, starred]() { places::set_starred(paths, !starred); });

    QString here = p->dir();   // empty in Starred / Recent: no "… Here" there
    QMenu *lm = m->addMenu(icon({"emblem-symbolic-link", "insert-link"}), "Links && Shortcuts");
    if (!here.isEmpty()) {
        lm->addAction("Create Symbolic Link Here", this, [this, paths, here]() { make_links(paths, here, "sym"); });
        lm->addAction("Create Relative Symbolic Link Here", this, [this, paths, here]() { make_links(paths, here, "rel"); });
    }
    if (!here.isEmpty() && std::all_of(paths.begin(), paths.end(), [](const QString &x) { return isfile(x) && !islink(x); }))
        lm->addAction("Create Hard Link Here", this, [this, paths, here]() { make_links(paths, here, "hard"); });
    lm->addAction("Create Symbolic Link In…", this, [this, paths]() { make_links(paths, QString(), "sym"); });
    lm->addSeparator();
    QString desktop = xdg_user_dir("DESKTOP");
    lm->addAction("Send Link to Desktop", this, [this, paths, desktop]() { make_links(paths, desktop, "sym"); });
    if (!here.isEmpty())
        lm->addAction("Create Desktop Shortcut (.desktop) Here", this,
                      [this, paths, here]() { make_links(paths, here, "desktop"); });
    lm->addAction("Send Shortcut (.desktop) to Desktop", this,
                  [this, paths, desktop]() { make_links(paths, desktop, "desktop"); });
    if (std::any_of(paths.begin(), paths.end(), [](const QString &x) { return islink(x); })) {
        lm->addSeparator();
        lm->addAction("Open Link Target Location", this, [this, paths]() { reveal(realpath(paths.first())); });
    }
    if (is_dir) {
        lm->addSeparator();
        lm->addAction(icon({"bookmark-new"}), "Add to Bookmarks", this, [this, single]() { sidebar->add_bookmark(single); });
    }

    m->addSeparator();
    if (!single.isEmpty() && !is_dir && archive::can_extract(single)) {
        QString missing = archive::missing_extract_tool(single);
        if (!missing.isEmpty()) {
            QAction *a = m->addAction(icon({"archive-extract", "package-x-generic"}), QString("Extract (install %1)").arg(missing));
            a->setEnabled(false);
        } else {
            m->addAction(icon({"archive-extract", "package-x-generic"}), "Extract Here", this,
                         [this, single]() { archive_ui::extract_here(this, single); });
            m->addAction(icon({"archive-extract", "package-x-generic"}), "Extract To…", this,
                         [this, single]() { archive_ui::extract_dialog(this, single); });
        }
    }
    QString quick = archive_ui::quick_compress_label(paths);
    if (!quick.isEmpty())
        m->addAction(icon({"package-x-generic", "archive-insert"}), quick, this,
                     [this, paths]() { archive_ui::quick_compress(this, paths); });
    m->addAction(icon({"package-x-generic", "archive-insert"}), "Compress…", this,
                 [this, paths]() { archive_ui::compress_dialog(this, paths); });
    m->addAction(icon({"security-high", "document-properties"}), "Create Checksum File…", this,
                 [this, paths]() { hashcheck::create_dialog(this, paths); });
    if (!single.isEmpty() && (is_image(single) || is_video(single)) && uwp::editor_open())
        m->addAction(icon({"preferences-desktop-wallpaper", "video-display"}), "Add to Selected UWP Monitor", this,
                     [this, single]() { uwp_add(single); });
    if (!single.isEmpty() && is_video(single) && uwp::available())
        m->addAction(icon({"preferences-desktop-wallpaper"}), uwp::wallpaper_label(), this,
                     [single]() { set_wallpaper(single); });
    if (!single.isEmpty() && is_image(single)) {
        QMenu *im = m->addMenu(icon({"image-x-generic"}), "Image");
        im->addAction(uwp::wallpaper_label(), this, [single]() { set_wallpaper(single); });
        im->addAction("Use as Folder Cover", this, [this, single]() { thumbs->set_cover(dirname(single), single); });
        im->addAction("Copy Image to Clipboard", this, [single]() { QGuiApplication::clipboard()->setImage(QImage(single)); });
    }
    QStringList folders;
    for (const QString &x : paths)
        if (isdir(x))
            folders << x;
    if (!folders.isEmpty() && folders.size() == paths.size())
        folder_style_menu(m, folders);
    if (is_dir && thumbs->covers.contains(single))
        m->addAction("Reset Folder Cover", this, [this, single]() { thumbs->set_cover(single, QString()); });
    sharing::add_send_to_menu(m, paths);
    if (is_dir && sharing::can_share())
        m->addAction(icon({"folder-remote", "network-workgroup"}), "Network Sharing…", this,
                     [this, single]() { sharing::share_dialog(this, single); });
    sharing::add_scripts_menu(m, paths, here, [this](const QString &d) { navigate(d); });
    if (is_dir) {
        m->addAction("Regenerate Preview", this, [this, single]() {
            thumbs->invalidate(single);
            atc::announce("folders", {{"paths", QJsonArray{single}}});
        });
        m->addAction(icon({"view-refresh"}), "Generate Previews Recursively", this,
                     [this, single]() { build_previews(single); });
        m->addAction(icon({"utilities-terminal"}), "Open in Terminal", this, [single]() { open_terminal(single); });
    }
    m->addSeparator();
    m->addAction(icon({"user-trash"}), "Move to Trash", this, [this, paths]() { trash_paths(paths); });
    m->addAction(icon({"edit-delete"}), "Delete Permanently…", this, [this, paths]() { delete_paths(paths); });
    if (fileops::can_shred())
        m->addAction(icon({"edit-shred", "edit-delete"}), "Shred with BleachBit…", this,
                     [this, paths]() { shred_paths(paths); });
    m->addSeparator();
    m->addAction(icon({"document-properties"}), "Properties", this, [this, paths]() { properties(paths); });
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
        QMessageBox::warning(this, "UWP", "Couldn't reach UWP. Is its editor window still open?");
}

void MainWindow::properties(const QStringList &paths, QWidget *parent)
{
    if (paths.isEmpty())
        return;
    PropertiesDialog(parent ? parent : this, paths).exec();
    update_status();
}

// -- settings

void MainWindow::preferences()
{
    // A separate, non-modal window: GNOME attaches modal dialogs to their parent ("attach-modal-dialogs"), so
    // dragging a modal Preferences would drag the whole Kestrel window with it.
    if (prefs) {
        prefs->raise();
        prefs->activateWindow();
        return;
    }
    prefs = new PreferencesDialog(this);
    prefs->setWindowModality(Qt::NonModal);
    prefs->setAttribute(Qt::WA_DeleteOnClose);
    connect(prefs, &QDialog::accepted, this, &MainWindow::preferences_saved);
    prefs->show();
}

void repaint_all()
{
    for (MainWindow *w : WINDOWS)
        for (Pane *p : w->panes())
            p->view()->viewport()->update();
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
}

void MainWindow::preferences_saved()
{
    apply_preferences();
    settings().sync();   // other Kestrels read the file as soon as they hear the report
    atc::announce("settings");
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
    QMessageBox box(QMessageBox::Warning, "Delete All Thumbnails",
                    QString("Delete every thumbnail and folder preview %1 has made?\n\n"
                            "• Folder mosaics in %2\n"
                            "• Image/video thumbnails it wrote to %3\n\n"
                            "They will be regenerated as you browse.")
                        .arg(APP_NAME, join(APP_CACHE(), "folders"), THUMB_DIR()),
                    QMessageBox::Cancel, this);
    QPushButton *go = box.addButton("Delete", QMessageBox::DestructiveRole);
    auto *shared = new QCheckBox("Also delete thumbnails made by other apps (GNOME Files, etc.)");
    box.setCheckBox(shared);
    box.exec();
    if (box.clickedButton() != go)
        return;
    thumbs->cancel_pending();
    bool include = shared->isChecked();
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
    QMessageBox::information(this, "Keyboard Shortcuts", text);
}

void MainWindow::closeEvent(QCloseEvent *ev)
{
    QList<Task *> tasks = task_panel->tasks;
    if (!tasks.isEmpty()) {
        // the worker threads report to this window; closing now would drop their results mid-operation
        QStringList names;
        for (int i = 0; i < std::min<qsizetype>(5, tasks.size()); ++i)
            names << "• " + tasks[i]->title;
        QMessageBox box(QMessageBox::Question, "Operations Running",
                        QString("These operations are still running:\n\n%1\n\n"
                                "Stop them and close the window? Items already processed stay processed.")
                            .arg(names.join('\n')),
                        QMessageBox::NoButton, this);
        QPushButton *stop = box.addButton("Stop and Close", QMessageBox::DestructiveRole);
        box.addButton("Keep Running", QMessageBox::RejectRole);
        box.exec();
        ev->ignore();
        if (box.clickedButton() == stop) {
            for (Task *t : tasks)
                t->cancel();
            task_panel->refresh();
            centralWidget()->setEnabled(false);   // stay visible (showing "cancelling…") until stopped
            close_when_idle();
        }
        return;
    }
    if (!chooser_mode || !(isMaximized() || isFullScreen()))   // a chooser always opens as a smaller window
        set_view_value("geometry", saveGeometry());
    set_view_value("splitter", split->saveState());
    if (chooser)
        chooser->finish(false);   // closed without choosing: cancelled
    for (Pane *p : panes())
        p->stop_search();
    if (builder) {
        builder->cancel();
        builder->wait(5000);
    }
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
    if (!isdir(start))
        start = settings().value("chooser_folder").toString();   // where the last chooser picked something
    if (!isdir(start))
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
    stats::print_at_quit();   // KESTREL_STATS=1
    qRegisterMetaType<fileops::DirStats>();
    setup_icon_theme();
    follow_gtk_theme();   // Qt < 6.5: the GTK theme's colours, following changes
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
