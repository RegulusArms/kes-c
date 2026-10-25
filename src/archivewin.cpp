#include "archivewin.h"

#include "app.h"
#include "archive_ui.h"
#include "fileops.h"
#include "windows.h"

#include <QCollator>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QStatusBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <deque>
#include <memory>

using namespace util;

enum { PathRole = Qt::UserRole + 1, DirRole, SizeRole, TimeRole };

// folders first, then by the column's value (sizes and times as numbers). A file's icon is looked up when its row is
// drawn: an archive can hold a million files, and theme icons can only be made on the UI thread.
class EntryItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    QStringList icon_names;   // theme names, the first one the theme has
    QVariant data(int column, int role) const override
    {
        if (column == 0 && role == Qt::DecorationRole && !icon_names.isEmpty())
            return theme_icon(icon_names);
        return QTreeWidgetItem::data(column, role);
    }
    bool operator<(const QTreeWidgetItem &other) const override
    {
        bool a = data(0, DirRole).toBool(), b = other.data(0, DirRole).toBool();
        if (a != b) {
            Qt::SortOrder order = treeWidget() ? treeWidget()->header()->sortIndicatorOrder() : Qt::AscendingOrder;
            return order == Qt::AscendingOrder ? a : b;   // folders stay on top either way
        }
        int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (col == 1 || col == 2)
            return data(col, SizeRole).toLongLong() < other.data(col, SizeRole).toLongLong();
        if (col == 3)
            return data(0, TimeRole).toDateTime() < other.data(0, TimeRole).toDateTime();
        return QString::localeAwareCompare(text(0), other.text(0)) < 0;
    }
};

// The listing as items, built by the reading job (off the UI thread, which a big archive's would freeze for seconds),
// each folder's items in the window's first order (folders first, then by name). They're the window's once shown;
// until then (a closed window, a cancelled job) they're deleted with this.
struct ArchiveWindow::Listing {
    QList<archive::Entry> entries;
    QString password;
    QList<QTreeWidgetItem *> top;
    int files = 0, folders = 0;
    qint64 total = 0;
    bool encrypted = false;
    ~Listing() { qDeleteAll(top); }
    void build();
};

void ArchiveWindow::Listing::build()
{
    struct Node {
        EntryItem *item;
        QCollatorSortKey key;
        bool dir;
        std::vector<Node *> children;
    };
    std::deque<Node> nodes;
    std::vector<Node *> roots;
    QHash<QString, Node *> dirs;   // "a/b/" -> its node
    QCollator collator;
    QLocale loc;
    auto add = [&](Node *parent, EntryItem *item, const QString &name, bool dir) {
        item->setText(0, name);
        nodes.push_back(Node{item, collator.sortKey(name), dir, {}});
        (parent ? parent->children : roots).push_back(&nodes.back());
        return &nodes.back();
    };
    std::function<Node *(const QString &)> folder = [&](const QString &dir) -> Node * {
        if (dir.isEmpty())
            return nullptr;
        if (Node *f = dirs.value(dir))
            return f;
        QString parent_dir = dir.left(dir.left(dir.size() - 1).lastIndexOf('/') + 1);
        Node *parent = folder(parent_dir);
        auto *item = new EntryItem;
        item->icon_names = QStringList{"folder"};
        item->setData(0, PathRole, dir);
        item->setData(0, DirRole, true);
        Node *n = add(parent, item, basename(dir.left(dir.size() - 1)), true);
        dirs.insert(dir, n);
        return n;
    };
    QHash<QString, QStringList> icons;   // by extension: the MIME type's icon names
    for (const archive::Entry &e : entries) {
        if (e.dir) {
            Node *n = folder(e.path);
            if (e.modified.isValid()) {
                n->item->setText(3, loc.toString(e.modified, QLocale::ShortFormat));
                n->item->setData(0, TimeRole, e.modified);
            }
            continue;
        }
        Node *parent = folder(e.path.left(e.path.lastIndexOf('/') + 1));
        auto *item = new EntryItem;
        QString name = basename(e.path);
        QString ext = name.contains('.') ? name.mid(name.lastIndexOf('.')).toLower() : name;
        auto it = icons.find(ext);
        if (it == icons.end()) {
            QMimeType m = mime_for(name, 0);
            it = icons.insert(ext, {m.iconName(), m.genericIconName(), "text-x-generic"});
        }
        item->icon_names = *it;
        item->setData(0, PathRole, e.path);
        item->setData(0, DirRole, false);
        if (e.size >= 0) {
            item->setText(1, human_size(e.size));
            item->setData(1, SizeRole, e.size);
            total += e.size;
        }
        if (e.packed >= 0) {
            item->setText(2, human_size(e.packed));
            item->setData(2, SizeRole, e.packed);
        }
        if (e.modified.isValid()) {
            item->setText(3, loc.toString(e.modified, QLocale::ShortFormat));
            item->setData(0, TimeRole, e.modified);
        }
        if (e.encrypted)
            item->setToolTip(0, "Encrypted: extracting it needs the archive's password");
        add(parent, item, e.encrypted ? name + "  🔒" : name, false);
        encrypted = encrypted || e.encrypted;
        ++files;
    }
    folders = int(dirs.size());
    auto order = [](const Node *a, const Node *b) { return a->dir != b->dir ? a->dir : a->key.compare(b->key) < 0; };
    for (Node &n : nodes) {
        std::stable_sort(n.children.begin(), n.children.end(), order);
        QList<QTreeWidgetItem *> kids;
        for (Node *c : n.children)
            kids << c->item;
        n.item->addChildren(kids);
    }
    std::stable_sort(roots.begin(), roots.end(), order);
    for (Node *r : roots)
        top << r->item;
}

static QList<QPointer<ArchiveWindow>> &open_windows()
{
    static QList<QPointer<ArchiveWindow>> list;
    return list;
}

ArchiveWindow *ArchiveWindow::open(MainWindow *owner, const QString &path_in)
{
    QString path = archive::first_volume(path_in);
    for (const QPointer<ArchiveWindow> &w : open_windows())
        if (w && w->archive_path == path) {
            w->raise();
            w->activateWindow();
            return w;
        }
    auto *w = new ArchiveWindow(owner, path);
    open_windows() << w;
    show_free(w);
    w->load();
    return w;
}

ArchiveWindow::ArchiveWindow(MainWindow *owner_, const QString &path) : QWidget(owner_), owner(owner_), archive_path(path)
{
    setObjectName("archive_window");
    setWindowTitle(basename(path) + " — Archive");
    auto *lay = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    extract_here = new QPushButton("Extract Here");
    extract_here->setToolTip("Extract the selected items next to the archive");
    extract_to = new QPushButton("Extract To…");
    extract_to->setToolTip("Extract the selected items into a folder you choose");
    extract_all = new QPushButton("Extract All…");
    test = new QPushButton("Test");
    test->setToolTip("Check every file in the archive against its checksum, without extracting");
    filter = new QLineEdit;
    filter->setPlaceholderText("Filter by name");
    filter->setClearButtonEnabled(true);
    for (QPushButton *b : {extract_here, extract_to, extract_all, test})
        bar->addWidget(b);
    bar->addStretch(1);
    bar->addWidget(filter);
    lay->addLayout(bar);
    tree = new QTreeWidget;
    tree->setHeaderLabels({"Name", "Size", "Packed", "Modified"});
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // sorted when a column's header is clicked, but not kept sorted by the tree itself, which would sort a big
    // listing again on the UI thread once it's added (it comes sorted by name)
    tree->header()->setSortIndicator(0, Qt::AscendingOrder);
    tree->header()->setSortIndicatorShown(true);
    tree->header()->setSectionsClickable(true);
    connect(tree->header(), &QHeaderView::sortIndicatorChanged, tree,
            [this](int col, Qt::SortOrder order) { tree->sortItems(col, order); });
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setStretchLastSection(false);
    lay->addWidget(tree, 1);
    status = new QLabel("Reading the archive…");
    status->setWordWrap(true);
    lay->addWidget(status);

    connect(extract_here, &QPushButton::clicked, this, [this]() { extract_selected(dirname(archive_path)); });
    connect(extract_to, &QPushButton::clicked, this, &ArchiveWindow::choose_and_extract);
    connect(extract_all, &QPushButton::clicked, this, [this]() {
        if (owner)
            archive_ui::extract_dialog(owner, archive_path);
    });
    connect(test, &QPushButton::clicked, this, [this]() {
        if (owner)
            archive_ui::test_archives(owner, {archive_path});
    });
    connect(filter, &QLineEdit::textChanged, this, &ArchiveWindow::apply_filter);
    connect(tree, &QTreeWidget::itemSelectionChanged, this, &ArchiveWindow::selection_changed);
    connect(tree, &QTreeWidget::customContextMenuRequested, this, &ArchiveWindow::context_menu);
    for (QPushButton *b : {extract_here, extract_to, extract_all, test})
        b->setEnabled(false);
    resize(760, 520);
}

void ArchiveWindow::load()
{
    if (!owner)
        return;
    auto got = std::make_shared<Listing>();
    QPointer<QWidget> asker(this);
    QString path = archive_path;
    auto work = [got, asker, path](Task *task) -> QVariant {
        QString missing = archive::missing_extract_tool(path);
        if (!missing.isEmpty())
            return QVariantMap{{"error", QString("Kestrel needs another program to open it. Install it with:  sudo apt "
                                                 "install ") + missing}};
        QString pw;
        QStringList tried;
        for (;;) {
            try {
                got->entries = archive::list(task, path, pw);
                got->password = pw;
                task->report(0, 0, "Reading " + basename(path) + " — sorting " + QString::number(got->entries.size()) +
                                       " items", true);
                got->build();
                return QVariantMap{{"ok", true}};
            } catch (const archive::WrongPassword &) {   // its names are encrypted
                if (!pw.isEmpty())
                    tried << pw;
                pw = archive_ui::job_password(task, asker, path, tried);
                if (pw.isEmpty())
                    return QVariantMap{{"error", QString("Its contents can't be shown without its password.")}};
            }
        }
    };
    QPointer<ArchiveWindow> self(this);
    auto done = [self, got](const QVariant &r) {
        if (!self)
            return;
        QVariantMap m = r.toMap();
        if (!r.isValid()) {
            self->status->setText("Reading the archive was cancelled.");
        } else if (!m["ok"].toBool()) {
            self->status->setText(m["error"].toString());
        } else {
            self->password = got->password;
            self->show_listing(*got);
        }
    };
    Task *t = fileops::queue_job(owner, "Reading " + basename(archive_path), {archive_path}, work, done);
    if (t)
        connect(t, &Task::error, this, [self](const QString &msg) {
            if (self)
                self->status->setText("Couldn't read the archive: " + msg);
        });
}

void ArchiveWindow::show_listing(Listing &l)
{
    entries = l.entries;
    tree->clear();
    tree->addTopLevelItems(l.top);
    l.top.clear();
    // they come in the window's first order; sorted again only if a column was clicked meanwhile
    if (tree->sortColumn() != 0 || tree->header()->sortIndicatorOrder() != Qt::AscendingOrder)
        tree->sortItems(tree->sortColumn(), tree->header()->sortIndicatorOrder());
    if (tree->topLevelItemCount() == 1)
        tree->topLevelItem(0)->setExpanded(true);
    for (int c = 1; c < 4; ++c)
        tree->resizeColumnToContents(c);
    status->setText(QString("%1 file%2 in %3 folder%4, %5%6. Select items to extract them; a folder brings what's "
                            "inside.")
                        .arg(QString::number(l.files), l.files == 1 ? "" : "s", QString::number(l.folders),
                             l.folders == 1 ? "" : "s", human_size(l.total),
                             l.encrypted || !password.isEmpty() ? " · encrypted" : ""));
    is_loaded = true;
    extract_all->setEnabled(true);
    test->setEnabled(archive::can_test(archive_path));
    selection_changed();
}

void ArchiveWindow::apply_filter()
{
    QString f = filter->text().trimmed();
    // an item shows when its name matches, or something inside it does (a folder's contents stay when it matches)
    std::function<bool(QTreeWidgetItem *, bool)> visit = [&](QTreeWidgetItem *item, bool shown_by_parent) {
        bool match = f.isEmpty() || shown_by_parent || item->text(0).contains(f, Qt::CaseInsensitive);
        bool any_child = false;
        for (int i = 0; i < item->childCount(); ++i)
            any_child = visit(item->child(i), match && !f.isEmpty() && item->data(0, DirRole).toBool() &&
                                                  item->text(0).contains(f, Qt::CaseInsensitive)) || any_child;
        bool show = match || any_child;
        item->setHidden(!show);
        if (!f.isEmpty() && any_child)
            item->setExpanded(true);
        return show;
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        visit(tree->topLevelItem(i), false);
}

QStringList ArchiveWindow::selected_members() const
{
    QStringList out;
    for (QTreeWidgetItem *item : tree->selectedItems())
        out << item->data(0, PathRole).toString();
    return out;
}

void ArchiveWindow::selection_changed()
{
    bool any = is_loaded && !tree->selectedItems().isEmpty();
    extract_here->setEnabled(any);
    extract_to->setEnabled(any);
}

void ArchiveWindow::context_menu(const QPoint &pos)
{
    if (!is_loaded || tree->selectedItems().isEmpty())
        return;
    auto *m = new QMenu(this);
    m->addAction("Extract Here", this, [this]() { extract_selected(dirname(archive_path)); });
    m->addAction("Extract To…", this, &ArchiveWindow::choose_and_extract);
    m->popup(tree->viewport()->mapToGlobal(pos));
    connect(m, &QMenu::aboutToHide, m, &QObject::deleteLater);
}

void ArchiveWindow::choose_and_extract()
{
    QPointer<ArchiveWindow> self(this);
    choose_folder(this, "Extract To", dirname(archive_path), [self](const QString &d) {
        if (self)
            self->extract_selected(d);
    });
}

bool ArchiveWindow::needs_password(const QStringList &members) const
{
    for (const archive::Entry &e : entries)
        if (e.encrypted)
            for (const QString &m : members)
                if (e.path == m || (m.endsWith('/') && e.path.startsWith(m)))
                    return true;
    return false;
}

void ArchiveWindow::extract_selected(const QString &dest)
{
    QStringList members = selected_members();
    if (members.isEmpty() || !owner)
        return;
    QString path = archive_path;
    auto pw = std::make_shared<QString>(password);
    bool ask_first = password.isEmpty() && needs_password(members);
    QPointer<QWidget> asker(this);
    auto work = [path, dest, members, pw, ask_first, asker](Task *task) -> QVariant {
        QStringList tried;
        if (ask_first) {
            *pw = archive_ui::job_password(task, asker, path, tried);
            if (pw->isEmpty())
                return QVariantMap{{"no_password", true}};
        }
        for (;;) {
            QList<archive::DroppedLink> dropped;
            try {
                QStringList made = archive::extract_members(task, path, dest, *pw, members, &dropped);
                QVariantList links;
                for (const archive::DroppedLink &l : dropped)
                    links << QVariant(QStringList{l.path, l.target, l.why});
                return QVariantMap{{"made", made}, {"dropped", links}};
            } catch (const archive::WrongPassword &) {
                if (!pw->isEmpty())
                    tried << *pw;
                *pw = archive_ui::job_password(task, asker, path, tried);
                if (pw->isEmpty())
                    return QVariantMap{{"no_password", true}};
            }
        }
    };
    QPointer<ArchiveWindow> self(this);
    QPointer<MainWindow> w(owner);
    QString name = basename(path);
    auto done = [self, w, pw, name, dest](const QVariant &r) {
        if (!w)
            return;
        if (!r.isValid()) {
            w->statusBar()->showMessage("Extracting from " + name + " cancelled", 4000);
            return;
        }
        QVariantMap m = r.toMap();
        if (m["no_password"].toBool()) {
            w->statusBar()->showMessage("Extracting from " + name + " cancelled: no password", 4000);
            return;
        }
        if (self && !pw->isEmpty())
            self->password = *pw;   // not asked again while this window is open
        QStringList made = m["made"].toStringList();
        w->statusBar()->showMessage(QString("Extracted %1 item%2 from %3 to %4")
                                        .arg(QString::number(made.size()), made.size() == 1 ? "" : "s", name, dest),
                                    6000);
        if (!m["dropped"].toList().isEmpty())
            archive_ui::say_dropped_links(self ? static_cast<QWidget *>(self) : w, name, m["dropped"].toList());
        if (w->pane() && w->pane()->dir() == dest)
            for (const QString &p : made)
                w->pane()->select_later(p);
    };
    QStringList steps;
    for (const QString &m : members)
        steps << m;
    fileops::queue_job(owner, "Extracting from " + name, {path, dest}, work, done, true,
                       members.size() > 1 ? steps : QStringList());
}

namespace archive_ui {

void browse(MainWindow *win, const QString &path)
{
    QString missing = archive::missing_extract_tool(path);
    if (!missing.isEmpty()) {
        warn(win, "Open as Archive",
             QString("Kestrel needs another program to open “%1”.\n\nInstall it with:  sudo apt install ").arg(basename(path)) +
                 missing);
        return;
    }
    ArchiveWindow::open(win, path);
}

}  // namespace archive_ui
