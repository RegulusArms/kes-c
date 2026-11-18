#include "archivebrowser.h"

#include "app.h"
#include "archive_ui.h"
#include "archivecache.h"
#include "dialogs.h"
#include "fileops.h"
#include "i18n.h"
#include "pane.h"
#include "prefs.h"
#include "thumbs.h"
#include "viewer.h"
#include "widgets.h"
#include "windows.h"

#include <QApplication>
#include <QClipboard>
#include <QCollator>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSlider>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <atomic>
#include <optional>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace util;

namespace {

// One item of a folder in the archive
struct Node {
    QString name, path, type;   // path: in the archive ("a/b.txt"; a folder's ends in "/")
    bool dir = false, encrypted = false;
    qint64 size = -1, packed = -1;
    QDateTime modified;
    std::optional<QCollatorSortKey> key;
};

constexpr int THUMB_SIZE = 256;     // thumbnails made at (the grid scales them to its zoom)
constexpr int THUMB_BATCH = 48;     // members extracted for thumbnails at a time

QString parent_of(const QString &member)   // "a/b/c.txt" → "a/b/", "a/b/" → "a/", "x" → ""
{
    QString m = member.endsWith('/') ? member.left(member.size() - 1) : member;
    return m.left(m.lastIndexOf('/') + 1);
}

}  // namespace

// The listing, by folder: each folder's items ("" the archive's top), made on the reading job's thread
struct ArchiveBrowser::Tree {
    QHash<QString, QVector<Node>> folders;
    QHash<QString, QPair<QString, int>> where;   // an item's folder and place in it
    int files = 0, nfolders = 0;
    qint64 total = 0;
    bool encrypted = false;

    const Node *node(const QString &member) const
    {
        auto it = where.find(member);
        if (it == where.end())
            return nullptr;
        auto f = folders.find(it->first);
        return f == folders.end() || it->second >= f->size() ? nullptr : &f->at(it->second);
    }

    void build(const QList<archive::Entry> &entries)
    {
        folders[QString()];
        std::function<void(const QString &)> ensure = [&](const QString &dir) {
            if (dir.isEmpty() || where.contains(dir))
                return;
            QString parent = parent_of(dir);
            ensure(parent);
            Node n;
            n.dir = true;
            n.path = dir;
            n.name = basename(dir.left(dir.size() - 1));
            n.type = fsmodel::type_of(n.name, true);
            folders[parent] << n;
            where.insert(dir, {parent, int(folders[parent].size()) - 1});
            folders[dir];
            ++nfolders;
        };
        for (const archive::Entry &e : entries) {
            if (e.path.isEmpty() || e.path == "/")
                continue;
            if (e.dir) {
                QString d = e.path.endsWith('/') ? e.path : e.path + '/';
                ensure(d);
                auto w = where.value(d);
                if (e.modified.isValid())
                    folders[w.first][w.second].modified = e.modified;
                continue;
            }
            if (where.contains(e.path))
                continue;   // (a tar can hold a name twice: its last one is what extracting gives)
            QString parent = parent_of(e.path);
            ensure(parent);
            Node n;
            n.path = e.path;
            n.name = basename(e.path);
            n.type = fsmodel::type_of(n.name, false);
            n.size = e.size;
            n.packed = e.packed;
            n.modified = e.modified;
            n.encrypted = e.encrypted;
            folders[parent] << n;
            where.insert(e.path, {parent, int(folders[parent].size()) - 1});
            ++files;
            total += std::max<qint64>(0, e.size);
            encrypted = encrypted || e.encrypted;
        }
        QCollator c;   // as the folders' own (fsmodel): numbers by value, case ignored
        c.setNumericMode(true);
        c.setCaseSensitivity(Qt::CaseInsensitive);
        for (auto it = folders.begin(); it != folders.end(); ++it)
            for (Node &n : *it)
                n.key = c.sortKey(n.name);
    }
};

// ---------------------------------------------------------------- the model: one folder of the archive

// Shaped as the folder model is for the views (FSModel): a row per item, its columns, and the roles GridDelegate and
// ListDelegate draw from (PathRole: its path in the archive; ThumbRole: its thumbnail once made)
class ArchiveModel : public QAbstractTableModel {
public:
    enum Column { NAME, SIZE, PACKED, TYPE, MODIFIED, COLUMNS };
    explicit ArchiveModel(ArchiveBrowser *b) : QAbstractTableModel(b), browser(b) {}

    void set_items(const QVector<Node> *list)
    {
        beginResetModel();
        items = list;
        rebuild();
        endResetModel();
    }
    void set_filter(const QString &f)
    {
        if (f == filter)
            return;
        beginResetModel();
        filter = f;
        rebuild();
        endResetModel();
    }
    const Node *node(const QModelIndex &i) const
    {
        return items && i.isValid() && i.row() < rows.size() ? &items->at(rows[i.row()]) : nullptr;
    }
    QModelIndex index_of(const QString &member) const
    {
        auto it = row_of.find(member);
        return it == row_of.end() ? QModelIndex() : index(*it, 0);
    }
    QStringList shown() const
    {
        QStringList out;
        for (int r : rows)
            out << items->at(r).path;
        return out;
    }
    void thumb_changed(const QString &member)
    {
        QModelIndex i = index_of(member);
        if (i.isValid())
            Q_EMIT dataChanged(i, i, {Qt::DecorationRole, ThumbRole});
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override { return parent.isValid() ? 0 : int(rows.size()); }
    int columnCount(const QModelIndex &parent = QModelIndex()) const override { return parent.isValid() ? 0 : COLUMNS; }
    QVariant headerData(int section, Qt::Orientation o, int role) const override
    {
        if (o != Qt::Horizontal || role != Qt::DisplayRole)
            return QVariant();
        static const char *names[] = {TX_NOOP("Name"), TX_NOOP("Size"), TX_NOOP("Packed"), TX_NOOP("Type"), TX_NOOP("Modified")};
        return section >= 0 && section < COLUMNS ? tx(names[section]) : QVariant();
    }
    Qt::ItemFlags flags(const QModelIndex &i) const override
    {
        return i.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled : Qt::NoItemFlags;
    }
    QVariant data(const QModelIndex &i, int role) const override
    {
        const Node *n = node(i);
        if (!n)
            return QVariant();
        switch (role) {
        case Qt::DisplayRole:
            switch (i.column()) {
            case NAME:
                return n->encrypted ? n->name + "  🔒" : n->name;
            case SIZE:
                return n->dir || n->size < 0 ? QString() : human_size(n->size);
            case PACKED:
                return n->dir || n->packed < 0 ? QString() : human_size(n->packed);
            case TYPE:
                return n->type;
            case MODIFIED:
                return n->modified.isValid() ? i18n::date_time(n->modified) : QString();
            }
            return QVariant();
        case Qt::DecorationRole:
            if (i.column() != NAME)
                return QVariant();
            if (QPixmap pm = browser->thumbnail(n->path); !pm.isNull())
                return QIcon(pm);
            return icon(*n);
        case ThumbRole: {
            QPixmap pm = browser->thumbnail(n->path);
            return pm.isNull() ? QVariant() : QVariant(pm);
        }
        case PathRole:
            return n->path;
        case KindRole:
            return n->dir ? int(KIND_DIR) : 0;
        case SizeRole:
            return n->size;
        case Qt::ToolTipRole:
            return n->encrypted ? tx("Encrypted: extracting it needs the archive's password") : QVariant();
        case Qt::TextAlignmentRole:
            return i.column() == SIZE || i.column() == PACKED ? QVariant(Qt::AlignRight | Qt::AlignVCenter) : QVariant();
        }
        return QVariant();
    }
    void sort(int column, Qt::SortOrder o) override
    {
        Q_EMIT layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
        QModelIndexList before = persistentIndexList();
        QVector<int> which;
        for (const QModelIndex &p : before)
            which << (p.row() < rows.size() ? rows[p.row()] : -1);
        sort_col = column;
        order = o;
        sort_rows();
        QHash<int, int> at;
        for (int r = 0; r < rows.size(); ++r)
            at.insert(rows[r], r);
        QModelIndexList after;
        for (int k = 0; k < before.size(); ++k)
            after << (at.contains(which[k]) ? index(at.value(which[k]), before[k].column()) : QModelIndex());
        changePersistentIndexList(before, after);
        Q_EMIT layoutChanged({}, QAbstractItemModel::VerticalSortHint);
    }
    QStringList mimeTypes() const override { return {"text/uri-list", archive::MEMBERS_MIME}; }
    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        QStringList members;
        for (const QModelIndex &i : indexes)
            if (i.column() == 0)
                if (const Node *n = node(i))
                    members << n->path;
        return members.isEmpty() ? nullptr : browser->drag_data(members);
    }
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }

    int sort_col = NAME;
    Qt::SortOrder order = Qt::AscendingOrder;

private:
    void rebuild()
    {
        rows.clear();
        row_of.clear();
        if (!items)
            return;
        for (int k = 0; k < items->size(); ++k)
            if (filter.isEmpty() || items->at(k).name.contains(filter, Qt::CaseInsensitive))
                rows << k;
        sort_rows();
    }
    void sort_rows()
    {
        if (!items)   // (the list sorts its model as soon as it's given it, before any folder)
            return;
        const QVector<Node> &l = *items;
        auto by_name = [&](const Node &a, const Node &b) { return a.key->compare(*b.key) < 0; };
        auto less = [&](int x, int y) {
            const Node &a = l[x], &b = l[y];
            if (a.dir != b.dir)
                return a.dir;   // folders first, either way
            bool desc = order == Qt::DescendingOrder;
            const Node &p = desc ? b : a, &q = desc ? a : b;
            switch (sort_col) {
            case SIZE:
                if (p.size != q.size)
                    return p.size < q.size;
                break;
            case PACKED:
                if (p.packed != q.packed)
                    return p.packed < q.packed;
                break;
            case TYPE:
                if (int c = QString::localeAwareCompare(p.type, q.type))
                    return c < 0;
                break;
            case MODIFIED:
                if (p.modified != q.modified)
                    return p.modified < q.modified;
                break;
            default:
                break;
            }
            return by_name(p, q);
        };
        std::stable_sort(rows.begin(), rows.end(), less);
        row_of.clear();
        for (int r = 0; r < rows.size(); ++r)
            row_of.insert(l[rows[r]].path, r);
    }
    QIcon icon(const Node &n) const
    {
        if (n.dir)
            return theme_icon(QStringList{"folder"});
        QString ext = n.name.contains('.') ? n.name.mid(n.name.lastIndexOf('.')).toLower() : n.name;
        auto it = icons.find(ext);
        if (it == icons.end()) {
            QMimeType m = mime_for(n.name, 0);
            it = icons.insert(ext, theme_icon({m.iconName(), m.genericIconName(), "text-x-generic"}));
        }
        return *it;
    }

    ArchiveBrowser *browser;
    const QVector<Node> *items = nullptr;
    QVector<int> rows;   // the items shown (filtered), in order
    QHash<QString, int> row_of;   // their rows, by path
    QString filter;
    mutable QHash<QString, QIcon> icons;   // by extension
};

namespace {

// What a drag out of the browser carries. For any app, the copies in the RAM cache, extracted when the drop asks for
// them (marked copy-only: never moved out of the cache). Items too big for the cache, or encrypted ones whose password
// isn't known, only as MEMBERS_MIME, which Kestrel takes (extracted straight into the folder, asking for the password).
class CacheMime : public QMimeData {
public:
    QPointer<ArchiveBrowser> browser;
    QString archive;
    QStringList members;
    bool offer_files = false;
    mutable bool fetched = false;
    mutable QList<QUrl> files;

    QStringList formats() const override
    {
        if (!offer_files)
            return {archive::MEMBERS_MIME};
        return {"text/uri-list", COPY_ONLY_MIME};
    }
    bool hasFormat(const QString &mime) const override { return formats().contains(mime); }

protected:
    QVariant retrieveData(const QString &mime, QMetaType type) const override
    {
        if (mime == archive::MEMBERS_MIME && !offer_files)
            return QJsonDocument(QJsonObject{{"archive", archive}, {"members", QJsonArray::fromStringList(members)}})
                .toJson(QJsonDocument::Compact);
        if (mime == COPY_ONLY_MIME && offer_files)
            return QByteArray("1");
        if (mime != "text/uri-list" || !offer_files)
            return QVariant();
        if (!fetched && browser) {
            fetched = true;
            QString why;
            QStringList got = browser->fetch_now(members, &why);
            browser->hold_for_drag(got);   // (pinned by fetch_now until the drag ends)
            for (const QString &p : got)
                files << QUrl::fromLocalFile(p);
            if (got.isEmpty() && !why.isEmpty()) {
                QPointer<ArchiveBrowser> b = browser;
                QTimer::singleShot(0, b, [b, why]() {   // (not in the middle of the drop)
                    if (b)
                        warn(b, tx("Drag"), why);
                });
            }
        }
        if (type.id() == QMetaType::QByteArray) {
            QByteArray out;
            for (const QUrl &u : files)
                out += u.toEncoded() + "\r\n";
            return out;
        }
        QVariantList l;
        for (const QUrl &u : files)
            l << u;
        return l;
    }
};

// the panes' views, with the drag's copies let go when it ends
class BrowserGrid : public QListView {
public:
    explicit BrowserGrid(ArchiveBrowser *b) : browser(b) {}

protected:
    void startDrag(Qt::DropActions) override
    {
        QListView::startDrag(Qt::CopyAction);
        browser->drag_ended();
    }

private:
    ArchiveBrowser *browser;
};

class BrowserList : public QTreeView {
public:
    explicit BrowserList(ArchiveBrowser *b) : browser(b) {}

protected:
    void startDrag(Qt::DropActions) override
    {
        QTreeView::startDrag(Qt::CopyAction);
        browser->drag_ended();
    }

private:
    ArchiveBrowser *browser;
};

// What a part folder made: each member's file (by its name; the folder's items have unique names) taken into the
// cache, pinned. Paths in members' order (empty for one that wasn't made).
QStringList take_in(const QString &archive, const QStringList &members, const QList<qint64> &sizes, const QStringList &made)
{
    ArchiveCache *cache = ArchiveCache::instance();
    QStringList out;
    QSet<int> used;
    for (int i = 0; i < members.size(); ++i) {
        QString name = basename(rstrip(members[i], '/'));
        int at = -1;
        for (int k = 0; k < made.size() && at < 0; ++k)
            if (!used.contains(k) && basename(made[k]) == name)
                at = k;
        if (at < 0) {
            out << QString();
            continue;
        }
        used.insert(at);
        try {
            QString p = cache->add(ArchiveCache::key(archive, members[i]), made[at], sizes.value(i));
            cache->pin(p);
            out << p;
        } catch (const OSError &) {
            out << QString();
        }
    }
    return out;
}

// tests: KESTREL_TEST_CACHE_HOLD=<file> holds an extraction into the cache once it's whole in its part folder, before
// it's taken in, while that file exists (making <file>.reached when it gets there), still answering a cancel
void test_hold(Task *task)
{
    QString hold = qEnvironmentVariable("KESTREL_TEST_CACHE_HOLD");
    if (hold.isEmpty())
        return;
    write_text(hold + ".reached", "");
    while (exists(hold)) {
        task->check();
        ::usleep(10000);
    }
}

}  // namespace

// ---------------------------------------------------------------- the window

ArchiveBrowser *ArchiveBrowser::open(MainWindow *owner, const QString &path_in)
{
    QString path = archive::first_volume(path_in);
    for (const QPointer<ArchiveView> &v : open_views())
        if (auto *b = qobject_cast<ArchiveBrowser *>(v.data()); b && b->path() == path && b->isVisible()) {
            b->raise();
            b->activateWindow();
            return b;
        }
    auto *b = new ArchiveBrowser(owner, path);
    open_views() << b;
    show_free(b);
    QPointer<ArchiveBrowser> self(b);
    auto t = std::make_shared<Tree>();
    b->read_listing([t](Task *, const QList<archive::Entry> &e) { t->build(e); },   // (on the job's thread)
                    [self, t]() {
                        if (!self)
                            return;
                        self->tree = t;
                        self->loaded_listing();
                    });
    return b;
}

ArchiveBrowser::ArchiveBrowser(MainWindow *owner_, const QString &path) : ArchiveView(owner_, path)
{
    setObjectName("archive_browser");
    setWindowTitle(tx("%1 — Archive").arg(basename(path)));
    grid_size = std::clamp(settings().value("archive/grid_size", 128).toInt(), GRID_MIN, GRID_MAX);
    list_size = std::clamp(settings().value("archive/list_size", 24).toInt(), LIST_MIN, LIST_MAX);
    auto *lay = new QVBoxLayout(this);

    // navigation, the path inside the archive, the view
    auto *nav = new QHBoxLayout;
    auto tool = [&](const QStringList &icons, const QString &tip) {
        auto *b = new QToolButton;
        b->setIcon(theme_icon(icons));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        nav->addWidget(b);
        return b;
    };
    back_btn = tool({"go-previous-symbolic", "go-previous"}, tx("Back (Alt+Left)"));
    fwd_btn = tool({"go-next-symbolic", "go-next"}, tx("Forward (Alt+Right)"));
    up_btn = tool({"go-up-symbolic", "go-up"}, tx("Up (Alt+Up)"));
    crumbs = new QWidget;
    auto *crumb_lay = new QHBoxLayout(crumbs);
    crumb_lay->setContentsMargins(0, 0, 0, 0);
    crumb_lay->setSpacing(0);
    nav->addWidget(crumbs, 1);
    grid_btn = tool({"view-grid-symbolic", "view-grid", "view-list-icons"}, tx("Icons"));
    list_btn = tool({"view-list-symbolic", "view-list", "view-list-details"}, tx("List"));
    grid_btn->setCheckable(true);
    list_btn->setCheckable(true);
    sort_btn = tool({"view-sort-ascending-symbolic", "view-sort-ascending"}, tx("Sort"));
    sort_btn->setPopupMode(QToolButton::InstantPopup);
    lay->addLayout(nav);

    auto *bar = new QHBoxLayout;
    extract_here = new QPushButton(tx("Extract Here"));
    extract_here->setToolTip(tx("Extract the selected items next to the archive"));
    extract_to = new QPushButton(tx("Extract To…"));
    extract_to->setToolTip(tx("Extract the selected items into a folder you choose"));
    extract_all = new QPushButton(tx("Extract All…"));
    test = new QPushButton(tx("Test"));
    test->setToolTip(tx("Check every file in the archive against its checksum, without extracting"));
    filter = new QLineEdit;
    filter->setPlaceholderText(tx("Filter this folder by name"));
    filter->setClearButtonEnabled(true);
    for (QPushButton *b : {extract_here, extract_to, extract_all, test}) {
        bar->addWidget(b);
        b->setEnabled(false);
    }
    bar->addStretch(1);
    bar->addWidget(filter);
    lay->addLayout(bar);

    // the views: the panes' own
    model = new ArchiveModel(this);
    auto *g = new BrowserGrid(this);
    grid = g;
    style_grid_view(grid);
    delegate = new GridDelegate(this, ThumbnailManager::instance(), nullptr, grid);
    delegate->icon_size = grid_size;
    grid->setItemDelegate(delegate);
    list = new BrowserList(this);
    style_list_view(list);
    list->setIconSize(QSize(list_size, list_size));
    grid->setModel(model);
    list->setModel(model);
    QItemSelectionModel *own = list->selectionModel();
    list->setSelectionModel(grid->selectionModel());   // one selection, whichever view shows
    delete own;
    list->header()->setSortIndicator(ArchiveModel::NAME, Qt::AscendingOrder);
    list->header()->setSectionResizeMode(ArchiveModel::NAME, QHeaderView::Stretch);
    list->header()->setStretchLastSection(false);
    for (QAbstractItemView *v : {static_cast<QAbstractItemView *>(grid), static_cast<QAbstractItemView *>(list)}) {
        v->setSelectionMode(QAbstractItemView::ExtendedSelection);
        v->setEditTriggers(QAbstractItemView::NoEditTriggers);
        v->setDragEnabled(true);
        v->setDragDropMode(QAbstractItemView::DragOnly);
        v->setDefaultDropAction(Qt::CopyAction);
        v->setContextMenuPolicy(Qt::CustomContextMenu);
        v->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        v->installEventFilter(this);
        v->viewport()->installEventFilter(this);
        connect(v, &QWidget::customContextMenuRequested, this, [this, v](const QPoint &pos) { context_menu(v, pos); });
        connect(v, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex &i) {
            activate(i.data(PathRole).toString());
        });
    }
    stack = new QStackedWidget;
    stack->addWidget(grid);
    stack->addWidget(list);
    lay->addWidget(stack, 1);

    auto *foot = new QHBoxLayout;
    status = new QLabel(tx("Reading the archive…"));
    status->setWordWrap(true);
    foot->addWidget(status, 1);
    zoom_slider = new QSlider(Qt::Horizontal);
    zoom_slider->setFixedWidth(120);
    zoom_slider->setToolTip(tx("Zoom (Ctrl+wheel, Ctrl++ / Ctrl+−)"));
    foot->addWidget(zoom_slider);
    lay->addLayout(foot);

    // the sort menu (the list's column headers sort too)
    auto *sort_menu = new QMenu(sort_btn);
    static const char *cols[] = {TX_NOOP("Name"), TX_NOOP("Size"), TX_NOOP("Packed"), TX_NOOP("Type"), TX_NOOP("Modified")};
    for (int c = 0; c < ArchiveModel::COLUMNS; ++c)
        sort_menu->addAction(tx(cols[c]), this, [this, c]() { sort_by(c, model->order); });
    sort_menu->addSeparator();
    sort_menu->addAction(tx("Ascending"), this, [this]() { sort_by(model->sort_col, Qt::AscendingOrder); });
    sort_menu->addAction(tx("Descending"), this, [this]() { sort_by(model->sort_col, Qt::DescendingOrder); });
    sort_btn->setMenu(sort_menu);

    connect(back_btn, &QToolButton::clicked, this, &ArchiveBrowser::go_back);
    connect(fwd_btn, &QToolButton::clicked, this, &ArchiveBrowser::go_forward);
    connect(up_btn, &QToolButton::clicked, this, &ArchiveBrowser::go_up);
    connect(grid_btn, &QToolButton::clicked, this, [this]() { set_view_mode("grid"); });
    connect(list_btn, &QToolButton::clicked, this, [this]() { set_view_mode("list"); });
    connect(zoom_slider, &QSlider::valueChanged, this, [this](int v) {
        if (v != zoom_value())
            zoom(0, v);
    });
    connect(filter, &QLineEdit::textChanged, this, [this](const QString &t) {
        model->set_filter(t.trimmed());
        update_buttons();
        queue_thumbs();
    });
    connect(grid->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ArchiveBrowser::update_buttons);
    connect(extract_here, &QPushButton::clicked, this, [this]() { extract_selected(dirname(archive_path)); });
    connect(extract_to, &QPushButton::clicked, this, &ArchiveBrowser::choose_and_extract);
    connect(extract_all, &QPushButton::clicked, this, [this]() {
        if (owner)
            archive_ui::extract_dialog(owner, archive_path);
    });
    connect(test, &QPushButton::clicked, this, [this]() {
        if (owner)
            archive_ui::test_archives(owner, {archive_path});
    });
    connect(grid->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this]() { update_grid_size(); });

    auto key = [this](const QKeySequence &k, std::function<void()> fn) {
        auto *s = new QShortcut(k, this);
        s->setContext(Qt::WidgetWithChildrenShortcut);
        connect(s, &QShortcut::activated, this, fn);
    };
    key(QKeySequence::Back, [this]() { go_back(); });
    key(QKeySequence(Qt::Key_Backspace), [this]() { go_back(); });
    key(QKeySequence::Forward, [this]() { go_forward(); });
    key(QKeySequence(Qt::ALT | Qt::Key_Up), [this]() { go_up(); });
    key(QKeySequence::Copy, [this]() { copy_selected(); });
    key(QKeySequence::ZoomIn, [this]() { zoom(1); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Equal), [this]() { zoom(1); });
    key(QKeySequence::ZoomOut, [this]() { zoom(-1); });
    key(QKeySequence::Find, [this]() { filter->setFocus(); });
    key(QKeySequence(Qt::CTRL | Qt::Key_1), [this]() { set_view_mode("grid"); });
    key(QKeySequence(Qt::CTRL | Qt::Key_2), [this]() { set_view_mode("list"); });

    set_view_mode(settings().value("archive/view_mode", "grid").toString());
    update_crumbs();
    update_buttons();
    resize(820, 580);
}

ArchiveBrowser::~ArchiveBrowser() { let_go(); }

void ArchiveBrowser::closeEvent(QCloseEvent *ev)
{
    ArchiveView::closeEvent(ev);
    let_go();   // (now: the window is deleted later)
}

void ArchiveBrowser::let_go()
{
    closed = true;
    stop_thumbs();
    ArchiveCache *cache = ArchiveCache::instance();
    for (const QString &p : drag_pins)
        cache->unpin(p);
    drag_pins.clear();
    cache->forget(archive_path);   // its copies go (those still in use once they're free)
}

void ArchiveBrowser::loaded_listing()
{
    is_loaded = true;
    extract_all->setEnabled(true);
    test->setEnabled(archive::can_test(archive_path));
    // an archive holding a single folder opens in it (most do: name.zip → name/…)
    const QVector<Node> &top = tree->folders.value(QString());
    go_to(top.size() == 1 && top.first().dir ? top.first().path : QString(), false);
}

QStringList ArchiveBrowser::selected_members() const
{
    QStringList out;
    QModelIndexList sel = grid->selectionModel()->selectedRows(0);
    std::sort(sel.begin(), sel.end(), [](const QModelIndex &a, const QModelIndex &b) { return a.row() < b.row(); });
    for (const QModelIndex &i : sel)
        out << i.data(PathRole).toString();
    return out;
}

void ArchiveBrowser::select_members(const QStringList &members)
{
    QItemSelectionModel *s = grid->selectionModel();
    s->clearSelection();
    QModelIndex first;
    for (const QString &m : members) {
        QModelIndex i = model->index_of(m);
        if (!i.isValid())
            continue;
        s->select(i, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        if (!first.isValid())
            first = i;
    }
    if (first.isValid()) {
        s->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
        grid->scrollTo(first);
        list->scrollTo(first);
    }
}

QStringList ArchiveBrowser::shown_members() const { return model->shown(); }

void ArchiveBrowser::go_to(const QString &folder, bool record, const QString &select)
{
    if (!tree || !tree->folders.contains(folder))
        return;
    if (record && folder != cur) {
        back_stack << cur;
        fwd_stack.clear();
    }
    cur = folder;
    show_folder(select);
}

void ArchiveBrowser::show_folder(const QString &select)
{
    stop_thumbs();
    filter->blockSignals(true);
    filter->clear();
    filter->blockSignals(false);
    model->set_filter(QString());
    model->set_items(&tree->folders[cur]);
    model->sort(model->sort_col, model->order);
    if (!select.isEmpty())
        select_members({select});
    else if (model->rowCount() > 0)
        grid->selectionModel()->setCurrentIndex(model->index(0, 0), QItemSelectionModel::NoUpdate);
    grid->scrollToTop();
    list->scrollToTop();
    update_crumbs();
    update_buttons();
    update_grid_size();
    queue_thumbs();
}

void ArchiveBrowser::go_back()
{
    if (back_stack.isEmpty())
        return;
    QString from = cur;
    fwd_stack << cur;
    cur = back_stack.takeLast();
    show_folder(from.startsWith(cur) ? from.left(from.indexOf('/', cur.size()) + 1) : QString());
}

void ArchiveBrowser::go_forward()
{
    if (fwd_stack.isEmpty())
        return;
    back_stack << cur;
    cur = fwd_stack.takeLast();
    show_folder(QString());
}

void ArchiveBrowser::go_up()
{
    if (cur.isEmpty())
        return;
    QString from = cur;
    go_to(parent_of(cur), true, from);   // the folder it came from selected
}

void ArchiveBrowser::activate(const QString &member)
{
    if (member.isEmpty())
        return;
    if (member.endsWith('/'))
        go_to(member);
    else
        open_member(member);
}

void ArchiveBrowser::update_crumbs()
{
    auto *lay = static_cast<QHBoxLayout *>(crumbs->layout());
    while (QLayoutItem *it = lay->takeAt(0)) {
        if (QWidget *w = it->widget())
            w->deleteLater();
        delete it;
    }
    auto add = [&](const QString &label, const QString &target, const QIcon &icon) {
        auto *b = new QToolButton;
        b->setText(label);
        b->setAutoRaise(true);
        if (!icon.isNull()) {
            b->setIcon(icon);
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }
        b->setCheckable(true);
        b->setChecked(target == cur);
        connect(b, &QToolButton::clicked, this, [this, target]() { go_to(target); });
        lay->addWidget(b);
    };
    add(basename(archive_path), QString(), theme_icon({"package-x-generic", "application-x-archive"}));
    QString at;
    for (const QString &seg : cur.split('/', Qt::SkipEmptyParts)) {
        auto *sep = new QLabel("›");
        sep->setEnabled(false);
        lay->addWidget(sep);
        at += seg + '/';
        add(seg, at, QIcon());
    }
    lay->addStretch(1);
}

void ArchiveBrowser::update_buttons()
{
    back_btn->setEnabled(!back_stack.isEmpty());
    fwd_btn->setEnabled(!fwd_stack.isEmpty());
    up_btn->setEnabled(!cur.isEmpty());
    QStringList sel = is_loaded ? selected_members() : QStringList();
    extract_here->setEnabled(!sel.isEmpty());
    extract_to->setEnabled(!sel.isEmpty());
    if (!is_loaded || !tree)
        return;
    QString summary = tx("%1 in %2, %3%4.")
                          .arg(txn("%Ln file(s)", tree->files), txn("%Ln folder(s)", tree->nfolders), human_size(tree->total),
                               tree->encrypted || !password.isEmpty() ? tx(" · encrypted") : QString());
    if (!sel.isEmpty())
        status->setText(tx("%1 selected (%2). %3").arg(txn("%Ln item(s)", sel.size()), human_size(size_of(sel)), summary));
    else
        status->setText(tx("This folder: %1. Whole archive: %2").arg(txn("%Ln item(s)", model->rowCount()), summary));
}

// -- views

bool ArchiveBrowser::is_grid() const { return stack->currentWidget() == grid; }

void ArchiveBrowser::set_view_mode(const QString &mode)
{
    bool g = mode != "list";
    stack->setCurrentWidget(g ? static_cast<QWidget *>(grid) : list);
    grid_btn->setChecked(g);
    list_btn->setChecked(!g);
    settings().setValue("archive/view_mode", g ? "grid" : "list");
    zoom_slider->blockSignals(true);
    zoom_slider->setRange(g ? GRID_MIN : LIST_MIN, g ? GRID_MAX : LIST_MAX);
    zoom_slider->setValue(zoom_value());
    zoom_slider->blockSignals(false);
    update_grid_size();
    queue_thumbs();
}

int ArchiveBrowser::zoom_value() const { return is_grid() ? grid_size : list_size; }

void ArchiveBrowser::zoom(int step, int absolute)
{
    if (is_grid()) {   // as the panes zoom (Pane::zoom)
        int nw = absolute >= 0 ? absolute : int(grid_size * (step > 0 ? 1.15 : 1 / 1.15));
        grid_size = std::clamp(nw, GRID_MIN, GRID_MAX);
        delegate->icon_size = grid_size;
        settings().setValue("archive/grid_size", grid_size);
        update_grid_size();
    } else {
        int nw = absolute >= 0 ? absolute : list_size + (step > 0 ? 8 : -8);
        list_size = std::clamp(nw, LIST_MIN, LIST_MAX);
        list->setIconSize(QSize(list_size, list_size));
        settings().setValue("archive/list_size", list_size);
    }
    zoom_slider->blockSignals(true);
    zoom_slider->setValue(zoom_value());
    zoom_slider->blockSignals(false);
    stack->currentWidget()->update();
    static_cast<QAbstractItemView *>(stack->currentWidget())->viewport()->update();
    queue_thumbs();
}

void ArchiveBrowser::update_grid_size()
{
    QSize size = grid_cell_size(grid, delegate);
    if (size != grid->gridSize())
        grid->setGridSize(size);
}

void ArchiveBrowser::resizeEvent(QResizeEvent *ev)
{
    ArchiveView::resizeEvent(ev);
    update_grid_size();
}

void ArchiveBrowser::sort_by(int column, Qt::SortOrder order)
{
    list->header()->setSortIndicator(column, order);   // (the list sorts the model: the grid shows the same)
    if (model->sort_col != column || model->order != order)
        model->sort(column, order);
}

bool ArchiveBrowser::eventFilter(QObject *obj, QEvent *ev)
{
    QEvent::Type t = ev->type();
    if ((obj == grid || obj == list) && t == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(ev);
        if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && k->modifiers() == Qt::NoModifier) {
            QStringList sel = selected_members();
            if (sel.size() == 1)
                activate(sel.first());
            return true;
        }
        if (k->key() == Qt::Key_Space && k->modifiers() == Qt::NoModifier) {
            quick_look();
            return true;
        }
    }
    if (obj == grid->viewport() || obj == list->viewport()) {
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
    }
    return ArchiveView::eventFilter(obj, ev);
}

void ArchiveBrowser::context_menu(QAbstractItemView *v, const QPoint &pos)
{
    QStringList sel = selected_members();
    if (!is_loaded || sel.isEmpty())
        return;
    auto *m = new QMenu(this);
    if (sel.size() == 1) {
        QString member = sel.first();
        m->addAction(tx("Open"), this, [this, member]() { activate(member); });
    }
    bool pictures = std::any_of(sel.begin(), sel.end(), [](const QString &s) { return !s.endsWith('/') && is_image(s); });
    if (pictures)
        m->addAction(tx("View\tSpace"), this, &ArchiveBrowser::quick_look);
    m->addAction(tx("Copy\tCtrl+C"), this, &ArchiveBrowser::copy_selected);
    m->addSeparator();
    m->addAction(tx("Extract Here"), this, [this]() { extract_selected(dirname(archive_path)); });
    m->addAction(tx("Extract To…"), this, &ArchiveBrowser::choose_and_extract);
    m->popup(v->viewport()->mapToGlobal(pos));
    connect(m, &QMenu::aboutToHide, m, &QObject::deleteLater);
}

// -- the RAM cache

void ArchiveBrowser::fetch(const QStringList &members, const QString &title, std::function<void(const QStringList &)> then)
{
    if (!owner || members.isEmpty())
        return;
    ArchiveCache *cache = ArchiveCache::instance();
    QStringList out, need, pinned;
    QList<qint64> sizes;
    qint64 bytes = 0;
    for (const QString &m : members) {
        QString p = cache->find(ArchiveCache::key(archive_path, m));
        out << p;
        if (!p.isEmpty()) {
            cache->pin(p);   // (not let go while the rest is extracted)
            pinned << p;
        } else {
            need << m;
            sizes << size_of({m});
            bytes += sizes.last();
        }
    }
    if (need.isEmpty()) {
        then(out);
        return;
    }
    auto let_go = [cache, pinned]() {
        for (const QString &p : pinned)
            cache->unpin(p, false);
    };
    ArchiveCache::Room room = cache->reserve(bytes);
    if (room != ArchiveCache::OK) {
        let_go();
        warn(this, title, ArchiveCache::room_message(room));
        return;
    }
    QString part;
    try {
        part = cache->new_part();
    } catch (const OSError &e) {
        cache->release(bytes);
        let_go();
        warn(this, title, e.message());
        return;
    }
    auto pw = std::make_shared<QString>(password);
    bool ask_first = pw->isEmpty() && needs_password(need);
    QPointer<QWidget> asker(this);
    QString path = archive_path;
    auto work = [path, part, need, pw, ask_first, asker](Task *task) -> QVariant {
        try {
            QStringList tried;
            if (ask_first) {
                *pw = archive_ui::job_password(task, asker, path, tried);
                if (pw->isEmpty()) {
                    util::rmtree(part);
                    return QVariantMap{{"no_password", true}};
                }
            }
            for (;;) {
                try {
                    QStringList made = archive::extract_members(task, path, part, *pw, need);
                    test_hold(task);
                    task->check();
                    return QVariantMap{{"made", made}};
                } catch (const archive::WrongPassword &) {
                    if (!pw->isEmpty())
                        tried << *pw;
                    *pw = archive_ui::job_password(task, asker, path, tried);
                    if (pw->isEmpty()) {
                        util::rmtree(part);
                        return QVariantMap{{"no_password", true}};
                    }
                }
            }
        } catch (...) {   // a cancel or a failure: nothing of it is kept
            util::rmtree(part);
            throw;
        }
    };
    // (once, from whichever comes: the result, or an error)
    auto ended = std::make_shared<bool>(false);
    auto finish = [cache, bytes, let_go, part, ended]() {
        if (*ended)
            return false;
        *ended = true;
        cache->release(bytes);
        let_go();
        util::rmtree(part);
        return true;
    };
    QPointer<ArchiveBrowser> self(this);
    QPointer<MainWindow> w(owner);
    auto done = [self, w, finish, path, need, sizes, out, members, pw, then, part, cache](const QVariant &r) mutable {
        QVariantMap m = r.toMap();
        QStringList got;
        if (r.isValid() && !m["no_password"].toBool())
            got = take_in(path, need, sizes, m["made"].toStringList());   // (before the part folder goes)
        if (!finish())
            return;
        if (!r.isValid() || m["no_password"].toBool()) {
            if (w)
                w->statusBar()->showMessage(tx("Extracting from %1 cancelled").arg(basename(path)), 4000);
            return;
        }
        for (int i = 0, k = 0; i < out.size(); ++i)
            if (out[i].isEmpty())
                out[i] = got.value(k++);
        for (const QString &p : out)
            if (!p.isEmpty())
                cache->pin(p);   // the caller's pin (finish let go of the ones that were there)
        for (const QString &p : got)
            if (!p.isEmpty())
                cache->unpin(p, false);   // (take_in's)
        if (!self) {   // the window has closed meanwhile
            for (const QString &p : out)
                if (!p.isEmpty())
                    cache->unpin(p, false);
            return;
        }
        if (!pw->isEmpty())
            self->password = *pw;   // not asked again while the window is open
        if (out.contains(QString())) {
            for (const QString &p : out)
                if (!p.isEmpty())
                    cache->unpin(p, false);
            warn(self, tx("Extract"), tx("Some of the items couldn't be extracted from the archive."));
            return;
        }
        then(out);
    };
    Task *t = fileops::queue_job(owner, title, {archive_path}, work, done, true, QStringList(), false, QStringList(), true);
    if (t)
        connect(t, &Task::error, cache, [finish]() mutable { finish(); });
    else
        finish();
}

QStringList ArchiveBrowser::fetch_now(const QStringList &members, QString *why)
{
    ArchiveCache *cache = ArchiveCache::instance();
    QStringList out, need, pinned;
    QList<qint64> sizes;
    qint64 bytes = 0;
    for (const QString &m : members) {
        QString p = cache->find(ArchiveCache::key(archive_path, m));
        out << p;
        if (!p.isEmpty()) {
            cache->pin(p);
            pinned << p;
        } else {
            need << m;
            sizes << size_of({m});
            bytes += sizes.last();
        }
    }
    auto fail = [&](const QString &reason) {
        for (const QString &p : pinned)
            cache->unpin(p, false);
        if (why)
            *why = reason;
        return QStringList();
    };
    if (need.isEmpty())
        return out;
    if (password.isEmpty() && needs_password(need))
        return fail(tx("Encrypted items can be dragged out only once the archive's password has been given."));
    ArchiveCache::Room room = cache->reserve(bytes);
    if (room != ArchiveCache::OK)
        return fail(ArchiveCache::room_message(room));
    QString part;
    try {
        part = cache->new_part();
    } catch (const OSError &e) {
        cache->release(bytes);
        return fail(e.message());
    }
    std::atomic<bool> done{false};
    QStringList made;
    QString err;
    QString path = archive_path, pw = password;
    std::thread th([&]() {
        Task task(tx("Extracting for a drop"), [](Task *) { return QVariant(); });
        try {
            made = archive::extract_members(&task, path, part, pw, need);
        } catch (const std::exception &e) {
            err = QString::fromUtf8(e.what());
        }
        done = true;
    });
    QTimer tick;   // wakes the wait below
    tick.start(30);
    while (!done)   // the windows keep drawing meanwhile
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 30);
    th.join();
    QStringList got = err.isEmpty() ? take_in(path, need, sizes, made) : QStringList();
    cache->release(bytes);
    util::rmtree(part);
    if (!err.isEmpty() || got.contains(QString())) {
        for (const QString &p : got)
            if (!p.isEmpty())
                cache->unpin(p, false);
        return fail(err.isEmpty() ? tx("Some of the items couldn't be extracted from the archive.") : err);
    }
    for (int i = 0, k = 0; i < out.size(); ++i)
        if (out[i].isEmpty())
            out[i] = got.value(k++);
    return out;
}

void ArchiveBrowser::open_member(const QString &member)
{
    if (member.isEmpty() || member.endsWith('/'))
        return;
    QPointer<ArchiveBrowser> self(this);
    fetch({member}, tx("Opening %1 from %2").arg(basename(member), basename(archive_path)), [self](const QStringList &got) {
        ArchiveCache *cache = ArchiveCache::instance();
        QString p = got.value(0);
        // read-only: it's a copy, and changing it wouldn't change the archive
        ::chmod(enc(p).constData(), 0444);
        bool ok = open_default(p);
        cache->unpin(p, true);   // in use while the app has it open (and a while before it opens it)
        if (!ok && self)
            warn(self, tx("Open"), tx("No application is set to open “%1”.").arg(basename(p)));
    });
}

void ArchiveBrowser::quick_look()
{
    QStringList pics;
    for (const QString &m : selected_members())
        if (!m.endsWith('/') && is_image(m))
            pics << m;
    if (pics.isEmpty()) {
        status->setText(tx("Quick Look (Space) shows pictures: select one or more."));
        return;
    }
    QPointer<ArchiveBrowser> self(this);
    QPointer<MainWindow> w(owner);
    fetch(pics, tx("Viewing pictures from %1").arg(basename(archive_path)), [self, w](const QStringList &got) {
        ArchiveCache *cache = ArchiveCache::instance();
        if (!self) {
            for (const QString &p : got)
                cache->unpin(p, false);
            return;
        }
        // read-only: nothing it does is written back (no delete, no move, no folder of its own to show)
        ImageViewer::Callbacks cb;
        cb.on_open_with = [](const QString &p) { show_free(new OpenWithDialog(nullptr, {p})); };
        cb.on_transfer = [w](const QString &p, const QString &folder, const QString &) {
            if (w)
                fileops::transfer(w, {p}, folder, "copy", nullptr, "the image viewer's Copy");
        };
        auto *v = new ImageViewer(got, 0, cb);
        v->setAttribute(Qt::WA_DeleteOnClose);
        cache->hold_while(v, got);   // pinned while it shows them
        for (const QString &p : got)
            cache->unpin(p, false);   // (fetch's)
        self->viewer = v;
        v->show();
        v->activateWindow();
    });
}

void ArchiveBrowser::copy_selected()
{
    QStringList members = selected_members();
    if (members.isEmpty())
        return;
    QPointer<MainWindow> w(owner);
    fetch(members, tx("Copying from %1").arg(basename(archive_path)), [w](const QStringList &got) {
        ArchiveCache *cache = ArchiveCache::instance();
        auto *md = new QMimeData;
        md->setUrls(url_list(got));
        QStringList uris;
        for (const QString &p : got)
            uris << file_uri(p);
        md->setData("x-special/gnome-copied-files", ("copy\n" + uris.join('\n')).toUtf8());
        md->setData("application/x-kde-cutselection", "0");
        md->setData(COPY_ONLY_MIME, "1");
        md->setText(got.join('\n'));
        md->setData(ArchiveCache::CLIP_MIME, cache->hold_on_clipboard(got));   // pinned while it's on the clipboard
        for (const QString &p : got)
            cache->unpin(p, false);   // (fetch's)
        QGuiApplication::clipboard()->setMimeData(md);
        if (w)
            w->statusBar()->showMessage(txn("%Ln item(s) copied", got.size()), 3000);
    });
}

QMimeData *ArchiveBrowser::drag_data(const QStringList &members)
{
    ArchiveCache *cache = ArchiveCache::instance();
    auto *md = new CacheMime;
    md->browser = this;
    md->archive = archive_path;
    md->members = members;
    bool known = !password.isEmpty() || !needs_password(members);
    md->offer_files = known && !cache->root().isEmpty() && size_of(members) <= cache->limit();
    return md;
}

void ArchiveBrowser::drag_ended()
{
    ArchiveCache *cache = ArchiveCache::instance();
    for (const QString &p : drag_pins)
        cache->unpin(p, true);   // the drop's program may still be reading them
    drag_pins.clear();
}

// -- thumbnails: made from copies in the RAM cache, a batch at a time, kept in memory

void ArchiveBrowser::stop_thumbs()
{
    if (auto *t = qobject_cast<Task *>(thumb_job.data()))
        t->cancel();
}

void ArchiveBrowser::queue_thumbs()
{
    if (closed || thumb_job || !is_loaded || !tree || !owner || (!is_grid() && list_size < 48))
        return;
    ArchiveCache *cache = ArchiveCache::instance();
    ThumbnailManager *tm = ThumbnailManager::instance();
    qint64 max_bytes = qint64(tm ? tm->max_file_mb : 200) * 1000000;
    qint64 room = cache->limit() / 2;
    QStringList have, have_paths, need;
    QList<qint64> sizes;
    qint64 bytes = 0;
    for (const QString &m : shown_members()) {
        if (have.size() + need.size() >= THUMB_BATCH)
            break;
        const Node *n = tree->node(m);
        if (!n || n->dir || thumbs.contains(m) || thumb_failed.contains(m) || !(is_image(m) || is_video(m)))
            continue;
        if ((n->encrypted && password.isEmpty()) || n->size < 0 || n->size > max_bytes || n->size > room) {
            thumb_failed << m;
            continue;
        }
        QString p = cache->find(ArchiveCache::key(archive_path, m));
        if (!p.isEmpty()) {
            cache->pin(p);
            have << m;
            have_paths << p;
        } else if (bytes + n->size <= room) {
            need << m;
            sizes << n->size;
            bytes += n->size;
        }
    }
    if (have.isEmpty() && need.isEmpty())
        return;
    QString part;
    if (!need.isEmpty()) {
        bool ok = cache->reserve(bytes) == ArchiveCache::OK;
        if (ok) {
            try {
                part = cache->new_part();
            } catch (const OSError &) {
                cache->release(bytes);
                ok = false;
            }
        }
        if (!ok) {   // a full cache: the copies in it still get theirs, the rest none for now
            need.clear();
            bytes = 0;
            if (have.isEmpty())
                return;
        }
    }
    QString path = archive_path, pw = password;
    auto work = [path, part, pw, have, have_paths, need](Task *task) -> QVariant {
        try {
            QStringList made;
            if (!need.isEmpty()) {
                try {
                    made = archive::extract_members(task, path, part, pw, need);
                } catch (const archive::WrongPassword &) {
                }
            }
            QHash<QString, QString> by_name;
            for (const QString &f : made)
                by_name.insert(basename(f), f);
            QStringList members = have + need, files = have_paths;
            for (const QString &m : need)
                files << by_name.value(basename(m));
            QVariantList images;
            for (int i = 0; i < members.size(); ++i) {
                task->check();
                QImage img;
                if (!files[i].isEmpty())
                    img = is_video(files[i]) ? thumbs::video_frame(files[i], THUMB_SIZE) : thumbs::load_scaled(files[i], THUMB_SIZE);
                images << img;
            }
            return QVariantMap{{"members", members}, {"images", images}, {"made", made}};
        } catch (...) {
            if (!part.isEmpty())
                util::rmtree(part);
            throw;
        }
    };
    auto ended = std::make_shared<bool>(false);
    auto finish = [cache, bytes, have_paths, part, ended]() {
        if (*ended)
            return false;
        *ended = true;
        cache->release(bytes);
        for (const QString &p : have_paths)
            cache->unpin(p, false);
        if (!part.isEmpty())
            util::rmtree(part);
        return true;
    };
    QPointer<ArchiveBrowser> self(this);
    QString folder = cur;
    auto done = [self, finish, path, need, sizes, folder](const QVariant &r) mutable {
        QVariantMap m = r.toMap();
        if (r.isValid())   // the copies made are kept in the cache, for the next look (unpinned: first in, first out)
            for (const QString &p : take_in(path, need, sizes, m["made"].toStringList()))
                if (!p.isEmpty())
                    ArchiveCache::instance()->unpin(p, false);
        if (!finish())
            return;
        if (!self)
            return;
        self->thumb_job = nullptr;
        if (r.isValid()) {
            QList<QImage> images;
            for (const QVariant &v : m["images"].toList())
                images << v.value<QImage>();
            self->thumbs_made(folder, m["members"].toStringList(), images);
        }
        QTimer::singleShot(0, self, [self]() {
            if (self)
                self->queue_thumbs();   // the next batch, or the folder shown now
        });
    };
    Task *t = fileops::queue_job(owner, tx("Thumbnails from %1").arg(basename(archive_path)), {archive_path}, work, done,
                                 true, QStringList(), true, QStringList(), true);
    thumb_job = t;
    if (t) {
        QStringList all = have + need;
        connect(t, &Task::error, this, [self, finish, all]() mutable {
            if (!finish() || !self)
                return;
            self->thumb_job = nullptr;
            for (const QString &m : all)
                self->thumb_failed << m;   // not tried again and again
            self->queue_thumbs();
        });
    } else {
        finish();
    }
}

void ArchiveBrowser::thumbs_made(const QString &, const QStringList &members, const QList<QImage> &images)
{
    for (int i = 0; i < members.size(); ++i) {
        const QString &m = members[i];
        QImage img = images.value(i);
        if (img.isNull()) {
            thumb_failed << m;
            continue;
        }
        if (!thumbs.contains(m))
            thumb_order << m;
        thumbs.insert(m, QPixmap::fromImage(img));
        while (thumb_order.size() > MAX_THUMBS)
            thumbs.remove(thumb_order.takeFirst());
        model->thumb_changed(m);
    }
}

// ---------------------------------------------------------------- which window Open as Archive opens

namespace archive_ui {

void browse(MainWindow *win, const QString &path)
{
    QString missing = archive::missing_extract_tool(path);
    if (!missing.isEmpty()) {
        warn(win, tx("Open as Archive"),
             tx("Kestrel needs another program to open “%1”.\n\nInstall it with:  sudo apt install ").arg(basename(path)) +
                 missing);
        return;
    }
    if (prefs::get("archive/opens").toString() == "list")
        ArchiveWindow::open(win, path);
    else
        ArchiveBrowser::open(win, path);
}

}  // namespace archive_ui
