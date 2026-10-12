// Pinned items: pinned in their own folder (not in Starred), and Pinned & Starred First.
#include "common.h"

using namespace test;

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    QString fs = home_path("fs");
    makedirs(join(fs, "sub"), true);
    for (const QString &n : {"a.txt", "b.txt", "c.txt"})
        write_text(join(fs, n), "x");
    QString fsub = join(fs, "sub"), fa = join(fs, "a.txt"), fb = join(fs, "b.txt"), fc = join(fs, "c.txt"),
            fd = join(fs, "d.txt");
    setup_app();
    QObject::connect(atc::radio(), &atc::Radio::heard, qApp, on_atc);
    atc::radio()->start();
    MainWindow *w = open_window({fs});
    Pane *p = w->pane();
    FakeFlight fake;
    QString me = QString::fromUtf8(g_dbus_connection_get_unique_name(g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr)));
    wait_for([&]() { return fake.tower_pid() != 0; });
    fake.check_in();
    p->sort_by(0, Qt::AscendingOrder);
    check(wait_for([&]() { return p->all_paths() == QStringList({fsub, fa, fb, fc}); }), "the folder lists by name");

    // -- pinning
    places::set_pinned({fc, fsub}, true);
    check(places::is_pinned(fc) && places::is_pinned(fsub) && !places::is_pinned(fa) && !places::is_starred(fc) &&
              !places::starred().contains(fsub),
          "a pin marks the item, but doesn't add it to Starred");
    check(QJsonDocument::fromJson(read_file(join(CONFIG_DIR(), "pinned.json"))).object().value(fs).toArray() ==
              QJsonArray({"c.txt", "sub"}),
          "pins are saved in pinned.json");
    check(wait_for([&]() { return fake.heard_type("starred", me); }), "a pin is reported to other Kestrels");

    // -- Pinned & Starred First
    check(p->all_paths() == QStringList({fsub, fa, fb, fc}), "with it off the folder keeps its sort order");
    p->set_pinned_first(true);
    check(wait_for([&]() { return p->all_paths() == QStringList({fsub, fc, fa, fb}); }),
          "Pinned & Starred First puts the pinned items first, each group still sorted");
    p->sort_by(0, Qt::DescendingOrder);
    check(wait_for([&]() { return p->all_paths() == QStringList({fc, fsub, fb, fa}); }),
          "sorting the other way keeps the pinned items first");
    p->sort_by(0, Qt::AscendingOrder);
    places::set_starred({fb}, true);
    check(wait_for([&]() { return p->all_paths() == QStringList({fsub, fb, fc, fa}); }),
          "starred items in the folder go first too, as soon as they're starred");
    places::set_starred({fb}, false);
    places::set_pinned({fc}, false);
    check(wait_for([&]() { return p->all_paths() == QStringList({fsub, fa, fb, fc}); }),
          "unpinning puts the item back in its place");
    write_text(fd, "x");
    places::set_pinned({fd}, true);
    check(wait_for([&]() { return p->all_paths() == QStringList({fsub, fd, fa, fb, fc}); }),
          "a new file that's pinned is sorted first");
    p->set_path(HOME());
    p->set_path(fs);
    check(wait_for([&]() { return p->all_paths() == QStringList({fsub, fd, fa, fb, fc}); }),
          "going away and back keeps the pinned items first");
    p->select_paths({fc});
    check(p->selected_paths() == QStringList({fc}), "selecting by path works through the sorting");
    w->set_pinned_first(false);
    check(wait_for([&]() { return p->all_paths() == QStringList({fsub, fa, fb, fc, fd}); }) &&
              !w->view_value("pinned_first").toBool(),
          "turning it off restores the order and is saved");

    // -- another Kestrel's pins
    write_text(join(CONFIG_DIR(), "pinned.json"), QJsonDocument(QJsonObject{{fs, QJsonArray{"a.txt"}}}).toJson());
    fake.report({{"type", "starred"}});
    check(wait_for([&]() { return places::is_pinned(fa) && !places::is_pinned(fsub); }),
          "pins from another Kestrel are picked up");

    finish();
}
