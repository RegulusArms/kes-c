// Animated GIFs and WebM clips playing in the file view (Preferences → Play … in the file view).
//
// GIFs play straight from the file. Qt can't play video without a multimedia library, so a WebM gets a small silent
// looping preview made by ffmpeg (animated WebP, thumbnail-sized, at most the first 15 seconds), cached in
// ~/.cache/kestrel-explorer/animated, which then plays like a GIF. Only items on screen play: one that hasn't been
// painted for a moment (scrolled away, another folder) is stopped and freed.
#pragma once

#include <QHash>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

class QAbstractItemView;
class QMovie;

namespace animate {

bool enabled(const QString &path);   // playback is switched on for this kind of file
QString anim_dir();
QString preview_path(const QString &path, qint64 mtime, int size);
bool make_preview(const QString &path, const QString &out, int size);   // ffmpeg; true on success

}  // namespace animate

// Plays the animated items of one view. The grid delegate asks frame() for each item it paints.
class Animator : public QObject {
    Q_OBJECT
public:
    explicit Animator(QAbstractItemView *view, QObject *parent = nullptr);
    ~Animator() override;
    // the current frame (already scaled to fit size) if path is playing, else a null pixmap (and it starts playing
    // if it can)
    QPixmap frame(const QString &path, const QModelIndex &index, int size);
    void clear();   // stop everything (another folder, changed settings)

Q_SIGNALS:
    void made(const QString &path, bool ok);

private:
    struct Entry {
        QMovie *movie;
        QPersistentModelIndex index;
        qint64 last;
        int size;
    };
    bool start(const QString &path, const QModelIndex &index, int size);
    void stop(const QString &path);
    void prune();
    void on_made(const QString &path, bool ok);

    QAbstractItemView *view;
    QHash<QString, Entry> movies;
    QSet<QString> making, failed;
    QThreadPool pool;
    QTimer timer;
};
