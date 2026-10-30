// Extracting archives through the window (archive_ui.h): Extract All for several archives (one job, one summary, a
// split set counted once), the password typed once being tried on the archives after it, the quick password test
// (archive::password_opens), and remembered passwords in the keyring (keyring.h) with a real gnome-keyring-daemon on
// the test's private session bus (skipped where it isn't installed). Also tar archives with a password (through gpg):
// made, read back by gpg itself, opened, listed, tested and extracted by Kestrel, refused with a wrong password. And
// checksum files made while a tar archive is compressed.
#include "common.h"

#include "archive_ui.h"
#include "archivewin.h"
#include "hashcheck.h"
#include "prefswin.h"
#include "keyring.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDialog>
#include <QGroupBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QMimeData>
#include <QUrl>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QStatusBar>
#include <QTreeWidget>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <atomic>
#include <thread>

using namespace test;

// make an archive of `files` (names in `src`) with the given tool command, in `src`'s folder
static bool make(const QStringList &argv, const QString &cwd)
{
    proc::Options o;
    o.cwd = cwd;
    proc::Result r = proc::run(argv, 60000, o);
    return r.rc == 0;
}

static QDialog *prompt()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *d = qobject_cast<QDialog *>(w); d && d->isVisible() && d->objectName() == "archive_password")
            return d;
    return nullptr;
}

static QMessageBox *summary()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *b = qobject_cast<QMessageBox *>(w); b && b->isVisible() && b->objectName() == "extract_all_summary")
            return b;
    return nullptr;
}

// What a run of the job asked and told: each password window's message (and whether it offered to remember), and the
// summary (several archives) or the status bar's message (one)
struct Asked {
    QStringList prompts;
    QList<bool> offered;
    bool free = true;   // every password window was a free one (windows.h), though a job's thread asked for it
    QString told;
};

struct Answer {
    QString password;   // empty: cancel the window
    bool remember = false;
};

// Answer the password windows in turn until the job ends (`ended` says it has)
static Asked drive(QList<Answer> answers, const std::function<QString()> &ended, int ms = 60000)
{
    Asked a;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (QDialog *d = prompt()) {
            QList<QLabel *> labels = d->findChildren<QLabel *>();
            a.prompts << (labels.isEmpty() ? QString() : labels.first()->text());
            auto *box = d->findChild<QCheckBox *>("remember");
            a.offered << bool(box);
            a.free = a.free && is_free_window(d);
            Answer ans = answers.isEmpty() ? Answer{} : answers.takeFirst();
            if (box)
                box->setChecked(ans.remember);
            d->findChild<QLineEdit *>()->setText(ans.password);
            QPointer<QDialog> gone(d);
            ans.password.isEmpty() ? d->reject() : d->accept();
            wait_for([&]() { return !gone || !gone->isVisible(); });
            continue;
        }
        QString e = ended();
        if (!e.isEmpty()) {
            a.told = e;
            return a;
        }
        spin(30);
    }
    a.told = "(timed out)";
    return a;
}

static QString summary_text()
{
    QMessageBox *b = summary();
    if (!b)
        return QString();
    QString text = b->text();
    b->close();
    return text;
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    setup_app();

    // ---- which archives a selection holds
    QString sel = home_path("sel");
    makedirs(sel, true);
    for (const QString &n : {"x.part1.rar", "x.part2.rar", "x.part3.rar", "b.7z", "c.7z.001", "c.7z.002", "notes.txt"})
        write_text(join(sel, n), "x");
    QStringList found = archive_ui::archives_of(
        {join(sel, "x.part2.rar"), join(sel, "x.part1.rar"), join(sel, "x.part3.rar"), join(sel, "b.7z"),
         join(sel, "c.7z.002"), join(sel, "c.7z.001"), join(sel, "notes.txt")});
    check(found == QStringList{join(sel, "x.part1.rar"), join(sel, "b.7z"), join(sel, "c.7z.001")},
          "a selection's archives: each split set once (by its first part, whichever part comes first), no other files");

    MainWindow *w = open_window({HOME()});
    auto labels = [&](const QStringList &paths) {
        QMenu *m = w->build_menu(w->pane(), paths);
        QStringList out;
        for (QAction *a : m->actions())
            out << a->text();
        delete m;
        return out;
    };
    QStringList many = labels({join(sel, "b.7z"), join(sel, "x.part1.rar")});
    QStringList split = labels({join(sel, "x.part1.rar"), join(sel, "x.part2.rar"), join(sel, "x.part3.rar")});
    QStringList mixed = labels({join(sel, "b.7z"), join(sel, "x.part1.rar"), join(sel, "notes.txt")});
    check(many.contains("Extract All Here") && many.contains("Extract All To…") && !many.contains("Extract Here"),
          "two archives selected: the menu offers Extract All Here and Extract All To…");
    bool split_one = (split.contains("Extract Here") && split.contains("Extract To…")) ||
                     std::any_of(split.begin(), split.end(), [](const QString &l) { return l.startsWith("Extract (install"); });
    check(split_one && !split.contains("Extract All Here"),
          "a split set selected whole is one archive: Extract Here and Extract To…, not Extract All");
    check(!mixed.contains("Extract All Here") && !mixed.contains("Extract Here"),
          "archives selected with other files: no Extract");

    // ---- no keyring: the Secret Service's name is held here, so the bus doesn't start one (where gnome-keyring is
    // installed it would) and calls to it fail. Taken before anything looks for a saved password (the gpg checks'
    // Open as Archive and Extract Here do): that would have the bus start a gnome-keyring, which the keyring checks
    // further down then can't replace with their own.
    GDBusConnection *holder = nullptr;
    {
        gchar *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
        holder = g_dbus_connection_new_for_address_sync(
            addr,
            GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, nullptr, nullptr);
        g_free(addr);
        if (GVariant *v = g_dbus_connection_call_sync(holder, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                      "org.freedesktop.DBus", "RequestName",
                                                      g_variant_new("(su)", "org.freedesktop.secrets", 4u), nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr))
            g_variant_unref(v);
    }
    // ---- tar archives with a password: piped through gpg (name.tar.gz.gpg)
    if (archive::tool("gpg").isEmpty()) {
        skip("password-encrypted tar archives (gpg isn't installed)");
    } else {
        QString g = home_path("gpg");
        makedirs(join(g, "src/inner"), true);
        write_text(join(g, "src/inner/a.txt"), "hello");
        write_text(join(g, "src/top.txt"), "hello");
        const QString pw = "pw 'one' \"two\"";
        auto fmt_of = [](const QString &id) {
            for (const archive::Format &f : archive::formats())
                if (f.id == id)
                    return f;
            return archive::Format{};
        };
        Task gt("gpg", [](Task *) { return QVariant(); });
        auto spec_of = [&](const QString &id, const QString &name, const QStringList &rels) {
            archive::Spec sp;
            sp.format = fmt_of(id);
            sp.tool = sp.format.available.value(0);
            if (id != "tar")
                sp.level = 1;
            sp.base = join(g, "src");
            sp.rels = rels;
            sp.out = join(g, name);
            sp.password = pw;
            sp.total = 1 << 20;
            return sp;
        };
        QString gz = join(g, "sealed.tar.gz.gpg"), plain = join(g, "plain.tar.gpg");
        QString preview = archive::command_preview(spec_of("tar.gz", "sealed.tar.gz.gpg", {"inner", "top.txt"})) + "\n" +
                          archive::command_preview(spec_of("tar", "plain.tar.gpg", {"inner", "top.txt"}));
        bool made = fmt_of("tar.gz").password && fmt_of("tar").password;
        try {
            made = made && archive::compress(&gt, spec_of("tar.gz", "sealed.tar.gz.gpg", {"inner", "top.txt"})) == gz &&
                   archive::compress(&gt, spec_of("tar", "plain.tar.gpg", {"inner", "top.txt"})) == plain;
        } catch (const std::exception &e) {
            made = false;
            std::printf("  (compress: %s)\n", e.what());
        }
        // gpg itself, with the password on its own command line (the test's, not Kestrel's)
        auto gpg_names = [&](const QString &path, const QString &pass, const QString &unpack) {
            proc::Result r = proc::run({"sh", "-c",
                                        "gpg --batch --quiet --pinentry-mode loopback --no-symkey-cache --passphrase \"$1\" "
                                        "-d -- \"$2\" | " + unpack + " | tar tf -",
                                        "sh", pass, path},
                                       60000);
            QStringList names = QString::fromUtf8(r.out).split('\n', Qt::SkipEmptyParts);
            names.sort();
            return r.rc == 0 ? names : QStringList();
        };
        QStringList want{"inner/", "inner/a.txt", "top.txt"};
        check(made && gpg_names(gz, pw, "gzip -dc") == want && gpg_names(plain, pw, "cat") == want &&
                  gpg_names(gz, "wrong", "gzip -dc").isEmpty() && !read_file(plain).contains("top.txt"),
              "a tar.gz and a tar made with a password are gpg-encrypted (name.tar.gz.gpg): gpg opens them with that "
              "password only, and no file name can be read from them");
        check(preview.contains(" -c -1 | gpg --symmetric --cipher-algo AES256 -z 0 > sealed.tar.gz.gpg") &&
                  preview.contains("tar -cf - -- inner top.txt | gpg --symmetric") && preview.contains("password passed on a pipe") &&
                  !preview.contains("one"),
              "the command shown for a tar with a password pipes it through gpg, without the password (" +
                  preview.replace('\n', " ; ") + ")");

        auto [k, suf] = archive::kind(gz);
        bool kinds = k == "tar" && suf == "gz" && archive::gpg_encrypted(gz) && archive::gpg_encrypted(plain) &&
                     archive::archive_stem(gz) == "sealed" && archive::kind(join(g, "notes.txt.gpg")).first.isEmpty() &&
                     !archive::gpg_encrypted(join(g, "x.tar.gz")) && archive::probe(gz).encrypted && archive::opens_as_archive(gz);
        bool right = false, wrong = true;
        try {
            right = archive::password_opens(&gt, gz, pw);
            wrong = archive::password_opens(&gt, gz, "wrong");
        } catch (const std::exception &) {
        }
        check(kinds && right && !wrong,
              "a .tar.gz.gpg is a tar archive that is encrypted (a .txt.gpg isn't an archive), and the quick test tells "
              "its password from a wrong one");

        bool list_asks = false, extract_refuses = false, test_refuses = false;
        QStringList listed;
        bool all_marked = true;
        archive::TestResult tested;
        QString out = join(g, "out"), bad = join(g, "bad");
        makedirs(out, true);
        makedirs(bad, true);
        try {
            archive::list(&gt, gz, QString());
        } catch (const archive::WrongPassword &) {
            list_asks = true;
        } catch (const std::exception &) {
        }
        try {
            archive::extract(&gt, gz, bad, "wrong");
        } catch (const archive::WrongPassword &) {
            extract_refuses = true;
        } catch (const std::exception &) {
        }
        try {
            archive::test(&gt, plain, "wrong");
        } catch (const archive::WrongPassword &) {
            test_refuses = true;
        } catch (const std::exception &) {
        }
        try {
            for (const archive::Entry &e : archive::list(&gt, gz, pw)) {
                listed << e.path;
                all_marked = all_marked && e.encrypted;
            }
            archive::extract(&gt, plain, out, pw);
            tested = archive::test(&gt, gz, pw);
        } catch (const std::exception &e) {
            std::printf("  (reading: %s)\n", e.what());
        }
        listed.sort();
        check(list_asks && listed == want && all_marked && read_file(join(out, "inner/a.txt")) == "hello" &&
                  read_file(join(out, "top.txt")) == "hello" && tested.ok,
              "Kestrel lists (only with the password), extracts and tests a tar through gpg (" + listed.join(", ") + ")");
        check(extract_refuses && test_refuses && listdir(bad).isEmpty(),
              "a wrong password for a tar through gpg is reported as a wrong password (extract and test), and extracts "
              "nothing");

        // a write that fails part-way (a full disk): the archive of that name is kept, and nothing half-made is left
        {
            QFile f(join(g, "src/noise.bin"));
            QFile rnd("/dev/urandom");
            if (f.open(QIODevice::WriteOnly) && rnd.open(QIODevice::ReadOnly))
                f.write(rnd.read(3 << 20));
        }
        QByteArray before = read_file(gz);
        struct rlimit old_lim, lim;
        ::getrlimit(RLIMIT_FSIZE, &old_lim);
        lim = old_lim;
        lim.rlim_cur = 1 << 20;
        auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
        ::setrlimit(RLIMIT_FSIZE, &lim);
        bool threw = false;
        try {
            archive::compress(&gt, spec_of("tar.gz", "sealed.tar.gz.gpg", {"noise.bin"}));
        } catch (const std::exception &) {
            threw = true;
        }
        ::setrlimit(RLIMIT_FSIZE, &old_lim);
        ::signal(SIGXFSZ, old_handler);
        QStringList parts;
        for (const QString &n : listdir(g))
            if (n.startsWith(".kes-"))
                parts << n;
        check(threw && read_file(gz) == before && parts.isEmpty(),
              "a tar through gpg that fails part-way (a full disk) keeps the archive of that name as it was, and no "
              "half-made one (" + parts.join(", ") + ")");

        // the windows: Compress… names it .gpg; Open as Archive and Extract Here ask for its password
        {
            CompressDialog cd(w, {join(g, "src/top.txt")});
            QComboBox *fmt = cd.findChild<QComboBox *>();
            fmt->setCurrentIndex(fmt->findData("tar.gz"));
            auto *enc = cd.findChild<QGroupBox *>();   // the first: "Encrypt with a password"
            auto shows = [&](const QString &text) {
                for (QLabel *l : cd.findChildren<QLabel *>())
                    if (l->text() == text)
                        return true;
                return false;
            };
            bool offered = enc && enc->title().contains("password") && !enc->isHidden() && enc->isEnabled();
            bool plain_ext = shows(".tar.gz");
            if (enc)
                enc->setChecked(true);
            check(offered && plain_ext && shows(".tar.gz.gpg"),
                  "Compress… offers a password for tar.gz, and the archive's name then ends in .tar.gz.gpg");
        }
        ArchiveWindow *gw = ArchiveWindow::open(w, gz);
        Asked ga = drive({{pw}}, [&]() { return gw->loaded() ? QString("loaded") : QString(); });
        int shown = gw->tree->topLevelItemCount();
        gw->close();
        check(ga.prompts.size() == 1 && ga.prompts.value(0).contains("is encrypted") && shown == 2,
              "Open as Archive on a tar through gpg asks for its password, then shows what's inside");
        w->statusBar()->clearMessage();
        archive_ui::extract_here(w, gz);
        ga = drive({{"wrong"}, {pw}}, [&]() {
            QString m = w->statusBar()->currentMessage();
            return m.startsWith("Extracted") ? m : QString();
        });
        check(ga.prompts.size() == 2 && ga.prompts.value(1).contains("Wrong password") &&
                  read_file(join(g, "sealed/inner/a.txt")) == "hello",
              "Extract Here on a tar through gpg asks for its password (again when wrong) and extracts it into a "
              "folder named without .tar.gz.gpg");
        proc::run({"gpgconf", "--kill", "gpg-agent"}, 10000);   // the one gpg started for the test's home
    }

    // ---- checksum files made while a tar archive is compressed
    {
        QString c = home_path("sums");
        makedirs(join(c, "src/inner"), true);
        write_text(join(c, "src/inner/a.txt"), "hello");
        write_text(join(c, "src/top.txt"), "hello");
        {
            QFile f(join(c, "src/noise.bin"));
            QFile rnd("/dev/urandom");
            if (f.open(QIODevice::WriteOnly) && rnd.open(QIODevice::ReadOnly))
                f.write(rnd.read(3 << 20));
        }
        auto fmt_of = [](const QString &id) {
            for (const archive::Format &f : archive::formats())
                if (f.id == id)
                    return f;
            return archive::Format{};
        };
        auto spec_of = [&](const QString &id, const QString &name, const QStringList &algos, bool one = false,
                           const QStringList &rels = {"inner", "top.txt", "noise.bin"}) {
            archive::Spec sp;
            sp.format = fmt_of(id);
            sp.tool = sp.format.available.value(0);
            if (id != "tar")
                sp.level = 1;
            sp.base = join(c, "src");
            sp.rels = rels;
            sp.out = join(c, name);
            sp.checksums = algos;
            sp.checksum_one_file = one;
            sp.total = 3 << 20;
            return sp;
        };
        // what the tools say the archive's hash is (the test's own reading of the finished file)
        auto tool_sum = [](const QString &tool, const QString &path) {
            return QString::fromUtf8(proc::run({tool, "--", path}, 60000).out).section(' ', 0, 0);
        };
        // every entry of a checksum file checks out against the archive beside it, by its name there
        auto verifies = [](const QString &sumfile, const QString &archive_name, int entries) {
            QList<hashcheck::Entry> es = hashcheck::parse(sumfile);
            bool ok = es.size() == entries;
            for (const hashcheck::Entry &e : es)
                ok = ok && e.name == archive_name && hashcheck::verify(e).status == "ok";
            return ok;
        };
        Task ct("sums", [](Task *) { return QVariant(); });
        QString gz = join(c, "s.tar.gz"), plain = join(c, "p.tar");
        QString preview = archive::command_preview(spec_of("tar.gz", "s.tar.gz", {"crc32", "sha256"}));
        bool made = true;
        try {
            archive::compress(&ct, spec_of("tar.gz", "s.tar.gz", {"crc32", "sha256"}));
            archive::compress(&ct, spec_of("tar", "p.tar", {"md5", "sha512"}, true));
        } catch (const std::exception &e) {
            made = false;
            std::printf("  (compress: %s)\n", e.what());
        }
        QString sha = read_file(gz + ".sha256");
        check(made && sha == tool_sum("sha256sum", gz) + "  s.tar.gz\n" && verifies(gz + ".sha256", "s.tar.gz", 1) &&
                  verifies(gz + ".sfv", "s.tar.gz", 1) && read_file(gz + ".sfv").contains("s.tar.gz "),
              "a tar.gz compressed with checksums gets s.tar.gz.sha256 and s.tar.gz.sfv beside it, with the hashes of "
              "the finished archive (sha256sum's own) under its name (" + sha.trimmed() + ")");
        QString bsd = read_file(join(c, "p.tar-CHECKSUM"));
        check(made && bsd.contains("MD5 (p.tar) = " + tool_sum("md5sum", plain)) &&
                  bsd.contains("SHA512 (p.tar) = " + tool_sum("sha512sum", plain)) &&
                  verifies(join(c, "p.tar-CHECKSUM"), "p.tar", 2),
              "a plain tar compressed with checksums in one file gets p.tar-CHECKSUM with each algorithm as a BSD tag");
        check(preview.contains("(and s.tar.gz.sfv, s.tar.gz.sha256, made from the archive as it's written)"),
              "the command shown names the checksum files that will be made (" + preview.replace('\n', " ; ") + ")");

        if (!archive::tool("gpg").isEmpty()) {
            archive::Spec sp = spec_of("tar.gz", "e.tar.gz.gpg", {"sha256"});
            sp.password = "pw";
            QString e = join(c, "e.tar.gz.gpg");
            bool ok = true;
            try {
                archive::compress(&ct, sp);
            } catch (const std::exception &) {
                ok = false;
            }
            check(ok && read_file(e + ".sha256") == tool_sum("sha256sum", e) + "  e.tar.gz.gpg\n",
                  "a tar.gz through gpg gets the checksum of the encrypted file, e.tar.gz.gpg.sha256");
            proc::run({"gpgconf", "--kill", "gpg-agent"}, 10000);
        } else {
            skip("checksums of a tar through gpg (gpg isn't installed)");
        }

        auto parts = [&]() {
            QStringList out;
            for (const QString &n : listdir(c))
                if (n.startsWith(".kes-"))
                    out << n;
            return out;
        };
        // a write that fails part-way (a full disk): the archive and its old checksum file are kept, the error said.
        // 3 MB: the programs end on their own; 64 MB: they're still being fed when the file stops taking more.
        {
            QFile f(join(c, "src/big.bin"));
            QFile rnd("/dev/urandom");
            if (f.open(QIODevice::WriteOnly) && rnd.open(QIODevice::ReadOnly))
                for (int i = 0; i < 8; ++i)
                    f.write(rnd.read(8 << 20));
        }
        for (const QString &rel : {"noise.bin", "big.bin"}) {
            QByteArray before = read_file(gz), sum_before = read_file(gz + ".sha256");
            struct rlimit old_lim, lim;
            ::getrlimit(RLIMIT_FSIZE, &old_lim);
            lim = old_lim;
            lim.rlim_cur = 1 << 20;
            auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
            ::setrlimit(RLIMIT_FSIZE, &lim);
            QString said;
            try {
                archive::compress(&ct, spec_of("tar.gz", "s.tar.gz", {"crc32", "sha256"}, false, {rel}));
            } catch (const std::exception &e) {
                said = QString::fromStdString(e.what());
            }
            ::setrlimit(RLIMIT_FSIZE, &old_lim);
            ::signal(SIGXFSZ, old_handler);
            check(said.contains("File too large") && read_file(gz) == before && read_file(gz + ".sha256") == sum_before &&
                      parts().isEmpty(),
                  rel + ": a tar.gz with checksums that fails part-way (a full disk) says why, and keeps the archive "
                        "and its checksum file as they were, with nothing half-made (" + said + ")");
        }

        // ✕ part-way: no archive, no checksum file, no part
        Task cancel_task("sums", [](Task *) { return QVariant(); });
        std::atomic<bool> stopped{false};
        std::thread job([&]() {
            try {
                archive::compress(&cancel_task, spec_of("tar.gz", "c.tar.gz", {"sha256"}, false, {"big.bin"}));
            } catch (const Cancelled &) {
                stopped = true;
            } catch (const std::exception &) {
            }
        });
        bool started = wait_for([&]() { return !parts().isEmpty(); }, 30000);
        cancel_task.cancelled = true;
        job.join();
        check(started && stopped && !lexists(join(c, "c.tar.gz")) && !lexists(join(c, "c.tar.gz.sha256")) && parts().isEmpty(),
              "a tar.gz with checksums cancelled part-way leaves no archive, no checksum file and no part");

        // Compress…: offered for the tar formats only
        CompressDialog cd(w, {join(c, "src/top.txt")});
        QComboBox *fmt = cd.findChild<QComboBox *>();
        fmt->setCurrentIndex(fmt->findData("tar.zst"));
        QGroupBox *sums = nullptr;
        for (QGroupBox *b : cd.findChildren<QGroupBox *>())
            if (b->title() == "Create a checksum file")
                sums = b;
        bool shown = sums && !sums->isHidden();
        if (sums)
            sums->setChecked(true);
        archive::Spec on = cd.spec();
        QString other;
        for (const archive::Format &f : archive::formats())
            if (!archive::is_tar(f) && !f.single && !f.available.isEmpty() && other.isEmpty())
                other = f.id;
        bool hidden_other = true;
        if (!other.isEmpty()) {
            fmt->setCurrentIndex(fmt->findData(other));
            hidden_other = sums && sums->isHidden() && cd.spec().checksums.isEmpty();
        }
        check(shown && on.format.id == "tar.zst" && !on.checksums.isEmpty() && hidden_other,
              "Compress… offers a checksum file for tar formats (with the algorithms chosen), and not for " +
                  (other.isEmpty() ? QString("other formats") : other));
    }

    // ---- Compress…'s volume sizes: standard ones by what they fit (exact bytes), or Custom… with a unit
    if (archive::tool("7z").isEmpty()) {
        skip("volume sizes (7z isn't installed)");
    } else {
        QString c = home_path("vols");
        makedirs(join(c, "src"), true);
        QByteArray noise;
        for (int i = 0; i < 600 * 1024; ++i)
            noise += char(QRandomGenerator::global()->bounded(256));
        write_text(join(c, "src/noise.bin"), noise);
        auto *cd = new CompressDialog(w, {join(c, "src/noise.bin")});
        cd->show();   // (what's offered depends on what's shown)
        QComboBox *fmt = cd->findChild<QComboBox *>();
        fmt->setCurrentIndex(fmt->findData("7z"));
        QGroupBox *split = nullptr;
        for (QGroupBox *b : cd->findChildren<QGroupBox *>())
            if (b->title() == "Split into volumes")
                split = b;
        QComboBox *sizes = nullptr;
        QSpinBox *num = nullptr;
        if (split) {
            split->setChecked(true);
            for (QComboBox *b : split->findChildren<QComboBox *>())
                if (b->findData("dvd") >= 0)
                    sizes = b;
            num = split->findChild<QSpinBox *>();
        }
        bool dvd = false, custom_shown = false;
        if (sizes && num) {
            sizes->setCurrentIndex(sizes->findData("dvd"));
            dvd = cd->spec().volume_bytes == 4700372992LL - 4 * 1024 * 1024 && num->isHidden() &&
                  archive::command_preview(cd->spec()).contains("-v4696178688b");
            sizes->setCurrentIndex(sizes->findData("custom"));
            custom_shown = !num->isHidden();
        }
        QStringList labels;
        for (int i = 0; sizes && i < sizes->count(); ++i)
            labels << sizes->itemText(i);
        check(dvd && custom_shown && labels.contains("700 MB (CD)") && labels.contains("4 GB (FAT32 USB stick)") &&
                  labels.contains("50 GB (dual-layer Blu-ray)") && labels.last() == "Custom…",
              "Compress… offers volume sizes by what they fit, a DVD's exact (4,700,372,992 bytes less its file "
              "system), passed in bytes, and Custom… shows the number and unit (" + labels.join(", ") + ")");
        archive::Spec sp;
        if (num) {
            num->setValue(200);
            for (QComboBox *b : split->findChildren<QComboBox *>())
                if (b->findText("KB") >= 0)
                    b->setCurrentText("KB");
            sp = cd->spec();
            if (auto *bb = cd->findChild<QDialogButtonBox *>())   // OK: the options are saved
                bb->button(QDialogButtonBox::Ok)->click();
        }
        delete cd;
        bool remembered = false;
        {
            CompressDialog again(w, {join(c, "src/noise.bin")});
            again.show();
            QComboBox *f2 = again.findChild<QComboBox *>();
            f2->setCurrentIndex(f2->findData("7z"));
            remembered = again.spec().volume_bytes == 200 * 1024 || !split;
            QComboBox *s2 = nullptr;
            for (QComboBox *b : again.findChildren<QComboBox *>())
                if (b->findData("dvd") >= 0)
                    s2 = b;
            remembered = remembered && s2 && s2->currentData() == "custom";
        }
        sp.level = 0;
        Task vt("vols", [](Task *) { return QVariant(); });
        QStringList made;
        try {
            archive::compress(&vt, sp);
            for (const QString &n : listdir(dirname(sp.out)))
                if (n.startsWith(basename(sp.out)))
                    made << n;
        } catch (const std::exception &e) {
            made << QString("error: ") + e.what();
        }
        made.sort();
        bool sized = made.size() == 4 && QFileInfo(join(dirname(sp.out), made.first())).size() == 200 * 1024;
        check(sp.volume_bytes == 200 * 1024 && remembered && sized,
              "a custom size in KB makes volumes of exactly that size, and the choice is remembered for the format (" +
                  made.join(", ") + ")");
    }

    QString z7 = archive::tool("7z");
    if (z7.isEmpty()) {
        skip("Extract All and the password checks (7z isn't installed)");
        finish();
    }

    // ---- the quick password test
    QString base = home_path("arc");
    makedirs(join(base, "src/inner"), true);
    write_text(join(base, "src/inner/a.txt"), "hello");
    write_text(join(base, "src/top.txt"), "hello");   // two items at the top: extracted into a folder of their own
    QString content = join(base, "src");
    auto make7 = [&](const QString &name, const QString &pw, bool hidden_names = false) {
        QStringList argv{z7, "a", "-t7z", "-mx1"};
        if (!pw.isEmpty())
            argv << "-p" + pw << (hidden_names ? "-mhe=on" : "-mhe=off");
        return make(argv << join(base, name) << "inner" << "top.txt", content);
    };
    bool made = make7("names.7z", "one") && make7("hidden.7z", "one", true) &&
                make({z7, "a", "-tzip", "-pone", join(base, "z.zip"), "inner"}, content);
    Task probe_task("probe", [](Task *) { return QVariant(); });
    auto opens = [&](const QString &name, const QString &pw) { return archive::password_opens(&probe_task, join(base, name), pw); };
    check(made && opens("names.7z", "one") && !opens("names.7z", "two") && opens("hidden.7z", "one") &&
              !opens("hidden.7z", "two") && opens("z.zip", "one") && !opens("z.zip", "two"),
          "a quick test tells the password that opens a 7z (also with encrypted names) or zip archive from a wrong one");
    for (const QString &t : {"rar", "zpaq"}) {
        if (archive::tool(t).isEmpty() || (t == QString("rar") && archive::tool("unrar").isEmpty())) {
            skip(QString("the quick password test for %1 (it isn't installed)").arg(t));
            continue;
        }
        bool ok = t == QString("rar")
                      ? make({archive::tool("rar"), "a", "-ponE", join(base, "p.rar"), "inner"}, content) &&
                            make({archive::tool("rar"), "a", "-hponE", join(base, "ph.rar"), "inner"}, content) &&
                            opens("p.rar", "onE") && !opens("p.rar", "one") && opens("ph.rar", "onE") && !opens("ph.rar", "x")
                      : make({archive::tool("zpaq"), "a", join(base, "p.zpaq"), "inner", "-key", "onE"}, content) &&
                            opens("p.zpaq", "onE") && !opens("p.zpaq", "one");
        check(ok, QString("...and for %1 archives").arg(t));
    }

    check(!keyring::available(), "with no Secret Service answering, Kestrel sees no keyring");

    // ---- Extract All: one password typed is tried on the rest
    QString all = home_path("all");
    makedirs(all, true);
    auto in_all = [&](const QString &name, const QString &pw, bool hidden = false) {
        return make7("../all/" + name, pw, hidden);
    };
    made = in_all("a1.7z", "one") && in_all("a2.7z", "one", true) && in_all("a3.7z", "two") && in_all("a4.7z", "two") &&
           in_all("plain.7z", "");
    write_text(join(all, "broken.7z"), "this isn't an archive");
    QStringList set{join(all, "a1.7z"), join(all, "a2.7z"), join(all, "broken.7z"), join(all, "a3.7z"),
                    join(all, "a4.7z"), join(all, "plain.7z")};
    archive_ui::extract_all_here(w, archive_ui::archives_of(set));
    Asked r = drive({{"one"}, {"wrong"}, {"two"}}, summary_text);
    auto extracted = [&](const QString &stem) { return read_file(join(all, stem + "/top.txt")) == "hello"; };
    check(made && r.prompts.size() == 3 && r.prompts.value(0).contains("is encrypted") &&
              r.prompts.value(1).contains("entered before doesn't open") && r.prompts.value(2).contains("Wrong password"),
          "Extract All asks once for the first encrypted archive, again only for one that password doesn't open, and "
          "again after a wrong one (asked: " + r.prompts.join(" | ") + ")");
    check(extracted("a1") && extracted("a2") && extracted("a3") && extracted("a4") && extracted("plain"),
          "...each archive is extracted into its own folder next to it (a2 and a4 with the password typed before)");
    check(r.told.contains("Extracted 5 of 6 archives") && r.told.contains("✗ broken.7z") && r.told.contains("✓ a4.7z") &&
              !exists(join(all, "broken")),
          "...and one summary at the end says what was extracted and what failed (a damaged archive leaves no folder)");
    check(!r.offered.contains(true), "no keyring running: the password window has no Remember box");
    check(r.free, "the password window a job asks with is a free window (not modal, not kept above Kestrel)");

    // a cancelled password window: that archive is skipped, the rest go on
    QString skip_dir = home_path("skip");
    makedirs(skip_dir, true);
    made = make7("../skip/s1.7z", "three") && make7("../skip/s2.7z", "");
    archive_ui::extract_all_here(w, {join(skip_dir, "s1.7z"), join(skip_dir, "s2.7z")});
    r = drive({{}}, summary_text);
    check(made && r.prompts.size() == 1 && r.told.contains("✗ s1.7z — no password") && r.told.contains("✓ s2.7z") &&
              !exists(join(skip_dir, "s1")) && read_file(join(skip_dir, "s2/top.txt")) == "hello",
          "cancelling an archive's password window skips that archive and goes on with the next");

    // ✕ while it waits for a password: the window closes and the job stops, telling what was done
    QString stop_dir = home_path("stop");
    makedirs(stop_dir, true);
    made = make7("../stop/t0.7z", "") && make7("../stop/t1.7z", "four") && make7("../stop/t2.7z", "");
    Task *job = archive_ui::extract_all(w, {join(stop_dir, "t0.7z"), join(stop_dir, "t1.7z"), join(stop_dir, "t2.7z")},
                                        {{"dest", QString()}, {"subfolder", true}, {"overwrite", "rename"}});
    bool asked = wait_for([]() { return prompt() != nullptr; }, 30000);
    QString waiting = job ? job->text : QString();
    if (job)
        job->cancel();
    bool closed = wait_for([]() { return prompt() == nullptr; });
    r = drive({}, summary_text);
    check(made && asked && waiting.contains("Waiting for the password for t1.7z") && closed &&
              r.told.contains("Extracted 1 of 3 archives (stopped)") && r.told.contains("t2.7z — not extracted") &&
              read_file(join(stop_dir, "t0/top.txt")) == "hello" && !exists(join(stop_dir, "t2")),
          "✕ while a job waits for a password closes its window and stops it; the summary tells what was extracted");

    // one archive: Extract Here as before (the status bar), through the same job
    QString one_dir = home_path("one");
    makedirs(one_dir, true);
    make7("../one/o.7z", "five");
    w->statusBar()->clearMessage();
    archive_ui::extract_here(w, join(one_dir, "o.7z"));
    r = drive({{"six"}, {"five"}}, [&]() { return w->statusBar()->currentMessage().startsWith("Extracted") ? w->statusBar()->currentMessage() : QString(); });
    check(r.prompts.size() == 2 && r.prompts.value(1).contains("Wrong password") && !summary() &&
              read_file(join(one_dir, "o/top.txt")) == "hello",
          "Extract Here on one encrypted archive asks for its password (again when wrong), with no summary window");

    // ---- looking inside: listing, extracting single items, testing (the engine)
    QString look = home_path("look");
    makedirs(look, true);
    QByteArray noise(300000, '\0');
    for (int i = 0; i < noise.size(); ++i)
        noise[i] = char((unsigned(i) * 7919u + unsigned(i) / 13u) & 0xffu);
    write_text(join(base, "src/inner/big.bin"), noise);
    auto make_in = [&](const QString &name, QStringList argv) { return make(argv << join(look, name) << "inner" << "top.txt", content); };
    QMap<QString, bool> formats;   // archive name -> made
    formats["l.7z"] = make_in("l.7z", {z7, "a", "-t7z", "-mx0"});
    formats["l.zip"] = make_in("l.zip", {z7, "a", "-tzip", "-mx0"});
    {
        proc::Options o;
        o.cwd = content;
        formats["l.tar.gz"] = proc::run({"tar", "czf", join(look, "l.tar.gz"), "inner", "top.txt"}, 60000, o).rc == 0;
    }
    if (!archive::tool("rar").isEmpty() && !archive::tool("unrar").isEmpty())
        formats["l.rar"] = make_in("l.rar", {archive::tool("rar"), "a", "-m0"});
    if (!archive::tool("zpaq").isEmpty())
        formats["l.zpaq"] = make({archive::tool("zpaq"), "a", join(look, "l.zpaq"), "inner", "top.txt"}, content);
    Task look_task("look", [](Task *) { return QVariant(); });
    for (auto it = formats.begin(); it != formats.end(); ++it) {
        QString arc = join(look, it.key());
        QStringList files;
        qint64 a_size = -1;
        bool listed = false;
        try {
            for (const archive::Entry &e : archive::list(&look_task, arc)) {
                if (!e.dir)
                    files << e.path;
                if (e.path == "inner/a.txt")
                    a_size = e.size;
            }
            listed = true;
        } catch (const std::exception &) {
        }
        files.sort();
        check(it.value() && listed && files == QStringList({"inner/a.txt", "inner/big.bin", "top.txt"}) && a_size == 5,
              it.key() + ": its contents are listed, with their paths and sizes (" + files.join(", ") + ")");
        QString out = join(look, "out-" + it.key());
        makedirs(out, true);
        write_text(join(out, "a.txt"), "mine");
        QStringList made, made2;
        try {
            made = archive::extract_members(&look_task, arc, out, QString(), {"inner/a.txt"});
            made2 = archive::extract_members(&look_task, arc, out, QString(), {"inner/"});
        } catch (const std::exception &e) {
            made << QString("error: ") + e.what();
        }
        QStringList left = listdir(out);
        left.sort();
        check(made == QStringList{join(out, "a (2).txt")} && read_file(join(out, "a (2).txt")) == "hello" &&
                  read_file(join(out, "a.txt")) == "mine" && made2 == QStringList{join(out, "inner")} &&
                  read_file(join(out, "inner/big.bin")) == noise && !exists(join(out, "top.txt")) &&
                  left == QStringList({"a (2).txt", "a.txt", "inner"}),
              it.key() + ": a single file is extracted right into the folder (a taken name gets a number, nothing is "
                         "replaced), a folder with what's inside, nothing else and no part folder left (" +
                  left.join(", ") + ")");
    }

    // a big archive: its listing (several MB) is read whole, not just its last part
    {
        QString many = join(base, "many");
        const int folders = 250, per = 100;
        QSet<QString> want;
        for (int d = 0; d < folders; ++d) {
            QString dir = QString("tree/folder_%1").arg(d, 3, 10, QChar('0'));
            makedirs(join(many, dir), true);
            for (int f = 0; f < per; ++f) {
                QString name = QString("%1/a_rather_long_file_name_to_make_the_listing_longer_%2.txt").arg(dir).arg(f, 3, 10, QChar('0'));
                write_text(join(many, name), "x");
                want << name;
            }
        }
        QMap<QString, bool> big;   // archive name -> made
        big["big.7z"] = make({z7, "a", "-t7z", "-mx0", join(look, "big.7z"), "tree"}, many);
        big["big.zip"] = make({z7, "a", "-tzip", "-mx0", join(look, "big.zip"), "tree"}, many);
        big["big.tar.gz"] = make({"tar", "czf", join(look, "big.tar.gz"), "tree"}, many);
        if (!archive::tool("rar").isEmpty() && !archive::tool("unrar").isEmpty())
            big["big.rar"] = make({archive::tool("rar"), "a", "-m0", "-r", join(look, "big.rar"), "tree"}, many);
        if (!archive::tool("zpaq").isEmpty())
            big["big.zpaq"] = make({archive::tool("zpaq"), "a", join(look, "big.zpaq"), "tree"}, many);
        for (auto it = big.begin(); it != big.end(); ++it) {
            QSet<QString> files;
            bool top = false;
            QString error;
            try {
                for (const archive::Entry &e : archive::list(&look_task, join(look, it.key()))) {
                    if (!e.dir)
                        files << e.path;
                    top = top || e.path == "tree/folder_000/";
                }
            } catch (const std::exception &e) {
                error = e.what();
            }
            check(it.value() && files == want && top,
                  it.key() + ": all " + QString::number(want.size()) + " files of a big archive are listed, the first " +
                      "folder too (" + QString::number(files.size()) + " listed" + (error.isEmpty() ? "" : "; " + error) + ")");
        }
        // the quick password test finds a file to test in a big listing (finding none, it takes any password)
        QElapsedTimer took;
        took.start();
        bool made = make({z7, "a", "-t7z", "-mx0", "-psecret", join(look, "big-enc.7z"), "tree"}, many);
        qint64 make_ms = took.restart();
        bool right = false, wrong = true;
        QString error;
        try {
            right = archive::password_opens(&look_task, join(look, "big-enc.7z"), "secret");
            wrong = archive::password_opens(&look_task, join(look, "big-enc.7z"), "nope");
        } catch (const std::exception &e) {
            error = QString::fromStdString(e.what());
        }
        check(made && right && !wrong,
              QString("big encrypted 7z: the quick password test takes the right password and refuses a wrong one "
                      "(made: %1 in %2 ms; right: %3, wrong: %4 in %5 ms%6)")
                  .arg(made ? "yes" : "no", QString::number(make_ms), right ? "opens" : "doesn't open",
                       wrong ? "opens" : "doesn't open", QString::number(took.elapsed()), error.isEmpty() ? "" : "; " + error));
    }

    // a write that fails part-way (a full disk): nothing half-made, no part folder
    {
        QString out = join(look, "full");
        makedirs(out, true);
        struct rlimit old_lim, lim;
        ::getrlimit(RLIMIT_FSIZE, &old_lim);
        lim = old_lim;
        lim.rlim_cur = 100000;
        auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
        ::setrlimit(RLIMIT_FSIZE, &lim);
        bool threw = false;
        try {
            archive::extract_members(&look_task, join(look, "l.7z"), out, QString(), {"top.txt", "inner/"});
        } catch (const std::exception &) {
            threw = true;
        }
        ::setrlimit(RLIMIT_FSIZE, &old_lim);
        ::signal(SIGXFSZ, old_handler);
        check(threw && listdir(out).isEmpty(),
              "extracting single items that fails part-way (a full disk) leaves nothing in the folder, not even what "
              "was whole (" + listdir(out).join(", ") + ")");
    }

    // a link that stays inside the archive, but would lead out of the folder once its item is taken out on its own
    {
        QString src = join(look, "linksrc");
        makedirs(join(src, "a/b"), true);
        write_text(join(src, "a/secret.txt"), "x");
        ::symlink("../../a/secret.txt", join(src, "a/b/up").toUtf8().constData());
        ::symlink("../x", join(src, "a/b/fine").toUtf8().constData());
        proc::Options o;
        o.cwd = src;
        bool tarred = proc::run({"tar", "czf", join(look, "links.tar.gz"), "a"}, 60000, o).rc == 0;
        QString out = join(look, "links-out");
        makedirs(out, true);
        QList<archive::DroppedLink> dropped;
        try {
            archive::extract_members(&look_task, join(look, "links.tar.gz"), out, QString(), {"a/b/"}, &dropped);
        } catch (const std::exception &) {
        }
        check(tarred && isdir(join(out, "b")) && !lexists(join(out, "b/up")) && islink(join(out, "b/fine")) &&
                  dropped.size() == 1 && dropped.value(0).target == "../../a/secret.txt",
              "a folder taken out on its own leaves out a symlink that would then lead out of the folder, and keeps "
              "one that stays inside");
    }

    // testing
    {
        QByteArray good = read_file(join(look, "l.7z"));
        QByteArray bad = good;
        for (int i = 100000; i < 100200; ++i)   // inside big.bin, stored as it is
            bad[i] = char(bad[i] ^ 0x5a);
        write_text(join(look, "bad.7z"), bad);
        QByteArray gz = read_file(join(look, "l.tar.gz"));
        for (int i = int(gz.size() / 2); i < gz.size() / 2 + 64; ++i)
            gz[i] = char(gz[i] ^ 0x5a);
        write_text(join(look, "bad.tar.gz"), gz);
        archive::TestResult t_good, t_bad, t_zip, t_gz, t_badgz;
        bool wrong = false;
        try {
            t_good = archive::test(&look_task, join(look, "l.7z"));
            t_bad = archive::test(&look_task, join(look, "bad.7z"));
            t_zip = archive::test(&look_task, join(look, "l.zip"));
            t_gz = archive::test(&look_task, join(look, "l.tar.gz"));
            t_badgz = archive::test(&look_task, join(look, "bad.tar.gz"));
            archive::test(&look_task, join(base, "hidden.7z"), "nope");
        } catch (const archive::WrongPassword &) {
            wrong = true;
        } catch (const std::exception &) {
        }
        check(t_good.ok && t_zip.ok && t_gz.ok && !t_bad.ok && t_bad.damaged.contains("inner/big.bin") && !t_badgz.ok &&
                  !t_badgz.details.isEmpty() && wrong,
              "Test tells a sound archive (7z, zip, tar.gz) from a damaged one, names the damaged file where the tool "
              "does, and a wrong password apart (damaged: " + t_bad.damaged.join(", ") + ")");
    }

    // the menu
    QStringList one_menu = labels({join(look, "l.7z")});
    QStringList two_menu = labels({join(look, "l.7z"), join(look, "l.zip")});
    write_text(join(look, "note.txt.gz"), "x");
    QStringList gz_menu = labels({join(look, "note.txt.gz")});
    check(one_menu.contains("Open as Archive") && one_menu.contains("Test Archive") && two_menu.contains("Test Archives") &&
              !gz_menu.contains("Open as Archive") && gz_menu.contains("Test Archive"),
          "the menu offers Open as Archive and Test Archive for an archive, Test Archives for several; a single "
          "compressed file can be tested but not opened as an archive");

    // Test Archives: one job, one message
    write_text(join(look, "enc.7z"), read_file(join(base, "names.7z")));   // password "one"
    archive_ui::test_archives(w, {join(look, "l.7z"), join(look, "bad.7z"), join(look, "enc.7z")});
    auto test_text = []() {
        for (QWidget *x : QApplication::topLevelWidgets())
            if (auto *b = qobject_cast<QMessageBox *>(x); b && b->isVisible() && b->objectName() == "test_summary") {
                QString t = b->text();
                b->close();
                return t;
            }
        return QString();
    };
    r = drive({{"one"}}, test_text);
    check(r.prompts.size() == 1 && r.told.contains("2 of 3 archives are OK") && r.told.contains("✗ bad.7z — damaged") &&
              r.told.contains("✓ enc.7z"),
          "Test Archives tests each (asking for an encrypted one's password) and tells the results in one message");

    // Open as Archive: the window
    ArchiveWindow *aw = ArchiveWindow::open(w, join(base, "hidden.7z"));   // encrypted names: asks first
    r = drive({{"one"}}, [&]() { return aw->loaded() ? QString("loaded") : QString(); });
    QStringList names;
    std::function<void(QTreeWidgetItem *)> collect = [&](QTreeWidgetItem *it) {
        names << it->data(0, Qt::UserRole + 1).toString();
        for (int i = 0; i < it->childCount(); ++i)
            collect(it->child(i));
    };
    for (int i = 0; i < aw->tree->topLevelItemCount(); ++i)
        collect(aw->tree->topLevelItem(i));
    names.sort();
    check(r.prompts.size() == 1 && names == QStringList({"inner/", "inner/a.txt", "top.txt"}) && is_free_window(aw) &&
              ArchiveWindow::open(w, join(base, "hidden.7z")) == aw,
          "Open as Archive asks for the password of an archive with encrypted names, then shows its folders and "
          "files in a free window (opening it again raises the same one) (" + names.join(", ") + ")");
    QString pick = join(look, "picked");
    makedirs(pick, true);
    auto select = [&](const QStringList &members) {
        aw->tree->clearSelection();
        std::function<void(QTreeWidgetItem *)> sel = [&](QTreeWidgetItem *it) {
            if (members.contains(it->data(0, Qt::UserRole + 1).toString()))
                it->setSelected(true);
            for (int i = 0; i < it->childCount(); ++i)
                sel(it->child(i));
        };
        for (int i = 0; i < aw->tree->topLevelItemCount(); ++i)
            sel(aw->tree->topLevelItem(i));
    };
    select({"inner/a.txt", "top.txt"});
    bool enabled = aw->extract_to->isEnabled() && aw->extract_here->isEnabled();
    aw->extract_selected(pick);
    r = drive({}, [&]() { return exists(join(pick, "top.txt")) && exists(join(pick, "a.txt")) ? QString("done") : QString(); });
    check(enabled && r.prompts.isEmpty() && read_file(join(pick, "a.txt")) == "hello" && read_file(join(pick, "top.txt")) == "hello" &&
              listdir(pick).size() == 2,
          "the selected files are extracted from the window into the chosen folder, with the password found when it "
          "opened (no second ask)");
    aw->close();

    // encrypted files, visible names: opens without asking, asks when one is extracted, and only once
    ArchiveWindow *fw = ArchiveWindow::open(w, join(base, "names.7z"));
    r = drive({}, [&]() { return fw->loaded() ? QString("loaded") : QString(); });
    bool opened_free = r.prompts.isEmpty() && fw->status->text().contains("encrypted");
    aw = fw;
    QString pick2 = join(look, "picked2");
    makedirs(pick2, true);
    select({"inner/"});
    fw->extract_selected(pick2);
    Asked first = drive({{"one"}}, [&]() { return exists(join(pick2, "inner/a.txt")) ? QString("done") : QString(); });
    select({"top.txt"});
    fw->extract_selected(pick2);
    Asked second = drive({}, [&]() { return exists(join(pick2, "top.txt")) ? QString("done") : QString(); });
    check(opened_free && first.prompts.size() == 1 && second.prompts.isEmpty() &&
              read_file(join(pick2, "inner/a.txt")) == "hello" && read_file(join(pick2, "top.txt")) == "hello",
          "an archive with encrypted files but visible names opens without asking, asks when an encrypted file is "
          "extracted, and not again while the window is open");
    fw->close();

    // ---- opening a file from inside the archive, and dragging items out
    {
        // the default app for text: a script noting what it was given
        QString marker = home_path("opened.txt"), script = home_path("bin/note-open");
        makedirs(home_path("bin"), true);
        write_text(script, ("#!/bin/sh\necho \"$1\" > '" + marker + "'\n").toUtf8());
        ::chmod(enc(script).constData(), 0755);
        makedirs(home_path(".local/share/applications"), true);
        write_text(home_path(".local/share/applications/kes-test-open.desktop"),
                   ("[Desktop Entry]\nType=Application\nName=Note\nExec=" + script + " %f\nMimeType=text/plain;\n").toUtf8());
        makedirs(home_path(".config"), true);
        write_text(home_path(".config/mimeapps.list"), "[Default Applications]\ntext/plain=kes-test-open.desktop\n");
        spin(500);   // (GIO notices new applications by watching their folders)
        bool made_plain = make7("plain.7z", "");
        ArchiveWindow *pw = ArchiveWindow::open(w, join(base, "plain.7z"));
        wait_for([&]() { return pw->loaded(); });
        aw = pw;
        QTreeWidgetItem *top = nullptr;
        for (int i = 0; i < pw->tree->topLevelItemCount(); ++i)
            if (pw->tree->topLevelItem(i)->data(0, Qt::UserRole + 1).toString() == "top.txt")
                top = pw->tree->topLevelItem(i);
        if (top)
            Q_EMIT pw->tree->itemActivated(top, 0);
        bool opened = wait_for([&]() { return exists(marker); }, 10000);
        QString copy = QString::fromUtf8(read_file(marker)).trimmed();
        struct stat st;
        check(made_plain && opened && copy.startsWith(ArchiveWindow::temp_root() + "/") && copy.endsWith("/top.txt") &&
                  read_file(copy) == "hello" && lstat_(copy, st) && !(st.st_mode & 0222),
              "a file double-clicked in the archive window is extracted to a temporary copy (read-only) and opened "
              "with its app (" + copy + ")");

        // dragged out: Kestrel takes the items themselves, extracted straight into the folder it's dropped on
        auto temp_count = [&]() { return isdir(ArchiveWindow::temp_root()) ? listdir(ArchiveWindow::temp_root()).size() : 0; };
        QString drop_dir = home_path("dropped-out");
        makedirs(drop_dir, true);
        w->pane()->set_path(drop_dir);
        wait_for([&]() { return w->pane()->path == drop_dir; });
        int temps = temp_count();
        std::unique_ptr<QMimeData> md(pw->drag_data({"inner/", "top.txt"}));
        bool both = md->hasFormat(archive::MEMBERS_MIME) && md->formats().contains("text/uri-list");
        bool taken = w->pane()->view()->model()->dropMimeData(md.get(), Qt::CopyAction, -1, -1, QModelIndex());
        bool landed = wait_for([&]() { return read_file(join(drop_dir, "inner/a.txt")) == "hello" &&
                                              read_file(join(drop_dir, "top.txt")) == "hello"; }, 10000);
        check(both && taken && landed && temp_count() == temps,
              "items dragged out onto a Kestrel folder are extracted straight into it (no temporary copy made)");
        // another app asks for the files: extracted then, to the temporary folder
        std::unique_ptr<QMimeData> other(pw->drag_data({"top.txt"}));
        QList<QUrl> urls = other->urls();
        check(urls.size() == 1 && urls.first().isLocalFile() &&
                  urls.first().toLocalFile().startsWith(ArchiveWindow::temp_root() + "/") &&
                  read_file(urls.first().toLocalFile()) == "hello",
              "for another app the dragged items are extracted when the drop asks for them, into the temporary "
              "folder");
        pw->close();
        // encrypted files whose password isn't known yet: only Kestrel takes them (it can ask)
        ArchiveWindow *ew = ArchiveWindow::open(w, join(base, "names.7z"));
        wait_for([&]() { return ew->loaded(); });
        std::unique_ptr<QMimeData> enc_md(ew->drag_data({"top.txt"}));
        check(enc_md->hasFormat(archive::MEMBERS_MIME) && !enc_md->formats().contains("text/uri-list"),
              "encrypted items whose password isn't known are offered to Kestrel only, not as files to other apps");
        ew->close();
        // a day-old temporary copy is deleted when the next one is made
        QString old = join(ArchiveWindow::temp_root(), "old-copy");
        makedirs(old, true);
        write_text(join(old, "f.txt"), "x");
        ::chmod(enc(join(old, "f.txt")).constData(), 0444);
        struct timeval tv[2] = {{QDateTime::currentSecsSinceEpoch() - 2 * 86400, 0},
                                {QDateTime::currentSecsSinceEpoch() - 2 * 86400, 0}};
        ::utimes(enc(old).constData(), tv);
        ArchiveWindow *pw2 = ArchiveWindow::open(w, join(base, "plain.7z"));
        wait_for([&]() { return pw2->loaded(); });
        std::unique_ptr<QMimeData> again(pw2->drag_data({"top.txt"}));
        again->urls();
        check(!lexists(old), "temporary copies not used for a day are deleted when a new one is made");
        pw2->close();
    }

    // ---- unzip, taken only without 7-Zip (which the tests always have): listing, single items, testing
    if (archive::tool("unzip").isEmpty()) {
        skip("the unzip path (unzip isn't installed)");
    } else {
        QString uz = home_path("uz");
        makedirs(join(uz, "src"), true);
        for (const QString &n : {"a [1].txt", "a 1.txt", "b*?.txt", "bxy.txt", "c.txt"})
            write_text(join(uz, "src/" + n), n.toUtf8());
        bool zipped = make({z7, "a", "-tzip", join(uz, "u.zip"), "a [1].txt", "a 1.txt", "b*?.txt", "bxy.txt", "c.txt"},
                           join(uz, "src"));
        qputenv("KESTREL_TEST_HIDE_TOOLS", "7z");
        Task ut("unzip", [](Task *) { return QVariant(); });
        QStringList names, got1, got2;
        bool tested = false;
        QString err;
        try {
            for (const archive::Entry &e : archive::list(&ut, join(uz, "u.zip")))
                names << e.path;
            names.sort();
            makedirs(join(uz, "o1"), true);
            makedirs(join(uz, "o2"), true);
            archive::extract_members(&ut, join(uz, "u.zip"), join(uz, "o1"), QString(), {"a [1].txt"});
            archive::extract_members(&ut, join(uz, "u.zip"), join(uz, "o2"), QString(), {"b*?.txt"});
            got1 = listdir(join(uz, "o1"));
            got2 = listdir(join(uz, "o2"));
            tested = archive::test(&ut, join(uz, "u.zip")).ok;
        } catch (const std::exception &e) {
            err = e.what();
        }
        bool via_unzip = archive::extract_tool(join(uz, "u.zip")) == "unzip";
        qunsetenv("KESTREL_TEST_HIDE_TOOLS");
        check(zipped && via_unzip && names == QStringList{"a 1.txt", "a [1].txt", "b*?.txt", "bxy.txt", "c.txt"},
              "without 7-Zip, unzip lists a zip (" + names.join(", ") + err + ")");
        check(got1 == QStringList{"a [1].txt"} && got2 == QStringList{"b*?.txt"} &&
                  read_file(join(uz, "o1/a [1].txt")) == "a [1].txt",
              "...extracts single items, names with [ ] * ? taken as they are, not as patterns (" + got1.join(", ") +
                  " | " + got2.join(", ") + ")");
        check(tested, "...and tests it");
    }

    // a big archive: its items are made in the job, so showing them doesn't hold up the window (it froze for 1–2 s
    // per 100,000 items), in order and with their times
    {
        QString big = home_path("bigsrc/big");
        for (int d = 0; d < 300; ++d) {
            QString dir = join(big, QString("d%1").arg(d, 3, 10, QChar('0')));
            makedirs(dir, true);
            for (int f = 0; f < 200; ++f)
                write_text(join(dir, QString("f%1").arg(f, 3, 10, QChar('0'))), "");
        }
        write_text(join(big, "a.txt"), "x");
        QDateTime when(QDate(2021, 3, 4), QTime(5, 6, 7));
        struct timeval tv[2] = {{when.toSecsSinceEpoch(), 0}, {when.toSecsSinceEpoch(), 0}};
        ::utimes(join(big, "a.txt").toUtf8().constData(), tv);
        QString tar = home_path("big.tar");
        auto res = proc::run({"tar", "-cf", tar, "-C", home_path("bigsrc"), "big"}, 120000);
        QElapsedTimer gap;
        qint64 worst = 0;
        QTimer tick;
        QObject::connect(&tick, &QTimer::timeout, [&]() { worst = std::max(worst, gap.restart()); });
        ArchiveWindow *bw = ArchiveWindow::open(w, tar);
        gap.start();
        tick.start(10);
        drive({}, [&]() { return bw->loaded() ? QString("loaded") : QString(); }, 300000);
        QElapsedTimer after;   // the stall would come once it's loaded
        after.start();
        while (after.elapsed() < 500)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        tick.stop();
        QTreeWidgetItem *top = bw->tree->topLevelItemCount() == 1 ? bw->tree->topLevelItem(0) : nullptr;
        auto name = [](QTreeWidgetItem *it) { return it ? it->text(0) : QString("none"); };
        QTreeWidgetItem *first = top ? top->child(0) : nullptr, *last = top ? top->child(top->childCount() - 1) : nullptr;
        bool ordered = top && top->isExpanded() && top->childCount() == 301 && name(first) == "d000" &&
                       name(last) == "a.txt" && first->childCount() == 200 && name(first->child(0)) == "f000" &&
                       name(first->child(199)) == "f199";
        check(res.rc == 0 && bw->loaded() && worst < 500 && ordered &&
                  last->data(0, Qt::UserRole + 4).toDateTime() == when && bw->status->text().startsWith(QLocale().toString(60001) + " files in 301 folders"),
              QString("a big archive (60,000 files) opens without holding up the window (longest pause %1 ms), folders "
                      "first, by name, with the files' times (%2, %3 … %4)")
                  .arg(QString::number(worst), name(first), name(first ? first->child(0) : nullptr), name(last)));
        bw->tree->header()->setSortIndicator(0, Qt::DescendingOrder);   // a click on the Name column
        check(top && name(top->child(0)) == "d299" && name(top->child(300)) == "a.txt" &&
                  name(top->child(0)->child(0)) == "f199",
              "clicking a column's header sorts by it (folders still first)");
        bw->close();
    }

    // ---- the keyring
    QString daemon = archive::tool("gnome-keyring-daemon");
    if (daemon.isEmpty()) {
        skip("remembered passwords (gnome-keyring-daemon isn't installed)");
        finish();
    }
    g_dbus_connection_close_sync(holder, nullptr, nullptr);   // the name is free for the daemon
    QString run_dir = home_path("run");   // its sockets: not the real session's
    makedirs(run_dir, true);
    ::chmod(run_dir.toUtf8().constData(), 0700);
    proc::Options o;
    o.env = {"XDG_RUNTIME_DIR=" + run_dir};
    o.in = proc::PIPE;
    proc::run({daemon, "--unlock", "--components=secrets"}, 20000, o, "test-login");
    // until it has the Secret Service's name (asked with NameHasOwner, which doesn't start anything: asking libsecret
    // could make the bus start a keyring of its own, locked, which would take the name first)
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    auto owned = [bus]() {
        GVariant *v = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                                  "NameHasOwner", g_variant_new("(s)", "org.freedesktop.secrets"),
                                                  G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
        gboolean has = FALSE;
        if (v) {
            g_variant_get(v, "(b)", &has);
            g_variant_unref(v);
        }
        return bool(has);
    };
    if (!wait_for(owned, 10000) || !wait_for([]() { return keyring::available(); }, 10000)) {
        skip("remembered passwords (gnome-keyring-daemon didn't start on the test's bus)");
        finish();
    }
    QString fp = keyring::fingerprint(join(one_dir, "o.7z"));
    write_text(home_path("o-copy.7z"), read_file(join(one_dir, "o.7z")));
    write_text(home_path("other.bin"), "something else");
    check(!fp.isEmpty() && keyring::fingerprint(home_path("o-copy.7z")) == fp && keyring::fingerprint(home_path("other.bin")) != fp,
          "an archive's fingerprint is its contents: the same for a copy under another name, not for another file");
    check(keyring::lookup(fp).isEmpty() && keyring::all().isEmpty(),
          "a password typed without ticking Remember is not saved");

    QString kr = home_path("kr");
    makedirs(kr, true);
    made = make7("../kr/k1.7z", "seven") && make7("../kr/k2.7z", "seven");
    archive_ui::extract_here(w, join(kr, "k1.7z"));
    w->statusBar()->clearMessage();
    auto said = [&]() { return w->statusBar()->currentMessage().startsWith("Extracted") ? w->statusBar()->currentMessage() : QString(); };
    r = drive({{"seven", true}}, said);
    QString kfp = keyring::fingerprint(join(kr, "k1.7z"));
    check(made && r.offered == QList<bool>{true} && keyring::lookup(kfp) == "seven" && keyring::all() == QStringList{"seven"},
          "with a keyring the password window offers Remember, and a ticked one is saved for that archive");

    // renamed and moved: found by its fingerprint, without asking
    makedirs(home_path("moved"), true);
    util::rename(join(kr, "k1.7z"), home_path("moved/renamed.7z"));
    w->statusBar()->clearMessage();
    archive_ui::extract_here(w, home_path("moved/renamed.7z"));
    r = drive({}, said);
    check(r.prompts.isEmpty() && read_file(home_path("moved/renamed/top.txt")) == "hello" &&
              keyring::lookup(keyring::fingerprint(home_path("moved/renamed.7z"))) == "seven",
          "an archive renamed and moved still opens with its saved password, without asking");
    w->statusBar()->clearMessage();
    archive_ui::extract_here(w, join(kr, "k2.7z"));
    r = drive({}, said);
    check(r.prompts.isEmpty() && read_file(join(kr, "k2/top.txt")) == "hello",
          "another archive is opened with a saved password (tried in turn) when none is saved for it");

    // forgetting them: Preferences
    auto *prefs = new PreferencesDialog(w);
    auto *forget = prefs->findChild<QPushButton *>("forget_passwords");
    if (forget)
        forget->click();
    QMessageBox *q = nullptr;
    wait_for([&]() {
        for (QWidget *x : QApplication::topLevelWidgets())
            if (auto *b = qobject_cast<QMessageBox *>(x); b && b->isVisible() && b->windowTitle() == "Forget Saved Archive Passwords")
                q = b;
        return q != nullptr;
    });
    if (q)
        q->button(QMessageBox::Yes)->click();
    QString told;
    wait_for([&]() {
        for (QWidget *x : QApplication::topLevelWidgets())
            if (auto *b = qobject_cast<QMessageBox *>(x); b && b->isVisible() && b->text().startsWith("Forgot"))
                told = b->text();
        return !told.isEmpty();
    });
    check(forget && told == "Forgot 1 saved archive password." && keyring::all().isEmpty() && keyring::lookup(kfp).isEmpty(),
          "Preferences → Forget Saved Archive Passwords deletes them from the keyring (" + told + ")");
    prefs->close();

    finish();
}
