#include "animate.h"

#include "proc.h"
#include "thumbs.h"
#include "util.h"

#include <QAbstractItemView>
#include <QImageReader>
#include <QMovie>

#include <time.h>

using namespace util;

static const int MAX_PLAYING = 40;
static const int PREVIEW_SECONDS = 15;
static const int PREVIEW_FPS = 12;

static qint64 monotonic_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

namespace animate {

bool enabled(const QString &path)
{
    QString ext = ext_of(path);
    if (ext == ".gif")
        return settings().value("play_gifs", false).toBool();
    if (ext == ".webm")
        return settings().value("play_webm", false).toBool() && which("ffmpeg");
    return false;
}

QString anim_dir() { return join(APP_CACHE(), "animated"); }

QString preview_path(const QString &path, qint64 mtime, int size)
{
    // named after the video's path, modification time and size
    return join(anim_dir(), QString("%1-%2.webp").arg(md5(path + "|" + QString::number(mtime)), QString::number(size)));
}

bool make_preview(const QString &path, const QString &out, int size)
{
    try {
        makedirs(dirname(out), true);
    } catch (const OSError &) {
        return false;
    }
    QString tmp = out + ".part.webp";
    auto r = proc::run({"ffmpeg", "-v", "error", "-y", "-t", QString::number(PREVIEW_SECONDS), "-i", path, "-an", "-vf",
                        QString("fps=%1,scale=%2:%2:force_original_aspect_ratio=decrease").arg(PREVIEW_FPS).arg(size),
                        "-c:v", "libwebp_anim", "-loop", "0", "-q:v", "70", "-compression_level", "3", tmp},
                       120000);
    struct stat st;
    if (r.rc == 0 && stat_(tmp, st) && st.st_size > 0) {
        try {
            util::rename(tmp, out);
            return true;
        } catch (const OSError &) {
        }
    }
    ::unlink(enc(tmp).constData());
    return false;
}

}  // namespace animate

Animator::Animator(QAbstractItemView *view, QObject *parent) : QObject(parent), view(view)
{
    pool.setMaxThreadCount(2);
    connect(this, &Animator::made, this, &Animator::on_made, Qt::QueuedConnection);
    timer.setInterval(1000);
    connect(&timer, &QTimer::timeout, this, &Animator::prune);
    timer.start();
}

Animator::~Animator()
{
    clear();
    pool.clear();
    pool.waitForDone();
}

QPixmap Animator::frame(const QString &path, const QModelIndex &index, int size)
{
    if (!animate::enabled(path))
        return QPixmap();
    auto it = movies.find(path);
    if (it != movies.end() && it->size != size) {   // zoomed: start again at the new size
        stop(path);
        it = movies.end();
    }
    if (it == movies.end()) {
        if (!start(path, index, size))
            return QPixmap();
        it = movies.find(path);
    }
    it->index = QPersistentModelIndex(index);
    it->last = monotonic_ms();
    return it->movie->currentPixmap();
}

bool Animator::start(const QString &path, const QModelIndex &index, int size)
{
    if (movies.size() >= MAX_PLAYING || failed.contains(path))
        return false;
    QString source = path;
    bool video = ext_of(path) == ".webm";
    if (video) {
        int bucket = thumbs::bucket_for(size);
        struct stat st;
        if (!stat_(path, st))
            return false;
        source = animate::preview_path(path, st.st_mtime, bucket);
        if (!exists(source)) {
            if (!making.contains(path)) {
                making << path;
                QString out = source;
                pool.start([this, path, out, bucket]() { Q_EMIT made(path, animate::make_preview(path, out, bucket)); });
            }
            return false;
        }
    }
    QSize src = QImageReader(source).size();
    auto *movie = new QMovie(source, QByteArray(), this);
    if (!movie->isValid()) {
        delete movie;
        failed << path;
        return false;
    }
    if (src.isValid())
        movie->setScaledSize(src.width() > size || src.height() > size ? src.scaled(size, size, Qt::KeepAspectRatio) : src);
    movie->setCacheMode(video ? QMovie::CacheAll : QMovie::CacheNone);
    connect(movie, &QMovie::frameChanged, this, [this, path]() {
        auto e = movies.constFind(path);
        if (e != movies.constEnd() && e->index.isValid())
            view->update(QModelIndex(e->index));
    });
    movies.insert(path, Entry{movie, QPersistentModelIndex(index), monotonic_ms(), size});
    movie->start();
    return true;
}

void Animator::on_made(const QString &path, bool ok)
{
    making.remove(path);
    if (!ok)
        failed << path;
    view->viewport()->update();
}

void Animator::stop(const QString &path)
{
    auto it = movies.find(path);
    if (it == movies.end())
        return;
    it->movie->stop();
    it->movie->deleteLater();
    movies.erase(it);
}

void Animator::prune()
{
    qint64 now = monotonic_ms();
    QStringList old;
    for (auto it = movies.begin(); it != movies.end(); ++it)
        if (now - it->last > 1500)
            old << it.key();
    for (const QString &p : old)
        stop(p);
}

void Animator::clear()
{
    for (const QString &p : movies.keys())
        stop(p);
    failed.clear();
}
