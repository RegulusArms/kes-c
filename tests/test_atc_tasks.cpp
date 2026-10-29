// The shared task list: every window's status bar shows the operations running in other windows and Kestrels.
#include "common.h"

#include "jobswin.h"

#include <QListWidget>
#include <QPlainTextEdit>
#include <QTreeWidget>

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

    // ---- one Jobs window for every Kestrel: it shows another Kestrel's jobs whole (running and finished: their steps
    // and logs, asked for), its thumbnail jobs, and controls them; opened in another Kestrel, ours closes
    fake2.check_in();
    me = atc::radio()->flight();
    QJsonObject running{{"id", "41"}, {"title", "Copying 3 items to Backup"}, {"text", "1 of 3"}, {"fraction", 0.3},
                        {"cancellable", true}, {"state", "running"}, {"elapsed", 65000.0}};
    fake2.report({{"type", "tasks"}, {"keep", true}, {"tasks", QJsonArray{running}}});
    fake2.report({{"type", "finished"}, {"keep", true},
                  {"jobs", QJsonArray{QJsonObject{{"id", "40"}, {"title", "Extracting old.zip"}, {"state", "done with errors"},
                                                  {"took", 4000.0}, {"ended", double(QDateTime::currentMSecsSinceEpoch())}}}}});
    fake2.report({{"type", "thumbs"}, {"keep", true},
                  {"jobs", QJsonArray{QJsonObject{{"id", "7"}, {"title", "Thumbnails: Pictures"}, {"folder", "/tmp/p"},
                                                  {"window", "Pictures"}, {"state", "Running"}, {"done", 5}, {"total", 9}}}}});
    JobsWindow *jw = JobsWindow::open(w);
    check(wait_for([&]() { return fake2.heard_type("jobs_window", me); }), "opening the Jobs window tells the other Kestrels");
    auto row_of = [&](const QString &title) -> QTreeWidgetItem * {
        for (int i = 0; i < jw->list->topLevelItemCount(); ++i)
            if (jw->list->topLevelItem(i)->text(0).contains(title))
                return jw->list->topLevelItem(i);
        return nullptr;
    };
    check(wait_for([&]() { return row_of("Copying 3 items to Backup") && row_of("Extracting old.zip"); }),
          "it lists another Kestrel's running job and its finished one");
    check(row_of("Copying 3 items to Backup") && row_of("Copying 3 items to Backup")->text(3).startsWith("1:05"),
          "...with how long the running one has taken (" +
              (row_of("Copying 3 items to Backup") ? row_of("Copying 3 items to Backup")->text(3) : QString()) + ")");
    auto log_box = jw->findChild<QPlainTextEdit *>();
    jw->list->setCurrentItem(row_of("Copying 3 items to Backup"));
    check(wait_for([&]() {
              for (const auto &[f, m] : fake2.heard)
                  if (f == me && m.value("type") == "job_detail_ask" && m.value("flight") == fake2.name() && m.value("task") == "41")
                      return true;
              return false;
          }),
          "choosing another Kestrel's job asks it for the job's steps and log");
    fake2.report({{"type", "job_detail"}, {"to", me}, {"task", "41"}, {"steps", QJsonArray{"Copy a.txt", "Copy b.txt", "Copy c.txt"}},
                  {"steps_from", 0}, {"steps_total", 3}, {"step", 1}, {"log", "Copy a.txt\n  done\nCopy b.txt"}});
    auto steps_box = jw->findChild<QListWidget *>();
    check(wait_for([&]() {
              return log_box && log_box->toPlainText().contains("Copy b.txt") && steps_box && steps_box->count() == 3 &&
                     steps_box->item(0)->text() == "✔ Copy a.txt" && steps_box->item(1)->text() == "▶ Copy b.txt";
          }),
          "...and shows them, where it is in its steps too");
    jw->pause_btn->click();
    check(wait_for([&]() {
              for (const auto &[f, m] : fake2.heard)
                  if (f == me && m.value("type") == "job_control" && m.value("flight") == fake2.name() &&
                      m.value("task") == "41" && m.value("action") == "pause")
                      return true;
              return false;
          }),
          "Pause on another Kestrel's job (outside the queue) asks it to pause it");
    jw->list->setCurrentItem(row_of("Extracting old.zip"));
    fake2.report({{"type", "job_detail"}, {"to", me}, {"task", "40"}, {"steps", QJsonArray{"Extract old.zip"}},
                  {"steps_from", 0}, {"steps_total", 1}, {"step", 1}, {"log", "old.zip  failed: damaged"}});
    check(wait_for([&]() { return log_box && log_box->toPlainText().contains("old.zip  failed: damaged"); }),
          "a finished job of another Kestrel shows its log too");
    // its thumbnail jobs, and Stop on one
    QTreeWidgetItem *thumb = nullptr;
    wait_for([&]() {
        for (int i = 0; i < jw->thumb_list->topLevelItemCount(); ++i)
            if (jw->thumb_list->topLevelItem(i)->text(0).startsWith("Thumbnails: Pictures, 5 of 9"))
                thumb = jw->thumb_list->topLevelItem(i);
        return thumb != nullptr;
    });
    check(thumb && thumb->text(1) == "Pictures", "another Kestrel's thumbnail jobs are listed too");
    if (thumb) {
        jw->thumb_list->setCurrentItem(thumb);
        jw->thumb_stop->click();
    }
    check(wait_for([&]() {
              for (const auto &[f, m] : fake2.heard)
                  if (f == me && m.value("type") == "thumb_control" && m.value("flight") == fake2.name() &&
                      m.value("id") == "7" && m.value("action") == "stop")
                      return true;
              return false;
          }),
          "...and Stop on one asks that Kestrel to stop it");
    // ours, for the Jobs window in another Kestrel: our finished jobs, and a job's steps and log when it asks
    Task *done_job = fileops::run_job(w, "Our finished job", [](Task *t) -> QVariant {
        t->steps = {"Step one", "Step two"};
        t->log("our log line");
        return QVariant(true);
    });
    QString done_id = done_job->id;
    check(wait_for([&]() {
              for (const QJsonValue &v : fake2.last_of("finished").value("jobs").toArray())
                  if (v.toObject().value("title") == "Our finished job" && v.toObject().value("id") == done_id)
                      return true;
              return false;
          }),
          "our finished jobs are told to the other Kestrels");
    fake2.report({{"type", "job_detail_ask"}, {"flight", me}, {"task", done_id}});
    check(wait_for([&]() {
              QJsonObject d = fake2.last_of("job_detail");
              return d.value("to") == fake2.name() && d.value("task") == done_id && d.value("log").toString().contains("our log line") &&
                     d.value("steps").toArray().size() == 2 && d.value("steps_total") == 2;
          }),
          "...and asked for one's steps and log, we answer the Kestrel that asked");
    // Clear Finished clears every Kestrel's; another's clearing clears ours
    TaskBoard::instance()->clear_finished();
    check(wait_for([&]() { return fake2.heard_type("clear_finished", me) && !row_of("Extracting old.zip") && !row_of("Our finished job"); }),
          "Clear Finished forgets every Kestrel's finished jobs, and tells the others to");
    fileops::run_job(w, "Another finished job", [](Task *) -> QVariant { return QVariant(true); });
    wait_for([&]() { return row_of("Another finished job") != nullptr; });
    fake2.report({{"type", "clear_finished"}});
    check(wait_for([&]() { return TaskBoard::instance()->finished.isEmpty() && !row_of("Another finished job"); }),
          "...and another Kestrel's Clear Finished clears ours");
    // another Kestrel's Pause on one of ours outside the queue pauses it, and its Resume resumes it
    Task *ours = fileops::run_job(w, "Our long job", [](Task *t) -> QVariant {
        for (;;) {
            t->check();
            QThread::msleep(10);
        }
    });
    wait_for([&]() { return ours->elapsed_ms() > 0; });
    fake2.report({{"type", "job_control"}, {"flight", me}, {"task", ours->id}, {"action", "pause"}});
    check(wait_for([&]() { return ours->paused(); }), "another Kestrel's Pause pauses our job that isn't in the queue");
    fake2.report({{"type", "job_control"}, {"flight", me}, {"task", ours->id}, {"action", "resume"}});
    check(wait_for([&]() { return !ours->paused(); }), "...and its Resume resumes it");
    ours->cancel();
    // opened in another Kestrel: ours closes (it's the one window now)
    fake2.report({{"type", "jobs_window"}});
    check(wait_for([&]() { return !JobsWindow::is_open(); }), "the Jobs window opened in another Kestrel closes ours");
    finish();
}
