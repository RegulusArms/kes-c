// Slow drives (src/drives.h): tapes (LTFS) and floppy disks. Folders marked as such with KESTREL_TEST_SLOW stand in for
// them, and KESTREL_TEST_STATFS_DELAY_MS makes their statfs wait, as a tape busy with another program's reads does.
#include "common.h"

#include "drives.h"
#include "groups.h"
#include "hashcheck.h"
#include "jobs.h"
#include "places.h"
#include "tapebuf.h"

#include <QElapsedTimer>
#include <QRegularExpression>
#include <sys/resource.h>
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
    qputenv("GSETTINGS_BACKEND", "memory");   // GNOME's settings that tape copies change: the test's own, in memory
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

    // ---- a tape isn't asked anything until the tower has told this process the others' jobs: the file chooser, which
    // never joins it, sent a health query into a tape another Kestrel was writing (and its size every 30 s). Once the
    // tower has answered, the tape is asked without anything asking again.
    {
        drives::Sizes *sizes = drives::Sizes::instance();
        int answers = 0;
        QMetaObject::Connection counted =
            QObject::connect(sizes, &drives::Sizes::changed, [&](const QString &root) { answers += root == tape; });
        sizes->get(tape);
        sizes->once(tape);
        spin(1500);
        bool held = answers == 0 && !atc::radio()->caught_up();
        atc::radio()->start();
        bool joined = wait_for([]() { return atc::radio()->caught_up(); }, 10000);
        bool asked = wait_for([&]() { return answers > 0; }, 5000);
        QObject::disconnect(counted);
        check(held && joined && asked && sizes->get(tape).total > 0,
              QString("a tape isn't asked its size or health before the tower has told this process the others' jobs, "
                      "and is asked once it has (answers before: %1, tower: %2, asked after: %3)")
                  .arg(held ? "none" : "some", joined ? "joined" : "not joined", asked ? "yes" : "no"));
    }

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


    // ---- while the tape is busy seeking (every stat on it waits, as LTFS's do), the window asks it nothing: selecting
    // files there (the status bar, the info panel) and opening a checksum file, which is then read in the background
    {
        QString busy = join(tape, "busy");
        makedirs(busy, true);
        write_text(join(busy, "a.bin"), "hello");
        write_text(join(busy, "b.bin"), "world");
        write_text(join(busy, "sums.sha256"),
                   "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03  a.bin\n");
        w->pane()->set_path(busy);
        wait_for([&]() { return w->pane()->all_paths().size() == 3; });
        for (QAction *a : w->findChildren<QAction *>())
            if (a->text() == "Info Panel" && !a->isChecked())
                a->trigger();
        w->pane()->select_paths({join(busy, "a.bin")});
        spin(500);   // (the info panel has read it, the tape idle)
        util::set_test_busy(tape, 1500);
        qint64 selecting = longest_pause([&]() { w->pane()->select_paths({join(busy, "b.bin")}); }, 800);
        w->pane()->select_paths({join(busy, "sums.sha256")});
        // (long enough to cover the Verify window opening, once the background read has waited on the tape too)
        qint64 opening = longest_pause([&]() { w->open_paths(w->pane(), {join(busy, "sums.sha256")}); }, 4500);
        bool info_later = wait_for([&]() { return labels_in(w->info).join("|").contains("Size:"); }, 8000);
        util::set_test_busy(QString(), 0);
        QWidget *verify = nullptr;
        wait_for([&]() {
            for (QWidget *x : QApplication::topLevelWidgets())
                if (x->isVisible() && x->windowTitle().contains("sums.sha256"))
                    verify = x;
            return verify != nullptr;
        }, 10000);
        if (verify)
            verify->close();
        for (QAction *a : w->findChildren<QAction *>())
            if (a->text() == "Info Panel" && a->isChecked())
                a->trigger();
        check(selecting < 600 && opening < 600 && info_later && verify,
              QString("while the tape is busy seeking, selecting files on it (%1 ms) and opening a checksum file there "
                      "(%2 ms) don't hold the window: the info panel fills in when the tape answers, and Verify Checksums "
                      "opens once the file is read")
                  .arg(selecting).arg(opening));
    }

    // ---- copies onto a tape go through a RAM buffer (tapebuf.h), so the drive keeps streaming
    {
        QString mi = home_path("meminfo");
        auto meminfo = [&](qint64 total_kb, qint64 avail_kb) {
            write_text(mi, QString("MemTotal:       %1 kB\nMemFree:        1000 kB\nMemAvailable:   %2 kB\n")
                               .arg(total_kb).arg(avail_kb).toUtf8());
        };
        qputenv("KESTREL_TEST_MEMINFO", mi.toUtf8());
        const qint64 GiB = 1024LL * 1024 * 1024, kGiB = 1024 * 1024;
        meminfo(32 * kGiB, 20 * kGiB);
        tapebuf::Memory m = tapebuf::memory();
        qint64 mid = tapebuf::budget(m);   // 20 GB available of 32: half of it, under the 16 GB limit
        settings().setValue("tape_buffer_gb", 4);
        qint64 b4 = tapebuf::budget(m);
        settings().setValue("tape_buffer_gb", 0);   // no limit of its own: still only what's safe
        qint64 unlimited = tapebuf::budget(m);
        settings().remove("tape_buffer_gb");
        meminfo(64 * kGiB, 50 * kGiB);   // a big machine: 16 GB, the limit, is safe
        qint64 big_box = tapebuf::budget(tapebuf::memory());
        meminfo(32 * kGiB, 5 * kGiB);    // squeezed: the 10% of 32 GB kept free leaves 1.8 GB of the 5 available
        qint64 squeezed = tapebuf::budget(tapebuf::memory());
        qint64 holding = tapebuf::budget(tapebuf::memory(), 10 * GiB);   // what it holds counts: 15 GB, half of it
        settings().setValue("tape_buffer_keep", 20);
        meminfo(32 * kGiB, 10 * kGiB);
        qint64 keep20 = tapebuf::budget(tapebuf::memory());   // 10 - 6.4 = 3.6 GB, under half (5)
        settings().remove("tape_buffer_keep");
        meminfo(32 * kGiB, 50 * 1024);   // 50 MB available
        qint64 low = tapebuf::budget(tapebuf::memory());
        auto in_gb = [](qint64 b) { return QString::number(double(b) / (1 << 30), 'f', 2); };
        check(m.total == 32 * GiB && m.available == 20 * GiB && mid == 10 * GiB && b4 == 4000000000LL &&
                  unlimited == 10 * GiB && big_box == 16000000000LL && squeezed == 5 * GiB - qint64(0.1 * 32 * GiB) && holding == 15 * GiB / 2 &&
                  keep20 == 10 * GiB - qint64(0.2 * 32 * GiB) && low == 64LL * 1024 * 1024,
              "the tape buffer is what's safe: half the memory available at most (counting what it holds), never leaving "
              "less than the share kept free, and the 16 GB limit (or the one chosen) only where that's safe; at least "
              "64 MB (" + in_gb(mid) + ", " + in_gb(b4) + ", " + in_gb(unlimited) + ", " + in_gb(big_box) + ", " +
                  in_gb(squeezed) + ", " + in_gb(holding) + ", " + in_gb(keep20) + " GB)");

        // the buffer itself: files in order, whole, also ones larger than it; skipped ones dropped
        QString bs = home_path("bufsrc");
        makedirs(bs, true);
        QList<qint64> sizes = {0, 1000, 300000, 3000000, 10000, 2500000};
        QStringList files;
        QHash<QString, QByteArray> content;
        for (int i = 0; i < sizes.size(); ++i) {
            QByteArray d(sizes[i], '\0');
            for (qint64 k = 0; k < d.size(); ++k)
                d[k] = char((k * 31 + i * 7) & 0xff);
            QString f = join(bs, QString("f%1").arg(i));
            write_text(f, d);
            files << f;
            content[f] = d;
        }
        auto read_through = [&](tapebuf::Buffer &buf, const QStringList &which, bool *ok) {
            *ok = true;
            for (const QString &f : which) {
                struct stat st;
                ::stat(enc(f).constData(), &st);
                if (!buf.begin(f, st.st_dev, st.st_ino)) {
                    *ok = false;
                    return;
                }
                QByteArray got;
                const char *data;
                while (qint64 n = buf.next(&data))
                    got.append(data, int(n));
                *ok = *ok && got == content[f];
            }
        };
        const qint64 cap = 1024 * 1024, chunk = cap / 8;
        bool whole = false, skipping = false;
        {
            tapebuf::Buffer buf(nullptr, files, cap);
            read_through(buf, files, &whole);
        }
        {
            tapebuf::Buffer buf(nullptr, files, cap);
            read_through(buf, {files[0], files[3], files[5]}, &skipping);   // 1, 2 and 4 skipped (a conflict, an error)
        }
        struct stat st0;
        ::stat(enc(files[1]).constData(), &st0);
        tapebuf::Buffer other(nullptr, files, cap);
        bool unplanned = !other.begin(home_path("not-planned"), st0.st_dev, st0.st_ino);
        bool changed = !other.begin(files[1], st0.st_dev, st0.st_ino + 1);
        check(whole && skipping && unplanned && changed,
              "the buffer gives every file whole and in order (3 MB and 2.5 MB ones through a 1 MB buffer), drops the "
              "ones skipped, and leaves a file not planned or changed since to be read directly");

        // fill first, then: below the drive's lowest streaming speed, refill before writing again; above it, carry on
        qputenv("KESTREL_TEST_TAPEBUF_DELAY_MS", "15");   // a slow source: about 8 MB/s
        bool ok1 = false, ok2 = false;
        qint64 first = -1, resumed = -1, resumed_stream = -2;
        int waits = 0, waits_stream = -1;
        {
            tapebuf::Buffer buf(nullptr, files, cap);
            read_through(buf, files, &ok1);
            first = buf.first_write_fill;
            resumed = buf.min_resume_fill;
            waits = buf.waits();
        }
        settings().setValue("tape_min_speed", 1);   // a drive that streams down to 1 MB/s
        {
            tapebuf::Buffer buf(nullptr, files, cap);
            read_through(buf, files, &ok2);
            resumed_stream = buf.min_resume_fill;
            waits_stream = buf.waits();
        }
        settings().remove("tape_min_speed");
        check(ok1 && first >= cap - chunk && waits > 0 && resumed >= cap - chunk,
              QString("writing starts only once the buffer is full (%1 of %2), and after running dry, waits until it's "
                      "full again (%3 times, from %4) when the source is slower than the drive streams")
                  .arg(first).arg(cap).arg(waits).arg(resumed));
        check(ok2 && waits_stream == 0 && resumed_stream == -1,
              "...and when the source is within the drive's speed-matching range, carries on at the source's pace "
              "without waiting for a refill");

        // a copy onto the tape: nothing written until the buffer is full; files whole, in order; the log says how it went
        QString big = home_path("bigsrc");
        makedirs(big, true);
        QHash<QString, QByteArray> data;
        for (int i = 0; i < 6; ++i) {
            QByteArray d(18 * 1024 * 1024, char('a' + i));   // 108 MB: more than the 64 MB buffer it gets here
            QString f = join(big, QString("part%1.bin").arg(i));
            write_text(f, d);
            data[basename(f)] = d;
        }
        QStringList src_order = listdir(big);
        qputenv("KESTREL_TEST_TAPEBUF_DELAY_MS", "120");   // 8 MB pieces: about 65 MB/s
        QString into = join(tape, "bigcopy");
        QPointer<Task> t = fileops::start_ops(w, {{"copy", big, into}}, "Test", []() {});
        QStringList appeared, texts;
        bool early = false, filling = false;
        QElapsedTimer clock;
        clock.start();
        while (t && clock.elapsed() < 60000) {
            if (t->text.startsWith("Filling the buffer"))
                filling = true;
            if (isdir(into))
                for (const QString &n : listdir(into)) {
                    if (!n.startsWith(".kes-") && !appeared.contains(n))
                        appeared << n;
                    // data written before the buffer could have filled (8 pieces × 120 ms); a new file on a tape is
                    // made under its name when it's opened, empty until then
                    if (filling && !n.startsWith(".kes-") && QFileInfo(join(into, n)).size() > 0 && clock.elapsed() < 400)
                        early = true;
                }
            texts << t->text;
            spin(20);
        }
        if (isdir(into))
            for (const QString &n : listdir(into))
                if (!n.startsWith(".kes-") && !appeared.contains(n))
                    appeared << n;
        bool intact = true, in_order = true;
        auto ctime_ns = [&](const QString &n) {   // when it was finished (its times and permissions set when whole)
            struct stat st;
            return ::stat(enc(join(into, n)).constData(), &st) == 0 ? qint64(st.st_ctim.tv_sec) * 1000000000 + st.st_ctim.tv_nsec
                                                                     : -1;
        };
        for (int i = 0; i < src_order.size(); ++i) {
            intact = intact && read_file(join(into, src_order[i])) == data[src_order[i]];
            if (i > 0)
                in_order = in_order && ctime_ns(src_order[i - 1]) <= ctime_ns(src_order[i]);
        }
        appeared.sort();
        QStringList expected = src_order;
        expected.sort();
        spin(300);   // (the job's end reaches the task list through the event loop)
        FinishedTask last;   // the copy's (the tape's index is written by a job of its own after it)
        for (const FinishedTask &f : TaskBoard::instance()->finished)
            if (f.title == "Test" && f.log.contains("bigcopy"))
                last = f;
        check(filling && !early && appeared == expected && in_order && intact,
              "a copy onto a tape fills the buffer first (saying so), writes nothing meanwhile, and then writes every "
              "file whole and in order (" + appeared.join(", ") + ")");
        check(last.log.contains("Tape buffer: up to " + human_size(qint64(64) * 1024 * 1024)) &&
                  last.log.contains("written to the tape at") && last.log.contains("read from the source at") &&
                  (last.log.contains("never had to wait") || last.log.contains("had to wait for it")),
              "the job's log gives the buffer's size, the speeds, and how many times the tape had to wait");

        // pause: the reading stops too (what's buffered stays); ✕ while filling: nothing written, no part file
        auto rchar = []() {
            for (const QString &line : QString::fromUtf8(read_file("/proc/self/io")).split('\n'))
                if (line.startsWith("rchar:"))
                    return line.mid(6).trimmed().toLongLong();
            return 0LL;
        };
        QString into2 = join(tape, "bigcopy2");
        QPointer<Task> t2 = fileops::start_ops(w, {{"copy", big, into2}}, "Test", []() {});
        wait_for([&]() { return t2 && t2->text.startsWith("Filling the buffer"); }, 10000);
        spin(300);
        if (t2)
            t2->set_paused(true);
        spin(300);
        qint64 r0 = rchar();
        spin(800);
        qint64 paused_read = rchar() - r0;
        if (t2)
            t2->set_paused(false);
        qint64 r1 = rchar();
        spin(800);
        qint64 running_read = rchar() - r1;
        if (t2)
            t2->cancel();
        bool ended = wait_for([&]() { return !t2; }, 10000);
        QStringList left = isdir(into2) ? listdir(into2) : QStringList();
        check(paused_read < 1024 * 1024 && running_read > 8 * 1024 * 1024,
              QString("pausing a copy onto a tape pauses the buffer's reading too (%1 bytes read while paused, %2 "
                      "running)").arg(paused_read).arg(running_read));
        check(ended && left.isEmpty(), "✕ while the buffer fills ends the copy with nothing written to the tape, not even "
                                       "a part file (" + left.join(", ") + ")");
        qunsetenv("KESTREL_TEST_TAPEBUF_DELAY_MS");

        // room on the tape is checked before a file is written
        qputenv("KESTREL_TEST_TAPE_FREE", "1000");
        write_text(home_path("fat.bin"), QByteArray(5000, 'x'));
        QPointer<Task> t3 = fileops::start_ops(w, {{"copy", home_path("fat.bin"), join(tape, "fat.bin")}}, "Test", []() {});
        wait_for([&]() { return !t3; }, 10000);
        QMessageBox *box = nullptr;
        wait_for([&]() { return (box = shown_window<QMessageBox>()) != nullptr; }, 5000);
        QString said = box ? box->text() + "\n" + box->detailedText() : QString();
        if (box)
            box->close();
        qunsetenv("KESTREL_TEST_TAPE_FREE");
        check(!lexists(join(tape, "fat.bin")) && said.contains("Not enough space on the tape"),
              "a file that doesn't fit on the tape isn't started, and the job says why");
        qunsetenv("KESTREL_TEST_MEMINFO");
    }

    // ---- LTFS's timed index writes (each one stops the drive) are recognised from its command line
    {
        QString proc = home_path("proc");
        auto ltfs = [&](const QString &pid, const QList<QByteArray> &args) {
            makedirs(join(proc, pid), true);
            write_text(join(join(proc, pid), "cmdline"), args.join('\0') + '\0');
        };
        ltfs("200", {"bash", "-c", "x"});
        ltfs("300", {"/usr/local/bin/ltfs", "-o", "devname=/dev/sg0", "-o", "allow_other", tape.toUtf8()});
        qputenv("KESTREL_TEST_PROC", proc.toUtf8());
        QString plain = drives::ltfs_sync(tape), none = drives::ltfs_sync(home_path("elsewhere"));
        ltfs("300", {"ltfs", "-o", "devname=/dev/sg0,sync_type=unmount", tape.toUtf8()});
        QString unmount = drives::ltfs_sync(tape);
        ltfs("300", {"ltfs", "-osync_type=time@10", (tape + "/").toUtf8()});
        QString ten = drives::ltfs_sync(tape);
        qunsetenv("KESTREL_TEST_PROC");
        check(plain == "time@5" && none.isEmpty() && unmount == "unmount" && ten == "time@10" &&
                  drives::ltfs_sync_minutes(plain) == 5 && drives::ltfs_sync_minutes(unmount) == 0,
              "how LTFS writes a tape's index is read from its ltfs process (every 5 minutes by default, or as "
              "sync_type says), so a copy's log and the Overview can say each one stops the drive");
    }

    // ---- programs asking the tape for its free space stop it streaming: GNOME's low-disk-space check skips it while a
    // job writes to it, and a system monitor that's open is named in the job's log
    {
        GSettingsSchemaSource *src = g_settings_schema_source_get_default();
        GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, "org.gnome.settings-daemon.plugins.housekeeping", TRUE) : nullptr;
        bool gnome = schema != nullptr;   // (without it, GSettings aborts on the key)
        if (schema)
            g_settings_schema_unref(schema);
        auto ignored = [gnome]() {
            QStringList out;
            if (!gnome)
                return out;
            GSettings *gs = g_settings_new("org.gnome.settings-daemon.plugins.housekeeping");
            gchar **v = g_settings_get_strv(gs, "ignore-paths");
            for (gchar **p = v; p && *p; ++p)
                out << QString::fromUtf8(*p);
            g_strfreev(v);
            g_object_unref(gs);
            return out;
        };
        auto set_ignored = [](const QStringList &l) {
            GSettings *gs = g_settings_new("org.gnome.settings-daemon.plugins.housekeeping");
            QList<QByteArray> b;
            QVector<const gchar *> p;
            for (const QString &x : l)
                b << x.toUtf8();
            for (const QByteArray &x : b)
                p << x.constData();
            p << nullptr;
            g_settings_set_strv(gs, "ignore-paths", p.constData());
            g_object_unref(gs);
        };
        QString proc = home_path("proc2");
        makedirs(join(proc, "4242"), true);
        write_text(join(join(proc, "4242"), "comm"), "gnome-system-mo\n");
        makedirs(join(proc, "4243"), true);
        write_text(join(join(proc, "4243"), "comm"), "bash\n");
        write_text(home_path("quiet.bin"), QByteArray(40 * 1024 * 1024, 'q'));
        // a copy onto the tape, lasting a moment (a slow source); whether the tape was skipped while it ran, and its log
        auto copy = [&](const QString &name, bool *skipped_meanwhile, const char *delay = "150") {
            qputenv("KESTREL_TEST_TAPEBUF_DELAY_MS", delay);
            QPointer<Task> t = fileops::start_ops(w, {{"copy", home_path("quiet.bin"), join(tape, name)}}, "Quiet", []() {});
            *skipped_meanwhile = false;
            while (t) {
                if (gnome && ignored().contains(tape))
                    *skipped_meanwhile = true;
                spin(20);
            }
            qunsetenv("KESTREL_TEST_TAPEBUF_DELAY_MS");
            spin(300);
            QString log;
            for (const FinishedTask &f : TaskBoard::instance()->finished)
                if (f.title == "Quiet" && f.log.contains(name))
                    log = f.log;
            return log;
        };
        if (!gnome) {
            skip("GNOME's low-disk-space check skips a tape while a job writes to it (GNOME's settings aren't installed)");
        } else {
            set_ignored({"/srv/keep"});
            bool during = false;
            QString log = copy("quiet1.bin", &during);
            check(during && ignored() == QStringList({"/srv/keep"}) && log.contains("low-disk-space check skips this tape"),
                  "while a job writes to a tape, GNOME's low-disk-space check skips it (it asks the drive for its free "
                  "space once a minute), and afterwards its list is as it was (" + ignored().join(", ") + ")");
            set_ignored({"/srv/keep", tape});
            log = copy("quiet2.bin", &during);
            check(ignored() == QStringList({"/srv/keep", tape}) && !log.contains("low-disk-space check skips this tape"),
                  "a tape the user put on that list stays there after the job");
            set_ignored({});
        }
        qputenv("KESTREL_TEST_PROC", proc.toUtf8());
        QStringList pollers = drives::free_space_pollers();
        bool during = false;
        QString log = copy("quiet3.bin", &during, "2500");   // past a 5-second check: still said once
        qunsetenv("KESTREL_TEST_PROC");
        check(pollers == QStringList({"GNOME System Monitor"}) && log.count("Note: GNOME System Monitor is running") == 1,
              "a system monitor that's open (it asks every drive for its free space every few seconds) is named in a "
              "tape copy's log, once, also when it runs on (" + pollers.join(", ") + ")");
    }

    // ---- writing onto a tape with Kestrel's copy or with dd, as asked (or as Preferences say)
    {
        QString data_file = home_path("ddsrc.bin");
        QByteArray payload(3 * 1024 * 1024 + 12345, '\0');
        for (int i = 0; i < payload.size(); ++i)
            payload[i] = char((i * 7 + i / 4096) & 0xff);
        write_text(data_file, payload);
        QList<fileops::Job> planned{{"copy", data_file, join(tape, "dd1.bin")}, {"copy", data_file, home_path("not-tape.bin")}};
        settings().setValue("tape_writer", "ask");
        QList<fileops::Job> got;
        bool went = false;
        auto run = [&]() {
            went = false;
            fileops::choose_tape_writer(w, planned, [&](const QList<fileops::Job> &jobs) {
                got = jobs;
                went = true;
            });
        };
        auto question = [&]() {
            QMessageBox *b = nullptr;
            wait_for([&]() { return (b = shown_window<QMessageBox>()) && b->text().contains("write to the tape"); }, 3000);
            return b;
        };
        auto press = [&](QMessageBox *b, const QString &text) {
            for (QAbstractButton *x : b ? b->buttons() : QList<QAbstractButton *>())
                if (x->text() == text)
                    x->click();
        };
        run();
        QMessageBox *q = question();
        bool asked = q && is_free_window(q) && q->text().contains(basename(tape)) && !went;
        press(q, "Cancel");
        spin(100);
        bool cancelled = !went;
        run();
        q = question();
        press(q, "dd");
        spin(100);
        check(asked && cancelled && went && got.size() == 2 && got[0].dd && !got[1].dd,
              "a copy onto a tape asks (a free window) whether Kestrel's copy or dd writes it; dd marks only the jobs "
              "onto the tape, and Cancel starts nothing");
        run();
        q = question();
        if (q && q->checkBox())
            q->checkBox()->setChecked(true);
        press(q, "Kestrel's Copy");
        spin(100);
        bool kestrel = went && !got[0].dd && settings().value("tape_writer").toString() == "kestrel";
        run();
        spin(200);
        bool silent = went && !got[0].dd && !shown_window<QMessageBox>();
        settings().setValue("tape_writer", "dd");
        run();
        spin(200);
        check(kestrel && silent && went && got[0].dd && !got[1].dd,
              "“Don't ask again” saves the choice (Preferences → Files → Tapes), and then nothing is asked");
        settings().setValue("tape_writer", "ask");

        // dd writing a file onto the tape, in a job: its command and its output are in the job's log
        fileops::Job job{"copy", data_file, join(tape, "dd1.bin")};
        job.dd = true;
        QPointer<Task> t = fileops::start_ops(w, {job}, "With dd", []() {});
        wait_for([&]() { return !t; }, 20000);
        spin(300);
        QString log;
        for (const FinishedTask &f : TaskBoard::instance()->finished)
            if (f.title == "With dd")
                log = f.log;
        check(read_file(join(tape, "dd1.bin")) == payload && log.contains("dd bs=512K iflag=fullblock status=progress") &&
                  log.contains("records out") && !log.contains("Tape buffer:"),
              "with dd, the file is written whole by dd (512 KB blocks), with no buffer of Kestrel's, and the job's log "
              "has dd's command and its output");

        // a dd write that fails part-way (a file-size limit, as a full tape) leaves no half-written file
        fileops::Job job2{"copy", data_file, join(tape, "dd2.bin")};
        job2.dd = true;
        struct rlimit old_lim, lim;
        ::getrlimit(RLIMIT_FSIZE, &old_lim);
        lim = old_lim;
        lim.rlim_cur = 1024 * 1024;
        auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
        ::setrlimit(RLIMIT_FSIZE, &lim);   // (dd inherits it)
        QPointer<Task> t2 = fileops::start_ops(w, {job2}, "With dd 2", []() {});
        wait_for([&]() { return !t2; }, 20000);
        ::setrlimit(RLIMIT_FSIZE, &old_lim);
        ::signal(SIGXFSZ, old_handler);
        QMessageBox *box = nullptr;
        wait_for([&]() { return (box = shown_window<QMessageBox>()) != nullptr; }, 5000);
        QString said = box ? box->text() + "\n" + box->detailedText() : QString();
        if (box)
            box->close();
        check(!lexists(join(tape, "dd2.bin")) && said.contains("dd couldn't write") && said.contains("dd2.bin"),
              "a dd write that fails part-way (a file-size limit) leaves no half-written file on the tape, and says dd "
              "failed (" + said.simplified().left(160) + ")");
    }

    // ---- Properties names a file's type on a tape by its name (its contents would make the drive seek)
    {
        QByteArray pdf = "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n1 0 obj\n<<>>\nendobj\n";
        write_text(join(tape, "document"), pdf);   // no extension: only its contents say it's a PDF
        write_text(home_path("document"), pdf);
        auto type_shown = [&](const QString &path) {
            w->properties({path});
            QString text;
            wait_for([&]() {
                for (QWidget *x : QApplication::topLevelWidgets())
                    if (x->isVisible() && x->windowTitle().contains("document")) {
                        text = labels_in(x).join("|");
                        x->close();
                        return true;
                    }
                return false;
            });
            return text;
        };
        QString on_tape = type_shown(join(tape, "document")), at_home = type_shown(home_path("document"));
        check(!on_tape.contains("PDF") && at_home.contains("PDF"),
              "Properties of a file on a tape doesn't read it to name its type (a file elsewhere is)");

        // GIO's type of a file (for Open With, opening it, and Recent) reads its start: on a tape, by its name instead
        QString tape_doc = join(tape, "document"), home_doc = home_path("document");
        check(content_type(tape_doc) == "application/octet-stream" && content_type(home_doc) == "application/pdf",
              "the type GIO would read from a file's contents is taken from its name on a tape (" + content_type(tape_doc) +
                  ", elsewhere " + content_type(home_doc) + ")");
        places::add_recent({tape_doc});
        QString xbel = QString::fromUtf8(read_file(home_path(".local/share/recently-used.xbel")));
        int at = xbel.indexOf(QUrl::fromLocalFile(tape_doc).toString(QUrl::FullyEncoded));
        QString entry = at >= 0 ? xbel.mid(at, xbel.indexOf("</bookmark>", at) - at) : QString();
        check(!entry.isEmpty() && entry.contains("application/octet-stream") && !entry.contains("application/pdf"),
              "opening a file on a tape adds it to Recent without reading it (its type by name)");
    }

    // ---- onto a tape, a new file is written straight to its name (no part file: LTFS never reuses tape anyway, and has
    // no RENAME_NOREPLACE or link() for a part's claim); a replaced one, and anything onto a floppy, still go through one
    {
        auto run = [&](const QList<fileops::Job> &jobs) {
            QPointer<Task> t = fileops::start_ops(w, jobs, "Test", []() {});
            wait_for([&]() { return !t; }, 30000);
            spin(200);
            if (auto *box = shown_window<QMessageBox>())   // a conflict or failure says so: answered
                box->close();
        };
        QByteArray data(3 * 1024 * 1024, 'd');
        write_text(home_path("direct.bin"), data);
        QString on_tape = join(tape, "direct.bin");
        QStringList seen;
        {
            DirWatch watch(tape);
            run({{"copy", home_path("direct.bin"), on_tape}});
            seen = watch.events();
        }
        check(seen == QStringList{"+direct.bin"} && read_file(on_tape) == data,
              "a new file copied onto a tape is written under its own name, with no part file (" + seen.join(" ") + ")");
        write_text(home_path("direct.bin"), QByteArray(1024, 'n'));
        {
            DirWatch watch(tape);
            run({{"copy", home_path("direct.bin"), on_tape, true}});
            seen = watch.events();
        }
        check(seen.size() == 2 && seen[0].startsWith("+.kes-") && seen[1] == ">direct.bin" &&
                  read_file(on_tape) == QByteArray(1024, 'n'),
              "replacing a file on a tape still goes through a part file, renamed over it when whole (" + seen.join(" ") + ")");
        QString on_floppy = join(floppy, "direct.bin");
        {
            DirWatch watch(floppy);
            run({{"copy", home_path("direct.bin"), on_floppy}});
            seen = watch.events();
        }
        check(seen.size() == 2 && seen[0].startsWith("+.kes-") && seen[1] == ">direct.bin" && isfile(on_floppy),
              "a new file onto a floppy still goes through a part file (" + seen.join(" ") + ")");

        // a name taken by the time the copy runs is kept, and nothing is put in its place
        write_text(join(tape, "taken.bin"), "theirs");
        run({{"copy", home_path("direct.bin"), join(tape, "taken.bin")}});
        check(read_file(join(tape, "taken.bin")) == "theirs", "a name on the tape taken meanwhile is kept as it is");

        // a write that fails part-way (a file-size limit standing for a full tape): nothing left under the name
        auto leftovers = [&]() {
            QStringList out;
            for (const QString &n : listdir(tape))
                if (n.startsWith(".kes-") || n == "full.bin")
                    out << n;
            return out;
        };
        write_text(home_path("full.bin"), QByteArray(4 * 1024 * 1024, 'f'));
        struct rlimit old_lim, lim;
        ::getrlimit(RLIMIT_FSIZE, &old_lim);
        lim = old_lim;
        lim.rlim_cur = 1 << 20;
        auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
        ::setrlimit(RLIMIT_FSIZE, &lim);
        run({{"copy", home_path("full.bin"), join(tape, "full.bin")}});
        ::setrlimit(RLIMIT_FSIZE, &old_lim);
        ::signal(SIGXFSZ, old_handler);
        check(leftovers().isEmpty(), "a write onto a tape that fails part-way leaves no half-written file under its name, "
                                     "and no part file (" + leftovers().join(", ") + ")");

        // cancelled while it writes: the half-written file goes too
        write_text(home_path("cancel.bin"), QByteArray(100 * 1024 * 1024, 'c'));
        qputenv("KESTREL_TEST_TAPEBUF_DELAY_MS", "120");   // a slow source: writing spans a while
        bool writing = false;
        {
            DirWatch watch(tape);
            QPointer<Task> t = fileops::start_ops(w, {{"copy", home_path("cancel.bin"), join(tape, "cancel.bin")}},
                                                  "Test", []() {});
            writing = wait_for([&]() { return watch.events().contains("+cancel.bin"); }, 30000);
            if (t)
                t->cancel();
            wait_for([&]() { return !t; }, 30000);
        }
        qunsetenv("KESTREL_TEST_TAPEBUF_DELAY_MS");
        spin(200);
        check(writing && !lexists(join(tape, "cancel.bin")) && leftovers().isEmpty(),
              "cancelling a copy onto a tape while it writes deletes the half-written file");

        // the job's log gets a line every 5 seconds: the time, the tape's speed (or that it waits for the buffer), the
        // source's, the buffer and how far the copy is. A small buffer (64 MB), a slow source (8 MB a second) and a low
        // streaming speed: it fills for ~8 s, then the tape writes at the source's pace for a few seconds more
        {
            QString mi2 = home_path("meminfo-small");
            write_text(mi2, "MemTotal:       1048576 kB\nMemFree:        1000 kB\nMemAvailable:   102400 kB\n");
            qputenv("KESTREL_TEST_MEMINFO", mi2.toUtf8());
            qputenv("KESTREL_TEST_TAPEBUF_DELAY_MS", "1000");
            settings().setValue("tape_min_speed", 1);
            write_text(home_path("rated.bin"), QByteArray(120 * 1024 * 1024, 'r'));
            QPointer<Task> rt = fileops::start_ops(w, {{"copy", home_path("rated.bin"), join(tape, "rated.bin")}},
                                                   "Rated", []() {});
            wait_for([&]() { return !rt; }, 60000);
            qunsetenv("KESTREL_TEST_TAPEBUF_DELAY_MS");
            qunsetenv("KESTREL_TEST_MEMINFO");
            settings().remove("tape_min_speed");
            spin(300);
            QString log;
            for (const FinishedTask &f : TaskBoard::instance()->finished)
                if (f.title == "Rated")
                    log = f.log;
            static const QRegularExpression line(
                R"(^  \d\d:\d\d:\d\d  tape (waiting for the buffer|[\d.]+ [kMG]?B/s) · source \S+ \S+ · buffer .+ of .+ \(\d+%\) · .+ of .+ written$)");
            QStringList lines, waiting, rated;
            for (const QString &l : log.split('\n'))
                if (line.match(l).hasMatch()) {
                    lines << l;
                    (l.contains("waiting for the buffer") ? waiting : rated) << l;
                }
            check(lines.size() >= 2 && !waiting.isEmpty() && !rated.isEmpty() && read_file(join(tape, "rated.bin")).size() == 120 * 1024 * 1024,
                  "a copy onto a tape logs its speed every 5 seconds: while the buffer fills, and while the tape writes (" +
                      lines.join(" | ") + ")");
        }

        // nothing is asked of a tape while a job uses it (each question is a command to the drive, which interrupts its
        // writing); once the job has ended, it's asked again. (A slow source: the job outlasts the checks.)
        qputenv("KESTREL_TEST_TAPEBUF_DELAY_MS", "1000");
        drives::Sizes *sizes = drives::Sizes::instance();
        int answers = 0;
        QMetaObject::Connection counted = QObject::connect(sizes, &drives::Sizes::changed, [&](const QString &root) {
            answers += root == tape;
        });
        QPointer<Task> busy_job = fileops::start_ops(w, {{"copy", home_path("cancel.bin"), join(tape, "held.bin")}},
                                                     "Test", []() {});
        wait_for([&]() { return jobs::Queue::instance()->busy(tape); }, 10000);
        spin(1000);   // (a question sent before the job held the drive may still be answered)
        answers = 0;
        sizes->forget(tape);   // nothing known: would be asked at once
        sizes->get(tape);
        sizes->once(tape);
        spin(1500);
        bool held = jobs::Queue::instance()->busy(tape);   // (still, at the end of the wait)
        int while_busy = answers;
        if (busy_job)
            busy_job->cancel();
        wait_for([&]() { return !busy_job && !jobs::Queue::instance()->busy(tape); }, 30000);
        qunsetenv("KESTREL_TEST_TAPEBUF_DELAY_MS");
        sizes->get(tape);
        bool asked_after = wait_for([&]() { return answers > while_busy; }, 5000);
        QObject::disconnect(counted);
        check(held && while_busy == 0 && asked_after,
              "a tape isn't asked its size or health while a job uses it, and is asked again once the job has ended");
    }
    finish();
}
