// Extracting archives through the window (archive_ui.h): Extract All for several archives (one job, one summary, a
// split set counted once), the password typed once being tried on the archives after it, the quick password test
// (archive::password_opens), and remembered passwords in the keyring (keyring.h) with a real gnome-keyring-daemon on
// the test's private session bus (skipped where it isn't installed). Also tar archives with a password (through gpg):
// made, read back by gpg itself, opened, listed, tested and extracted by Kestrel, refused with a wrong password.
#include "common.h"

#include "archive_ui.h"
#include "archivewin.h"
#include "prefswin.h"
#include "keyring.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QGroupBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTreeWidget>

#include <sys/resource.h>

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

    // ---- no keyring: the Secret Service's name is held here, so the bus doesn't start one (where gnome-keyring is
    // installed it would) and calls to it fail
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
        bool made = make({z7, "a", "-t7z", "-mx0", "-psecret", join(look, "big-enc.7z"), "tree"}, many);
        bool right = false, wrong = true;
        try {
            right = archive::password_opens(&look_task, join(look, "big-enc.7z"), "secret");
            wrong = archive::password_opens(&look_task, join(look, "big-enc.7z"), "nope");
        } catch (const std::exception &) {
        }
        check(made && right && !wrong, "big encrypted 7z: the quick password test takes the right password and refuses a wrong one");
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
