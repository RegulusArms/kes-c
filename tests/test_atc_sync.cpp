// The tower (atc.h): starting it, checking in, and passing shared-state changes between Kestrels.
#include "common.h"

#include "stats.h"

using namespace test;

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    if (argc == 4 && QByteArray(argv[1]) == "--set-setting") {   // plays another Kestrel saving its Preferences
        QSettings s(APP_ID, APP_ID);
        s.setValue(argv[2], QString::fromLocal8Bit(argv[3]));
        s.sync();
        return 0;
    }
    QApplication app(argc, argv);
    QString home = HOME();
    for (const QString &d : {"d1", "d2"})
        makedirs(home_path(d), true);
    write_text(home_path("f1"), "a");
    write_text(home_path("f2"), "b");
    setup_app();
    stats::enable();   // KESTREL_STATS: the tower delay check
    ThumbnailManager *thumbs = g_thumbs;
    QObject::connect(atc::radio(), &atc::Radio::heard, qApp, on_atc);
    atc::radio()->start();
    MainWindow *w = open_window({home});
    FakeFlight fake;
    QString me = QString::fromUtf8(g_dbus_connection_get_unique_name(g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr)));

    check(wait_for([&]() { return fake.tower_pid() != 0; }), "the first Kestrel starts a tower");
    check(wait_for([&]() { return fake.flights_have(QCoreApplication::applicationPid(), "c++"); }),
          "Kestrel checked in (impl c++)");
    fake.check_in();
    check(fake.flights_have(1, "fake"), "a second flight is listed");

    // ---- this Kestrel reports its changes
    QString f1 = home_path("f1"), f2 = home_path("f2"), d1 = home_path("d1");
    places::set_starred({f1}, true);
    check(wait_for([&]() { return fake.heard_type("starred", me); }), "Star is reported");
    thumbs->set_folder_color({d1}, "#123456");
    check(wait_for([&]() { return fake.heard_type("folders", me); }) &&
              fake.last_of("folders").value("paths").toArray() == QJsonArray{d1},
          "Folder colour is reported with its folder");
    int n = fake.heard.size();
    thumbs->set_cover(d1, f1);
    check(wait_for([&]() { return fake.heard.size() > n; }) && fake.last_of("folders").value("paths").toArray() == QJsonArray{d1},
          "Folder cover is reported");
    write_bookmarks({{home_path("d2"), "Mine"}});
    check(wait_for([&]() { return fake.heard_type("bookmarks", me); }), "Bookmarks are reported");
    w->preferences();
    auto *pd = w->findChild<PreferencesDialog *>();
    if (pd)
        pd->accept();
    check(pd && wait_for([&]() { return fake.heard_type("settings", me); }), "Saved Preferences are reported");
    w->clear_cache();
    check(wait_for([&]() { return fake.heard_type("thumbs_cleared", me); }), "Clearing the preview cache is reported");

    // ---- changes from another Kestrel reach this one
    int starred_signals = 0;
    QObject::connect(places::signals_(), &places::Signals::starred_changed, [&]() { ++starred_signals; });
    write_text(join(CONFIG_DIR(), "starred.json"), QJsonDocument(QJsonArray{f2}).toJson());
    fake.report({{"type", "starred"}});
    check(wait_for([&]() { return places::is_starred(f2) && !places::is_starred(f1); }) && starred_signals > 0,
          "Another Kestrel's stars are picked up");
    fake.report({{"type", "starred"}, {"sent", double(QDateTime::currentMSecsSinceEpoch())}});
    check(wait_for([]() { return stats::summary().contains("  tower delay (ms): "); }),
          "with KESTREL_STATS, a report's time to arrive through the tower is counted");
    write_text(thumbs::styles_file(), QJsonDocument(QJsonObject{{d1, QJsonObject{{"color", "#abcdef"}}}}).toJson());
    write_text(thumbs::covers_file(), "{}");
    fake.report({{"type", "folders"}, {"paths", QJsonArray{d1}}});
    check(wait_for([&]() { return thumbs->custom_color(d1) == "#abcdef"; }), "Another Kestrel's folder colour is picked up");
    check(thumbs->covers.value(d1).isEmpty(), "...and its cover change");
    QProcess::execute(QCoreApplication::applicationFilePath(), {"--set-setting", "folder_count", "2"});   // another process
    fake.report({{"type", "settings"}});
    check(wait_for([&]() { return thumbs->folder_count == 2; }), "Another Kestrel's Preferences are applied");
    write_text(GTK_BOOKMARKS(), ("file://" + home + "/d1 Theirs\n").toUtf8());
    fake.report({{"type", "bookmarks"}});
    check(wait_for([&]() { return !w->sidebar->findItems("Theirs", Qt::MatchExactly).isEmpty(); }),
          "Another Kestrel's bookmarks appear in the sidebar");

    // ---- read-modify-write: our next change keeps theirs
    write_text(thumbs::styles_file(), QJsonDocument(QJsonObject{{d1, QJsonObject{{"color", "#abcdef"}}},
                                                                {home_path("d2"), QJsonObject{{"color", "#00ff00"}}}})
                                          .toJson());
    thumbs->set_folder_previews({d1}, false);
    QJsonObject st = QJsonDocument::fromJson(read_file(thumbs::styles_file())).object();
    check(st.value(home_path("d2")).toObject().value("color").toString() == "#00ff00",
          "Saving a folder style keeps another Kestrel's change");

    // ---- the protocol: other programs on the session bus can talk to the tower, so messages are checked
    int before = fake.heard.size();
    QByteArray big = QJsonDocument(QJsonObject{{"type", "starred"}, {"pad", QString(atc::MAX_MESSAGE, 'x')}})
                         .toJson(QJsonDocument::Compact);
    for (const QByteArray &raw : QList<QByteArray>{"not json", "[1,2]", R"({"type":"nonsense"})",
                                                   R"({"type":"folders","paths":"/not/a/list"})",
                                                   R"({"type":"folders","paths":["relative"]})",
                                                   R"({"type":"tasks","tasks":[{"id":5}]})", R"({"type":"left"})", big})
        fake.call("Report", raw);
    fake.report({{"type", "folders"}, {"paths", QJsonArray{d1}}, {"unknown_field", 1}, {"marker", "good"}});
    auto mine_since = [&](int from) {
        QList<QJsonObject> out;
        for (int i = from; i < fake.heard.size(); ++i)
            if (fake.heard[i].first == fake.name())
                out << fake.heard[i].second;
        return out;
    };
    wait_for([&]() { return !mine_since(before).isEmpty(); });
    spin(300);
    QList<QJsonObject> passed = mine_since(before);
    check(passed.size() == 1 && passed[0].value("marker").toString() == "good",
          "the tower drops malformed, unknown and oversized messages, and passes on the next good one");
    check(atc::valid_message({{"type", "tasks"}, {"tasks", QJsonArray{QJsonObject{{"id", "1"}, {"fraction", 0.5}}}}}) &&
              !atc::valid_message({{"type", "cancel"}, {"task", 7}}) &&
              !atc::valid_message({{"type", "open"}, {"folders", QJsonArray{"rel"}}}) &&
              !atc::valid_message({{"type", "settings"}, {"keep", "yes"}}) &&
              atc::valid_message({{"type", "settings"}, {"from_the_future", QJsonArray{1, 2}}}) &&
              atc::valid_undo({{"kind", "trash"}, {"label", "Trash"}, {"items", QJsonArray{QJsonArray{"/a", ""}}}}) &&
              !atc::valid_undo({{"kind", "move"}, {"label", "Move"}, {"items", QJsonArray{QJsonArray{"/a", "b"}}}}),
          "message fields are checked: wrong types and relative paths are refused, unknown fields are allowed");
    fake.call("UndoPush", R"({"kind":"wipe","label":"x","items":[["/a","/b"]]})");
    fake.call("UndoPush", R"({"kind":"move","label":"x","items":[["a","b"]]})");
    bool none = fake.call("UndoPop") == "";
    fake.call("UndoPush", R"({"kind":"move","label":"Move","items":[["/a","/b"]]})");
    check(none && QJsonDocument::fromJson(fake.call("UndoPop").toUtf8()).object().value("label").toString() == "Move",
          "the tower keeps only valid undo entries");
    FakeFlight other;
    other.call("CheckIn", R"({"pid":2,"impl":"fake","protocol":999})");
    other.report({{"type", "starred"}});
    spin(500);
    bool from_other = false;
    for (const auto &[f, m] : fake.heard)
        from_other = from_other || f == other.name();
    check(!from_other, "a flight speaking another protocol version isn't passed on");

    // ---- the tower goes down: the Kestrel starts a new one and checks in again
    qint64 old = fake.tower_pid();
    ::kill(old, SIGKILL);
    check(wait_for([&]() { qint64 p = fake.tower_pid(); return p && p != old; }, 8000), "A crashed tower is replaced");
    check(wait_for([&]() { return fake.flights_have(QCoreApplication::applicationPid(), "c++"); }),
          "...and Kestrel checks in again");
    finish();
}
