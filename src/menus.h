// The right-click menus, as the user lays them out (Preferences → Menus).
//
// Every entry the item menus can have carries an id (tag(); MainWindow::build_menu builds the default menu with them).
// For each kind of click (a folder, a file, an archive, several items, the empty space in a folder) the user can hide
// entries, reorder them, add separators, move entries into or out of submenus (Links & Shortcuts and the others) and
// make submenus of their own. arrange() then rebuilds the menu from that layout: an entry that doesn't apply this
// time (Extract for a folder, a tool that isn't installed, the admin entries) still doesn't appear, wherever it was
// put. A layout remembers every id it has seen ("known"), so an entry added in a later version shows up in its default
// place, after the entry it follows by default.
//
// Saved as JSON in the settings, "menus/<kind>": {"items": [...], "known": [ids]}, an item being {"id": id} (with
// "off": true when hidden), {"sep": true}, or {"menu": id, "title": text, "items": [...]} (one level of submenus).
// No saved layout: the default menu, as built.
#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class QAction;
class QComboBox;
class QMenu;
class QTreeWidget;
class QTreeWidgetItem;

namespace menus {

enum Kind { FOLDER, FILE, ARCHIVE, SEVERAL, BACKGROUND };
QList<Kind> kinds();
QString kind_key(Kind k);     // folder, file, archive, several, background
QString kind_label(Kind k);   // "A folder", "Empty space in a folder"…

struct Entry {
    enum Type { ITEM, SEP, MENU } type = ITEM;
    QString id;      // ITEM: the entry; MENU: a built-in submenu's id (links, copy_path, image, previews) or "custom…"
    QString label;   // ITEM: as shown in the editor; MENU: its title
    QString note;    // ITEM: when it appears ("for images", "when BleachBit is installed")
    bool off = false;          // ITEM: hidden
    QList<Entry> children;     // MENU
};

QList<Entry> defaults(Kind k);   // the default layout (what build_menu makes, every entry that can appear)
QList<Entry> layout(Kind k);     // the saved one with any entries it hasn't seen added in their default places
bool customised(Kind k);
void save(Kind k, const QList<Entry> &items);
void reset(Kind k);
void reset_all();

void tag(QAction *a, const QString &id);   // an entry's id (build_menu)
QString id_of(const QAction *a);
void arrange(QMenu *menu, Kind k);          // lay a built menu out as saved for k (nothing saved: as it is)

// Preferences → Menus
class Editor : public QWidget {
    Q_OBJECT
public:
    explicit Editor(QWidget *parent = nullptr);
    void reload();   // the saved layout of the kind chosen
    QComboBox *kind_box;   // (the tests use these)
    QTreeWidget *tree;
    Kind kind() const;
    void add_separator();
    void add_submenu(const QString &title);
    void move_selected(int by);   // up or down among its siblings; out of / into a submenu at the ends
    void remove_selected();       // a separator or a submenu (its entries move up to where it was)
    void store();                 // save the tree as the kind's layout

private:
    void fill(const QList<Entry> &items);
    QTreeWidgetItem *item_for(const Entry &e);
    bool loading = false;
    bool storing = false;   // saving its own change: not read back (the tree, its selection and what's open stay)
};

}  // namespace menus
