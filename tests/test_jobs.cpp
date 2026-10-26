// The job queue (jobs.h): one queue for every Kestrel, by drive or one at a time; pausing, cancelling, starting now and
// reordering jobs; their steps and logs; the Jobs window; and the tower's part (lanes across Kestrels, the Tower window).
#include "common.h"

#include "drives.h"
#include "jobs.h"
#include "jobswin.h"
#include "towerwin.h"
#include "windiag.h"

#include <QClipboard>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QMouseEvent>
#include <QTreeWidget>

#include <thread>

using namespace test;

// a job that holds its lanes until told to stop
static Task *holder(MainWindow *w, const QString &title, const QString &path, std::shared_ptr<std::atomic<bool>> held)
{
    return fileops::queue_job(w, title, {path}, [held](Task *t) {
        while (*held) {
            t->check();
            QThread::msleep(20);
        }
        return QVariant();
    });
}

static char proc_state(qint64 pid)
{
    QByteArray stat = read_file(QString("/proc/%1/stat").arg(pid));
    int close = stat.lastIndexOf(')');
    return close > 0 && close + 2 < stat.size() ? stat[close + 2] : '?';
}

static QJsonObject json(const QString &s) { return QJsonDocument::fromJson(s.toUtf8()).object(); }

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    windiag::start();   // as kes_main does

    // ---- the scheduler
    using jobs::Entry;
    auto starts = [](const QList<Entry> &q, const QString &mode) { return jobs::to_start(q, mode).join(","); };
    check(starts({{"run", {"a", "b"}, true}, {"ac", {"a", "c"}}, {"de", {"d", "e"}}}, "drive") == "de",
          "by drive: a job waits while a running one uses one of its drives; one on other drives starts");
    check(starts({{"w1", {"a"}}, {"w2", {"a"}}}, "drive") == "w1", "...jobs on the same drive start in queue order");
    check(starts({{"run", {"a"}, true}, {"ab", {"a", "b"}}, {"b", {"b"}}}, "drive") == "",
          "...a later job doesn't overtake an earlier waiting one in its lane");
    check(starts({{"p", {"a"}, false, true}, {"w", {"a"}}}, "drive") == "w",
          "...a paused waiting job neither starts nor holds up the ones after it");
    check(starts({{"run", {"a"}, true}, {"f", {"a"}, false, false, true}}, "drive") == "f", "...a job started now starts anyway");
    check(starts({{"run", {"a"}, true}, {"b", {"b"}}}, "one") == "" && starts({{"x", {"a"}}, {"y", {"b"}}}, "one") == "x",
          "one at a time: nothing starts while a job runs, then only the first");
    check(starts({{"run", {"a"}, true}, {"w1", {"a"}}, {"p", {"a"}, false, true}, {"w2", {"a", "b"}}}, "all") == "w1,w2",
          "all at once: every waiting job starts, also on a drive in use, except a paused one");
    check(jobs::known_mode("all") == "all" && jobs::known_mode("one") == "one" && jobs::known_mode("rip") == "drive" &&
              jobs::known_mode(QString()) == "drive",
          "...the queue modes known are by drive, one at a time and all at once; anything else is by drive");

    // ---- drives: the physical disk, the same in every Kestrel
    QString tape = home_path("tape");
    makedirs(tape, true);
    qputenv("KESTREL_TEST_SLOW", ("tape=" + tape).toUtf8());
    QStringList home_ids = drives::drive_ids(home_path("x/y.txt")), tape_ids = drives::drive_ids(join(tape, "z"));
    check(!home_ids.isEmpty() && home_ids == drives::drive_ids(HOME()) && tape_ids == QStringList{"mount:fuse:ltfs:test"} &&
              home_ids != tape_ids,
          "paths on one drive share its id (also a file not made yet); a tape is a drive of its own (" + home_ids.join(",") + ")");
    QStringList many;   // a job's items: thousands on one drive, then one on the tape
    for (int i = 0; i < 2000; ++i)
        many << home_path("x/item" + QString::number(i));
    many << join(tape, "z") << home_path("y");
    check(drives::drive_ids(many) == home_ids + tape_ids && drives::drive_ids(QStringList{}).isEmpty(),
          "a job's items give each drive they're on once, in order (" + drives::drive_ids(many).join(",") + ")");
    QString part_src, disk;
    for (const drives::Mount &m : drives::mounts()) {
        QString name = basename(realpath(m.source));
        if (m.source.startsWith("/dev/") && exists("/sys/class/block/" + name + "/partition") && part_src.isEmpty()) {
            part_src = m.root;
            disk = basename(dirname(realpath("/sys/class/block/" + name)));
        }
    }
    if (part_src.isEmpty())
        skip("a partition counts as its whole disk (no partition mounted here)");
    else
        check(drives::drive_ids(part_src).contains("disk:" + disk), "a partition counts as its whole disk (" + part_src +
                                                                         " → disk:" + disk + ")");

    setup_app();
    MainWindow *w = open_window({HOME()});
    jobs::Queue *q = jobs::Queue::instance();

    // ---- the queue in this Kestrel (no tower yet)
    write_text(home_path("a.txt"), "a");
    auto held = std::make_shared<std::atomic<bool>>(true);
    QPointer<Task> h = holder(w, "Holding the home drive", HOME(), held);
    spin(150);
    QPointer<Task> copy = fileops::start_ops(w, {{"copy", home_path("a.txt"), home_path("a2.txt")}}, "Copy on the same drive");
    QPointer<Task> other = holder(w, "On the tape", tape, std::make_shared<std::atomic<bool>>(false));
    spin(600);
    check(h && !h->queued && copy && copy->queued && !exists(home_path("a2.txt")) && copy->text.contains("Waiting"),
          "a job on a drive another job is using waits in the queue, saying so");
    check(wait_for([&]() { return !other; }), "...one on another drive runs meanwhile");
    *held = false;
    check(wait_for([&]() { return !copy && exists(home_path("a2.txt")); }), "...and the waiting one runs once the drive is free");

    // one at a time
    jobs::set_mode("one");
    *held = true;
    h = holder(w, "Holding again", HOME(), held);
    spin(150);
    other = holder(w, "On the tape, one at a time", tape, std::make_shared<std::atomic<bool>>(false));
    spin(500);
    bool waited_one = other && other->queued;
    *held = false;
    check(waited_one && wait_for([&]() { return !other && !h; }), "one at a time: a job on another drive waits too");

    // all at once
    jobs::set_mode("all");
    check(jobs::mode() == "all" && settings().value("jobs_mode").toString() == "all",
          "all at once is kept as the queue's mode");
    *held = true;
    h = holder(w, "Holding, all at once", HOME(), held);
    spin(150);
    QPointer<Task> same = fileops::start_ops(w, {{"copy", home_path("a.txt"), home_path("a-rip.txt")}}, "Copy, all at once");
    bool rip = wait_for([&]() { return !same && exists(home_path("a-rip.txt")); }) && h && !h->queued;
    *held = false;
    check(rip && wait_for([&]() { return !h; }),
          "all at once: a job on a drive another job is using starts at once instead of waiting");
    jobs::set_mode("rip");
    check(jobs::mode() == "drive", "...and an unknown mode is stored as by drive");
    jobs::set_mode("drive");

    // cancel, start now, reorder, start paused
    *held = true;
    h = holder(w, "Holding", HOME(), held);
    spin(150);
    auto ran = std::make_shared<std::atomic<bool>>(false);
    QPointer<Task> cancelled = fileops::queue_job(w, "Never runs", {HOME()}, [ran](Task *) {
        *ran = true;
        return QVariant();
    });
    spin(200);
    q->control(QString(), cancelled->id, "cancel");
    check(wait_for([&]() { return !cancelled; }) && !*ran, "a waiting job cancelled ends without running");
    auto order = std::make_shared<QStringList>();
    auto job = [&](const QString &name) {
        return fileops::queue_job(w, name, {HOME()}, [order, name](Task *) {
            *order << name;
            return QVariant();
        });
    };
    QPointer<Task> first = job("first"), second = job("second");
    spin(200);
    q->control(QString(), second->id, "up");
    QPointer<Task> now = job("now");
    spin(200);
    q->control(QString(), now->id, "start_now");
    check(wait_for([&]() { return !now; }) && order->value(0) == "now", "Start Now runs a waiting job at once");
    *held = false;
    check(wait_for([&]() { return !first && !second; }) && order->mid(1) == QStringList({"second", "first"}),
          "moving a waiting job up runs it before the one ahead of it (" + order->join(", ") + ")");
    q->set_start_paused(true);
    QPointer<Task> later = job("later");
    spin(400);
    bool still = later && later->queued;
    q->set_start_paused(false);
    q->control(QString(), later->id, "resume");
    check(still && wait_for([&]() { return !later; }), "with Start New Jobs Paused, a job waits until resumed, even with "
                                                       "its drive free");

    // pausing: Kestrel's own copy (between chunks) and a program it runs (SIGSTOP)
    {
        QString big = home_path("big.bin"), out = home_path("pause-out");
        makedirs(out, true);
        write_text(big, "b");
        ::truncate(enc(big).constData(), qint64(512) << 20);
        QPointer<Task> t = fileops::start_ops(w, {{"copy", big, join(out, "big.bin")}}, "Big copy");
        auto once = std::make_shared<std::atomic<bool>>(false);   // paused once, part-way through the file
        QObject::connect(t, &Task::progress, t, [t, once](double f, const QString &) {
            if (f > 0 && !once->exchange(true))
                t->set_paused(true);
        }, Qt::DirectConnection);
        auto part_size = [&]() {
            qint64 n = -1;
            for (const QString &f : listdir(out))
                if (f.startsWith(".kes-"))
                    n = getsize(join(out, f));
            return n;
        };
        wait_for([&]() { return t && t->paused(); });
        spin(300);
        qint64 a = part_size();
        spin(700);
        qint64 b = part_size();
        bool held_still = t && t->paused() && a > 0 && a == b && a < (qint64(512) << 20);
        if (t)
            t->set_paused(false);
        check(held_still && wait_for([&]() { return !t; }, 20000) && exists(join(out, "big.bin")) &&
                  getsize(join(out, "big.bin")) == (qint64(512) << 20),
              QString("a paused copy stops between chunks, its part file kept (%1 bytes), and finishes when resumed").arg(a));
        auto pid = std::make_shared<std::atomic<qint64>>(0);
        QPointer<Task> tool = fileops::queue_job(w, "A program", {home_path("tool")}, [pid](Task *t) {
            auto p = proc::spawn({"sleep", "60"});
            *pid = p->pid;
            while (!p->poll()) {
                t->check();
                QThread::msleep(20);
            }
            return QVariant();
        });
        wait_for([&]() { return *pid > 0; });
        tool->set_paused(true);
        bool stopped = wait_for([&]() { return proc_state(*pid) == 'T'; }, 3000);
        tool->set_paused(false);
        bool going = wait_for([&]() { return proc_state(*pid) != 'T'; }, 3000);
        tool->cancel();
        check(stopped && going && wait_for([&]() { return !tool; }) && wait_for([&]() { return proc_state(*pid) == '?'; }),
              "pausing a job pauses the program it runs (stopped), resuming continues it, and cancelling ends both");
    }

    // ---- steps and the log, kept after the job ends
    makedirs(home_path("steps"), true);
    for (const QString &n : {"s1.txt", "s2.txt", "s3.txt"})
        write_text(home_path("steps/" + n), "s");
    makedirs(home_path("steps-out"), true);
    QPointer<Task> st = fileops::start_ops(w, {{"copy", home_path("steps/s1.txt"), home_path("steps-out/s1.txt")},
                                               {"copy", home_path("steps/s2.txt"), home_path("steps-out/s2.txt")},
                                               {"copy", home_path("steps/s3.txt"), home_path("steps-out/s3.txt")}},
                                           "Three files");
    check(st && st->steps.size() == 3 && st->steps[0].startsWith("Copy s1.txt → "), "a copy lists its steps, one per item");
    wait_for([&]() { return !st; });
    const QList<FinishedTask> &done = TaskBoard::instance()->finished;
    check(!done.isEmpty() && done.last().title == "Three files" && done.last().state == "done" &&
              done.last().log.contains("Copy s2.txt → ") && done.last().log.contains("done") &&
              done.last().log.contains("Finished."),
          "a finished job is kept with its log: each step, how it went, and the end");

    // ---- a paste queued behind another job: its title and log say what it is (what started it, in which window,
    // from where to where, what it waited for); its destination moved meanwhile, it fails once, clearly
    {
        makedirs(home_path("pq-dest"), true);
        makedirs(home_path("pq-elsewhere"), true);
        write_text(home_path("pq1.txt"), "1");
        write_text(home_path("pq2.txt"), "2");
        auto *md = new QMimeData;
        md->setData("x-special/gnome-copied-files",
                    ("copy\n" + file_uri(home_path("pq1.txt")) + "\n" + file_uri(home_path("pq2.txt"))).toUtf8());
        QGuiApplication::clipboard()->setMimeData(md);
        *held = true;
        h = holder(w, "Holding the drive", HOME(), held);
        spin(150);
        w->paste(home_path("pq-dest"));   // Paste Into Folder on it
        QPointer<Task> pq;
        wait_for([&]() {
            for (Task *t : w->task_panel->tasks)
                if (t->title == "Copying 2 items to pq-dest")
                    pq = t;
            return pq;
        });
        check(pq && pq->queued, "a copy's title names how much it copies and where to (" +
                                    (pq ? pq->title : QString("no such job")) + ")");
        check(w->read_clipboard().second.isEmpty(), "copied files are forgotten once pasted (no second paste of them by "
                                                    "mistake)");
        ::rename(enc(home_path("pq-dest")).constData(), enc(home_path("pq-elsewhere/pq-dest")).constData());
        spin(1100);   // a wait the log mentions (from 1 s)
        *held = false;
        check(wait_for([&]() { return !pq; }), "...it runs once the drive is free");
        wait_for([]() { return shown_window<QMessageBox>() != nullptr; }, 3000);
        spin(200);   // (a message for each item would come together)
        QStringList shown;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (auto *box = qobject_cast<QMessageBox *>(top); box && box->isVisible()) {
                if (box->windowTitle().startsWith("Copying"))
                    shown << box->text();
                box->close();
            }
        const FinishedTask &f = done.last();
        QString msg = shown.value(0);
        check(shown.size() == 1 && msg.contains("“" + home_path("pq-dest") + "”, isn't there any more") &&
                  msg.contains("while this job waited") && !msg.contains("No such file") && f.state == "failed" &&
                  !exists(home_path("pq-elsewhere/pq-dest/pq1.txt")),
              "a copy whose destination was moved while it waited fails once, saying so, and copies nothing (" +
                  shown.join(" | ") + ")");
        check(f.log.contains("Copy of 2 items, started by Paste Into Folder in the window “" + w->windowTitle() + "” at ") &&
                  f.log.contains("From: " + HOME()) && f.log.contains("To: " + home_path("pq-dest")) &&
                  f.log.contains(QRegularExpression("Waited [12] s for: Holding the drive")) &&
                  f.log.indexOf("From: ") < f.log.indexOf("Waited"),
              "its log starts with what started it, in which window and when, from where to where, and how long it "
              "waited for which job:\n" + f.log.left(400));
    }

    // ---- the Jobs window: opened from the status bar's job panel, every job, its log, Delete cancels
    *held = true;
    h = holder(w, "Holding for the window", HOME(), held);
    spin(150);
    QPointer<Task> waiting = fileops::start_ops(w, {{"copy", home_path("a.txt"), home_path("a3.txt")}}, "Waiting copy");
    spin(200);
    QMouseEvent click(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w->task_panel, &click);
    JobsWindow *jw = nullptr;
    wait_for([&]() { return (jw = shown_window<JobsWindow>()) != nullptr; });
    QStringList rows;
    QTreeWidgetItem *wait_row = nullptr;
    if (jw) {
        jw->refresh();
        for (int i = 0; i < jw->list->topLevelItemCount(); ++i) {
            QTreeWidgetItem *it = jw->list->topLevelItem(i);
            rows << it->text(0) + ":" + it->text(1);
            if (it->text(0) == "Waiting copy")
                wait_row = it;
        }
    }
    check(jw && rows.contains("Holding for the window:Running") && rows.contains("Waiting copy:Waiting") &&
              rows.join("|").contains("Three files:Done"),
          "clicking the job panel opens the Jobs window, listing running, waiting and finished jobs (" + rows.join(" | ") + ")");
    if (jw && wait_row) {
        w->pane()->select_paths({home_path("a.txt")});   // must stay: Delete here isn't the main window's Move to Trash
        jw->list->setCurrentItem(wait_row);
        QApplication::setActiveWindow(jw);   // as clicking in it does: the main window's own Delete (Move to Trash)
        jw->list->setFocus();                // is a shortcut of that window only
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(jw->list, &del);
    }
    check(wait_for([&]() { return !waiting; }) && !exists(home_path("a3.txt")) && exists(home_path("a.txt")),
          "Delete in the Jobs window cancels the selected job (and doesn't move the main window's selection to the trash)");
    *held = false;
    wait_for([&]() { return !h; });
    if (jw)
        jw->close();

    // ---- through the tower: one queue for every Kestrel
    QObject::connect(atc::radio(), &atc::Radio::heard, qApp, on_atc);
    atc::radio()->start();
    FakeFlight fake;
    wait_for([&]() { return fake.tower_pid() != 0 && atc::radio()->tower_up(); }, 8000);
    fake.check_in();
    spin(300);
    QString me = atc::radio()->flight(), drive = home_ids.value(0);
    auto ask = [&](FakeFlight &f, const QString &id) {
        f.call("JobAsk", QJsonDocument(QJsonObject{{"id", id}, {"title", "Fake " + id}, {"drives", QJsonArray{drive}}})
                             .toJson(QJsonDocument::Compact));
    };
    auto lane_opened = [&](FakeFlight &f, const QString &id) {
        for (const auto &[from, m] : f.heard)
            if (m.value("type").toString() == "lane_open" && m.value("flight").toString() == f.name() &&
                m.value("task").toString() == id)
                return true;
        return false;
    };
    ask(fake, "f1");
    check(wait_for([&]() { return lane_opened(fake, "f1"); }), "the tower opens a lane for another Kestrel's job on a free drive");
    QPointer<Task> mine = fileops::start_ops(w, {{"copy", home_path("a.txt"), home_path("a4.txt")}}, "My copy");
    spin(800);
    bool waits = mine && mine->queued && !exists(home_path("a4.txt"));
    bool listed = false;
    for (const jobs::Job &j : q->jobs())
        listed = listed || (j.flight == fake.name() && j.id == "f1" && j.state == "running");
    fake.call("JobDone", QJsonDocument(QJsonObject{{"id", "f1"}}).toJson(QJsonDocument::Compact));
    check(waits && listed && wait_for([&]() { return !mine && exists(home_path("a4.txt")); }),
          "a job waits while another Kestrel's job uses its drive (both in one queue), and runs when that one is done");
    {
        FakeFlight leaving;
        leaving.check_in();
        ask(leaving, "f2");
        wait_for([&]() { return lane_opened(leaving, "f2"); });
        mine = fileops::start_ops(w, {{"copy", home_path("a.txt"), home_path("a5.txt")}}, "After it leaves");
        spin(600);
        bool waits2 = mine && mine->queued;
        leaving.disconnect_bus();
        check(waits2 && wait_for([&]() { return !mine && exists(home_path("a5.txt")); }),
              "a Kestrel that leaves (or crashes) frees its lanes for the others' jobs");
    }
    *held = true;
    h = holder(w, "Controlled from elsewhere", home_path("ctl"), held);
    wait_for([&]() { return h && !h->queued; });
    fake.report({{"type", "job_control"}, {"flight", me}, {"task", h->id}, {"action", "pause"}});
    bool paused_remote = wait_for([&]() { return h && h->paused(); });
    fake.report({{"type", "job_control"}, {"flight", me}, {"task", h->id}, {"action", "resume"}});
    check(paused_remote && wait_for([&]() { return h && !h->paused(); }), "another Kestrel's Jobs window pauses and resumes our job");
    check(wait_for([&]() {
              QJsonObject qm = fake.last_of("queue");
              for (const QJsonValue &v : qm.value("jobs").toArray())
                  if (v.toObject().value("title").toString() == "Controlled from elsewhere")
                      return true;
              return false;
          }),
          "every Kestrel hears the queue, with each Kestrel's jobs");
    *held = false;
    wait_for([&]() { return !h; });

    // the tower's own methods, and the Tower window
    QJsonObject status = json(fake.call("Status"));
    int refused = status.value("refused").toInt();
    fake.report({{"type", "no_such_type"}});
    check(status.value("pid").toInteger() == fake.tower_pid() && status.value("protocol").toInt() == atc::PROTOCOL &&
              json(fake.call("Status")).value("refused").toInt() == refused + 1 && fake.call("UndoList").startsWith("["),
          "the tower tells its status (pid, protocol, refused messages) and its shared undo list");
    TowerWindow *tw = TowerWindow::open(w);
    tw->refresh();
    bool status_ok = tw->status->text().contains("The tower is running") &&
                     tw->status->text().contains(QString::number(fake.tower_pid()));
    atc::announce("starred");
    bool logged = wait_for([&]() {
        for (int i = 0; i < tw->messages->topLevelItemCount(); ++i)
            if (tw->messages->topLevelItem(i)->text(2) == "starred")
                return true;
        return false;
    });
    check(status_ok && logged, "the Tower window shows the tower's status and the messages it passes on, live");

    // its Windows tab: this Kestrel's windows as Qt sees them, and their recent changes (for a window that misbehaves)
    w->resize(1011, 707);
    spin(100);
    auto *wide = new QLabel("x", w);   // a child asking for more room than the window has
    wide->setObjectName("too_wide");
    wide->setMinimumSize(5000, 10);
    wide->show();
    tw->show_windows();
    QString rep = tw->windows->toPlainText();
    delete wide;
    check(rep.contains("== MainWindow") && rep.contains("resize 1011×707") && rep.contains("asks for minimum") &&
              rep.contains("border margins") && rep.contains("Screen ") && rep.contains("too_wide"),
          "the Tower's Windows tab shows each window's size, the minimum it asks for, its border and screen, its recent "
          "resizes, and children wanting more room than it has");
    QStringList before_saved = listdir(HOME());
    if (auto *save = tw->findChild<QPushButton *>("save_windows"))
        save->click();
    QString saved;
    for (const QString &n : listdir(HOME()))
        if (n.startsWith("kestrel-windows-") && !before_saved.contains(n))
            saved = join(HOME(), n);
    check(!saved.isEmpty() && QString::fromUtf8(read_file(saved)).contains("== MainWindow"),
          "...and saves it to a file in the home folder, to send");
    for (QWidget *x : QApplication::topLevelWidgets())
        if (auto *b = qobject_cast<QMessageBox *>(x); b && b->isVisible())
            b->close();
    tw->close();

    // ---- jobs belong to Kestrel, not to a window: closing the window that started one doesn't stop it (nor ask);
    // the last window asks; with every window closed Kestrel runs on until its jobs have ended
    {
        for (QWidget *x : QApplication::topLevelWidgets())
            if (x != w && x->isVisible() && x->isWindow())
                x->close();
        spin(100);
        MainWindow *w2 = open_window({HOME()});
        spin(200);
        auto hold = std::make_shared<std::atomic<bool>>(true);
        QPointer<Task> job = holder(w, "Keeps going", home_path("x"), hold);
        spin(200);
        QPointer<MainWindow> first = w;
        w->close();
        spin(300);
        bool asked = shown_window<QMessageBox>() != nullptr;
        check((!first || !first->isVisible()) && job && !job->isFinished() && !asked && w2->task_panel->tasks.contains(job.data()),
              "closing a window that started a job doesn't ask or stop it: it goes on, shown in another window's "
              "status bar");
        QPointer<MainWindow> last = w2;
        w2->close();
        QMessageBox *q = nullptr;
        wait_for([&]() { return (q = shown_window<QMessageBox>()) != nullptr; }, 2000);
        QStringList buttons;
        QAbstractButton *keep = nullptr;
        if (q)
            for (QAbstractButton *b : q->buttons()) {
                buttons << b->text();
                if (b->text() == "Exit and Open the Jobs Window")
                    keep = b;
            }
        check(last && last->isVisible() && q && q->text().contains("Keeps going") &&
                  buttons.contains("Stop All Jobs and Exit") && keep,
              "closing the last window while a job runs asks: exit and open the Jobs window, or stop all jobs (" +
                  buttons.join(", ") + ")");
        if (keep)
            keep->click();
        spin(300);
        check((!last || !last->isVisible()) && JobsWindow::is_open() && job && !job->isFinished() && !shown_window<JobsWindow>()->parentWidget(),
              "...Exit and Open the Jobs Window: the window closes, the Jobs window (a window of its own) opens, and the "
              "job goes on");
        QPointer<Task> quiet = fileops::queue_job(nullptr, "quiet", {}, [](Task *) { return QVariant(); }, nullptr, true,
                                                  {}, true);
        check(job && job->keeps_running && (!quiet || !quiet->keeps_running),
              "a job the user sees keeps Kestrel running; a quiet helper task doesn't");
        // the Jobs window closed too: Kestrel quits only once the job has ended
        QElapsedTimer since;
        since.start();
        qint64 job_ended = -1, quit_at = -1;
        QTimer::singleShot(300, []() {
            if (JobsWindow *jw = shown_window<JobsWindow>())
                jw->close();
        });
        QTimer::singleShot(1500, [&]() { *hold = false; });
        QObject::connect(job.data(), &QThread::finished, [&]() { job_ended = since.elapsed(); });
        QTimer::singleShot(15000, []() { QApplication::exit(9); });   // (a safety net)
        int rc = app.exec();
        quit_at = since.elapsed();
        check(rc == 0 && job_ended >= 1400 && quit_at >= job_ended,
              QString("with every window closed, Kestrel goes on until its jobs have ended, then quits (job ended "
                      "at %1 ms, quit at %2 ms)").arg(job_ended).arg(quit_at));
    }

    finish();
}
