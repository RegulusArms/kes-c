// A browser pane (pane.h): one tab's view of a folder, the overview page, the combined trash, Starred and Recent,
// search, history and selection. The window around it is in app.cpp.
#include "pane.h"

#include "animate.h"
#include "app.h"
#include "chooser.h"
#include "fileops.h"
#include "focus.h"
#include "overview.h"
#include "places.h"
#include "thumbs.h"
#include "stats.h"
#include "util.h"
#include "widgets.h"

#include <QCheckBox>
#include <QDrag>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <unistd.h>

using namespace util;

static QIcon icon(const QStringList &names) { return theme_icon(names); }

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
    search_edit->setAccessibleName("Search");
    search_edit->setClearButtonEnabled(true);
    search_sub = new QCheckBox("Include subfolders");
    search_sub->setChecked(win->view_value("search_recursive", false).toBool());
    auto *close = new QToolButton;
    close->setIcon(icon({"window-close-symbolic", "window-close"}));
    close->setToolTip("Close the search (Esc)");
    close->setAccessibleName("Close the search");
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
    v->setAccessibleName("Files");
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
    t->header()->setAccessibleName("Columns");
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
    listing_since = stats::now_ms();
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
        if (listing_since >= 0)
            stats::sample("folder listing (ms)", double(stats::now_ms() - listing_since));
        listing_since = -1;
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
