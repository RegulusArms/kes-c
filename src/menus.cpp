#include "menus.h"

#include "prefs.h"
#include "util.h"
#include "windows.h"

#include <QAction>
#include <QComboBox>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

using namespace util;

namespace menus {

// the built-in submenus whose entries can be moved one by one (others, like Open With or Send To, move as a whole)
static const QSet<QString> CONTAINERS = {"links", "copy_path", "image", "previews", "viewer_view", "viewer_edit"};

QList<Kind> kinds() { return {FOLDER, FILE, ARCHIVE, SEVERAL, BACKGROUND, VIEWER}; }

QString kind_key(Kind k)
{
    switch (k) {
    case FOLDER: return "folder";
    case FILE: return "file";
    case ARCHIVE: return "archive";
    case SEVERAL: return "several";
    case BACKGROUND: return "background";
    case VIEWER: return "viewer";
    }
    return QString();
}

QString kind_label(Kind k)
{
    switch (k) {
    case FOLDER: return "A folder";
    case FILE: return "A file";
    case ARCHIVE: return "An archive";
    case SEVERAL: return "Several items";
    case BACKGROUND: return "Empty space in a folder";
    case VIEWER: return "The image viewer";
    }
    return QString();
}

// ---------------------------------------------------------------- the default layouts

static Entry item(const QString &id, const QString &label, const QString &note = QString())
{
    Entry e;
    e.id = id, e.label = label, e.note = note;
    return e;
}

static Entry sep()
{
    Entry e;
    e.type = Entry::SEP;
    return e;
}

static Entry submenu(const QString &id, const QString &title, const QList<Entry> &children, const QString &note = QString())
{
    Entry e;
    e.type = Entry::MENU;
    e.id = id, e.label = title, e.note = note, e.children = children;
    return e;
}

static QList<Entry> previews_menu()
{
    return {submenu("previews", "Previews",
                    {item("gen_previews_rec", "Generate Previews Recursively"),
                     item("gen_folder_previews", "Generate Folder Previews"),
                     item("gen_folder_previews_rec", "Generate Folder Previews Recursively"), sep(),
                     item("regen_previews", "Regenerate Previews"),
                     item("regen_previews_rec", "Regenerate Previews Recursively"),
                     item("regen_folder_previews", "Regenerate Folder Previews"),
                     item("regen_folder_previews_rec", "Regenerate Folder Previews Recursively")})};
}

QList<Entry> defaults(Kind k)
{
    QList<Entry> l;
    if (k == VIEWER) {   // ImageViewer::build_menu
        l << item("next", "Next") << item("previous", "Previous") << sep()
          << submenu("viewer_view", "View",
                     {item("fullscreen", "Fullscreen"), item("slideshow", "Start / Stop Slideshow"),
                      item("fit", "Fit to Window"), item("actual_size", "Actual Size"),
                      item("keep_zoom", "Keep Zoom Between Images"), sep(), item("filmstrip", "Filmstrip"),
                      item("info_panel", "Info Panel"), item("info_line", "Info Line"), sep(),
                      item("compare", "Compare With the Next Image"),
                      item("compare_ab", "One at a Time (A/B)", "while comparing"),
                      item("compare_slider", "Over Each Other, With a Slider", "while comparing"),
                      item("compare_switch", "Show A / Show B", "comparing one at a time")})
          << submenu("viewer_edit", "Rotate and Edit",
                     {item("rotate_right", "Rotate Right"), item("rotate_left", "Rotate Left"),
                      item("flip", "Flip Horizontally"), item("save_changes", "Save Rotation / Changes…"),
                      item("undo_edits", "Undo the Edits", "with crops or resizes not saved"), sep(),
                      item("crop", "Crop…"), item("resize", "Resize…"), item("straighten", "Straighten")})
          << sep() << item("star", "Star / Unstar") << item("pin", "Pin / Unpin")
          << item("viewer_copy_to", "Copy To ▸") << item("viewer_move_to", "Move To ▸")
          << item("show_in_folder", "Show in Folder") << sep()
          << item("open_default", "Open With Default Application") << item("open_with", "Open With…")
          << item("copy_image", "Copy Image") << item("copy_full_path", "Copy Path") << item("wallpaper", "Set as Wallpaper")
          << item("uwp_add", "Add to Selected UWP Monitor", "while UWP's editor is open") << sep()
          << item("properties", "Properties") << item("trash", "Move to Trash") << item("undo", "Undo");
        return l;
    }
    if (k == BACKGROUND) {
        l << item("new_folder", "New Folder…") << item("new_document", "New Document ▸") << sep() << item("paste", "Paste")
          << item("paste_link", "Paste as Link") << item("select_all", "Select All") << sep()
          << item("sort_by", "Sort By ▸") << item("show_hidden", "Show Hidden Files")
          << item("collapse_groups", "Collapse All Groups", "when grouped by date")
          << item("expand_groups", "Expand All Groups", "when grouped by date") << sep()
          << item("open_terminal", "Open in Terminal") << item("bookmark_folder", "Bookmark This Folder")
          << previews_menu() << item("empty_trash", "Empty Trash", "in the Trash")
          << item("empty_trash_bleachbit", "Empty Trash with BleachBit…", "in the Trash, with BleachBit installed")
          << item("scripts", "Scripts ▸", "when you have scripts") << sep() << item("properties", "Properties");
        return l;
    }
    bool folder = k == FOLDER, file = k == FILE, arc = k == ARCHIVE, several = k == SEVERAL;
    l << item("open", "Open");
    if (folder || several) {
        QString n = several ? "when all are folders" : QString();
        l << item("open_new_tab", "Open in New Tab", n) << item("open_new_window", "Open in New Window", n);
    }
    if (folder)
        l << item("open_in_editor", "Open in (a code editor)", "when one is installed") << item("open_with", "Open With ▸");
    if (file || several)
        l << item("view_image", several ? "View Images" : "View Image", "for images");
    l << item("show_in_folder", "Show in Folder", "in search results")
      << item("collapse_group", "Collapse This Group", "when grouped by date");
    if (file || arc)
        l << item("open_with", "Open With ▸");
    l << sep() << item("cut", "Cut") << item("copy", "Copy");
    if (folder)
        l << item("paste_into", "Paste Into Folder");
    l << item("move_to", "Move To…") << item("copy_to", "Copy To…") << item("duplicate", "Duplicate")
      << item("new_folder_with", "New Folder with Selection…")
      << item("rename", "Rename…")
      << submenu("copy_path", "Copy Path / Name",
                 {item("copy_full_path", "Copy Full Path"), item("copy_name", "Copy Name"), item("copy_uri", "Copy URI")})
      << item("star", "Star / Unstar") << item("pin", "Pin / Unpin");
    QList<Entry> links = {item("link_sym_here", "Create Symbolic Link Here"),
                          item("link_rel_here", "Create Relative Symbolic Link Here")};
    if (!folder)
        links << item("link_hard_here", "Create Hard Link Here", "for files");
    links << item("link_sym_in", "Create Symbolic Link In…") << sep() << item("link_desktop", "Send Link to Desktop")
          << item("desktop_shortcut_here", "Create Desktop Shortcut (.desktop) Here")
          << item("desktop_shortcut_desktop", "Send Shortcut (.desktop) to Desktop") << sep()
          << item("link_target", "Open Link Target Location", "for symbolic links");
    if (folder)
        links << sep() << item("add_bookmark", "Add to Bookmarks");
    l << submenu("links", "Links & Shortcuts", links) << sep();
    if (arc || several) {
        QString n = several ? "a split set selected whole" : QString();
        l << item("extract_here", "Extract Here", n) << item("extract_to", "Extract To…", n)
          << item("open_as_archive", "Open as Archive", several ? n : "when its program can list it")
          << item("test_archive", "Test Archive", n);
    }
    if (several)
        l << item("extract_all_here", "Extract All Here", "when all are archives")
          << item("extract_all_to", "Extract All To…", "when all are archives")
          << item("test_archives", "Test Archives", "when all are archives");
    l << item("compress_quick", "Compress to “name.ext”", "once you've compressed something")
      << item("compress", "Compress…") << item("checksum_create", "Create Checksum File…");
    if (folder || several)
        l << item("find_duplicates", "Find Duplicates…", several ? "when all are folders" : QString());
    if (folder)
        l << item("compare_with", "Compare With…") << item("sync_to", "Sync To…");
    if (file)
        l << item("verify_signature", "Verify Signature", "for signatures (.sig, .asc, .gpg, .sign)");
    if (file)
        l << item("uwp_add", "Add to Selected UWP Monitor", "images and videos, while UWP's editor is open")
          << item("wallpaper", "Set as Wallpaper", "for videos, with UWP")
          << submenu("image", "Image",
                     {item("image_wallpaper", "Set as Wallpaper"), item("image_cover", "Use as Folder Cover"),
                      item("image_copy", "Copy Image to Clipboard")},
                     "for images");
    if (several)
        l << item("compare_images", "Compare in Viewer", "for two images");
    if (file || several) {
        QString all = several ? "when all are " : "for ";
        l << submenu("rotate", "Rotate",
                     {item("rotate_cw", "Rotate 90° Clockwise"), item("rotate_180", "Rotate 180°"),
                      item("rotate_ccw", "Rotate 90° Counter-clockwise"), sep(), item("flip_horizontal", "Flip Horizontally"),
                      item("flip_vertical", "Flip Vertically")},
                     all + "images")
          << item("convert_image", "Convert Image…", all + "images")
          << item("convert_video", "Convert Video…", all + "videos, with ffmpeg installed")
          << item("extract_audio", "Extract Audio…", all + "videos, with ffmpeg installed");
    }
    if (file || several || folder)
        l << item("convert_audio", "Convert Audio…",
                  folder ? QString("with ffmpeg installed") : several ? QString("when all are audio files or all are folders, with ffmpeg installed") : QString("for audio files, with ffmpeg installed"));
    if (file || several)
        l << item("wipe_metadata", "Wipe Metadata…", several ? "when images are selected" : "for images");
    if (folder || several) {
        QString n = several ? "when all are folders" : QString();
        l << item("folder_colour", "Folder Colour ▸", n) << item("show_image_previews", "Show Image Previews", n);
    }
    if (folder)
        l << item("reset_cover", "Reset Folder Cover", "when it has a cover");
    if (!folder)
        l << item("send_to", "Send To ▸", "with apps to send to, not for folders");
    if (folder)
        l << item("network_sharing", "Network Sharing…", "with Samba installed");
    l << item("scripts", "Scripts ▸", "when you have scripts");
    if (folder)
        l << item("regen_preview", "Regenerate Preview") << previews_menu() << item("open_terminal", "Open in Terminal");
    l << sep() << item("trash", "Move to Trash") << item("delete", "Delete Permanently…")
      << item("shred", "Shred with BleachBit…", "with BleachBit installed") << sep();
    if (folder || several)
        l << item("calculate_size", "Calculate Folder Size", several ? "when all are folders" : QString());
    if (file || arc)
        l << item("open_metadata", "Open Metadata", "for images; with exiftool, videos, audio and documents too");
    l << item("properties", "Properties");
    return l;
}

// ---------------------------------------------------------------- saving and reading

static QString key_for(Kind k) { return "menus/" + kind_key(k); }

static QJsonArray to_json(const QList<Entry> &items)
{
    QJsonArray a;
    for (const Entry &e : items) {
        if (e.type == Entry::SEP)
            a << QJsonObject{{"sep", true}};
        else if (e.type == Entry::MENU)
            a << QJsonObject{{"menu", e.id}, {"title", e.label}, {"items", to_json(e.children)}};
        else if (e.off)
            a << QJsonObject{{"id", e.id}, {"off", true}};
        else
            a << QJsonObject{{"id", e.id}};
    }
    return a;
}

static void ids_in(const QList<Entry> &items, QStringList &out)
{
    for (const Entry &e : items) {
        if (e.type != Entry::SEP && !e.id.isEmpty() && !out.contains(e.id))
            out << e.id;
        ids_in(e.children, out);
    }
}

static const Entry *find_entry(const QList<Entry> &items, const QString &id)
{
    for (const Entry &e : items) {
        if (e.type != Entry::SEP && e.id == id)
            return &e;
        if (const Entry *c = find_entry(e.children, id))
            return c;
    }
    return nullptr;
}

static QList<Entry> from_json(const QJsonArray &a, const QList<Entry> &defs)
{
    QList<Entry> out;
    for (const QJsonValue &v : a) {
        QJsonObject o = v.toObject();
        if (o.value("sep").toBool()) {
            out << sep();
        } else if (o.contains("menu")) {
            Entry m = submenu(o.value("menu").toString(), o.value("title").toString(), from_json(o.value("items").toArray(), defs));
            out << m;
        } else if (o.contains("id")) {
            QString id = o.value("id").toString();
            const Entry *d = find_entry(defs, id);
            if (!d || d->type != Entry::ITEM)
                continue;   // gone in this version
            Entry e = *d;
            e.off = o.value("off").toBool();
            out << e;
        }
    }
    return out;
}

bool customised(Kind k) { return settings().contains(key_for(k)); }

// Entries of the defaults the saved layout has never seen (added in a later version) go where they are by default:
// right after the entry they follow there (in the same submenu as it), or first if none of those is in the layout.
static void add_unseen(QList<Entry> &items, const QList<Entry> &defs, const QStringList &known)
{
    QStringList have;
    ids_in(items, have);
    // the defaults flattened in order (a built-in submenu: itself, then its entries)
    QList<const Entry *> flat;
    std::function<void(const QList<Entry> &)> walk = [&](const QList<Entry> &l) {
        for (const Entry &e : l) {
            if (e.type == Entry::SEP)
                continue;
            flat << &e;
            walk(e.children);
        }
    };
    walk(defs);
    // where an id sits in `items`: its list and index
    std::function<bool(QList<Entry> &, const QString &, QList<Entry> **, int *)> locate =
        [&](QList<Entry> &l, const QString &id, QList<Entry> **list, int *at) -> bool {
        for (int i = 0; i < l.size(); ++i) {
            if (l[i].type != Entry::SEP && l[i].id == id) {
                *list = &l, *at = i;
                return true;
            }
            if (locate(l[i].children, id, list, at))
                return true;
        }
        return false;
    };
    for (int i = 0; i < flat.size(); ++i) {
        const Entry *e = flat[i];
        if (known.contains(e->id) || have.contains(e->id))
            continue;
        QList<Entry> *list = &items;
        int at = -1;
        for (int j = i - 1; j >= 0; --j)
            if (locate(items, flat[j]->id, &list, &at))
                break;
        Entry copy = *e;
        if (copy.type == Entry::MENU) {   // a whole new submenu: only its unseen entries
            QList<Entry> kids;
            for (const Entry &c : copy.children)
                if (c.type == Entry::SEP || (!known.contains(c.id) && !have.contains(c.id)))
                    kids << c;
            copy.children = kids;
        }
        list->insert(at + 1, copy);
        ids_in(QList<Entry>{copy}, have);
    }
}

QList<Entry> layout(Kind k)
{
    QList<Entry> defs = defaults(k);
    if (!customised(k))
        return defs;
    QJsonObject o = QJsonDocument::fromJson(settings().value(key_for(k)).toString().toUtf8()).object();
    QList<Entry> items = from_json(o.value("items").toArray(), defs);
    QStringList known;
    for (const QJsonValue &v : o.value("known").toArray())
        known << v.toString();
    add_unseen(items, defs, known);
    return items;
}

void save(Kind k, const QList<Entry> &items)
{
    QStringList known;
    ids_in(defaults(k), known);
    ids_in(items, known);
    QJsonObject o{{"items", to_json(items)}, {"known", QJsonArray::fromStringList(known)}};
    prefs::set(key_for(k), QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

void reset(Kind k) { prefs::reset({key_for(k)}); }

void reset_all()
{
    QStringList keys;
    for (Kind k : kinds())
        keys << key_for(k);
    prefs::reset(keys);
}

// ---------------------------------------------------------------- arranging a built menu

void tag(QAction *a, const QString &id)
{
    if (a)
        a->setProperty("menu_id", id);
}

QString id_of(const QAction *a) { return a ? a->property("menu_id").toString() : QString(); }

void arrange(QMenu *menu, Kind k)
{
    if (!customised(k))
        return;
    QList<Entry> lay = layout(k);
    QHash<QString, QList<QAction *>> present;   // an entry can be several actions (one per code editor…)
    QStringList order;                          // the ids as the default menu has them
    QHash<QString, QMenu *> containers;
    for (QAction *a : menu->actions()) {
        QString id = id_of(a);
        if (a->menu() && CONTAINERS.contains(id)) {
            containers[id] = a->menu();
            for (QAction *c : a->menu()->actions())
                if (!c->isSeparator() && !id_of(c).isEmpty()) {
                    present[id_of(c)] << c;
                    if (!order.contains(id_of(c)))
                        order << id_of(c);
                }
        } else if (!a->isSeparator() && !id.isEmpty()) {
            present[id] << a;
            if (!order.contains(id))
                order << id;
        }
    }
    for (QAction *a : menu->actions())   // (they stay the menu's: deleted with it)
        menu->removeAction(a);
    for (QMenu *c : containers)
        for (QAction *a : c->actions())
            c->removeAction(a);
    // entries the layout doesn't know at all (not even hidden) follow the one before them in the default menu
    QStringList in_layout;
    ids_in(lay, in_layout);
    QHash<QString, QStringList> followers;
    QStringList first;
    for (int i = 0; i < order.size(); ++i) {
        if (in_layout.contains(order[i]))
            continue;
        QString pred;
        for (int j = i - 1; j >= 0 && pred.isEmpty(); --j)
            if (in_layout.contains(order[j]) || followers.contains(order[j]))
                pred = order[j];
        if (pred.isEmpty())
            first << order[i];
        else
            followers[pred] << order[i];
    }
    std::function<void(QMenu *, const QList<Entry> &, bool)> place = [&](QMenu *target, const QList<Entry> &items, bool top) {
        bool pending = false;
        auto put = [&](const QString &id, auto &self) -> void {
            if (present.contains(id)) {
                if (pending && !target->isEmpty())
                    target->addSeparator();
                pending = false;
                for (QAction *a : present[id])
                    target->addAction(a);
            }
            for (const QString &f : followers.value(id))
                self(f, self);
        };
        if (top)
            for (const QString &f : first)
                put(f, put);
        for (const Entry &e : items) {
            if (e.type == Entry::SEP) {
                pending = true;
            } else if (e.type == Entry::MENU) {
                auto *sub = new QMenu(QString(e.label).replace('&', "&&"), target);   // (a lone & would underline the next letter)
                if (QMenu *c = containers.value(e.id))
                    sub->setIcon(c->icon());
                place(sub, e.children, false);
                if (sub->isEmpty()) {
                    delete sub;
                    continue;
                }
                if (pending && !target->isEmpty())
                    target->addSeparator();
                pending = false;
                tag(target->addMenu(sub), e.id);
            } else if (!e.off) {
                put(e.id, put);
            } else {
                for (const QString &f : followers.value(e.id))   // after a hidden entry: still in that place
                    put(f, put);
            }
        }
    };
    place(menu, lay, true);
}

// ---------------------------------------------------------------- the editor

enum { TypeRole = Qt::UserRole + 1, IdRole };

// Dragging rearranges; a submenu can't go inside another, and entries can only go inside submenus
class MenuTree : public QTreeWidget {
public:
    std::function<void()> changed;

protected:
    void dropEvent(QDropEvent *ev) override
    {
        QTreeWidget::dropEvent(ev);
        for (int i = topLevelItemCount() - 1; i >= 0; --i) {
            QTreeWidgetItem *top = topLevelItem(i);
            for (int j = top->childCount() - 1; j >= 0; --j)
                if (top->child(j)->data(0, TypeRole).toInt() == Entry::MENU)
                    insertTopLevelItem(i + 1, top->takeChild(j));
        }
        if (changed)
            changed();
    }
};

Editor::Editor(QWidget *parent) : QWidget(parent)
{
    setObjectName("menu_editor");
    auto *lay = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel("The right-click menu for:"));
    kind_box = new QComboBox;
    kind_box->setObjectName("menu_kind");
    for (Kind k : kinds())
        kind_box->addItem(kind_label(k), int(k));
    top->addWidget(kind_box, 1);
    lay->addLayout(top);
    auto *help = new QLabel("Untick an entry to hide it. Drag entries to reorder them or to move them into or out of a "
                            "submenu. An entry only appears when it applies (Extract for archives, a tool that's "
                            "installed…), wherever it's placed.");
    help->setWordWrap(true);
    lay->addWidget(help);
    auto *mid = new QHBoxLayout;
    auto *t = new MenuTree;
    tree = t;
    tree->setObjectName("menu_tree");
    tree->setHeaderLabels({"Entry", "Appears"});
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->setDragDropMode(QAbstractItemView::InternalMove);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    t->changed = [this]() { store(); };
    mid->addWidget(tree, 1);
    auto *side = new QVBoxLayout;
    auto button = [&](const QString &text, std::function<void()> fn) {
        auto *b = new QPushButton(text);
        connect(b, &QPushButton::clicked, this, fn);
        side->addWidget(b);
        return b;
    };
    button("Move Up", [this]() { move_selected(-1); });
    button("Move Down", [this]() { move_selected(1); });
    button("Add Separator", [this]() { add_separator(); });
    button("New Submenu…", [this]() {
        ask_text(this, "New Submenu", "Name:", "", [this](const QString &name) {
            if (!name.trimmed().isEmpty())
                add_submenu(name.trimmed());
        });
    });
    button("Rename Submenu…", [this]() {
        QTreeWidgetItem *it = tree->currentItem();
        if (it && it->data(0, TypeRole).toInt() == Entry::MENU)
            tree->editItem(it, 0);
    });
    button("Remove", [this]() { remove_selected(); });
    side->addStretch(1);
    button("Reset to Default", [this]() {
        ask_yes(this, "Reset to Default", QString("Put the menu for “%1” back as it was?").arg(kind_box->currentText()),
                [this]() {
                    reset(kind());
                    reload();
                });
    });
    mid->addLayout(side);
    lay->addLayout(mid, 1);
    connect(kind_box, &QComboBox::currentIndexChanged, this, &Editor::reload);
    connect(tree, &QTreeWidget::itemChanged, this, [this]() { store(); });
    reload();
}

Kind Editor::kind() const { return Kind(kind_box->currentData().toInt()); }

QTreeWidgetItem *Editor::item_for(const Entry &e)
{
    auto *it = new QTreeWidgetItem;
    it->setData(0, TypeRole, int(e.type));
    it->setData(0, IdRole, e.id);
    Qt::ItemFlags base = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (e.type == Entry::SEP) {
        it->setText(0, "──────── separator");
        it->setFlags(base);
    } else if (e.type == Entry::MENU) {
        it->setText(0, e.label);
        QFont f = it->font(0);
        f.setBold(true);
        it->setFont(0, f);
        it->setText(1, e.note.isEmpty() ? QString("submenu") : "submenu, " + e.note);
        it->setFlags(base | Qt::ItemIsDropEnabled | Qt::ItemIsEditable);
        for (const Entry &c : e.children)
            it->addChild(item_for(c));
    } else {
        it->setText(0, e.label);
        it->setText(1, e.note);
        it->setFlags(base | Qt::ItemIsUserCheckable);
        it->setCheckState(0, e.off ? Qt::Unchecked : Qt::Checked);
    }
    return it;
}

void Editor::fill(const QList<Entry> &items)
{
    loading = true;
    tree->clear();
    for (const Entry &e : items)
        tree->addTopLevelItem(item_for(e));
    tree->expandAll();
    loading = false;
}

void Editor::reload()
{
    if (!storing)
        fill(menus::layout(kind()));
}

static QList<Entry> read_tree(QTreeWidgetItem *parent, Kind k)
{
    QList<Entry> defs = defaults(k);
    QList<Entry> out;
    for (int i = 0; i < parent->childCount(); ++i) {
        QTreeWidgetItem *it = parent->child(i);
        int type = it->data(0, TypeRole).toInt();
        if (type == Entry::SEP) {
            out << sep();
        } else if (type == Entry::MENU) {
            out << submenu(it->data(0, IdRole).toString(), it->text(0), read_tree(it, k));
        } else {
            QString id = it->data(0, IdRole).toString();
            const Entry *d = find_entry(defs, id);
            Entry e = d ? *d : item(id, it->text(0));
            e.off = it->checkState(0) != Qt::Checked;
            out << e;
        }
    }
    return out;
}

void Editor::store()
{
    if (loading)
        return;
    storing = true;
    save(kind(), read_tree(tree->invisibleRootItem(), kind()));
    storing = false;
}

void Editor::add_separator()
{
    QTreeWidgetItem *cur = tree->currentItem();
    QTreeWidgetItem *parent = cur && cur->parent() ? cur->parent() : tree->invisibleRootItem();
    int at = cur ? parent->indexOfChild(cur) + 1 : parent->childCount();
    loading = true;
    auto *it = item_for(sep());
    parent->insertChild(at, it);
    tree->setCurrentItem(it);
    loading = false;
    store();
}

void Editor::add_submenu(const QString &title)
{
    QTreeWidgetItem *cur = tree->currentItem();
    while (cur && cur->parent())
        cur = cur->parent();
    QTreeWidgetItem *root = tree->invisibleRootItem();
    int at = cur ? root->indexOfChild(cur) + 1 : root->childCount();
    int n = 1;
    QStringList used;
    for (int i = 0; i < root->childCount(); ++i)
        used << root->child(i)->data(0, IdRole).toString();
    while (used.contains(QString("custom%1").arg(n)))
        ++n;
    loading = true;
    auto *it = item_for(submenu(QString("custom%1").arg(n), title, {}));
    root->insertChild(at, it);
    tree->setCurrentItem(it);
    loading = false;
    store();
}

void Editor::move_selected(int by)
{
    QTreeWidgetItem *it = tree->currentItem();
    if (!it)
        return;
    QTreeWidgetItem *root = tree->invisibleRootItem();
    QTreeWidgetItem *parent = it->parent() ? it->parent() : root;
    int i = parent->indexOfChild(it);
    bool is_menu = it->data(0, TypeRole).toInt() == Entry::MENU;
    loading = true;
    bool expanded = it->isExpanded();
    if (parent != root && ((by < 0 && i == 0) || (by > 0 && i == parent->childCount() - 1))) {
        // out of its submenu, just before or after it
        parent->takeChild(i);
        int at = root->indexOfChild(parent) + (by > 0 ? 1 : 0);
        root->insertChild(at, it);
    } else {
        QTreeWidgetItem *next = parent->child(i + by);
        if (!next) {
            loading = false;
            return;
        }
        parent->takeChild(i);
        if (!is_menu && parent == root && next->data(0, TypeRole).toInt() == Entry::MENU) {
            // into the submenu it meets: at its end coming up, at its start going down
            next->insertChild(by < 0 ? next->childCount() : 0, it);
            next->setExpanded(true);
        } else {
            parent->insertChild(i + by, it);
        }
    }
    it->setExpanded(expanded);
    tree->setCurrentItem(it);
    loading = false;
    store();
}

void Editor::remove_selected()
{
    QTreeWidgetItem *it = tree->currentItem();
    if (!it)
        return;
    int type = it->data(0, TypeRole).toInt();
    if (type == Entry::ITEM)
        return;   // hidden by unticking it, so it keeps its place
    QTreeWidgetItem *parent = it->parent() ? it->parent() : tree->invisibleRootItem();
    int at = parent->indexOfChild(it);
    loading = true;
    parent->takeChild(at);
    if (type == Entry::MENU)
        while (it->childCount())   // its entries stay, where it was
            parent->insertChild(at++, it->takeChild(0));
    delete it;
    loading = false;
    store();
}

}  // namespace menus
