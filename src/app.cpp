#include "app.h"

#include "admin.h"
#include "animate.h"
#include "archive.h"
#include "archive_ui.h"
#include "atc.h"
#include "chooser.h"
#include "focus.h"
#include "dialogs.h"
#include "fileops.h"
#include "fm1.h"
#include "overview.h"
#include "places.h"
#include "sharing.h"
#include "undo.h"
#include "proc.h"
#include "thumbs.h"
#include "util.h"
#include "uwp.h"
#include "viewer.h"
#include "widgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDrag>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStorageInfo>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <csignal>
#include <cstdio>
#include <cstdlib>

using namespace util;

static const int GRID_MIN = 48, GRID_MAX = 320;
static const int LIST_MIN = 16, LIST_MAX = 128;
static const int GRID_DEFAULT = 160, LIST_DEFAULT = 28;
static const int CHOOSER_GRID = 96, CHOOSER_LIST = 24;   // a chooser window starts with smaller icons
static const QStringList SORT_COLUMNS = {"Name", "Size", "Type", "Modified"};
static QList<MainWindow *> WINDOWS;
static QPointer<MainWindow> last_active;   // the window used most recently
static qint64 last_active_ms = 0;
static void report_windows();
static ThumbnailManager *g_thumbs = nullptr;

static QIcon icon(const QStringList &names) { return theme_icon(names); }

// ---------------------------------------------------------------- pane

// The file views. Their drags are Qt's, apart from giving the focus to the app the files are dropped into (focus.h).
template <class View>
class FileView : public View {
protected:
    void startDrag(Qt::DropActions supported) override
    {
        QModelIndexList indexes;
        for (const QModelIndex &i : this->selectedIndexes())
            if (this->model()->flags(i) & Qt::ItemIsDragEnabled)
                indexes << i;
        QMimeData *data = indexes.isEmpty() ? nullptr : this->model()->mimeData(indexes);
        if (!data)
            return;
        auto *drag = new QDrag(this);
        drag->setMimeData(data);
        QPoint hot;
        drag->setPixmap(drag_pixmap(indexes, &hot));
        drag->setHotSpot(hot);
        Qt::DropAction def = this->defaultDropAction();
        if (def == Qt::IgnoreAction || !(supported & def))
            def = supported & Qt::CopyAction ? Qt::CopyAction : Qt::IgnoreAction;
        if (drag->exec(supported, def) != Qt::IgnoreAction && !drag->target())   // dropped into another app
            focus::activate_at_pointer();
    }

private:
    QPixmap drag_pixmap(const QModelIndexList &indexes, QPoint *hot) const
    {
        // the dragged items as they look in the view (what Qt draws)
        QRect all;
        for (const QModelIndex &i : indexes)
            all |= this->visualRect(i).intersected(this->viewport()->rect());
        *hot = this->viewport()->mapFromGlobal(QCursor::pos()) - all.topLeft();
        if (all.isEmpty())
            return QPixmap();
        qreal dpr = this->devicePixelRatioF();
        QPixmap pm(all.size() * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        for (const QModelIndex &i : indexes) {
            QRect r = this->visualRect(i);
            if (!r.intersects(this->viewport()->rect()))
                continue;
            QStyleOptionViewItem opt;
            this->initViewItemOption(&opt);
            opt.rect = r.translated(-all.topLeft());
            opt.state |= QStyle::State_Selected;
            this->itemDelegateForIndex(i)->paint(&p, opt, i);
        }
        return pm;
    }
};

Pane::Pane(MainWindow *win, const QString &start) : win(win), thumbs(win->thumbs)
{
    grid_size = win->view_value("grid_size").toInt();
    list_size = win->view_value("list_size").toInt();

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    // search bar
    search_bar = new QWidget;
    auto *sl = new QHBoxLayout(search_bar);
    sl->setContentsMargins(6, 4, 6, 4);
    search_edit = new QLineEdit;
    search_edit->setPlaceholderText("Search… (supports * and ? wildcards)");
    search_edit->setClearButtonEnabled(true);
    search_sub = new QCheckBox("Include subfolders");
    search_sub->setChecked(win->view_value("search_recursive", false).toBool());
    auto *close = new QToolButton;
    close->setIcon(icon({"window-close-symbolic", "window-close"}));
    close->setAutoRaise(true);
    connect(close, &QToolButton::clicked, this, [this]() { close_search(); });
    sl->addWidget(search_edit, 1);
    search_contents = new QCheckBox("File contents");
    search_contents->setToolTip("Search inside files too, using the desktop's search index (localsearch).\n"
                                "Includes subfolders; only finds files in indexed folders.");
    search_contents->setChecked(win->view_value("search_contents", false).toBool());
    connect(search_contents, &QCheckBox::toggled, this, [this](bool v) {
        this->win->set_view_value("search_contents", v);
        do_search();
    });
    sl->addWidget(search_sub);
    sl->addWidget(search_contents);
    // only once it has a parent: showing a parentless widget opens it as a window of its own, which on Wayland
    // uses up the launch's activation token and leaves GNOME's busy cursor spinning until it times out
    search_contents->setVisible(can_search_contents());
    sl->addWidget(close);
    search_bar->hide();
    search_timer = new QTimer(this);
    search_timer->setSingleShot(true);
    search_timer->setInterval(250);
    connect(search_timer, &QTimer::timeout, this, &Pane::do_search);
    connect(search_edit, &QLineEdit::textChanged, this, [this]() { search_timer->start(); });
    connect(search_sub, &QCheckBox::toggled, this, [this](bool v) {
        this->win->set_view_value("search_recursive", v);
        do_search();
    });
    search_edit->installEventFilter(this);
    lay->addWidget(search_bar);

    stack = new QStackedWidget;
    grid = new FileView<QListView>;
    tree = new FileView<QTreeView>;
    setup_grid();
    setup_tree();
    stack->addWidget(grid);
    stack->addWidget(tree);
    mode_view = grid;
    overview = new OverviewPage(thumbs);
    connect(overview, &OverviewPage::navigate, win, &MainWindow::navigate);
    connect(overview, &OverviewPage::open_in_tab, win, [win](const QString &t) { win->open_location(t, true); });
    connect(overview, &OverviewPage::sidebar_changed, win, [win]() { win->sidebar->refresh(); });
    connect(overview, &OverviewPage::add_bookmark, win, [win](const QString &t) { win->sidebar->add_bookmark(t); });
    connect(overview, &OverviewPage::properties, win, [win](const QString &t) { win->properties({t}); });
    stack->addWidget(overview);
    lay->addWidget(stack);
    empty = new QLabel("Folder is empty", stack);
    empty->setAlignment(Qt::AlignCenter);
    empty->setStyleSheet("color: palette(placeholder-text); font-size: 16px; background: transparent");
    empty->setAttribute(Qt::WA_TransparentForMouseEvents);
    empty->hide();

    model = make_model();
    search_model = new SearchModel(thumbs, this);
    connect(places::signals_(), &places::Signals::starred_changed, this, &Pane::starred_changed);
    attach(model);
    set_view_mode(win->view_value("view_mode", "grid").toString());
    if (win->chooser)
        set_type_filter(win->chooser->type_filter());
    set_path(start);
}

void Pane::set_type_filter(const QStringList &globs)
{
    type_filters = globs;
    QDir::Filters f = model->filter();
    // name filters also hide folders unless AllDirs is set
    model->setFilter(globs.isEmpty() ? f & ~QDir::AllDirs : f | QDir::AllDirs);
    if (!in_search && !search_bar->isVisible())
        model->setNameFilters(globs);
}

Pane::~Pane()
{
    if (search_thread) {
        search_thread->stop = true;
        search_thread->wait();
    }
}

// -- setup

void Pane::setup_common(QAbstractItemView *v)
{
    v->setSelectionMode(QAbstractItemView::ExtendedSelection);
    v->setEditTriggers(QAbstractItemView::NoEditTriggers);
    v->setDragEnabled(true);
    v->setAcceptDrops(true);
    v->setDropIndicatorShown(true);
    v->setDragDropMode(QAbstractItemView::DragDrop);
    v->setDefaultDropAction(Qt::MoveAction);
    v->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(v, &QWidget::customContextMenuRequested, this, [this, v](const QPoint &pos) { win->context_menu(this, v, pos); });
    connect(v, &QAbstractItemView::doubleClicked, this, &Pane::double_clicked);
    connect(v, &QAbstractItemView::clicked, this, &Pane::clicked);
    v->installEventFilter(this);
    v->viewport()->installEventFilter(this);
    v->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    v->verticalScrollBar()->setSingleStep(40);
}

void Pane::setup_grid()
{
    QListView *g = grid;
    g->setViewMode(QListView::IconMode);
    g->setMovement(QListView::Static);
    g->setResizeMode(QListView::Adjust);
    g->setWrapping(true);
    g->setUniformItemSizes(true);
    g->setLayoutMode(QListView::Batched);
    g->setBatchSize(400);
    g->setMouseTracking(true);
    g->setFrameShape(QFrame::NoFrame);
    MainWindow *w = win;
    delegate = new GridDelegate(this, thumbs, [w](const QString &p) { return w->cut_paths.contains(p); }, g);
    animator = new Animator(g, this);
    delegate->animator = animator;
    delegate->icon_size = grid_size;
    g->setItemDelegate(delegate);
    setup_common(g);
}

void Pane::setup_tree()
{
    QTreeView *t = tree;
    t->setRootIsDecorated(false);
    t->setItemsExpandable(false);
    t->setUniformRowHeights(true);
    t->setAlternatingRowColors(true);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSortingEnabled(true);
    t->setFrameShape(QFrame::NoFrame);
    t->setIconSize(QSize(list_size, list_size));
    int col = win->view_value("sort_col", 0).toInt();
    auto order = Qt::SortOrder(win->view_value("sort_order", 0).toInt());
    t->header()->setSortIndicator(col, order);
    connect(t->header(), &QHeaderView::sortIndicatorChanged, this, &Pane::sort_changed);
    setup_common(t);
}

FSModel *Pane::make_model()
{
    auto *m = new FSModel(thumbs, this);
    MainWindow *w = win;
    m->drop_handler = [w](const QStringList &paths, const QString &target) { w->handle_drop(paths, target); };
    QDir::Filters f = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (win->show_hidden)
        f |= QDir::Hidden;
    m->setFilter(f);
    m->setNameFilterDisables(false);
    connect(m, &QFileSystemModel::directoryLoaded, this, &Pane::dir_loaded);
    connect(m, &QAbstractItemModel::rowsInserted, this, &Pane::update_empty);
    connect(m, &QAbstractItemModel::rowsRemoved, this, &Pane::update_empty);
    m->thumb_size = grid_size;
    return m;
}

void Pane::attach(QAbstractItemModel *m)
{
    QItemSelectionModel *old_shared = tree->selectionModel();
    grid->setModel(m);
    QItemSelectionModel *grid_own = grid->selectionModel();
    tree->setModel(m);
    grid->setSelectionModel(tree->selectionModel());
    for (QItemSelectionModel *sm : {grid_own, old_shared})
        if (sm && sm != tree->selectionModel())
            sm->deleteLater();
    connect(tree->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this]() { Q_EMIT selection_changed(); });
    QHeaderView *hdr = tree->header();
    hdr->setSectionResizeMode(0, QHeaderView::Interactive);
    hdr->resizeSection(0, 380);
    hdr->setStretchLastSection(true);
    apply_folder_previews();
}

void Pane::use_model_root()
{
    QModelIndex root = model->index(path);
    grid->setRootIndex(root);
    tree->setRootIndex(root);
}

// -- view mode / zoom

bool Pane::is_grid() const { return mode_view == grid; }
QAbstractItemView *Pane::view() const { return mode_view; }
bool Pane::is_overview() const { return path == OVERVIEW; }
bool Pane::is_trash() const { return path == join(TRASH_DIR(), "files"); }
bool Pane::is_virtual() const { return places::is_virtual(path); }
bool Pane::is_listing() const { return is_trash() || is_virtual(); }
QString Pane::dir() const { return is_overview() || is_virtual() ? QString() : path; }   // empty: no folder

void Pane::set_view_mode(const QString &mode)
{
    mode_view = mode == "grid" ? static_cast<QAbstractItemView *>(grid) : static_cast<QAbstractItemView *>(tree);
    if (!is_overview())
        stack->setCurrentWidget(mode_view);
    apply_folder_previews();
    update_grid_size();
}

void Pane::apply_folder_previews()
{
    bool g = mode_view == grid;
    bool on = win->folder_previews && (g || settings().value("list_folder_previews", false).toBool());
    int size = g ? grid_size : std::max(list_size, 96);
    if (model) {
        model->folder_previews = on;
        model->thumb_size = size;
    }
    if (search_model) {
        search_model->folder_previews = on;
        search_model->thumb_size = size;
    }
}

void Pane::zoom(int step, int absolute)
{
    if (is_grid()) {
        int cur = grid_size;
        int nw = absolute >= 0 ? absolute : int(cur * (step > 0 ? 1.15 : 1 / 1.15));
        grid_size = std::max(GRID_MIN, std::min(GRID_MAX, nw));
        delegate->icon_size = grid_size;
        win->set_view_value("grid_size", grid_size);
        update_grid_size();
    } else {
        int cur = list_size;
        int nw = absolute >= 0 ? absolute : cur + (step > 0 ? 8 : -8);
        list_size = std::max(LIST_MIN, std::min(LIST_MAX, nw));
        tree->setIconSize(QSize(list_size, list_size));
        win->set_view_value("list_size", list_size);
    }
    apply_folder_previews();
    view()->viewport()->update();
    win->sync_zoom_slider();
}

int Pane::zoom_value() const { return is_grid() ? grid_size : list_size; }

void Pane::update_grid_size()
{
    QSize cell = delegate->cell_size();
    // Width as if the scrollbar were always shown: otherwise the scrollbar appearing/disappearing changes the
    // column width and the grid re-flows.
    int sb = grid->verticalScrollBar()->isVisible() ? 0 : grid->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    int avail = grid->viewport()->width() - sb;
    int cols = std::max(1, avail / cell.width());
    int extra = cols > 1 ? (avail - cols * cell.width()) / cols : 0;
    QSize size(cell.width() + std::max(0, extra), cell.height());
    if (size != grid->gridSize())
        grid->setGridSize(size);
}

// -- navigation

bool Pane::set_path(const QString &target, bool record, const QString &select_in)
{
    QString select = select_in;
    if (target == OVERVIEW) {
        if (record && !path.isEmpty() && path != OVERVIEW) {
            back_stack << snapshot(OVERVIEW);
            fwd_stack.clear();
        }
        if (in_search || search_bar->isVisible())
            close_search(false, true);
        path = OVERVIEW;
        view()->clearSelection();
        stack->setCurrentWidget(overview);
        empty->hide();
        Q_EMIT path_changed();
        return true;
    }
    if (places::is_virtual(target)) {
        if (record && !path.isEmpty() && path != target) {
            back_stack << snapshot(target);
            fwd_stack.clear();
        }
        if (in_search || search_bar->isVisible())
            close_search(false, true);
        path = target;
        stack->setCurrentWidget(mode_view);
        view()->selectionModel()->clear();
        pending_select = select;
        Q_EMIT path_changed();
        show_trash();
        return true;
    }
    QString p = abspath(expanduser(target));
    if (isfile(p)) {
        select = p;
        p = dirname(p);
    }
    if (!isdir(p)) {
        QMessageBox::warning(this, "Not found", QString("“%1” does not exist.").arg(p));
        return false;
    }
    if (!util::access(p, R_OK | X_OK)) {
        QMessageBox::warning(this, "Permission denied", QString("You don't have permission to open “%1”.").arg(p));
        return false;
    }
    QString prev = path;
    bool searching_now = search_bar->isVisible() && !search_edit->text().trimmed().isEmpty();
    if (record && !prev.isEmpty() && (prev != p || searching_now)) {
        back_stack << snapshot(p);
        fwd_stack.clear();
    }
    if (in_search || search_bar->isVisible())
        close_search(false, true);
    path = p;
    stack->setCurrentWidget(mode_view);
    thumbs->cancel_pending();
    animator->clear();
    model->setNameFilters(type_filters);
    QModelIndex root = model->setRootPath(p);
    grid->setRootIndex(root);
    tree->setRootIndex(root);
    view()->selectionModel()->clear();   // drop the previous folder's selection and current item
    pending_select = !select.isEmpty() ? select : (!prev.isEmpty() && dirname(prev) == p ? prev : QString());
    QTimer::singleShot(0, this, &Pane::try_select);
    grid->scrollToTop();
    tree->scrollToTop();
    update_empty();
    Q_EMIT path_changed();
    if (is_trash())
        show_trash();
    return true;
}

// -- combined trash

void Pane::show_trash()
{
    // list the items of every trash folder (home + each drive's .Trash-$uid) in the results model, with the folder
    // each item was deleted from as its Location; or the Starred / Recent files with their folder. Loaded on a thread.
    trash_gen += 1;
    int gen = trash_gen;
    QString place = is_trash() ? QString() : path;
    fileops::run_task(
        this, "",
        [place]() {
            QVariantList items;
            for (const auto &[p, o] : place.isEmpty() ? trashed_items() : places::items(place))
                items << QVariant(QStringList{p, o});
            return QVariant(items);
        },
        [this, gen](const QVariant &items) { fill_trash(items, gen); }, true);
}

void Pane::starred_changed()
{
    if (path == places::STARRED)
        show_trash();
    view()->viewport()->update();
}

void Pane::fill_trash(const QVariant &items_v, int gen)
{
    if (gen != trash_gen || !is_listing())
        return;
    QString text = search_bar->isVisible() ? search_edit->text().trimmed().toLower() : QString();
    QStringList paths;
    QHash<QString, QString> locations;
    for (const QVariant &v : items_v.toList()) {
        QStringList po = v.toStringList();
        QString p = po.value(0), o = po.value(1);
        if (!text.isEmpty() && !basename(p).toLower().contains(text))
            continue;
        paths << p;
        locations.insert(p, o.isEmpty() ? QString("(unknown)") : dirname(o));
    }
    if (!in_search) {
        in_search = true;
        attach(search_model);
        grid->setRootIndex(QModelIndex());
        tree->setRootIndex(QModelIndex());
    }
    QStringList sel = selected_paths();
    search_model->clear_results();
    add_trash_rows(paths, locations, gen, sel);
    if (!trash_watch) {
        trash_watch = new QFileSystemWatcher(this);
        connect(trash_watch, &QFileSystemWatcher::directoryChanged, this, &Pane::trash_changed);
    }
    QStringList fresh;
    for (const QString &r : trash_dirs()) {
        QString d = join(r, "files");
        if (!trash_watch->directories().contains(d))
            fresh << d;
    }
    if (!fresh.isEmpty())
        trash_watch->addPaths(fresh);
}

void Pane::add_trash_rows(QStringList paths, const QHash<QString, QString> &locations, int gen, const QStringList &sel)
{
    // in chunks, so a trash with tens of thousands of items doesn't block the window
    if (gen != trash_gen || !is_listing())
        return;
    search_model->add_paths(paths.mid(0, 1000), locations);
    if (paths.size() > 1000) {
        QStringList rest = paths.mid(1000);
        QTimer::singleShot(0, this, [this, rest, locations, gen, sel]() { add_trash_rows(rest, locations, gen, sel); });
        return;
    }
    if (!sel.isEmpty()) {
        QStringList keep;
        for (const QString &p : sel)
            if (search_model->rows.contains(p))
                keep << p;
        select_paths(keep);
    }
    try_select();
    update_empty();
    if (win->pane() == this)
        win->update_status();
}

void Pane::trash_changed()
{
    if (is_trash() && !trash_reload) {
        trash_reload = true;   // coalesce bursts (deleting thousands of items) into one reload
        QTimer::singleShot(400, this, [this]() {
            trash_reload = false;
            if (is_trash())
                show_trash();
        });
    }
}

void Pane::dir_loaded(const QString &p)
{
    if (p == path) {
        update_empty();
        if (win->pane() == this)
            win->update_status();
        QTimer::singleShot(30, this, &Pane::try_select);
    }
}

void Pane::try_select()
{
    if (pending_select.isEmpty() || is_overview())
        return;
    QModelIndex idx = index_for(pending_select);
    if (idx.isValid()) {
        select_paths({pending_select});
        pending_select.clear();
    }
}

void Pane::select_later(const QString &p)
{
    // select path once it shows up in the model (after create/rename/paste)
    pending_select = p;
    for (int ms : {60, 300, 1000, 2500})
        QTimer::singleShot(ms, this, &Pane::try_select);
}

void Pane::select_paths(const QStringList &paths)
{
    if (is_overview())
        return;
    QItemSelectionModel *sm = view()->selectionModel();
    sm->clearSelection();
    QModelIndex first;
    for (const QString &p : paths) {
        QModelIndex idx = index_for(p);
        if (idx.isValid()) {
            sm->select(idx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
            if (!first.isValid())
                first = idx;
        }
    }
    if (first.isValid()) {
        sm->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
        view()->scrollTo(first);
        // large folders are laid out in batches, so the item may not have its final position yet: scroll again once
        // layout has caught up (unless the user has moved on to another item)
        QString target = first.siblingAtColumn(0).data(PathRole).toString();
        for (int ms : {50, 250, 700})
            QTimer::singleShot(ms, this, [this, target]() { scroll_to_current(target); });
    }
}

void Pane::scroll_to_current(const QString &p)
{
    QModelIndex idx = index_for(p);
    QModelIndex cur = view()->currentIndex();
    if (idx.isValid() && cur.isValid() && cur.siblingAtColumn(0) == idx.siblingAtColumn(0))
        view()->scrollTo(idx);
}

QModelIndex Pane::index_for(const QString &p) const
{
    if (in_search) {
        QStandardItem *item = search_model->rows.value(p);
        return item ? item->index() : QModelIndex();
    }
    return model->index(p);
}

Pane::HistoryEntry Pane::snapshot(const QString &dest, bool search) const
{
    // history entry for the current location: the item to refocus on returning (the folder that leads to dest,
    // else the current item) and, if search, the active search
    HistoryEntry e;
    if (is_overview()) {
        e.path = OVERVIEW;
        return e;
    }
    QString focus;
    if (!dest.isEmpty() && dest != OVERVIEW) {
        if (in_search) {
            focus = search_model->rows.contains(dest) ? dest : QString();
        } else if (dest.startsWith(rstrip(path, '/') + "/")) {
            focus = join(path, relpath(dest, path).section('/', 0, 0));
        }
    }
    if (focus.isEmpty()) {
        QString cur = current_path();   // the view's current index can be left over from a previous folder
        if (!cur.isEmpty() && (in_search ? search_model->rows.contains(cur) : dirname(cur) == path))
            focus = cur;
    }
    QString text = search && search_bar->isVisible() ? search_edit->text().trimmed() : QString();
    e.path = path;
    e.focus = focus;
    if (!text.isEmpty()) {
        e.has_search = true;
        e.search_text = text;
        e.search_recursive = search_sub->isChecked();
    }
    return e;
}

void Pane::restore(const HistoryEntry &entry)
{
    if (!set_path(entry.path, false, entry.focus))
        return;
    if (entry.has_search) {
        search_sub->blockSignals(true);
        search_sub->setChecked(entry.search_recursive);
        search_sub->blockSignals(false);
        search_edit->blockSignals(true);
        search_edit->setText(entry.search_text);
        search_edit->blockSignals(false);
        search_bar->show();
        search_recorded = true;   // its folder is already in the history
        do_search();
        Q_EMIT path_changed();
    }
}

void Pane::go_back()
{
    if (!back_stack.isEmpty()) {
        HistoryEntry entry = back_stack.takeLast();
        fwd_stack << snapshot(entry.path);
        restore(entry);
    }
}

void Pane::go_forward()
{
    if (!fwd_stack.isEmpty()) {
        HistoryEntry entry = fwd_stack.takeLast();
        back_stack << snapshot(entry.path);
        restore(entry);
    }
}

void Pane::go_up()
{
    if (is_overview() || is_virtual())
        return;
    QString parent = dirname(path);
    if (parent != path)
        set_path(parent, true, path);
}

void Pane::refresh()
{
    if (is_overview()) {
        overview->refresh();
        return;
    }
    if (is_listing()) {
        show_trash();
        return;
    }
    for (const QString &p : all_paths())
        if (isdir(p))
            thumbs->invalidate(p);
    QStringList sel = selected_paths();
    FSModel *old = model;
    model = make_model();
    if (!in_search) {
        attach(model);
        QModelIndex root = model->setRootPath(path);
        grid->setRootIndex(root);
        tree->setRootIndex(root);
        pending_select = sel.isEmpty() ? QString() : sel.first();
    }
    old->deleteLater();
    update_grid_size();
}

void Pane::apply_hidden()
{
    QDir::Filters f = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (win->show_hidden)
        f |= QDir::Hidden;
    if (!type_filters.isEmpty())
        f |= QDir::AllDirs;   // a chooser's file types don't hide folders
    model->setFilter(f);
}

void Pane::update_empty()
{
    if (is_overview()) {
        empty->hide();
        return;
    }
    QString text;
    if (in_search && is_listing()) {
        bool searching_text = search_bar->isVisible() && !search_edit->text().trimmed().isEmpty();
        QString none = is_trash() ? QString("Trash is empty") : places::empty_text(path);
        if (!search_model->rowCount())
            text = searching_text ? QString("No matches") : none;
    } else if (in_search) {
        if (search_model->rowCount() == 0 && !searching())
            text = "No results";
    } else {
        int n = path.isEmpty() ? 0 : model->rowCount(model->index(path));
        if (n == 0)
            text = "Folder is empty";
    }
    empty->setText(text);
    empty->setVisible(!text.isEmpty());
    empty->setGeometry(stack->rect());
}

bool Pane::searching() const { return search_thread && search_thread->isRunning(); }

// -- search

void Pane::start_search()
{
    if (is_overview())
        return;
    search_bar->show();
    search_edit->setFocus();
    search_edit->selectAll();
}

void Pane::close_search(bool refocus, bool navigating)
{
    if (!navigating)
        unrecord_search();   // a cancelled search leaves no step in the history
    search_recorded = false;
    search_edit->blockSignals(true);
    search_edit->clear();
    search_edit->blockSignals(false);
    search_bar->hide();
    stop_search();
    model->setNameFilters(type_filters);
    if (in_search) {
        in_search = false;
        attach(model);
        use_model_root();
    }
    update_empty();
    if (refocus)
        view()->setFocus();
    Q_EMIT path_changed();
    if (is_trash())
        show_trash();
}

void Pane::stop_search()
{
    if (search_thread) {
        search_thread->stop = true;
        search_thread->wait(2000);
        search_thread = nullptr;
    }
}

void Pane::record_search()
{
    // a search is its own step in the history: Back from it returns to the plain folder
    if (!search_recorded) {
        search_recorded = true;
        back_stack << snapshot(QString(), false);
        fwd_stack.clear();
        Q_EMIT path_changed();
    }
}

void Pane::unrecord_search()
{
    if (search_recorded) {
        search_recorded = false;
        if (!back_stack.isEmpty() && back_stack.last().path == path && !back_stack.last().has_search)
            back_stack.removeLast();
        Q_EMIT path_changed();
    }
}

void Pane::do_search()
{
    QString text = search_edit->text().trimmed();
    stop_search();
    if (!text.isEmpty())
        record_search();
    else
        unrecord_search();
    if (is_listing()) {   // filter the combined trash / Starred / Recent list by name
        show_trash();
        return;
    }
    if (text.isEmpty()) {
        model->setNameFilters(type_filters);
        if (in_search) {
            in_search = false;
            attach(model);
            use_model_root();
        }
        update_empty();
        return;
    }
    bool contents = can_search_contents() && search_contents->isChecked();
    if (search_sub->isChecked() || contents) {
        if (!in_search) {
            in_search = true;
            attach(search_model);
            grid->setRootIndex(QModelIndex());
            tree->setRootIndex(QModelIndex());
        }
        search_model->clear_results();
        auto *t = new SearchThread(path, text, win->show_hidden, nullptr, contents);
        connect(t, &SearchThread::found, this, [this, t](const QStringList &paths) {
            if (t != search_thread)
                return;   // a search that was replaced
            search_model->add_paths(paths);
            try_select();
            update_empty();
        });
        connect(t, &QThread::finished, this, [this, t]() {
            if (t == search_thread) {
                update_empty();
                win->update_status();
            }
        });
        connect(t, &QThread::finished, t, &QObject::deleteLater);
        search_thread = t;
        t->start();
    } else {
        if (in_search) {
            in_search = false;
            attach(model);
            use_model_root();
        }
        bool wild = text.contains('*') || text.contains('?') || text.contains('[');
        model->setNameFilters({wild ? text : "*" + text + "*"});
    }
    update_empty();
    win->update_status();
}

// -- selection helpers

QStringList Pane::selected_paths() const
{
    if (is_overview())
        return {};
    QItemSelectionModel *sm = view()->selectionModel();
    if (!sm)
        return {};
    QList<QPair<int, QModelIndex>> rows;
    for (const QModelIndex &i : sm->selectedIndexes()) {
        QPair<int, QModelIndex> key(i.row(), i.parent());
        if (!rows.contains(key))
            rows << key;
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    QAbstractItemModel *m = view()->model();
    QStringList out;
    for (const auto &[row, parent] : rows) {
        QString p = m->index(row, 0, parent).data(PathRole).toString();
        if (!p.isEmpty())
            out << p;
    }
    return out;
}

QStringList Pane::all_paths() const
{
    if (is_overview())
        return {};
    QStringList out;
    if (in_search) {
        for (int r = 0; r < search_model->rowCount(); ++r)
            out << search_model->item(r, 0)->data(PathRole).toString();
        return out;
    }
    QModelIndex root = model->index(path);
    for (int r = 0; r < model->rowCount(root); ++r)
        out << model->filePath(model->index(r, 0, root));
    return out;
}

QString Pane::current_path() const
{
    QModelIndex idx = view()->currentIndex();
    return idx.isValid() ? idx.siblingAtColumn(0).data(PathRole).toString() : QString();
}

void Pane::invert_selection()
{
    QStringList sel = selected_paths();
    QSet<QString> s(sel.begin(), sel.end());
    QStringList inv;
    for (const QString &p : all_paths())
        if (!s.contains(p))
            inv << p;
    select_paths(inv);
}

void Pane::sort_changed(int col, Qt::SortOrder order)
{
    win->set_view_value("sort_col", col);
    win->set_view_value("sort_order", int(order));
}

void Pane::sort_by(int col, int order)
{
    Qt::SortOrder o = order < 0 ? tree->header()->sortIndicatorOrder() : Qt::SortOrder(order);
    tree->header()->setSortIndicator(col, o);
    tree->model()->sort(col, o);
}

// -- activation

void Pane::clicked(const QModelIndex &idx)
{
    if (settings().value("single_click", false).toBool() && QGuiApplication::keyboardModifiers() == Qt::NoModifier)
        win->open_paths(this, {idx.siblingAtColumn(0).data(PathRole).toString()});
}

void Pane::double_clicked(const QModelIndex &idx)
{
    if (!settings().value("single_click", false).toBool())
        win->open_paths(this, {idx.siblingAtColumn(0).data(PathRole).toString()});
}

bool Pane::eventFilter(QObject *obj, QEvent *ev)
{
    QEvent::Type t = ev->type();
    if (obj == search_edit && t == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(ev);
        if (ke->key() == Qt::Key_Escape) {
            close_search();
            return true;
        }
        if (ke->key() == Qt::Key_Down || ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            view()->setFocus();
            if (view()->model()->rowCount(view()->rootIndex())) {
                QModelIndex first = view()->model()->index(0, 0, view()->rootIndex());
                if (!view()->currentIndex().isValid())
                    view()->setCurrentIndex(first);
            }
            return true;
        }
        return false;
    }
    if ((obj == grid || obj == tree) && t == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(ev);
        int k = ke->key();
        Qt::KeyboardModifiers mods = ke->modifiers();
        if ((k == Qt::Key_Return || k == Qt::Key_Enter) && !(mods & Qt::AltModifier)) {
            QStringList paths = selected_paths();
            if (!paths.isEmpty())
                win->open_paths(this, paths, mods & Qt::ControlModifier);
            return true;
        }
        if (k == Qt::Key_Space && mods == Qt::NoModifier) {
            QStringList paths = selected_paths();
            if (!paths.isEmpty())
                win->quick_view(this, paths);
            return true;
        }
        if (k == Qt::Key_Escape && search_bar->isVisible()) {
            close_search();
            return true;
        }
    }
    if (obj == grid->viewport() || obj == tree->viewport()) {
        if (t == QEvent::Wheel && (static_cast<QWheelEvent *>(ev)->modifiers() & Qt::ControlModifier)) {
            zoom(static_cast<QWheelEvent *>(ev)->angleDelta().y() > 0 ? 1 : -1);
            return true;
        }
        if (t == QEvent::MouseButtonPress) {
            Qt::MouseButton b = static_cast<QMouseEvent *>(ev)->button();
            if (b == Qt::BackButton) {
                go_back();
                return true;
            }
            if (b == Qt::ForwardButton) {
                go_forward();
                return true;
            }
        }
        if (t == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(ev)->button() == Qt::MiddleButton) {
            QModelIndex idx = view()->indexAt(static_cast<QMouseEvent *>(ev)->position().toPoint());
            QString p = idx.isValid() ? idx.siblingAtColumn(0).data(PathRole).toString() : QString();
            if (!p.isEmpty() && isdir(p)) {
                win->new_tab(p, false);
                return true;
            }
        }
        if (t == QEvent::Resize) {
            if (obj == grid->viewport())
                QTimer::singleShot(0, this, &Pane::update_grid_size);
            empty->setGeometry(stack->rect());
        }
    }
    return QWidget::eventFilter(obj, ev);
}

QString Pane::title() const
{
    if (is_overview())
        return OVERVIEW_TITLE;
    if (is_listing() && !(search_bar->isVisible() && !search_edit->text().trimmed().isEmpty()))
        return is_trash() ? QString("Trash") : places::title(path);
    if (in_search)
        return "Search: " + search_edit->text();
    QString b = basename(rstrip(path, '/'));
    return b.isEmpty() ? "/" : b;
}

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
    view_btn->setAutoRaise(true);
    connect(view_btn, &QToolButton::clicked, this, &MainWindow::toggle_view);
    tb->addWidget(view_btn);
    sort_btn = new QToolButton;
    sort_btn->setAutoRaise(true);
    sort_btn->setIcon(icon({"view-sort-ascending-symbolic", "view-sort-ascending"}));
    sort_btn->setToolTip("Sort");
    sort_btn->setPopupMode(QToolButton::InstantPopup);
    sort_menu = new QMenu(this);
    connect(sort_menu, &QMenu::aboutToShow, this, &MainWindow::fill_sort_menu);
    sort_btn->setMenu(sort_menu);
    tb->addWidget(sort_btn);
    menu_btn = new QToolButton;
    menu_btn->setAutoRaise(true);
    menu_btn->setIcon(icon({"open-menu-symbolic", "application-menu", "preferences-system"}));
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
        if (in_trash(join(cur, "x")))
            m->addAction(icon({"user-trash"}), "Empty Trash", this, [this]() { empty_trash(); });
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

static void repaint_all()
{
    for (MainWindow *w : WINDOWS)
        for (Pane *p : w->panes())
            p->view()->viewport()->update();
}

static void apply_preferences()
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

// ---- Preferences → "Open folders from other apps as tabs in an open Kestrel window"

static void report_windows()
{
    // for the tower's Handoff: whether we have windows, and when one was last used
    atc::announce("windows", {{"count", int(WINDOWS.size())}, {"active", double(last_active_ms)}}, true);
}

static bool open_in_tabs()
{
    // never from a conda environment the user activated: the Kestrel taking over may run in a different one
    return settings().value("open_in_tabs", false).toBool() && !explicit_conda_env();
}

static MainWindow *recent_window()
{
    if (last_active && WINDOWS.contains(last_active.data()))
        return last_active;
    return WINDOWS.isEmpty() ? nullptr : WINDOWS.last();
}

static void use_token(const QString &token)
{
    // the launcher's activation token: without it GNOME (Wayland) won't let the window come to the front
    if (!token.isEmpty()) {
        qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
        qputenv("DESKTOP_STARTUP_ID", token.toUtf8());
    }
}

static void open_as_tabs(MainWindow *w, const QStringList &folders, const QStringList &select)
{
    // folders as new tabs (selecting select[i] in folders[i] when given); the first becomes current
    int first = w->tabs->count();
    for (int i = 0; i < folders.size(); ++i) {
        if (select.value(i).isEmpty()) {
            w->open_location(folders[i], true);
        } else if (Pane *p = w->new_tab(folders[i], false)) {
            p->select_later(select[i]);
        }
    }
    if (w->tabs->count() > first) {
        w->tabs->setCurrentIndex(first);
        w->pane()->view()->setFocus();
    }
    w->setWindowState(w->windowState() & ~Qt::WindowMinimized);
    w->raise();
    w->activateWindow();
}

static bool tabs_instead(const QStringList &folders, const QStringList &select)
{
    // true if the folders went to an open window as tabs: one of ours, or another Kestrel's (through the tower)
    if (!open_in_tabs())
        return false;
    if (MainWindow *w = recent_window()) {
        open_as_tabs(w, folders, select);
        return true;
    }
    return !atc::hand_off(folders, select).isEmpty();
}

void handle_fm1(const QString &method, const QStringList &uris, const QString &startup_id)
{
    // a request to the org.freedesktop.FileManager1 service (see fm1.h), e.g. a browser's "Show in folder"
    QStringList paths;
    for (const QString &u : uris) {
        QString p = uri_to_path(u);
        if (!p.isEmpty())
            paths << p;
    }
    if (paths.isEmpty())
        return;
    if (!startup_id.isEmpty()) {
        // the caller's activation token: without it GNOME (Wayland) won't let the new window take focus
        qputenv("XDG_ACTIVATION_TOKEN", startup_id.toUtf8());
        qputenv("DESKTOP_STARTUP_ID", startup_id.toUtf8());
    }
    if (method == "ShowItemProperties") {
        MainWindow *w = WINDOWS.isEmpty() ? open_window({dirname(paths.first())}) : WINDOWS.last();
        w->properties(paths);
        return;
    }
    MainWindow *w;
    if (method == "ShowFolders") {
        if (tabs_instead(paths, {}))
            return;
        w = open_window(paths);
    } else {
        // ShowItems: each item's folder in a tab, with the item selected, scrolled to and focused (folders too:
        // they're shown in their parent, not opened)
        QStringList folders, firsts;
        for (const QString &p : paths) {
            QString folder = dirname(rstrip(p, '/'));
            if (folder.isEmpty())
                folder = "/";
            if (!folders.contains(folder)) {
                folders << folder;
                firsts << p;
            }
        }
        if (tabs_instead(folders, firsts))
            return;
        w = open_window({folders.first()});
        for (int i = 0; i < folders.size(); ++i) {
            Pane *pane = i == 0 ? w->pane() : w->new_tab(folders[i], false);
            if (pane)
                pane->select_later(firsts[i]);
        }
    }
    w->raise();
    w->activateWindow();
    if (w->pane()) {
        w->tabs->setCurrentIndex(0);
        w->pane()->view()->setFocus();
    }
}

void on_atc(const QJsonObject &m)
{
    // a change reported by another Kestrel through the tower (see atc.h), or by this one ("own")
    QString type = m.value("type").toString();
    bool own = m.value("own").toBool();
    if (type == "sidebar") {   // its order or collapsed sections; also refreshes this process's other windows
        if (!own)
            settings().sync();
        for (MainWindow *w : WINDOWS)
            w->sidebar->refresh();
    }
    if (type == "bookmarks") {   // also refreshes this process's other windows
        for (MainWindow *w : WINDOWS) {
            w->sidebar->refresh();
            for (Pane *p : w->panes())
                if (p->is_overview())
                    p->refresh();
        }
    }
    if (own)
        return;   // the rest was already applied where it was changed
    if (type == "open" && m.value("flight").toString() == atc::radio()->flight()) {
        // folders another Kestrel handed over (open_in_tabs)
        QStringList folders, select;
        for (const QJsonValue &v : m.value("folders").toArray())
            folders << v.toString();
        for (const QJsonValue &v : m.value("select").toArray())
            select << v.toString();
        if (folders.isEmpty())
            return;
        use_token(m.value("token").toString());
        if (MainWindow *w = recent_window()) {
            open_as_tabs(w, folders, select);
        } else {   // our last window closed in the meantime
            MainWindow *nw = open_window(folders.mid(0, 1));
            open_as_tabs(nw, folders.mid(1), select.mid(1));
            if (nw->pane() && !select.value(0).isEmpty())
                nw->pane()->select_later(select[0]);
        }
    } else if (type == "settings") {
        settings().sync();
        apply_preferences();
    } else if (type == "starred") {
        places::reload_starred();
    } else if (type == "folders") {
        g_thumbs->reload_styles();
        for (const QJsonValue &v : m.value("paths").toArray())
            g_thumbs->invalidate(v.toString(), false);   // the sender already removed the cached mosaic
    } else if (type == "thumbs_cleared") {
        g_thumbs->clear_memory();
        for (MainWindow *w : WINDOWS)
            for (Pane *p : w->panes())
                p->animator->clear();
        repaint_all();
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
    QObject::connect(qApp, &QGuiApplication::focusWindowChanged, qApp, [](QWindow *win) {
        for (MainWindow *w : WINDOWS)
            if (win && w->windowHandle() == win) {
                last_active = w;
                last_active_ms = QDateTime::currentMSecsSinceEpoch();
                report_windows();
            }
    });
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
