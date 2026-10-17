// Slow drives (src/drives.h): tapes (LTFS) and floppy disks. Folders marked as such with KESTREL_TEST_SLOW stand in for
// them, and KESTREL_TEST_STATFS_DELAY_MS makes their statfs wait, as a tape busy with another program's reads does.
#include "common.h"

#include "drives.h"
#include "groups.h"
#include "hashcheck.h"

#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>

#include <sys/stat.h>
#include <sys/xattr.h>

#include <thread>

using namespace test;

static bool set_attr(const QString &path, const char *name, const QString &value)
{
    QByteArray v = value.toUtf8();
    return ::setxattr(enc(path).constData(), name, v.constData(), size_t(v.size()), 0) == 0;
}

// the longest the window went without answering (a 20 ms timer's longest gap, in ms) while `work` ran and for `ms`
// after: a wait on a slow drive's statfs shows as a gap as long as that wait, however slow the machine is otherwise
static qint64 longest_pause(const std::function<void()> &work, int ms)
{
    QElapsedTimer clock;
    clock.start();
    qint64 last = 0, longest = 0;
    QTimer t;
    QObject::connect(&t, &QTimer::timeout, [&]() {
        longest = std::max(longest, clock.elapsed() - last);
        last = clock.elapsed();
    });
    t.start(20);
    work();
    spin(ms);
    return std::max(longest, clock.elapsed() - last);
}

static QStringList labels_in(QWidget *w)
{
    QStringList out;
    for (QLabel *l : w->findChildren<QLabel *>())
        out << l->text();
    return out;
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    QString tape = home_path("tape"), floppy = home_path("floppy"), out = home_path("out");
    for (const QString &d : {tape, floppy, out})
        makedirs(d, true);
    qputenv("KESTREL_TEST_SLOW", QString("tape=%1;floppy=%2").arg(tape, floppy).toUtf8());

    // ---- recognising them
    check(drives::slow_kind(join(tape, "a/b.txt")) == drives::Slow::Tape &&
              drives::slow_kind(join(floppy, "x.txt")) == drives::Slow::Floppy &&
              drives::slow_kind(home_path("other")) == drives::Slow::None && drives::slow_root(join(tape, "a")) == tape,
          "a path on a tape or floppy is recognised, with its drive's mount point; others aren't");
    bool root_listed = false, tape_listed = false;
    for (const drives::Mount &m : drives::mounts()) {
        root_listed = root_listed || m.root == "/";
        tape_listed = tape_listed || (m.root == tape && m.source.startsWith("ltfs"));
    }
    check(root_listed && tape_listed, "the mount list comes from the mount table (/ is in it), with the tape");
    QStringList loops;   // snap's squashfs images are loop devices as small as a floppy: not floppies
    for (const drives::Mount &m : drives::mounts())
        if (m.source.startsWith("/dev/loop") && drives::slow_kind(m.root) != drives::Slow::None)
            loops << m.root;
    check(loops.isEmpty(), "a small loop device (a snap's image) isn't taken for a floppy" +
                               (loops.isEmpty() ? QString() : " (" + loops.join(", ") + ")"));

    setup_app();
    MainWindow *w = open_window({HOME()});
    w->sidebar->refresh();
    QStringList devices = w->sidebar->entry_keys("devices");
    check(devices.contains(tape) && devices.contains(floppy), "the sidebar lists the tape and the floppy under Devices");

    // ---- nothing on the UI thread waits for a slow drive's size (a tape asked for its capacity waits behind the
    // reads another program makes: the whole window froze every 4 s)
    qputenv("KESTREL_TEST_STATFS_DELAY_MS", "2500");
    write_text(join(tape, "notes.txt"), "x");
    qint64 pause = longest_pause(
        [&]() {
            w->sidebar->refresh();
            w->pane()->set_path(tape);
            w->properties({join(tape, "notes.txt")});
            w->pane()->set_path(OVERVIEW);
        },
        600);
    check(pause < 1500, QString("asking a busy tape for its size (2.5 s) doesn't hold the window: the sidebar, the "
                                "status bar, Properties and the Overview get it in the background (longest pause %1 ms)")
                            .arg(pause));
    check(wait_for([&]() { return drives::Sizes::instance()->get(tape).total > 0; }, 8000),
          "...and the size comes when the drive answers");
    qputenv("KESTREL_TEST_STATFS_DELAY_MS", "0");

    // ---- opening a checksum file reads it in the background (on a tape the read waits for the drive to get there)
    {
        QString fifo = join(tape, "slow.sha256");   // a read that waits until something is written, as a seeking tape
        ::mkfifo(enc(fifo).constData(), 0600);
        write_text(join(tape, "data.bin"), "hello");
        std::thread writer([fifo]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            QFile f(fifo);
            if (f.open(QIODevice::WriteOnly))
                f.write("2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824  data.bin\n");
        });
        QElapsedTimer clock;
        clock.start();
        w->open_paths(w->pane(), {fifo});
        qint64 took = clock.elapsed();
        bool shown = wait_for([]() { return shown_window<hashcheck::VerifyDialog>() != nullptr; }, 6000);
        writer.join();
        check(took < 500 && shown, QString("opening a checksum file doesn't wait for it to be read (%1 ms), and Verify "
                                           "Checksums opens once it has been").arg(took));
        if (auto *d = shown_window<hashcheck::VerifyDialog>())
            d->close();
    }

    // ---- no previews, details or animations read from a slow drive, unless allowed for it
    QString img = join(tape, "photo.png");
    QImage(64, 48, QImage::Format_RGB32).save(img);
    QFileInfo fi(img);
    qint64 mtime = fi.lastModified().toSecsSinceEpoch();
    g_thumbs->get(img, mtime, false, 128, fi.size());
    spin(1500);
    check(g_thumbs->get(img, mtime, false, 128, fi.size()).isNull(), "an image on a tape gets no thumbnail");
    bool pending = true;
    QDate taken = groups::Dates::instance()->taken(img, mtime, &pending);
    check(!pending && taken.isValid(), "grouping by Date Taken doesn't read a file on a tape (its date created is used)");
    settings().setValue("play_gifs", true);
    check(!animate::enabled(join(tape, "anim.gif")) && animate::enabled(home_path("anim.gif")),
          "GIFs on a tape don't play in the view (they do elsewhere)");
    w->pane()->set_path(tape);
    w->info->setVisible(true);
    w->info->show_path(img);
    spin(400);
    QStringList info = labels_in(w->info);
    check(!info.join("|").contains("64 × 48") && info.join("|").contains("not read from this tape"),
          "the info panel doesn't read an image's details from a tape, and says why");
    drives::set_previews_allowed(tape, true);
    check(wait_for([&]() { return !g_thumbs->get(img, mtime, false, 128, fi.size()).isNull(); }, 5000),
          "with previews allowed for the tape, the image gets its thumbnail");
    w->info->show_path(join(tape, "notes.txt"));
    spin(300);
    w->info->show_path(img);
    check(wait_for([&]() { return labels_in(w->info).join("|").contains("64 × 48"); }, 5000),
          "...and the info panel reads its details (in the background)");
    drives::set_previews_allowed(tape, false);

    // ---- a tape is read in the order its files lie on it
    {
        QString src = join(tape, "src");
        makedirs(src, true);
        QStringList names = {"a.bin", "b.bin", "c.bin", "d.bin"};
        QStringList blocks = {"300", "900", "100", "500"};   // tape order: c, a, d, b
        bool attrs = true;
        for (int i = 0; i < 4; ++i) {
            write_text(join(src, names[i]), "data");
            attrs = attrs && set_attr(join(src, names[i]), "user.ltfs.startblock", blocks[i]) &&
                    set_attr(join(src, names[i]), "user.ltfs.partition", "b");
        }
        if (!attrs) {
            skip("reading a tape in tape order (this file system has no user extended attributes)");
        } else {
            auto order_of = [&](const QList<fileops::Job> &jobs) {
                QStringList seen;
                Task *t = fileops::start_ops(w, jobs, "Test", []() {});
                QObject::connect(t, &Task::progress, t, [&seen](double, const QString &text) {
                    if (text.startsWith("Positioning the tape — ") && !seen.contains(text.section("— ", 1)))
                        seen << text.section("— ", 1);
                }, Qt::DirectConnection);
                QPointer<Task> tp(t);
                wait_for([&]() { return !tp; }, 10000);
                return seen;
            };
            QStringList folder = order_of({{"copy", src, join(out, "fromtape")}});
            check(folder == QStringList({"c.bin", "a.bin", "d.bin", "b.bin"}),
                  "a folder copied off a tape is read in tape order, saying it's positioning the tape (" +
                      folder.join(", ") + ")");
            QStringList jobs = order_of({{"copy", join(src, "b.bin"), join(out, "b.bin")},
                                         {"copy", join(src, "a.bin"), join(out, "a.bin")},
                                         {"copy", join(src, "c.bin"), join(out, "c.bin")}});
            check(jobs == QStringList({"c.bin", "a.bin", "b.bin"}), "files copied off a tape go in tape order (" +
                                                                        jobs.join(", ") + ")");
        }
    }

    // ---- one job at a time on a slow drive: a job waits in the queue while another uses the tape
    {
        write_text(home_path("one.txt"), "1");
        auto held = std::make_shared<std::atomic<bool>>(true);
        Task *holder = fileops::queue_job(w, "Holding the tape", {tape}, [held](Task *t) {   // another job using it
            while (*held) {
                t->check();
                QThread::msleep(20);
            }
            return QVariant();
        });
        QPointer<Task> hp(holder);
        spin(100);
        Task *t = fileops::start_ops(w, {{"copy", home_path("one.txt"), join(tape, "one.txt")}}, "Test", []() {});
        QPointer<Task> tp(t);
        spin(800);
        bool waited = !exists(join(tape, "one.txt")) && tp && tp->queued && tp->text.contains("Waiting");
        *held = false;
        check(waited && wait_for([&]() { return !tp && !hp && exists(join(tape, "one.txt")); }),
              "a copy to a tape that another job is using waits in the queue, saying so, and runs when the tape is free");
    }

    // ---- a tape's index is written after a job changes it
    ::removexattr(enc(tape).constData(), "user.ltfs.sync");
    write_text(home_path("two.txt"), "2");
    Task *t2 = fileops::start_ops(w, {{"copy", home_path("two.txt"), join(tape, "two.txt")}}, "Test", []() {});
    QPointer<Task> t2p(t2);
    wait_for([&]() { return !t2p; });
    char buf[8];
    check(wait_for([&]() { return ::getxattr(enc(tape).constData(), "user.ltfs.sync", buf, sizeof buf) >= 0; }, 5000),
          "after a copy to a tape, its index is written (LTFS's sync attribute set on the tape)");

    // ---- many small files to a tape: asked first
    {
        QString many = home_path("many");
        makedirs(many, true);
        for (int i = 0; i < 1001; ++i)
            write_text(join(many, QString("s%1.txt").arg(i)), "s");
        bool planned = false;
        fileops::plan_transfer(w, {many}, tape, "copy", [&planned](const QList<fileops::Job> &) { planned = true; });
        QMessageBox *box = nullptr;
        wait_for([&]() { return (box = shown_window<QMessageBox>()) != nullptr; }, 5000);
        bool asked = box && box->text().contains("small files") && !planned;
        if (box)
            for (QAbstractButton *b : box->buttons())
                if (box->buttonRole(b) == QMessageBox::YesRole || box->standardButton(b) == QMessageBox::Yes)
                    b->click();
        check(asked && wait_for([&]() { return planned; }), "copying more than 1000 small files to a tape asks first, "
                                                            "and goes ahead when told to");
        bool planned_few = false;
        write_text(home_path("few.txt"), "f");
        fileops::plan_transfer(w, {home_path("few.txt")}, tape, "copy",
                               [&planned_few](const QList<fileops::Job> &) { planned_few = true; });
        check(wait_for([&]() { return planned_few; }) && !shown_window<QMessageBox>(), "...a few don't");
    }

    // ---- the Overview shows the tape with its volume name
    set_attr(tape, "user.ltfs.volumeName", "BACKUP01");
    drives::Sizes::instance()->forget(tape);
    w->pane()->set_path(HOME());
    w->pane()->set_path(OVERVIEW);
    check(wait_for([&]() {
              QStringList l = labels_in(w->pane());
              return l.contains("BACKUP01") && l.join("|").contains("Tape") && l.join("|").contains("LTFS");
          }, 8000),
          "the Overview shows the tape, by its volume name, as a tape");

    finish();
}
