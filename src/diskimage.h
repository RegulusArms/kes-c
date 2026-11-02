// Disk images: backing up a drive to an image file, and mounting image files.
//
// Back Up Drive to Image… (a drive's right-click menu, in the sidebar's Devices or on its Overview card): the raw
// device, a whole disk or one partition, copied into an image file, optionally compressed (zstd) and encrypted with a
// password (gpg --symmetric, as Compress…'s password-encrypted tar archives). Reading the device needs root: the admin
// helper's readdev (admin_helper.cpp) reads it and writes it into a FIFO Kestrel made, and Kestrel, as the user, makes
// the image from that stream, so it's never on the disk unencrypted. Written under a part name and renamed into place
// once whole and complete; a cancel or a failure deletes the part.
//
// Mount Image (double-click an .img, or right-click any disk image): the image set up as a loop device through udisks
// (no root: polkit asks if needed, as GNOME's Disk Image Mounter does), its file systems mounted, read-only unless
// chosen otherwise, so it shows in the sidebar and on the Overview like a plugged-in drive. A LUKS-encrypted file
// system inside asks for its password (udisks unlocks it). A compressed (.img.zst/.xz/.gz) or gpg-encrypted
// (.img.gpg, as Back Up makes them) image can't be mounted as it is (a loop device needs to read anywhere in it): it's
// expanded first into a copy in the cache (asked first), mounted read-only and deleted once unmounted. Once
// everything mounted from an image is unmounted, its loop device is detached (and LUKS locked again).
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class MainWindow;
class QWidget;
class Task;

namespace diskimage {

// ---- back up

// device: a partition or a whole disk (/dev/…)
void backup_window(QWidget *parent, const QString &device);
QString disk_of(const QString &device);   // a partition's disk (/dev/sdb for /dev/sdb1); a disk itself
// Why the image can't be saved in dest_dir: it's on the drive being read (empty: it can be)
QString refuse_reason(const QString &device, const QString &dest_dir);
QStringList mounted_writable(const QString &device);   // its file systems mounted read-write (their mount points)

struct Backup {
    QString device, dest;   // dest: the image file to make (must not exist)
    bool compress = false;
    QString password;       // empty: not encrypted
};
Task *start_backup(QWidget *parent, const Backup &b);
// The image made from a stream (the device's bytes on in_fd, total of them if known): compressed and encrypted as
// asked, into a part beside dest, renamed to dest only once the stream has ended and whole() says it all came
// (whole: waited for after the end, e.g. the helper's answer). Raises Error or Cancelled; then no part is left.
void write_image(Task *t, int in_fd, qint64 total, const QString &dest, bool compress, const QString &password,
                 const std::function<bool()> &whole);

// ---- mount

bool is_image(const QString &path);      // .img, .iso, .raw; compressed (.zst, .xz, .gz) or encrypted (.gpg) too
bool needs_expanding(const QString &path);   // compressed or encrypted: mounted from a copy
bool can_write(const QString &path);     // plain (.img, .raw) and writable: may be mounted for writing
void mount_image(MainWindow *win, const QString &path, bool writable = false);
QString cache_dir();   // where expanded images are kept while mounted
// The images mounted here that have nothing mounted any more: their loop devices detached, LUKS locked, an expanded
// copy deleted. Run when the mount table changes, and at start (copies left by a Kestrel that ended meanwhile).
void tidy();

}  // namespace diskimage
