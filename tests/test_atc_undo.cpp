// Undo per Kestrel (the default) and shared undo between all Kestrels (Preferences → Share undo…).
#include "common.h"

using namespace test;

static QString P(const QString &n) { return home_path(n); }

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    setup_app();
    QObject::connect(atc::radio(), &atc::Radio::heard, qApp, on_atc);
    atc::radio()->start();
    MainWindow *w = open_window({HOME()});
    FakeFlight fake;
    check(wait_for([]() { return atc::radio()->tower_up(); }), "tower up");
    fake.check_in();
    auto push = [&](const QString &kind, const QString &label, const QJsonArray &items) {
        fake.call("UndoPush", QJsonDocument(QJsonObject{{"kind", kind}, {"label", label}, {"items", items}})
                                  .toJson(QJsonDocument::Compact));
    };
    write_text(P("a"), "a");
    write_text(P("d"), "d");

    // ---- default: off. Each Kestrel undoes only its own
    check(!settings().value("shared_undo", false).toBool(), "Share undo is off by default");
    util::rename(P("a"), P("b"));
    undo::record("rename", "Rename", {qMakePair(P("a"), P("b"))});
    push("rename", "Their Rename", QJsonArray{QJsonArray{P("c"), P("d")}});
    spin(300);
    check(undo::label() == "Rename", "off: Ctrl+Z offers our own action, not theirs (" + undo::label() + ")");
    undo::undo(w);
    check(wait_for([&]() { return exists(P("a")) && !exists(P("b")); }), "off: Ctrl+Z undoes our rename");
    check(exists(P("d")), "off: their action is left alone");
    check(undo::label().isEmpty(), "off: nothing left to undo here");

    // ---- on: one history for every Kestrel
    settings().setValue("shared_undo", true);
    settings().sync();
    fake.report({{"type", "settings"}});   // as if another window's Preferences turned it on
    spin(300);
    check(undo::label() == "Their Rename", "on: Ctrl+Z offers the newest action from any Kestrel (" + undo::label() + ")");
    undo::undo(w);
    check(wait_for([&]() { return exists(P("c")) && !exists(P("d")); }), "on: Ctrl+Z here undoes their rename");
    check(wait_for([&]() {
              QJsonObject m = fake.last_of("undo_changed");
              return m.contains("label") && m.value("label").toString().isEmpty();
          }),
          "the tower tells everyone the history is empty");
    check(fake.call("UndoPop").isEmpty(), "an undone action is handed out only once");

    makedirs(P("m1"), true);
    write_text(P("x"), "x");
    util::rename(P("x"), P("m1/x"));
    undo::record("move", "Move", {qMakePair(P("x"), P("m1/x"))});
    check(wait_for([&]() { return fake.last_of("undo_changed").value("label").toString() == "Move"; }),
          "on: our action goes to the shared history and every Kestrel hears about it");
    QString op = fake.call("UndoPop");
    QJsonObject o = QJsonDocument::fromJson(op.toUtf8()).object();
    check(o.value("kind") == "move" && o.value("items").toArray() == QJsonArray{QJsonArray{P("x"), P("m1/x")}},
          "...in the shared format");
    fake.call("UndoPush", op.toUtf8());   // put it back
    spin(200);
    undo::undo(w);
    check(wait_for([&]() { return exists(P("x")) && !exists(P("m1/x")); }), "on: and Ctrl+Z undoes it");

    write_text(P("t"), "t");
    undo::record_paths("trash", "Move to Trash", {P("t")});
    op = fake.call("UndoPop");
    check(QJsonDocument::fromJson(op.toUtf8()).object().value("items").toArray() == QJsonArray{QJsonArray{P("t"), ""}},
          "trash/create items are sent as [path, \"\"] (as the Python version expects)");
    makedirs(P("newdir"), true);
    push("create", "New Folder", QJsonArray{QJsonArray{P("newdir"), ""}});
    spin(300);
    undo::undo(w);
    check(wait_for([&]() { return !exists(P("newdir")); }), "on: undoing another Kestrel's New Folder moves it to the trash");

    // ---- no tower: falls back to this Kestrel's own history
    push("rename", "Stale", QJsonArray{QJsonArray{P("q"), P("r")}});
    spin(200);
    ::kill(fake.tower_pid(), SIGKILL);
    check(wait_for([&]() { return !atc::radio()->tower_up(); }, 3000), "tower down");
    write_text(P("e"), "e");
    util::rename(P("e"), P("f"));
    undo::record("rename", "Rename", {qMakePair(P("e"), P("f"))});
    check(undo::label() == "Rename", "no tower: the action is kept here");
    check(wait_for([&]() { return atc::radio()->tower_up(); }, 8000), "a new tower is started");
    spin(300);
    check(undo::label() == "Rename", "the new tower's empty history doesn't hide ours (" + undo::label() + ")");
    undo::undo(w);
    check(wait_for([&]() { return exists(P("e")); }), "...and Ctrl+Z still undoes it");
    finish();
}
