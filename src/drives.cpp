#include "drives.h"

#include "atc.h"
#include "fileops.h"
#include "jobs.h"
#include "util.h"
#include "windows.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QMutex>
#include <QPointer>
#include <QSettings>
#include <QThread>

#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>

#include <algorithm>
#include <atomic>
#include <cerrno>

using namespace util;

namespace drives {

// ---------------------------------------------------------------- the mount list

// a field of mountinfo, its \040-style escapes (space, tab, newline, backslash) undone
static QString unescape(const QByteArray &f)
{
    QByteArray out;
    for (int i = 0; i < f.size(); ++i) {
        if (f[i] == '\\' && i + 3 < f.size() && f[i + 1] >= '0' && f[i + 1] <= '3' && f[i + 2] >= '0' &&
            f[i + 2] <= '7' && f[i + 3] >= '0' && f[i + 3] <= '7') {
            out += char((f[i + 1] - '0') * 64 + (f[i + 2] - '0') * 8 + (f[i + 3] - '0'));
            i += 3;
        } else {
            out += f[i];
        }
    }
    return QString::fromUtf8(out);
}

// Tests mark folders as slow drives: KESTREL_TEST_SLOW="tape=/path;floppy=/other" (each also listed as a mount)
static QList<QPair<Slow, QString>> test_slow()
{
    QList<QPair<Slow, QString>> out;
    for (const QString &part : qEnvironmentVariable("KESTREL_TEST_SLOW").split(';', Qt::SkipEmptyParts)) {
        QString kind = part.section('=', 0, 0), path = part.section('=', 1);
        if (!path.isEmpty())
            out << qMakePair(kind == "floppy" ? Slow::Floppy : Slow::Tape, path);
    }
    return out;
}

static Slow kind_of(const Mount &mt);
static std::atomic<bool> slow_mounted{false};   // a tape or floppy is in the mount list (kept with it, below)
static std::atomic<bool> mounts_stale{false};   // the kernel said the mount table changed (MountWatch)

void reread_mounts() { mounts_stale = true; }

QList<Mount> mounts()
{
    static QMutex m;
    static QList<Mount> cached;
    static qint64 read_ms = -10000;
    QMutexLocker l(&m);
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - read_ms < 1000 && !mounts_stale.exchange(false))
        return cached;
    read_ms = now;
    cached.clear();
    bool ok = false;
    QByteArray data = read_file("/proc/self/mountinfo", &ok);
    for (const QByteArray &line : data.split('\n')) {
        int dash = line.indexOf(" - ");
        if (dash < 0)
            continue;
        QList<QByteArray> before = line.left(dash).split(' '), after = line.mid(dash + 3).split(' ');
        if (before.size() < 5 || after.size() < 2)
            continue;
        cached << Mount{unescape(before[4]), unescape(after[1]), unescape(after[0])};
    }
    for (const auto &[kind, path] : test_slow())
        cached << Mount{path, kind == Slow::Floppy ? "/dev/test-floppy" : "ltfs:test", kind == Slow::Floppy ? "vfat" : "fuse"};
    // Tests add mounts of their own: KESTREL_TEST_MOUNTS="/path|fstype|source;…"
    for (const QString &part : qEnvironmentVariable("KESTREL_TEST_MOUNTS").split(';', Qt::SkipEmptyParts)) {
        QStringList f = part.split('|');
        if (f.size() == 3)
            cached << Mount{f[0], f[2], f[1]};
    }
    slow_mounted = std::any_of(cached.begin(), cached.end(), [](const Mount &mt) { return kind_of(mt) != Slow::None; });
    return cached;
}

static QMutex labels_lock;
static QHash<QString, QString> labels;   // device -> its label, until the mount table changes (MountWatch)

static QString read_label(const QString &device);

QString device_label(const QString &device)
{
    if (!device.startsWith("/dev/"))
        return QString();
    {
        QMutexLocker l(&labels_lock);
        auto it = labels.constFind(device);
        if (it != labels.constEnd())
            return *it;
    }
    QString label = read_label(device);
    QMutexLocker l(&labels_lock);
    if (!label.isEmpty())   // none yet may be udev not having made the by-label link of a drive just mounted
        labels.insert(device, label);
    return label;
}

qint64 device_bytes(const QString &device)
{
    // the kernel's own figure (what lsblk shows): /sys, no I/O to the drive
    if (!device.startsWith("/dev/"))
        return -1;
    bool ok = false;
    qint64 sectors = read_file("/sys/class/block/" + basename(realpath(device)) + "/size", &ok).trimmed().toLongLong();
    return ok && sectors > 0 ? sectors * 512 : -1;
}

static QString read_label(const QString &device)
{
    QString real = realpath(device);
    QDir byl("/dev/disk/by-label");
    for (const QString &n : byl.entryList(QDir::System | QDir::Files | QDir::NoDotAndDotDot)) {
        if (realpath(byl.filePath(n)) != real)
            continue;
        QByteArray raw = n.toUtf8(), out;   // udev escapes characters as \xNN (a space is \x20)
        for (int i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 3 < raw.size() && raw[i + 1] == 'x') {
                bool ok = false;
                int c = raw.mid(i + 2, 2).toInt(&ok, 16);
                if (ok) {
                    out += char(c);
                    i += 3;
                    continue;
                }
            }
            out += raw[i];
        }
        return QString::fromUtf8(out);
    }
    return QString();
}

static const Mount *mount_of(const QList<Mount> &all, const QString &path)
{
    const Mount *best = nullptr;
    for (const Mount &mt : all) {   // called for every item of a job: no string built per mount
        qsizetype n = mt.root.size();
        bool under = mt.root == "/" || (path.startsWith(mt.root) && (path.size() == n || path[n] == '/'));
        if (under && (!best || mt.root.size() >= best->root.size()))
            best = &mt;
    }
    return best;
}

bool network_fs(const QString &fstype)
{
    static const QSet<QString> NET = {"cifs", "smb3", "smbfs", "nfs", "nfs4", "fuse.sshfs", "sshfs", "davfs",
                                      "fuse.rclone", "9p", "fuse.davfs2", "afs", "ceph", "glusterfs"};
    return NET.contains(fstype);
}

bool user_mount(const Mount &mt)
{
    QByteArray root = mt.root.toUtf8(), fs = mt.fstype.toUtf8(), dev = mt.source.toUtf8();
    if (g_unix_is_mount_path_system_internal(root.constData()) || g_unix_is_system_fs_type(fs.constData()) ||
        g_unix_is_system_device_path(dev.constData()))
        return false;
    if (mt.root.contains("/."))   // inside a hidden folder: hidden on purpose
        return false;
    if (mt.root.startsWith("/media/") || mt.root.startsWith("/run/media/" + QString::fromUtf8(g_get_user_name()) + "/"))
        return true;
    return mt.root.startsWith(HOME() + "/");
}

QString mount_root(const QString &path)
{
    QList<Mount> all = mounts();
    const Mount *mt = mount_of(all, path);
    return mt ? mt->root : QString("/");
}

QString mount_fstype(const QString &path)
{
    QList<Mount> all = mounts();
    const Mount *mt = mount_of(all, path);
    return mt ? mt->fstype : QString();
}

// the whole disks under a block device: a partition's disk, the disks under a device-mapper or md device (its slaves)
static QStringList disks_of(const QString &name, int depth = 0)
{
    QString sys = "/sys/class/block/" + name;
    QStringList out;
    QStringList slaves = depth < 8 ? QDir(sys + "/slaves").entryList(QDir::Dirs | QDir::NoDotAndDotDot) : QStringList();
    for (const QString &s : slaves)
        for (const QString &d : disks_of(s, depth + 1))
            if (!out.contains(d))
                out << d;
    if (!out.isEmpty())
        return out;
    if (exists(sys + "/partition"))
        return {basename(dirname(realpath(sys)))};   // the link first: abspath() would drop "/.." before it
    return {name};
}

// the ids of a mount's drives (not a GVFS mount's: those depend on the path)
static QStringList mount_ids(const Mount *mt)
{
    if (mt->source.startsWith("/dev/")) {
        QStringList out;
        for (const QString &d : disks_of(basename(realpath(mt->source))))
            out << "disk:" + d;
        return out;
    }
    if (mt->fstype == "zfs")
        return {"zfs:" + mt->source.section('/', 0, 0)};
    return {"mount:" + mt->fstype + ":" + mt->source};
}

QStringList drive_ids(const QStringList &paths)
{
    QList<Mount> all = mounts();
    QHash<const Mount *, QStringList> known;   // a mount's ids, looked up once (a job may name thousands of items on it)
    QStringList out;
    for (const QString &path : paths) {
        const Mount *mt = path.startsWith('/') ? mount_of(all, path) : nullptr;
        if (!mt)
            continue;
        QStringList ids;
        if (mt->fstype == "fuse.gvfsd-fuse") {   // one FUSE mount for every GVFS share and phone: each is its own drive
            QString first = path.mid(mt->root.size() + 1).section('/', 0, 0);
            ids = {first.isEmpty() ? QString("gvfs") : "gvfs:" + first};
        } else {
            auto it = known.constFind(mt);
            ids = it != known.constEnd() ? *it : *known.insert(mt, mount_ids(mt));
        }
        for (const QString &id : ids)
            if (!out.contains(id))
                out << id;
    }
    return out;
}

QStringList drive_ids(const QString &path) { return drive_ids(QStringList{path}); }

// a floppy: the old /dev/fd*, or a removable disk (a USB floppy drive is an sd device) holding at most 4 MB (a 2.88 MB
// disk, with room). Not loop, RAM or device-mapper devices, which can be as small (snap's squashfs images are)
static bool is_floppy_device(const QString &source)
{
    if (source.startsWith("/dev/fd"))
        return true;
    if (!source.startsWith("/dev/"))
        return false;
    static QMutex m;
    static QHash<QString, bool> known;
    QMutexLocker l(&m);
    auto it = known.constFind(source);
    if (it != known.constEnd())
        return *it;
    // /sys: answered by the kernel, not the drive
    QString name = basename(realpath(source)), sys = "/sys/class/block/" + name;
    bool floppy = false;
    if (name.startsWith("sd") || name.startsWith("fd")) {
        bool ok = false;
        qint64 sectors = read_file(sys + "/size", &ok).trimmed().toLongLong();
        QString disk = exists(sys + "/partition") ? dirname(realpath(sys)) : sys;   // a partition: its disk says
        bool removable = read_file(disk + "/removable").trimmed() == "1";
        floppy = ok && removable && sectors > 0 && sectors * 512 <= (qint64(4) << 20);
    }
    known.insert(source, floppy);
    return floppy;
}

static Slow kind_of(const Mount &mt)
{
    for (const auto &[kind, path] : test_slow())
        if (mt.root == path)
            return kind;
    if (mt.source.startsWith("ltfs") || mt.fstype.contains("ltfs"))
        return Slow::Tape;
    if (is_floppy_device(mt.source))
        return Slow::Floppy;
    return Slow::None;
}

// none mounted (nearly always): asked for every file a job copies or deletes, so answered without looking up the path
static bool any_slow()
{
    mounts();   // re-read if it's old
    return slow_mounted;
}

Slow slow_kind(const QString &path)
{
    if (path.isEmpty() || !path.startsWith('/') || !any_slow())
        return Slow::None;
    QList<Mount> all = mounts();
    const Mount *mt = mount_of(all, path);
    return mt ? kind_of(*mt) : Slow::None;
}

QString slow_root(const QString &path)
{
    if (path.isEmpty() || !path.startsWith('/') || !any_slow())
        return QString();
    QList<Mount> all = mounts();
    const Mount *mt = mount_of(all, path);
    return mt && kind_of(*mt) != Slow::None ? mt->root : QString();
}

QString slow_name(Slow kind) { return kind == Slow::Tape ? "tape" : kind == Slow::Floppy ? "floppy" : "drive"; }

bool previews_allowed(const QString &path)
{
    QString root = slow_root(path);
    return !root.isEmpty() && settings().value("slow_previews").toStringList().contains(root);
}

void set_previews_allowed(const QString &root, bool on)
{
    QStringList l = settings().value("slow_previews").toStringList();
    l.removeAll(root);
    if (on)
        l << root;
    settings().setValue("slow_previews", l);
    for (QWidget *w : QApplication::topLevelWidgets())   // thumbnails asked again (or dropped) as the views repaint
        w->update();
}

bool reads_allowed(const QString &path) { return slow_kind(path) == Slow::None || previews_allowed(path); }

// ---------------------------------------------------------------- asking the drive (workers only)

bool statfs_(const QString &path, qint64 *total, qint64 *free)
{
    // tests: a slow drive's statfs taking this long, as a busy tape's does
    int delay = qEnvironmentVariableIntValue("KESTREL_TEST_STATFS_DELAY_MS");
    if (delay > 0 && slow_kind(path) != Slow::None)
        QThread::msleep(delay);
    struct statvfs sv;
    if (::statvfs(enc(path).constData(), &sv) != 0)
        return false;
    *total = qint64(sv.f_blocks) * qint64(sv.f_frsize);
    *free = qint64(sv.f_bavail) * qint64(sv.f_frsize);
    return true;
}

// LTFS shows its index through virtual extended attributes, "user.ltfs.<name>" on Linux (plain "ltfs.<name>" kept as
// a fallback). Some are answered from the index in memory (volumeName, startblock, partition); the cartridge's
// health (mediaLoads, mediaStorageAlert…) makes LTFS send the drive commands (log pages, and on an HP drive, which
// rejects one of them, eleven drive-dump attempts): see Sizes::get for how rarely those are asked
QString tape_attr(const QString &path, const QString &name)
{
    QByteArray p = enc(path);
    char buf[4096];
    for (const QString &prefix : {QString("user.ltfs."), QString("ltfs.")}) {
        ssize_t n = ::getxattr(p.constData(), (prefix + name).toUtf8().constData(), buf, sizeof(buf) - 1);
        if (n >= 0)
            return QString::fromUtf8(buf, int(n)).trimmed();
    }
    return QString();
}

QPair<qint64, qint64> tape_position(const QString &path)
{
    QString block = tape_attr(path, "startblock");
    if (block.isEmpty())
        return {-1, -1};
    QString part = tape_attr(path, "partition");
    qint64 partition = part.size() == 1 && part[0].isLetter() ? part[0].toLower().unicode() - 'a' : part.toLongLong();
    return {partition, block.toLongLong()};
}

static const char *HOUSEKEEPING = "org.gnome.settings-daemon.plugins.housekeeping";

static GSettings *housekeeping()   // null where the desktop has no such setting
{
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, HOUSEKEEPING, TRUE) : nullptr;
    if (!schema)
        return nullptr;
    bool has = g_settings_schema_has_key(schema, "ignore-paths");
    g_settings_schema_unref(schema);
    return has ? g_settings_new(HOUSEKEEPING) : nullptr;
}

// Adds (on) or removes path in the check's ignore-paths; true if the list changed
static bool set_ignored(const QString &path, bool on)
{
    static QMutex lock;   // jobs on two tapes at once: one read-modify-write at a time
    QMutexLocker l(&lock);
    GSettings *gs = housekeeping();
    if (!gs)
        return false;
    QStringList list;
    gchar **v = g_settings_get_strv(gs, "ignore-paths");
    for (gchar **p = v; p && *p; ++p)
        list << QString::fromUtf8(*p);
    g_strfreev(v);
    bool changed = on ? !list.contains(path) : list.removeAll(path) > 0;
    if (on && changed)
        list << path;
    if (changed) {
        QList<QByteArray> bytes;
        QVector<const gchar *> ptrs;
        for (const QString &s : list)
            bytes << s.toUtf8();
        for (const QByteArray &b : bytes)
            ptrs << b.constData();
        ptrs << nullptr;
        g_settings_set_strv(gs, "ignore-paths", ptrs.constData());
        g_settings_sync();
    }
    g_object_unref(gs);
    return changed;
}

bool quiet_tape(const QString &root) { return !root.isEmpty() && set_ignored(rstrip(root, '/'), true); }

void unquiet_tape(const QString &root)
{
    if (!root.isEmpty())
        set_ignored(rstrip(root, '/'), false);
}

QStringList free_space_pollers()
{
    // their process names (as /proc/<pid>/comm has them, at most 15 characters)
    static const QList<QPair<QString, QString>> KNOWN = {{"gnome-system-mo", "GNOME System Monitor"},
                                                        {"plasma-systemmo", "KDE's System Monitor"},
                                                        {"ksysguard", "KSysGuard"},
                                                        {"conky", "Conky"}};
    QString proc = qEnvironmentVariable("KESTREL_TEST_PROC", "/proc");
    QStringList pids, out;
    try {
        pids = listdir(proc);
    } catch (const OSError &) {
    }
    for (const QString &pid : pids) {
        bool num = false;
        pid.toLongLong(&num);
        if (!num)
            continue;
        QString comm = QString::fromLocal8Bit(read_file(join(join(proc, pid), "comm"))).trimmed();
        for (const auto &[name, shown] : KNOWN)
            if (comm == name && !out.contains(shown))
                out << shown;
    }
    return out;
}

QString ltfs_sync(const QString &root)
{
    QString proc = qEnvironmentVariable("KESTREL_TEST_PROC", "/proc");
    QString mount = rstrip(root, '/');
    QStringList pids;
    try {
        pids = listdir(proc);
    } catch (const OSError &) {
    }
    for (const QString &pid : pids) {
        bool num = false;
        pid.toLongLong(&num);
        if (!num)
            continue;
        QByteArray cmd = read_file(join(join(proc, pid), "cmdline"));   // (readable for root's processes too)
        QList<QByteArray> args = cmd.split('\0');
        while (!args.isEmpty() && args.last().isEmpty())
            args.removeLast();
        if (args.isEmpty() || basename(QString::fromLocal8Bit(args[0])) != "ltfs")
            continue;
        bool ours = false;
        for (const QByteArray &a : args)
            ours = ours || rstrip(dec(a), '/') == mount;
        if (!ours)
            continue;
        QString sync = "time@5";   // LTFS's default
        for (int i = 1; i < args.size(); ++i) {
            QString opts;
            if (args[i] == "-o" && i + 1 < args.size())
                opts = QString::fromLocal8Bit(args[++i]);
            else if (args[i].startsWith("-o"))
                opts = QString::fromLocal8Bit(args[i].mid(2));
            for (const QString &o : opts.split(','))
                if (o.startsWith("sync_type="))
                    sync = o.mid(10);
        }
        return sync;
    }
    return QString();
}

int ltfs_sync_minutes(const QString &sync)
{
    return sync.startsWith("time@") ? std::max(1, sync.mid(5).toInt()) : 0;
}

QString sync_tape(const QString &root)
{
    QByteArray p = enc(root);
    int err = 0;
    for (const char *name : {"user.ltfs.sync", "ltfs.sync"}) {
        if (::setxattr(p.constData(), name, "1", 1, 0) == 0)
            return QString();
        err = errno;
    }
    return errno_text(err, root);
}

void sync_tape_job(QWidget *parent, const QString &root)
{
    QPointer<QWidget> p(parent);
    fileops::run_task(
        parent, "Writing the tape's index", [root]() { return QVariant(sync_tape(root)); },
        [p, root](const QVariant &err) {
            if (!err.toString().isEmpty())
                warn(p, "Write Tape Index", "The tape's index couldn't be written: " + err.toString());
        });
}

// ---------------------------------------------------------------- sizes

Sizes *Sizes::instance()
{
    static Sizes *s = new Sizes;
    return s;
}

// ---------------------------------------------------------------- mount changes

MountWatch *MountWatch::instance()
{
    static MountWatch *w = new MountWatch;
    return w;
}

MountWatch::MountWatch()
{
    // GIO's watch on the kernel's mount table (it waits on /proc/self/mountinfo, which wakes it on every mount and
    // unmount): nothing is polled, and no drive is asked anything
    // The monitor belongs to the thread-default main context of the thread that asks for it, and signals there: the
    // default one (Qt's UI thread runs it), whichever thread makes this first, so a worker's context that's never run
    // again can't swallow its signals.
    moveToThread(QCoreApplication::instance() ? QCoreApplication::instance()->thread() : thread());
    g_main_context_push_thread_default(g_main_context_default());
    GUnixMountMonitor *m = g_unix_mount_monitor_get();
    g_main_context_pop_thread_default(g_main_context_default());
    g_signal_connect(m, "mounts-changed", G_CALLBACK(+[](GUnixMountMonitor *, gpointer self) {
                         static_cast<MountWatch *>(self)->mounts_changed();
                     }),
                     this);
}

void MountWatch::mounts_changed()
{
    mounts_stale = true;   // mounts() reads the table again at once
    {
        QMutexLocker l(&labels_lock);
        labels.clear();   // (a drive mounted again may have been relabelled)
    }
    Q_EMIT changed();
}

Sizes::Sizes()
{
    pool.setMaxThreadCount(4);
    connect(MountWatch::instance(), &MountWatch::changed, this, [this]() {   // drives gone: asked afresh if they come back
        QSet<QString> roots;
        for (const Mount &mt : mounts())
            roots << mt.root;
        for (auto it = cache.begin(); it != cache.end();)
            it = !roots.contains(it.key()) && !it->running ? cache.erase(it) : std::next(it);
    });
    connect(atc::radio(), &atc::Radio::kept_heard, this, [this]() {   // the others' jobs known: tapes held back till now
        QStringList waiting;
        for (auto it = cache.cbegin(); it != cache.cend(); ++it)
            if (it->asked_ms == 0 && slow_kind(it.key()) == Slow::Tape)
                waiting << it.key();
        for (const QString &root : waiting)
            get(root);
    });
}

Sizes::Info Sizes::once(const QString &root)
{
    auto it = cache.constFind(root);
    if (it != cache.constEnd() && it->asked_ms > 0)
        return it->info;
    return get(root);
}

// A tape is asked as little as possible: LTFS answers its size and its cartridge's health with commands to the drive,
// and while it writes they knock it out of streaming. So nothing at all while a job (of any Kestrel) uses it: the last
// answers stand. Otherwise its size every 30 s, and its health (loads, storage alert) once, then every 10 minutes.
// Nor before the tower has told this process the others' jobs (a Kestrel just started, the tower restarting): until
// then a job writing the tape in another Kestrel looks like none. The file chooser never joins the tower, so it never
// asks a tape anything (its dialogs used to send a health query into a tape being written, and every 30 s its size).
static const qint64 TAPE_SIZE_MS = 30000, TAPE_HEALTH_MS = 600000;

Sizes::Info Sizes::get(const QString &root)
{
    Entry &e = cache[root];
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool tape = slow_kind(root) == Slow::Tape;
    if (!e.running && now - e.asked_ms > (tape ? TAPE_SIZE_MS : 5000) &&
        !(tape && (!atc::radio()->caught_up() || jobs::Queue::instance()->busy(root)))) {
        e.running = true;
        e.asked_ms = now;
        if (e.info.total < 0)
            e.info.busy = true;
        bool health = tape && (e.health_ms == 0 || now - e.health_ms > TAPE_HEALTH_MS);
        if (health)
            e.health_ms = now;
        Info before = e.info;
        pool.start([this, root, tape, health, before]() {
            Info info;
            if (!statfs_(root, &info.total, &info.free))
                info.total = info.free = -1;
            if (tape) {
                info.volume_name = tape_attr(root, "volumeName");   // (from the index)
                info.loads = health ? tape_attr(root, "mediaLoads") : before.loads;
                info.alert = health ? tape_attr(root, "mediaStorageAlert") : before.alert;
                info.sync = ltfs_sync(root);   // (from ltfs's command line)
            }
            QMetaObject::invokeMethod(
                this,
                [this, root, info]() {
                    Entry &en = cache[root];
                    en.running = false;
                    en.info = info;
                    Q_EMIT changed(root);
                },
                Qt::QueuedConnection);
        });
    }
    return e.info;
}

void Sizes::forget(const QString &root)
{
    auto it = cache.find(root);
    if (it != cache.end() && !it->running)
        cache.erase(it);
}

}  // namespace drives
