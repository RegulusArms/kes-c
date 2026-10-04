// The sidebar: rearranging entries and sections, collapsing sections, and sharing that with other windows and Kestrels.
#include "common.h"

using namespace test;

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    for (const QString &d : {"a", "b", "c"})
        makedirs(home_path(d), true);
    write_bookmarks({{home_path("a"), "a"}, {home_path("b"), "b"}, {home_path("c"), "c"}});
    setup_app();
    QObject::connect(atc::radio(), &atc::Radio::heard, qApp, on_atc);
    atc::radio()->start();
    MainWindow *w = open_window({HOME()});
    Sidebar *s = w->sidebar;
    FakeFlight fake;
    QString me = QString::fromUtf8(g_dbus_connection_get_unique_name(g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr)));
    wait_for([&]() { return fake.tower_pid() != 0; });
    fake.check_in();
    QString trash = join(TRASH_DIR(), "files");

    // -- the order helpers
    check(sidebar::section_order({}) == QStringList({"places", "bookmarks", "devices"}) &&
              sidebar::section_order({"devices", "nonsense"}) == QStringList({"devices", "places", "bookmarks"}),
          "a saved section order ignores unknown sections and adds missing ones");
    check(sidebar::ordered({"a", "b", "c", "d"}, {"c", "gone", "a"}) == QStringList({"c", "a", "b", "d"}),
          "a saved order puts new entries after the saved ones, in their natural order");
    check(sidebar::moved({"a", "b", "c"}, "a", 3) == QStringList({"b", "c", "a"}) &&
              sidebar::moved({"a", "b", "c"}, "c", 0) == QStringList({"c", "a", "b"}) &&
              sidebar::moved({"a", "b", "c"}, "b", 2) == QStringList({"a", "b", "c"}),
          "moving to an insertion point counts positions before the move");

    // -- rearranging
    check(s->shown_sections() == QStringList({"places", "bookmarks", "devices"}),
          "the sections start as Places, Bookmarks, Devices");
    QStringList places_before = s->entry_keys("places");
    s->move_entry("places", trash, 0);
    check(wait_for([&]() { return s->entry_keys("places").value(0) == trash; }), "Trash can be moved to the top of Places");
    s->refresh();
    check(s->entry_keys("places").value(0) == trash && settings().value("sidebar_places_order").toStringList().value(0) == trash,
          "the new order is saved and survives a refresh");
    s->move_entry("bookmarks", home_path("c"), 0);
    check(wait_for([&]() { return s->entry_keys("bookmarks") == QStringList({home_path("c"), home_path("a"), home_path("b")}); }) &&
              read_bookmarks().value(0).first == home_path("c"),
          "moving a bookmark rewrites the bookmarks file in the new order");
    s->move_entry("places", trash, 99);
    check(wait_for([&]() { return s->entry_keys("places").value(s->entry_keys("places").size() - 1) == trash; }),
          "an entry stays in its own section");
    s->move_section("devices", 0);
    check(wait_for([&]() { return s->shown_sections() == QStringList({"devices", "places", "bookmarks"}); }),
          "Devices can be moved above Places and Bookmarks");
    s->move_section("places", 3);
    check(wait_for([&]() { return s->shown_sections() == QStringList({"devices", "bookmarks", "places"}); }),
          "a section can be moved to the bottom");

    // -- collapsing
    s->toggle_section("places");
    check(wait_for([&]() { return s->entry_keys("places").isEmpty(); }) && s->shown_sections().contains("places"),
          "a collapsed section shows only its header");
    s->toggle_section("places");
    check(wait_for([&]() { return s->entry_keys("places").size() == places_before.size(); }), "it expands again");

    // -- shared
    MainWindow *w2 = open_window({HOME()});
    s->toggle_section("bookmarks");
    check(wait_for([&]() { return w2->sidebar->entry_keys("bookmarks").isEmpty() && w2->sidebar->shown_sections() == s->shown_sections(); }),
          "another window shows the same order and collapsed sections");
    check(wait_for([&]() { return fake.heard_type("sidebar", me); }), "a sidebar change is reported to other Kestrels");
    s->toggle_section("devices");   // no waiting: another Kestrel reads the file as soon as it hears the report
    bool on_disk = false;
    for (const QString &line : QString::fromUtf8(read_file(settings().fileName())).split('\n'))
        on_disk = on_disk || (line.startsWith("sidebar_collapsed=") && line.contains("devices"));
    check(on_disk, "a sidebar change is on disk before it is reported");
    {
        QSettings other(APP_ID, APP_ID);   // what another Kestrel writes
        other.setValue("sidebar_sections", QStringList({"bookmarks", "places", "devices"}));
        other.setValue("sidebar_collapsed", QStringList());
        other.sync();
    }
    fake.report({{"type", "sidebar"}});
    check(wait_for([&]() { return s->shown_sections() == QStringList({"bookmarks", "places", "devices"}); }),
          "a sidebar change from another Kestrel is picked up");

    finish();
}
