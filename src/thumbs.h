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
#include <memory>

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

// One thumbnail or folder preview being made (ThumbnailManager::pending), and who asked for it.
struct ThumbRequest {
    quint64 id = 0;
    QSet<const QObject *> owners;   // the views that asked (a pane's models, an info panel, a viewer…)
    bool shared = false;            // also asked for without an owner: only cancel_pending() drops it
    QThreadPool *pool = nullptr;
    QRunnable *queued = nullptr;    // until it starts running (guarded by ThumbnailManager::run_lock)
    bool in_pool = false;           // queued is in pool (not held back: its jobs paused, or their drive in use)
    int prio = 0;                   // its pool priority: its highest job's
    QSet<quint64> jobs;             // the thumbnail jobs it's part of (ThumbJob)
    std::atomic<bool> cancelled{false};   // no one wants it any more: skipped if it hasn't started
};

// A thumbnail job: what one view asked for in one folder (its thumbnails and folder previews), listed in the Jobs
// window's lower part with Pause and Stop. Leaving the folder lowers it: what's newer runs first, coming back raises
// it, and a lowered job stops after a few minutes (Preferences: thumb_job_minutes). When it runs is the thumbnailing
// mode's (thumb_mode, ThumbnailManager::set_mode): "wait" (the default): while another job uses its drive (the job
// queue) it waits, and goes on once that's done; "drive": one thumbnail job per drive runs, also beside other jobs
// there, the others on that drive waiting behind it; "paused": none runs until the mode changes.
struct ThumbJob {
    quint64 id = 0;
    const QObject *owner = nullptr;
    QString folder, window;          // the folder, and the window it's for (its title then)
    QString drive;                   // the drive it's on (drives::drive_ids), for "drive"
    int done = 0, total = 0;
    bool lowered = false, paused = false, waiting = false;   // waiting: another job uses its drive
    bool behind = false;             // "drive": another thumbnail job on its drive runs first
    bool held_all = false;           // "paused": all thumbnailing is
    qint64 lowered_at = 0;           // ms since the epoch
    int prio = 0;
    QSet<QString> keys;              // its requests still pending
    QString title() const;           // "Thumbnails: Pictures/2024"
    QString state() const;           // "Running", "Paused", "Waiting for the drive", "Lowered (left)"…
};

class ThumbnailManager : public QObject {
    Q_OBJECT
public:
    explicit ThumbnailManager(QObject *parent = nullptr);
    static ThumbnailManager *instance();   // the app's manager (the first one created)

    // a pixmap if ready; otherwise schedule generation and return a null pixmap. owner: who wants it (cancel_for;
    // dropped when it's destroyed); nullptr for a request only cancel_pending() drops
    QPixmap get(const QString &path, qint64 mtime, bool is_dir, int size, qint64 fsize = 0,
                const QObject *owner = nullptr);
    QIcon get_icon(const QString &path, qint64 mtime, bool is_dir, int size, qint64 fsize = 0,
                   const QObject *owner = nullptr);
    QPixmap scaled(const QPixmap &pm, int size);   // cached smooth downscale to fit size x size
    void cancel_pending();   // everyone's (previews turned off, the caches deleted)
    // What owner asked for and no one else still wants: taken out of the queue, or skipped when its turn comes (one
    // already being made is finished and cached). Other owners' requests stay.
    void cancel_for(const QObject *owner);
    int pending_count() const { return int(pending.size()); }
    // owner moved on from its folder: its jobs are lowered (not stopped), so what it shows now comes first
    void lower(const QObject *owner);
    QList<ThumbJob> thumb_jobs() const;   // newest (highest) first
    void pause_job(quint64 id, bool on);
    void stop_job(quint64 id);            // its thumbnails made so far stay
    int job_minutes = 3;                  // a lowered job stops after this long (Preferences)
    // when thumbnail jobs run (ThumbJob): "drive", "wait" or "paused" (known_mode); applied to the jobs at once
    void set_mode(const QString &mode);
    QString mode() const { return thumb_mode; }
    static QString known_mode(const QString &mode);   // mode when it's one of them, else "wait"
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
    QPixmap folder_pixmap(const QString &path, qint64 mtime, int size, bool previews = true,
                          const QObject *owner = nullptr);
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
    void jobs_changed();                  // a thumbnail job came, went, moved on or was paused
    void job_done(const QString &key, const QString &path, const QImage &img, qint64 mtime, bool is_dir, quint64 id);

private:
    QString key(const QString &path, qint64 mtime, bool is_dir, int size) const;
    void on_done(const QString &key, const QString &path, const QImage &img, qint64 mtime, bool is_dir, quint64 id);
    void add_owner(ThumbRequest &req, const QObject *owner);
    void drop(const std::shared_ptr<ThumbRequest> &req);
    ThumbJob *job_for(const QObject *owner, const QString &folder);   // made, or raised if it was lowered
    void set_prio(ThumbJob &job, int prio);
    bool held(const ThumbRequest &req) const;   // all its jobs paused or waiting for their drive
    void place(ThumbRequest &req);              // into its pool, or out of it, as held() says (run_lock held)
    void release_jobs(const QString &key, const std::shared_ptr<ThumbRequest> &req, bool done);   // it's done or gone
    void end_job(quint64 id, bool by_user);   // by_user: Stop (not started again while its view shows the folder)
    void check_jobs();                          // drives in use, lowered jobs timed out
    void update_holds();                        // each job's waiting, behind and held_all, as the mode says
    void jobs_said();
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
    QHash<QString, std::shared_ptr<ThumbRequest>> pending;   // by key
    QSet<const QObject *> watched;   // owners whose destroyed() cancels their requests
    QMutex run_lock;
    quint64 next_id = 0;
    QHash<quint64, ThumbJob> jobs;   // by id
    QHash<QPair<const QObject *, QString>, quint64> job_ids;   // (owner, folder) → its job
    QSet<QPair<const QObject *, QString>> stopped;            // jobs stopped (Stop) while their view is still there
    int job_prio = 0;                // the newest job's priority
    QString thumb_mode = "wait";
    QHash<QString, quint64> runners;   // "drive": the thumbnail job running on each drive
    QTimer jobs_timer, said_timer;
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
