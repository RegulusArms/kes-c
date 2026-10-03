// Shared helpers: paths, file-system calls, mime types, icons, desktop integration, trash, bookmarks.
#pragma once

#include <QIcon>
#include <QMimeType>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>

#include <functional>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>

typedef struct _GAppInfo GAppInfo;
typedef struct _GIcon GIcon;

// ---------------------------------------------------------------- errors

// Any failure with a message for the user (Python's RuntimeError / ValueError / OSError).
class Error : public std::runtime_error {
public:
    explicit Error(const QString &msg) : std::runtime_error(msg.toStdString()) {}
    QString message() const { return QString::fromStdString(what()); }
};

// A failed system call; `code` is the errno.
class OSError : public Error {
public:
    OSError(int code, const QString &msg) : Error(msg), code(code) {}
    bool permission() const;   // EACCES or EPERM (Python's PermissionError)
    int code;
};

// Writing to a pipe whose reader has gone (Python's BrokenPipeError).
class BrokenPipe : public OSError {
public:
    using OSError::OSError;
};

// The user cancelled a background task.
struct Cancelled {};

// Raise OSError for errno, Python style: "[Errno 13] Permission denied: 'path'".
[[noreturn]] void throw_errno(const QString &path = QString(), int code = -1);
QString errno_text(int code, const QString &path = QString());

namespace util {

extern const char *APP_NAME;
extern const char *APP_ID;
extern const char *APP_COMMAND;
extern const QStringList LEGACY_NAMES;   // previous name: thumbnails/settings may carry it
extern const char *LEGACY_ID;
extern const char *VERSION;

QString HOME();
QString CONFIG_DIR();      // ~/.config/kestrel-explorer
QString CACHE_ROOT();      // ~/.cache
QString THUMB_DIR();       // ~/.cache/thumbnails
QString APP_CACHE();       // ~/.cache/kestrel-explorer
QString TRASH_DIR();       // ~/.local/share/Trash
QString GTK_BOOKMARKS();   // ~/.config/gtk-3.0/bookmarks

extern const QSet<QString> VIDEO_EXTS;
extern const QSet<QString> RAW_EXTS;

QSettings &settings();

// Prefer the system's programs and libraries over Anaconda's (or Miniconda's, Miniforge's…).
//
// Shell profiles set up by `conda init` put Anaconda's bin folder ahead of /usr/bin, with copies of gsettings, gio,
// ffmpeg, xz, zstd… that don't match the desktop: Anaconda's gsettings can't see your real settings (no dconf), so
// "Set as Wallpaper" silently does nothing, and its libraries and Qt plugins can break the tools Kestrel runs. So at
// startup Kestrel moves Anaconda's folders to the end of PATH and similar search paths, and drops the conda-only
// overrides for GLib and Qt modules — unless you deliberately run Kestrel from an activated conda environment
// (`conda activate myenv`, anything but the auto-activated "base"): then the environment is left as it is.
// Call before GLib or Qt are used.
bool explicit_conda_env();
void prefer_system_environment();

// ---------------------------------------------------------------- paths (os.path semantics)

QByteArray enc(const QString &path);    // file-system encoding of a path
QString dec(const char *path);
QString dirname(const QString &p);
QString basename(const QString &p);
QString join(const QString &a, const QString &b);
QString normpath(const QString &p);
QString abspath(const QString &p);
QString realpath(const QString &p);
QString relpath(const QString &p, const QString &start);
QString commonpath(const QStringList &paths);
QString expanduser(const QString &p);
QString rstrip(const QString &s, QChar c);
QString strip(const QString &s, const QString &chars);
QString splitext_ext(const QString &p);  // os.path.splitext(p)[1]
QString ext_of(const QString &p);        // lower-cased extension with the dot

// ---------------------------------------------------------------- file system (raise OSError)

bool stat_(const QString &p, struct stat &st);    // follows links; false on error
bool lstat_(const QString &p, struct stat &st);
bool exists(const QString &p);
bool lexists(const QString &p);
bool isdir(const QString &p);
bool isfile(const QString &p);
bool islink(const QString &p);
bool access(const QString &p, int mode);
qint64 getsize(const QString &p);                 // raises
QStringList listdir(const QString &p);            // raises
void makedirs(const QString &p, bool exist_ok = false, mode_t mode = 0777);
void mkdir(const QString &p, mode_t mode = 0777);
void rename(const QString &a, const QString &b);
void unlink(const QString &p);
void rmdir(const QString &p);
void symlink(const QString &target, const QString &link);
void link(const QString &target, const QString &link);
QString readlink(const QString &p);
void chmod(const QString &p, mode_t mode);
void copystat(const QString &src, const QString &dst, bool follow_symlinks = true);
void copyfile(const QString &src, const QString &dst);   // contents only, like shutil.copyfile
void move(const QString &src, const QString &dst);       // shutil.move
void rmtree(const QString &p);                            // ignores errors
void write_text(const QString &p, const QByteArray &data, bool exclusive = false);
QByteArray read_file(const QString &p, bool *ok = nullptr);

// os.walk: fn(root, dirs, files) for every folder; with topdown the callback may prune `dirs`.
// Directories are classified by following links (like os.walk), but links are never descended.
// Return false from fn to stop the walk.
using WalkFn = std::function<bool(const QString &root, QStringList &dirs, QStringList &files)>;
void walk(const QString &top, const WalkFn &fn, bool topdown = true);

// ---------------------------------------------------------------- strings

QString file_uri(const QString &path);   // escaped the way GLib does it (freedesktop thumbnail cache)
QString uri_to_path(const QString &uri); // null QString for non-file URIs
QString md5(const QString &s);
QString human_size(qint64 n);
QString human_size(double n);
QString group_digits(qint64 n);           // 12,345
bool natural_less(const QString &a, const QString &b);
void natural_sort(QStringList &list);
QString unique_path(const QString &directory, const QString &name, const QString &style = "copy");
QPair<QString, QString> split_ext(const QString &name);
QString fmt_time(qint64 ts, const char *fmt);   // strftime in local time

// shlex
QStringList shlex_split(const QString &s, bool *ok = nullptr, QString *err = nullptr);
QString shlex_quote(const QString &s);
QString shlex_join(const QStringList &args);

// ---------------------------------------------------------------- file types

const QSet<QString> &image_exts();
bool is_image(const QString &path);
bool is_raw(const QString &path);
bool is_video(const QString &path);
QMimeType mime_for(const QString &path, int is_dir = -1);
QMimeType mime_for_content(const QString &path);

// ---------------------------------------------------------------- icons

extern QHash<QString, QString> SPECIAL_DIR_ICONS;
QString xdg_user_dir(const QString &key);
QIcon theme_icon(const QStringList &names);
inline QIcon theme_icon(const QString &name) { return theme_icon(QStringList{name}); }
QIcon icon_for_path(const QString &path, int is_dir = -1);
const QHash<QString, QString> &special_dir_icons();
void setup_icon_theme();

// ---------------------------------------------------------------- applications (GIO)

using AppRef = std::shared_ptr<GAppInfo>;
AppRef app_ref(GAppInfo *app);    // takes ownership of one reference
QString app_name(const AppRef &app);
QString app_id(const AppRef &app);
QIcon app_icon(const AppRef &app);
QIcon gicon_to_qicon(GIcon *gicon);

bool open_default(const QString &path);
QString content_type(const QString &path);
QPair<QList<AppRef>, QList<AppRef>> apps_for(const QString &path);   // (recommended, others)
AppRef default_app(const QString &path);
AppRef default_app_for_type(const QString &ct);
QList<AppRef> apps_for_type(const QString &ct);
AppRef app_by_id(const QString &id);
void launch_app(const AppRef &app, const QStringList &paths);   // raises Error
void set_default_for_type(const AppRef &app, const QString &ct); // raises Error
bool which(const QString &name);
QString which_path(const QString &name);
bool open_terminal(const QString &directory);
void set_wallpaper(const QString &path);
void trash(const QString &path);   // raises OSError
void mark_trusted(const QString &path);

// ---------------------------------------------------------------- trash on every drive

QStringList trash_dirs();
QString trash_root(const QString &path);
bool in_trash(const QString &path);
QString trash_info_path(const QString &trashed);
QString trash_original_path(const QString &trashed);
QList<QPair<QString, QString>> trashed_items();   // (path inside files/, original path or null)
bool trash_is_empty();

// ---------------------------------------------------------------- bookmarks

QList<QPair<QString, QString>> read_bookmarks();   // (target, label)
void write_bookmarks(const QList<QPair<QString, QString>> &items);

QList<QUrl> url_list(const QStringList &paths);
void ensure_desktop_entry();
void migrate_legacy();

}  // namespace util

Q_DECLARE_METATYPE(util::AppRef)
