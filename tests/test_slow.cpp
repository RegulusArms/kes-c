// Slow drives (src/drives.h): tapes (LTFS) and floppy disks. Folders marked as such with KESTREL_TEST_SLOW stand in for
// them, and KESTREL_TEST_STATFS_DELAY_MS makes their statfs wait, as a tape busy with another program's reads does.
#include "common.h"

#include "drives.h"
#include "groups.h"
#include "hashcheck.h"
#include "places.h"
#include "tapebuf.h"

#include <QElapsedTimer>
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
        qint64 b10 = tapebuf::budget(m);
        settings().setValue("tape_buffer_keep", 20);
        qint64 b20 = tapebuf::budget(m);
        settings().setValue("tape_buffer_gb", 4);
        qint64 b4 = tapebuf::budget(m);
        settings().remove("tape_buffer_keep");
        settings().remove("tape_buffer_gb");
        meminfo(32 * kGiB, 50 * 1024);   // 50 MB available
        qint64 low = tapebuf::budget(tapebuf::memory());
        check(m.total == 32 * GiB && m.available == 20 * GiB && b10 == 20 * GiB - qint64(0.1 * 32 * GiB) &&
                  b20 == 20 * GiB - qint64(0.2 * 32 * GiB) && b4 == 4 * GiB && low == 64LL * 1024 * 1024,
              "the tape buffer is the memory available less 10% of the total kept free (20 GB of 32: 16.8 GB), follows "
              "the share kept free and the largest size chosen, and gets at least 64 MB");

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
    }
    finish();
}
