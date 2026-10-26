// Preferences (prefs.h, prefswin.h) and the right-click menus' layouts (menus.h): every setting in one window, in tabs,
// applied at once to every open window and told to the other Kestrels; changes made in the windows shown in an open
// Preferences; the search box and the resets; and menus laid out as the user chose (hidden, reordered, moved into or
// out of submenus, new entries in their default places).
#include "common.h"

#include "archive_ui.h"
#include "menus.h"
#include "prefs.h"
#include "prefswin.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDirIterator>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabWidget>
#include <QTreeWidget>

using namespace test;

static void choose(QComboBox *cb, const QVariant &data)
{
    int i = cb ? cb->findData(data) : -1;
    if (i < 0)
        return;
    cb->setCurrentIndex(i);
    Q_EMIT cb->activated(i);   // as a click does
}

// the menu's entries as ids, a submenu as "id[child, child]", separators as "-"
static QStringList shape(QMenu *m)
{
    QStringList out;
    for (QAction *a : m->actions()) {
        if (a->isSeparator()) {
            out << "-";
        } else if (a->menu() && !a->menu()->actions().isEmpty() && QStringList{"links", "copy_path", "image", "previews"}.contains(menus::id_of(a))) {
            out << menus::id_of(a) + "[" + shape(a->menu()).join(",") + "]";
        } else if (a->menu() && menus::id_of(a).startsWith("custom")) {
            out << a->text() + "[" + shape(a->menu()).join(",") + "]";
        } else {
            out << (menus::id_of(a).isEmpty() ? "?" + a->text() : menus::id_of(a));
        }
    }
    return out;
}

static QAction *action(MainWindow *w, const QString &text)   // a window's own action (View menu…)
{
    for (QAction *a : w->findChildren<QAction *>())
        if (a->text() == text)
            return a;
    return nullptr;
}

static QStringList flat_ids(const QList<menus::Entry> &items)
{
    QStringList out;
    for (const menus::Entry &e : items) {
        if (e.type == menus::Entry::SEP)
            continue;
        out << e.id;
        out << flat_ids(e.children);
    }
    return out;
}

static menus::Entry *find(QList<menus::Entry> &items, const QString &id)
{
    for (menus::Entry &e : items) {
        if (e.type != menus::Entry::SEP && e.id == id)
            return &e;
        if (menus::Entry *c = find(e.children, id))
            return c;
    }
    return nullptr;
}

static menus::Entry take(QList<menus::Entry> &items, const QString &id)
{
    for (int i = 0; i < items.size(); ++i) {
        if (items[i].type != menus::Entry::SEP && items[i].id == id)
            return items.takeAt(i);
        for (int j = 0; j < items[i].children.size(); ++j)
            if (items[i].children[j].type != menus::Entry::SEP && items[i].children[j].id == id)
                return items[i].children.takeAt(j);
    }
    return {};
}

static int index_of(const QList<menus::Entry> &items, const QString &id)
{
    for (int i = 0; i < items.size(); ++i)
        if (items[i].type != menus::Entry::SEP && items[i].id == id)
            return i;
    return -1;
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    setup_app();
    QObject::connect(atc::radio(), &atc::Radio::heard, qApp, on_atc);
    atc::radio()->start();
    QString dir = home_path("place");
    makedirs(join(dir, "sub"), true);
    write_text(join(dir, ".secret"), "s");
    write_text(join(dir, "big.txt"), QByteArray(5000, 'x'));
    write_text(join(dir, "small.txt"), "x");
    MainWindow *w1 = open_window({dir});
    MainWindow *w2 = open_window({dir});
    FakeFlight fake;
    wait_for([&]() { return fake.tower_pid() != 0 && fake.heard_type("windows", QString::fromUtf8(g_dbus_connection_get_unique_name(g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr)))); }, 15000);
    wait_for([&]() { return w1->pane()->all_paths().size() == 3 && w2->pane()->all_paths().size() == 3; });

    // ---- every setting the code reads is in the list Preferences is built from
    {
        // keys that aren't settings to choose but state kept as Kestrel is used (window sizes, history, orders)
        static const QStringList STATE = {"geometry", "splitter", "recent_servers", "chooser_folder", "jobs_window_geometry",
                                          "jobs_window_split", "jobs_window_lists", "sidebar_places_order", "sidebar_devices_order",
                                          "sidebar_mounts_order", "sidebar_collapsed", "tower_window_geometry",
                                          "viewer_geometry", "viewer_recent_folders"};
        QStringList missing;
        // settings().value("key"), view_value("key"), and s.value("key") where s is the settings (QSettings &s)
        static const QRegularExpression re(R"(settings\(\)\.value\("([^"]+)\"|view_value\("([^"]+)\")");
        static const QRegularExpression via_s(R"(\bs\.value\("([^"]+)\")");
        QDirIterator it(join(QString(TESTS_DIR), "../src"), {"*.cpp"});
        while (it.hasNext()) {
            QString f = it.next();
            if (f.endsWith("prefs.cpp") || f.endsWith("prefswin.cpp"))
                continue;
            QString text = QString::fromUtf8(read_file(f));
            QStringList keys;
            for (auto m = re.globalMatch(text); m.hasNext();) {
                auto x = m.next();
                keys << (x.captured(1).isEmpty() ? x.captured(2) : x.captured(1));
            }
            if (text.contains("QSettings &s = settings()"))
                for (auto m = via_s.globalMatch(text); m.hasNext();)
                    keys << m.next().captured(1);
            for (const QString &key : keys) {
                if (key.startsWith("chooser/") || key.endsWith('/'))
                    continue;
                if (!prefs::find(key) && !STATE.contains(key) && !missing.contains(key))
                    missing << basename(f) + ": " + key;
            }
        }
        check(missing.isEmpty(), "every setting the code reads is in Preferences' list (or is kept state, like window "
                                 "sizes)" + (missing.isEmpty() ? QString() : " (missing: " + missing.join(", ") + ")"));
    }

    // ---- the window
    w1->preferences();
    auto *pd = shown_window<PreferencesDialog>();
    QStringList tab_texts;
    for (int i = 0; pd && i < pd->tabs->count(); ++i)
        tab_texts << pd->tabs->tabText(i);
    QStringList no_editor;
    for (const prefs::Pref &p : prefs::all())
        if (!pd || !pd->editor(p.key))
            no_editor << p.key;
    check(pd && is_free_window(pd) &&
              tab_texts == QStringList({"General", "Views", "Sidebar", "Images & Videos", "Files", "Archives", "Windows", "Menus"}) &&
              no_editor.isEmpty(),
          "Preferences is a free window with tabs (" + tab_texts.join(", ") + ") and a control for every setting" +
              (no_editor.isEmpty() ? QString() : " (none for: " + no_editor.join(", ") + ")"));
    if (!pd)
        finish();
    auto box = [&](const QString &key) { return qobject_cast<QCheckBox *>(pd->editor(key)); };
    auto spin = [&](const QString &key) { return qobject_cast<QSpinBox *>(pd->editor(key)); };
    auto combo = [&](const QString &key) { return qobject_cast<QComboBox *>(pd->editor(key)); };

    // ---- a change applies at once to every open window, and is told to the other Kestrels
    box("show_hidden")->setChecked(true);
    bool hidden_both = wait_for([&]() {
        return w1->pane()->all_paths().contains(join(dir, ".secret")) && w2->pane()->all_paths().contains(join(dir, ".secret"));
    });
    check(hidden_both && settings().value("show_hidden").toBool() && action(w1, "Show Hidden Files")->isChecked() && action(w2, "Show Hidden Files")->isChecked(),
          "Show hidden files ticked in Preferences shows them at once in every window (and ticks the View menu's)");
    check(wait_for([&]() { return fake.last_of("settings").value("keys").toArray() == QJsonArray{"show_hidden"}; }),
          "...and the other Kestrels are told which setting changed");
    choose(combo("view_mode"), "list");
    check(!w1->pane()->is_grid() && !w2->pane()->is_grid(), "View → List in Preferences switches every window to the list");
    choose(combo("view_mode"), "grid");
    spin("grid_size")->setValue(200);
    check(w1->pane()->grid_size == 200 && w2->pane()->grid_size == 200, "the grid's icon size applies to every window");
    box("pinned_first")->setChecked(true);
    check(w1->pane()->pinned_first() && w2->pane()->pinned_first(), "Pinned & Starred First applies to every window");
    choose(combo("sort_col"), 1);
    choose(combo("sort_order"), 1);
    check(w1->pane()->tree->header()->sortIndicatorSection() == 1 && w2->pane()->tree->header()->sortIndicatorOrder() == Qt::DescendingOrder,
          "the default sort (size, descending) applies at once to folders without their own");
    box("sidebar")->setChecked(false);
    check(!w1->sidebar->isVisible() && !w2->sidebar->isVisible() && !action(w1, "Sidebar")->isChecked(),
          "Show the sidebar unticked hides it in every window");
    box("sidebar")->setChecked(true);
    spin("folder_count")->setValue(2);
    check(g_thumbs->folder_count == 2, "folder preview settings take effect at once");

    // ---- a change made in a window shows in the open Preferences
    action(w2, "Show Hidden Files")->trigger();   // Ctrl+H
    bool shown_off = wait_for([&]() { return !box("show_hidden")->isChecked(); });
    w2->pane()->zoom(0, 120);
    bool zoom_shown = wait_for([&]() { return spin("grid_size")->value() == 120; });
    check(shown_off && zoom_shown, "a change made in a window (Ctrl+H, the zoom) shows at once in Preferences");

    // ---- the search box
    pd->search->setText("hidden");
    int views = prefs::tabs().indexOf("Views"), menus_tab = pd->tabs->count() - 1;
    // (isHidden: rows on a tab not showing aren't visible either way)
    bool filtered = !pd->row("show_hidden")->isHidden() && pd->row("grid_size")->isHidden() &&
                    pd->tabs->isTabEnabled(views) && pd->tabs->tabText(views).startsWith("Views (") &&
                    !pd->tabs->isTabEnabled(menus_tab);
    pd->search->setText("right-click");
    bool menus_found = pd->tabs->isTabEnabled(menus_tab) && !pd->tabs->isTabEnabled(views);
    pd->search->clear();
    bool cleared = !pd->row("grid_size")->isHidden() && pd->tabs->isTabEnabled(views) && pd->tabs->tabText(views) == "Views";
    check(filtered && menus_found && cleared, "the search box shows only the matching settings in every tab, greys out tabs "
                                              "with none, and clearing it shows them all again");

    // ---- the homepage: a folder that isn't one isn't saved
    // the tape buffer's size now: when the system can't spare the largest size, it says so
    {
        QString mi = home_path("meminfo");
        write_text(mi, "MemTotal:       8388608 kB\nMemFree:        1000 kB\nMemAvailable:   6291456 kB\n");   // 8 GB, 6 free
        qputenv("KESTREL_TEST_MEMINFO", mi.toUtf8());
        pd->refresh({"tape_buffer_now"});
        auto *now = pd->findChild<QLabel *>("tape_buffer_now_label");
        QString small = now ? now->text() : QString();
        write_text(mi, "MemTotal:       67108864 kB\nMemFree:        1000 kB\nMemAvailable:   52428800 kB\n");   // 64, 50
        pd->refresh({"tape_buffer_now"});
        QString roomy = now ? now->text() : QString();
        qunsetenv("KESTREL_TEST_MEMINFO");
        check(small.startsWith("Up to 3.2 GB (memory 8.6 GB, 6.4 GB available now)\n16.0 GB is more than this system "
                               "can spare now") &&
                  roomy.startsWith("Up to 16.0 GB (") && !roomy.contains("spare"),
              "Preferences shows the tape buffer's size now, and says when the system can't spare the largest size (" +
                  small + " / " + roomy + ")");
    }
    auto *hp_mode = pd->findChild<QComboBox *>("homepage_mode");
    auto *hp_path = pd->findChild<QLineEdit *>("homepage_path");
    auto *hp_err = pd->findChild<QLabel *>("homepage_error");
    hp_path->setText(home_path("no-such-folder"));
    hp_mode->setCurrentIndex(2);
    Q_EMIT hp_mode->activated(2);
    bool refused = !hp_err->isHidden() && prefs::get("homepage").toString() == "overview";
    hp_path->setText(dir);
    Q_EMIT hp_path->editingFinished();
    check(refused && prefs::get("homepage").toString() == dir && hp_err->isHidden(),
          "a homepage that isn't a folder is refused with a message; a folder is saved");

    // ---- the file chooser's own settings: the main windows' until set, then its own
    bool follows = prefs::get("chooser/view_mode").toString() == "grid" && !settings().contains("chooser/view_mode");
    choose(combo("chooser/view_mode"), "list");
    check(follows && settings().value("chooser/view_mode").toString() == "list" && w1->pane()->is_grid() &&
              prefs::get("view_mode").toString() == "grid",
          "the file chooser's view follows the main windows' until set in Preferences, and setting it leaves theirs alone");

    // ---- archives: each format's options and Extract To…'s
    if (!archive::tool("7z").isEmpty()) {
        choose(pd->findChild<QComboBox *>("archive_formats_format"), "7z");
        pd->findChild<QSpinBox *>("archive_formats_level")->setValue(3);
        CompressDialog cd(w1, {join(dir, "small.txt")});
        choose(cd.findChild<QComboBox *>(), "7z");
        bool level = settings().value("archive/7z/level_7z").toInt() == 3;
        archive::Spec sp = cd.spec();
        check(level && sp.format.id == "7z" && sp.level && *sp.level == 3,
              "a format's level set in Preferences is what Compress… starts with");
    } else {
        skip("a format's options in Preferences (7z isn't installed)");
    }
    box("archive/x_subfolder")->setChecked(false);
    ExtractDialog ed(w1, {join(dir, "small.txt")}, false);
    check(!ed.options()["subfolder"].toBool(), "Extract To…'s choices set in Preferences are what it starts with");

    // ---- resets
    pd->reset_tab(views);
    bool views_back = !settings().contains("show_hidden") && !settings().contains("grid_size") &&
                      !settings().contains("sort_col") && w1->pane()->tree->header()->sortIndicatorSection() == 0 &&
                      !box("pinned_first")->isChecked() && !w1->pane()->pinned_first() && spin("grid_size")->value() == 160;
    check(views_back && settings().contains("chooser/view_mode"),
          "Reset This Tab puts that tab's settings back to their defaults (and the windows with them), and only that tab's");

    // ---- the right-click menus: every entry has an id, and the defaults list them in the order the menu has them
    QString img = join(dir, "photo.png");
    QImage(8, 8, QImage::Format_RGB32).save(img);
    QString arc = join(dir, "pack.zip");
    write_text(arc, "PK");
    struct Sample {
        menus::Kind kind;
        QStringList paths;
    };
    QList<Sample> samples = {{menus::FOLDER, {join(dir, "sub")}},
                             {menus::FILE, {join(dir, "small.txt")}},
                             {menus::FILE, {img}},
                             {menus::ARCHIVE, {arc}},
                             {menus::SEVERAL, {join(dir, "small.txt"), join(dir, "big.txt")}},
                             {menus::BACKGROUND, {}}};
    wait_for([&]() { return w1->pane()->all_paths().contains(img) && w1->pane()->all_paths().contains(arc); });
    QStringList problems;
    for (const Sample &s : samples) {
        QMenu *m = w1->build_menu(w1->pane(), s.paths);
        QStringList order = flat_ids(menus::defaults(s.kind));
        int last = -1;
        std::function<void(QMenu *)> walk = [&](QMenu *menu) {
            for (QAction *a : menu->actions()) {
                if (a->isSeparator())
                    continue;
                QString id = menus::id_of(a);
                int at = order.indexOf(id, std::max(last, 0));
                if (id.isEmpty())
                    problems << menus::kind_key(s.kind) + ": no id for " + a->text();
                else if (at < 0)
                    problems << menus::kind_key(s.kind) + ": " + id + " not in the defaults (or out of order)";
                else
                    last = at;
                if (a->menu() && QStringList{"links", "copy_path", "image", "previews"}.contains(id))
                    walk(a->menu());
            }
        };
        walk(m);
        delete m;
    }
    check(problems.isEmpty(), "every right-click menu entry has an id, and the default layouts list them all in the menu's "
                              "order" + (problems.isEmpty() ? QString() : " (" + problems.mid(0, 4).join("; ") + ")"));

    // ---- a layout: hidden, reordered, out of a submenu, into a submenu of the user's own
    QString txt = join(dir, "small.txt");
    auto menu_of = [&](const QStringList &paths) {
        QMenu *m = w1->build_menu(w1->pane(), paths);
        QStringList s = shape(m);
        delete m;
        return s;
    };
    QStringList before = menu_of({txt});
    QList<menus::Entry> lay = menus::defaults(menus::FILE);
    find(lay, "duplicate")->off = true;
    menus::Entry props = take(lay, "properties");
    lay.prepend(props);
    menus::Entry name = take(lay, "copy_name");
    lay.insert(index_of(lay, "copy") + 1, name);
    menus::Entry move = take(lay, "move_to"), copy_to = take(lay, "copy_to");
    menus::Entry mine;
    mine.type = menus::Entry::MENU;
    mine.id = "custom1";
    mine.label = "Mine";
    mine.children = {move, copy_to};
    lay.insert(index_of(lay, "rename"), mine);
    menus::Entry s1, s2;
    s1.type = s2.type = menus::Entry::SEP;
    lay.insert(index_of(lay, "cut"), s1);
    lay.insert(index_of(lay, "cut"), s2);
    menus::Entry view = take(lay, "view_image");
    lay.prepend(view);
    menus::Entry s0;
    s0.type = menus::Entry::SEP;
    lay.prepend(s0);   // before everything: must not show
    menus::save(menus::FILE, lay);
    QStringList after = menu_of({txt});
    QStringList image_menu = menu_of({img});
    bool seps_ok = true;
    for (int i = 0; i < after.size(); ++i)
        if (after[i] == "-" && (i == 0 || i == after.size() - 1 || after[i - 1] == "-"))
            seps_ok = false;
    int copy_at = after.indexOf("copy");
    check(after.value(0) == "properties" && !after.contains("duplicate") && after.value(copy_at + 1) == "copy_name" &&
              after.contains("Mine[move_to,copy_to]") && !after.contains("move_to") && seps_ok &&
              before.contains("duplicate"),
          "a file's menu follows its layout: an entry hidden, one moved to the top, one taken out of its submenu, two put in "
          "a submenu of the user's own, and no doubled or stray separators (" + after.mid(0, 8).join(" ") + "…)");
    check(!after.contains("view_image") && image_menu.value(0) == "view_image" && image_menu.value(1) == "properties",
          "an entry that doesn't apply (View Image for a text file) doesn't appear, and appears where it was put when it "
          "does (an image)");
    check(menu_of({join(dir, "sub")}).contains("duplicate") && menu_of({}).contains("properties"),
          "a layout is for its kind of item only: a folder's menu and the empty space's are as before");

    // ---- an entry added in a later version: in its default place
    {
        QList<menus::Entry> old = menus::defaults(menus::FILE);
        take(old, "duplicate");
        QStringList known = flat_ids(old);
        QJsonArray items;
        std::function<QJsonArray(const QList<menus::Entry> &)> js = [&](const QList<menus::Entry> &l) {
            QJsonArray a;
            for (const menus::Entry &e : l)
                if (e.type == menus::Entry::SEP)
                    a << QJsonObject{{"sep", true}};
                else if (e.type == menus::Entry::MENU)
                    a << QJsonObject{{"menu", e.id}, {"title", e.label}, {"items", js(e.children)}};
                else
                    a << QJsonObject{{"id", e.id}};
            return a;
        };
        QList<menus::Entry> moved = old;   // the user had moved Copy To… first
        menus::Entry ct = take(moved, "copy_to");
        moved.prepend(ct);
        settings().setValue("menus/file", QString::fromUtf8(QJsonDocument(QJsonObject{{"items", js(moved)},
                                                                                        {"known", QJsonArray::fromStringList(known)}})
                                                              .toJson(QJsonDocument::Compact)));
        QStringList m = menu_of({txt});
        QStringList editor_view = flat_ids(menus::layout(menus::FILE));
        check(m.value(0) == "copy_to" && m.value(1) == "duplicate" && m.indexOf("duplicate") == 1 &&
                  editor_view.value(1) == "duplicate",
              "an entry the saved layout has never seen (added in a later version) appears after the one it follows by "
              "default, wherever that was moved (in the menu and in the editor)");
    }

    // ---- the editor (Preferences → Menus)
    menus::reset_all();
    menus::Editor *me = pd->menu_editor;
    pd->tabs->setCurrentIndex(menus_tab);
    choose(me->kind_box, int(menus::FILE));
    auto top_item = [&](const QString &id) -> QTreeWidgetItem * {
        for (int i = 0; i < me->tree->topLevelItemCount(); ++i)
            if (me->tree->topLevelItem(i)->data(0, Qt::UserRole + 2).toString() == id)
                return me->tree->topLevelItem(i);
        return nullptr;
    };
    QTreeWidgetItem *dup = top_item("duplicate");
    if (dup)
        dup->setCheckState(0, Qt::Unchecked);
    bool hidden = menus::customised(menus::FILE) && !menu_of({txt}).contains("duplicate");
    QTreeWidgetItem *pr = top_item("properties");
    me->tree->setCurrentItem(pr);
    int at = me->tree->indexOfTopLevelItem(pr);
    me->move_selected(-1);
    me->move_selected(-1);   // past the separator before it
    bool up = me->tree->indexOfTopLevelItem(top_item("properties")) == at - 2;
    me->tree->setCurrentItem(top_item("cut"));
    me->add_submenu("Tools");
    QTreeWidgetItem *tools = me->tree->currentItem();
    me->tree->setCurrentItem(top_item("rename"));
    QTreeWidgetItem *ren = me->tree->currentItem();
    for (int guard = 0; guard < 100 && ren && ren->parent() == nullptr &&
                        me->tree->indexOfTopLevelItem(ren) > me->tree->indexOfTopLevelItem(tools) + 1;
         ++guard)
        me->move_selected(-1);
    me->move_selected(-1);   // into the submenu it meets
    QStringList with_tools = menu_of({txt});
    bool into = ren && ren->parent() == tools && with_tools.contains("Tools[rename]");
    me->tree->setCurrentItem(tools);
    me->remove_selected();
    bool out = !menu_of({txt}).contains("Tools[rename]") && menu_of({txt}).contains("rename");
    check(hidden && up && into && out,
          "the menu editor hides an entry (unticked), moves one up, into a new submenu and, removing the submenu, back out");
    bool told = wait_for([&]() {
        QJsonArray keys = fake.last_of("settings").value("keys").toArray();
        return keys.contains("menus/file");
    });
    check(told, "a menu layout changed is told to the other Kestrels");
    pd->reset_all();
    check(!menus::customised(menus::FILE) && menu_of({txt}) == before && !settings().contains("chooser/view_mode") &&
              !settings().contains("homepage"),
          "Reset All puts every setting and every menu back to the defaults");
    pd->close();
    finish();
}
