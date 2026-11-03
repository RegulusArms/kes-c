// RAM disks, and private mode.
//
// A RAM disk is a folder whose files are kept in memory (tmpfs): fast, and gone when it's taken down. Two kinds:
// - FOLDER: a folder in /dev/shm (in Kestrel's own folder there, /dev/shm/kestrel-explorer-<uid>, only yours). No
//   password, no size limit of its own: it shares /dev/shm's (half the RAM). The desktop empties /dev/shm of your
//   files when you log out (logind's RemoveIPC), so it lasts at most until then.
// - MOUNT: a tmpfs of its own, mounted on a folder you choose, with its own size limit. Mounting takes the admin
//   password (kes-admin-helper's tmpfs_mount / tmpfs_unmount / tmpfs_unit ops).
//
// How long one lasts (Life):
// - EXIT: until the last Kestrel closes (the tower takes it down when it lands, so also after a crash).
// - SESSION: until you log out (FOLDER) or restart (MOUNT).
// - PERMANENT: comes back, empty, at every login (FOLDER: a systemd user unit makes the folder) or boot (MOUNT: a
//   systemd mount unit in /etc/systemd/system, written by the helper).
// - PERSISTENT: PERMANENT, and its files are saved to a folder on disk (save_to) when you log out or shut down, every
//   `interval` minutes in between (a user timer) and with Save to Disk Now, and loaded back into it when it comes up.
//   The saving and loading are `kes --ramdisk-start|stop|save ID`, run by those user units.
//
// A persistent RAM disk is saved only once it has been loaded (marked with the user.kestrel-ramdisk attribute on its
// top folder, or a file in $XDG_RUNTIME_DIR where tmpfs can't hold one): an empty RAM disk that came up without its
// files (a failed load) is never mirrored over the saved copy.
//
// The list is ~/.config/kestrel-explorer/ramdisks.json (written whole, atomically); a change is announced as
// "sidebar", so every Kestrel's sidebar shows it.
//
// Private mode (the private_mode setting, kept until it's turned off, also across restarts): Kestrel's caches
// (thumbnails it makes, folder previews, job logs…) and its histories (Frequent Folders, recent servers, remembered
// syncs, the viewer's folders, the last chooser folder) go to a hidden RAM disk of its own, /dev/shm/kestrel-explorer-
// <uid>/.private, and nothing is added to the desktop's Recent. Its Private folder is where every Save window starts.
// All of it is deleted when the last Kestrel closes (by the tower), and when private mode is turned off.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QMessageBox;
class QWidget;

namespace ramdisk {

enum Kind { FOLDER, MOUNT };
enum Life { EXIT, SESSION, PERMANENT, PERSISTENT };

struct Disk {
    QString id;          // 8 hex digits: the units' names
    QString name;        // shown
    Kind kind = FOLDER;
    QString path;        // FOLDER: shm_root()/name; MOUNT: the folder it's mounted on
    Life life = SESSION;
    qint64 size_mb = 0;  // MOUNT: its limit
    QString save_to;     // PERSISTENT: the folder on disk its files are saved to
    int interval = 15;   // PERSISTENT: minutes between saves (0: only at logout and shutdown)
};

// ---- the list
QString list_file();   // ~/.config/kestrel-explorer/ramdisks.json
QList<Disk> disks();
void save_disks(const QList<Disk> &list);   // and tells every Kestrel ("sidebar")
const Disk *find(const QList<Disk> &list, const QString &id);
QString new_id(const QList<Disk> &list);

// /dev/shm/kestrel-explorer-<uid>, made (0700) if needed; empty if it can't be made or isn't ours (someone else's
// folder or a symlink by that name: then FOLDER RAM disks and private mode aren't available)
QString shm_root();
QString folder_path(const QString &name);   // a FOLDER RAM disk's
// a RAM disk's name: not empty, not "." or "..", no "/", not starting with "."; empty if fine, else why not
QString name_problem(const QString &name);

bool online(const Disk &d);   // its folder is there (FOLDER) or a tmpfs is mounted on it (MOUNT)
bool is_tmpfs_mount(const QString &path);
bool is_empty(const QString &dir);   // no files (or not there)
QString life_text(Life l, Kind k);
QString describe(const Disk &d);   // "In RAM · until you log out · saved to ~/RAM backup"

// ---- loading and saving (persistent ones)
// Make dst match src: each file that differs (size or modification time) is written under a part name, synced when
// it replaces one, and renamed into place, so an interrupted save leaves every file whole, old or new; with deletes,
// what's only in dst goes after. Symlinks are copied as links; devices, FIFOs and sockets are left out. Throws Error
// (OSError) only if it can't start (src or dst missing); each file that fails is added to *errors and skipped.
// cancelled() is asked between files: true stops it (Cancelled).
struct MirrorStats {
    int copied = 0, deleted = 0;
    qint64 bytes = 0;
};
MirrorStats mirror(const QString &src, const QString &dst, bool deletes, QStringList *errors,
                   const std::function<bool()> &cancelled = nullptr);
bool loaded(const QString &root, const QString &id);   // its files have been loaded into it (this boot)
void mark_loaded(const QString &root, const QString &id);
// Load / save one now (the CLI's and the window's); errors in *errors. load: only into a RAM disk not loaded yet, and
// never deleting what's in it already. save: only from a loaded one.
bool load(const Disk &d, QStringList *errors, const std::function<bool()> &cancelled = nullptr);
bool save(const Disk &d, QStringList *errors, const std::function<bool()> &cancelled = nullptr);

// `kes --ramdisk-start|--ramdisk-stop|--ramdisk-save ID` (the user units'), before any QApplication; -1: not one of these
int run_cli(int argc, char **argv);

// ---- systemd user units (FOLDER PERMANENT, and every PERSISTENT one)
bool have_user_systemd();
QString unit_dir();                        // ~/.config/systemd/user
QStringList unit_names(const Disk &d);     // kestrel-ramdisk-<id>.service, -save.service, .timer (those it needs)
QString service_text(const Disk &d, const QString &kes);   // the units' contents (kes: the program they run)
QString save_service_text(const Disk &d, const QString &kes);
QString timer_text(const Disk &d);
bool needs_user_units(const Disk &d);
void install_units(const Disk &d, QStringList *errors);   // written, enabled and started
void remove_units(const Disk &d, QStringList *errors);    // stopped (no save: the caller saves first), removed

// ---- bringing them up and down (as the user; MOUNT ones through the admin session, as jobs)
// Make a new one (or bring an existing one back up: a click on one that's down): its folder or mount, its units, a
// first load (persistent). done(ok) runs on the UI thread, after it has been added to the list (new) and warned about.
void create(QWidget *parent, const Disk &d, std::function<void(bool)> done = nullptr);
void bring_up(QWidget *parent, const QString &id);
// At start: forget those whose time is over (EXIT and SESSION ones no longer in memory)
void prune();
// Take one down for good: saved first (persistent), units removed, unmounted / its folder deleted, out of the list.
void remove(QWidget *parent, const QString &id);
// Change one (name, life, size, save location, interval): its units and mount are brought in line.
void change(QWidget *parent, const Disk &before, const Disk &after);
void save_now(QWidget *parent, const QString &id);   // a job

// ---- private mode
bool private_mode();             // the setting
QString private_dir();           // shm_root()/.private (empty if shm_root() is)
QString private_files();         // private_dir()/Private: where Save windows start
// Point this process's caches and histories at the private RAM disk (on) or back at ~/.cache and ~/.config (off),
// making its folders and copying the histories into it the first time. Called at start and when the setting changes.
void apply_private();
// The tower, landing (the last Kestrel has gone): EXIT RAM disks taken down (a MOUNT one emptied: unmounting needs
// the password), and the private RAM disk deleted.
void last_kestrel_gone();
// Turn private mode on or off for every Kestrel (off: the private RAM disk is deleted, after the warning below).
void set_private(QWidget *parent, bool on);
// What closing the last Kestrel (or leaving private mode) deletes: files in Private and in EXIT RAM disks.
// True when there are some and the warning (ramdisk_wipe_warned) hasn't been turned off.
bool wipe_needs_warning(bool leaving_private_only = false);
// The warning: "these files will be deleted", with Don't Show This Again; go() runs on Delete. Returns the box, shown.
QMessageBox *warn_wipe(QWidget *parent, bool leaving_private_only, std::function<void()> go,
                       std::function<void()> cancel = nullptr);

}  // namespace ramdisk
