// Drives that answer slowly: tapes (LTFS) and floppy disks. Reading a file's data, or even asking the drive how big it
// is (statfs), can take seconds to minutes: an LTFS tape winds to the file, and asks the drive for its capacity behind
// whatever it's reading for another program; a USB floppy drive reads the disk and passes the data on slowly. So:
// nothing on the UI thread may wait on such a drive (sizes come from a worker, with the last known value meanwhile), no
// previews, metadata or animations are read from it unless the user allows them for that drive, only one job at a time
// uses it, and a tape is read in the order its files lie on the tape.
//
// The mount list comes from /proc/self/mountinfo, which asks no drive anything (QStorageInfo::mountedVolumes() asks
// every mount for its size: it must not run on the UI thread).
#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThreadPool>

#include <functional>

class QWidget;

namespace drives {

struct Mount {
    QString root, source, fstype;
};
QList<Mount> mounts();   // every mount, from /proc/self/mountinfo (cached for a second); safe on any thread
QString device_label(const QString &device);   // a file system's label (/dev/disk/by-label), without asking the drive
qint64 device_bytes(const QString &device);    // a block device's size from /sys (as lsblk), without asking it; -1: none

// The kernel's mount table changing (a drive mounted or unmounted, by anything): changed() then, and only then. Nothing
// is polled: GIO waits on /proc/self/mountinfo, which the kernel wakes. mounts() and the labels are read afresh after.
class MountWatch : public QObject {
    Q_OBJECT
public:
    static MountWatch *instance();
    void mounts_changed();   // (GIO's signal; also for the tests, standing for a mount or unmount)

Q_SIGNALS:
    void changed();

private:
    MountWatch();
};
QString mount_root(const QString &path);       // the mount point path is on
QString mount_fstype(const QString &path);     // the file system type of the mount path is on ("fuse.sshfs"…)
// A mount file managers show of their own accord, by GIO's rule (g_unix_mount_guess_should_display, which GNOME Files'
// volume monitors follow): not a system file system, device or path, not inside a hidden folder, and under the home
// folder, /media or /run/media/<user>. From the mount table alone: GIO also checks it can enter a /media mount, which
// asks the drive (a busy tape, a dead network share), and it skips bind mounts, which GNOME Files' udisks2 monitor lists.
bool user_mount(const Mount &mt);
bool network_fs(const QString &fstype);   // a network file system (NFS, SMB, sshfs, rclone…)
// The physical drives a path is on, the same in every Kestrel (the job queue's lanes, jobs.h), from the mount table and
// /sys only (no drive is asked): "disk:sda" for a partition of sda (an encrypted or LVM volume: the disks under it),
// "zfs:tank" for a ZFS dataset (its pool), "gvfs:<share>" for each network share or phone, otherwise the mount's
// source ("mount:fuse:ltfs:/dev/sg0" for a tape, "mount:nfs:server:/export"…). Empty if it can't be told.
QStringList drive_ids(const QString &path);
QStringList drive_ids(const QStringList &paths);   // every drive any of them is on, each once

enum class Slow { None, Tape, Floppy };
Slow slow_kind(const QString &path);   // of the drive path is on
QString slow_root(const QString &path);   // the slow drive's mount point path is on, or empty
QString slow_name(Slow kind);             // "tape", "floppy"
// Previews, metadata and animations from this slow drive (off by default; a per-drive choice, by mount point)
bool previews_allowed(const QString &path);
void set_previews_allowed(const QString &root, bool on);
bool reads_allowed(const QString &path);   // not a slow drive, or one the user allowed previews for

// statfs, on a worker only (it can wait on the drive); false if it failed
bool statfs_(const QString &path, qint64 *total, qint64 *free);

// What a tape's LTFS index says (its virtual extended attributes); empty where it has none. On a worker only.
QString tape_attr(const QString &path, const QString &name);
QPair<qint64, qint64> tape_position(const QString &path);   // (partition, start block); (-1, -1) if unknown
// How LTFS writes the index of the tape mounted at root, from its ltfs process's command line (sync_type): "time@5"
// (every 5 minutes, LTFS's default, each time stopping the drive), "unmount", "close"; empty if no ltfs process for it
// is found (KESTREL_TEST_PROC: a folder in /proc's layout, for the tests)
QString ltfs_sync(const QString &root);
int ltfs_sync_minutes(const QString &sync);   // "time@5" -> 5; 0 when the index isn't written on a timer
// Programs that ask every drive for its free space keep a tape from streaming: on LTFS each question is commands to the
// drive, and it drops to a fraction of its speed for a while after each. While a job writes to a tape, GNOME's
// low-disk-space check (gsd-housekeeping, once a minute) is told to skip it: its mount point goes on the check's
// ignore-paths (org.gnome.settings-daemon.plugins.housekeeping), and comes off again after (unquiet_tape) if Kestrel
// put it there. quiet_tape returns false when it was there already, or there's no such setting (not GNOME).
bool quiet_tape(const QString &root);
void unquiet_tape(const QString &root);
// The running programs known to ask every drive for its free space every few seconds ("GNOME System Monitor"…)
// (KESTREL_TEST_PROC: a folder in /proc's layout, for the tests)
QStringList free_space_pollers();
QString sync_tape(const QString &root);   // write the tape's index now (LTFS's sync attribute); an error, or empty
void sync_tape_job(QWidget *parent, const QString &root);   // the same as a status-bar job, a message if it fails

// Sizes of drives without waiting for them: get() answers from what was last read and asks again in the background
// (at most every 5 s per drive); changed(root) says when a new answer came. A tape's volume name and media details
// are read the same way, but a tape is asked far less (its size every 30 s, its health every 10 minutes) and not at
// all while a job uses it: each question is a command to the drive, which interrupts its writing.
class Sizes : public QObject {
    Q_OBJECT
public:
    struct Info {
        qint64 total = -1, free = -1;   // -1: not known (yet)
        QString volume_name, loads, alert, sync;   // tapes only (sync: ltfs_sync)
        bool busy = false;   // asked, no answer yet
    };
    static Sizes *instance();
    Info get(const QString &root);    // never waits
    Info once(const QString &root);   // asked once while it's mounted (a total size doesn't change): never again after
    void forget(const QString &root);

Q_SIGNALS:
    void changed(const QString &root);

private:
    Sizes();
    struct Entry {
        Info info;
        qint64 asked_ms = 0;
        qint64 health_ms = 0;   // a tape's health last asked (see get)
        bool running = false;
    };
    QHash<QString, Entry> cache;
    QThreadPool pool;
};

}  // namespace drives
