// Asynchronous thumbnail generation.
//
// File thumbnails are read from / written to the shared freedesktop cache (~/.cache/thumbnails), so thumbnails
// made by GNOME Files are reused and vice versa. Folder previews are a mosaic of the first images in the folder,
// cached in ~/.cache/kestrel-explorer/folders.
#pragma once

#include <QHash>
#include <QIcon>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPixmap>
#include <QQueue>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>

#include <atomic>
#include <list>

namespace thumbs {

extern const QStringList COVER_NAMES;
QString covers_file();
QString styles_file();   // per-folder {"color": "#rrggbb", "previews": false}

// colours offered for folder icons (right-click a folder → Folder Colour, or its Properties)
extern const QList<QPair<QString, QString>> FOLDER_COLORS;
QIcon color_swatch(const QString &color, int size = 16);   // a small rounded square for menus and lists
int bucket_for(int size);

struct FolderOpts {
    int count = 4;
    QString order = "name";
    QString color = "#d9652f";
    QString cover;
};

QImage load_scaled(const QString &path, int size);
// system thumbnailers (PDF, fonts, audio, …): {mime type: [Exec line, …]} from the freedesktop .thumbnailer files
const QHash<QString, QStringList> &thumbnailers();
QStringList thumbnailers_for(const QString &path);
// whether a file can get a thumbnail: images and videos (Kestrel's own), anything else a system thumbnailer handles
bool can_thumbnail(const QString &path);
QImage system_thumb(const QString &path, int size);
QImage video_frame(const QString &path, int size);
// a QImage thumbnail (max size x size) using the freedesktop cache; null if there is none. fresh: made again even if
// the cache has one (Regenerate Previews), and the cache's copy replaced
QImage file_thumb(const QString &path, qint64 mtime, int size, bool fresh = false);
QStringList pick_folder_images(const QString &folder, int count, const QString &order, const QString &cover = QString());
QImage compose_folder(const QList<QImage> &images, int size, const QString &color, const QList<bool> &videos);
// The symbol of a special folder (icon theme name, e.g. "folder-download": its -symbolic version) drawn on a plain
// folder in `color`, in a shade of that colour so it shows on any. Unchanged if the theme has no such symbol. UI thread
// (it uses the icon theme).
QImage add_folder_symbol(const QImage &folder, const QString &icon, const QString &color);
// fresh: made again even if cached, its tiles' thumbnails too (Regenerate Folder Previews)
QImage folder_thumb(const QString &path, qint64 mtime, int size, const FolderOpts &opts, bool fresh = false);
// The phone's own small preview of a file (is_device_path; uri from device_uri), scaled to fit size; null if it has
// none. Worker thread.
QImage device_preview(const QString &uri, int size);

// The default folder colour setting (folder_color): "accent", unset, or the old fixed default (#d9652f, which
// Preferences used to save every time) follow the desktop's accent colour; anything else is a fixed colour.
bool follows_accent(const QString &setting);

// Delete every thumbnail this app may have written. Returns (files, bytes).
QPair<qint64, qint64> purge_thumbnails(bool include_shared = false);

}  // namespace thumbs

class ThumbnailManager : public QObject {
    Q_OBJECT
public:
    explicit ThumbnailManager(QObject *parent = nullptr);
    static ThumbnailManager *instance();   // the app's manager (the first one created)

    // a pixmap if ready; otherwise schedule generation and return a null pixmap
    QPixmap get(const QString &path, qint64 mtime, bool is_dir, int size, qint64 fsize = 0);
    QIcon get_icon(const QString &path, qint64 mtime, bool is_dir, int size, qint64 fsize = 0);
    QPixmap scaled(const QPixmap &pm, int size);   // cached smooth downscale to fit size x size
    void cancel_pending();
    void invalidate(const QString &path, bool disk = true);   // disk=false keeps the cached mosaic on disk
    void clear_memory();
    void reload_styles();   // re-read covers and folder styles (another Kestrel changed them)
    void set_cover(const QString &folder, const QString &image);   // empty image resets it

    // per-folder style: colour and previews on/off
    QString custom_color(const QString &folder) const;   // empty for the default
    QString color_for(const QString &folder) const;
    bool previews_for(const QString &folder) const;      // false if image previews are off for this folder
    void set_folder_color(const QStringList &folders, const QString &color);   // empty color = default
    void set_folder_previews(const QStringList &folders, bool on);
    // a plain folder icon in `color` (cached), with a special folder's symbol (add_folder_symbol) if `symbol` is set
    QPixmap plain_folder(const QString &color, int size, const QString &symbol = QString());
    // What to draw for a folder: its preview mosaic (in its colour), else a plain folder in its colour. previews=false
    // skips the mosaic and gives a plain folder only in a custom colour, else a null pixmap for the theme's icon.
    QPixmap folder_pixmap(const QString &path, qint64 mtime, int size, bool previews = true);
    QHash<QString, QVariantMap> styles;

    QHash<QString, QString> covers;
    QSet<QString> failed;
    int max_items = 1500;
    int max_file_mb = 200;
    int folder_count = 4;
    QString folder_order = "name";
    QString folder_color = "#d9652f";   // the default colour in use (the accent's, when folder_accent)
    bool folder_accent = true;

Q_SIGNALS:
    void updated(const QString &path);
    void progress(int done, int total);   // for the current batch (0, 0 = idle)
    void job_done(const QString &key, const QString &path, const QImage &img, qint64 mtime, bool is_dir);

private:
    QString key(const QString &path, qint64 mtime, bool is_dir, int size) const;
    void on_done(const QString &key, const QString &path, const QImage &img, qint64 mtime, bool is_dir);
    void schedule_progress();
    void emit_progress();
    void load_covers();
    void set_style(const QStringList &folders, const QString &key, const QVariant &value);
    QHash<QString, QPixmap> plain_cache;   // by colour, size and symbol
    void cache_put(const QString &key, const QPixmap &pm);

    QThreadPool pool, dir_pool, device_pool;
    int batch_total = 0, batch_done = 0;
    QTimer progress_timer;
    std::list<std::pair<QString, QPixmap>> lru;   // most recently used at the back
    QHash<QString, std::list<std::pair<QString, QPixmap>>::iterator> cache;
    QHash<QString, QIcon> icons;
    QSet<QString> pending;
    int prio = 0;
    QHash<QPair<qint64, int>, QPixmap> scaled_cache;
    QQueue<QPair<qint64, int>> scaled_order;
};

class Task;

// Pre-generate file thumbnails and folder previews for a tree (Generate / Regenerate Previews, each its own job:
// thumbs::preview_job in app.cpp's MainWindow::build_previews).
// folders_only: only folder previews (each makes thumbnails of just the few images it shows), not one for every file.
// depth: how many levels of folders below root to go (-1: all; 1: root and the folders directly in it, whose folder
// previews are made; files' thumbnails only in root).
// fresh: make them again even where the caches have them (Regenerate).
//
// Scanning and generating overlap: items are queued to a small worker pool as the walk finds them, so work starts
// immediately and the total grows until the walk completes. Everything goes to the on-disk caches. Made on the UI
// thread (it reads the manager's settings), then run() on a job's thread.
class RecursiveBuilder : public QObject {
    Q_OBJECT
public:
    RecursiveBuilder(const QString &root, int size, ThumbnailManager *manager, QObject *parent = nullptr,
                     int workers = 3, bool folders_only = false, int depth = -1, bool fresh = false);
    void cancel() { stop = true; }
    // Builds them, here (blocking). task: the job it runs in, for its progress, ✕ and pause (also stops when cancel()ed).
    // Returns whether it was stopped before the end.
    bool run(Task *task = nullptr);
    int done_count() const { return done; }
    int total_count() const { return total; }

Q_SIGNALS:
    void progress(int done, int total, bool scanning, const QString &current);

private:
    void one(char kind, const QString &path);
    void emit_progress(bool force = false);

    QString root;
    int size;
    std::atomic<bool> stop{false};
    int workers;
    bool folders_only;
    int depth;
    bool fresh;
    Task *task = nullptr;
    qint64 max_bytes;
    thumbs::FolderOpts opts;
    QHash<QString, QString> covers;
    QHash<QString, QString> colors;
    QSet<QString> no_previews;
    QMutex lock;
    int done = 0, total = 0;
    bool scanning = true;
    QString current;
    qint64 last_emit = 0;
};
