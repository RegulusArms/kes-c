// The shared task list: every window's status bar shows the operations running in other windows and Kestrels.
#include "common.h"

using namespace test;

static QString label_of(MainWindow *w) { return w->task_panel->findChild<QLabel *>()->text(); }

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    setup_app();

    // a tower and another Kestrel with a running (admin) task are already there
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {"--atc"});
    FakeFlight fake;
    check(wait_for([&]() { return fake.tower_pid() != 0; }), "tower up");
    fake.check_in();
    QJsonObject theirs{{"id", "9"}, {"title", "Copying"}, {"text", "3 of 7"}, {"fraction", 0.5}, {"cancellable", true},
                       {"cancelling", false}, {"admin", true}};
    fake.report({{"type", "tasks"}, {"keep", true}, {"tasks", QJsonArray{theirs}}});

    QObject::connect(atc::radio(), &atc::Radio::heard, qApp, on_atc);
    atc::radio()->start();
    MainWindow *w = open_window({HOME()});
    check(wait_for([&]() { return label_of(w).contains("Copying (in another window): 3 of 7"); }),
          "a new window shows another Kestrel's running task");
    QString me = atc::radio()->flight();
    check(label_of(w).contains("🛡"), "...marked as an admin task");
    check(w->task_panel->isVisible() && !label_of(w).contains("more"), "...on its own: " + label_of(w));

    w->task_panel->findChild<QToolButton *>()->click();
    check(wait_for([&]() {
              for (const auto &[f, m] : fake.heard)
                  if (f == me && m.value("type") == "cancel" && m.value("flight") == fake.name() && m.value("task") == "9")
                      return true;
              return false;
          }),
          "✕ asks the other Kestrel to cancel it");
    check(label_of(w).contains("cancelling"), "...and shows it cancelling");

    bool cancelled = false;
    Task *job = fileops::run_job(w, "Test job", [](Task *t) -> QVariant {
        for (int i = 0;; ++i) {
            t->report(i % 1000, 1000, "working");
            t->check();
            QThread::msleep(5);
        }
    });
    QObject::connect(job, &Task::result, [&]() { cancelled = job->was_cancelled; });
    QString jid = job->id;
    check(wait_for([&]() {
              QJsonObject t = fake.last_of("tasks");
              return fake.heard.last().first == me && t.value("keep").toBool() &&
                     t.value("tasks").toArray().first().toObject().value("title") == "Test job";
          }),
          "a task here is reported (kept state)");
    check(wait_for([&]() { return label_of(w).startsWith("Test job") && label_of(w).contains("+1 in other windows"); }),
          "this window's task comes first, the other is counted: " + label_of(w));
    int before = fake.count_type("tasks", me);
    spin(2000);
    int n = fake.count_type("tasks", me) - before;
    check(n >= 2 && n <= 6, QString("progress reports are throttled (%1 in 2 s)").arg(n));

    MainWindow *w2 = open_window({HOME()});
    check(wait_for([&]() {
              return label_of(w2).contains("Test job (in another window)") && label_of(w2).contains("+1 in other windows");
          }),
          "a second window shows the first window's task");

    fake.report({{"type", "cancel"}, {"flight", me}, {"task", jid}});
    check(wait_for([&]() { return cancelled; }), "another Kestrel's ✕ cancels our task");
    check(wait_for([&]() {
              return fake.last_of("tasks").value("tasks").toArray().isEmpty() && fake.heard.last().first == me;
          }),
          "...and the empty list is reported");
    check(wait_for([&]() { return label_of(w).startsWith("🛡 Copying (in another window)"); }), "back to showing only theirs");

    fake.disconnect_bus();
    check(wait_for([&]() { return !w->task_panel->isVisible() && !w2->task_panel->isVisible(); }),
          "a Kestrel that leaves takes its tasks with it");

    // tower restart: our kept state is sent to the new tower
    FakeFlight fake2;
    Task *job2 = fileops::run_job(w, "Second job", [](Task *t) -> QVariant {
        for (;;) {
            t->check();
            QThread::msleep(20);
        }
    });
    job2->admin = true;
    spin(300);
    ::kill(fake2.tower_pid(), SIGKILL);
    spin(300);
    check(wait_for([&]() {
              QString k = fake2.call("Kept");
              return k.contains("Second job") && k.contains(R"("admin":true)");
          }, 8000),
          "a new tower gets our running tasks again");
    job2->cancel();
    spin(300);
    finish();
}
