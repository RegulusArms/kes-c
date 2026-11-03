#include "ramdisk.h"

#include "admin.h"
#include "atc.h"
#include "fileops.h"
#include "prefs.h"
#include "proc.h"
#include "util.h"
#include "windows.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSet>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/xattr.h>
#include <unistd.h>

using namespace util;

namespace ramdisk {

static const char *MARK_ATTR = "user.kestrel-ramdisk";   // on a loaded RAM disk's top folder: "<id>" or "<id> partial"

static QString life_key(Life l)
{
    return l == EXIT ? "exit" : l == SESSION ? "session" : l == PERMANENT ? "permanent" : "persistent";
}

static Life life_of(const QString &k)
{
    return k == "exit" ? EXIT : k == "permanent" ? PERMANENT : k == "persistent" ? PERSISTENT : SESSION;
}

// ---------------------------------------------------------------- the list

QString list_file() { return join(CONFIG_DIR(), "ramdisks.json"); }

QList<Disk> disks()
{
    QList<Disk> out;
    QJsonArray a = QJsonDocument::fromJson(read_file(list_file())).object().value("disks").toArray();
    for (const QJsonValue &v : a) {
        QJsonObject o = v.toObject();
        Disk d;
        d.id = o.value("id").toString();
        d.name = o.value("name").toString();
        d.kind = o.value("kind").toString() == "mount" ? MOUNT : FOLDER;
        d.path = o.value("path").toString();
        d.life = life_of(o.value("life").toString());
        d.size_mb = qint64(o.value("size_mb").toDouble());
        d.save_to = o.value("save_to").toString();
        d.interval = o.value("interval").toInt(15);
        if (!d.id.isEmpty() && d.path.startsWith('/') && !find(out, d.id))
            out << d;
    }
    return out;
}

void save_disks(const QList<Disk> &list)
{
    QJsonArray a;
    for (const Disk &d : list) {
        QJsonObject o{{"id", d.id},     {"name", d.name},       {"kind", d.kind == MOUNT ? "mount" : "folder"},
                      {"path", d.path}, {"life", life_key(d.life)}, {"size_mb", double(d.size_mb)},
                      {"interval", d.interval}};
        if (!d.save_to.isEmpty())
            o["save_to"] = d.save_to;
        a << o;
    }
    makedirs(CONFIG_DIR(), true);
    write_atomic(list_file(), QJsonDocument(QJsonObject{{"disks", a}}).toJson());
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        atc::announce("sidebar");   // every Kestrel's sidebar (this one's too)
}

const Disk *find(const QList<Disk> &list, const QString &id)
{
    for (const Disk &d : list)
        if (d.id == id)
            return &d;
    return nullptr;
}

QString new_id(const QList<Disk> &list)
{
    for (;;) {
        QString id = QString("%1").arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));
        if (!find(list, id))
            return id;
    }
}

// ---------------------------------------------------------------- where they are

QString shm_root()
{
    // (KESTREL_SHM_DIR: the tests' own, so their towers never delete a real private RAM disk)
    QString base = qEnvironmentVariableIsSet("KESTREL_SHM_DIR") ? qEnvironmentVariable("KESTREL_SHM_DIR")
                   : isdir("/dev/shm")                         ? QString("/dev/shm")
                                                               : QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
    if (base.isEmpty())
        return QString();
    QString root = join(base, QString("kestrel-explorer-%1").arg(getuid()));
    QByteArray r = enc(root);
    if (::mkdir(r.constData(), 0700) != 0 && errno != EEXIST)
        return QString();
    // someone else's folder (or a symlink) by that name in the shared /dev/shm: not ours to use
    struct stat st;
    if (::lstat(r.constData(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid())
        return QString();
    if ((st.st_mode & 0777) != 0700)
        ::chmod(r.constData(), 0700);
    return root;
}

QString folder_path(const QString &name)
{
    QString root = shm_root();
    return root.isEmpty() ? QString() : join(root, name);
}

QString name_problem(const QString &name)
{
    if (name.trimmed().isEmpty())
        return tx("Give it a name.");
    if (name.contains('/') || name.startsWith('.'))
        return tx("A name can't contain “/” or start with “.”.");
    for (QChar c : name)
        if (c.category() == QChar::Other_Control)
            return tx("A name can't contain control characters.");
    return QString();
}

bool is_tmpfs_mount(const QString &path)
{
    struct statfs sf;
    struct stat st, up;
    QByteArray p = enc(path);
    if (::statfs(p.constData(), &sf) != 0 || sf.f_type != TMPFS_MAGIC)
        return false;
    // a mount point: its parent is on another file system
    return ::stat(p.constData(), &st) == 0 && ::stat(enc(dirname(path)).constData(), &up) == 0 && st.st_dev != up.st_dev;
}

bool online(const Disk &d) { return d.kind == MOUNT ? is_tmpfs_mount(d.path) : isdir(d.path); }

bool is_empty(const QString &dir)
{
    try {
        return listdir(dir).isEmpty();
    } catch (const OSError &) {
        return true;
    }
}

QString life_text(Life l, Kind k)
{
    switch (l) {
    case EXIT:
        return tx("Until Kestrel closes");
    case SESSION:
        return k == MOUNT ? tx("Until you restart") : tx("Until you log out or restart");
    case PERMANENT:
        return k == MOUNT ? tx("Always: back, empty, at every start") : tx("Always: back, empty, at every login");
    case PERSISTENT:
        return tx("Always, with its files saved to disk");
    }
    return QString();
}

QString describe(const Disk &d)
{
    QString what = d.kind == MOUNT ? tx("RAM disk of %1, mounted on %2").arg(human_size(d.size_mb * 1024 * 1024), d.path)
                                   : tx("RAM disk in %1").arg(d.path);
    QString life = life_text(d.life, d.kind);
    if (d.life == PERSISTENT)
        life = d.interval > 0 ? txn("Saved to %1 every %Ln minute(s), at logout and at shutdown", d.interval).arg(d.save_to)
                              : tx("Saved to %1 at logout and at shutdown").arg(d.save_to);
    return what + "\n" + life + (online(d) ? QString() : "\n" + tx("Not in memory now: click to bring it back"));
}

// ---------------------------------------------------------------- the mirror

static bool is_part(const QString &name) { return name.startsWith(".kes-") && name.endsWith(".part"); }

namespace {

struct Mirror {
    bool deletes;
    QStringList *errors;
    const std::function<bool()> &cancelled;
    MirrorStats st;

    void check()
    {
        if (cancelled && cancelled())
            throw Cancelled();
    }
    void failed(const QString &path, int e) { *errors << errno_text(e, path); }

    // remove what's at path (a file, link or whole folder), not following links
    void remove(const QString &path)
    {
        struct stat s;
        if (!lstat_(path, s))
            return;
        if (S_ISDIR(s.st_mode)) {
            rmtree(path);
            if (lexists(path))
                failed(path, EACCES);
            else
                st.deleted++;
        } else if (::unlink(enc(path).constData()) == 0) {
            st.deleted++;
        } else {
            failed(path, errno);
        }
    }

    // put a finished part file in place of dst: a folder that was there goes after the file is whole (a file can't be
    // renamed over a folder); what was there is replaced only now, never emptied or deleted first
    bool put(const QString &part, const QString &dst)
    {
        struct stat s;
        if (lstat_(dst, s) && S_ISDIR(s.st_mode))
            rmtree(dst);
        if (::rename(enc(part).constData(), enc(dst).constData()) != 0) {
            failed(dst, errno);
            ::unlink(enc(part).constData());
            return false;
        }
        return true;
    }

    void copy_file(const QString &src, const struct stat &ss, const QString &dst, bool replacing)
    {
        int in = ::open(enc(src).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (in < 0)
            return failed(src, errno);
        QString part;
        int out = -1;
        int dfd = ::open(enc(dirname(dst)).constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        try {
            if (dfd < 0)
                throw_errno(dirname(dst));
            out = open_part_at(dfd, QString(), &part);
        } catch (const OSError &e) {
            ::close(in);
            if (dfd >= 0)
                ::close(dfd);
            *errors << e.message();
            return;
        }
        ::close(dfd);
        QString part_path = join(dirname(dst), part);
        bool ok = true;
        int err = 0;
        qint64 left = ss.st_size;
        while (ok && left > 0) {
            check();
            ssize_t n = ::copy_file_range(in, nullptr, out, nullptr, size_t(std::min<qint64>(left, 8 << 20)), 0);
            if (n < 0 && (errno == EXDEV || errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)) {
                char buf[1 << 16];
                ssize_t r = ::read(in, buf, sizeof buf);
                if (r < 0) {
                    ok = false, err = errno;
                } else if (r == 0) {
                    break;
                } else {
                    try {
                        write_all(out, buf, r, dst);
                    } catch (const OSError &e) {
                        ok = false, err = e.code;
                    }
                    n = r;
                }
            } else if (n < 0) {
                if (errno == EINTR)
                    continue;
                ok = false, err = errno;
            } else if (n == 0) {
                break;   // it got shorter meanwhile
            }
            if (ok && n > 0)
                left -= n, st.bytes += n;
        }
        ::close(in);
        if (ok) {
            struct timespec times[2] = {ss.st_atim, ss.st_mtim};
            ::fchmod(out, ss.st_mode & 07777);
            ::futimens(out, times);
            // replacing a saved file: on the disk before the rename, so a crash leaves the old one or the new
            if (replacing && ::fsync(out) != 0)
                ok = false, err = errno;
        }
        if (::close(out) != 0 && ok)
            ok = false, err = errno;
        if (!ok) {
            ::unlink(enc(part_path).constData());
            return failed(dst, err);
        }
        if (put(part_path, dst))
            st.copied++;
    }

    void copy_link(const QString &src, const QString &dst)
    {
        QString target;
        try {
            target = readlink(src);
        } catch (const OSError &e) {
            *errors << e.message();
            return;
        }
        struct stat s;
        if (lstat_(dst, s) && S_ISLNK(s.st_mode)) {
            try {
                if (readlink(dst) == target)
                    return;
            } catch (const OSError &) {
            }
        }
        QString part = join(dirname(dst), part_name());
        if (::symlink(enc(target).constData(), enc(part).constData()) != 0)
            return failed(dst, errno);
        if (put(part, dst))
            st.copied++;
    }

    void dir(const QString &src, const QString &dst)
    {
        check();
        QStringList names;
        try {
            names = listdir(src);
        } catch (const OSError &e) {
            *errors << e.message();
            return;
        }
        QSet<QString> here;
        for (const QString &n : names) {
            if (is_part(n))
                continue;   // a copy into the RAM disk still being made (or ours, left by a crash)
            here << n;
            check();
            QString s = join(src, n), d = join(dst, n);
            struct stat ss, ds;
            if (!lstat_(s, ss))
                continue;   // gone meanwhile
            bool there = lstat_(d, ds);
            if (S_ISDIR(ss.st_mode)) {
                if (there && !S_ISDIR(ds.st_mode))
                    remove(d);   // a file where a folder is now: it isn't in the source any more
                if (!isdir(d) && ::mkdir(enc(d).constData(), 0700) != 0) {
                    failed(d, errno);
                    continue;
                }
                dir(s, d);
                ::chmod(enc(d).constData(), ss.st_mode & 07777);
                struct timespec times[2] = {ss.st_atim, ss.st_mtim};
                ::utimensat(AT_FDCWD, enc(d).constData(), times, AT_SYMLINK_NOFOLLOW);
            } else if (S_ISREG(ss.st_mode)) {
                bool same = there && S_ISREG(ds.st_mode) && ds.st_size == ss.st_size &&
                            ds.st_mtim.tv_sec == ss.st_mtim.tv_sec && ds.st_mtim.tv_nsec == ss.st_mtim.tv_nsec;
                if (!same)
                    copy_file(s, ss, d, there);
            } else if (S_ISLNK(ss.st_mode)) {
                copy_link(s, d);
            }
            // devices, FIFOs and sockets aren't files to keep
        }
        if (!deletes)
            return;
        QStringList old;
        try {
            old = listdir(dst);
        } catch (const OSError &e) {
            *errors << e.message();
            return;
        }
        for (const QString &n : old)
            if (!here.contains(n)) {
                check();
                remove(join(dst, n));   // (also our own parts, left by an earlier save that was cut off)
            }
    }
};

}  // namespace

MirrorStats mirror(const QString &src, const QString &dst, bool deletes, QStringList *errors,
                   const std::function<bool()> &cancelled)
{
    QStringList none;
    if (!errors)
        errors = &none;
    if (!isdir(src))
        throw_errno(src, ENOENT);
    if (!isdir(dst))
        throw_errno(dst, ENOENT);
    Mirror m{deletes, errors, cancelled, {}};
    m.dir(src, dst);
    return m.st;
}

// ---------------------------------------------------------------- loaded or not

static QString runtime_mark(const QString &id)   // where tmpfs can't hold the attribute
{
    QString rt = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
    return rt.isEmpty() ? QString() : join(join(rt, APP_ID), "ramdisk-" + id + ".loaded");
}

static QString mark_of(const QString &root, const QString &id)
{
    char buf[128];
    ssize_t n = ::getxattr(enc(root).constData(), MARK_ATTR, buf, sizeof buf - 1);
    if (n > 0)
        return QString::fromUtf8(buf, int(n));
    // the fallback: the top folder's device and inode, valid while it's the same folder (this login)
    QString f = runtime_mark(id);
    struct stat st;
    if (f.isEmpty() || !stat_(root, st))
        return QString();
    QStringList parts = QString::fromUtf8(read_file(f)).split(' ');
    if (parts.value(0) != QString("%1:%2").arg(st.st_dev).arg(st.st_ino))
        return QString();
    return id + (parts.value(1) == "partial" ? " partial" : "");
}

bool loaded(const QString &root, const QString &id) { return mark_of(root, id).section(' ', 0, 0) == id; }

static void set_mark(const QString &root, const QString &id, bool partial)
{
    QByteArray v = (id + (partial ? " partial" : "")).toUtf8();
    if (::setxattr(enc(root).constData(), MARK_ATTR, v.constData(), size_t(v.size()), 0) == 0)
        return;
    QString f = runtime_mark(id);
    struct stat st;
    if (f.isEmpty() || !stat_(root, st))
        return;
    try {
        makedirs(dirname(f), true, 0700);
        write_atomic(f, QString("%1:%2%3").arg(st.st_dev).arg(st.st_ino).arg(partial ? " partial" : "").toUtf8());
    } catch (const OSError &) {
    }
}

void mark_loaded(const QString &root, const QString &id) { set_mark(root, id, false); }

bool load(const Disk &d, QStringList *errors, const std::function<bool()> &cancelled)
{
    if (!online(d)) {
        *errors << tx("“%1” isn't in memory.").arg(d.name);
        return false;
    }
    if (d.life != PERSISTENT || loaded(d.path, d.id))
        return true;
    if (!isdir(d.save_to)) {
        try {
            makedirs(d.save_to, true, 0700);   // nothing saved yet
        } catch (const OSError &e) {
            *errors << e.message();
            return false;
        }
        mark_loaded(d.path, d.id);
        return true;
    }
    // into what's there already (nothing, as a rule), deleting nothing
    QStringList errs;
    mirror(d.save_to, d.path, false, &errs, cancelled);
    // a file that couldn't be loaded must not be deleted from the saved copy by the next save: partly loaded, saves
    // then only add and replace
    set_mark(d.path, d.id, !errs.isEmpty());
    *errors << errs;
    return errs.isEmpty();
}

bool save(const Disk &d, QStringList *errors, const std::function<bool()> &cancelled)
{
    if (d.life != PERSISTENT)
        return true;
    if (!online(d)) {
        *errors << tx("“%1” isn't in memory: nothing to save.").arg(d.name);
        return false;
    }
    QString mark = mark_of(d.path, d.id);
    if (mark.section(' ', 0, 0) != d.id) {
        // came up without its files: saving now would empty the saved copy
        *errors << tx("“%1” hasn't been loaded from %2 yet, so it isn't saved over it.").arg(d.name, d.save_to);
        return false;
    }
    try {
        makedirs(d.save_to, true, 0700);
    } catch (const OSError &e) {
        *errors << e.message();
        return false;
    }
    // one save at a time (the timer's, logging out's, Save to Disk Now)
    int lock = ::open(enc(d.save_to).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (lock >= 0)
        ::flock(lock, LOCK_EX);
    QStringList errs;
    try {
        mirror(d.path, d.save_to, !mark.endsWith(" partial"), &errs, cancelled);
    } catch (...) {
        if (lock >= 0)
            ::close(lock);
        throw;
    }
    if (lock >= 0)
        ::close(lock);
    *errors << errs;
    return errs.isEmpty();
}

// ---------------------------------------------------------------- the user units' command line

int run_cli(int argc, char **argv)
{
    QString what, id;
    for (int i = 1; i + 1 < argc; ++i) {
        QByteArray a(argv[i]);
        if (a == "--ramdisk-start" || a == "--ramdisk-stop" || a == "--ramdisk-save") {
            what = QString::fromLatin1(a.mid(10));
            id = QString::fromLocal8Bit(argv[i + 1]);
        }
    }
    if (what.isEmpty())
        return -1;
    QCoreApplication app(argc, argv);
    QList<Disk> list = disks();
    const Disk *found = find(list, id);
    if (!found) {
        std::fprintf(stderr, "kes: no RAM disk %s\n", qPrintable(id));
        return 2;
    }
    Disk d = *found;
    QStringList errors;
    bool ok = true;
    if (what == "start") {
        if (d.kind == FOLDER && !isdir(d.path)) {
            if (shm_root().isEmpty() || ::mkdir(enc(d.path).constData(), 0700) != 0) {
                std::perror(qPrintable(d.path));
                return 1;
            }
        }
        for (int i = 0; i < 600 && !online(d); ++i)   // a MOUNT one: its mount unit, at boot (a minute at most)
            QThread::msleep(100);
        ok = load(d, &errors);
    } else {
        ok = save(d, &errors);   // stop (logging out, shutting down) and the timer's save
    }
    for (const QString &e : std::as_const(errors))
        std::fprintf(stderr, "kes: %s\n", qPrintable(e));
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------- systemd user units

bool have_user_systemd()
{
    static int have = -1;
    if (have < 0)
        have = which("systemctl") && proc::run({"systemctl", "--user", "show-environment"}, 5000).rc == 0;
    return have == 1;
}

QString unit_dir()
{
    return join(QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME")).isEmpty() ? join(HOME(), ".config")
                                                                              : QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME")),
                "systemd/user");
}

bool needs_user_units(const Disk &d) { return d.life == PERSISTENT || (d.life == PERMANENT && d.kind == FOLDER); }

QStringList unit_names(const Disk &d)
{
    QString base = "kestrel-ramdisk-" + d.id;
    if (!needs_user_units(d))
        return {};
    if (d.life != PERSISTENT || d.interval <= 0)
        return {base + ".service"};
    return {base + ".service", base + "-save.service", base + ".timer"};
}

static QString exec_arg(const QString &s)   // one argument of an Exec line: quoted, and nothing in it expanded
{
    QString o = s;
    o.replace('\\', "\\\\").replace('"', "\\\"").replace('%', "%%").replace('$', "$$");
    return '"' + o + '"';
}

static QString unit_text(const QString &s)   // a value in a unit file (a Description): no specifier expanded
{
    QString o = s;
    o.replace('%', "%%").replace('\n', ' ');
    return o;
}

QString service_text(const Disk &d, const QString &kes)
{
    return QString("# Made by Kestrel Explorer for its RAM disk \"%1\"; removed with it (Remove RAM Disk)\n"
                   "[Unit]\n"
                   "Description=Kestrel Explorer RAM disk %1\n"
                   "\n"
                   "[Service]\n"
                   "Type=oneshot\n"
                   "RemainAfterExit=yes\n"
                   "ExecStart=%2 --ramdisk-start %3\n"
                   "ExecStop=%2 --ramdisk-stop %3\n"
                   "TimeoutStartSec=30min\n"
                   "TimeoutStopSec=10min\n"
                   "\n"
                   "[Install]\n"
                   "WantedBy=default.target\n")
        .arg(unit_text(d.name), exec_arg(kes), d.id);
}

QString save_service_text(const Disk &d, const QString &kes)
{
    return QString("# Made by Kestrel Explorer for its RAM disk \"%1\"; removed with it (Remove RAM Disk)\n"
                   "[Unit]\n"
                   "Description=Save Kestrel Explorer RAM disk %1\n"
                   "\n"
                   "[Service]\n"
                   "Type=oneshot\n"
                   "ExecStart=%2 --ramdisk-save %3\n")
        .arg(unit_text(d.name), exec_arg(kes), d.id);
}

QString timer_text(const Disk &d)
{
    return QString("# Made by Kestrel Explorer for its RAM disk \"%1\"; removed with it (Remove RAM Disk)\n"
                   "[Unit]\n"
                   "Description=Save Kestrel Explorer RAM disk %1 every %2 minutes\n"
                   "\n"
                   "[Timer]\n"
                   "OnActiveSec=%2min\n"
                   "OnUnitActiveSec=%2min\n"
                   "Unit=kestrel-ramdisk-%3-save.service\n"
                   "\n"
                   "[Install]\n"
                   "WantedBy=timers.target\n")
        .arg(unit_text(d.name))
        .arg(d.interval)
        .arg(d.id);
}

static void systemctl(const QStringList &args, QStringList *errors)
{
    proc::Result r = proc::run(QStringList{"systemctl", "--user"} + args, 60000);
    if (r.rc != 0)
        *errors << tx("systemctl %1 failed: %2").arg(args.join(' '), QString::fromLocal8Bit(r.err).trimmed());
}

void install_units(const Disk &d, QStringList *errors)
{
    QStringList names = unit_names(d);
    if (names.isEmpty())
        return;
    QString kes = QCoreApplication::applicationFilePath();
    try {
        makedirs(unit_dir(), true);
        write_atomic(join(unit_dir(), names[0]), service_text(d, kes).toUtf8());
        if (names.size() > 1) {
            write_atomic(join(unit_dir(), names[1]), save_service_text(d, kes).toUtf8());
            write_atomic(join(unit_dir(), names[2]), timer_text(d).toUtf8());
        }
    } catch (const OSError &e) {
        *errors << e.message();
        return;
    }
    systemctl({"daemon-reload"}, errors);
    systemctl({"enable", "--now", names[0]}, errors);
    if (names.size() > 1)
        systemctl({"enable", "--now", names[2]}, errors);
}

void remove_units(const Disk &d, QStringList *errors)
{
    QString base = "kestrel-ramdisk-" + d.id;
    QStringList all{base + ".timer", base + ".service", base + "-save.service"};
    QStringList there;
    for (const QString &n : all)
        if (exists(join(unit_dir(), n)))
            there << n;
    if (there.isEmpty())
        return;
    // stopping the service runs its ExecStop: a last save (the caller has saved just before: quick)
    systemctl(QStringList{"disable", "--now"} + there, errors);
    for (const QString &n : there)
        ::unlink(enc(join(unit_dir(), n)).constData());
    systemctl({"daemon-reload"}, errors);
}

// ---------------------------------------------------------------- bringing them up and down

static bool needs_mount_unit(const Disk &d) { return d.kind == MOUNT && (d.life == PERMANENT || d.life == PERSISTENT); }

// in a job: mount (or make) it, as `d` says. MOUNT ones through the admin session.
static void make_it(Task *task, const Disk &d)
{
    if (d.kind == FOLDER) {
        if (shm_root().isEmpty())
            throw Error(tx("RAM disks without a size limit need a folder of Kestrel's own in /dev/shm, and it can't be made."));
        if (!isdir(d.path))
            util::mkdir(d.path, 0700);
        return;
    }
    if (!isdir(d.path))
        makedirs(d.path, false, 0700);
    if (needs_mount_unit(d))
        admin::session().call(task, "tmpfs_unit", {{"path", d.path}, {"size_mb", d.size_mb}, {"enable", true}});
    else if (!is_tmpfs_mount(d.path))
        admin::session().call(task, "tmpfs_mount", {{"path", d.path}, {"size_mb", d.size_mb}});
}

static void log_errors(Task *task, const QStringList &errors)
{
    for (const QString &e : errors)
        task->log(tx("Failed: %1").arg(e));
    if (!errors.isEmpty())
        task->had_errors = true;
}

static std::function<bool()> cancel_of(Task *task)
{
    return [task]() {
        task->check();
        return false;
    };
}

void create(QWidget *parent, const Disk &d, std::function<void(bool)> done)
{
    QPointer<QWidget> p(parent);
    QStringList lanes{d.path};
    if (d.life == PERSISTENT)
        lanes << d.save_to;
    Task *t = fileops::queue_job(
        parent, tx("Making the RAM disk “%1”").arg(d.name), lanes,
        [d](Task *task) -> QVariant {
            make_it(task, d);
            QStringList errors;
            if (d.life == PERSISTENT) {
                task->log(tx("Loading what's saved in %1").arg(d.save_to));
                load(d, &errors, cancel_of(task));
            }
            if (needs_user_units(d))
                install_units(d, &errors);
            log_errors(task, errors);
            return QVariantMap{{"errors", errors}};
        },
        [p, d, done](const QVariant &res) {
            bool ok = res.isValid() && online(d);
            if (ok) {
                QList<Disk> list = disks();
                bool found = false;
                for (Disk &x : list)
                    if (x.id == d.id)
                        x = d, found = true;
                if (!found)
                    list << d;
                save_disks(list);
            }
            QStringList errors = res.toMap().value("errors").toStringList();
            if (!errors.isEmpty())
                warn(p, tx("RAM Disk"), errors.mid(0, 20).join('\n'));
            if (done)
                done(ok);
        },
        true, {}, false, {}, true);
    t->admin = d.kind == MOUNT;
}

void prune()
{
    QList<Disk> list = disks(), keep;
    for (const Disk &d : list)
        if ((d.life != EXIT && d.life != SESSION) || online(d))
            keep << d;
    if (keep.size() != list.size()) {
        try {
            save_disks(keep);
        } catch (const OSError &) {
        }
    }
}

void bring_up(QWidget *parent, const QString &id)
{
    const QList<Disk> list = disks();
    if (const Disk *d = find(list, id))
        create(parent, *d);
}

void save_now(QWidget *parent, const QString &id)
{
    const QList<Disk> list = disks();
    const Disk *found = find(list, id);
    if (!found)
        return;
    Disk d = *found;
    fileops::queue_job(parent, tx("Saving the RAM disk “%1” to disk").arg(d.name), {d.path, d.save_to},
                       [d](Task *task) -> QVariant {
                           QStringList errors;
                           save(d, &errors, cancel_of(task));
                           log_errors(task, errors);
                           if (!errors.isEmpty())
                               throw Error(errors.mid(0, 20).join('\n'));
                           return true;
                       });
}

// in a job: take it down (saved first, if it's persistent)
static void take_down(Task *task, const Disk &d, QStringList *errors)
{
    if (d.life == PERSISTENT && online(d) && loaded(d.path, d.id)) {
        task->log(tx("Saving to %1").arg(d.save_to));
        save(d, errors, cancel_of(task));
        if (!errors->isEmpty())
            throw Error(tx("It couldn't all be saved, so it's kept:\n%1").arg(errors->mid(0, 20).join('\n')));
    }
    remove_units(d, errors);
    if (d.kind == MOUNT) {
        if (needs_mount_unit(d))
            admin::session().call(task, "tmpfs_unit", {{"path", d.path}, {"enable", false}});
        if (is_tmpfs_mount(d.path))
            admin::session().call(task, "tmpfs_unmount", {{"path", d.path}});
    } else {
        rmtree(d.path);
        if (lexists(d.path))
            *errors << tx("Not all of %1 could be deleted.").arg(d.path);
    }
}

void remove(QWidget *parent, const QString &id)
{
    const QList<Disk> list = disks();
    const Disk *found = find(list, id);
    if (!found)
        return;
    Disk d = *found;
    QString text = d.life == PERSISTENT
                       ? tx("Remove the RAM disk “%1”? Its files are saved to %2 one last time, and that copy is kept; "
                            "they're no longer kept in memory.").arg(d.name, d.save_to)
                       : tx("Remove the RAM disk “%1”? The files in it are deleted.").arg(d.name);
    auto *box = new QMessageBox(QMessageBox::Warning, tx("Remove RAM Disk"), text, QMessageBox::Cancel, parent);
    QPushButton *go = box->addButton(tx("Remove"), QMessageBox::DestructiveRole);
    box->setDefaultButton(QMessageBox::Cancel);
    QPointer<QWidget> p(parent);
    ask_box(box, [p, go, d](QAbstractButton *b) {
        if (b != go)
            return;
        Task *t = fileops::queue_job(
            p, tx("Removing the RAM disk “%1”").arg(d.name), {d.path},
            [d](Task *task) -> QVariant {
                QStringList errors;
                take_down(task, d, &errors);
                log_errors(task, errors);
                return QVariantMap{{"errors", errors}};
            },
            [p, d](const QVariant &res) {
                if (!res.isValid() || online(d))
                    return;   // (its error has been shown)
                QList<Disk> list = disks();
                list.removeIf([&d](const Disk &x) { return x.id == d.id; });
                save_disks(list);
                QStringList errors = res.toMap().value("errors").toStringList();
                if (!errors.isEmpty())
                    warn(p, tx("Remove RAM Disk"), errors.mid(0, 20).join('\n'));
            },
            true, {}, false, {}, true);
        t->admin = d.kind == MOUNT;
    });
}

void change(QWidget *parent, const Disk &before, const Disk &after_in)
{
    Disk after = after_in;
    if (after.kind == FOLDER)
        after.path = folder_path(after.name);
    QPointer<QWidget> p(parent);
    QStringList lanes{before.path};
    if (!after.save_to.isEmpty())
        lanes << after.save_to;
    Task *t = fileops::queue_job(
        parent, tx("Changing the RAM disk “%1”").arg(before.name), lanes,
        [before, after](Task *task) -> QVariant {
            QStringList errors;
            // the last save to where it went, before its units go
            if (before.life == PERSISTENT && online(before) && loaded(before.path, before.id))
                save(before, &errors, cancel_of(task));
            if (!errors.isEmpty())
                throw Error(errors.mid(0, 20).join('\n'));
            remove_units(before, &errors);
            if (before.kind == FOLDER && before.path != after.path && isdir(before.path))
                util::rename(before.path, after.path);
            if (after.kind == MOUNT) {
                if (needs_mount_unit(before) && !needs_mount_unit(after))
                    admin::session().call(task, "tmpfs_unit", {{"path", after.path}, {"enable", false}});
                if (needs_mount_unit(after) && (!needs_mount_unit(before) || before.size_mb != after.size_mb))
                    admin::session().call(task, "tmpfs_unit", {{"path", after.path}, {"size_mb", after.size_mb}, {"enable", true}});
                if (before.size_mb != after.size_mb && is_tmpfs_mount(after.path))
                    admin::session().call(task, "tmpfs_mount", {{"path", after.path}, {"size_mb", after.size_mb}, {"remount", true}});
            }
            if (after.life == PERSISTENT && (before.life != PERSISTENT || before.save_to != after.save_to)) {
                // what's in it now is what's kept: what's saved there already is added to it (nothing deleted)
                set_mark(after.path, after.id, true);
                if (isdir(after.save_to))
                    mirror(after.save_to, after.path, false, &errors, cancel_of(task));
                else
                    makedirs(after.save_to, true, 0700);
                set_mark(after.path, after.id, !errors.isEmpty());
            }
            if (needs_user_units(after))
                install_units(after, &errors);
            log_errors(task, errors);
            return QVariantMap{{"errors", errors}};
        },
        [p, after](const QVariant &res) {
            if (!res.isValid())
                return;
            QList<Disk> list = disks();
            for (Disk &x : list)
                if (x.id == after.id)
                    x = after;
            save_disks(list);
            QStringList errors = res.toMap().value("errors").toStringList();
            if (!errors.isEmpty())
                warn(p, tx("RAM Disk"), errors.mid(0, 20).join('\n'));
        },
        true, {}, false, {}, true);
    t->admin = before.kind == MOUNT;
}

// ---------------------------------------------------------------- private mode

bool private_mode() { return prefs::get("private_mode").toBool(); }

QString private_dir()
{
    QString root = shm_root();
    return root.isEmpty() ? QString() : join(root, ".private");
}

QString private_files()
{
    QString p = private_dir();
    return p.isEmpty() ? QString() : join(p, "Private");
}

// the private RAM disk's folders, with the histories copied in the first time (so they're there, and what's added
// stays in memory)
static QString make_private()
{
    QString dir = private_dir();
    if (dir.isEmpty())
        return QString();
    try {
        for (const QString &sub : {QString(), QString("Private"), QString("cache"), QString("thumbnails"), QString("history")})
            makedirs(sub.isEmpty() ? dir : join(dir, sub), true, 0700);
        QString history = join(dir, "history"), seeded = join(history, ".seeded");
        if (!exists(seeded)) {
            for (const char *f : {"frequent.json", "sync-pairs.json", "sync-history.json"}) {
                QString from = join(CONFIG_DIR(), f);
                if (isfile(from))
                    write_atomic(join(history, f), read_file(from));
            }
            QSettings ini(join(history, "history.ini"), QSettings::IniFormat);
            for (const char *key : {"recent_servers", "viewer_recent_folders"})
                if (settings().contains(key))
                    ini.setValue(key, settings().value(key));
            ini.sync();
            write_text(seeded, QByteArray());
        }
    } catch (const OSError &) {
        return QString();
    }
    return dir;
}

void apply_private() { set_private_dir(private_mode() ? make_private() : QString()); }

static QStringList wipe_targets(bool leaving_private_only)
{
    QStringList out;
    if (private_mode() && !private_files().isEmpty() && !is_empty(private_files()))
        out << private_files();
    if (!leaving_private_only)
        for (const Disk &d : disks())
            if (d.life == EXIT && online(d) && !is_empty(d.path))
                out << d.path;
    return out;
}

void last_kestrel_gone()
{
    QString priv = private_dir();
    if (!priv.isEmpty() && lexists(priv))
        rmtree(priv);
    QList<Disk> list = disks(), keep;
    bool changed = false;
    for (const Disk &d : list) {
        if (d.life != EXIT) {
            keep << d;
            continue;
        }
        if (d.kind == FOLDER) {
            rmtree(d.path);
            changed = true;
        } else {
            // unmounting needs the password: its files go, freeing the memory, and the empty mount stays until
            // it's removed or the computer restarts
            if (is_tmpfs_mount(d.path)) {
                try {
                    for (const QString &n : listdir(d.path))
                        rmtree(join(d.path, n));
                } catch (const OSError &) {
                }
            }
            keep << d;
        }
    }
    if (changed)
        save_disks(keep);
}

void set_private(QWidget *parent, bool on)
{
    if (on) {
        if (private_dir().isEmpty()) {
            warn(parent, tx("Private Mode"), tx("Private mode needs a folder of Kestrel's own in /dev/shm, and it can't be made."));
            return;
        }
        prefs::set("private_mode", true);
        return;
    }
    auto go = []() {
        prefs::set("private_mode", false);
        // the other Kestrels have stopped using it by now
        QTimer::singleShot(2000, qApp, []() {
            if (!private_mode() && !private_dir().isEmpty())
                rmtree(private_dir());
        });
    };
    if (wipe_needs_warning(true))
        warn_wipe(parent, true, go);
    else
        go();
}

bool wipe_needs_warning(bool leaving_private_only)
{
    return prefs::get("ramdisk_wipe_warning").toBool() && !wipe_targets(leaving_private_only).isEmpty();
}

QMessageBox *warn_wipe(QWidget *parent, bool leaving_private_only, std::function<void()> go, std::function<void()> cancel)
{
    QStringList where;
    for (const QString &t : wipe_targets(leaving_private_only))
        where << "• " + (t == private_files() ? tx("Private (private mode's folder)") : t);
    QString text = leaving_private_only
                       ? tx("Leaving private mode deletes what's in its RAM disk, also the files saved in Private:\n\n%1")
                             .arg(where.join('\n'))
                       : tx("Closing Kestrel deletes the files in these RAM disks:\n\n%1").arg(where.join('\n'));
    auto *box = new QMessageBox(QMessageBox::Warning, leaving_private_only ? tx("Leave Private Mode") : tx("Close Kestrel"),
                                text, QMessageBox::Cancel, parent);
    QPushButton *ok = box->addButton(leaving_private_only ? tx("Delete and Leave Private Mode") : tx("Delete and Close"),
                                     QMessageBox::DestructiveRole);
    box->setDefaultButton(ok);
    auto *again = new QCheckBox(tx("Don't show this again (Preferences → Privacy)"));
    box->setCheckBox(again);
    ask_box(box, [ok, again, go, cancel](QAbstractButton *b) {
        if (b != ok) {
            if (cancel)
                cancel();
            return;
        }
        if (again->isChecked())
            prefs::set("ramdisk_wipe_warning", false);
        go();
    });
    return box;
}

}  // namespace ramdisk
