#include "archivecache.h"

#include "fileops.h"
#include "i18n.h"
#include "prefs.h"
#include "ramdisk.h"
#include "util.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMimeData>
#include <QRandomGenerator>
#include <QDateTime>
#include <QPointer>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

using namespace util;

static qint64 now_ms() { return QDateTime::currentMSecsSinceEpoch(); }

// read-only copies (opened with an app) and folders in a copy: made writable, so the whole tree can go
static void remove_tree(const QString &p)
{
    struct stat st;
    if (!lstat_(p, st))
        return;
    if (S_ISDIR(st.st_mode))
        walk(p, [](const QString &root, QStringList &, QStringList &files) {
            ::chmod(enc(root).constData(), 0700);
            for (const QString &f : files)
                ::chmod(enc(join(root, f)).constData(), 0600);
            return true;
        });
    util::rmtree(p);
}

ArchiveCache *ArchiveCache::instance()
{
    static ArchiveCache *c = new ArchiveCache;   // (deleted with the application: its folder goes then)
    return c;
}

ArchiveCache::ArchiveCache() : QObject(QCoreApplication::instance())
{
    open_files = [this]() { return scan_open(); };
    orphan_timer.setInterval(15000);
    connect(&orphan_timer, &QTimer::timeout, this, &ArchiveCache::sweep_orphans);
    if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        connect(app->clipboard(), &QClipboard::dataChanged, this, &ArchiveCache::clipboard_changed);
}

ArchiveCache::~ArchiveCache()
{
    if (!dir.isEmpty())
        remove_tree(dir);
    if (lock_fd >= 0)
        ::close(lock_fd);
}

QString ArchiveCache::base()
{
    QString shm = ramdisk::shm_root();
    return shm.isEmpty() ? QString() : join(shm, "archive-cache");
}

bool ArchiveCache::contains(const QString &path)
{
    // (asked for every thumbnail: the folder's name worked out once)
    static const QString b = []() {
        QString shm = ramdisk::shm_root();
        return shm.isEmpty() ? QString() : join(shm, "archive-cache");
    }();
    return !b.isEmpty() && path.startsWith(b + '/');
}

void ArchiveCache::sweep()
{
    QString b = base();
    if (b.isEmpty() || !isdir(b))
        return;
    QStringList names;
    try {
        names = listdir(b);
    } catch (const OSError &) {
        return;
    }
    for (const QString &n : names) {
        QString d = join(b, n);
        if (d == instance()->dir)
            continue;
        // its Kestrel holds the lock while it runs: one we can take is one whose Kestrel has gone
        int fd = ::open(enc(join(d, ".lock")).constData(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            bool free = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
            ::close(fd);
            if (!free)
                continue;
        }
        remove_tree(d);
    }
}

QString ArchiveCache::root()
{
    if (!dir.isEmpty() && isdir(dir))
        return dir;
    dir.clear();
    QString b = base();
    if (b.isEmpty() || (::mkdir(enc(b).constData(), 0700) != 0 && errno != EEXIST))
        return QString();
    for (int i = 0; i < 100 && dir.isEmpty(); ++i) {
        QString d = join(b, QString("%1-%2").arg(getpid()).arg(i));
        if (::mkdir(enc(d).constData(), 0700) == 0)
            dir = d;
    }
    if (dir.isEmpty())
        return QString();
    lock_fd = ::open(enc(join(dir, ".lock")).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd >= 0)
        ::flock(lock_fd, LOCK_EX | LOCK_NB);
    items.clear();
    reserved = 0;
    return dir;
}

qint64 ArchiveCache::limit() const
{
    if (forced_limit >= 0)
        return forced_limit;
    qint64 ram = qint64(::sysconf(_SC_PHYS_PAGES)) * qint64(::sysconf(_SC_PAGESIZE));
    int pct = std::clamp(prefs::get("archive/ram_cache").toInt(), 1, 90);
    qint64 lim = ram / 100 * pct;
    // never more than the tmpfs can take (logind caps $XDG_RUNTIME_DIR at 10% of the RAM; /dev/shm is half of it,
    // shared with every other program's files there)
    struct statvfs sv;
    if (!dir.isEmpty() && ::statvfs(enc(dir).constData(), &sv) == 0) {
        qint64 room = qint64(sv.f_bavail) * qint64(sv.f_frsize) + used();
        lim = std::min(lim, room);
    }
    return std::max<qint64>(lim, 0);
}

qint64 ArchiveCache::used() const
{
    qint64 n = reserved;
    for (const Item &it : items)
        n += it.size;
    return n;
}

QString ArchiveCache::key(const QString &archive, const QString &member) { return archive + '\n' + member; }

QString ArchiveCache::find(const QString &k) const
{
    for (const Item &it : items)
        if (it.key == k && !it.orphan)
            return it.path;
    return QString();
}

QSet<QString> ArchiveCache::scan_open() const
{
    QSet<QString> out;
    if (dir.isEmpty())
        return out;
    QString prefix = dir + '/';
    DIR *proc = ::opendir("/proc");
    if (!proc)
        return out;
    while (dirent *e = ::readdir(proc)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        QByteArray fds = QByteArray("/proc/") + e->d_name + "/fd";
        DIR *fd_dir = ::opendir(fds.constData());   // (only our own programs': others' aren't readable, nor could open ours)
        if (!fd_dir)
            continue;
        while (dirent *f = ::readdir(fd_dir)) {
            if (f->d_name[0] == '.')
                continue;
            char buf[4096];
            ssize_t n = ::readlink((fds + '/' + f->d_name).constData(), buf, sizeof buf - 1);
            if (n <= 0)
                continue;
            QString target = dec(QByteArray(buf, int(n)));
            if (target.startsWith(prefix))
                out.insert(target);
        }
        ::closedir(fd_dir);
    }
    ::closedir(proc);
    return out;
}

bool ArchiveCache::busy(const Item &it, const QSet<QString> &open) const
{
    if (it.pins > 0 || now_ms() - it.handed < grace_ms)
        return true;
    QString inside = it.path + '/';
    for (const QString &p : open)
        if (p == it.path || p.startsWith(inside))
            return true;
    // a job of this Kestrel reading it (copying it out), running or waiting for its turn
    for (const TaskInfo &t : TaskBoard::instance()->all())
        if (t.local)
            for (const QString &p : t.local->paths)
                if (p == it.path || p.startsWith(inside) || it.path.startsWith(p + '/'))
                    return true;
    return false;
}

bool ArchiveCache::in_use(const QString &path)
{
    Item *it = item_for(path);
    return it && busy(*it, open_files());
}

void ArchiveCache::remove_at(int i)
{
    remove_tree(dirname(items[i].path));   // its own folder
    items.removeAt(i);
}

ArchiveCache::Room ArchiveCache::reserve(qint64 bytes)
{
    if (root().isEmpty())
        return NO_CACHE;
    qint64 lim = limit();
    if (bytes > lim)
        return TOO_BIG;
    if (used() + bytes > lim) {
        QSet<QString> open = open_files();   // (looked at once: it reads every program's open files)
        for (int i = 0; i < items.size() && used() + bytes > lim;)
            if (busy(items[i], open))
                ++i;
            else
                remove_at(i);   // first in, first out
    }
    if (used() + bytes > lim)
        return FULL;
    reserved += bytes;
    return OK;
}

void ArchiveCache::release(qint64 bytes) { reserved = std::max<qint64>(0, reserved - bytes); }

QString ArchiveCache::room_message(Room r)
{
    switch (r) {
    case FULL:
        return tx("The archive's memory cache is full: everything in it is still in use (open in an app, being dragged or "
                  "copied, or on the clipboard). Close what uses it, or give the cache more memory in Preferences → "
                  "Archives.");
    case TOO_BIG:
        return tx("It's bigger than the archive's whole memory cache. Extract it to a folder instead (Extract To…), or "
                  "give the cache more memory in Preferences → Archives.");
    case NO_CACHE:
        return tx("Kestrel has no folder in memory to extract it to (/dev/shm and $XDG_RUNTIME_DIR can't be used). Extract "
                  "it to a folder instead (Extract To…).");
    case OK:
        break;
    }
    return QString();
}

QString ArchiveCache::new_part()
{
    QString r = root();
    if (r.isEmpty())
        throw OSError(ENOENT, room_message(NO_CACHE));
    QString part = join(r, part_name());
    if (::mkdir(enc(part).constData(), 0700) != 0)
        throw_errno(part);
    return part;
}

QString ArchiveCache::add(const QString &k, const QString &made, qint64 size)
{
    QString have = find(k);
    if (!have.isEmpty()) {
        remove_tree(made);
        return have;
    }
    QString r = root();
    QString folder;
    for (;;) {
        folder = join(r, QString::number(++next));
        if (::mkdir(enc(folder).constData(), 0700) == 0)
            break;
        if (errno != EEXIST)
            throw_errno(folder);
    }
    QString to = join(folder, basename(made));
    util::rename(made, to);   // (within one tmpfs: whole, at once)
    Item it;
    it.key = k;
    it.path = to;
    it.size = size;
    items << it;
    return to;
}

ArchiveCache::Item *ArchiveCache::item_for(const QString &path)
{
    for (Item &it : items)
        if (it.path == path)
            return &it;
    return nullptr;
}

void ArchiveCache::pin(const QString &path)
{
    if (Item *it = item_for(path))
        ++it->pins;
}

void ArchiveCache::unpin(const QString &path, bool handed)
{
    if (Item *it = item_for(path)) {
        it->pins = std::max(0, it->pins - 1);
        if (handed)
            it->handed = now_ms();
    }
}

void ArchiveCache::handed_over(const QString &path)
{
    if (Item *it = item_for(path))
        it->handed = now_ms();
}

QByteArray ArchiveCache::hold_on_clipboard(const QStringList &paths)
{
    for (const QString &p : clip_paths)
        unpin(p);
    clip_paths = paths;
    for (const QString &p : clip_paths)
        pin(p);
    clip_token = QByteArray::number(QRandomGenerator::global()->generate64(), 16);
    return clip_token;
}

void ArchiveCache::clipboard_changed()
{
    if (clip_paths.isEmpty())
        return;
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    if (md && md->hasFormat(CLIP_MIME) && md->data(CLIP_MIME) == clip_token)
        return;   // still ours
    for (const QString &p : clip_paths)
        unpin(p);   // (a paste's copy job may still be reading them: busy() sees its paths)
    clip_paths.clear();
    sweep_orphans();
}

void ArchiveCache::hold_while(QObject *holder, const QStringList &paths)
{
    for (const QString &p : paths)
        pin(p);
    connect(holder, &QObject::destroyed, this, [this, paths]() {
        for (const QString &p : paths)
            unpin(p);
        sweep_orphans();
    });
}

void ArchiveCache::forget(const QString &archive)
{
    QString prefix = archive + '\n';
    for (Item &it : items)
        if (it.key.startsWith(prefix))
            it.orphan = true;
    sweep_orphans();
}

void ArchiveCache::sweep_orphans()
{
    bool left = false;
    QSet<QString> open;
    bool looked = false;
    for (int i = 0; i < items.size();) {
        if (!items[i].orphan) {
            ++i;
            continue;
        }
        if (!looked) {
            open = open_files();
            looked = true;
        }
        if (busy(items[i], open)) {
            left = true;
            ++i;
        } else {
            remove_at(i);
        }
    }
    if (left)
        orphan_timer.start();
    else
        orphan_timer.stop();
}

QStringList ArchiveCache::paths() const
{
    QStringList out;
    for (const Item &it : items)
        out << it.path;
    return out;
}
