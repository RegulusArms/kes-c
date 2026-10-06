// File operations: copy, move, merge, replace, delete, cancel, trash, links, unique names, and undoing them.
#include "common.h"

#include "archive_ui.h"

#include <QElapsedTimer>
#include <QMessageBox>

#include <signal.h>
#include <sys/stat.h>
#include <utime.h>

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
    finish();
}
