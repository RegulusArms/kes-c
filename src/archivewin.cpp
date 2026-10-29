#include "archivewin.h"

#include "app.h"
#include "archive_ui.h"
#include "fileops.h"
#include "windows.h"

#include <QCollator>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QTimer>
#include <QUrl>
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

#include <atomic>
#include <deque>
#include <memory>
#include <sys/stat.h>
#include <thread>

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
            item->setToolTip(0, tx("Encrypted: extracting it needs the archive's password"));
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

static void make_tree_writable(const QString &p)
{
    walk(p, [](const QString &root, QStringList &, QStringList &files) {
        ::chmod(enc(root).constData(), 0700);
        for (const QString &f : files)
            ::chmod(enc(join(root, f)).constData(), 0600);
        return true;
    });
}

// A new folder for temporary copies; the ones not used for a day are deleted first (an app may still be reading a
// newer one)
static QString new_temp_folder()
{
    QString root = ArchiveWindow::temp_root();
    qint64 now = QDateTime::currentSecsSinceEpoch();
    if (isdir(root))
        for (const QString &n : listdir(root)) {
            struct stat st;
            QString p = join(root, n);
            if (lstat_(p, st) && now - qint64(st.st_mtime) > 86400) {
                make_tree_writable(p);   // the copies are read-only
                util::rmtree(p);
            }
        }
    makedirs(root, true);
    QByteArray tmpl = enc(join(root, "XXXXXX"));
    if (!::mkdtemp(tmpl.data()))
        throw OSError(errno, tx("Couldn't make a temporary folder in %1").arg(root));
    return dec(tmpl.constData());
}

namespace {
// What items dragged out of the window carry: MEMBERS_MIME for Kestrel, and a file list made only when a drop asks
// for it (extracted then, on a thread, the window kept drawing meanwhile)
class MembersMime : public QMimeData {
public:
    QString archive, password;
    QStringList members;
    bool offer_files = false;
    mutable bool extracted = false;
    mutable QList<QUrl> files;

    QStringList formats() const override
    {
        QStringList f{ArchiveWindow::MEMBERS_MIME};
        if (offer_files)
            f << "text/uri-list";
        return f;
    }
    bool hasFormat(const QString &mime) const override { return formats().contains(mime); }

protected:
    QVariant retrieveData(const QString &mime, QMetaType type) const override
    {
        if (mime == ArchiveWindow::MEMBERS_MIME)
            return QJsonDocument(QJsonObject{{"archive", archive}, {"members", QJsonArray::fromStringList(members)}})
                .toJson(QJsonDocument::Compact);
        if (mime != "text/uri-list" || !offer_files)
            return QVariant();
        if (!extracted) {
            extracted = true;
            extract_now();
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

private:
    void extract_now() const
    {
        std::atomic<bool> done{false};
        QStringList made;
        std::thread t([&]() {
            Task task(tx("Extracting for a drop"), [](Task *) { return QVariant(); });
            try {
                made = archive::extract_members(&task, archive, new_temp_folder(), password, members);
            } catch (const std::exception &) {
            }
            done = true;
        });
        QTimer tick;   // wakes the wait below
        tick.start(30);
        while (!done)   // the windows keep drawing meanwhile
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 30);
        t.join();
        for (const QString &p : made)
            files << QUrl::fromLocalFile(p);
    }
};

class MemberTree : public QTreeWidget {
public:
    explicit MemberTree(ArchiveWindow *win) : win(win) {}

protected:
    QStringList mimeTypes() const override { return {ArchiveWindow::MEMBERS_MIME, "text/uri-list"}; }
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override
    {
        QStringList members;
        for (QTreeWidgetItem *i : items)
            members << i->data(0, PathRole).toString();
        return win->drag_data(members);
    }
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction; }

private:
    ArchiveWindow *win;
};
}  // namespace

static QList<QPointer<ArchiveWindow>> &open_windows()
{
    static QList<QPointer<ArchiveWindow>> list;
    return list;
}

ArchiveWindow *ArchiveWindow::open(MainWindow *owner, const QString &path_in)
{
    QString path = archive::first_volume(path_in);
    for (const QPointer<ArchiveWindow> &w : open_windows())
        if (w && w->archive_path == path && w->isVisible()) {   // (a closed one is about to be deleted)
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
    setWindowTitle(tx("%1 — Archive").arg(basename(path)));
    auto *lay = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    extract_here = new QPushButton(tx("Extract Here"));
    extract_here->setToolTip(tx("Extract the selected items next to the archive"));
    extract_to = new QPushButton(tx("Extract To…"));
    extract_to->setToolTip(tx("Extract the selected items into a folder you choose"));
    extract_all = new QPushButton(tx("Extract All…"));
    test = new QPushButton(tx("Test"));
    test->setToolTip(tx("Check every file in the archive against its checksum, without extracting"));
    filter = new QLineEdit;
    filter->setPlaceholderText(tx("Filter by name"));
    filter->setClearButtonEnabled(true);
    for (QPushButton *b : {extract_here, extract_to, extract_all, test})
        bar->addWidget(b);
    bar->addStretch(1);
    bar->addWidget(filter);
    lay->addLayout(bar);
    tree = new MemberTree(this);
    tree->setDragEnabled(true);
    tree->setDragDropMode(QAbstractItemView::DragOnly);
    tree->setDefaultDropAction(Qt::CopyAction);
    tree->setHeaderLabels({tx("Name"), tx("Size"), tx("Packed"), tx("Modified")});
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
    status = new QLabel(tx("Reading the archive…"));
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
    connect(tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {   // double-click, Enter
        if (is_loaded && item && !item->data(0, DirRole).toBool())
            open_member(item->data(0, PathRole).toString());
    });
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
            return QVariantMap{{"error", tx("Kestrel needs another program to open it. Install it with:  sudo apt install %1").arg(missing)}};
        QString pw;
        QStringList tried;
        for (;;) {
            try {
                got->entries = archive::list(task, path, pw);
                got->password = pw;
                task->report(0, 0, txn("Reading %1 — sorting %Ln item(s)", got->entries.size()).arg(basename(path)), true);
                got->build();
                return QVariantMap{{"ok", true}};
            } catch (const archive::WrongPassword &) {   // its names are encrypted
                if (!pw.isEmpty())
                    tried << pw;
                pw = archive_ui::job_password(task, asker, path, tried);
                if (pw.isEmpty())
                    return QVariantMap{{"error", tx("Its contents can't be shown without its password.")}};
            }
        }
    };
    QPointer<ArchiveWindow> self(this);
    auto done = [self, got](const QVariant &r) {
        if (!self)
            return;
        QVariantMap m = r.toMap();
        if (!r.isValid()) {
            self->status->setText(tx("Reading the archive was cancelled."));
        } else if (!m["ok"].toBool()) {
            self->status->setText(m["error"].toString());
        } else {
            self->password = got->password;
            self->show_listing(*got);
        }
    };
    Task *t = fileops::queue_job(owner, tx("Reading %1").arg(basename(archive_path)), {archive_path}, work, done);
    if (t)
        connect(t, &Task::error, this, [self](const QString &msg) {
            if (self)
                self->status->setText(tx("Couldn't read the archive: %1").arg(msg));
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
    status->setText(tx("%1 in %2, %3%4. Select items to extract them; a folder brings what's inside.")
                        .arg(txn("%Ln file(s)", l.files), txn("%Ln folder(s)", l.folders), human_size(l.total),
                             l.encrypted || !password.isEmpty() ? tx(" · encrypted") : QString()));
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
    QTreeWidgetItem *cur = tree->currentItem();
    if (cur && tree->selectedItems().size() == 1 && !cur->data(0, DirRole).toBool()) {
        QString member = cur->data(0, PathRole).toString();
        m->addAction(tx("Open"), this, [this, member]() { open_member(member); });
        m->addSeparator();
    }
    m->addAction(tx("Extract Here"), this, [this]() { extract_selected(dirname(archive_path)); });
    m->addAction(tx("Extract To…"), this, &ArchiveWindow::choose_and_extract);
    m->popup(tree->viewport()->mapToGlobal(pos));
    connect(m, &QMenu::aboutToHide, m, &QObject::deleteLater);
}

void ArchiveWindow::choose_and_extract()
{
    QPointer<ArchiveWindow> self(this);
    choose_folder(this, tx("Extract To"), dirname(archive_path), [self](const QString &d) {
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
    QPointer<MainWindow> w(owner);
    QString name = basename(archive_path);
    extract_job(owner, this, archive_path, members, dest, tx("Extracting from %1").arg(name), [w, dest](const QStringList &made) {
        if (w && w->pane() && w->pane()->dir() == dest)
            for (const QString &p : made)
                w->pane()->select_later(p);
    });
}

void ArchiveWindow::extract_job(MainWindow *owner, QPointer<ArchiveWindow> win, const QString &path,
                                const QStringList &members, const QString &dest, const QString &title,
                                std::function<void(const QStringList &)> after)
{
    if (members.isEmpty() || !owner)
        return;
    auto pw = std::make_shared<QString>(win ? win->password : QString());
    bool ask_first = win && pw->isEmpty() && win->needs_password(members);
    QPointer<QWidget> asker = win ? static_cast<QWidget *>(win.data()) : static_cast<QWidget *>(owner);
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
    QPointer<MainWindow> w(owner);
    QString name = basename(path);
    auto done = [win, w, pw, name, dest, after](const QVariant &r) {
        if (!w)
            return;
        if (!r.isValid()) {
            w->statusBar()->showMessage(tx("Extracting from %1 cancelled").arg(name), 4000);
            return;
        }
        QVariantMap m = r.toMap();
        if (m["no_password"].toBool()) {
            w->statusBar()->showMessage(tx("Extracting from %1 cancelled: no password").arg(name), 4000);
            return;
        }
        if (win && !pw->isEmpty())
            win->password = *pw;   // not asked again while this window is open
        QStringList made = m["made"].toStringList();
        w->statusBar()->showMessage(txn("Extracted %Ln item(s) from %1 to %2", made.size()).arg(name, dest), 6000);
        if (!m["dropped"].toList().isEmpty())
            archive_ui::say_dropped_links(win ? static_cast<QWidget *>(win) : w, name, m["dropped"].toList());
        if (after)
            after(made);
    };
    QStringList steps;
    for (const QString &m : members)
        steps << m;
    fileops::queue_job(owner, title, {path, dest}, work, done, true, members.size() > 1 ? steps : QStringList());
}

QString ArchiveWindow::temp_root() { return join(APP_CACHE(), "archive-files"); }

void ArchiveWindow::open_member(const QString &member)
{
    if (!owner || member.isEmpty() || member.endsWith('/'))
        return;
    QString dir;
    try {
        dir = new_temp_folder();
    } catch (const OSError &e) {
        warn(this, tx("Open"), e.message());
        return;
    }
    QPointer<ArchiveWindow> self(this);
    extract_job(owner, this, archive_path, {member}, dir, "Opening " + basename(rstrip(member, '/')) + " from " +
                                                              basename(archive_path),
                [self](const QStringList &made) {
                    if (made.isEmpty())
                        return;
                    // read-only: it's a copy, and changing it wouldn't change the archive
                    ::chmod(enc(made.first()).constData(), 0444);
                    if (!open_default(made.first()) && self)
                        warn(self, tx("Open"), tx("No application is set to open “%1”.").arg(basename(made.first())));
                });
}

qint64 ArchiveWindow::size_of(const QStringList &members) const
{
    qint64 total = 0;
    for (const archive::Entry &e : entries)
        if (!e.dir)
            for (const QString &m : members)
                if (e.path == m || (m.endsWith('/') && e.path.startsWith(m))) {
                    total += std::max<qint64>(0, e.size);
                    break;
                }
    return total;
}


QMimeData *ArchiveWindow::drag_data(const QStringList &members) const
{
    auto *md = new MembersMime;
    md->archive = archive_path;
    md->members = members;
    md->password = password;
    // other apps need finished files: offered when they can be made quickly enough and without asking
    md->offer_files = size_of(members) <= BIG_DRAG && (!password.isEmpty() || !needs_password(members));
    return md;
}

bool ArchiveWindow::drop_members(MainWindow *win, const QMimeData *data, const QString &dest)
{
    if (!data || !data->hasFormat(MEMBERS_MIME) || !win)
        return false;
    QJsonObject o = QJsonDocument::fromJson(data->data(MEMBERS_MIME)).object();
    QString path = o.value("archive").toString();
    QStringList members;
    for (const QJsonValue &v : o.value("members").toArray())
        members << v.toString();
    if (!path.startsWith('/') || members.isEmpty())
        return false;
    QPointer<ArchiveWindow> from;
    for (const QPointer<ArchiveWindow> &w : open_windows())
        if (w && w->archive_path == path)
            from = w;
    QPointer<MainWindow> mw(win);
    extract_job(win, from, path, members, dest, "Extracting from " + basename(path), [mw, dest](const QStringList &made) {
        if (mw && mw->pane() && mw->pane()->dir() == dest)
            for (const QString &p : made)
                mw->pane()->select_later(p);
    });
    return true;
}

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
    ArchiveWindow::open(win, path);
}

}  // namespace archive_ui
