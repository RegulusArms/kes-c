// A browser pane (pane.h): one tab's view of a folder, the overview page, the combined trash, Starred and Recent,
// search, history and selection. The window around it is in app.cpp.
#include "pane.h"

#include "animate.h"
#include "app.h"
#include "chooser.h"
#include "drives.h"
#include "fileops.h"
#include "focus.h"
#include "groups.h"
#include "overview.h"
#include "places.h"
#include "thumbs.h"
#include "stats.h"
#include "util.h"
#include "archivewin.h"
#include "widgets.h"
#include "dupes.h"
#include "windows.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QStandardItemModel>
#include <QProgressBar>
#include <QStatusBar>
#include <QCheckBox>
#include <QDrag>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QGraphicsOpacityEffect>
#include <QDropEvent>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
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

// The file views. Their drags are Qt's, apart from giving the focus to the app the files are dropped into (focus.h),
// highlighting the folder a drag is over (where the files will go), and what a drag that starts here does in the view
// itself: it can only go into a folder. Over empty space or a file it's refused (Qt's grid would otherwise move the
// items there, out of the layout, until the folder is read again); dropped into a folder, the dragged items are shown
// shrinking into it, and a drag that ends anywhere else in nothing (refused, Esc) shows them going back where they are.
template <class View>
class FileView : public View {
protected:
    // The current item leaving the folder (moved into a subfolder, deleted, renamed away): the selection model makes a
    // neighbour current, and Qt's view would scroll to it, jumping to where the items were. It stays put instead.
    void rowsAboutToBeRemoved(const QModelIndex &parent, int start, int end) override
    {
        QModelIndex cur = this->currentIndex();
        if (cur.isValid() && cur.parent() == parent && cur.row() >= start && cur.row() <= end) {
            current_leaving = true;
            QTimer::singleShot(0, this, [this]() { current_leaving = false; });   // if no new current comes
        }
        View::rowsAboutToBeRemoved(parent, start, end);
    }

    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override
    {
        if (!current_leaving) {
            View::currentChanged(current, previous);
            return;
        }
        current_leaving = false;
        bool scroll = this->hasAutoScroll();   // autoScroll also scrolls at the edges during a drag: only off for this
        this->setAutoScroll(false);
        View::currentChanged(current, previous);
        this->setAutoScroll(scroll);
    }

    void mousePressEvent(QMouseEvent *ev) override
    {
        press_pos = ev->position().toPoint();   // where a drag starts (on Wayland, QCursor::pos() isn't reliable)
        View::mousePressEvent(ev);
    }

public:
    // The grid: an item is its icon and name (GridDelegate::shape), not its whole cell. A point elsewhere in its cell
    // is the folder's empty space, for whatever asks what's under the pointer (clicks, middle-click, the right-click
    // menu, hover, tooltips, drops). The list: the whole row, as Qt has it.
    QModelIndex indexAt(const QPoint &pos) const override
    {
        QModelIndex i = View::indexAt(pos);
        if constexpr (std::is_base_of_v<QListView, View>)
            if (i.isValid() && !item_shape(i).contains(QPointF(pos) + QPointF(0.5, 0.5)))
                return QModelIndex();
        return i;
    }

    QPainterPath item_shape(const QModelIndex &i) const
    {
        QRect cell = this->visualRect(i);
        auto *d = qobject_cast<GridDelegate *>(this->itemDelegate());
        if (!d) {
            QPainterPath whole;
            whole.addRect(cell);
            return whole;
        }
        QStyleOptionViewItem opt;
        this->initViewItemOption(&opt);
        opt.rect = cell;
        if (this->selectionModel() && this->selectionModel()->isSelected(i))
            opt.state |= QStyle::State_Selected;   // (its name bold: a little wider)
        return d->shape(opt, i);
    }

protected:
    // Selecting by position (QListView's setSelection takes the cells): a press selects the item under it as
    // indexAt has it, and the rubber band selects the items it touches the shape of. Shift and Ctrl ranges (from item
    // to item) stay Qt's.
    void setSelection(const QRect &rect, QItemSelectionModel::SelectionFlags command) override
    {
        if constexpr (std::is_base_of_v<QListView, View>) {
            // QRect(pos, pos); a band is QRect(press, pointer), its width and height negative when dragged up-left
            QRect n = rect.normalized();
            bool press = n.width() <= 1 && n.height() <= 1;
            if ((press || this->state() == QAbstractItemView::DragSelectingState) && this->selectionModel()) {
                QItemSelection sel;
                if (press) {
                    QModelIndex i = indexAt(rect.topLeft());
                    if (i.isValid() && (this->model()->flags(i) & Qt::ItemIsSelectable))
                        sel.select(i, i);
                } else {
                    QRectF band = QRectF(rect.normalized());
                    QAbstractItemModel *m = this->model();
                    for (int row = 0, n = m->rowCount(this->rootIndex()); row < n; ++row) {
                        QModelIndex i = m->index(row, 0, this->rootIndex());
                        QRect cell = this->visualRect(i);
                        if (!cell.intersects(rect.normalized()) || !(m->flags(i) & Qt::ItemIsSelectable))
                            continue;
                        if (item_shape(i).intersects(band))
                            sel.select(i, i);
                    }
                }
                this->selectionModel()->select(sel, command);
                return;
            }
        }
        View::setSelection(rect, command);
    }

    // Only the left button drags. Qt starts a drag on a move with any button held once an item was pressed: with the
    // mouse's back button still down as the hand moves (going back to the folder above), it dragged the folder just
    // left (selected there again) and, the drag going nowhere, showed its picture sliding back. Other buttons' moves
    // reach the view as plain moves (hover still works).
    void mouseMoveEvent(QMouseEvent *ev) override
    {
        if (ev->buttons() == Qt::NoButton || (ev->buttons() & Qt::LeftButton)) {
            View::mouseMoveEvent(ev);
            return;
        }
        QMouseEvent plain(ev->type(), ev->position(), ev->globalPosition(), Qt::NoButton, Qt::NoButton, ev->modifiers());
        View::mouseMoveEvent(&plain);
    }

    void dragMoveEvent(QDragMoveEvent *ev) override
    {
        View::dragMoveEvent(ev);
        QModelIndex i = this->indexAt(ev->position().toPoint());
        if (i.isValid())
            i = i.siblingAtColumn(0);
        // a folder that takes the drop, and isn't one of the items being dragged
        bool own = ev->source() == this;
        bool dragged = own && this->selectionModel()->isSelected(i);
        bool folder = i.isValid() && (this->model()->flags(i) & Qt::ItemIsDropEnabled);
        set_drop_target(ev->isAccepted() && folder && !dragged ? i : QModelIndex());
        if (own) {
            last_drag_pos = ev->position().toPoint();
            if (!drop_target.isValid())
                ev->ignore();   // nowhere to go: the items stay where they are
        }
    }

    void dragLeaveEvent(QDragLeaveEvent *ev) override
    {
        set_drop_target(QModelIndex());
        View::dragLeaveEvent(ev);
    }

    void dropEvent(QDropEvent *ev) override
    {
        QModelIndex target = drop_target;
        set_drop_target(QModelIndex());
        if (ev->source() == this) {
            last_drag_pos = ev->position().toPoint();
            if (!target.isValid()) {   // not into a folder: refused, so Qt doesn't move the items in the view
                ev->setDropAction(Qt::IgnoreAction);
                ev->ignore();
                return;
            }
            fly(QRect(last_drag_pos - drag_hot, drag_pm.deviceIndependentSize().toSize()), folder_rect(target), true);
        }
        View::dropEvent(ev);
    }

    void paintEvent(QPaintEvent *ev) override
    {
        View::paintEvent(ev);
        if (!drop_target.isValid())
            return;
        QRectF r = this->visualRect(drop_target);
        QPainterPath shape;
        if constexpr (std::is_base_of_v<QTreeView, View>)
            shape.addRoundedRect(QRectF(0, r.top(), this->viewport()->width(), r.height()).adjusted(1, 1, -1, -1), 4, 4);   // the whole row
        else
            shape = item_shape(drop_target).simplified();   // as the grid's selection
        QPainter p(this->viewport());
        p.setRenderHint(QPainter::Antialiasing);
        QColor c = this->palette().color(QPalette::Highlight);
        p.setPen(QPen(c, 2));
        c.setAlpha(70);
        p.setBrush(c);
        p.drawPath(shape);
    }

    void startDrag(Qt::DropActions supported) override
    {
        QModelIndexList indexes;
        for (const QModelIndex &i : this->selectedIndexes())
            if (this->model()->flags(i) & Qt::ItemIsDragEnabled)
                indexes << i;
        QMimeData *data = indexes.isEmpty() ? nullptr : this->model()->mimeData(indexes);
        if (!data)
            return;
        QPersistentModelIndex root = this->rootIndex();   // the folder shown (a drag that outlives it doesn't fly back)
        auto *drag = new QDrag(this);
        drag->setMimeData(data);
        QRect from;
        drag_pm = drag_pixmap(indexes, &from);
        drag_hot = from.isEmpty() ? QPoint() : press_pos - from.topLeft();
        drag_hot.setX(std::clamp(drag_hot.x(), 0, std::max(0, from.width() - 1)));   // on the picture, under the pointer
        drag_hot.setY(std::clamp(drag_hot.y(), 0, std::max(0, from.height() - 1)));
        std::unique_ptr<DragGhost> ghost;
        if (QGuiApplication::platformName() == "wayland")
            ghost = std::make_unique<DragGhost>(drag_pm, drag_hot);
        else
            drag->setPixmap(drag_pm);
        drag->setHotSpot(drag_hot);
        QPoint start_scroll(this->horizontalScrollBar()->value(), this->verticalScrollBar()->value());
        last_drag_pos = press_pos;
        Qt::DropAction def = this->defaultDropAction();
        if (def == Qt::IgnoreAction || !(supported & def))
            def = supported & Qt::CopyAction ? Qt::CopyAction : Qt::IgnoreAction;
        QPointer<FileView> self(this);
        Qt::DropAction done = drag->exec(supported, def);
        ghost.reset();
        if (!self)
            return;
        if (done == Qt::IgnoreAction && this->rootIndex() == root) {   // went nowhere: back to where the items are
            QPoint scrolled = QPoint(this->horizontalScrollBar()->value(), this->verticalScrollBar()->value()) - start_scroll;
            fly(QRect(last_drag_pos - drag_hot, drag_pm.deviceIndependentSize().toSize()), from.translated(-scrolled),
                false);
        } else if (!drag->target()) {   // dropped into another app
            focus::activate_at_pointer();
        }
    }

private:
    bool current_leaving = false;        // the current item is being removed (rowsAboutToBeRemoved)
    QPersistentModelIndex drop_target;   // the folder highlighted under a drag
    QPoint press_pos, last_drag_pos, drag_hot;   // viewport coordinates; drag_hot: on the drag's picture
    QPixmap drag_pm;

    void set_drop_target(const QModelIndex &i)
    {
        if (i == drop_target)
            return;
        drop_target = i;
        this->viewport()->update();
        this->setProperty("drop_target", i.isValid() ? i.data(PathRole).toString() : QString());   // for the tests
    }

    // where a folder's icon is (a list row: its start)
    QRect folder_rect(const QModelIndex &i) const
    {
        QRect r = this->visualRect(i);
        if constexpr (std::is_base_of_v<QTreeView, View>)
            return QRect(r.left(), r.top(), r.height(), r.height());
        return r;
    }

    // The drag's picture, moving in the view from `from` to `to` and gone: into a folder (shrinking and fading), or
    // back onto the items (they never moved, so it lands on them). Not when the picture is empty (items off screen).
    void fly(const QRect &from, const QRect &to, bool into)
    {
        if (drag_pm.isNull() || from.isEmpty() || to.isEmpty())
            return;
        auto *ghost = new QLabel(this->viewport());
        ghost->setAttribute(Qt::WA_TransparentForMouseEvents);
        ghost->setPixmap(drag_pm);
        ghost->setScaledContents(true);
        ghost->setGeometry(from);
        auto *fade = new QGraphicsOpacityEffect(ghost);
        ghost->setGraphicsEffect(fade);
        ghost->show();
        auto *group = new QParallelAnimationGroup(ghost);
        auto *move = new QPropertyAnimation(ghost, "geometry", group);
        move->setDuration(into ? 220 : 260);
        move->setStartValue(from);
        QSize small = from.size() * 0.2;
        move->setEndValue(into ? QRect(to.center() - QPoint(small.width() / 2, small.height() / 2), small)
                               : QRect(to.topLeft(), from.size()));
        move->setEasingCurve(into ? QEasingCurve::InCubic : QEasingCurve::OutCubic);
        auto *opacity = new QPropertyAnimation(fade, "opacity", group);
        opacity->setDuration(move->duration());
        opacity->setStartValue(into ? 0.9 : 0.75);
        opacity->setEndValue(into ? 0.15 : 1.0);
        QObject::connect(group, &QAbstractAnimation::finished, ghost, &QObject::deleteLater);
        group->start();
    }

    // the dragged items as they look in the view (what Qt draws); *at: where they are in the viewport
    QPixmap drag_pixmap(const QModelIndexList &indexes, QRect *at) const
    {
        QRect all;
        for (const QModelIndex &i : indexes)
            all |= this->visualRect(i).intersected(this->viewport()->rect());
        *at = all;
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
    auto *bar_lay = new QVBoxLayout(search_bar);
    bar_lay->setContentsMargins(6, 4, 6, 4);
    bar_lay->setSpacing(4);
    auto *sl = new QHBoxLayout;
    sl->setContentsMargins(0, 0, 0, 0);
    bar_lay->addLayout(sl);
    search_edit = new QLineEdit;
    search_edit->setPlaceholderText(tx("Search… (supports * and ? wildcards)"));
    search_edit->setAccessibleName(tx("Search"));
    search_edit->setClearButtonEnabled(true);
    search_sub = new QCheckBox(tx("Include subfolders"));
    search_sub->setChecked(win->view_value("search_recursive", false).toBool());
    auto *close = new QToolButton;
    close->setIcon(icon({"window-close-symbolic", "window-close"}));
    close->setToolTip(tx("Close the search (Esc)"));
    close->setAccessibleName(tx("Close the search"));
    close->setAutoRaise(true);
    connect(close, &QToolButton::clicked, this, [this]() { close_search(); });
    sl->addWidget(search_edit, 1);
    search_contents = new QCheckBox(tx("File contents"));
    search_contents->setToolTip(tx("Search inside files: with the desktop's search index where it covers the folder "
                                "(instant), else by reading them (ripgrep or grep)"));
    search_contents->setChecked(win->view_value("search_contents", false).toBool());
    connect(search_contents, &QCheckBox::toggled, this, [this](bool v) {
        this->win->set_view_value("search_contents", v);
        content_opts->setVisible(v);
        do_search();
    });
    // how File contents searches (shown while it's on)
    content_opts = new QWidget;
    auto *cl = new QHBoxLayout(content_opts);
    cl->setContentsMargins(0, 0, 0, 0);
    c_engine = new QComboBox;
    c_engine->setAccessibleName(tx("Search inside files with"));
    c_engine->addItems({tx("Automatic"), tx("Search index"), tx("Read the files")});
    c_engine->setToolTip(tx("Automatic: the desktop's search index where it covers this folder and the search is plain "
                         "words, else reading the files (any folder, regular expressions, match case)"));
    if (!can_search_contents())   // (localsearch isn't installed)
        qobject_cast<QStandardItemModel *>(c_engine->model())->item(ContentSearch::INDEX)->setEnabled(false);
    c_regex = new QCheckBox(tx("Regular expression"));
    c_case = new QCheckBox(tx("Match case"));
    cl->addWidget(c_engine);
    cl->addWidget(c_regex);
    cl->addWidget(c_case);
    for (QCheckBox *b : {c_regex, c_case})
        connect(b, &QCheckBox::toggled, this, &Pane::filters_changed);
    connect(c_engine, &QComboBox::currentIndexChanged, this, &Pane::filters_changed);
    filters_btn = new QToolButton;
    filters_btn->setText(tx("Filters"));
    filters_btn->setCheckable(true);
    filters_btn->setAutoRaise(true);
    filters_btn->setToolTip(tx("Find by type, size and date too (also with an empty name)"));
    filters_btn->setAccessibleName(tx("Filters"));
    sl->addWidget(search_sub);
    sl->addWidget(search_contents);
    sl->addWidget(content_opts);
    sl->addWidget(filters_btn);
    sl->addWidget(close);
    build_filter_row();
    bar_lay->addWidget(filter_row);
    search_progress = new QProgressBar;   // a search's progress: busy (how much is left isn't known) until it ends
    search_progress->setRange(0, 0);
    search_progress->setTextVisible(false);
    search_progress->setFixedHeight(4);
    search_progress->setAccessibleName(tx("Searching"));
    bar_lay->addWidget(search_progress);
    connect(filters_btn, &QToolButton::toggled, filter_row, &QWidget::setVisible);
    // only once they have a parent: showing a parentless widget opens it as a window of its own, which on Wayland
    // uses up the launch's activation token and leaves GNOME's busy cursor spinning until it times out
    content_opts->setVisible(search_contents->isChecked());
    filter_row->setVisible(false);
    search_progress->setVisible(false);
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
    connect(overview, &OverviewPage::find_duplicates, win, [win](const QString &t) { dupes::open_window(win, {t}); });
    connect(overview, &OverviewPage::open_file, win, [win](const QString &t) { win->open_file(t); });
    stack->addWidget(overview);
    lay->addWidget(stack);
    empty = new QLabel(tx("Folder is empty"), stack);
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setStyleSheet("color: palette(placeholder-text); font-size: 16px; background: transparent");
    empty->setAttribute(Qt::WA_TransparentForMouseEvents);
    empty->hide();

    model = make_model();
    search_model = new SearchModel(thumbs, this);
    grouper = new DateGroupProxy(this);
    // after the tree view has taken in the change (a reset clears its spanned rows)
    connect(grouper, &QAbstractItemModel::layoutChanged, this, &Pane::span_headers, Qt::QueuedConnection);
    connect(grouper, &QAbstractItemModel::modelReset, this, &Pane::span_headers, Qt::QueuedConnection);
    connect(places::signals_(), &places::Signals::starred_changed, this, &Pane::starred_changed);
    connect(places::signals_(), &places::Signals::sorts_changed, this, &Pane::apply_folder_sort);
    attach(proxy);
    set_view_mode(win->view_value("view_mode", "grid").toString());
    if (win->chooser) {   // a new tab in a chooser window
        set_type_filter(win->chooser->type_filter());
        overview->set_save_places(win->chooser->saving());
    }
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
    cancel_thumbs();
    if (search_thread) {
        search_thread->stop = true;
        search_thread->wait();
    }
}

const QObject *Pane::thumb_owner() const { return model; }

void Pane::cancel_thumbs()
{
    // only this pane's: other tabs and windows of this process keep theirs
    thumbs->cancel_for(model);
    thumbs->cancel_for(search_model);
}

// -- setup

void Pane::setup_common(QAbstractItemView *v)
{
    v->setAccessibleName(tx("Files"));
    v->setSelectionMode(QAbstractItemView::ExtendedSelection);
    v->setEditTriggers(QAbstractItemView::NoEditTriggers);
    v->setDragEnabled(true);
    v->setAcceptDrops(true);
    v->setDropIndicatorShown(false);   // FileView highlights the folder instead (Qt's also marks between rows)
    v->setDragDropMode(QAbstractItemView::DragDrop);
    v->setDefaultDropAction(Qt::MoveAction);
    v->viewport()->setAcceptDrops(true);   // the grid's Static movement turns drops off there (QListView::setMovement)
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
    // One pass, not in batches: a batched layout redone (after a zoom, a resize, a change in the folder) started each
    // batch on a new row, leaving an empty cell at the end of the row before it. A pass over 50,000 items takes about
    // 20 ms (45 ms grouped by date, where each item is asked its size).
    g->setLayoutMode(QListView::SinglePass);
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
    t->setItemDelegate(new ListDelegate(t));
    t->header()->setAccessibleName(tx("Columns"));
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
    // goes with the model; connected first, so it has taken in a change before the slots below (update_empty can add
    // rows) run
    proxy = new PinSortProxy(m);
    proxy->set_pinned_first(win->view_value("pinned_first", false).toBool());
    MainWindow *w = win;
    m->drop_handler = [w](const QStringList &paths, const QString &target, bool copy_only) {
        w->handle_drop(paths, target, copy_only);
    };
    m->members_handler = [w](const QMimeData *data, const QString &target) { ArchiveWindow::drop_members(w, data, target); };
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

void Pane::attach(QAbstractItemModel *src)
{
    attached = src;
    QAbstractItemModel *m = src;
    if (grouped) {
        grouper->set_source(nullptr, QModelIndex(), nullptr);   // set_root gives it the folder
        m = grouper;
    }
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

void Pane::use_model_root() { set_root(proxy->mapFromSource(model->index(path))); }

void Pane::set_root(const QModelIndex &root)
{
    if (!grouped) {
        grid->setRootIndex(root);
        tree->setRootIndex(root);
        return;
    }
    // an item's path and modification time (seconds)
    std::function<QPair<QString, qint64>(const QModelIndex &)> file_of;
    if (attached == search_model) {
        file_of = [](const QModelIndex &i) {
            return qMakePair(i.data(PathRole).toString(), i.data(StatRole).toList().value(1).toLongLong());
        };
    } else {
        FSModel *fm = model;
        PinSortProxy *px = proxy;
        file_of = [fm, px](const QModelIndex &i) {
            QModelIndex s = px->mapToSource(i);
            return qMakePair(fm->filePath(s), fm->lastModified(s).toSecsSinceEpoch());
        };
    }
    int by = group_by;
    auto date_of = [file_of, by](const QModelIndex &i) {
        auto [p, mtime] = file_of(i);
        if (p.isEmpty())
            return QDate();
        if (by == groups::CREATED)
            return groups::Dates::instance()->created(p, mtime);
        if (by == groups::TAKEN)
            return groups::Dates::instance()->taken(p, mtime);
        return QDateTime::fromSecsSinceEpoch(mtime).date();
    };
    grouper->set_order(tree->header()->sortIndicatorSection(), tree->header()->sortIndicatorOrder());
    grouper->set_mode(group_mode);
    grouper->set_watch_dates(by == groups::TAKEN);
    grouper->set_source(attached, root, date_of);
    grid->setRootIndex(QModelIndex());
    tree->setRootIndex(QModelIndex());
    span_headers();
}

void Pane::span_headers()
{
    if (!grouped)
        return;
    for (int r : grouper->header_rows())
        tree->setFirstColumnSpanned(r, QModelIndex(), true);
}

// Grouping (groups.h): the views show the grouper, which shows the folder (or the results); the grid then has no fixed
// cells, so that a header can take a line of its own (update_grid_size sizes the items instead).
void Pane::apply_grouping(int mode, int by)
{
    if (mode == group_mode && (by == group_by || mode == groups::NONE)) {
        group_by = by;
        return;
    }
    QStringList sel = selected_paths();
    bool on = mode != groups::NONE;
    // persistent: attaching sorts the model (QTreeView::setModel), which can move the folder's index
    QPersistentModelIndex root = grouped ? grouper->source_root() : grid->rootIndex();
    bool reattach = on != grouped;
    group_mode = mode;
    group_by = by;
    grouped = on;
    grid->setUniformItemSizes(!on);
    if (reattach)
        attach(attached);
    set_root(root);
    if (!on) {
        grouper->set_source(nullptr, QModelIndex(), nullptr);
        grouper->set_watch_dates(false);
    }
    update_grid_size();
    if (!sel.isEmpty())
        select_paths(sel);
}

void Pane::set_grouping(int mode, int by)
{
    apply_grouping(mode, by);
    if (path.isEmpty() || is_overview())
        return;
    places::set_folder_sort({path}, {tree->header()->sortIndicatorSection(), int(tree->header()->sortIndicatorOrder()),
                                     group_mode, group_by});
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

void Pane::set_sizes(int grid, int list)
{
    grid_size = std::clamp(grid, GRID_MIN, GRID_MAX);
    list_size = std::clamp(list, LIST_MIN, LIST_MAX);
    delegate->icon_size = grid_size;
    update_grid_size();
    tree->setIconSize(QSize(list_size, list_size));
    apply_folder_previews();
    view()->viewport()->update();
}

// ---------------------------------------------------------------- the search bar's filters

// for the session: every tab and window starts its searches with the filters and content options last chosen
static SearchFilter session_filter;
static ContentSearch session_content;

void Pane::build_filter_row()
{
    filter_row = new QWidget;
    auto *fl = new QHBoxLayout(filter_row);
    fl->setContentsMargins(0, 0, 0, 0);
    f_type = new QComboBox;
    f_type->setAccessibleName(tx("Type"));
    f_type->addItems({tx("Any type"), tx("Images"), tx("Videos"), tx("Audio"), tx("Documents"), tx("Archives"), tx("Folders")});
    f_size_mode = new QComboBox;
    f_size_mode->setAccessibleName(tx("Size"));
    f_size_mode->addItems({tx("Any size"), tx("Larger than"), tx("Smaller than")});
    f_size = new QDoubleSpinBox;
    f_size->setAccessibleName(tx("Size limit"));
    f_size->setRange(0, 1e6);
    f_size->setDecimals(1);
    f_size->setValue(10);
    f_unit = new QComboBox;
    f_unit->setAccessibleName(tx("Size unit"));
    f_unit->addItems({tx("KB"), tx("MB"), tx("GB")});
    f_unit->setCurrentIndex(1);
    f_when = new QComboBox;
    f_when->setAccessibleName(tx("Date"));
    f_when->addItems({tx("Any time"), tx("Today"), tx("This week"), tx("This month"), tx("This year"), tx("Between…")});
    f_date_by = new QComboBox;
    f_date_by->setAccessibleName(tx("Date of"));
    f_date_by->addItems({tx("modified"), tx("created")});
    f_from = new QDateEdit(QDate::currentDate().addMonths(-1));
    f_to = new QDateEdit(QDate::currentDate());
    f_from->setAccessibleName(tx("From"));
    f_to->setAccessibleName(tx("To"));
    for (QDateEdit *d : {f_from, f_to}) {
        d->setCalendarPopup(true);
        d->setDisplayFormat(QLocale().dateFormat(QLocale::ShortFormat));
    }
    auto *clear = new QToolButton;
    clear->setText(tx("Clear"));
    clear->setAutoRaise(true);
    clear->setToolTip(tx("Clear the filters"));
    connect(clear, &QToolButton::clicked, this, [this]() { set_search_filter(SearchFilter()); });
    fl->addWidget(f_type);
    fl->addSpacing(8);
    fl->addWidget(f_size_mode);
    fl->addWidget(f_size);
    fl->addWidget(f_unit);
    fl->addSpacing(8);
    fl->addWidget(f_when);
    fl->addWidget(f_from);
    fl->addWidget(f_to);
    fl->addWidget(f_date_by);
    fl->addWidget(clear);
    fl->addStretch(1);
    for (QComboBox *c : {f_type, f_size_mode, f_unit, f_when, f_date_by})
        connect(c, &QComboBox::currentIndexChanged, this, &Pane::filters_changed);
    connect(f_size, &QDoubleSpinBox::valueChanged, this, &Pane::filters_changed);
    for (QDateEdit *d : {f_from, f_to})
        connect(d, &QDateEdit::dateChanged, this, &Pane::filters_changed);
    set_search_filter(session_filter);   // what this session chose last
    ContentSearch c = session_content;
    c.on = search_contents->isChecked();   // (on or off: its own setting)
    set_content_search(c);
}

SearchFilter Pane::search_filter() const
{
    SearchFilter f;
    f.type = f_type->currentIndex();
    f.size_mode = f_size_mode->currentIndex();
    f.size = qint64(f_size->value() * double(qint64(1) << (10 * (f_unit->currentIndex() + 1))));
    f.when = f_when->currentIndex();
    f.from = f_from->date();
    f.to = f_to->date();
    f.by_created = f_date_by->currentIndex() == 1;
    return f;
}

void Pane::set_search_filter(const SearchFilter &f)
{
    loading_filters = true;
    f_type->setCurrentIndex(f.type);
    f_size_mode->setCurrentIndex(f.size_mode);
    if (f.size_mode != SearchFilter::ANY_SIZE) {   // in the largest unit it's a whole number of
        int unit = f.size >= (qint64(1) << 30) && f.size % (qint64(1) << 30) == 0 ? 2 : f.size >= (1 << 20) ? 1 : 0;
        f_unit->setCurrentIndex(unit);
        f_size->setValue(double(f.size) / double(qint64(1) << (10 * (unit + 1))));
    }
    f_when->setCurrentIndex(f.when);
    if (f.from.isValid())
        f_from->setDate(f.from);
    if (f.to.isValid())
        f_to->setDate(f.to);
    f_date_by->setCurrentIndex(f.by_created ? 1 : 0);
    loading_filters = false;
    filters_changed();
}

ContentSearch Pane::content_search() const
{
    ContentSearch c;
    c.on = search_contents->isChecked();
    c.engine = c_engine->currentIndex();
    c.regex = c_regex->isChecked();
    c.match_case = c_case->isChecked();
    return c;
}

void Pane::set_content_search(const ContentSearch &c)
{
    loading_filters = true;
    c_engine->setCurrentIndex(c.engine);
    c_regex->setChecked(c.regex);
    c_case->setChecked(c.match_case);
    loading_filters = false;
    if (search_contents->isChecked() != c.on)
        search_contents->setChecked(c.on);   // (searches again itself)
    else
        filters_changed();
}

void Pane::filters_changed()
{
    SearchFilter f = search_filter();
    f_size->setVisible(f.size_mode != SearchFilter::ANY_SIZE);
    f_unit->setVisible(f.size_mode != SearchFilter::ANY_SIZE);
    f_from->setVisible(f.when == SearchFilter::RANGE);
    f_to->setVisible(f.when == SearchFilter::RANGE);
    f_date_by->setVisible(f.when != SearchFilter::ANY_TIME);
    filters_btn->setText(f.active() ? tx("Filters •") : tx("Filters"));
    if (loading_filters)
        return;
    session_filter = f;
    ContentSearch c = content_search();
    session_content = c;
    if (search_bar->isVisible() || !search_edit->text().isEmpty())
        search_timer->start();
}

void Pane::search_for(const QString &text)
{
    if (!search_bar->isVisible())
        start_search();
    search_edit->setText(text);
    search_timer->stop();
    do_search();
}

QString Pane::view_message() const   // as seen: shown, and on top of the view (not behind it)
{
    QObjectList z = stack->children();   // bottom to top
    bool seen = !empty->isHidden() && z.indexOf(empty) > z.indexOf(stack->currentWidget());
    return seen ? empty->text() : QString();
}

bool Pane::search_busy_shown() const { return !search_progress->isHidden(); }

int Pane::match_line(const QString &p) const
{
    QStandardItem *item = in_search ? search_model->rows.value(p) : nullptr;
    return item ? item->data(LineRole).toInt() : 0;
}

void Pane::set_search_options(bool subfolders, bool contents)
{
    for (auto [box, on] : {std::pair{search_sub, subfolders}, std::pair{search_contents, contents}}) {
        QSignalBlocker quiet(box);   // (their handlers save them; they're saved already)
        if (box->isEnabled())
            box->setChecked(on);
    }
}

void Pane::update_grid_size()
{
    QSize cell = delegate->cell_size();
    // Width as if the scrollbar were always shown: otherwise the scrollbar appearing/disappearing changes the
    // column width and the grid re-flows.
    int sb = grid->verticalScrollBar()->isVisible() ? 0 : grid->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    // less 1: QListView wraps an item that would end past the last pixel column (width - 1), so cells adding up to
    // exactly the width would push the last column onto the next row and leave a gap at the end of every row
    int avail = grid->viewport()->width() - sb - 1;
    int cols = std::max(1, avail / cell.width());
    int extra = cols > 1 ? (avail - cols * cell.width()) / cols : 0;
    QSize size(cell.width() + std::max(0, extra), cell.height());
    if (grouped) {
        if (grid->gridSize().isValid())
            grid->setGridSize(QSize());
        if (size != delegate->cell || avail != delegate->header_width) {
            delegate->cell = size;
            delegate->header_width = avail;
            grid->doItemsLayout();   // the items' sizes are kept by the view
        }
        return;
    }
    delegate->cell = QSize();
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
        apply_folder_sort();
        Q_EMIT path_changed();
        show_trash();
        return true;
    }
    QString p = abspath(expanduser(target));
    // on a tape or floppy, these checks wait on the drive: it's taken as a folder, and the folder model (which reads it
    // on its own thread) shows what's there, or nothing
    bool slow = drives::slow_kind(p) != drives::Slow::None;
    if (!slow && isfile(p)) {
        select = p;
        p = dirname(p);
    }
    if (!slow && !isdir(p)) {
        warn(this, tx("Not found"), tx("“%1” does not exist.").arg(p));
        return false;
    }
    if (!slow && !util::access(p, R_OK | X_OK)) {
        warn(this, tx("Permission denied"), tx("You don't have permission to open “%1”.").arg(p));
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
    if (p != prev && !win->chooser && !is_trash())   // (a chooser's folders are the other app's choice)
        places::visit_folder(p);
    stack->setCurrentWidget(mode_view);
    // the folder left: its thumbnail jobs wait behind what's shown now (and stop after a while), not stopped at once
    thumbs->lower(model);
    thumbs->lower(search_model);
    animator->clear();
    model->setNameFilters(type_filters);
    QModelIndex src_root = model->setRootPath(p);
    proxy->set_root(p);   // before mapping: it may start or stop sorting
    set_root(proxy->mapFromSource(src_root));
    apply_folder_sort();
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
    proxy->pins_changed();
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
        locations.insert(p, o.isEmpty() ? tx("(unknown)") : dirname(o));
    }
    if (!in_search) {
        in_search = true;
        attach(search_model);
        set_root(QModelIndex());
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
    QModelIndex idx = source_index_for(pending_select);   // also in a collapsed group (select_paths expands it)
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
        if (grouped)
            grouper->reveal(source_index_for(p));
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
        // a folder still loading is laid out again as rows arrive, so the item may not have its final position yet:
        // scroll again once
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

QModelIndex Pane::source_index_for(const QString &p) const
{
    if (in_search) {
        QStandardItem *item = search_model->rows.value(p);
        return item ? item->index() : QModelIndex();
    }
    return proxy->mapFromSource(model->index(p));
}

QModelIndex Pane::index_for(const QString &p) const
{
    QModelIndex i = source_index_for(p);
    return grouped ? grouper->mapFromSource(i) : i;
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
        attach(proxy);
        QModelIndex src_root = model->setRootPath(path);
        proxy->set_root(path);
        set_root(proxy->mapFromSource(src_root));
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
        QString none = is_trash() ? tx("Trash is empty") : places::empty_text(path);
        if (!search_model->rowCount())
            text = searching_text ? tx("No matches") : none;
    } else if (in_search) {
        // in the view itself, never a window: searching, or nothing found (and why, when something went wrong)
        if (search_model->rowCount() == 0)
            text = searching() ? tx("Searching…")
                               : tx("Nothing found") + (search_error.isEmpty() ? QString() : "\n" + search_error);
    } else {
        bool named = !search_bar->isHidden() && !search_edit->text().trimmed().isEmpty();
        // filtered by the name in the search bar: what the view shows; else the folder's items
        int n = path.isEmpty() ? 0
                : named        ? view()->model()->rowCount(view()->rootIndex())
                               : model->rowCount(model->index(path));
        if (n == 0)
            text = named ? tx("Nothing found") : tx("Folder is empty");
    }
    empty->setText(text);
    empty->setVisible(!text.isEmpty());
    empty->setGeometry(stack->rect());
    empty->raise();   // over the view (the stack raises the page it shows)
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
        attach(proxy);
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
    search_progress->hide();
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
    SearchFilter filter = search_filter();
    bool filtering = filter.active() && !search_bar->isHidden();   // (the filters count while the bar is open)
    stop_search();
    if (!text.isEmpty() || filtering)
        record_search();
    else
        unrecord_search();
    if (is_listing()) {   // filter the combined trash / Starred / Recent list by name
        show_trash();
        return;
    }
    if (text.isEmpty() && !filtering) {
        model->setNameFilters(type_filters);
        if (in_search) {
            in_search = false;
            attach(proxy);
            use_model_root();
        }
        update_empty();
        return;
    }
    ContentSearch how = content_search();
    bool contents = how.on && !text.isEmpty();
    if (contents && drives::slow_kind(path) != drives::Slow::None) {   // it would read every file on the drive
        contents = false;
        win->statusBar()->showMessage(tx("File contents isn't searched on a tape or floppy: it would read every file"), 8000);
    }
    if (search_sub->isChecked() || contents || filtering) {
        if (!in_search) {
            in_search = true;
            attach(search_model);
            set_root(QModelIndex());
        }
        tree->setColumnHidden(4, !contents);   // Match: the matching line (reading the files)
        search_model->clear_results();
        auto *t = new SearchThread(path, text, win->show_hidden, nullptr, contents, search_sub->isChecked(),
                                   filtering ? filter : SearchFilter(), how);
        connect(t, &SearchThread::found, this, [this, t](const QStringList &paths, const QVariantMap &matches) {
            if (t != search_thread)
                return;   // a search that was replaced
            search_model->add_paths(paths, {}, matches);
            try_select();
            update_empty();
        });
        connect(t, &QThread::finished, this, [this, t]() {
            if (t == search_thread) {
                search_error = t->error;
                search_progress->hide();
                update_empty();
                win->update_status();
            }
        });
        connect(t, &QThread::finished, t, &QObject::deleteLater);
        search_thread = t;
        search_error.clear();
        search_progress->show();
        t->start();
    } else {
        if (in_search) {
            in_search = false;
            attach(proxy);
            use_model_root();
        }
        bool wild = text.contains('*') || text.contains('?') || text.contains('[');
        model->setNameFilters({wild ? text : "*" + text + "*"});
        QTimer::singleShot(50, this, &Pane::update_empty);   // once the model has applied it
    }
    update_empty();
    win->update_status();
}

// -- selection helpers

QList<Pane::Known> Pane::selected_known() const
{
    QList<Known> out;
    if (is_overview())
        return out;
    QItemSelectionModel *sm = view()->selectionModel();
    if (!sm)
        return out;
    QAbstractItemModel *m = view()->model();
    QSet<QPair<int, QModelIndex>> seen;
    for (const QModelIndex &i : sm->selectedIndexes()) {
        if (seen.contains({i.row(), i.parent()}))
            continue;
        seen.insert({i.row(), i.parent()});
        QModelIndex c = m->index(i.row(), 0, i.parent());
        Known k;
        k.path = c.data(PathRole).toString();
        if (k.path.isEmpty())
            continue;
        QVariant kind = c.data(KindRole), size = c.data(SizeRole), st = c.data(StatRole);
        if (kind.isValid())
            k.kind = kind.toInt();
        if (size.isValid())
            k.size = size.toLongLong();
        if (st.isValid()) {   // search results: (is_dir, mtime, size)
            QVariantList l = st.toList();
            if (l.size() >= 3) {
                k.kind = l[0].toBool() ? KIND_DIR : 0;
                k.size = l[2].toLongLong();
            }
        }
        out << k;
    }
    return out;
}

QStringList Pane::selected_paths() const
{
    if (is_overview())
        return {};
    QItemSelectionModel *sm = view()->selectionModel();
    if (!sm)
        return {};
    QList<QPair<int, QModelIndex>> rows;
    QSet<QPair<int, QModelIndex>> seen;   // (a set: each row comes once per column, and 800 rows made a list slow)
    for (const QModelIndex &i : sm->selectedIndexes()) {
        QPair<int, QModelIndex> key(i.row(), i.parent());
        if (!seen.contains(key)) {
            seen.insert(key);
            rows << key;
        }
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
    if (grouped) {   // in the groups' order, also the collapsed ones
        QModelIndex root = grouper->source_root();
        for (int r : grouper->source_rows())
            out << attached->index(r, 0, root).data(PathRole).toString();
        return out;
    }
    if (in_search) {
        for (int r = 0; r < search_model->rowCount(); ++r)
            out << search_model->item(r, 0)->data(PathRole).toString();
        return out;
    }
    QModelIndex root = proxy->mapFromSource(model->index(path));   // in the order shown
    for (int r = 0; r < proxy->rowCount(root); ++r)
        out << proxy->index(r, 0, root).data(PathRole).toString();
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

// Sorting (the header, the Sort menu) changes the folder's own sort, which it keeps: other folders keep theirs, or the
// default (Sort → Set as Default).
void Pane::sort_changed(int col, Qt::SortOrder order)
{
    if (applying_sort || path.isEmpty() || is_overview())
        return;
    places::set_folder_sort({path}, {col, int(order), group_mode, group_by});
}

places::FolderSort Pane::folder_sort(const QString &p) const
{
    if (auto own = places::folder_sort(p))
        return *own;
    return {win->view_value("sort_col", 0).toInt(), win->view_value("sort_order", 0).toInt(),
            win->view_value("sort_group", 0).toInt(), win->view_value("sort_group_by", 0).toInt()};
}

void Pane::apply_folder_sort()
{
    if (path.isEmpty() || is_overview())
        return;
    places::FolderSort sort = folder_sort(path);
    int col = sort.column, order = sort.order;
    apply_grouping(sort.group, sort.group_by);
    QHeaderView *h = tree->header();
    if (h->sortIndicatorSection() == col && h->sortIndicatorOrder() == Qt::SortOrder(order))
        return;
    applying_sort = true;
    h->setSortIndicator(col, Qt::SortOrder(order));   // the view sorts the model by it
    applying_sort = false;
}

void Pane::set_pinned_first(bool on)
{
    if (on != proxy->pinned_first())
        proxy->set_pinned_first(on);
}

bool Pane::pinned_first() const { return proxy->pinned_first(); }

void Pane::sort_by(int col, int order)
{
    Qt::SortOrder o = order < 0 ? tree->header()->sortIndicatorOrder() : Qt::SortOrder(order);
    tree->header()->setSortIndicator(col, o);
    tree->model()->sort(col, o);
}

// -- activation

void Pane::clicked(const QModelIndex &idx)
{
    if (grouper->is_header(idx)) {
        if (QGuiApplication::keyboardModifiers() == Qt::NoModifier) {
            if (grouper->is_footer(idx))   // collapsed from its end: the view goes to its header
                collapse_group(grouper->group_of(idx));
            else
                grouper->toggle(idx);
        }
        return;
    }
    if (settings().value("single_click", false).toBool() && QGuiApplication::keyboardModifiers() == Qt::NoModifier)
        win->open_paths(this, {idx.siblingAtColumn(0).data(PathRole).toString()});
}

int Pane::current_group() const
{
    return grouped ? grouper->group_of(view()->currentIndex()) : -1;
}

void Pane::collapse_group(int key)
{
    if (!grouped || key < 0)
        return;
    grouper->set_collapsed(key, true);
    // its header stays in view and becomes the current row (nothing selected), so the user keeps their place
    QModelIndex h = grouper->header_index(key);
    if (h.isValid()) {
        view()->selectionModel()->setCurrentIndex(h, QItemSelectionModel::NoUpdate);
        view()->scrollTo(h);
    }
}

void Pane::expand_group(int key)
{
    if (grouped && key >= 0)
        grouper->set_collapsed(key, false);
}

void Pane::set_all_groups_collapsed(bool on)
{
    if (!grouped)
        return;
    int cur = current_group();
    grouper->set_all_collapsed(on);
    QModelIndex h = on ? grouper->header_index(cur) : QModelIndex();
    if (h.isValid()) {
        view()->selectionModel()->setCurrentIndex(h, QItemSelectionModel::NoUpdate);
        view()->scrollTo(h);
    }
}

int Pane::group_of_path(const QString &path) const
{
    return grouped ? grouper->group_of(index_for(path)) : -1;
}

QList<int> Pane::group_keys() const { return grouped ? grouper->group_keys() : QList<int>(); }

void Pane::double_clicked(const QModelIndex &idx)
{
    if (grouper->is_header(idx))
        return;
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
            QAbstractItemModel *m = view()->model();
            for (int r = 0; r < m->rowCount(view()->rootIndex()) && !view()->currentIndex().isValid(); ++r) {
                QModelIndex first = m->index(r, 0, view()->rootIndex());
                if (!grouper->is_header(first))
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
        QModelIndex cur = view()->currentIndex();   // a group's header, with nothing selected: Enter or Space folds it
        if ((k == Qt::Key_Return || k == Qt::Key_Enter || k == Qt::Key_Space) && mods == Qt::NoModifier &&
            grouper->is_header(cur) && !view()->selectionModel()->hasSelection()) {
            if (grouper->is_footer(cur))
                collapse_group(grouper->group_of(cur));
            else
                grouper->toggle(cur);
            return true;
        }
        // grouped: Ctrl+Left collapses the current item's group (from anywhere in it), Ctrl+Right expands it
        if ((k == Qt::Key_Left || k == Qt::Key_Right) && mods == Qt::ControlModifier && grouped && current_group() >= 0) {
            if (k == Qt::Key_Left)
                collapse_group(current_group());
            else
                expand_group(current_group());
            return true;
        }
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
        return tx(OVERVIEW_TITLE);
    if (is_listing() && !(search_bar->isVisible() && !search_edit->text().trimmed().isEmpty()))
        return is_trash() ? tx("Trash") : places::title(path);
    if (in_search)
        return tx("Search: %1").arg(search_edit->text());
    QString b = basename(rstrip(path, '/'));
    return b.isEmpty() ? "/" : b;
}
