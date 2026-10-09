// The admin helper (src/admin_helper.cpp), the part of Kestrel that runs as root: its operations, the folders it must
// never delete or replace, and symlinks. It runs here as the normal user (no pkexec) and gets its requests over its
// pipe, as Kestrel sends them. Paths it must refuse are ones that don't exist (/kestrel-test-none), so a slip can't
// touch anything real. A "victim" folder stands for a system folder: requests that reach it through a symlink must be
// refused, and it must stay unchanged.
#include "common.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <cstdio>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <thread>

using namespace test;
using namespace util;

class Helper {
public:
    bool start()
    {
        p.start(join(dirname(QString::fromLocal8Bit(qgetenv("KES_CXX"))), "kes-admin-helper"), {});
        return p.waitForStarted(5000) && next_reply(-1).value("hello").toBool();
    }

    // send a request; its final reply ({"ok": …}), skipping progress
    QJsonObject call(QJsonObject req, int ms = 20000)
    {
        qint64 id = send(req);
        return reply(id, ms);
    }

    qint64 send(QJsonObject req)
    {
        qint64 id = next_id++;
        req["id"] = id;
        p.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
        p.waitForBytesWritten(5000);
        return id;
    }

    void cancel(qint64 id)
    {
        p.write(QJsonDocument(QJsonObject{{"op", "cancel"}, {"id", id}}).toJson(QJsonDocument::Compact) + "\n");
        p.waitForBytesWritten(5000);
    }

    QJsonObject reply(qint64 id, int ms = 20000)
    {
        for (;;) {
            QJsonObject r = next_reply(id, ms);
            if (r.isEmpty() || r.contains("ok"))
                return r;
        }
    }

    // the next message for a request: a progress report ({"progress": [done, total, text]}) or its final reply
    QJsonObject next(qint64 id, int ms = 20000) { return next_reply(id, ms); }

    // close its input, as when the session ends; true if it then exits
    bool stop(int ms = 5000)
    {
        p.closeWriteChannel();
        return p.waitForFinished(ms);
    }

private:
    QJsonObject next_reply(qint64 id, int ms = 20000)
    {
        for (;;) {
            int nl = buf.indexOf('\n');
            if (nl >= 0) {
                QJsonObject r = QJsonDocument::fromJson(buf.left(nl)).object();
                buf.remove(0, nl + 1);
                if (id < 0 || r.value("id").toVariant().toLongLong() == id)
                    return r;
                continue;
            }
            if (!p.waitForReadyRead(ms))
                return QJsonObject();
            buf += p.readAllStandardOutput();
        }
    }

    QProcess p;
    QByteArray buf;
    qint64 next_id = 1;
};

static bool ok(const QJsonObject &r) { return r.value("ok").toBool(); }
static bool refused(const QJsonObject &r) { return !ok(r) && r.value("error").toString().contains("refusing"); }

static void write_file(const QString &p, const QByteArray &data = "x")
{
    QFile f(p);
    f.open(QIODevice::WriteOnly);
    f.write(data);
}

static int mode_of(const QString &p)
{
    struct stat st;
    return lstat(p.toLocal8Bit().constData(), &st) == 0 ? int(st.st_mode & 07777) : -1;
}

static bool is_link(const QString &p)
{
    struct stat st;
    return lstat(p.toLocal8Bit().constData(), &st) == 0 && S_ISLNK(st.st_mode);
}

// every name under a folder, with its permissions: to see that a folder is unchanged
static QStringList snapshot(const QString &dir)
{
    QStringList out;
    QDirIterator it(dir, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString p = it.next();
        out << QString("%1 %2").arg(p.mid(dir.size()), QString::number(mode_of(p), 8));
    }
    out.sort();
    return out;
}

static void make_tree(const QString &root, int dirs, int files)
{
    QDir().mkpath(root);
    for (int d = 0; d < dirs; ++d) {
        QString sub = join(root, QString("d%1").arg(d));
        QDir().mkpath(sub);
        for (int f = 0; f < files; ++f)
            write_file(join(sub, QString("f%1").arg(f)));
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString T = join(HOME(), "t");
    const QString W = join(T, "work");
    const QString victim = join(T, "victim");   // stands for a system folder
    const QString link = join(T, "link");       // → victim
    auto reset_victim = [&]() {   // as it was, so each check starts from the same victim folder
        QDir(victim).removeRecursively();
        QDir().mkpath(join(victim, "sub"));
        write_file(join(victim, "keep"), "keep");
        chmod(join(victim, "keep").toLocal8Bit().constData(), 0644);
    };
    QDir().mkpath(W);
    reset_victim();
    symlink(victim.toLocal8Bit().constData(), link.toLocal8Bit().constData());
    const QStringList before = snapshot(victim);
    auto victim_intact = [&]() { return snapshot(victim) == before && read_file(join(victim, "keep")) == "keep"; };

    Helper h;
    check(h.start(), "the helper starts and says hello");

    // -- the operations
    check(ok(h.call({{"op", "mkdir"}, {"path", join(W, "a/b/c")}})) && isdir(join(W, "a/b/c")),
          "mkdir makes a folder and its missing parents");
    bool made = ok(h.call({{"op", "touch"}, {"path", join(W, "t.txt")}})) &&
                ok(h.call({{"op", "write"}, {"path", join(W, "w.txt")}, {"text", "hello"}, {"mode", 0600}}));
    check(made && QFile::exists(join(W, "t.txt")) && read_file(join(W, "w.txt")) == "hello" &&
              mode_of(join(W, "w.txt")) == 0600,
          "touch and write make new files (write sets the mode)");
    check(!ok(h.call({{"op", "touch"}, {"path", join(W, "w.txt")}})) &&
              !ok(h.call({{"op", "write"}, {"path", join(W, "w.txt")}, {"text", "other"}})) &&
              read_file(join(W, "w.txt")) == "hello",
          "touch and write refuse an existing file");
    check(ok(h.call({{"op", "copyfile"}, {"src", join(W, "w.txt")}, {"dst", join(W, "c.txt")}})) &&
              read_file(join(W, "c.txt")) == "hello" &&
              !ok(h.call({{"op", "copyfile"}, {"src", join(W, "t.txt")}, {"dst", join(W, "c.txt")}})),
          "copyfile copies a file and won't overwrite one");

    QString tree = join(W, "tree");
    make_tree(tree, 2, 2);
    symlink(victim.toLocal8Bit().constData(), join(tree, "to-victim").toLocal8Bit().constData());
    bool copied = ok(h.call({{"op", "copy"}, {"src", tree}, {"dst", join(W, "tree2")}}));
    check(copied && QFile::exists(join(W, "tree2/d1/f1")) && is_link(join(W, "tree2/to-victim")) &&
              QFile::symLinkTarget(join(W, "tree2/to-victim")) == victim && victim_intact(),
          "copy copies a folder tree, and symlinks in it as links");
    check(ok(h.call({{"op", "move"}, {"src", join(W, "tree2")}, {"dst", join(W, "tree3")}})) &&
              !QFile::exists(join(W, "tree2")) && QFile::exists(join(W, "tree3/d0/f0")),
          "move moves a folder");
    // the same entry as source and destination: the helper must notice (its "replace the destination" step would
    // delete the source), whatever Kestrel sends
    auto contents = [](const QString &root) {   // names and file contents
        QStringList out;
        QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString f = it.next();
            out << relpath(f, root) + "=" + (QFileInfo(f).isFile() ? QString::fromUtf8(read_file(f)) : QString());
        }
        out.sort();
        return out;
    };
    write_file(join(W, "self.txt"), "mine");
    QStringList tree_before = contents(join(W, "tree3"));
    h.call({{"op", "move"}, {"src", join(W, "self.txt")}, {"dst", join(W, "self.txt")}});
    h.call({{"op", "copy"}, {"src", join(W, "self.txt")}, {"dst", join(W, "self.txt")}});
    h.call({{"op", "move"}, {"src", join(W, "tree3")}, {"dst", join(W, "tree3")}});
    h.call({{"op", "copy"}, {"src", join(W, "tree3")}, {"dst", join(W, "tree3")}});
    h.call({{"op", "copy"}, {"src", join(W, "tree3")}, {"dst", join(W, "tree3")}, {"merge", true}});
    check(read_file(join(W, "self.txt")) == "mine" && !tree_before.isEmpty() && contents(join(W, "tree3")) == tree_before,
          "moving or copying a file or folder onto itself leaves it as it was");
    QJsonObject into_move = h.call({{"op", "move"}, {"src", join(W, "tree3")}, {"dst", join(W, "tree3/d0/in")}}, 10000);
    QJsonObject into_copy = h.call({{"op", "copy"}, {"src", join(W, "tree3")}, {"dst", join(W, "tree3/d0/in")}}, 10000);
    check(!into_move.isEmpty() && !ok(into_move) && !into_copy.isEmpty() && !ok(into_copy) &&
              contents(join(W, "tree3")) == tree_before,
          "moving or copying a folder into itself is refused, and the folder is left as it was");
    write_file(join(W, "hl-a"), "linked");
    (void)!::link(enc(join(W, "hl-a")).constData(), enc(join(W, "hl-b")).constData());
    h.call({{"op", "move"}, {"src", join(W, "hl-a")}, {"dst", join(W, "hl-b")}});
    check(read_file(join(W, "hl-b")) == "linked", "moving a file onto a hard link to it keeps the file");

    // -- replacing: what's replaced stays as it was until the new one is complete (a failure, a cancel or the session
    // ending leaves it, and no half-made copy); a folder is swapped in whole
    auto leftovers = [&]() {   // half-made copies (".kes-….part") anywhere in W
        QStringList out;
        QDirIterator it(W, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString f = it.next();
            if (basename(f).startsWith(".kes-"))
                out << f;
        }
        return out;
    };
    write_file(join(W, "big6"), QByteArray(6 << 20, 'b'));
    write_file(join(W, "conf.txt"), "original");
    QDir().mkpath(join(W, "newdir"));
    write_file(join(W, "newdir/a.txt"), "a");
    write_file(join(W, "newdir/big"), QByteArray(6 << 20, 'n'));
    QDir().mkpath(join(W, "olddir"));
    write_file(join(W, "olddir/keep.txt"), "keep");
    QStringList old_before = contents(join(W, "olddir"));
    {
        // a helper whose writes fail past 1 MB (EFBIG), as on a full disk: limits pass to the program it starts
        Helper small;
        struct rlimit old_lim, lim;
        ::getrlimit(RLIMIT_FSIZE, &old_lim);
        lim = old_lim;
        lim.rlim_cur = 1 << 20;
        auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
        ::setrlimit(RLIMIT_FSIZE, &lim);
        bool started = small.start();
        ::setrlimit(RLIMIT_FSIZE, &old_lim);
        ::signal(SIGXFSZ, old_handler);
        QJsonObject file_r = small.call({{"op", "copy"}, {"src", join(W, "big6")}, {"dst", join(W, "conf.txt")}});
        QJsonObject dir_r = small.call({{"op", "copy"}, {"src", join(W, "newdir")}, {"dst", join(W, "olddir")}});
        small.stop();
        check(started && !file_r.isEmpty() && !ok(file_r) && read_file(join(W, "conf.txt")) == "original" &&
                  leftovers().isEmpty(),
              "replacing a file: a write that fails part-way (a full disk) keeps the old file, and leaves no "
              "half-made copy");
        check(!dir_r.isEmpty() && !ok(dir_r) && contents(join(W, "olddir")) == old_before && leftovers().isEmpty(),
              "replacing a folder: a failure part-way keeps the old folder as it was, and leaves no half-made copy");
    }
    write_file(join(W, "huge"), "x");
    ::truncate(enc(join(W, "huge")).constData(), qint64(2) << 30);   // 2 GB (sparse): long enough to stop part-way
    {
        qint64 id = h.send({{"op", "copy"}, {"src", join(W, "huge")}, {"dst", join(W, "conf.txt")}});
        QJsonObject m = h.next(id);
        QJsonArray pr = m.value("progress").toArray();
        bool partway = !pr.isEmpty() && pr[0].toDouble() > 0 && pr[0].toDouble() < pr[1].toDouble() / 2;
        QElapsedTimer clock;
        clock.start();
        h.cancel(id);
        QJsonObject r = h.reply(id);
        check(partway && r.value("cancelled").toBool() && clock.elapsed() < 2000 &&
                  read_file(join(W, "conf.txt")) == "original" && leftovers().isEmpty(),
              "a large file's copy reports its bytes as it goes, and a cancel stops it part-way at once, keeping "
              "the file it was replacing and no half-made copy");
    }
    {
        Helper ending;
        bool started = ending.start();
        qint64 id = ending.send({{"op", "copy"}, {"src", join(W, "huge")}, {"dst", join(W, "conf.txt")}});
        bool partway = !ending.next(id).value("progress").toArray().isEmpty();
        bool exited = ending.stop();
        check(started && partway && exited && read_file(join(W, "conf.txt")) == "original" && leftovers().isEmpty(),
              "ending the admin session part-way through a copy stops it, keeping the file it was replacing and no "
              "half-made copy");
    }
    QFile::remove(join(W, "huge"));
    QDir().mkpath(join(W, "mvsrc"));
    write_file(join(W, "mvsrc/moved.txt"), "moved");
    QDir().mkpath(join(W, "mvdst"));
    write_file(join(W, "mvdst/old.txt"), "old");
    bool copied_over = ok(h.call({{"op", "copy"}, {"src", join(W, "newdir")}, {"dst", join(W, "olddir")}}));
    bool moved_over = ok(h.call({{"op", "move"}, {"src", join(W, "mvsrc")}, {"dst", join(W, "mvdst")}}));
    check(copied_over && contents(join(W, "olddir")) == contents(join(W, "newdir")) && moved_over &&
              !QFile::exists(join(W, "mvsrc")) && contents(join(W, "mvdst")) == QStringList{"moved.txt=moved"} &&
              leftovers().isEmpty(),
          "copying or moving a folder onto another replaces it whole: the old contents are gone, nothing is left "
          "behind");
    write_file(join(W, "r1"));
    check(ok(h.call({{"op", "rename"}, {"src", join(W, "r1")}, {"dst", join(W, "r2")}})) &&
              QFile::exists(join(W, "r2")) &&
              !ok(h.call({{"op", "rename"}, {"src", join(W, "r2")}, {"dst", join(W, "c.txt")}})) &&
              read_file(join(W, "c.txt")) == "hello",
          "rename renames, and won't replace an existing name");
    check(ok(h.call({{"op", "symlink"}, {"target", "w.txt"}, {"link", join(W, "sl")}})) &&
              QFile::symLinkTarget(join(W, "sl")) == join(W, "w.txt") &&
              ok(h.call({{"op", "hardlink"}, {"target", join(W, "w.txt")}, {"link", join(W, "hl")}})) &&
              read_file(join(W, "hl")) == "hello",
          "symlink and hardlink make links");
    QJsonObject other = h.call({{"op", "hardlink"}, {"target", "/etc/hostname"}, {"link", join(W, "not-mine")}});
    check(!ok(other) && other.value("error").toString().contains("isn't yours") && !QFile::exists(join(W, "not-mine")),
          "hardlink refuses a file that isn't the user's (the helper's own rule, before the kernel's)");
    check(ok(h.call({{"op", "chmod"}, {"path", join(W, "c.txt")}, {"mode", 0640}})) && mode_of(join(W, "c.txt")) == 0640,
          "chmod changes permissions");
    check(ok(h.call({{"op", "delete"}, {"path", join(W, "tree3")}})) && !QFile::exists(join(W, "tree3")),
          "delete removes a folder tree");
    check(ok(h.call({{"op", "delete"}, {"path", tree}})) && !QFile::exists(tree) && victim_intact(),
          "deleting a tree with a symlink inside removes the link, not what it points to");
    symlink(victim.toLocal8Bit().constData(), join(W, "lone-link").toLocal8Bit().constData());
    check(ok(h.call({{"op", "delete"}, {"path", join(W, "lone-link")}})) && !is_link(join(W, "lone-link")) &&
              victim_intact(),
          "deleting a symlink removes the link only");
    make_tree(join(W, "big"), 20, 100);
    qint64 id = h.send({{"op", "copy"}, {"src", join(W, "big")}, {"dst", join(W, "big2")}});
    h.cancel(id);
    check(h.reply(id).value("cancelled").toBool(), "a running job can be cancelled");

    // -- paths
    check(!ok(h.call({{"op", "delete"}, {"path", "relative/x"}})) &&
              h.call({{"op", "delete"}, {"path", "relative/x"}}).value("error").toString().contains("absolute"),
          "relative paths are refused");

    // -- folders that are never deleted or replaced (paths that don't exist: refused, not "not found")
    const QStringList top = {"/kestrel-test-none", "/home/kestrel-test-none"};
    bool all = true;
    for (const QString &p : top)
        all = all && refused(h.call({{"op", "delete"}, {"path", p}}));
    check(all, "top-level and home folders can't be deleted");
    all = true;
    for (const QString &p : top)
        all = all && refused(h.call({{"op", "copy"}, {"src", join(W, "c.txt")}, {"dst", p}})) &&
              refused(h.call({{"op", "move"}, {"src", join(W, "c.txt")}, {"dst", p}}));
    check(all && QFile::exists(join(W, "c.txt")), "...or replaced by a copy or move");
    all = true;
    for (const QString &p : top)
        all = all && refused(h.call({{"op", "move"}, {"src", p}, {"dst", join(W, "moved")}}));
    check(all, "...or moved away");
    all = true;
    for (const QString &p : top)
        all = all && refused(h.call({{"op", "rename"}, {"src", p}, {"dst", join(W, "renamed")}}));
    check(all, "...or renamed");
    all = true;
    for (const QString &p : top)
        all = all && refused(h.call({{"op", "chmod"}, {"path", p}, {"mode", 0777}}));
    check(all, "...or have their permissions changed");

    // -- symlinks in the path: refused, and the victim folder stays as it was
    reset_victim();
    check(!ok(h.call({{"op", "mkdir"}, {"path", join(link, "new/deeper")}})) && victim_intact(),
          "mkdir through a symlinked folder is refused");
    reset_victim();
    check(!ok(h.call({{"op", "touch"}, {"path", join(link, "t")}})) &&
              !ok(h.call({{"op", "write"}, {"path", join(link, "w")}, {"text", "x"}})) &&
              !ok(h.call({{"op", "copyfile"}, {"src", join(W, "c.txt")}, {"dst", join(link, "c")}})) && victim_intact(),
          "new files through a symlinked folder are refused (touch, write, copyfile)");
    reset_victim();
    check(!ok(h.call({{"op", "symlink"}, {"target", "/"}, {"link", join(link, "s")}})) &&
              !ok(h.call({{"op", "hardlink"}, {"target", join(W, "c.txt")}, {"link", join(link, "h")}})) &&
              victim_intact(),
          "links through a symlinked folder are refused (symlink, hardlink)");
    reset_victim();
    check(!ok(h.call({{"op", "delete"}, {"path", join(link, "keep")}})) && victim_intact(),
          "delete through a symlinked folder is refused");
    reset_victim();
    check(!ok(h.call({{"op", "copy"}, {"src", join(W, "c.txt")}, {"dst", join(link, "keep")}})) &&
              !ok(h.call({{"op", "move"}, {"src", join(W, "w.txt")}, {"dst", join(link, "keep")}})) && victim_intact(),
          "copy or move into a symlinked folder is refused");
    reset_victim();
    check(!ok(h.call({{"op", "rename"}, {"src", join(link, "keep")}, {"dst", join(W, "out")}})) && victim_intact(),
          "rename out of a symlinked folder is refused");
    reset_victim();
    check(!ok(h.call({{"op", "chmod"}, {"path", join(link, "keep")}, {"mode", 0777}})) && victim_intact(),
          "chmod through a symlinked folder is refused");
    reset_victim();
    symlink(join(victim, "keep").toLocal8Bit().constData(), join(W, "to-keep").toLocal8Bit().constData());
    h.call({{"op", "chmod"}, {"path", join(W, "to-keep")}, {"mode", 0777}});
    check(victim_intact(), "chmod on a symlink doesn't change what it points to");

    // -- Kestrel's side: the user's own symlinked folders are resolved before the helper sees them
    QVariantMap args = admin::resolve_paths("delete", {{"path", join(link, "keep")}});
    QVariantMap whole = admin::resolve_paths("chmod", {{"path", join(W, "to-keep")}, {"mode", 0600}});
    check(args.value("path").toString() == join(victim, "keep") &&
              admin::resolve_paths("delete", {{"path", join(W, "to-keep")}}).value("path").toString() == join(W, "to-keep") &&
              whole.value("path").toString() == join(victim, "keep") && whole.value("mode").toInt() == 0600,
          "Kestrel resolves the user's own symlinked folders first (chmod: the whole path; a link itself otherwise)");

    // -- a folder swapped for a symlink while a delete runs: renameat2(RENAME_EXCHANGE) trades a folder in the tree
    // for a symlink to the victim (kept outside the tree) and back, in one step each, so the path always exists
    // (a race: the old helper loses it in some rounds, not all, so rounds run for a few seconds)
    std::atomic<bool> stop{false};
    bool safe = true;
    QElapsedTimer clock;
    clock.start();
    for (int round = 0; clock.elapsed() < 4000 && safe; ++round) {
        reset_victim();
        QString race = join(T, QString("race%1").arg(round)), links = join(T, QString("links%1").arg(round));
        make_tree(race, 40, 5);
        QDir().mkpath(links);
        for (int d = 0; d < 40; ++d)
            symlink(victim.toLocal8Bit().constData(), join(links, QString("d%1").arg(d)).toLocal8Bit().constData());
        stop = false;
        std::thread swapper([&]() {
            while (!stop) {
                for (int d = 0; d < 40 && !stop; ++d) {
                    QByteArray a = join(race, QString("d%1").arg(d)).toLocal8Bit();
                    QByteArray b = join(links, QString("d%1").arg(d)).toLocal8Bit();
                    if (renameat2(AT_FDCWD, a.constData(), AT_FDCWD, b.constData(), RENAME_EXCHANGE) == 0) {
                        usleep(200);
                        renameat2(AT_FDCWD, a.constData(), AT_FDCWD, b.constData(), RENAME_EXCHANGE);
                    }
                }
            }
        });
        h.call({{"op", "delete"}, {"path", race}});
        stop = true;
        swapper.join();
        safe = victim_intact();
    }
    check(safe, "a folder swapped for a symlink during a delete doesn't let it delete outside the tree");

    h.stop();
    finish();
}
