// Preferences → "Open folders from other apps as tabs": launched Kestrels hand their folders to an open window.
//
// This window is the open one; real `kes` programs (this project's build/kes) are launched against it.
#include "common.h"

using namespace test;

static QStringList tab_paths(MainWindow *w)
{
    QStringList out;
    for (Pane *p : w->panes())
        out << p->path;
    return out;
}

// What a launched Kestrel did: handed its folder over and exited successfully, opened a window of its own (it told
// the tower it has one: heard by `fake`), or failed (exited with an error or crashed, or did neither). Whichever comes
// first, however long the launch takes on this machine.
enum class Launch { HandedOff, OwnWindow, Failed };

static Launch launch(FakeFlight &fake, const QString &prog, const QStringList &args, const QStringList &extra_env = {})
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &kv : extra_env)
        env.insert(kv.section('=', 0, 0), kv.section('=', 1));
    p.setProcessEnvironment(env);
    p.setStandardOutputFile(QProcess::nullDevice());
    p.setStandardErrorFile(QProcess::nullDevice());
    qsizetype from = fake.heard.size();
    QString me = atc::radio()->flight();
    auto own_window = [&]() {
        for (qsizetype i = from; i < fake.heard.size(); ++i) {
            const auto &[f, m] = fake.heard[i];
            if (f != me && f != fake.name() && m.value("type").toString() == "windows" && m.value("count").toInt() > 0)
                return true;
        }
        return false;
    };
    p.start(prog, args);
    wait_for([&]() { return p.state() == QProcess::NotRunning || own_window(); }, 60000);
    if (p.state() != QProcess::NotRunning) {
        bool own = own_window();
        p.kill();
        p.waitForFinished(5000);
        return own ? Launch::OwnWindow : Launch::Failed;
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0 ? Launch::HandedOff : Launch::Failed;
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    auto P = [](const QString &n) { return home_path(n); };
    for (const QString &d : {"d1", "d2", "d3", "d4", "d5", "d6"})
        makedirs(P(d), true);
    write_text(P("d5/pick.txt"), "x");
    write_text(P("d6/item.txt"), "x");
    setup_app();
    QObject::connect(atc::radio(), &atc::Radio::heard, qApp, on_atc);
    atc::radio()->start();
    MainWindow *w = open_window({P("d1")});
    check(wait_for([]() { return atc::radio()->tower_up(); }), "tower up");
    wait_for([]() { return atc::radio()->caught_up(); });   // checked in: a launched Kestrel can hand over to it
    FakeFlight fake;   // (only listens: what the launched Kestrels tell the tower)
    QString CXX = qEnvironmentVariable("KES_CXX");

    check(!settings().value("open_in_tabs", false).toBool(), "the switch is off by default");
    check(launch(fake, CXX, {P("d2")}) == Launch::OwnWindow, "off: a launched Kestrel keeps its own window");
    check(w->tabs->count() == 1, "off: no tab added here");

    settings().setValue("open_in_tabs", true);
    settings().sync();
    check(launch(fake, CXX, {P("d2")}) == Launch::HandedOff, "on: a launched Kestrel hands its folder over and exits");
    check(wait_for([&]() { return tab_paths(w).contains(P("d2")); }), "on: ...and it opens here as a tab");
    check(w->pane() && w->pane()->path == P("d2"), "on: ...which becomes the current tab");
    check(launch(fake, CXX, {P("d3")}) == Launch::HandedOff, "on: a second launched Kestrel hands its folder over too");
    check(wait_for([&]() { return tab_paths(w).contains(P("d3")) && tab_paths(w).contains(P("d2")); }),
          "on: ...and it opens here as another tab");
    check(launch(fake, CXX, {P("d4")}, {"CONDA_DEFAULT_ENV=myenv"}) == Launch::OwnWindow, "on: not from an activated conda environment");
    check(!tab_paths(w).contains(P("d4")), "on: ...so no tab for that one");
    check(launch(fake, CXX, {}) == Launch::OwnWindow, "on: launching Kestrel without a folder still opens a new window");

    auto selected = [&]() {
        QStringList sel;
        for (const QModelIndex &i : w->pane()->view()->selectionModel()->selectedIndexes())
            sel << i.data(Qt::DisplayRole).toString();
        return sel;
    };
    check(atc::hand_off({P("d5")}, {P("d5/pick.txt")}) == atc::radio()->flight(),
          "a handoff with a selection is routed to the Kestrel with a window");
    check(wait_for([&]() { return tab_paths(w).contains(P("d5")); }), "...which opens the folder as a tab");
    check(wait_for([&]() { return selected().contains("pick.txt"); }, 3000), "...with the item selected");

    int n = w->tabs->count();
    handle_fm1("ShowItems", {file_uri(P("d6/item.txt"))}, "");
    check(WINDOWS.size() == 1 && w->tabs->count() == n + 1 && w->pane()->path == P("d6"),
          "on: Show in folder opens a tab in this window");
    check(wait_for([&]() { return selected().contains("item.txt"); }, 3000), "on: ...with the item selected");

    settings().setValue("open_in_tabs", false);
    settings().sync();
    handle_fm1("ShowFolders", {file_uri(P("d6"))}, "");
    check(WINDOWS.size() == 2, "off: Show in folder opens a new window again");
    finish();
}
