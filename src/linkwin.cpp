#include "linkwin.h"

#include "admin.h"
#include "app.h"
#include "fileops.h"
#include "i18n.h"
#include "undo.h"
#include "util.h"
#include "windows.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <sys/stat.h>

#include <memory>

using namespace util;

static const int ID_ROLE = Qt::UserRole, TARGET_ROLE = Qt::UserRole + 1;
static const int MAX_PREVIEWS = 200;   // picks shown as links-to-be (a whole folder selected: not thousands)

// A link-to-be's icon: what it points to, faded, with the link emblem (fainter still for a pick not in the list yet)
static QIcon ghost_icon(const links::Link &l, bool preview)
{
    const int S = 48;
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setOpacity(preview ? 0.35 : 0.6);
    QIcon base = lexists(l.points_to) ? icon_for_path(l.points_to) : theme_icon({"image-missing", "dialog-question"});
    base.paint(&p, QRect(0, 0, S, S));
    if (l.kind != "hard") {
        p.setOpacity(preview ? 0.6 : 0.9);
        theme_icon({"emblem-symbolic-link", "insert-link"}).paint(&p, QRect(S / 2, S / 2, S / 2, S / 2));
    }
    p.end();
    return QIcon(pm);
}

static QString change_text(const links::Link &l)
{
    if (!l.is_edit())
        return tx("New");
    bool moved = l.path() != l.existing, changed = l.text() != l.old_text;
    if (moved && changed)
        return tx("Points elsewhere, renamed");
    if (moved)
        return dirname(l.path()) == dirname(l.existing) ? tx("Renamed") : tx("Moved");
    return changed ? tx("Points elsewhere") : tx("No change yet");
}

// the nearest folder above path that's there (a broken link's target's folder may be gone too)
static QString nearest_folder(QString p)
{
    p = dirname(p);
    while (p != "/" && !p.isEmpty() && !isdir(p))
        p = dirname(p);
    return p.isEmpty() ? QString("/") : p;
}

static QLabel *heading(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

static QLabel *hint(const QString &text)
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}

LinkWindow::LinkWindow(MainWindow *w, const QString &left_dir, const QString &right_dir) : QWidget(w), win(w)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tx("Link"));
    setWindowIcon(theme_icon({"emblem-symbolic-link", "insert-link"}));
    resize(1320, 900);
    ThumbnailManager *thumbs = w ? w->thumbs : g_thumbs;

    // the two browsers: the file chooser's (chooser-mode Kestrel windows, embedded)
    left = new MainWindow({left_dir}, thumbs, true);
    left->embed([this](const QStringList &) {   // a file double-clicked: picked
        if (selected_ids().size() == 1)
            change_selected();
        else
            add_picks();
    });
    right = new MainWindow({right_dir}, thumbs, true);
    right->embed();
    left->setAccessibleName(tx("Points to"));
    right->setAccessibleName(tx("Link goes in"));

    auto *lcol = new QWidget;
    auto *ll = new QVBoxLayout(lcol);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->addWidget(heading(tx("Points to")));
    ll->addWidget(hint(tx("What the links lead to: pick one item, or select several.")));
    ll->addWidget(left, 1);

    ghosts = new QListWidget;
    ghosts->setAccessibleName(tx("Links to be made in this folder"));
    ghosts->setViewMode(QListView::IconMode);
    ghosts->setIconSize(QSize(40, 40));
    ghosts->setGridSize(QSize(140, 76));
    ghosts->setWordWrap(true);
    ghosts->setResizeMode(QListView::Adjust);
    ghosts->setMovement(QListView::Static);
    ghosts->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ghosts->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                            QAbstractItemView::SelectedClicked);
    ghosts->setMaximumHeight(170);
    ghost_label = hint(QString());
    auto *rcol = new QWidget;
    auto *rl = new QVBoxLayout(rcol);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(heading(tx("Link goes in")));
    rl->addWidget(hint(tx("The folder the links are made in. They show below, among what's here, until they're made.")));
    rl->addWidget(right, 1);
    rl->addWidget(ghost_label);
    rl->addWidget(ghosts);

    auto *sides = new QSplitter(Qt::Horizontal);
    sides->addWidget(lcol);
    sides->addWidget(rcol);
    sides->setChildrenCollapsible(false);

    // what kind, and adding to the list
    kind = new QComboBox;
    kind->setAccessibleName(tx("Kind of link"));
    for (const QString &k : links::kinds())
        kind->addItem(links::kind_label(k), k);
    kind->setItemData(kind->findData("rel"), tx("A relative path: the link keeps working when it's moved together with "
                                                "what it points to"), Qt::ToolTipRole);
    kind->setItemData(kind->findData("hard"), tx("Another name of the same file: files only, on the same drive"),
                      Qt::ToolTipRole);
    kind->setItemData(kind->findData("desktop"), tx("A .desktop file that opens it, as a launcher on a desktop or dock"),
                      Qt::ToolTipRole);
    pick_info = new QLabel;
    pick_info->setWordWrap(true);
    find_btn = new QPushButton(tx("Find the Other Names"));
    find_btn->setToolTip(tx("Searches its drive for the other names of this file (a job: slow on a big drive)"));
    add_btn = new QPushButton(theme_icon({"list-add-symbolic", "list-add"}), tx("Add to List"));
    add_btn->setToolTip(tx("Keeps the links shown on the right in the list, to be made with OK"));
    change_btn = new QPushButton(tx("Change Link"));
    change_btn->setToolTip(tx("The link selected in the list points to what's picked on the left, in the folder shown on "
                              "the right"));
    auto *opts = new QHBoxLayout;
    opts->addWidget(new QLabel(tx("Kind:")));
    opts->addWidget(kind);
    opts->addWidget(pick_info, 1);
    opts->addWidget(find_btn);
    opts->addWidget(add_btn);
    opts->addWidget(change_btn);

    list = new QTreeWidget;
    list->setAccessibleName(tx("Links to make or change"));
    list->setColumnCount(6);
    list->setHeaderLabels({tx("Name"), tx("Folder"), tx("Points to"), tx("Kind"), tx("Change"), tx("Problems")});
    list->setRootIsDecorated(false);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->header()->setStretchLastSection(true);
    list->setColumnWidth(0, 200);
    list->setColumnWidth(1, 260);
    list->setColumnWidth(2, 300);
    list->setColumnWidth(3, 150);
    list->setColumnWidth(4, 150);
    remove_btn = new QPushButton(theme_icon({"list-remove-symbolic", "list-remove"}), tx("Remove"));
    remove_btn->setToolTip(tx("Takes the selected links out of the list (nothing on the disk changes)"));
    auto *close_btn = new QPushButton(tx("Close"));
    ok_btn = new QPushButton(tx("OK"));
    ok_btn->setToolTip(tx("Shows what will be made or changed, to confirm"));
    ok_btn->setDefault(true);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(remove_btn);
    bottom->addStretch(1);
    bottom->addWidget(close_btn);
    bottom->addWidget(ok_btn);

    auto *lower = new QWidget;
    auto *lw = new QVBoxLayout(lower);
    lw->setContentsMargins(0, 0, 0, 0);
    lw->addLayout(opts);
    lw->addWidget(heading(tx("Links to make or change")));
    lw->addWidget(hint(tx("Nothing is made or changed until OK. Click a link to show it on both sides and change it.")));
    lw->addWidget(list, 1);
    lw->addLayout(bottom);

    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(sides);
    split->addWidget(lower);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setChildrenCollapsible(false);
    auto *lay = new QVBoxLayout(this);
    lay->addWidget(split);

    auto picks_changed = [this]() {
        refresh_ghosts();
        update_buttons();
        update_pick_info();
    };
    connect(left, &MainWindow::selection_changed, this, picks_changed);
    connect(left, &MainWindow::folder_changed, this, picks_changed);
    connect(right, &MainWindow::folder_changed, this, picks_changed);
    connect(kind, &QComboBox::currentIndexChanged, this, &LinkWindow::kind_changed);
    connect(list, &QTreeWidget::itemSelectionChanged, this, &LinkWindow::list_selected);
    connect(ghosts, &QListWidget::itemChanged, this, &LinkWindow::ghost_renamed);
    // a link-to-be clicked: selected in the list too (unless it's the one already: a second click renames it in place)
    connect(ghosts, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        int id = it->data(ID_ROLE).toInt();
        if (!id || selected_ids() == QList<int>{id})
            return;
        QTimer::singleShot(0, this, [this, id]() {   // (not while the strip is still handling the click)
            for (int i = 0; i < list->topLevelItemCount(); ++i)
                if (list->topLevelItem(i)->data(0, ID_ROLE).toInt() == id)
                    list->setCurrentItem(list->topLevelItem(i));
        });
    });
    connect(add_btn, &QPushButton::clicked, this, &LinkWindow::add_picks);
    connect(change_btn, &QPushButton::clicked, this, &LinkWindow::change_selected);
    connect(remove_btn, &QPushButton::clicked, this, &LinkWindow::remove_selected);
    connect(find_btn, &QPushButton::clicked, this, &LinkWindow::find_names);
    connect(close_btn, &QPushButton::clicked, this, &QWidget::close);
    connect(ok_btn, &QPushButton::clicked, this, &LinkWindow::commit);
    refresh();
}

LinkWindow *LinkWindow::open(MainWindow *win, const QStringList &picks_in, const QString &folder, const QStringList &edit)
{
    QList<links::Link> edits;
    QStringList bad;
    for (const QString &p : edit) {
        try {
            edits << links::from_existing(p);
        } catch (const OSError &e) {
            bad << p + ": " + e.message();
        }
    }
    QString cur = win ? win->cur_dir() : QString();
    if (cur.isEmpty())
        cur = HOME();
    QStringList sel;
    for (const QString &p : picks_in)
        sel << abspath(p);
    QString left_dir = cur;
    if (!sel.isEmpty())
        left_dir = dirname(sel.first());
    else if (!edits.isEmpty())
        left_dir = nearest_folder(edits.first().points_to);
    QString right_dir = !folder.isEmpty() ? folder : !edits.isEmpty() ? edits.first().folder : cur;
    auto *w = new LinkWindow(win, left_dir, right_dir);
    for (const links::Link &l : edits)
        w->add(l);
    w->refresh();
    show_free(w);
    if (!edits.isEmpty()) {   // the first one shown on both sides, to change it
        w->list->setCurrentItem(w->list->topLevelItem(0));
    } else if (!sel.isEmpty()) {   // selected once their folder has loaded (unless something else is picked by then)
        QPointer<LinkWindow> self(w);
        for (int ms : {150, 500, 1500})
            QTimer::singleShot(ms, w, [self, sel]() {
                if (self && self->left && self->left->pane() && self->picks().isEmpty())   // (closed: its browsers go first)
                    self->left->pane()->select_paths(sel);
            });
    }
    if (!bad.isEmpty())
        warn(w, tx("Edit Link"), tx("These links couldn't be read:\n\n%1").arg(bad.join('\n')));
    return w;
}

void LinkWindow::closeEvent(QCloseEvent *ev)
{
    bool unmade = std::any_of(pending.begin(), pending.end(), [](const links::Link &l) { return !l.unchanged(); });
    if (unmade && !running && !close_confirmed) {
        ev->ignore();
        QPointer<LinkWindow> self(this);
        ask_yes(this, tx("Close"), tx("The links in the list haven't been made or changed. Close the window anyway?"),
                [self]() {
                    if (!self)
                        return;
                    self->close_confirmed = true;
                    self->close();
                });
        return;
    }
    for (MainWindow *m : {left.data(), right.data()})   // (their searches and thumbnails stop)
        if (m)
            m->close();
    QWidget::closeEvent(ev);
}

QStringList LinkWindow::picks() const
{
    Pane *p = left ? left->pane() : nullptr;
    if (!p || p->is_overview() || p->is_trash())
        return {};
    QStringList out;
    for (const QString &x : p->selected_paths())
        out << abspath(x);
    return out;
}

QString LinkWindow::dest() const
{
    Pane *p = right ? right->pane() : nullptr;
    if (!p || p->is_overview() || p->is_trash() || p->is_virtual())
        return QString();
    return p->dir();
}

void LinkWindow::add(links::Link l)
{
    l.id = next_id++;
    pending << l;
}

QSet<QString> LinkWindow::taken(int except) const
{
    QSet<QString> out;
    for (const links::Link &l : pending)
        if (l.id != except)
            out << l.path();
    return out;
}

links::Link *LinkWindow::find(int id)
{
    for (links::Link &l : pending)
        if (l.id == id)
            return &l;
    return nullptr;
}

QList<int> LinkWindow::selected_ids() const
{
    QList<int> out;
    for (QTreeWidgetItem *it : list->selectedItems())
        out << it->data(0, ID_ROLE).toInt();
    return out;
}

void LinkWindow::add_picks()
{
    QString d = dest();
    QStringList ps = picks();
    if (d.isEmpty() || ps.isEmpty() || running)
        return;
    QString k = kind->currentData().toString();
    QSet<QString> t = taken();
    for (const QString &pk : ps) {
        links::Link l;
        l.folder = d;
        l.points_to = pk;
        l.kind = k;
        if (preview_names.contains(pk)) {
            l.name = preview_names.value(pk);
            l.renamed = true;
        } else {
            l.name = links::free_name(k, pk, d, t);
        }
        t << l.path();
        add(l);
    }
    preview_names.clear();
    if (Pane *p = left ? left->pane() : nullptr)
        p->view()->clearSelection();   // (their previews would show again beside them)
    refresh();
}

void LinkWindow::change_selected()
{
    QList<int> ids = selected_ids();
    QStringList ps = picks();
    QString d = dest();
    links::Link *l = ids.size() == 1 ? find(ids.first()) : nullptr;
    if (!l || ps.size() != 1 || d.isEmpty() || running)
        return;
    l->points_to = ps.first();
    l->folder = d;
    l->kind = kind->currentData().toString();
    if (!l->renamed)
        l->name = links::free_name(l->kind, l->points_to, l->folder, taken(l->id));
    refresh();
}

void LinkWindow::remove_selected()
{
    QList<int> ids = selected_ids();
    if (running)
        return;
    pending.erase(std::remove_if(pending.begin(), pending.end(), [&ids](const links::Link &l) { return ids.contains(l.id); }),
                  pending.end());
    refresh();
}

void LinkWindow::kind_changed()
{
    if (filling || running)
        return;
    QString k = kind->currentData().toString();
    for (int id : selected_ids())   // the links selected in the list become that kind
        if (links::Link *l = find(id)) {
            l->kind = k;
            if (!l->renamed)
                l->name = links::free_name(k, l->points_to, l->folder, taken(l->id));
        }
    refresh();
}

void LinkWindow::refresh()
{
    QList<int> sel = selected_ids();
    filling = true;
    list->clear();
    for (const links::Link &l : pending) {
        auto *it = new QTreeWidgetItem(list);
        it->setData(0, ID_ROLE, l.id);
        it->setText(0, l.name);
        it->setIcon(0, ghost_icon(l, false));
        it->setText(1, l.folder);
        QString to = l.points_to;
        struct stat st;
        if (l.kind == "hard" && lstat_(l.points_to, st) && st.st_nlink > 1)   // a file with more names already
            to = tx("%1  (%2)").arg(l.points_to, txn("%Ln names", qint64(st.st_nlink)));
        it->setText(2, to);
        it->setToolTip(2, l.kind == "sym" || l.kind == "rel" ? tx("The link will hold: %1").arg(l.text()) : l.points_to);
        it->setText(3, links::kind_label(l.kind));
        it->setText(4, change_text(l));
        QStringList texts;
        bool blocks = false;
        for (const links::Problem &p : links::check(l, pending)) {
            texts << p.text;
            blocks = blocks || p.blocks;
        }
        if (!texts.isEmpty()) {
            it->setText(5, texts.join("; "));
            it->setToolTip(5, texts.join('\n'));
            it->setIcon(5, blocks ? theme_icon({"dialog-error-symbolic", "dialog-error"})
                                  : theme_icon({"dialog-warning-symbolic", "dialog-warning"}));
        }
        it->setSelected(sel.contains(l.id));
    }
    filling = false;
    refresh_ghosts();
    update_buttons();
    update_pick_info();
}

void LinkWindow::refresh_ghosts()
{
    QString d = dest();
    filling = true;
    ghosts->clear();
    if (d.isEmpty()) {
        ghost_label->setText(tx("Open a folder on the right to make links in it."));
        filling = false;
        return;
    }
    ghost_label->setText(tx("The links to be made or changed in this folder (double-click a name to rename it):"));
    QList<int> sel = selected_ids();
    QFont italic = ghosts->font();
    italic.setItalic(true);
    QColor faint = palette().color(QPalette::PlaceholderText);
    for (const links::Link &l : pending) {
        if (l.folder != d)
            continue;
        auto *it = new QListWidgetItem(ghost_icon(l, false), l.name, ghosts);
        it->setData(ID_ROLE, l.id);
        it->setFlags(it->flags() | Qt::ItemIsEditable);
        it->setToolTip(tx("Points to: %1").arg(l.points_to));
        it->setFont(italic);
        it->setForeground(faint);
        it->setSelected(sel.contains(l.id));
    }
    if (sel.isEmpty()) {   // the picks, as they'd be added
        QSet<QString> t = taken();
        QString k = kind->currentData().toString();
        QStringList ps = picks().mid(0, MAX_PREVIEWS);
        faint.setAlphaF(0.6f);
        for (const QString &pk : ps) {
            links::Link l;
            l.folder = d;
            l.points_to = pk;
            l.kind = k;
            l.name = preview_names.contains(pk) ? preview_names.value(pk) : links::free_name(k, pk, d, t);
            t << l.path();
            auto *it = new QListWidgetItem(ghost_icon(l, true), l.name, ghosts);
            it->setData(ID_ROLE, 0);
            it->setData(TARGET_ROLE, pk);
            it->setFlags(it->flags() | Qt::ItemIsEditable);
            it->setToolTip(tx("Points to: %1\nNot in the list yet: Add to List keeps it.").arg(pk));
            it->setFont(italic);
            it->setForeground(faint);
        }
    }
    filling = false;
}

void LinkWindow::ghost_renamed(QListWidgetItem *it)
{
    if (filling || running)
        return;
    QString name = it->text();
    if (int id = it->data(ID_ROLE).toInt()) {
        if (links::Link *l = find(id)) {
            l->name = name;
            l->renamed = true;
        }
    } else {
        preview_names.insert(it->data(TARGET_ROLE).toString(), name);
    }
    QTimer::singleShot(0, this, [this]() { refresh(); });   // (not while the item's editor is still closing)
}

void LinkWindow::update_buttons()
{
    QStringList ps = picks();
    QString d = dest();
    QList<int> ids = selected_ids();
    add_btn->setEnabled(!running && !ps.isEmpty() && !d.isEmpty());
    change_btn->setEnabled(!running && ids.size() == 1 && ps.size() == 1 && !d.isEmpty());
    remove_btn->setEnabled(!running && !ids.isEmpty());
    ok_btn->setEnabled(!running && !pending.isEmpty());
    kind->setToolTip(ids.isEmpty() ? tx("The kind of the links added") : tx("The kind of the links selected in the list"));
}

void LinkWindow::update_pick_info()
{
    pick_info->clear();
    find_btn->hide();
    QList<int> ids = selected_ids();
    if (ids.size() == 1)
        if (links::Link *l = find(ids.first()); l && !lexists(l->points_to)) {
            pick_info->setText(tx("“%1” isn't there: this link is broken.").arg(l->points_to));
            return;
        }
    QStringList ps = picks();
    struct stat st;
    if (ps.size() == 1 && lstat_(ps.first(), st) && S_ISREG(st.st_mode) && st.st_nlink > 1) {
        pick_info->setText(txn("“%1” has %Ln names (hard links).", qint64(st.st_nlink)).arg(basename(ps.first())));
        find_btn->show();
    }
}

void LinkWindow::list_selected()
{
    if (filling)
        return;
    QList<int> ids = selected_ids();
    if (links::Link *l = ids.isEmpty() ? nullptr : find(ids.first())) {
        filling = true;
        kind->setCurrentIndex(kind->findData(l->kind));
        filling = false;
        if (ids.size() == 1) {
            if (lexists(l->points_to))
                show_in(left, dirname(l->points_to), {l->points_to});
            else   // broken: as near as it gets (update_pick_info says so)
                show_in(left, nearest_folder(l->points_to), {});
            show_in(right, l->folder, {});
        }
    }
    refresh_ghosts();
    update_buttons();
    update_pick_info();
}

void LinkWindow::show_in(MainWindow *side, const QString &folder, const QStringList &select)
{
    Pane *p = side ? side->pane() : nullptr;
    if (!p)
        return;
    if (p->dir() != folder)
        p->set_path(folder, true, select.value(0));
    else if (!select.isEmpty())
        p->select_paths(select);
    else
        p->view()->clearSelection();
}

void LinkWindow::find_names()
{
    QStringList ps = picks();
    if (ps.size() != 1)
        return;
    QString path = ps.first();
    QPointer<LinkWindow> self(this);
    fileops::queue_job(
        this, tx("Finding the other names of “%1”").arg(basename(path)), {path},
        [path](Task *t) {
            QStringList names = links::other_names(path, t);
            for (const QString &n : names)
                t->log(tx("Also named: %1").arg(n));
            return QVariant(names);
        },
        [self, path](const QVariant &res) {
            if (!self || !res.isValid())
                return;
            QStringList names = res.toStringList();
            QString more = names.size() > 50 ? "\n" + txn("… and %Ln more", names.size() - 50) : QString();
            inform(self, tx("Other Names"),
                   names.isEmpty() ? tx("No other name of “%1” was found on its drive (it may have some in folders "
                                        "you can't open).").arg(path)
                                   : tx("The other names of “%1”:\n\n%2").arg(path, names.mid(0, 50).join('\n') + more));
        });
}

void LinkWindow::commit()
{
    if (running)
        return;
    QStringList blocked, warns;
    for (const links::Link &l : pending)
        for (const links::Problem &p : links::check(l, pending))
            (p.blocks ? blocked : warns) << tx("%1: %2").arg(l.path(), p.text);
    if (!blocked.isEmpty()) {
        warn(this, tx("Link"), tx("Some links in the list can't be made as they are. Change them or remove them from "
                                  "the list first:\n\n%1").arg(blocked.mid(0, 20).join('\n')));
        return;
    }
    QList<links::Link> batch;
    QStringList made, changed, renamed;
    for (const links::Link &l : pending) {
        if (l.unchanged())
            continue;
        batch << l;
        if (!l.is_edit()) {
            bool sym = l.kind == "sym" || l.kind == "rel";
            made << tx("%1 → %2 (%3)").arg(l.path(), sym ? l.text() : l.points_to, links::kind_label(l.kind));
            continue;
        }
        if (l.text() != l.old_text)
            changed << tx("%1: %2 → %3").arg(l.existing, l.old_text, l.text());
        if (l.path() != l.existing)
            renamed << tx("%1 → %2").arg(l.existing, l.path());
    }
    if (batch.isEmpty()) {
        inform(this, tx("Link"), tx("Nothing in the list changes anything yet."));
        return;
    }
    QStringList lines;
    auto section = [&lines](const QString &head, const QStringList &items) {
        if (items.isEmpty())
            return;
        lines << head;
        for (const QString &i : items)
            lines << "    " + i;
        lines << QString();
    };
    section(txn("%Ln new link(s):", made.size()), made);
    section(txn("%Ln link(s) to point elsewhere:", changed.size()), changed);
    section(txn("%Ln link(s) to rename or move:", renamed.size()), renamed);
    section(tx("To note:"), warns);
    auto *box = new QMessageBox(QMessageBox::Question, tx("Make and Change Links"),
                                tx("These links will be made or changed. Undo (Ctrl+Z) puts them all back as they were."),
                                QMessageBox::NoButton, this);
    const int SHOWN = 40;
    box->setInformativeText(lines.mid(0, SHOWN).join('\n') + (lines.size() > SHOWN ? "\n…" : ""));
    if (lines.size() > SHOWN)
        box->setDetailedText(lines.join('\n'));
    QAbstractButton *go = box->addButton(QMessageBox::Ok);
    box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(qobject_cast<QPushButton *>(go));
    QPointer<LinkWindow> self(this);
    ask_box(box, [self, go, batch](QAbstractButton *b) {
        if (self && b == go)
            self->run(batch);
    });
}

void LinkWindow::run(const QList<links::Link> &batch)
{
    if (running)
        return;
    // checked again: a name may have been taken, or a link changed, while the summary was open
    QStringList blocked;
    for (const links::Link &l : batch)
        for (const links::Problem &p : links::check(l, pending))
            if (p.blocks)
                blocked << tx("%1: %2").arg(l.path(), p.text);
    if (!blocked.isEmpty()) {
        refresh();
        warn(this, tx("Link"), tx("Something changed since the list was checked, so nothing was made or changed:\n\n%1")
                                   .arg(blocked.mid(0, 20).join('\n')));
        return;
    }
    struct Outcome {
        links::UndoItems undo;
        QList<int> done, denied;   // denied: new links this user may not make there (an administrator may)
        QStringList failed;
    };
    auto out = std::make_shared<Outcome>();
    QStringList paths, steps;
    bool all_new = true;
    for (const links::Link &l : batch) {
        paths << l.folder;
        if (l.kind == "hard")
            paths << l.points_to;
        steps << l.path();
        all_new = all_new && !l.is_edit();
    }
    paths.removeDuplicates();
    running = true;
    update_buttons();
    QPointer<LinkWindow> self(this);
    QPointer<MainWindow> w = win;
    QString title = all_new ? txn("Making %Ln link(s)", batch.size()) : txn("Making and changing %Ln link(s)", batch.size());
    fileops::queue_job(
        win ? static_cast<QWidget *>(win.data()) : this, title, paths,
        [batch, out](Task *t) {
            for (int i = 0; i < batch.size(); ++i) {
                t->check();   // a cancel between links: the ones done so far stay, and are undone with the rest
                const links::Link &l = batch[i];
                t->step = i;
                t->report(i, batch.size(), l.name);
                try {
                    links::commit(l, &out->undo);
                    if (!l.is_edit())
                        t->log(tx("Made: %1 → %2").arg(l.path(), l.kind == "sym" || l.kind == "rel" ? l.text() : l.points_to));
                    else if (l.path() != l.existing && l.text() != l.old_text)
                        t->log(tx("Changed and renamed: %1 → %2 (now → %3)").arg(l.existing, l.path(), l.text()));
                    else if (l.path() != l.existing)
                        t->log(tx("Renamed: %1 → %2").arg(l.existing, l.path()));
                    else
                        t->log(tx("Changed: %1 → %2 (was → %3)").arg(l.path(), l.text(), l.old_text));
                    out->done << l.id;
                } catch (const OSError &e) {
                    if (e.permission() && !l.is_edit())
                        out->denied << l.id;
                    else
                        out->failed << l.path() + ": " + e.message();
                    t->log(tx("Couldn't make or change %1: %2").arg(l.path(), e.message()));
                    t->had_errors = true;
                } catch (const Error &e) {
                    out->failed << l.path() + ": " + e.message();
                    t->log(tx("Couldn't make or change %1: %2").arg(l.path(), e.message()));
                    t->had_errors = true;
                }
            }
            return QVariant(true);
        },
        [self, w, out, batch, all_new](const QVariant &) {
            // recorded also when this window has closed meanwhile: what was made can be undone
            if (!out->undo.isEmpty())
                undo::record("links", all_new ? TX_NOOP("Create Link") : TX_NOOP("Change Links"), out->undo);
            if (!self)
                return;
            self->running = false;
            QList<int> done = out->done;
            self->pending.erase(std::remove_if(self->pending.begin(), self->pending.end(),
                                               [&done](const links::Link &l) { return done.contains(l.id); }),
                                self->pending.end());
            self->refresh();
            if (!out->failed.isEmpty())
                warn(self, tx("Link"), tx("Some links couldn't be made or changed, and are still in the list:\n\n%1")
                                           .arg(out->failed.mid(0, 20).join('\n')));
            QList<QVariantMap> plans;   // the ones this user may not make: as an administrator, if they like
            for (const links::Link &l : batch) {
                if (!out->denied.contains(l.id))
                    continue;
                if (l.kind == "sym" || l.kind == "rel")
                    plans << QVariantMap{{"op", "symlink"}, {"target", l.text()}, {"link", l.path()}, {"id", l.id}};
                else if (l.kind == "hard")
                    plans << QVariantMap{{"op", "hardlink"}, {"target", l.points_to}, {"link", l.path()}, {"id", l.id}};
                else
                    try {
                        plans << QVariantMap{{"op", "write"}, {"path", l.path()}, {"text", fileops::desktop_entry(l.points_to)},
                                             {"mode", 0755}, {"id", l.id}};
                    } catch (const Error &) {
                    }
            }
            if (!plans.isEmpty()) {
                auto work = [plans](Task *task) {
                    QStringList errs;
                    for (const QVariantMap &pl : plans) {
                        QVariantMap args = pl;
                        QString op = args.take("op").toString();
                        args.remove("id");
                        try {
                            admin::session().call(task, op, args);
                        } catch (const admin::AdminError &e) {
                            errs << basename(fileops::plan_path(pl)) + ": " + e.message();
                        }
                    }
                    return errs;
                };
                auto finish = [self, plans](bool) {
                    if (!self)
                        return;
                    QList<int> made;   // (not undoable: made by the administrator)
                    for (const QVariantMap &pl : plans)
                        if (lexists(fileops::plan_path(pl)))
                            made << pl.value("id").toInt();
                    self->pending.erase(std::remove_if(self->pending.begin(), self->pending.end(),
                                                       [&made](const links::Link &l) { return made.contains(l.id); }),
                                        self->pending.end());
                    self->refresh();
                };
                admin::retry_as_admin(self, tx("Link"),
                                      txn("You don't have permission to make %Ln of these links.", plans.size()), work,
                                      finish);
                return;
            }
            if (self->pending.isEmpty()) {
                if (w)
                    w->statusBar()->showMessage(txn("Made or changed %Ln link(s)", out->done.size()), 4000);
                self->close_confirmed = true;
                self->close();
            }
        },
        true, steps, false, {}, true);
}
