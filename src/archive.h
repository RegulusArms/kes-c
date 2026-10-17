// Archive engine: create and extract archives with the system's command-line tools.
//
// Creating: 7z, zip, rar, tar (plain, or piped through pigz/gzip, pbzip2/lbzip2/bzip2, xz, zstd, plzip/lzip, lz4),
// zpaq, and single-file gz/bz2/xz/zst/lz/lz4. Extracting: all of those plus iso/cab/deb/rpm/... through 7z.
//
// Each job runs on a Task thread: tools are started detached from any terminal with stdin closed (so nothing can
// sit waiting on a password prompt), progress is read from their "NN%" output or counted from the bytes Kestrel
// pipes through them, Cancel kills the whole process group, and partial output is removed.
//
// Passwords go to 7z, rar and unrar on stdin, and to zip and unzip in an environment variable. zpaq only accepts
// them as a command-line switch, which other local users could read from the process list while the job runs.
#pragma once

#include "util.h"

#include <QDateTime>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

class Task;

namespace archive {

class WrongPassword : public Error {
public:
    WrongPassword() : Error("wrong password") {}
};

QString tool(const QString &name);   // full path, preferring the system's copy; empty if missing
int cpu_count();

struct Levels {
    int min, max, def;
};

struct Format {
    QString id, label, ext;
    QStringList tools, available;
    std::optional<Levels> levels;
    bool threads = false, password = false, encrypt_names = false, volumes = false, solid = false, recovery = false;
    QStringList methods;
    QString stream;   // compressor suffix for tar.* and single-file formats
    bool single = false;
};

extern const QMap<int, QString> LEVEL_NAMES_7Z;
QList<Format> formats();   // formats you can create, in menu order
std::optional<Levels> tool_levels(const Format &fmt, const QString &tool_name);
bool tool_threads(const Format &fmt, const QString &tool_name);
QString install_hint(const QString &name);

// What a Compress dialog asks for (see archive_ui).
struct Spec {
    Format format;
    QString tool, base, out;
    QStringList rels;
    std::optional<int> level;
    QString method;
    int threads = 0;
    QString password;
    bool encrypt_names = false;
    QString zip_encryption;
    qint64 volume_mb = 0;
    std::optional<bool> solid;
    int recovery = 0;
    QStringList extra;
    bool trash_originals = false;
    qint64 total = 1;
};

// (kind, stream suffix) for an archive path; kind is rar, 7z, tar, single or zpaq, or empty
QPair<QString, QString> kind(const QString &path);
QString first_volume(const QString &path);
QString extract_tool(const QString &path);
bool can_extract(const QString &path);
QString missing_extract_tool(const QString &path);   // package to install, or empty if a tool is available
// Opening it (double-click, Enter) shows Kestrel's Extract dialog: an archive whose tool is installed. Not packages,
// disk images and apps that are archives inside (.deb, .iso, .apk…): those open with the system's app.
bool opens_as_archive(const QString &path);
QString archive_stem(const QString &path);

QString command_preview(const Spec &spec);   // shell-like text of what will run (password masked)
QString compress(Task *task, const Spec &spec);   // returns the output path

struct ProbeResult {
    bool encrypted = false;
    QString error;
};
ProbeResult probe(const QString &path);   // look inside an archive without a password
// Whether `password` opens the encrypted archive: a quick check (its listing, or a test of its smallest encrypted
// file), not an extraction. True also when the check can't tell (a damaged archive): extracting will say.
bool password_opens(Task *task, const QString &path, const QString &password);

// A symlink an extraction left out for leading outside the folder: its path in the folder, its target, and why it's
// still there (empty: it was removed).
struct DroppedLink {
    QString path, target, why;
};
// Files an extraction kept although they failed their checksum (a damaged or cut-short archive): their names in the
// archive, and the tool's error lines.
struct Damaged {
    QStringList files;
    QString details;
};
// Extract `path` into the existing folder `dest`. overwrite: "overwrite", "skip" or "rename".
// Raises WrongPassword, Error or Cancelled. Symlinks that led out of dest were removed: they're added to dropped_links.
// damaged: when given (and can_keep_damaged()), files that fail their checksum are kept as far as they could be
// extracted and listed in it, and only those failing doesn't fail the job. Without it unrar deletes them, and any
// damage fails the job.
// members: only these (paths in the archive as list() gives them, a folder's ending in "/": it brings what's inside),
// each under its path in the archive.
QString extract(Task *task, const QString &path, const QString &dest, const QString &password = QString(),
                const QString &overwrite = "rename", int threads = 0, QList<DroppedLink> *dropped_links = nullptr,
                Damaged *damaged = nullptr, const QStringList &members = QStringList());
// What's inside an archive (Open as Archive): each item's path in the archive (a folder's ends in "/"), its size
// and packed size (-1: unknown), time and whether it's encrypted
struct Entry {
    QString path;
    bool dir = false;
    qint64 size = -1, packed = -1;
    QDateTime modified;
    bool encrypted = false;
};
bool can_browse(const QString &path);   // its tool can list it and extract single items (not a single .gz/.xz… file)
// Raises WrongPassword (names encrypted, or a wrong password), Error or Cancelled
QList<Entry> list(Task *task, const QString &path, const QString &password = QString());
// Only `members` (as list() gives them), each put right in `dest` under its own name (a taken name gets a number;
// nothing is replaced). Returns the paths made. Symlinks that would lead out of dest are left out (dropped_links).
QStringList extract_members(Task *task, const QString &path, const QString &dest, const QString &password,
                            const QStringList &members, QList<DroppedLink> *dropped_links = nullptr);
// Test Archive: every file decompressed and checked against its checksum, nothing written
struct TestResult {
    bool ok = false;
    QStringList damaged;   // the files that failed, where the tool names them
    QString details;       // the tool's error lines
};
bool can_test(const QString &path);
TestResult test(Task *task, const QString &path, const QString &password = QString());   // WrongPassword, Error, Cancelled
bool can_keep_damaged(const QString &path);   // its tool tells which files are damaged (unrar, 7z, unzip)
// A symlink target that leads out of the folder an archive is extracted into: an absolute one, or one whose ".."s
// climb above it. depth: how many folders below that folder the link is.
bool link_escapes(int depth, const QByteArray &target);

}  // namespace archive
