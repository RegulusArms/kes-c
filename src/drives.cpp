#include "drives.h"

#include "fileops.h"
#include "util.h"
#include "windows.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QPointer>
#include <QSettings>
#include <QThread>

#include <sys/statvfs.h>
#include <sys/xattr.h>

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

QList<Mount> mounts()
{
    static QMutex m;
    static QList<Mount> cached;
    static qint64 read_ms = -10000;
    QMutexLocker l(&m);
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - read_ms < 1000)
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
    return cached;
}

QString device_label(const QString &device)
{
    if (!device.startsWith("/dev/"))
        return QString();
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
    for (const Mount &mt : all) {
        bool under = path == mt.root || mt.root == "/" || path.startsWith(mt.root + "/");
        if (under && (!best || mt.root.size() >= best->root.size()))
            best = &mt;
    }
    return best;
}

QString mount_root(const QString &path)
{
    QList<Mount> all = mounts();
    const Mount *mt = mount_of(all, path);
    return mt ? mt->root : QString("/");
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

QStringList drive_ids(const QString &path)
{
    if (path.isEmpty() || !path.startsWith('/'))
        return {};
    QList<Mount> all = mounts();
    const Mount *mt = mount_of(all, path);
    if (!mt)
        return {};
    if (mt->fstype == "fuse.gvfsd-fuse") {   // one FUSE mount for every GVFS share and phone: each is its own drive
        QString first = path.mid(mt->root.size() + 1).section('/', 0, 0);
        return {first.isEmpty() ? QString("gvfs") : "gvfs:" + first};
    }
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

Slow slow_kind(const QString &path)
{
    if (path.isEmpty() || !path.startsWith('/'))
        return Slow::None;
    QList<Mount> all = mounts();
    const Mount *mt = mount_of(all, path);
    return mt ? kind_of(*mt) : Slow::None;
}

QString slow_root(const QString &path)
{
    if (path.isEmpty() || !path.startsWith('/'))
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
// a fallback); answered from the index in memory
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

Sizes::Sizes() { pool.setMaxThreadCount(4); }

Sizes::Info Sizes::get(const QString &root)
{
    Entry &e = cache[root];
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!e.running && now - e.asked_ms > 5000) {
        e.running = true;
        e.asked_ms = now;
        if (e.info.total < 0)
            e.info.busy = true;
        bool tape = slow_kind(root) == Slow::Tape;
        pool.start([this, root, tape]() {
            Info info;
            if (!statfs_(root, &info.total, &info.free))
                info.total = info.free = -1;
            if (tape) {
                info.volume_name = tape_attr(root, "volumeName");
                info.loads = tape_attr(root, "mediaLoads");
                info.alert = tape_attr(root, "mediaStorageAlert");
                info.sync = ltfs_sync(root);
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
