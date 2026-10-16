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
#include <QWaitCondition>

#include <functional>

class Task;
class QWidget;

namespace drives {

struct Mount {
    QString root, source, fstype;
};
QList<Mount> mounts();   // every mount, from /proc/self/mountinfo (cached for a second); safe on any thread
QString device_label(const QString &device);   // a file system's label (/dev/disk/by-label), without asking the drive
QString mount_root(const QString &path);       // the mount point path is on

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
QString sync_tape(const QString &root);   // write the tape's index now (LTFS's sync attribute); an error, or empty
void sync_tape_job(QWidget *parent, const QString &root);   // the same as a status-bar job, a message if it fails

// Sizes of drives without waiting for them: get() answers from what was last read and asks again in the background
// (at most every few seconds per drive); changed(root) says when a new answer came. A tape's volume name and media
// details are read the same way.
class Sizes : public QObject {
    Q_OBJECT
public:
    struct Info {
        qint64 total = -1, free = -1;   // -1: not known (yet)
        QString volume_name, loads, alert;   // tapes only
        bool busy = false;   // asked, no answer yet
    };
    static Sizes *instance();
    Info get(const QString &root);   // never waits
    void forget(const QString &root);

Q_SIGNALS:
    void changed(const QString &root);

private:
    Sizes();
    struct Entry {
        Info info;
        qint64 asked_ms = 0;
        bool running = false;
    };
    QHash<QString, Entry> cache;
    QThreadPool pool;
};

// One job at a time per slow drive: a job takes the lanes of the slow drives it reads or writes, waiting (and saying
// so through `waiting`) while another job of this Kestrel holds one. Cancel (task->check()) stops the wait.
class Lanes {
public:
    Lanes(const QStringList &paths, Task *task, const std::function<void(const QString &)> &waiting);
    ~Lanes();
    static bool busy(const QString &root);

private:
    QSet<QString> held;
    static QMutex lock;
    static QWaitCondition freed;
    static QSet<QString> taken;
};

}  // namespace drives
