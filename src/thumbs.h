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

#include <atomic>
#include <list>

namespace thumbs {

extern const QStringList COVER_NAMES;
QString covers_file();
int bucket_for(int size);

struct FolderOpts {
    int count = 4;
    QString order = "name";
    QString color = "#d9652f";
    QString cover;
};

QImage load_scaled(const QString &path, int size);
QImage video_frame(const QString &path, int size);
// a QImage thumbnail (max size x size) using the freedesktop cache; null if there is none
QImage file_thumb(const QString &path, qint64 mtime, int size);
QStringList pick_folder_images(const QString &folder, int count, const QString &order, const QString &cover = QString());
QImage compose_folder(const QList<QImage> &images, int size, const QString &color, const QList<bool> &videos);
QImage folder_thumb(const QString &path, qint64 mtime, int size, const FolderOpts &opts);
// Delete every thumbnail this app may have written. Returns (files, bytes).
QPair<qint64, qint64> purge_thumbnails(bool include_shared = false);

}  // namespace thumbs

class ThumbnailManager : public QObject {
    Q_OBJECT
public:
    explicit ThumbnailManager(QObject *parent = nullptr);

    // a pixmap if ready; otherwise schedule generation and return a null pixmap
    QPixmap get(const QString &path, qint64 mtime, bool is_dir, int size, qint64 fsize = 0);
    QIcon get_icon(const QString &path, qint64 mtime, bool is_dir, int size, qint64 fsize = 0);
    QPixmap scaled(const QPixmap &pm, int size);   // cached smooth downscale to fit size x size
    void cancel_pending();
    void invalidate(const QString &path);
    void clear_memory();
    void set_cover(const QString &folder, const QString &image);   // empty image resets it

    QHash<QString, QString> covers;
    QSet<QString> failed;
    int max_items = 1500;
    int max_file_mb = 200;
    int folder_count = 4;
    QString folder_order = "name";
    QString folder_color = "#d9652f";

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
    void cache_put(const QString &key, const QPixmap &pm);

    QThreadPool pool, dir_pool;
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

// Pre-generate file thumbnails and folder previews for a whole tree.
//
// Scanning and generating overlap: items are queued to a small worker pool as the walk finds them, so work starts
// immediately and the total grows until the walk completes. Everything goes to the on-disk caches.
class RecursiveBuilder : public QThread {
    Q_OBJECT
public:
    RecursiveBuilder(const QString &root, int size, ThumbnailManager *manager, QObject *parent = nullptr,
                     int workers = 3);
    void cancel() { stop = true; }

Q_SIGNALS:
    void progress(int done, int total, bool scanning, const QString &current);
    void finished_build(int done, int total, bool cancelled);

protected:
    void run() override;

private:
    void one(char kind, const QString &path);
    void emit_progress(bool force = false);

    QString root;
    int size;
    std::atomic<bool> stop{false};
    int workers;
    qint64 max_bytes;
    thumbs::FolderOpts opts;
    QHash<QString, QString> covers;
    QMutex lock;
    int done = 0, total = 0;
    bool scanning = true;
    QString current;
    qint64 last_emit = 0;
};
