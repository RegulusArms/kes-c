// File operations: copy, move, merge, replace, delete, cancel, trash, links, unique names, and undoing them.
#include "common.h"

#include "archive_ui.h"
#include "dialogs.h"
#include "hashcheck.h"
#include "stats.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>

#include <fcntl.h>
#include <gio/gdesktopappinfo.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <utime.h>

#include <atomic>
#include <thread>

using namespace test;

static QStringList boxes;   // texts of message boxes that popped up (closed automatically)

static QString P(const QString &n) { return home_path(n); }

static void make(const QString &path, const QByteArray &data = "x")
{
    makedirs(dirname(path), true);
    write_text(path, data);
}

static QString text_of(const QString &path) { return QString::fromUtf8(read_file(path)); }

// Run start_ops and wait until it has finished; false if it didn't within the time.
static bool run_ops(MainWindow *w, const QList<fileops::Job> &jobs, const QString &undo_label = QString(),
                    Task **task_out = nullptr)
{
    bool done = false;
    Task *t = fileops::start_ops(w, jobs, "Test", [&done]() { done = true; }, undo_label);
    if (task_out)
        *task_out = t;
    QPointer<Task> tp(t);
    return wait_for([&]() { return done || !tp; }, 20000) && wait_for([&]() { return !tp; }, 5000);
}

static bool undo_and_wait(MainWindow *w, const std::function<bool()> &result)
{
    undo::undo(w);
    return wait_for(result, 8000);
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    setup_app();
    stats::enable();   // KESTREL_STATS: checked at the end
    MainWindow *w = open_window({HOME()});
    QTimer closer;   // error boxes are modal: note their text and close them
    QObject::connect(&closer, &QTimer::timeout, []() {
        if (auto *m = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            boxes << m->text();
            m->done(0);
        }
    });
    closer.start(100);

    // ---- copy
    makedirs(P("dst"), true);
    make(P("src/a.txt"), "alpha");
    ::chmod(enc(P("src/a.txt")).constData(), 0640);
    struct utimbuf times{1000000000, 1000000000};
    ::utime(enc(P("src/a.txt")).constData(), &times);
    check(run_ops(w, {{"copy", P("src/a.txt"), P("dst/a.txt")}}) && boxes.isEmpty(), "a copy finishes without errors");
    struct stat st;
    stat_(P("dst/a.txt"), st);
    check(text_of(P("dst/a.txt")) == "alpha" && exists(P("src/a.txt")), "copy: same contents, original kept");
    check((st.st_mode & 0777) == 0640 && st.st_mtime == 1000000000, "copy: permissions and modification time kept");

    make(P("tree/sub/deep/f.bin"), QByteArray(300000, 'z'));
    make(P("tree/sub/g.txt"), "g");
    makedirs(P("tree/empty"));
    util::symlink("sub/g.txt", P("tree/rel-link"));
    util::symlink("/nonexistent/target", P("tree/dangling"));
    make(P("tree/100% %1 файл.txt"), "odd name");
    check(run_ops(w, {{"copy", P("tree"), P("tree2")}}), "a folder copy finishes");
    check(read_file(P("tree2/sub/deep/f.bin")).size() == 300000 && text_of(P("tree2/sub/g.txt")) == "g",
          "folder copy: nested files");
    check(isdir(P("tree2/empty")), "folder copy: empty folders");
    check(islink(P("tree2/rel-link")) && util::readlink(P("tree2/rel-link")) == "sub/g.txt",
          "folder copy: relative links stay links to the same target");
    check(islink(P("tree2/dangling")) && util::readlink(P("tree2/dangling")) == "/nonexistent/target",
          "folder copy: broken links are copied as they are");
    check(text_of(P("tree2/100% %1 файл.txt")) == "odd name", "folder copy: names with %, %1 and non-ASCII letters");

    // ---- move
    make(P("m/one.txt"), "1");
    struct stat before;
    stat_(P("m/one.txt"), before);
    check(run_ops(w, {{"move", P("m/one.txt"), P("dst/one.txt")}}), "a move finishes");
    stat_(P("dst/one.txt"), st);
    check(!lexists(P("m/one.txt")) && text_of(P("dst/one.txt")) == "1", "move: the file is at the new place only");
    check(st.st_ino == before.st_ino, "move on the same drive renames (no copy)");

    // ---- merge and replace
    make(P("A/only-a.txt"), "a");
    make(P("A/both.txt"), "from A");
    make(P("B/only-b.txt"), "b");
    make(P("B/both.txt"), "from B");
    check(run_ops(w, {{"merge_copy", P("A"), P("B")}}), "a merge finishes");
    check(text_of(P("B/only-a.txt")) == "a" && text_of(P("B/only-b.txt")) == "b",
          "merge: files from both folders are there");
    check(text_of(P("B/both.txt")) == "from A", "merge: a file in both is replaced by the copied one");
    make(P("C/only-c.txt"), "c");
    check(run_ops(w, {{"copy", P("A"), P("C")}}), "a replace finishes");
    check(!lexists(P("C/only-c.txt")) && exists(P("C/only-a.txt")), "replace: the old folder's contents are gone");

    // ---- delete
    make(P("ro/inner/f.txt"));
    ::chmod(enc(P("ro/inner")).constData(), 0500);
    check(run_ops(w, {{"delete", P("ro"), QString()}}), "a delete finishes");
    check(!lexists(P("ro")), "delete: removes read-only folders you own");
    util::symlink(P("tree"), P("link-to-tree"));
    check(run_ops(w, {{"delete", P("link-to-tree"), QString()}}) && !lexists(P("link-to-tree")) && exists(P("tree/sub/g.txt")),
          "delete: a link to a folder removes the link, not the folder");

    // ---- cancel
    const int BIG = 6 * 1024 * 1024;   // more than one 4 MB chunk, so a file can be cancelled half-way
    for (int i = 0; i < 24; ++i)
        make(P(QString("big/f%1.bin").arg(i, 2, 10, QChar('0'))), QByteArray(BIG, char('a' + i)));
    bool finished = false;
    Task *t = fileops::start_ops(w, {{"copy", P("big"), P("big2")}}, "Test", [&finished]() { finished = true; });
    QPointer<Task> tp(t);
    // at once: on a fast disk (the test's home is in /tmp, often in memory) the whole copy can finish before the
    // first progress report arrives
    tp->cancel();
    check(wait_for([&]() { return !tp; }, 10000), "a cancelled copy stops");
    QStringList copied = isdir(P("big2")) ? listdir(P("big2")) : QStringList();
    bool whole = true;
    for (const QString &f : copied)
        whole = whole && getsize(join(P("big2"), f)) == BIG;
    check(copied.size() < 24, QString("cancel: stops part-way (%1 of 24 copied)").arg(copied.size()));
    check(whole, "cancel: no half-copied file is left behind");

    // -- replacing a file: the old one stays until the new one is complete (a cancel or an error keeps it); a symlink
    // in the way is replaced, not written through; another hard link of the old file keeps its contents
    {
        makedirs(P("rep"), true);
        auto leftovers = []() {   // temporary files left in rep/
            QStringList out;
            for (const QString &n : listdir(P("rep")))
                if (n.startsWith(".kes-"))
                    out << n;
            return out;
        };
        make(P("rep/new.bin"), "new");
        ::truncate(enc(P("rep/new.bin")).constData(), qint64(1) << 30);   // 1 GB (sparse): long enough to cancel
        make(P("rep/report.txt"), "precious");
        Task *rt = nullptr;
        bool rep_done = false;
        rt = fileops::start_ops(w, {{"copy", P("rep/new.bin"), P("rep/report.txt")}}, "Test",
                                [&rep_done]() { rep_done = true; });
        // on the copying thread itself, as soon as the copy reports progress on this file: a cancel half-way through
        QObject::connect(rt, &Task::progress, rt, [rt](double, const QString &text) {
            if (text.endsWith("new.bin"))
                rt->cancel();
        }, Qt::DirectConnection);
        QPointer<Task> rtp(rt);
        wait_for([&]() { return !rtp; }, 20000);
        check(text_of(P("rep/report.txt")) == "precious" && leftovers().isEmpty(),
              "replacing a file and cancelling part-way keeps the original, and leaves no temporary file");

        make(P("rep/big.bin"), QByteArray(BIG, 'b'));
        make(P("rep/keep.txt"), "precious");
        struct rlimit old_lim;
        ::getrlimit(RLIMIT_FSIZE, &old_lim);
        struct rlimit lim = old_lim;
        lim.rlim_cur = 1 << 20;   // writes past 1 MB fail (EFBIG)
        auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
        ::setrlimit(RLIMIT_FSIZE, &lim);
        run_ops(w, {{"copy", P("rep/big.bin"), P("rep/keep.txt")}});
        ::setrlimit(RLIMIT_FSIZE, &old_lim);
        ::signal(SIGXFSZ, old_handler);
        check(text_of(P("rep/keep.txt")) == "precious" && leftovers().isEmpty(),
              "a copy that fails part-way (a write error) keeps the file it would have replaced, and leaves nothing "
              "half-written");

        make(P("rep/outside.txt"), "outside");
        ::symlink(enc(P("rep/outside.txt")).constData(), enc(P("rep/to-outside")).constData());
        make(P("rep/linked.txt"), "old");
        ::link(enc(P("rep/linked.txt")).constData(), enc(P("rep/other-link.txt")).constData());
        make(P("rep/small.txt"), "new");
        run_ops(w, {{"copy", P("rep/small.txt"), P("rep/to-outside")}, {"copy", P("rep/small.txt"), P("rep/linked.txt")}});
        check(!islink(P("rep/to-outside")) && text_of(P("rep/to-outside")) == "new" &&
                  text_of(P("rep/outside.txt")) == "outside" && text_of(P("rep/linked.txt")) == "new" &&
                  text_of(P("rep/other-link.txt")) == "old" && leftovers().isEmpty(),
              "replacing a symlink replaces the link, not its target; another hard link of a replaced file keeps its "
              "contents");
        // folders: the new one is built beside the old one and swapped in whole
        make(P("rep/newdir/a.txt"), "a");
        make(P("rep/newdir/huge"), "x");
        ::truncate(enc(P("rep/newdir/huge")).constData(), qint64(1) << 30);
        make(P("rep/olddir/keep.txt"), "keep");
        auto old_intact = []() { return listdir(P("rep/olddir")) == QStringList{"keep.txt"} &&
                                        text_of(P("rep/olddir/keep.txt")) == "keep"; };
        Task *dt = fileops::start_ops(w, {{"copy", P("rep/newdir"), P("rep/olddir")}}, "Test", []() {});
        QObject::connect(dt, &Task::progress, dt, [dt](double, const QString &text) {
            if (text.endsWith("huge"))
                dt->cancel();
        }, Qt::DirectConnection);
        QPointer<Task> dtp(dt);
        wait_for([&]() { return !dtp; }, 20000);
        check(old_intact() && leftovers().isEmpty(),
              "replacing a folder and cancelling part-way keeps the old folder as it was, and leaves no half-made copy");
        ::truncate(enc(P("rep/newdir/huge")).constData(), BIG);
        ::getrlimit(RLIMIT_FSIZE, &old_lim);
        lim = old_lim;
        lim.rlim_cur = 1 << 20;
        old_handler = ::signal(SIGXFSZ, SIG_IGN);
        ::setrlimit(RLIMIT_FSIZE, &lim);
        run_ops(w, {{"copy", P("rep/newdir"), P("rep/olddir")}});
        ::setrlimit(RLIMIT_FSIZE, &old_lim);
        ::signal(SIGXFSZ, old_handler);
        check(old_intact() && leftovers().isEmpty(),
              "a folder copy that fails part-way (a write error) keeps the folder it would have replaced");
        ::truncate(enc(P("rep/newdir/huge")).constData(), 10);
        make(P("rep/mvsrc/moved.txt"), "moved");
        make(P("rep/mvdst/old.txt"), "old");
        bool replaced = run_ops(w, {{"copy", P("rep/newdir"), P("rep/olddir")}, {"move", P("rep/mvsrc"), P("rep/mvdst")}});
        check(replaced && listdir(P("rep/olddir")).size() == 2 && lexists(P("rep/olddir/huge")) &&
                  !lexists(P("rep/olddir/keep.txt")) && !lexists(P("rep/mvsrc")) &&
                  listdir(P("rep/mvdst")) == QStringList{"moved.txt"} && leftovers().isEmpty(),
              "copying or moving a folder onto another replaces it whole: the old contents are gone, nothing is left "
              "behind");
        QDir(P("rep")).removeRecursively();
        boxes.clear();
    }

    // ---- trash
    make(P("t/gone.txt"), "trash me");
    util::trash(P("t/gone.txt"));
    QString trashed = join(TRASH_DIR(), "files/gone.txt");
    check(!lexists(P("t/gone.txt")) && exists(trashed), "trash: the file moves to the trash");
    check(trash_original_path(trashed) == P("t/gone.txt"), "trash: its original place is recorded");
    undo::record_paths("trash", "Move to Trash", {P("t/gone.txt")});
    check(undo_and_wait(w, [&]() { return exists(P("t/gone.txt")); }) && !exists(trashed),
          "undo Move to Trash: puts it back");

    // ---- undo
    make(P("u/file.txt"), "u");
    check(run_ops(w, {{"move", P("u/file.txt"), P("dst/file.txt")}}, "Move"), "a move with undo finishes");
    check(undo::label() == "Move", "the Edit menu offers \"Undo Move\"");
    check(undo_and_wait(w, [&]() { return exists(P("u/file.txt")) && !lexists(P("dst/file.txt")); }), "undo Move");
    check(run_ops(w, {{"copy", P("u/file.txt"), P("u/file (copy).txt")}}, "Copy"), "a copy with undo finishes");
    check(undo_and_wait(w, [&]() { return !lexists(P("u/file (copy).txt")); }), "undo Copy: removes the copy");
    check(exists(join(TRASH_DIR(), "files/file (copy).txt")), "...by moving it to the trash, not deleting it");
    check(exists(P("u/file.txt")), "...and leaves the original");
    util::rename(P("u/file.txt"), P("u/renamed.txt"));
    undo::record("rename", "Rename", {qMakePair(P("u/file.txt"), P("u/renamed.txt"))});
    check(undo_and_wait(w, [&]() { return exists(P("u/file.txt")) && !lexists(P("u/renamed.txt")); }), "undo Rename");
    undo::record("move", "Move", {qMakePair(P("u/gone-elsewhere.txt"), P("u/missing.txt"))});
    boxes.clear();
    undo::undo(w);
    check(wait_for([]() { return !boxes.isEmpty(); }) && boxes.last().contains("no longer at"),
          "undo explains what it can't put back");

    // a copy that replaces a file can't be undone (the old file is gone, and undoing it as a new copy would put the
    // only one left in the trash): in a batch, only the other copies are undone
    undo::record("rename", "Earlier", {qMakePair(P("u/earlier-a"), P("u/earlier-b"))});
    make(P("u/rep.txt"), "new");
    make(P("u/rep-dst/rep.txt"), "old");
    check(run_ops(w, {{"copy", P("u/rep.txt"), P("u/rep-dst/rep.txt")}}, "Copy") &&
              text_of(P("u/rep-dst/rep.txt")) == "new" && undo::label() == "Earlier",
          "a copy that replaced a file isn't offered for undo");
    make(P("u/fresh.txt"), "fresh");
    make(P("u/rep.txt"), "newer");
    check(run_ops(w, {{"copy", P("u/fresh.txt"), P("u/rep-dst/fresh.txt")}, {"copy", P("u/rep.txt"), P("u/rep-dst/rep.txt")}},
                  "Copy") &&
              undo_and_wait(w, [&]() { return !lexists(P("u/rep-dst/fresh.txt")); }) &&
              text_of(P("u/rep-dst/rep.txt")) == "newer",
          "undoing a batch of copies leaves the one that replaced a file where it is");

    // ---- batch rename: a failure part-way puts every item back under its old name (a dangling symlink too, which
    // exists() wouldn't see), and a name taken meanwhile is never replaced
    {
        makedirs(P("br"), true);
        util::symlink("/nonexistent/target", P("br/lnk"));
        make(P("br/b.txt"), "b");
        BatchRenameDialog dlg(w, {P("br/lnk"), P("br/b.txt")});
        dlg.findChildren<QLineEdit *>().first()->setText("[Name]-x");   // the template
        make(P("br/b-x.txt"), "taken meanwhile");
        QStringList earlier = boxes;
        boxes.clear();
        for (QPushButton *b : dlg.findChildren<QPushButton *>())
            if (b->text() == "Rename")
                b->click();
        QStringList left = listdir(P("br"));
        left.sort();
        check(left == QStringList({"b-x.txt", "b.txt", "lnk"}) && islink(P("br/lnk")) && text_of(P("br/b.txt")) == "b" &&
                  text_of(P("br/b-x.txt")) == "taken meanwhile" && !boxes.isEmpty(),
              "a batch rename that fails part-way puts every item back, a dangling symlink too, and replaces nothing");
        boxes = earlier;
    }

    // ---- the conflict dialog says what Merge does to files with the same names (they're replaced, for good)
    {
        ConflictDialog folder_dlg(w, P("dst"), true), file_dlg(w, P("dst/a.txt"), false);
        auto says = [](QDialog &d) {
            QString all;
            for (QLabel *l : d.findChildren<QLabel *>())
                all += l->text() + "\n";
            return all;
        };
        check(says(folder_dlg).contains("files there with the same names are\nreplaced. A merge can't be undone.") &&
                  !says(file_dlg).contains("Merge"),
              "the conflict dialog says a merge replaces files with the same names and can't be undone");
    }

    // ---- Move to Trash on a selection that holds something already in the trash: that one can only be deleted
    // permanently (it asks first: cancelled here), and the rest still goes to the trash
    {
        make(P("mt/in-trash.txt"), "t");
        util::trash(P("mt/in-trash.txt"));
        QString trashed_item = join(TRASH_DIR(), "files/in-trash.txt");
        make(P("mt/normal.txt"), "n");
        QStringList earlier = boxes;
        boxes.clear();
        w->trash_paths({trashed_item, P("mt/normal.txt")});
        check(wait_for([&]() { return !lexists(P("mt/normal.txt")); }) && exists(join(TRASH_DIR(), "files/normal.txt")) &&
                  exists(trashed_item) && !boxes.isEmpty() && boxes.first().contains("Permanently delete “in-trash.txt”"),
              "Move to Trash on a mixed selection trashes the items outside the trash and asks only about the one in it");
        boxes = earlier;
    }

    // ---- unique names and links
    make(P("n/a.txt"));
    check(unique_path(P("n"), "a.txt") == P("n/a (copy).txt"), "a copy's name: \"a (copy).txt\"");
    make(P("n/a (copy).txt"));
    check(unique_path(P("n"), "a.txt") == P("n/a (copy 2).txt"), "the next one: \"a (copy 2).txt\"");
    check(unique_path(P("n"), "a.txt", "num") == P("n/a (2).txt"), "keep both: \"a (2).txt\"");
    QString sym = fileops::make_link(fileops::link_plan("sym", P("n/a.txt"), P("dst")));
    check(islink(sym) && util::readlink(sym) == P("n/a.txt"), "symbolic link (absolute)");
    makedirs(P("dst/sub"), true);
    QString rel = fileops::make_link(fileops::link_plan("rel", P("n/a.txt"), P("dst/sub")));
    check(islink(rel) && !util::readlink(rel).startsWith('/') && realpath(rel) == P("n/a.txt"), "relative link");
    QString hard = fileops::make_link(fileops::link_plan("hard", P("n/a.txt"), P("dst")));
    struct stat h1, h2;
    stat_(hard, h1);
    stat_(P("n/a.txt"), h2);
    check(h1.st_ino == h2.st_ino, "hard link");
    QString same = fileops::make_link(fileops::link_plan("sym", P("n/a.txt"), P("n")));
    check(basename(same).startsWith("Link to a"), "a link in the same folder is named \"Link to …\"");

    // -- desktop shortcuts to items whose names hold quotes, $, `, \, % and line breaks that try to add keys of their
    // own (GLib keeps the last of a repeated key): the shortcut must still name and open exactly that item
    {
        const QString evil = "\"q\" $HOME `id` \\b %f %%\nExec=touch PWNED\nType=Application\n";
        QString sc = P("sc"), out = P("sc/out"), dir = join(sc, "d " + evil), marker = P("sc-ran");
        makedirs(out, true);
        makedirs(dir, true);
        QString script = join(dir, "run " + evil + ".sh"), doc = join(dir, "doc " + evil + ".txt");
        make(script, ("#!/bin/sh\nprintf '%s\\n' \"$#\" \"$0\" > '" + marker + "'\n").toUtf8());
        ::chmod(enc(script).constData(), 0755);
        make(doc, "doc");
        // each shortcut as GLib reads it, and the keys written (each once)
        auto read = [](const QString &path, QStringList *keys) {
            QMap<QString, QString> values;
            QStringList lines = text_of(path).split('\n');
            for (const QString &l : lines)
                if (l.contains('='))
                    *keys << l.section('=', 0, 0);
            GKeyFile *kf = g_key_file_new();
            if (g_key_file_load_from_file(kf, enc(path).constData(), G_KEY_FILE_NONE, nullptr)) {
                gsize n = 0;
                gchar **names = g_key_file_get_keys(kf, "Desktop Entry", &n, nullptr);
                for (gsize i = 0; i < n; i++) {
                    gchar *v = g_key_file_get_string(kf, "Desktop Entry", names[i], nullptr);
                    values[QString::fromUtf8(names[i])] = QString::fromUtf8(v ? v : "");
                    g_free(v);
                }
                g_strfreev(names);
            }
            g_key_file_free(kf);
            return values;
        };
        QStringList made;
        for (const QString &t : {script, doc, dir})
            made << fileops::make_link(fileops::link_plan("desktop", t, out));
        QStringList app_keys, doc_keys, dir_keys;
        auto app = read(made[0], &app_keys), docv = read(made[1], &doc_keys), dirv = read(made[2], &dir_keys);
        check(app_keys == QStringList{"Type", "Name", "Exec", "Path", "Icon", "Terminal"} &&
                  app.value("Type") == "Application" && app.value("Name") == basename(script) &&
                  app.value("Path") == dir && doc_keys == QStringList{"Type", "Name", "URL", "Icon"} &&
                  docv.value("Type") == "Link" && docv.value("Name") == basename(doc) &&
                  uri_to_path(docv.value("URL")) == doc && dir_keys == doc_keys && dirv.value("Type") == "Link" &&
                  uri_to_path(dirv.value("URL")) == dir,
              "a desktop shortcut keeps an item's name and path whole, whatever they hold (quotes, $, `, \\, %, line "
              "breaks), and adds no keys");
        if (!which("desktop-file-validate")) {
            skip("desktop-file-validate accepts the shortcuts (it isn't installed)");
        } else {
            QString complaints;
            for (const QString &m : made) {
                auto r = proc::run({"desktop-file-validate", m});
                if (r.rc != 0)
                    complaints += r.out + r.err;
            }
            check(complaints.isEmpty(), "desktop-file-validate accepts the shortcuts" +
                                            (complaints.isEmpty() ? QString() : " " + complaints.trimmed()));
        }
        QString ctl = join(sc, QString("bell") + QChar(7) + ".txt");
        make(ctl, "x");
        bool refused = false;
        try {
            fileops::link_plan("desktop", ctl, out);
        } catch (const Error &) {
            refused = true;
        }
        check(refused, "a shortcut to an item whose name holds other control characters is refused");
        QString cwd = QDir::currentPath();
        QDir::setCurrent(sc);   // where an injected relative command would land
        GDesktopAppInfo *info = g_desktop_app_info_new_from_filename(enc(made[0]).constData());
        bool launched = info && g_app_info_launch(G_APP_INFO(info), nullptr, nullptr, nullptr);
        if (info)
            g_object_unref(info);
        wait_for([&]() { return exists(marker); }, 5000);
        QDir::setCurrent(cwd);
        bool pwned = false;
        walk(HOME(), [&](const QString &, QStringList &, QStringList &files) {
            pwned = pwned || files.contains("PWNED");
            return true;
        });
        check(launched && text_of(marker) == "0\n" + script + "\n" && !pwned,
              "opening the program's shortcut runs that program alone, with no arguments, and nothing else");
    }

    check(location_arg("trash:///") == join(TRASH_DIR(), "files") && location_arg("recent:///") == places::RECENT,
          "trash:/// and recent:/// from other apps open the right places");

    check(boxes.size() == 1, QString("no error boxes besides that one (%1)").arg(boxes.join(" | ")));

    auto ds = fileops::dir_stats(P("tree"));
    check(ds.files == 5 && ds.dirs == 3, QString("folder size counts files and folders (%1 files, %2 folders)")
                                              .arg(ds.files)
                                              .arg(ds.dirs));

    // ---- opening an archive
    check(archive::opens_as_archive(P("photos.tar.gz")) && !archive::opens_as_archive(P("app.deb")) &&
              !archive::opens_as_archive(P("disk.iso")) && !archive::opens_as_archive(P("game.apk")),
          "a .tar.gz opens as an archive; .deb, .iso and .apk open with their own apps");
    make(P("arc/in.txt"));
    proc::run({"tar", "czf", P("arc.tar.gz"), "-C", P("arc"), "in.txt"});
    bool extract_shown = false;
    QTimer extract_closer;   // the Extract dialog is modal: note it and close it
    QObject::connect(&extract_closer, &QTimer::timeout, [&extract_shown]() {
        if (auto *d = qobject_cast<ExtractDialog *>(QApplication::activeModalWidget())) {
            extract_shown = true;
            d->reject();
        }
    });
    extract_closer.start(50);
    w->open_paths(w->pane(), {P("arc.tar.gz")});
    check(wait_for([&]() { return extract_shown; }), "double-clicking an archive opens Kestrel's Extract dialog");
    extract_closer.stop();

    // ---- checksum files
    check(hashcheck::is_hash_file(P("x.sfv")) && hashcheck::is_hash_file(P("x.MD5")) &&
              hashcheck::is_hash_file(P("x.sha256")) && hashcheck::is_hash_file(P("SHA256SUMS")) &&
              hashcheck::is_hash_file(P("Fedora-41-x86_64-CHECKSUM")) && hashcheck::is_hash_file(P("x.b2")) &&
              !hashcheck::is_hash_file(P("x.txt")) && !hashcheck::is_hash_file(P("md5.txt")) &&
              !hashcheck::is_hash_file(P("summary")),
          "checksum files are recognised by name (.sfv, .md5, .sha256, SHA256SUMS, …-CHECKSUM), other files aren't");
    const QString MD5 = "b1946ac92492d2347c6235b4d2611184", SHA1 = "f572d396fae9206628714fb2ce00f72e94f2258f",
                  SHA256 = "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03",
                  SHA512 = "e7c22b994c59d9cf2b48e549b1e24666636045930d3da7c1acb299d1c3b7f931f94aae41edda2c2b207a36e10f8bcb8d"
                           "45223e54878f5b316e7ce3b6bc019629",
                  B2 = "f60ce482e5cc1229f39d71313171a8d9f4ca3a87d066bf4b205effb528192a75f14f3271e2c1a90e1de53f275b4d4793ee"
                       "f2f5e31ea90d2ce29d2e481c36435f";
    make(P("sums/hello.txt"), "hello\n");
    make(P("sums/back\\slash.txt"), "hello\n");
    make(P("sums/changed.txt"), "changed\n");
    make(P("sums/MD5SUMS"), ("# made by md5sum\n" + MD5 + "  hello.txt\n" + MD5 + " *changed.txt\n" + MD5 +
                             "  missing.txt\n\\" + MD5 + "  back\\\\slash.txt\n")
                                .toUtf8());
    make(P("sums/CHECKSUM"), ("-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA256\n\nSHA256 (hello.txt) = " + SHA256 +
                              "\nSHA1 (hello.txt) = " + SHA1 + "\nSHA512 (hello.txt) = " + SHA512 +
                              "\nBLAKE2b (hello.txt) = " + B2 +
                              "\n-----BEGIN PGP SIGNATURE-----\niQIzBAEBCAAdFiEE\n-----END PGP SIGNATURE-----\n")
                                 .toUtf8());
    make(P("sums/files.sfv"), "; made by an SFV tool\r\nhello.txt 363A3020\r\nchanged.txt 363a3020\r\n");
    make(P("sums/hello.txt.sha256"), (SHA256 + "\n").toUtf8());
    make(P("sums/notes.md5"), "just some notes\n");
    auto md5s = hashcheck::parse(P("sums/MD5SUMS")), bsd = hashcheck::parse(P("sums/CHECKSUM")),
         sfv = hashcheck::parse(P("sums/files.sfv")), lone = hashcheck::parse(P("sums/hello.txt.sha256"));
    auto names = [](const QList<hashcheck::Entry> &es) {
        QStringList out;
        for (const auto &e : es)
            out << e.name + ":" + e.algo;
        return out.join(",");
    };
    check(names(md5s) == "hello.txt:md5,changed.txt:md5,missing.txt:md5,back\\slash.txt:md5" &&
              md5s.value(0).path == P("sums/hello.txt") &&
              names(bsd) == "hello.txt:sha256,hello.txt:sha1,hello.txt:sha512,hello.txt:blake2b" &&
              names(sfv) == "hello.txt:crc32,changed.txt:crc32" && sfv.value(0).expected == "363a3020" &&
              names(lone) == "hello.txt:sha256" && lone.value(0).path == P("sums/hello.txt") &&
              hashcheck::parse(P("sums/notes.md5")).isEmpty(),
          "GNU, BSD-tag, SFV and lone-hash checksum files are read (escaped names, comments and a PGP signature skipped)");
    auto statuses = [](const QList<hashcheck::Entry> &es) {
        QStringList out;
        for (const auto &e : es)
            out << hashcheck::verify(e).status;
        return out.join(",");
    };
    check(statuses(md5s) == "ok,failed,missing,ok" && statuses(bsd) == "ok,ok,ok,ok" && statuses(sfv) == "ok,failed" &&
              statuses(lone) == "ok",
          "verifying finds matching, changed and missing files (MD5, SHA-1, SHA-256, SHA-512, BLAKE2b, CRC32)");
    w->open_paths(w->pane(), {P("sums/MD5SUMS")});
    hashcheck::VerifyDialog *vd = nullptr;
    wait_for([&]() {
        for (QWidget *x : QApplication::topLevelWidgets())
            if (auto *d = qobject_cast<hashcheck::VerifyDialog *>(x); d && d->isVisible())
                vd = d;
        return vd && vd->complete();
    });
    QStringList got;
    for (const auto &r : vd ? vd->results : QList<hashcheck::Result>())
        got << r.status;
    check(got.join(",") == "ok,failed,missing,ok",
          "double-clicking a checksum file opens Verify Checksums, which checks every file it lists");
    if (vd)
        vd->close();
    QStringList said;
    for (const auto &[file, sums] : QList<QPair<QString, QString>>{
             {"hello.txt", "CHECKSUM"}, {"changed.txt", "MD5SUMS"}, {"hello.txt", "notes.md5"}}) {
        PropertiesDialog d(w, {P("sums/" + file)});
        d.verify_against(P("sums/" + sums));
        wait_for([&]() { return !d.hash_result->text().endsWith("…"); });
        said << d.hash_result->text();
    }
    check(said.value(0) == "✔ Matches (SHA256)." && said.value(1) == "✘ Doesn't match (MD5)." &&
              said.value(2).contains("isn't a checksum file"),
          QString("Properties' Checksums tab checks a file against a chosen checksum file (%1)").arg(said.join(" | ")));

    // -- making checksum files (Create Checksum File…)
    make(P("mk/a.txt"), "alpha\n");
    make(P("mk/back\\slash.txt"), "back\n");
    make(P("mk/sub/inner.txt"), "inner\n");
    make(P("mk/sub/deep/z.txt"), "zed\n");
    QStringList mk_paths{P("mk/a.txt"), P("mk/back\\slash.txt"), P("mk/sub")};
    auto all_ok = [](const QString &sums) {
        QStringList out;
        for (const auto &e : hashcheck::parse(sums))
            out << e.name + ":" + e.algo + ":" + hashcheck::verify(e).status;
        return out;
    };
    QStringList errs;
    QStringList made = hashcheck::create(nullptr, mk_paths, P("mk"), "set", hashcheck::create_algorithms(), false, &errs);
    QStringList made_names, per_algo;
    for (const QString &m : made) {
        made_names << basename(m);
        per_algo << all_ok(m).join(",");
    }
    const QString listed = "a.txt:%1:ok,back\\slash.txt:%1:ok,sub/inner.txt:%1:ok,sub/deep/z.txt:%1:ok";
    bool each_ok = made_names.join(",") == "set.sfv,set.md5,set.sha1,set.sha256,set.sha512,set.b2" && errs.isEmpty();
    for (int i = 0; i < per_algo.size(); i++)
        each_ok = each_ok && per_algo[i] == listed.arg(hashcheck::create_algorithms()[i]);
    check(each_ok, "Create Checksum File makes one file per algorithm (md5sum's format, SFV for CRC32) that verify as "
                   "OK, with the files inside folders");
    made = hashcheck::create(nullptr, mk_paths, P("mk"), "set", hashcheck::create_algorithms(), true, &errs);
    QStringList bsd_lines = all_ok(P("mk/set-CHECKSUM"));
    bool bsd_ok = made == QStringList{P("mk/set-CHECKSUM")} && bsd_lines.size() == 24 &&
                  std::all_of(bsd_lines.begin(), bsd_lines.end(), [](const QString &l) { return l.endsWith(":ok"); }) &&
                  text_of(P("mk/set-CHECKSUM")).contains("\\SHA256 (back\\\\slash.txt) = ");
    hashcheck::create(nullptr, {P("mk")}, P("mk"), "all", {"sha256"}, true, &errs);
    hashcheck::create(nullptr, {P("mk")}, P("mk"), "all", {"sha256"}, true, &errs);
    QStringList again = all_ok(P("mk/all-CHECKSUM"));
    check(bsd_ok && !again.isEmpty() && !again.join(",").contains("all-CHECKSUM") &&
              std::all_of(again.begin(), again.end(), [](const QString &l) { return l.endsWith(":ok"); }),
          "...or one file with every algorithm as BSD tags (escaped names), and making it again leaves out its own "
          "older copy");
    make(P("mk/locked.txt"), "locked\n");
    ::chmod(enc(P("mk/locked.txt")).constData(), 0);
    errs.clear();
    hashcheck::create(nullptr, {P("mk/a.txt"), P("mk/locked.txt")}, P("mk"), "two", {"md5"}, false, &errs);
    QStringList two = all_ok(P("mk/two.md5"));
    ::chmod(enc(P("mk/locked.txt")).constData(), 0644);
    hashcheck::CreateDialog cd(w, {P("mk/a.txt")});
    check(two == QStringList{"a.txt:md5:ok"} && errs.size() == 1 && errs.value(0).startsWith("locked.txt: ") &&
              cd.stem() == "a.txt" && cd.dir() == P("mk") && cd.outputs() == QStringList{P("mk/a.txt.sha256")},
          "a file that can't be read is left out and named; the dialog starts from the item's name and folder, and SHA256");

    // -- symlinks inside a tree: never followed, even when one is swapped in while a job runs
    QString victim = P("victim");   // stands for files elsewhere that a job mustn't touch
    auto reset_victim = [&]() {
        QDir(victim).removeRecursively();
        make(join(victim, "keep"), "keep");
        make(join(victim, "sub/deeper"), "deeper");
    };
    auto victim_intact = [&]() {
        return text_of(join(victim, "keep")) == "keep" && text_of(join(victim, "sub/deeper")) == "deeper" &&
               listdir(victim).size() == 2;
    };
    reset_victim();
    make(P("mc_src/sub/new.txt"), "new");
    makedirs(P("mc_dst"), true);
    ::symlink(victim.toLocal8Bit().constData(), P("mc_dst/sub").toLocal8Bit().constData());
    run_ops(w, {{"merge_copy", P("mc_src"), P("mc_dst")}});
    check(victim_intact() && islink(P("mc_dst/sub")), "a merge copy doesn't write through a symlink in the destination");
    reset_victim();
    make(P("lk_src.txt"), "new");
    ::symlink(join(victim, "keep").toLocal8Bit().constData(), P("lk_dst.txt").toLocal8Bit().constData());
    run_ops(w, {{"copy", P("lk_src.txt"), P("lk_dst.txt")}});
    check(victim_intact() && !islink(P("lk_dst.txt")) && text_of(P("lk_dst.txt")) == "new",
          "copying onto a symlink replaces the link, not the file it points to");
    // a folder swapped for a symlink to the victim (renameat2 exchange: the path always exists) while a delete runs
    bool safe = true;
    QElapsedTimer race_clock;
    race_clock.start();
    for (int round = 0; race_clock.elapsed() < 4000 && safe; ++round) {
        reset_victim();
        QString race = P(QString("race%1").arg(round)), links = P(QString("links%1").arg(round));
        for (int d = 0; d < 30; ++d)
            for (int f = 0; f < 5; ++f)
                make(join(race, QString("d%1/f%2").arg(d).arg(f)));
        makedirs(links, true);
        for (int d = 0; d < 30; ++d)
            ::symlink(victim.toLocal8Bit().constData(), join(links, QString("d%1").arg(d)).toLocal8Bit().constData());
        std::atomic<bool> stop{false};
        std::thread swapper([&]() {
            while (!stop) {
                for (int d = 0; d < 30 && !stop; ++d) {
                    QByteArray a = join(race, QString("d%1").arg(d)).toLocal8Bit();
                    QByteArray b = join(links, QString("d%1").arg(d)).toLocal8Bit();
                    if (renameat2(AT_FDCWD, a.constData(), AT_FDCWD, b.constData(), RENAME_EXCHANGE) == 0) {
                        usleep(200);
                        renameat2(AT_FDCWD, a.constData(), AT_FDCWD, b.constData(), RENAME_EXCHANGE);
                    }
                }
            }
        });
        run_ops(w, {{"delete", race, ""}});
        stop = true;
        swapper.join();
        safe = victim_intact();
    }
    check(safe, "a folder swapped for a symlink during a delete doesn't let it delete outside the tree");

    // -- crafted archives (tests/fixtures, make_evil_archives.sh): "../" names, absolute paths, and a symlink out of
    // the destination with a file written through it, extracted by whichever tool Kestrel picks
    for (const QString &ext : QStringList{"tar.gz", "zip", "7z", "rar"}) {
        QString label = "a crafted " + ext + " archive can't write outside the folder it's extracted into";
        QString base = P("evil-" + QString(ext).replace('.', '-')), archive_path = join(base, "evil." + ext);
        makedirs(join(base, "dest"), true);
        makedirs(join(base, "outside"), true);
        QFile::copy(join(QString(TESTS_DIR), "fixtures/evil." + ext), archive_path);
        if (archive::extract_tool(archive_path).isEmpty()) {
            skip(label + " (no tool for it is installed)");
            continue;
        }
        ::unlink("/tmp/kestrel-evil-abs.txt");
        Task task("test", [](Task *) { return QVariant(); });
        try {
            archive::extract(&task, archive_path, join(base, "dest"), QString(), "overwrite");
        } catch (const Error &) {   // refusing an entry may fail the job: fine, as long as nothing got out
        }
        check(!listdir(join(base, "dest")).isEmpty() && listdir(join(base, "outside")).isEmpty() &&
                  !lexists(join(base, "escape.txt")) && !lexists("/tmp/kestrel-evil-abs.txt"),
              label);
        bool escaping = false;   // a symlink left in dest that leads out of it (opening it would go there)
        QString dest = join(base, "dest");
        QDirIterator it(dest, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString f = it.next();
            QString target = islink(f) ? util::readlink(f) : QString();
            if (!target.isEmpty() && (target.startsWith('/') || !(QDir::cleanPath(join(dirname(f), target)) + "/").startsWith(dest + "/")))
                escaping = true;
        }
        check(!escaping, "a crafted " + ext + " archive leaves no symlink leading out of the folder it's extracted into");
    }
    check(archive::link_escapes(0, "../x") && archive::link_escapes(0, "/etc") && archive::link_escapes(1, "a/../../..") &&
              archive::link_escapes(2, "../../../x") && !archive::link_escapes(1, "../x") &&
              !archive::link_escapes(0, "a/../b") && !archive::link_escapes(0, "./a//b") && !archive::link_escapes(2, "../.."),
          "a link target leads out of the extracted folder when it's absolute or its \"..\"s climb above it");
    {   // extracting into an existing folder: the user's own links there stay, also ones that lead out of it
        QString base = P("evil-own"), dest = join(base, "dest");
        makedirs(dest, true);
        util::symlink("../elsewhere", join(dest, "mine"));
        QFile::copy(join(QString(TESTS_DIR), "fixtures/evil.tar.gz"), join(base, "evil.tar.gz"));
        spin(1100);   // older than the job's start
        Task task("test", [](Task *) { return QVariant(); });
        try {
            archive::extract(&task, join(base, "evil.tar.gz"), dest, QString(), "overwrite");
        } catch (const Error &) {
        }
        check(islink(join(dest, "mine")) && !lexists(join(dest, "lnk")),
              "extracting into an existing folder keeps the user's own links there and drops the archive's escaping one");
    }

    // -- a single compressed file (note.txt.gz → note.txt) where the destination already has a symlink named note.txt:
    // the name is replaced, never written through (a dangling link would create its target, a live one truncate it)
    {
        QString base = P("gzlink"), dest = join(base, "dest"), outside = join(base, "outside");
        makedirs(dest, true);
        makedirs(outside, true);
        make(join(base, "note.txt"), "new\n");
        proc::run({"gzip", "-k", join(base, "note.txt")});
        write_text(join(base, "bad.txt.gz"), "not gzip at all");
        auto run_extract = [&](const QString &archive_path, const QString &overwrite) {
            Task task("test", [](Task *) { return QVariant(); });
            try {
                archive::extract(&task, archive_path, dest, QString(), overwrite);
                return true;
            } catch (const Error &) {
                return false;
            }
        };
        auto reset = [&](const QString &target) {
            QDir(dest).removeRecursively();
            QDir(outside).removeRecursively();
            makedirs(dest, true);
            makedirs(outside, true);
            if (target == "keep.txt")
                make(join(outside, "keep.txt"), "keep\n");
            ::symlink(enc(join(outside, target)).constData(), enc(join(dest, "note.txt")).constData());
        };
        bool dangling_ok = true, live_ok = true;
        for (const QString &mode : QStringList{"rename", "overwrite"}) {
            reset("created.txt");
            bool ran = run_extract(join(base, "note.txt.gz"), mode);
            QString got = mode == "rename" ? join(dest, "note (2).txt") : join(dest, "note.txt");
            dangling_ok = dangling_ok && ran && listdir(outside).isEmpty() && !islink(got) && text_of(got) == "new\n" &&
                          (mode == "overwrite" || islink(join(dest, "note.txt")));
            reset("keep.txt");
            ran = run_extract(join(base, "note.txt.gz"), mode);
            live_ok = live_ok && ran && text_of(join(outside, "keep.txt")) == "keep\n" && !islink(got) &&
                      text_of(got) == "new\n";
        }
        check(dangling_ok,
              "decompressing a single file doesn't write through a dangling symlink of that name (Keep both and Replace)");
        check(live_ok, "...or truncate the file a symlink of that name points to (Keep both and Replace)");
        QDir(dest).removeRecursively();
        makedirs(dest, true);
        make(join(dest, "bad.txt"), "old\n");
        bool failed = !run_extract(join(base, "bad.txt.gz"), "overwrite");
        check(failed && text_of(join(dest, "bad.txt")) == "old\n" && listdir(dest) == QStringList{"bad.txt"},
              "a failed decompress leaves the file it would have replaced, and no temporary file");
    }

    // -- failure-atomic everywhere else Kestrel writes (CLAUDE.md): compressing over an archive, its own files, new
    // files, moves between drives. A file-size limit stands for a full disk.
    {
        auto small_disk = [](const std::function<void()> &fn) {
            struct rlimit old_lim, lim;
            ::getrlimit(RLIMIT_FSIZE, &old_lim);
            lim = old_lim;
            lim.rlim_cur = 1 << 20;   // writes past 1 MB fail (EFBIG)
            auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
            ::setrlimit(RLIMIT_FSIZE, &lim);
            try {
                fn();
            } catch (...) {
            }
            ::setrlimit(RLIMIT_FSIZE, &old_lim);
            ::signal(SIGXFSZ, old_handler);
        };
        auto leftovers = [](const QString &d) {
            QStringList out;
            for (const QString &n : listdir(d))
                if (n.startsWith(".kes-"))
                    out << n;
            return out;
        };
        makedirs(P("fa/src"), true);
        make(P("fa/src/big.bin"), QByteArray(BIG, 'b'));
        make(P("fa/src/small.txt"), "small");
        make(P("fa/arc.tar"), "the old archive");
        archive::Format tar_fmt;
        for (const archive::Format &f : archive::formats())
            if (f.id == "tar")
                tar_fmt = f;
        auto compress = [&](const QString &rel) {
            Task task("test", [](Task *) { return QVariant(); });
            archive::Spec spec;
            spec.format = tar_fmt;
            spec.tool = "tar";
            spec.base = P("fa/src");
            spec.rels = {rel};
            spec.out = P("fa/arc.tar");
            spec.total = BIG;
            archive::compress(&task, spec);
        };
        small_disk([&]() { compress("big.bin"); });
        bool kept = text_of(P("fa/arc.tar")) == "the old archive" && leftovers(P("fa")).isEmpty();
        compress("small.txt");
        check(kept && proc::run({"tar", "tf", P("fa/arc.tar")}).out.trimmed() == "small.txt" &&
                  leftovers(P("fa")).isEmpty(),
              "compressing over an archive that fails part-way keeps the old archive and no half-made one; one that "
              "succeeds replaces it");

        make(P("fa/state.json"), "old");
        ::chmod(enc(P("fa/state.json")).constData(), 0600);
        small_disk([&]() { write_text(P("fa/state.json"), QByteArray(2 << 20, 'n')); });
        bool state_kept = text_of(P("fa/state.json")) == "old" && leftovers(P("fa")).isEmpty();
        write_text(P("fa/state.json"), "new");
        struct stat sst;
        stat_(P("fa/state.json"), sst);
        make(P("fa/dotfiles/bookmarks"), "old");
        ::symlink(enc(P("fa/dotfiles/bookmarks")).constData(), enc(P("fa/bookmarks")).constData());
        write_text(P("fa/bookmarks"), "new");
        check(state_kept && text_of(P("fa/state.json")) == "new" && (sst.st_mode & 0777) == 0600 &&
                  islink(P("fa/bookmarks")) && text_of(P("fa/dotfiles/bookmarks")) == "new" &&
                  leftovers(P("fa")).isEmpty(),
              "Kestrel's own files are written in one step: a write that fails part-way keeps the old contents; they "
              "keep their permissions, and a symlinked one stays a symlink");

        make(P("fa/taken.txt"), "mine");
        bool refused_copy = false, refused_new = false;
        try {
            copyfile(P("fa/src/small.txt"), P("fa/taken.txt"), true);
        } catch (const OSError &) {
            refused_copy = true;
        }
        try {
            write_text(P("fa/taken.txt"), "", true);
        } catch (const OSError &) {
            refused_new = true;
        }
        small_disk([&]() { copyfile(P("fa/src/big.bin"), P("fa/new-from-template.bin"), true); });
        check(refused_copy && refused_new && text_of(P("fa/taken.txt")) == "mine" &&
                  !lexists(P("fa/new-from-template.bin")) && leftovers(P("fa")).isEmpty(),
              "a new file (from a template, or pasted) never replaces one that's there, and one that fails part-way "
              "leaves nothing");

        // moves between drives (undo, restoring from the trash): /dev/shm is another filesystem
        QString other = "/dev/shm/kestrel-test-" + QString::number(::getpid());
        struct stat a, b;
        if (!stat_("/dev/shm", b) || !stat_(P("fa"), a) || a.st_dev == b.st_dev) {
            skip("moving between drives copies whole before deleting (no second filesystem here)");
        } else {
            makedirs(other, true);
            small_disk([&]() { util::move(P("fa/src/big.bin"), join(other, "big.bin")); });
            bool src_kept = getsize(P("fa/src/big.bin")) == BIG && !lexists(join(other, "big.bin")) &&
                            leftovers(other).isEmpty();
            util::move(P("fa/src"), join(other, "src"));
            check(src_kept && !lexists(P("fa/src")) && getsize(join(other, "src/big.bin")) == BIG &&
                      leftovers(other).isEmpty(),
                  "moving between drives copies whole before deleting: a failure part-way keeps the original and no "
                  "half-made copy");
            QDir(other).removeRecursively();
        }
    }

    // -- Shred with BleachBit: `bleachbit --shred` on the chosen files and folders, and what's still there afterwards
    // reported (BleachBit reports success either way); Empty Trash with BleachBit hands it every item in the trash and
    // its record; ✕ stops it. A stand-in bleachbit here: it only touches this test's home (the trash list also has
    // other drives' trash folders), leaves anything named "locked" (as BleachBit leaves what it can't change), and
    // takes its time over anything named "slow"
    {
        QString bin = P("stub-bin");
        makedirs(bin, true);
        write_text(join(bin, "bleachbit"), "#!/bin/sh\n"
                                           "printf '%s\\n' \"$@\" > \"$HOME/bleachbit-args\"\n"
                                           "shift 2\n"
                                           "for p in \"$@\"; do\n"
                                           "    case \"$p\" in \"$HOME\"/*) ;; *) continue ;; esac\n"
                                           "    case \"$p\" in *slow*) sleep 30 ;; esac\n"
                                           "    case \"$p\" in *locked*) ;; *) rm -rf -- \"$p\" ;; esac\n"
                                           "done\n");
        util::chmod(join(bin, "bleachbit"), 0755);
        QByteArray old_path = qgetenv("PATH");
        qputenv("PATH", bin.toUtf8() + ":" + old_path);
        make(P("shred/a.txt"));
        make(P("shred/dir/sub/b.txt"));
        make(P("shred/locked.txt"));
        QStringList paths{P("shred/a.txt"), P("shred/dir"), P("shred/locked.txt")};
        bool called = false;
        QStringList left;
        fileops::shred(w, paths, "Test", [&](const QStringList &l) {
            called = true;
            left = l;
        });
        wait_for([&]() { return called; }, 10000);
        check(fileops::can_shred() && called &&
                  text_of(P("bleachbit-args")).split('\n', Qt::SkipEmptyParts) == QStringList{"--shred", "--"} + paths &&
                  !lexists(P("shred/a.txt")) && !lexists(P("shred/dir")) && left == QStringList{P("shred/locked.txt")},
              "Shred with BleachBit runs bleachbit --shred on the chosen files and folders, and reports what's still there");

        make(P("shred/old.txt"));
        util::trash(P("shred/old.txt"));
        QString files = join(TRASH_DIR(), "files"), info = join(TRASH_DIR(), "info");
        QStringList mine;
        for (const QString &p : fileops::trash_contents())
            if (p.startsWith(TRASH_DIR() + "/"))
                mine << p;
        called = false;
        fileops::shred(w, mine, "Test", [&](const QStringList &) { called = true; });
        wait_for([&]() { return called; }, 10000);
        check(mine.contains(join(files, "old.txt")) && mine.contains(join(info, "old.txt.trashinfo")) && called &&
                  listdir(files).isEmpty() && listdir(info).isEmpty(),
              "Empty Trash with BleachBit hands it every item in the trash and its record, and the trash ends up empty");

        make(P("shred/slow.txt"));
        called = false;
        Task *t = fileops::shred(w, {P("shred/slow.txt")}, "Test", [&](const QStringList &) { called = true; });
        QPointer<Task> tp(t);
        spin(500);
        t->cancel();
        QElapsedTimer clock;
        clock.start();
        bool stopped = wait_for([&]() { return !tp; }, 5000);
        check(stopped && clock.elapsed() < 5000 && lexists(P("shred/slow.txt")) && !called,
              "✕ stops a shred: BleachBit is stopped, and nothing is reported as left");
        qputenv("PATH", old_path);
    }

    // -- KESTREL_STATS: what this test did was counted
    QString report = stats::summary();
    check(report.contains("  folder listing (ms): ") && report.contains("  copy speed (MB/s): ") &&
              report.contains("  archive job (s): ") && report.contains("  tasks at once: most "),
          "with KESTREL_STATS, folder listings, copy speed, archive jobs and tasks at once are counted");

    // -- running programs: a timeout holds even without pipes, or once the program has closed its output
    proc::Options quiet;
    quiet.out = proc::DEVNULL;
    quiet.err = proc::DEVNULL;
    QElapsedTimer clock;
    clock.start();
    auto r = proc::run({"sh", "-c", "echo $$ > \"$0\"; exec sleep 5", P("sleeper.pid")}, 300, quiet);
    pid_t pid = pid_t(QString::fromUtf8(read_file(P("sleeper.pid"))).trimmed().toInt());
    check(r.timed_out && clock.elapsed() < 2000 && pid > 0 && ::kill(pid, 0) != 0,
          "a program's timeout holds without pipes, and it's stopped");
    clock.restart();
    r = proc::run({"sh", "-c", "exec >&- 2>&-; sleep 5"}, 300);
    check(r.timed_out && clock.elapsed() < 2000, "a program's timeout holds after it closes its output");
    r = proc::run({"sh", "-c", "echo hi"}, 5000);
    check(!r.timed_out && r.rc == 0 && r.out == "hi\n", "a program that finishes in time isn't affected");
    ::unlink(P("left.pid").toLocal8Bit().constData());
    pid_t left = 0;
    {
        auto tool = proc::spawn({"sh", "-c", "echo $$ > \"$0\"; exec sleep 30", P("left.pid")}, quiet);
        wait_for([&]() { return QFile::exists(P("left.pid")) && !read_file(P("left.pid")).trimmed().isEmpty(); });
        left = pid_t(QString::fromUtf8(read_file(P("left.pid"))).trimmed().toInt());
    }   // nothing refers to it any more
    check(left > 0 && ::kill(left, 0) != 0, "a tool still running when nothing refers to it any more is stopped");
    // with SIGPIPE at its default, which kills (kes ignores it, and so does GLib once D-Bus is used, but proc mustn't
    // rely on either)
    struct sigaction dfl = {}, saved = {};
    dfl.sa_handler = SIG_DFL;
    sigaction(SIGPIPE, &dfl, &saved);
    r = proc::run({"true"}, 10000, proc::Options(), QByteArray(4 << 20, 'x'));
    sigaction(SIGPIPE, &saved, nullptr);
    check(!r.failed && !r.timed_out && r.rc == 0, "writing to a program that exits without reading its input doesn't kill Kestrel");

    // -- archive passwords: never on a tool's command line (where every user can see it), apart from zpaq's
    const QString pw = "Kestrel-pw-7731 \"quoted\"";
    QDir().mkpath(P("pwsrc"));
    {
        QFile f(P("pwsrc/data.bin"));
        f.open(QIODevice::WriteOnly);
        QFile rnd("/dev/urandom");
        rnd.open(QIODevice::ReadOnly);
        f.write(rnd.read(30 << 20));   // big enough that the tool runs a while, for the watcher to see it
    }
    for (const QString &t : QStringList{"rar", "zip"}) {
        QString label = t + ": a password-protected archive is made and opened, and the password is never on a "
                            "command line";
        if (archive::tool(t).isEmpty() || archive::tool(t == "rar" ? "unrar" : "7z").isEmpty()) {
            skip(label + " (" + t + " isn't installed)");
            continue;
        }
        archive::Format fmt;
        for (const archive::Format &f : archive::formats())
            if (f.id == t)
                fmt = f;
        QString out = P("secret." + t);
        std::atomic<bool> watching{true};
        std::atomic<int> seen{0}, leaked{0};
        std::thread watcher([&]() {   // every command line on the system, while the jobs run
            QByteArray needle = pw.left(15).toUtf8(), name = QByteArray("secret.") + t.toUtf8();
            while (watching) {
                for (const QString &pid : QDir("/proc").entryList(QDir::Dirs)) {
                    QFile f("/proc/" + pid + "/cmdline");
                    if (pid.at(0).isDigit() && f.open(QIODevice::ReadOnly)) {
                        QByteArray line = f.readAll();
                        if (line.contains(name)) {
                            seen++;
                            if (line.contains(needle))
                                leaked++;
                        }
                    }
                }
            }
        });
        Task task("test", [](Task *) { return QVariant(); });
        archive::Spec spec;
        spec.format = fmt;
        spec.tool = t;
        spec.base = P("pwsrc");
        spec.rels = {"data.bin"};
        spec.out = out;
        spec.level = 1;
        spec.password = pw;
        spec.total = 30 << 20;
        bool made = false, opened = false, refused = false;
        try {
            archive::compress(&task, spec);
            made = exists(out);
            QDir().mkpath(P("pwout-" + t));
            archive::extract(&task, out, P("pwout-" + t), pw, "overwrite");
            opened = util::read_file(P("pwout-" + t + "/data.bin")) == util::read_file(P("pwsrc/data.bin"));
            QDir().mkpath(P("pwbad-" + t));
            archive::extract(&task, out, P("pwbad-" + t), "wrong", "overwrite");
        } catch (const archive::WrongPassword &) {
            refused = true;
        } catch (const Error &) {
        }
        watching = false;
        watcher.join();
        check(made && opened && refused && seen > 0 && leaked == 0, label);
    }
    finish();
}
