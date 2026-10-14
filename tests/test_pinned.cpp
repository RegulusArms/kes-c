// Pinned items: pinned in their own folder (not in Starred), and Pinned & Starred First. Folder sorts: each folder
// keeps its own, the default, Apply to Subfolders.
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

    // -- folder sorts
    QString other = home_path("other"), deep = join(fsub, "deep"), deeper = join(deep, "deeper");
    makedirs(deeper, true);
    makedirs(other, true);
    for (const QString &d : {other, fsub})
        for (const QString &n : {"x.txt", "y.txt"})
            write_text(join(d, n), "x");
    auto order_in = [&](const QString &dir) {
        p->set_path(dir);
        QStringList want = {join(dir, "x.txt"), join(dir, "y.txt")};
        wait_for([&]() { return p->all_paths().filter(".txt") == want ||
                                p->all_paths().filter(".txt") == QStringList({want[1], want[0]}); });
        return p->all_paths().filter(".txt") == want ? Qt::AscendingOrder : Qt::DescendingOrder;
    };
    check(order_in(other) == Qt::AscendingOrder, "a folder never sorted uses the default");
    p->sort_by(0, Qt::DescendingOrder);
    check(wait_for([&]() { return p->all_paths().filter(".txt") == QStringList({join(other, "y.txt"), join(other, "x.txt")}); }),
          "sorting a folder sorts it");
    check(QJsonDocument::fromJson(read_file(join(CONFIG_DIR(), "sorts.json"))).object().value(other).toArray() ==
              QJsonArray({0, int(Qt::DescendingOrder), 0, 0}),
          "the folder's sort is saved in sorts.json");
    check(wait_for([&]() { return fake.heard_type("sorts", me); }), "a folder's sort is reported to other Kestrels");
    check(order_in(fsub) == Qt::AscendingOrder && w->view_value("sort_order", 0).toInt() == Qt::AscendingOrder,
          "sorting a folder leaves the other folders and the default as they were");
    check(order_in(other) == Qt::DescendingOrder, "a folder keeps its own sort when you come back to it");
    Pane *t2 = w->new_tab(other);
    check(t2->tree->header()->sortIndicatorOrder() == Qt::DescendingOrder, "another tab opening the folder shows its sort");
    p->sort_by(0, Qt::AscendingOrder);
    check(wait_for([&]() { return t2->tree->header()->sortIndicatorOrder() == Qt::AscendingOrder; }),
          "sorting a folder re-sorts the other tabs showing it");
    p->sort_by(0, Qt::DescendingOrder);
    w->close_tab(w->tabs->indexOf(t2));

    // Apply to Subfolders: deep and deeper get fsub's sort, nothing outside fsub does
    auto is = [](const QString &d, int col, int order) {
        auto s = places::folder_sort(d);
        return s && s->column == col && s->order == order;
    };
    p->set_path(fsub);
    p->sort_by(1, Qt::DescendingOrder);   // by size
    check(!places::folder_sort(deep) && !places::folder_sort(deeper), "(before) the subfolders have no sort of their own");
    w->sort_subfolders(p);
    check(wait_for([&]() { return places::folder_sort(deeper).has_value(); }), "Apply to Subfolders reaches every folder below");
    check(is(deep, 1, Qt::DescendingOrder) && is(deeper, 1, Qt::DescendingOrder) && is(fsub, 1, Qt::DescendingOrder),
          "...with the folder's sort");
    check(is(fs, 0, Qt::AscendingOrder) && is(other, 0, Qt::DescendingOrder), "...and changes no folder outside it");
    p->set_path(deep);
    check(p->tree->header()->sortIndicatorSection() == 1 && p->tree->header()->sortIndicatorOrder() == Qt::DescendingOrder,
          "a subfolder opens with the sort it was given");

    // Set as Default: folders without their own sort follow it, the others keep theirs
    QString fresh = home_path("fresh");
    makedirs(fresh, true);
    w->set_default_sort(p);
    p->set_path(fresh);
    check(p->tree->header()->sortIndicatorSection() == 1 && p->tree->header()->sortIndicatorOrder() == Qt::DescendingOrder &&
              !places::folder_sort(fresh),
          "Set as Default sorts the folders without their own sort that way");
    check(order_in(other) == Qt::DescendingOrder && p->tree->header()->sortIndicatorSection() == 0,
          "...and a folder with its own sort keeps it");

    // another Kestrel's sorts
    p->set_path(fs);
    write_text(join(CONFIG_DIR(), "sorts.json"), QJsonDocument(QJsonObject{{fs, QJsonArray{0, 1}}}).toJson());
    fake.report({{"type", "sorts"}});
    check(wait_for([&]() { return p->tree->header()->sortIndicatorOrder() == Qt::DescendingOrder; }),
          "sorts from another Kestrel are picked up by a tab showing the folder");
    check(p->all_paths().first() == fd, "...and it re-sorts the folder");

    finish();
}
