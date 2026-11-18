// The archive browser's RAM cache (archivebrowser.h): the members it extracts (to open them, show them in the viewer,
// make their thumbnails, drag or copy them out) are kept in memory, never on disk.
//
// It's a folder of this Kestrel's on a tmpfs it can use without root: <ramdisk::shm_root()>/archive-cache/<pid>-<n>
// (/dev/shm/kestrel-explorer-<uid>, mode 0700, or $XDG_RUNTIME_DIR without /dev/shm), with a size limit of its own
// (Preferences → Archives: a share of the RAM, never more than the tmpfs has free). Each member's copy is in a folder
// of its own (<n>/<its name>), so it's handed over under its own name; it's extracted into a hidden part folder beside
// them first and renamed into place only once whole, so a failure or cancel leaves nothing under a member's name.
//
// When it's full it works first in, first out: the oldest copies are deleted to make room for what's wanted now. A copy
// still in use is never deleted: pinned (being extracted, shown in the viewer, being dragged, on the clipboard), handed
// over in the last `grace_ms` (an app started on it may not have opened it yet), open in any program (its /proc/<pid>/fd),
// or among the paths of a job of this Kestrel (a copy out of it). If everything left is in use, reserve() says FULL
// and nothing is deleted. A member bigger than the whole cache isn't cached (TOO_BIG).
//
// The folder is deleted when this Kestrel closes, an archive's copies when its window closes (those in use once
// they're free), and folders left by a Kestrel that crashed at the next start-up (sweep(): a folder's .lock file is
// held, with flock, while its Kestrel runs).
#pragma once

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>

class ArchiveCache : public QObject {
    Q_OBJECT
public:
    static ArchiveCache *instance();   // this Kestrel's (its folder made when first needed)
    ~ArchiveCache() override;
    static QString base();   // the folder every Kestrel's cache folder is in; empty if there's no tmpfs to use
    static bool contains(const QString &path);   // path is in a Kestrel's RAM cache (no thumbnails of it on disk)
    static void sweep();     // delete the folders whose Kestrel isn't running any more (start-up)

    QString root();          // this Kestrel's folder, made on first use; empty if it can't be made
    qint64 limit() const;    // bytes it may hold: the setting's share of the RAM, at most what its tmpfs can still take
    qint64 used() const;     // its copies' bytes, and the room reserved for extractions under way
    void set_limit(qint64 bytes) { forced_limit = bytes; }   // (tests; -1: the setting's)

    static QString key(const QString &archive, const QString &member);
    QString find(const QString &key) const;   // its copy's path, or empty

    // Room for `bytes` more, reserved until release(): the oldest copies not in use are deleted until it fits.
    enum Room { OK, FULL, TOO_BIG, NO_CACHE };
    Room reserve(qint64 bytes);
    void release(qint64 bytes);
    static QString room_message(Room r);   // what to tell the user (for FULL, TOO_BIG, NO_CACHE)

    // A hidden folder in root() to extract into (throws OSError); what's whole in it is then taken in with add()
    QString new_part();
    // made (in a part folder, whole) moved to a folder of its own in root(), under its name: returns the copy's path.
    // A copy of `key` already there is kept and made is deleted (two extractions of one member at once).
    QString add(const QString &key, const QString &made, qint64 size);

    void pin(const QString &path);
    // handed: it was just given to another program or a job (counts as in use for grace_ms more)
    void unpin(const QString &path, bool handed = true);
    void handed_over(const QString &path);
    bool in_use(const QString &path);
    // Pinned while the clipboard holds them (the mime data put there must carry clipboard_mark(), from the same call),
    // unpinned when something else is copied; the copies held before are let go
    QByteArray hold_on_clipboard(const QStringList &paths);
    static constexpr const char *CLIP_MIME = "application/x-kestrel-archive-cache";
    // Pinned until `holder` is destroyed (the image viewer showing them)
    void hold_while(QObject *holder, const QStringList &paths);
    // An archive's copies deleted (its window closed); those in use once they're free
    void forget(const QString &archive);
    QStringList paths() const;   // the copies, oldest first

    qint64 grace_ms = 60000;
    // The cache's files other programs have open; tests replace it
    std::function<QSet<QString>()> open_files;

private:
    ArchiveCache();
    struct Item {
        QString key, path;
        qint64 size = 0;
        qint64 handed = 0;   // ms since the epoch
        int pins = 0;
        bool orphan = false;   // its window has closed: deleted once it's free
    };
    Item *item_for(const QString &path);
    bool busy(const Item &it, const QSet<QString> &open) const;
    void remove_at(int i);
    void sweep_orphans();
    QSet<QString> scan_open() const;

    void clipboard_changed();
    QList<Item> items;   // oldest first
    QStringList clip_paths;
    QByteArray clip_token;
    qint64 reserved = 0;
    qint64 forced_limit = -1;
    QString dir;
    int lock_fd = -1;
    int next = 0;
    QTimer orphan_timer;
};
