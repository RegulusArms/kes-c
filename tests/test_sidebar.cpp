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
    check(sidebar::section_order({}) == QStringList({"places", "bookmarks", "devices", "mounts"}) &&
              sidebar::section_order({"devices", "nonsense"}) == QStringList({"devices", "places", "bookmarks", "mounts"}),
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
    check(wait_for([&]() { return s->entry_keys("places").value(s->entry_keys("places").size() - 1) == trash; }) &&
              !s->entry_keys("bookmarks").contains(trash) && !s->entry_keys("devices").contains(trash),
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

    // -- Mounts: what file managers show of their own accord (GIO's rule) that Devices doesn't list
    QString user = QString::fromUtf8(g_get_user_name());
    auto shown = [](const QString &root, const QString &fs, const QString &source) {
        return drives::user_mount(drives::Mount{root, source, fs});
    };
    check(shown(home_path("data"), "zfs", "tank/data") && shown(home_path("box"), "fuse.sshfs", "user@host:/srv") &&
              shown("/media/" + user + "/stick", "vfat", "/dev/sdz1") && shown("/run/media/" + user + "/x", "exfat", "/dev/sdz2"),
          "mounts in the home folder, /media and the user's /run/media are shown");
    check(!shown(home_path(".hidden/x"), "zfs", "tank/x") && !shown(HOME(), "zfs", "tank/home") &&
              !shown("/mnt/x", "ext4", "/dev/sdz3") && !shown(home_path("tmp"), "tmpfs", "tmpfs") &&
              !shown("/run/media/someone-else/x", "exfat", "/dev/sdz4"),
          "...not one inside a hidden folder, the home folder itself, /mnt, a system file system or another user's");
    DriveInfo share;
    share.uri = "smb://nas/photos/";
    check(overview::connected("smb://nas/photos/2024", {share}) && overview::connected("smb://nas/photos", {share}) &&
              !overview::connected("smb://nas/photos2", {share}) && !overview::connected("smb://nas/photos", {}),
          "a recent server counts as connected when it's a mounted share or inside one");
    QString stick = "/media/" + user + "/kestrel-test-stick";
    qputenv("KESTREL_TEST_MOUNTS", (home_path("data") + "|zfs|tank/data;" + home_path("box") + "|fuse.sshfs|user@host:/srv;" +
                                    home_path(".hidden/x") + "|zfs|tank/x;" + stick + "|vfat|/dev/kestrel-test")
                                       .toUtf8());
    settings().setValue("recent_servers", QStringList({"smb://nas/photos", "sftp://user@backup.local/srv"}));
    QStringList mounts_expected = {home_path("box"), home_path("data"), "server:smb://nas/photos",
                                   "server:sftp://user@backup.local/srv"};
    bool listed = wait_for([&]() {
        s->refresh();   // the mount list is re-read once it's a second old
        return s->entry_keys("mounts") == mounts_expected;
    });
    check(listed, "the Mounts section lists them by place, then the recent servers (" + s->entry_keys("mounts").join(", ") + ")");
    check(s->entry_keys("devices").contains(stick) && !s->entry_keys("mounts").contains(stick),
          "a mount Devices lists isn't listed again under Mounts");
    QListWidgetItem *data_item = nullptr, *server_item = nullptr;
    for (int i = 0; i < s->count(); ++i) {
        if (s->item(i)->data(PathRole).toString() == home_path("data"))
            data_item = s->item(i);
        if (s->item(i)->data(PathRole).toString() == "smb://nas/photos")
            server_item = s->item(i);
    }
    check(data_item && data_item->text() == "data" && data_item->toolTip().contains("ZFS dataset tank/data"),
          "a mount is named after its folder and says what it is");
    check(server_item && server_item->text() == "photos on nas" &&
              server_item->foreground().color() == s->palette().color(QPalette::PlaceholderText) &&
              server_item->toolTip().contains("click to connect"),
          "a recent server that isn't connected is greyed, named like GNOME Files names it, and connects on a click");
    s->move_entry("mounts", home_path("data"), 0);
    check(wait_for([&]() { return s->entry_keys("mounts").value(0) == home_path("data"); }) &&
              settings().value("sidebar_mounts_order").toStringList().value(0) == home_path("data"),
          "a mount can be moved within Mounts, and the order is saved");
    s->toggle_section("mounts");
    check(wait_for([&]() { return s->entry_keys("mounts").isEmpty() && s->shown_sections().contains("mounts"); }),
          "Mounts collapses like the other sections");
    s->toggle_section("mounts");
    auto sidebar_reports = [&]() {
        int n = 0;
        for (const auto &[f, msg] : fake.heard)
            n += f == me && msg.value("type").toString() == "sidebar";
        return n;
    };
    wait_for([&]() { return s->entry_keys("mounts").size() == 4; });
    int reports = sidebar_reports();
    s->forget_server("smb://nas/photos");
    check(wait_for([&]() {
              return s->entry_keys("mounts").size() == 3 && !s->entry_keys("mounts").contains("server:smb://nas/photos") &&
                     sidebar_reports() > reports;
          }) && !settings().value("recent_servers").toStringList().contains("smb://nas/photos"),
          "Remove from Recent Servers takes it out of the section and the saved list, and tells other Kestrels");
    qunsetenv("KESTREL_TEST_MOUNTS");

    finish();
}
