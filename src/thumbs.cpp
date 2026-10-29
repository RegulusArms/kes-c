#include "thumbs.h"

#include "drives.h"
#include "fileops.h"
#include "jobs.h"
#include "proc.h"
#include "atc.h"
#include "stats.h"
#include "util.h"

#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QIcon>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <fcntl.h>
#include <gio/gio.h>
#include <unistd.h>

using namespace util;

static qint64 monotonic_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

namespace thumbs {

static const QHash<int, QString> FLAVORS = {{128, "normal"}, {256, "large"}, {512, "x-large"}};
const QStringList COVER_NAMES = {"cover", "folder", ".cover", ".folder", "front", "poster"};

QString covers_file() { return join(CONFIG_DIR(), "covers.json"); }
QString styles_file() { return join(CONFIG_DIR(), "folder_styles.json"); }

const QList<QPair<QString, QString>> FOLDER_COLORS = {
    {TX_NOOP("Red"), "#e01b24"},  {TX_NOOP("Orange"), "#ff7800"}, {TX_NOOP("Yellow"), "#f6d32d"}, {TX_NOOP("Green"), "#33d17a"},
    {TX_NOOP("Teal"), "#2aa198"}, {TX_NOOP("Blue"), "#3584e4"},   {TX_NOOP("Purple"), "#9141ac"}, {TX_NOOP("Pink"), "#e66ba5"},
    {TX_NOOP("Brown"), "#986a44"}, {TX_NOOP("Grey"), "#77767b"}};   // (translated where shown)

QIcon color_swatch(const QString &color, int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QColor(0, 0, 0, 80));
    p.setBrush(QColor(color));
    p.drawRoundedRect(QRectF(0.5, 0.5, size - 1, size - 1), 3, 3);
    p.end();
    return QIcon(pm);
}

int bucket_for(int size)
{
    for (int b : {128, 256, 512})
        if (size <= b)
            return b;
    return 512;
}

// ---------------------------------------------------------------- workers (run in a thread pool)

QImage load_scaled(const QString &path, int size)
{
    // For camera RAW files, quality 0 makes the LibRaw plugin return the embedded JPEG preview (~0.3 s) instead of
    // demosaicing the sensor data (~20 s).
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (is_raw(path))
        reader.setQuality(0);
    QString ext = ext_of(path);
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".jfif") {
        // JPEG can decode straight to a smaller size (much faster), but that needs the dimensions up front
        QSize src = reader.size();
        if (src.isValid() && (src.width() > size || src.height() > size))
            reader.setScaledSize(src.scaled(size, size, Qt::KeepAspectRatio));
    }
    QImage img = reader.read();
    if (img.isNull())
        return QImage();
    if (img.width() > size || img.height() > size)
        img = img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
}

static double video_duration(const QString &path)
{
    auto r = proc::run({"ffprobe", "-v", "error", "-show_entries", "format=duration", "-of",
                        "default=noprint_wrappers=1:nokey=1", path},
                       15000);
    bool ok = false;
    double d = QString::fromUtf8(r.out).trimmed().toDouble(&ok);
    return ok ? d : -1;
}

QImage video_frame(const QString &path, int size)
{
    // a frame from the middle of the video (falls back to near the start)
    double dur = video_duration(path);
    QStringList seeks;
    if (dur > 0.5)
        seeks << QString::number(dur / 2, 'f', 2);
    seeks << "3" << "0";
    for (const QString &seek : seeks) {
        auto r = proc::run({"ffmpeg", "-v", "quiet", "-ss", seek, "-i", path, "-frames:v", "1", "-vf",
                            QString("scale=%1:%1:force_original_aspect_ratio=decrease").arg(size), "-f", "image2pipe",
                            "-vcodec", "png", "-"},
                           30000);
        if (r.failed || r.timed_out)
            return QImage();
        if (!r.out.isEmpty()) {
            QImage img = QImage::fromData(r.out);
            if (!img.isNull())
                return img;
        }
    }
    return QImage();
}

// ---------------------------------------------------------------- system thumbnailers (PDF, fonts, audio, …)

const QHash<QString, QStringList> &thumbnailers()
{
    // the same .thumbnailer files GNOME Files uses, in order of preference: ~/.local/share/thumbnailers first
    static const QHash<QString, QStringList> table = [] {
        QHash<QString, QStringList> t;
        QByteArray home = qgetenv("XDG_DATA_HOME"), dirs = qgetenv("XDG_DATA_DIRS");
        QStringList data_dirs{home.isEmpty() ? HOME() + "/.local/share" : QString::fromLocal8Bit(home)};
        data_dirs += (dirs.isEmpty() ? QString("/usr/local/share:/usr/share") : QString::fromLocal8Bit(dirs)).split(':');
        for (const QString &d : data_dirs) {
            QStringList names;
            try {
                names = listdir(join(d, "thumbnailers"));
            } catch (const OSError &) {
                continue;
            }
            names.sort();
            for (const QString &name : names) {
                if (!name.endsWith(".thumbnailer"))
                    continue;
                QHash<QString, QString> entry;
                bool ok = false;
                for (const QByteArray &line : read_file(join(join(d, "thumbnailers"), name), &ok).split('\n')) {
                    int eq = line.indexOf('=');
                    if (eq > 0)
                        entry.insert(QString::fromUtf8(line.left(eq)).trimmed(), QString::fromUtf8(line.mid(eq + 1)).trimmed());
                }
                QString exe = entry.value("Exec"), try_exec = entry.value("TryExec");
                if (exe.isEmpty() || (!try_exec.isEmpty() && !which(try_exec)))
                    continue;
                for (const QString &mt : entry.value("MimeType").split(';', Qt::SkipEmptyParts))
                    if (!t[mt].contains(exe))
                        t[mt] << exe;
            }
        }
        return t;
    }();
    return table;
}

QStringList thumbnailers_for(const QString &path)
{
    const QHash<QString, QStringList> &table = thumbnailers();
    if (table.isEmpty())
        return {};
    QMimeType m = mime_for(path, 0);
    for (const QString &name : QStringList{m.name()} + m.aliases() + m.allAncestors())
        if (table.contains(name))
            return table.value(name);
    return {};
}

bool in_app_cache(const QString &path)
{
    QString root = APP_CACHE();
    return path == root || path.startsWith(root + '/');
}

bool can_thumbnail(const QString &path)
{
    return is_image(path) || is_video(path) || !thumbnailers_for(path).isEmpty();
}

static QMutex tries_lock;
static QHash<QString, QPair<int, int>> tries;   // Exec line -> (successes, failures): one that only fails is skipped

static QImage run_thumbnailer(const QString &exe, const QString &path, int size)
{
    // The output goes to /tmp/gnome-desktop-thumbnailer-*.png like GNOME's own: Ubuntu's AppArmor profiles only let
    // thumbnailers such as evince/papers write there.
    QByteArray tmpl = "/tmp/gnome-desktop-thumbnailer-XXXXXX.png";
    int fd = ::mkstemps(tmpl.data(), 4);
    if (fd < 0)
        return QImage();
    ::close(fd);
    QString out = dec(tmpl.constData());
    bool ok = false;
    QStringList args = shlex_split(exe, &ok);
    QImage img;
    if (ok && !args.isEmpty()) {
        QStringList argv;
        for (QString a : args) {
            a.replace("%%", QString(QChar(0xFFFF)));
            a.replace("%i", abspath(path)).replace("%u", file_uri(path)).replace("%o", out).replace("%s", QString::number(size));
            argv << a.replace(QChar(0xFFFF), "%");
        }
        proc::Options o;
        o.out = proc::DEVNULL;
        o.err = proc::DEVNULL;
        auto r = proc::run(argv, 30000, o);
        if (!r.failed && !r.timed_out)
            img = QImage(out);
    }
    ::unlink(tmpl.constData());
    return img;
}

QImage system_thumb(const QString &path, int size)
{
    for (const QString &exe : thumbnailers_for(path)) {
        {
            QMutexLocker g(&tries_lock);
            QPair<int, int> t = tries.value(exe);
            if (!t.first && t.second >= 3)
                continue;
        }
        QImage img = run_thumbnailer(exe, path, size);
        {
            QMutexLocker g(&tries_lock);
            (img.isNull() ? tries[exe].second : tries[exe].first) += 1;
        }
        if (!img.isNull()) {
            if (img.width() > size || img.height() > size)
                img = img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            return img;
        }
    }
    return QImage();
}

QImage file_thumb(const QString &path, qint64 mtime, int size, bool fresh)
{
    QString flavor = FLAVORS.value(bucket_for(size));
    QString uri = file_uri(path);
    QString cache_dir = join(THUMB_DIR(), flavor);
    QString cache_path = join(cache_dir, md5(uri) + ".png");
    bool in_cache_dir = path.startsWith(THUMB_DIR());
    if (!in_cache_dir && !fresh && exists(cache_path)) {
        QImage img(cache_path);
        if (!img.isNull() && img.text("Thumb::MTime") == QString::number(mtime)) {
            stats::count("thumbnails from the disk cache");
            return img;
        }
    }
    QImage img;
    if (is_image(path))
        img = load_scaled(path, size);
    else if (is_video(path))
        img = video_frame(path, size);
    bool own = !img.isNull();
    if (img.isNull())   // PDFs, fonts, … (and images Qt can't decode): the system thumbnailers
        img = system_thumb(path, size);
    if (img.isNull())
        return QImage();
    stats::count("thumbnails made");
    // don't bother caching images that are already thumbnail sized
    if (!in_cache_dir && (img.width() >= size || img.height() >= size || is_video(path) || !own)) {
        try {
            makedirs(cache_dir, true, 0700);
            img.setText("Thumb::URI", uri);   // notr: a PNG text key
            img.setText("Thumb::MTime", QString::number(mtime));   // notr: a PNG text key
            img.setText("Software", APP_NAME);   // notr: a PNG text key
            QByteArray tmpl = enc(join(cache_dir, "kes-XXXXXX.png"));
            int fd = ::mkstemps(tmpl.data(), 4);
            if (fd >= 0) {
                ::close(fd);
                QString tmp = dec(tmpl.constData());
                if (img.save(tmp, "PNG")) {
                    ::chmod(tmpl.constData(), 0600);
                    util::rename(tmp, cache_path);
                } else {
                    ::unlink(tmpl.constData());
                }
            }
        } catch (const OSError &) {
        }
    }
    return img;
}

QStringList pick_folder_images(const QString &folder, int count, const QString &order, const QString &cover)
{
    QStringList picks;
    if (!cover.isEmpty() && isfile(cover))
        picks << cover;
    QStringList names;
    try {
        names = listdir(folder);
    } catch (const OSError &) {
        return picks;
    }
    const QSet<QString> &exts = image_exts();
    QStringList images;
    for (const QString &n : names)
        if (!n.startsWith('.') && exts.contains(ext_of(n)))
            images << n;
    if (images.isEmpty()) {   // no pictures: use videos (a still from the middle of each)
        for (const QString &n : names)
            if (!n.startsWith('.') && VIDEO_EXTS.contains(ext_of(n)))
                images << n;
    }
    // name: first by name; last: last by name; newest / oldest: by modification time; spread: by name, from the
    // beginning, middle and end (evenly spaced)
    bool by_time = order == "newest" || order == "oldest";
    bool reverse = order == "newest" || order == "last";
    auto mtime = [&](const QString &n) -> double {
        struct stat st;
        return stat_(join(folder, n), st) ? st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9 : 0;
    };
    auto sort_list = [&](QStringList &l) {
        if (by_time) {
            QHash<QString, double> m;
            for (const QString &n : l)
                m[n] = mtime(n);
            natural_sort(l);   // equal times in name order
            std::stable_sort(l.begin(), l.end(), [&](const QString &a, const QString &b) {
                return reverse ? m[a] > m[b] : m[a] < m[b];
            });
        } else {
            natural_sort(l);
            if (reverse)
                std::reverse(l.begin(), l.end());
        }
    };
    // spread: k of the list, evenly spaced from its first to its last (the first alone when k is 1)
    auto spaced = [&](const QStringList &l, int k) {
        if (order != "spread" || k <= 0 || l.size() <= k)
            return l;
        QStringList out;
        for (int i = 0; i < k; ++i)
            out << l.at(k == 1 ? 0 : int(std::lround(double(i) * (l.size() - 1) / (k - 1))));
        return out;
    };
    sort_list(images);
    if (picks.isEmpty()) {
        for (const QString &name : images) {
            QString stem = name.left(name.size() - splitext_ext(name).size()).toLower();
            if (COVER_NAMES.contains(stem)) {
                picks << join(folder, name);
                break;
            }
        }
    }
    QStringList rest;
    for (const QString &name : images)
        if (!picks.contains(join(folder, name)))
            rest << name;
    for (const QString &name : spaced(rest, count - int(picks.size()))) {
        if (picks.size() >= count)
            break;
        QString p = join(folder, name);
        if (isfile(p))
            picks << p;
    }
    if (picks.isEmpty()) {   // look one level down for nested galleries
        QStringList dirs;
        for (const QString &n : names)
            if (!n.startsWith('.') && isdir(join(folder, n)))
                dirs << n;
        sort_list(dirs);
        for (const QString &d : spaced(dirs, count).mid(0, 12)) {
            if (picks.size() >= count)
                break;
            picks += pick_folder_images(join(folder, d), 1, order).mid(0, 1);
        }
    }
    return picks.mid(0, count);
}

// draw img into rect, center-cropped to fill
static void draw_cover(QPainter &p, const QImage &img, const QRectF &rect)
{
    double iw = img.width(), ih = img.height();
    if (iw == 0 || ih == 0)
        return;
    double scale = std::max(rect.width() / iw, rect.height() / ih);
    double sw = rect.width() / scale, sh = rect.height() / scale;
    p.drawImage(rect, img, QRectF((iw - sw) / 2, (ih - sh) / 2, sw, sh));
}

static void play_badge(QPainter &p, const QRectF &cell)
{
    double d = std::max(10.0, std::min(cell.width(), cell.height()) * 0.32);
    QPointF c = cell.center();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 150));
    p.drawEllipse(QRectF(c.x() - d / 2, c.y() - d / 2, d, d));
    QPainterPath tri;
    tri.moveTo(c.x() - d * 0.15, c.y() - d * 0.22);
    tri.lineTo(c.x() + d * 0.25, c.y());
    tri.lineTo(c.x() - d * 0.15, c.y() + d * 0.22);
    tri.closeSubpath();
    p.setBrush(QColor(255, 255, 255, 235));
    p.drawPath(tri);
}

QImage compose_folder(const QList<QImage> &images, int size, const QString &color, const QList<bool> &videos)
{
    QImage canvas(size, size, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    double s = size;
    double m = s * 0.03;
    QColor back(color);
    QColor front = back.lighter(118);
    // folder tab + back panel
    p.setPen(Qt::NoPen);
    p.setBrush(back.darker(115));
    p.drawRoundedRect(QRectF(m, m + s * 0.06, s * 0.42, s * 0.16), s * 0.04, s * 0.04);
    p.setBrush(back);
    p.drawRoundedRect(QRectF(m, m + s * 0.12, s - 2 * m, s * 0.82), s * 0.06, s * 0.06);
    // front panel
    QRectF body(m, m + s * 0.19, s - 2 * m, s * 0.75);
    p.setBrush(front);
    p.drawRoundedRect(body, s * 0.06, s * 0.06);
    if (images.isEmpty()) {   // a plain folder in this colour (no previews)
        p.end();
        return canvas;
    }
    // mosaic
    QRectF inner = body.adjusted(s * 0.035, s * 0.035, -s * 0.035, -s * 0.035);
    QPainterPath clip;
    clip.addRoundedRect(inner, s * 0.035, s * 0.035);
    p.setClipPath(clip);
    p.fillRect(inner, QColor(0, 0, 0, 60));
    double g = std::max(1.0, s * 0.012);
    double x = inner.x(), y = inner.y(), w = inner.width(), h = inner.height();
    int n = images.size();
    QList<QRectF> cells;
    if (n == 1) {
        cells = {QRectF(x, y, w, h)};
    } else if (n == 2) {
        cells = {QRectF(x, y, w / 2 - g / 2, h), QRectF(x + w / 2 + g / 2, y, w / 2 - g / 2, h)};
    } else if (n == 3) {
        cells = {QRectF(x, y, w * 0.6 - g / 2, h), QRectF(x + w * 0.6 + g / 2, y, w * 0.4 - g / 2, h / 2 - g / 2),
                 QRectF(x + w * 0.6 + g / 2, y + h / 2 + g / 2, w * 0.4 - g / 2, h / 2 - g / 2)};
    } else {
        double cw = w / 2 - g / 2, ch = h / 2 - g / 2;
        cells = {QRectF(x, y, cw, ch), QRectF(x + cw + g, y, cw, ch), QRectF(x, y + ch + g, cw, ch),
                 QRectF(x + cw + g, y + ch + g, cw, ch)};
    }
    for (int i = 0; i < std::min(images.size(), cells.size()); ++i) {
        draw_cover(p, images[i], cells[i]);
        if (i < videos.size() && videos[i])
            play_badge(p, cells[i]);
    }
    p.end();
    return canvas;
}

QImage add_folder_symbol(const QImage &folder, const QString &icon, const QString &color)
{
    int s = folder.width();
    QIcon sym = icon.isEmpty() ? QIcon() : theme_icon(icon + "-symbolic");
    if (sym.isNull() || s < 32)   // too small to make out
        return folder;
    int n = qRound(s * 0.36);
    QImage glyph = sym.pixmap(QSize(n, n)).toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (glyph.isNull())
        return folder;
    if (glyph.width() != n)   // a theme with fixed sizes only (Breeze has 16-48 px)
        glyph = glyph.scaled(n, n, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // the front panel's colour (compose_folder), made darker on a light folder and lighter on a dark one
    QColor front = QColor(color).lighter(118);
    float h, sat, l, a;
    front.getHslF(&h, &sat, &l, &a);
    QColor tone = QColor::fromHslF(h, sat, l > 0.55f ? l - 0.42f : l + 0.42f, 0.9f);
    QPainter g(&glyph);
    g.setCompositionMode(QPainter::CompositionMode_SourceIn);
    g.fillRect(glyph.rect(), tone);
    g.end();
    QImage out = folder.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    // centred on the front panel (compose_folder: from 0.22 to 0.97 of the height)
    double cx = s / 2.0, cy = s * (0.03 + 0.19 + 0.75 / 2);
    p.drawImage(QPointF(cx - glyph.width() / 2.0, cy - glyph.height() / 2.0), glyph);
    p.end();
    return out;
}

QImage folder_thumb(const QString &path, qint64 mtime, int size, const FolderOpts &opts, bool fresh)
{
    QString tag = QString("v2|%1|%2|%3|%4|%5").arg(mtime).arg(opts.count).arg(opts.order, opts.color, opts.cover);
    QString cache = join(join(APP_CACHE(), "folders"), QString("%1-%2.png").arg(md5(path)).arg(size));
    if (!fresh && exists(cache)) {
        QImage img(cache);
        if (!img.isNull() && img.text("FE::Tag") == tag) {
            stats::count("folder previews from the disk cache");
            return img;
        }
        if (!img.isNull() && img.text("FE::Tag") == "empty|" + tag)
            return QImage();
    }
    QStringList picks = pick_folder_images(path, opts.count, opts.order, opts.cover);
    QList<QImage> images;
    QList<bool> videos;
    int tile = (picks.size() > 1 || size <= 128) ? 128 : 256;
    for (const QString &pth : picks) {
        struct stat st;
        if (!stat_(pth, st))
            continue;
        QImage im = file_thumb(pth, st.st_mtime, tile, fresh);
        if (!im.isNull()) {
            images << im;
            videos << is_video(pth);
        }
    }
    try {
        makedirs(dirname(cache), true);
    } catch (const OSError &) {
    }
    // saved whole under a hidden name, then renamed over the old one: another Kestrel never reads half a PNG
    auto save = [&cache](const QImage &im) {
        try {
            write_parts(cache, [&](int fd) {
                QFile f;
                if (!f.open(fd, QIODevice::WriteOnly, QFileDevice::DontCloseHandle) || !im.save(&f, "PNG"))
                    throw OSError(EIO, "couldn't save " + cache);
            }, false);
        } catch (const OSError &) {
        }
    };
    if (images.isEmpty()) {
        QImage marker(1, 1, QImage::Format_ARGB32);
        marker.fill(Qt::transparent);
        marker.setText("FE::Tag", "empty|" + tag);   // notr: a PNG text key
        save(marker);
        return QImage();
    }
    QImage img = compose_folder(images, size, opts.color, videos);
    img.setText("FE::Tag", tag);   // notr: a PNG text key
    save(img);
    stats::count("folder previews made");
    return img;
}

// True if a cached thumbnail PNG carries our Software tag (reads chunk headers only).
static bool made_by_us(const QString &png_path)
{
    QFile f(png_path);
    if (!f.open(QIODevice::ReadOnly) || f.read(8) != QByteArray("\x89PNG\r\n\x1a\n", 8))
        return false;
    for (;;) {
        QByteArray head = f.read(8);
        if (head.size() < 8)
            return false;
        quint32 length = (quint8(head[0]) << 24) | (quint8(head[1]) << 16) | (quint8(head[2]) << 8) | quint8(head[3]);
        QByteArray ctype = head.mid(4);
        if (ctype == "IDAT" || length > (1u << 20))
            return false;
        QByteArray data = f.read(length);
        f.seek(f.pos() + 4);   // crc
        if ((ctype == "tEXt" || ctype == "iTXt") && data.startsWith(QByteArray("Software\0", 9))) {
            if (data.contains(APP_NAME))
                return true;
            for (const QString &n : LEGACY_NAMES)
                if (data.contains(n.toUtf8()))
                    return true;
            return false;
        }
    }
}

QPair<qint64, qint64> purge_thumbnails(bool include_shared)
{
    // Always removes the folder-mosaic cache and the PNGs we tagged in the shared freedesktop cache. With
    // include_shared, empties ~/.cache/thumbnails entirely (also thumbnails made by GNOME Files and other apps).
    qint64 files = 0, size = 0;
    QStringList targets;
    auto collect = [&](const QString &dir) {
        walk(dir, [&](const QString &root, QStringList &, QStringList &names) {
            for (const QString &n : names)
                targets << join(root, n);
            return true;
        });
    };
    collect(join(APP_CACHE(), "folders"));    // folder mosaics
    collect(join(APP_CACHE(), "animated"));   // looping video previews
    for (const QString &flavor : FLAVORS) {
        QString d = join(THUMB_DIR(), flavor);
        if (!isdir(d))
            continue;
        QStringList names;
        try {
            names = listdir(d);
        } catch (const OSError &) {
            continue;
        }
        for (const QString &n : names) {
            QString p = join(d, n);
            if (n.endsWith(".png") && isfile(p) && (include_shared || made_by_us(p)))
                targets << p;
        }
    }
    if (include_shared)
        collect(join(THUMB_DIR(), "fail"));
    for (const QString &t : targets) {
        struct stat st;
        if (stat_(t, st) && ::unlink(enc(t).constData()) == 0) {
            size += st.st_size;
            files += 1;
        }
    }
    return {files, size};
}

bool follows_accent(const QString &setting)
{
    return setting.isEmpty() || setting == "accent" || setting.toLower() == "#d9652f";
}

// ---------------------------------------------------------------- phones and cameras

QImage device_preview(const QString &uri, int size)
{
    // the phone's own small preview (gvfs's preview::icon: gphoto2 and mtp have one, afc doesn't), scaled to fit size
    if (uri.isEmpty())
        return QImage();
    QImage img;
    GFile *f = g_file_new_for_uri(uri.toUtf8().constData());
    GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_PREVIEW_ICON, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    GObject *icon = info ? g_file_info_get_attribute_object(info, G_FILE_ATTRIBUTE_PREVIEW_ICON) : nullptr;
    if (icon && G_IS_LOADABLE_ICON(icon)) {
        GInputStream *in = g_loadable_icon_load(G_LOADABLE_ICON(icon), size, nullptr, nullptr, nullptr);
        if (in) {
            QByteArray data;
            char buf[65536];
            gssize n;
            while ((n = g_input_stream_read(in, buf, sizeof buf, nullptr, nullptr)) > 0 && data.size() < 20000000)
                data.append(buf, n);
            g_object_unref(in);
            QBuffer b(&data);
            QImageReader reader(&b);
            reader.setAutoTransform(true);
            img = reader.read();
        }
    }
    if (info)
        g_object_unref(info);
    g_object_unref(f);
    if (!img.isNull() && img.width() < size && img.height() < size)
        img = img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
}

}  // namespace thumbs

// ---------------------------------------------------------------- manager (main thread)

static ThumbnailManager *first_manager = nullptr;

ThumbnailManager *ThumbnailManager::instance() { return first_manager; }

ThumbnailManager::ThumbnailManager(QObject *parent) : QObject(parent)
{
    if (!first_manager)
        first_manager = this;
    pool.setMaxThreadCount(std::max(2, std::min(6, QThread::idealThreadCount() / 2)));
    dir_pool.setMaxThreadCount(2);   // folder mosaics (directory scans)
    device_pool.setMaxThreadCount(1);   // phones and cameras serve one request at a time (is_device_path)
    image_exts();                    // initialise on the main thread
    thumbs::thumbnailers();
    progress_timer.setSingleShot(true);
    progress_timer.setInterval(100);
    connect(&progress_timer, &QTimer::timeout, this, &ThumbnailManager::emit_progress);
    connect(this, &ThumbnailManager::job_done, this, &ThumbnailManager::on_done, Qt::QueuedConnection);
    said_timer.setSingleShot(true);
    said_timer.setInterval(200);
    connect(&said_timer, &QTimer::timeout, this, &ThumbnailManager::jobs_changed);
    // another job taking or leaving a drive holds or frees the thumbnail jobs there; lowered ones time out
    jobs_timer.setInterval(2000);
    connect(&jobs_timer, &QTimer::timeout, this, &ThumbnailManager::check_jobs);
    jobs_timer.start();
    connect(jobs::Queue::instance(), &jobs::Queue::changed, this, &ThumbnailManager::check_jobs);
    load_covers();
}

void ThumbnailManager::load_covers()
{
    bool ok = false;
    covers.clear();
    styles.clear();
    QByteArray data = read_file(thumbs::covers_file(), &ok);
    QJsonObject obj = QJsonDocument::fromJson(data).object();
    for (auto it = obj.begin(); it != obj.end(); ++it)
        covers[it.key()] = it.value().toString();
    QJsonObject st = QJsonDocument::fromJson(read_file(thumbs::styles_file(), &ok)).object();
    for (auto it = st.begin(); it != st.end(); ++it)
        styles[it.key()] = it.value().toObject().toVariantMap();
}

QString ThumbnailManager::custom_color(const QString &folder) const
{
    return styles.value(folder).value("color").toString();
}

QString ThumbnailManager::color_for(const QString &folder) const
{
    QString c = custom_color(folder);
    return c.isEmpty() ? folder_color : c;
}

bool ThumbnailManager::previews_for(const QString &folder) const
{
    return styles.value(folder).value("previews", true).toBool();
}

void ThumbnailManager::reload_styles() { load_covers(); }

void ThumbnailManager::set_style(const QStringList &folders, const QString &key, const QVariant &value)
{
    load_covers();   // another Kestrel may have changed them since
    for (const QString &f : folders) {
        QVariantMap st = styles.value(f);
        if (value.isValid())
            st[key] = value;
        else
            st.remove(key);
        if (st.isEmpty())
            styles.remove(f);
        else
            styles[f] = st;
    }
    QJsonObject obj;
    for (auto it = styles.begin(); it != styles.end(); ++it)
        obj[it.key()] = QJsonObject::fromVariantMap(it.value());
    try {
        makedirs(dirname(thumbs::styles_file()), true);
        write_text(thumbs::styles_file(), QJsonDocument(obj).toJson(QJsonDocument::Indented));
    } catch (const OSError &) {
    }
    for (const QString &f : folders)
        invalidate(f);
    atc::announce("folders", {{"paths", QJsonArray::fromStringList(folders)}});
}

void ThumbnailManager::set_folder_color(const QStringList &folders, const QString &color)
{
    set_style(folders, "color", color.isEmpty() ? QVariant() : QVariant(color));
}

void ThumbnailManager::set_folder_previews(const QStringList &folders, bool on)
{
    set_style(folders, "previews", on ? QVariant() : QVariant(false));
}

QPixmap ThumbnailManager::plain_folder(const QString &color, int size, const QString &symbol)
{
    int b = thumbs::bucket_for(size);
    QString k = QStringList{color, QString::number(b), symbol}.join('\x1f');
    auto it = plain_cache.find(k);
    if (it == plain_cache.end()) {
        QImage img = thumbs::compose_folder({}, b, color, {});
        if (!symbol.isEmpty())
            img = thumbs::add_folder_symbol(img, symbol, color);
        it = plain_cache.insert(k, QPixmap::fromImage(img));
    }
    return *it;
}

QPixmap ThumbnailManager::folder_pixmap(const QString &path, qint64 mtime, int size, bool previews,
                                       const QObject *owner)
{
    if (previews && previews_for(path)) {
        QPixmap pm = get(path, mtime, true, size, 0, owner);
        if (!pm.isNull())
            return pm;
    }
    // with previews on, a folder without images (or whose preview is still being made) is a plain folder in its
    // colour, like the folders beside it; with them off, only a folder with its own colour isn't the theme's icon
    QString symbol = special_dir_icons().value(path);   // Downloads, Music…: their symbol on the plain folder
    if (previews)
        return plain_folder(color_for(path), size, symbol);
    QString color = custom_color(path);
    return color.isEmpty() ? QPixmap() : plain_folder(color, size, symbol);
}

void ThumbnailManager::set_cover(const QString &folder, const QString &image)
{
    load_covers();
    if (!image.isEmpty())
        covers[folder] = image;
    else
        covers.remove(folder);
    QJsonObject obj;
    for (auto it = covers.begin(); it != covers.end(); ++it)
        obj[it.key()] = it.value();
    try {
        makedirs(dirname(thumbs::covers_file()), true);
        write_text(thumbs::covers_file(), QJsonDocument(obj).toJson(QJsonDocument::Indented));
    } catch (const OSError &) {
    }
    invalidate(folder);
    atc::announce("folders", {{"paths", QJsonArray{folder}}});
}

QString ThumbnailManager::key(const QString &path, qint64 mtime, bool is_dir, int size) const
{
    int b = thumbs::bucket_for(size);
    if (is_dir)
        return QStringList{path, "d", QString::number(b), QString::number(mtime), QString::number(folder_count),
                           folder_order, color_for(path), covers.value(path)}
            .join('\x1f');
    return QStringList{path, "f", QString::number(b), QString::number(mtime)}.join('\x1f');
}

static const qint64 DEVICE_MAX_BYTES = 30000000;   // larger files on a phone are only shown by their preview

static QString key_path(const QString &key) { return key.section('\x1f', 0, 0); }

QPixmap ThumbnailManager::get(const QString &path, qint64 mtime, bool is_dir, int size, qint64 fsize,
                              const QObject *owner)
{
    QString k = key(path, mtime, is_dir, size);
    auto it = cache.find(k);
    if (it != cache.end()) {
        lru.splice(lru.end(), lru, *it);   // most recently used
        return (*it)->second;
    }
    if (failed.contains(k))
        return QPixmap();
    // a thumbnail job of owner's that was stopped: nothing more for it here until it leaves the folder
    if (owner && stopped.contains(qMakePair(owner, dirname(path))))
        return QPixmap();
    // the job (owner and folder) it's part of: its progress, and the request's priority
    auto join_job = [&](ThumbRequest &req) {
        if (!owner)
            return;
        ThumbJob *j = job_for(owner, dirname(path));
        if (!req.jobs.contains(j->id)) {
            req.jobs << j->id;
            j->keys << k;
            j->total += 1;
            jobs_said();
        }
        req.prio = std::max(req.prio, j->prio);
    };
    if (auto p = pending.constFind(k); p != pending.constEnd()) {
        add_owner(**p, owner);   // another view wants it too
        int before = (*p)->prio;
        bool was_held = held(**p);
        join_job(**p);
        if ((*p)->prio != before || held(**p) != was_held) {
            QMutexLocker l(&run_lock);
            place(**p);
        }
    } else {
        // Kestrel's own cache (folder previews, animated previews, copies): thumbnails of it would only fill
        // ~/.cache/thumbnails and its folders with previews of previews
        if (thumbs::in_app_cache(path))
            return QPixmap();
        bool device = is_device_path(path);
        // a tape or floppy: no thumbnails or folder mosaics unless the user allowed them for that drive (each one
        // makes a tape seek, a floppy read for seconds). Not remembered as failed, so allowing them takes effect
        if (!drives::reads_allowed(path))
            return QPixmap();
        if (!is_dir && fsize > qint64(max_file_mb) * 1000000 && !is_video(path)) {
            failed << k;
            return QPixmap();
        }
        // a phone: one file at a time, and no folder mosaics (they would download every file)
        if (device && is_dir) {
            failed << k;
            return QPixmap();
        }
        auto req = std::make_shared<ThumbRequest>();
        req->id = ++next_id;
        req->prio = owner ? std::numeric_limits<int>::min() : ++job_prio;   // no owner: no job, as the newest
        add_owner(*req, owner);
        join_job(*req);
        pending.insert(k, req);
        stats::count("thumbnails not in memory");
        stats::peak("thumbnail queue", pending.size());
        thumbs::FolderOpts opts{folder_count, folder_order, color_for(path), covers.value(path)};
        int b = thumbs::bucket_for(size);
        QString uri = device ? device_uri(path) : QString();
        auto job = [this, req, k, path, mtime, is_dir, b, opts, device, uri, fsize]() {
            {
                QMutexLocker l(&run_lock);
                req->queued = nullptr;   // started: no longer in the queue for drop() to take
            }
            if (req->cancelled)   // no one wants it any more (cancel_for), and the queue couldn't give it back
                return;
            QImage img;
            try {
                if (device) {
                    img = thumbs::device_preview(uri, b);
                    // no preview: read the file itself if it's an image that isn't too big (a read downloads it all)
                    if (img.isNull() && !is_video(path) && fsize <= DEVICE_MAX_BYTES)
                        img = thumbs::file_thumb(path, mtime, b);
                } else {
                    img = is_dir ? thumbs::folder_thumb(path, mtime, b, opts) : thumbs::file_thumb(path, mtime, b);
                }
            } catch (...) {
                img = QImage();
            }
            Q_EMIT job_done(k, path, img, mtime, is_dir, req->id);
        };
        req->pool = device ? &device_pool : is_dir ? &dir_pool : &pool;
        QRunnable *r = QRunnable::create(std::move(job));   // auto-deleted after it has run
        {
            QMutexLocker l(&run_lock);
            req->queued = r;   // before start(): the job clears it when it begins
            place(*req);       // into the pool, unless its job is paused or waiting for its drive
        }
        batch_total += 1;
        schedule_progress();
    }
    return QPixmap();
}

QIcon ThumbnailManager::get_icon(const QString &path, qint64 mtime, bool is_dir, int size, qint64 fsize,
                                 const QObject *owner)
{
    QPixmap pm = get(path, mtime, is_dir, size, fsize, owner);
    if (pm.isNull())
        return QIcon();
    QString k = key(path, mtime, is_dir, size);
    auto it = icons.find(k);
    if (it == icons.end())
        it = icons.insert(k, QIcon(pm));
    return *it;
}

QPixmap ThumbnailManager::scaled(const QPixmap &pm, int size)
{
    QPair<qint64, int> k(pm.cacheKey(), size);
    auto it = scaled_cache.find(k);
    if (it != scaled_cache.end())
        return *it;
    QPixmap out = (pm.width() <= size && pm.height() <= size)
                      ? pm
                      : pm.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled_cache.insert(k, out);
    scaled_order.enqueue(k);
    if (scaled_order.size() > 800)
        scaled_cache.remove(scaled_order.dequeue());
    return out;
}

void ThumbnailManager::cancel_pending()
{
    {
        QMutexLocker l(&run_lock);
        for (const auto &req : std::as_const(pending)) {
            req->cancelled = true;
            if (req->queued && !req->in_pool)   // held back: ours to delete
                delete req->queued;
            req->queued = nullptr;   // clear() deletes the queued ones
        }
        pool.clear();
        dir_pool.clear();
        device_pool.clear();
    }
    pending.clear();
    jobs.clear();
    job_ids.clear();
    jobs_said();
    batch_total = batch_done = 0;
    schedule_progress();
}

void ThumbnailManager::add_owner(ThumbRequest &req, const QObject *owner)
{
    if (!owner) {
        req.shared = true;
        return;
    }
    req.owners.insert(owner);
    if (!watched.contains(owner)) {   // a view closed or replaced without cancelling: its requests go with it
        watched.insert(owner);
        connect(owner, &QObject::destroyed, this, [this, owner]() {
            watched.remove(owner);
            cancel_for(owner);
        });
    }
}

void ThumbnailManager::drop(const std::shared_ptr<ThumbRequest> &req)
{
    req->cancelled = true;
    QMutexLocker l(&run_lock);
    // still queued (it clears queued under this lock when it starts, so the pointer is alive): take it back; one the
    // pool is just handing to a thread isn't there any more, and skips itself (cancelled)
    if (req->queued && (!req->in_pool || req->pool->tryTake(req->queued))) {
        delete req->queued;
        req->queued = nullptr;
    }
}

bool ThumbnailManager::held(const ThumbRequest &req) const
{
    if (thumb_mode == "paused")   // all of it: also what no job asked for
        return true;
    if (req.shared || req.jobs.isEmpty())
        return false;
    for (quint64 id : req.jobs) {
        auto j = jobs.constFind(id);
        if (j != jobs.constEnd() && !j->paused && !j->waiting && !j->behind)
            return false;
    }
    return true;
}

void ThumbnailManager::place(ThumbRequest &req)
{
    if (!req.queued)   // running or done
        return;
    if (req.in_pool) {
        if (!req.pool->tryTake(req.queued))
            return;   // a thread is just taking it: it runs
        req.in_pool = false;
    }
    if (!held(req)) {
        req.pool->start(req.queued, req.prio);
        req.in_pool = true;
    }
}

void ThumbnailManager::release_jobs(const QString &k, const std::shared_ptr<ThumbRequest> &req, bool done)
{
    bool ended = false;
    for (quint64 id : std::as_const(req->jobs)) {
        auto j = jobs.find(id);
        if (j == jobs.end())
            continue;
        j->keys.remove(k);
        if (done)
            j->done += 1;
        if (j->keys.isEmpty()) {   // all made (or no longer wanted): the job is over
            job_ids.remove(qMakePair(j->owner, j->folder));
            jobs.erase(j);
            ended = true;
        }
    }
    req->jobs.clear();
    if (ended)   // the next on its drive may start
        update_holds();
    jobs_said();
}

static QString window_of(const QObject *owner)
{
    for (const QObject *o = owner; o; o = o->parent())
        if (auto *w = qobject_cast<const QWidget *>(o))
            return w->window()->windowTitle();
    return QString();
}

ThumbJob *ThumbnailManager::job_for(const QObject *owner, const QString &folder)
{
    auto id = job_ids.constFind(qMakePair(owner, folder));
    if (id != job_ids.constEnd()) {
        ThumbJob &j = jobs[*id];
        if (j.lowered) {   // back in that folder: first again
            j.lowered = false;
            set_prio(j, ++job_prio);
            jobs_said();
        }
        return &j;
    }
    ThumbJob j;
    j.id = ++next_id;
    j.owner = owner;
    j.folder = folder;
    j.window = window_of(owner);
    j.prio = ++job_prio;
    QStringList ids = drives::drive_ids(folder);
    j.drive = ids.value(0);
    job_ids.insert(qMakePair(owner, folder), j.id);
    jobs.insert(j.id, j);
    update_holds();   // (it has no requests yet: only its own flags, and no one else's, change)
    jobs_said();
    return &jobs[j.id];
}

void ThumbnailManager::set_prio(ThumbJob &job, int p)
{
    job.prio = p;
    QMutexLocker l(&run_lock);
    for (const QString &k : std::as_const(job.keys)) {
        auto req = pending.value(k);
        if (!req)
            continue;
        int best = std::numeric_limits<int>::min();
        for (quint64 id : std::as_const(req->jobs))
            if (auto j = jobs.constFind(id); j != jobs.constEnd())
                best = std::max(best, j->prio);
        if (req->prio != best) {
            req->prio = best;
            place(*req);   // taken back and queued again at its new place
        }
    }
}

void ThumbnailManager::lower(const QObject *owner)
{
    if (!owner)
        return;
    for (auto it = stopped.begin(); it != stopped.end();)   // coming back later makes them again
        it = it->first == owner ? stopped.erase(it) : std::next(it);
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (ThumbJob &j : jobs)
        if (j.owner == owner && !j.lowered) {
            j.lowered = true;
            j.lowered_at = now;
            set_prio(j, j.prio - 1000000000 / 2);   // below every job that isn't lowered, still newest first
        }
    update_holds();   // "drive": a job still shown takes over its drive
    jobs_said();
}

QList<ThumbJob> ThumbnailManager::thumb_jobs() const
{
    QList<ThumbJob> out = jobs.values();
    std::sort(out.begin(), out.end(), [](const ThumbJob &a, const ThumbJob &b) { return a.prio > b.prio; });
    return out;
}

void ThumbnailManager::pause_job(quint64 id, bool on)
{
    auto j = jobs.find(id);
    if (j == jobs.end() || j->paused == on)
        return;
    j->paused = on;
    {
        QMutexLocker l(&run_lock);
        for (const QString &k : std::as_const(j->keys))
            if (auto req = pending.value(k))
                place(*req);
    }
    update_holds();   // "drive": a paused job lets the next on its drive run
    jobs_said();
}

void ThumbnailManager::stop_job(quint64 id) { end_job(id, true); }

void ThumbnailManager::end_job(quint64 id, bool by_user)
{
    auto j = jobs.find(id);
    if (j == jobs.end())
        return;
    if (by_user)   // the view still showing that folder doesn't start it again as it's drawn
        stopped.insert(qMakePair(j->owner, j->folder));
    QSet<QString> keys = j->keys;
    job_ids.remove(qMakePair(j->owner, j->folder));
    jobs.erase(j);
    int n = 0;
    for (const QString &k : keys) {
        auto req = pending.value(k);
        if (!req)
            continue;
        req->jobs.remove(id);
        if (req->jobs.isEmpty() && !req->shared) {   // no one else's job wants it
            pending.remove(k);
            drop(req);
            n += 1;
        } else {
            QMutexLocker l(&run_lock);
            place(*req);
        }
    }
    if (n) {
        batch_total = std::max(batch_done, batch_total - n);
        schedule_progress();
    }
    update_holds();   // "drive": the next on its drive may start
    jobs_said();
}

QString ThumbnailManager::known_mode(const QString &m) { return m == "drive" || m == "paused" ? m : QString("wait"); }

void ThumbnailManager::set_mode(const QString &m)
{
    QString now = known_mode(m);
    if (now == thumb_mode)
        return;
    thumb_mode = now;
    update_holds();
    QMutexLocker l(&run_lock);   // held or let go: also what no job asked for ("paused" holds that too)
    for (const auto &req : std::as_const(pending))
        place(*req);
    l.unlock();
    jobs_said();
}

void ThumbnailManager::update_holds()
{
    bool wait = thumb_mode == "wait", per_drive = thumb_mode == "drive", all = thumb_mode == "paused";
    // "drive": each drive's runner keeps it while it runs (the others wait behind it, as in the job queue); a new one
    // is the newest job not paused, one still shown before a lowered one
    QHash<QString, quint64> pick;
    if (per_drive) {
        for (const ThumbJob &j : std::as_const(jobs)) {
            if (j.paused)
                continue;
            auto cur = pick.constFind(j.drive);
            quint64 had = runners.value(j.drive);
            auto better = [&](const ThumbJob &a, const ThumbJob &b) {   // a before b
                if (a.lowered != b.lowered)
                    return !a.lowered;
                if ((a.id == had) != (b.id == had))
                    return a.id == had;
                return a.prio > b.prio;
            };
            if (cur == pick.constEnd() || better(j, *jobs.constFind(*cur)))
                pick[j.drive] = j.id;
        }
    }
    runners = pick;
    for (ThumbJob &j : jobs) {
        bool waiting = wait && jobs::Queue::instance()->busy(j.folder);   // another job on its drive
        bool behind = per_drive && !j.paused && pick.value(j.drive) != j.id;
        if (waiting == j.waiting && behind == j.behind && all == j.held_all)
            continue;
        j.waiting = waiting;
        j.behind = behind;
        j.held_all = all;
        QMutexLocker l(&run_lock);
        for (const QString &k : std::as_const(j.keys))
            if (auto req = pending.value(k))
                place(*req);
        l.unlock();
        jobs_said();
    }
}

void ThumbnailManager::check_jobs()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<quint64> timed_out;
    for (ThumbJob &j : jobs)
        if (j.lowered && now - j.lowered_at > qint64(job_minutes) * 60000)
            timed_out << j.id;
    update_holds();
    for (quint64 id : timed_out)   // left behind long enough: stopped, what it made kept
        end_job(id, false);
}

void ThumbnailManager::jobs_said()
{
    if (!said_timer.isActive())
        said_timer.start();
}

QString ThumbJob::title() const
{
    QString home = HOME();
    QString where = folder == home ? tx("Home") : folder.startsWith(home + "/") ? folder.mid(home.size() + 1) : folder;
    return tx("Thumbnails: %1").arg(where);
}

QString ThumbJob::state() const
{
    return paused     ? tx("Paused")
           : held_all ? tx("Paused (all thumbnailing)")
           : waiting  ? tx("Waiting for the drive")
           : behind   ? tx("Waiting for another thumbnail job on its drive")
           : lowered  ? tx("Lowered (folder left)")
                      : tx("Running");
}

void ThumbnailManager::cancel_for(const QObject *owner)
{
    if (!owner)
        return;
    int n = 0;
    QSet<quint64> its;   // owner's jobs: they end with it
    for (const ThumbJob &j : std::as_const(jobs))
        if (j.owner == owner)
            its << j.id;
    for (auto it = pending.begin(); it != pending.end();) {
        std::shared_ptr<ThumbRequest> req = it.value();
        bool was = req->owners.remove(owner);
        req->jobs.subtract(its);
        if (was && req->owners.isEmpty() && !req->shared) {
            it = pending.erase(it);
            drop(req);
            n += 1;
        } else {
            if (was) {
                QMutexLocker l(&run_lock);
                place(*req);
            }
            ++it;
        }
    }
    for (quint64 id : its) {
        job_ids.remove(qMakePair(owner, jobs.value(id).folder));
        jobs.remove(id);
    }
    for (auto it = stopped.begin(); it != stopped.end();)
        it = it->first == owner ? stopped.erase(it) : std::next(it);
    if (!its.isEmpty())
        jobs_said();
    if (n) {
        batch_total = std::max(batch_done, batch_total - n);
        schedule_progress();
    }
}

void ThumbnailManager::schedule_progress()
{
    if (!progress_timer.isActive())
        progress_timer.start();
}

void ThumbnailManager::emit_progress()
{
    if (pending.isEmpty())
        batch_total = batch_done = 0;
    Q_EMIT progress(batch_done, batch_total);
}

void ThumbnailManager::invalidate(const QString &path, bool disk)
{
    for (auto it = lru.begin(); it != lru.end();) {
        if (key_path(it->first) == path) {
            cache.remove(it->first);
            icons.remove(it->first);
            it = lru.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = icons.begin(); it != icons.end();)
        it = key_path(it.key()) == path ? icons.erase(it) : std::next(it);
    for (auto it = failed.begin(); it != failed.end();)
        it = key_path(*it) == path ? failed.erase(it) : std::next(it);
    for (int size : {128, 256, 512})
        if (disk)
            ::unlink(enc(join(join(APP_CACHE(), "folders"), QString("%1-%2.png").arg(md5(path)).arg(size))).constData());
    Q_EMIT updated(path);
}

void ThumbnailManager::clear_memory()
{
    lru.clear();
    cache.clear();
    icons.clear();
    failed.clear();
    scaled_cache.clear();
    scaled_order.clear();
}

void ThumbnailManager::cache_put(const QString &k, const QPixmap &pm)
{
    auto it = cache.find(k);
    if (it != cache.end()) {
        lru.erase(*it);
        cache.erase(it);
    }
    lru.emplace_back(k, pm);
    cache.insert(k, std::prev(lru.end()));
    while (int(lru.size()) > max_items) {
        icons.remove(lru.front().first);
        cache.remove(lru.front().first);
        lru.pop_front();
    }
}

void ThumbnailManager::on_done(const QString &k, const QString &path, const QImage &img, qint64 mtime, bool is_dir,
                               quint64 id)
{
    // (a cancelled one that was already being made still counts: it's cached; a newer request for it stays pending)
    if (auto p = pending.find(k); p != pending.end() && (*p)->id == id) {
        release_jobs(k, *p, true);
        pending.erase(p);
        batch_done += 1;
        schedule_progress();
    }
    if (img.isNull()) {
        if (!is_dir && QDateTime::currentSecsSinceEpoch() - mtime < 30) {
            // file probably still being written; try again shortly
            QTimer::singleShot(3000, this, [this, path]() { Q_EMIT updated(path); });
            return;
        }
        failed << k;
    } else {
        cache_put(k, QPixmap::fromImage(img));
    }
    Q_EMIT updated(path);
}

// ---------------------------------------------------------------- recursive pre-generation

RecursiveBuilder::RecursiveBuilder(const QString &root, int size, ThumbnailManager *manager, QObject *parent,
                                   int workers, bool folders_only, int depth, bool fresh)
    : QObject(parent), root(root), size(thumbs::bucket_for(size)), workers(workers), folders_only(folders_only),
      depth(depth), fresh(fresh), current(root)
{
    max_bytes = qint64(manager->max_file_mb) * 1000000;
    opts = {manager->folder_count, manager->folder_order, manager->folder_color, QString()};
    covers = manager->covers;
    for (auto it = manager->styles.begin(); it != manager->styles.end(); ++it) {
        if (!it.value().value("color").toString().isEmpty())
            colors[it.key()] = it.value().value("color").toString();
        if (!it.value().value("previews", true).toBool())
            no_previews << it.key();
    }
}

void RecursiveBuilder::emit_progress(bool force)
{
    qint64 now = monotonic_ms();
    int d, t;
    bool s;
    QString c;
    {
        QMutexLocker g(&lock);
        if (!force && now - last_emit < 100)
            return;
        last_emit = now;
        d = done;
        t = total;
        s = scanning;
        c = current;
    }
    Q_EMIT progress(d, t, s, c);
    if (task) {
        QString name = basename(rstrip(root, '/'));
        task->report(d, s ? 0 : t,
                     QString("%1 — %2 of %3%4").arg(name.isEmpty() ? root : name, group_digits(d), group_digits(t),
                                                    s ? QString("+, scanning…") : QString()));
    }
}

void RecursiveBuilder::one(char kind, const QString &path)
{
    while (task && task->paused() && !stop && !task->cancelled)   // the job is paused: so are its workers
        QThread::msleep(50);
    if (task && task->cancelled)
        stop = true;
    if (stop)   // skipped: not counted as done
        return;
    {
        struct stat st;
        if (stat_(path, st)) {
            if (kind == 'd') {
                thumbs::FolderOpts o = opts;
                o.cover = covers.value(path);
                o.color = colors.value(path, opts.color);
                thumbs::folder_thumb(path, st.st_mtime, size, o, fresh);
            } else if (st.st_size <= max_bytes || is_video(path)) {
                thumbs::file_thumb(path, st.st_mtime, size, fresh);
                if (size != 128)   // mosaic tiles use the small size
                    thumbs::file_thumb(path, st.st_mtime, 128, fresh);
            }
        }
    }
    {
        QMutexLocker g(&lock);
        done += 1;
        current = path;
    }
    emit_progress();
}

bool RecursiveBuilder::run(Task *job)
{
    task = job;
    QSet<QString> exts = image_exts();
    exts.unite(VIDEO_EXTS);
    QThreadPool workers_pool;
    workers_pool.setMaxThreadCount(workers);
    // The walk queues everything it finds without waiting for the workers: listing a tree takes seconds where making
    // its previews takes minutes, so the total is known early (a cap on queued work held the walk to the workers' pace,
    // "scanning" until nearly the end). The queue is a path per item.
    auto submit = [&](char kind, const QString &path) {
        if (stop)
            return false;
        {
            QMutexLocker g(&lock);
            total += 1;
        }
        workers_pool.start([this, kind, path]() { one(kind, path); });
        return true;
    };
    QString top = rstrip(root, '/');
    if (thumbs::in_app_cache(top)) {   // Kestrel's own cache: no previews made in it (ThumbnailManager::get)
        task = nullptr;
        return false;
    }
    walk(root, [&](const QString &dir, QStringList &dirs, QStringList &files) {
        if (task && task->cancelled)
            stop = true;
        if (stop)
            return false;
        QStringList keep;
        int level = rstrip(dir, '/') == top ? 0 : QStringView(dir).mid(top.size()).count('/');
        if (depth < 0 || level < depth)
            for (const QString &d : dirs)
                if (!d.startsWith('.') && !thumbs::in_app_cache(join(dir, d)))
                    keep << d;
        natural_sort(keep);
        dirs = keep;
        natural_sort(files);
        if (!folders_only && (depth < 0 || level == 0))   // files' thumbnails: the whole tree, or just root's
            for (const QString &f : files)
                if (!f.startsWith('.') && exts.contains(ext_of(f)))
                    if (!submit('f', join(dir, f)))
                        return false;
        if (stop || (!no_previews.contains(dir) && !submit('d', dir)))
            return false;
        emit_progress();
        return true;
    }, true, folders_only);   // folder previews only: just the folders, like tree -d (each preview lists its own)
    {
        QMutexLocker g(&lock);
        scanning = false;
    }
    emit_progress(true);
    if (stop)
        workers_pool.clear();
    workers_pool.waitForDone();
    emit_progress(true);
    task = nullptr;
    return stop;
}
