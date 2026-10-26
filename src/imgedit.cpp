#include "imgedit.h"

#include "metadata.h"
#include "proc.h"
#include "util.h"

#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QMap>
#include <QSet>
#include <QTransform>

#include <fcntl.h>
#include <unistd.h>

using namespace util;

namespace imgedit {

// ---------------------------------------------------------------- orientations

namespace {

// What each EXIF orientation does to the stored image to show it, as a 2×2 matrix on (x, y), y pointing down
struct M {
    int a, b, c, d;
    bool operator==(const M &o) const { return a == o.a && b == o.b && c == o.c && d == o.d; }
};
const M ORIENT[9] = {{1, 0, 0, 1},  {1, 0, 0, 1},  {-1, 0, 0, 1}, {-1, 0, 0, -1}, {1, 0, 0, -1},
                     {0, 1, 1, 0},  {0, -1, 1, 0}, {0, -1, -1, 0}, {0, 1, -1, 0}};
M mul(const M &x, const M &y)
{
    return {x.a * y.a + x.b * y.c, x.a * y.b + x.b * y.d, x.c * y.a + x.d * y.c, x.c * y.b + x.d * y.d};
}

QByteArray tag_value(int v, bool le)
{
    QByteArray b(2, 0);
    b[le ? 0 : 1] = char(v);
    return b;
}

bool is_jpeg(const QString &path) { return ext_of(path) == ".jpg" || ext_of(path) == ".jpeg"; }

QByteArray writer_format(const QString &path)
{
    QByteArray ext = ext_of(path).mid(1).toLatin1();
    return QImageWriter::supportedImageFormats().contains(ext) ? ext : QByteArray();
}

// A copy of path with `bytes` written over it at `at` (an orientation tag's value), streamed into fd
void copy_patched(const QString &path, qint64 at, const QByteArray &bytes, int fd)
{
    int in = ::open(enc(path).constData(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        throw_errno(path);
    struct Close {
        int fd;
        ~Close() { ::close(fd); }
    } guard{in};
    QByteArray buf(1 << 20, 0);
    qint64 pos = 0;
    for (;;) {
        ssize_t n = ::read(in, buf.data(), buf.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw_errno(path);
        if (n == 0)
            break;
        for (int i = 0; i < bytes.size(); ++i)
            if (at + i >= pos && at + i < pos + n)
                buf[int(at + i - pos)] = bytes[i];
        write_all(fd, buf.constData(), n, path);
        pos += n;
    }
}

// A JPEG's APP1 segments (EXIF, XMP), whole, with the EXIF orientation tag set to 1: for an edited copy, whose pixels
// are already turned. (Its EXIF thumbnail and pixel dimensions stay the original's, as with most editors.)
QByteArray app1_segments(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.read(2) != "\xff\xd8")
        return QByteArray();
    metadata::OrientationTag tag = metadata::orientation_tag(path);
    QByteArray out;
    for (;;) {
        qint64 at = f.pos();
        QByteArray h = f.read(4);
        if (h.size() < 4 || uchar(h[0]) != 0xFF)
            break;
        uchar marker = uchar(h[1]);
        int len = (uchar(h[2]) << 8) | uchar(h[3]);
        if (marker == 0xDA || marker == 0xD9 || len < 2)
            break;
        QByteArray body = f.read(len - 2);
        if (marker == 0xE1) {
            QByteArray seg = h + body;
            if (tag.offset >= at && tag.offset + 2 <= at + seg.size())
                seg.replace(int(tag.offset - at), 2, tag_value(1, tag.little_endian));
            out += seg;
        }
    }
    return out;
}

// Qt's JPEG with `app1` put back after its start (and its JFIF segment)
QByteArray with_app1(const QByteArray &jpeg, const QByteArray &app1)
{
    if (app1.isEmpty() || !jpeg.startsWith("\xff\xd8"))
        return jpeg;
    int at = 2;
    if (jpeg.size() > 6 && uchar(jpeg[2]) == 0xFF && uchar(jpeg[3]) == 0xE0)
        at = 4 + ((uchar(jpeg[4]) << 8) | uchar(jpeg[5]));
    return jpeg.left(at) + app1 + jpeg.mid(at);
}

}  // namespace

int compose(int orientation, int rotation, bool flip)
{
    if (orientation < 1 || orientation > 8)
        orientation = 1;
    M u = ORIENT[flip ? 2 : 1];
    for (int i = 0; i < (rotation % 360 + 360) % 360 / 90; ++i)
        u = mul(ORIENT[6], u);   // a quarter turn clockwise
    M n = mul(u, ORIENT[orientation]);
    for (int i = 1; i <= 8; ++i)
        if (ORIENT[i] == n)
            return i;
    return 1;
}

// ---------------------------------------------------------------- rotation

QString rotation_method(const QString &path)
{
    static const QSet<QString> LOSSLESS = {".png", ".bmp", ".ppm", ".pgm", ".pbm"};
    if (metadata::orientation_tag(path).offset >= 0)
        return "tag";
    if (is_jpeg(path) && which("exiftool"))
        return "exiftool";
    if (LOSSLESS.contains(ext_of(path)) && can_write(path))
        return "pixels";
    return QString();
}

void save_rotation(const QString &path, int rotation, bool flip)
{
    QString how = rotation_method(path);
    if (how == "tag") {
        metadata::OrientationTag tag = metadata::orientation_tag(path);
        int v = compose(tag.value, rotation, flip);
        if (v == tag.value)
            return;
        write_parts(path, [&](int fd) { copy_patched(path, tag.offset, tag_value(v, tag.little_endian), fd); }, false,
                    true);
    } else if (how == "exiftool") {
        // exiftool writes its own copy and renames it over the file (not fsynced: the next best thing, see CLAUDE.md)
        int v = compose(1, rotation, flip);
        auto r = proc::run({"exiftool", "-q", "-overwrite_original", "-Orientation#=" + QString::number(v), path}, 60000);
        if (r.rc != 0)
            throw Error(QString("exiftool couldn't save the rotation of %1: %2")
                            .arg(basename(path), QString::fromUtf8(r.err).trimmed()));
    } else if (how == "pixels") {
        save_edit(path, path, Edit{rotation, flip, QRect(), QSize()});
    } else {
        throw Error(QString("%1: Kestrel can't save a rotation in this format without losing quality.").arg(basename(path)));
    }
}

static const QMap<int, QStringList> &jpegtran_ops()
{
    // what turns a JPEG's stored pixels into what EXIF orientation n shows
    static const QMap<int, QStringList> OPS = {{2, {"-flip", "horizontal"}}, {3, {"-rotate", "180"}},
                                               {4, {"-flip", "vertical"}},   {5, {"-transpose"}},
                                               {6, {"-rotate", "90"}},       {7, {"-transverse"}},
                                               {8, {"-rotate", "270"}}};
    return OPS;
}

// A JPEG's pixels turned as orientation n shows them (jpegtran, lossless; trim: the odd edge cut off), its metadata
// kept with the orientation tag set to 1; replacing the file (part file, fsynced)
static void turn_jpeg(const QString &path, int n, bool trim)
{
    QByteArray data;
    if (n == 1) {
        data = read_file(path);   // already as shown: only the tag
    } else {
        QStringList argv = {"jpegtran", "-copy", "all", trim ? "-trim" : "-perfect"};
        auto r = proc::run(argv + jpegtran_ops().value(n) + QStringList{path}, 120000);
        if (r.rc != 0 || r.out.isEmpty()) {
            QString err = QString::fromUtf8(r.err).trimmed();
            if (err.contains("perfect", Qt::CaseInsensitive))
                throw NotWholeBlocks(QString("%1: its width or height isn't a whole number of JPEG blocks, so turning "
                                             "it exactly would lose a strip at the edge")
                                         .arg(basename(path)));
            throw Error(QString("Couldn't turn %1: %2").arg(basename(path), err.isEmpty() ? QString("jpegtran failed") : err));
        }
        data = r.out;
    }
    metadata::OrientationTag t = metadata::orientation_tag_in(data);   // copied with -copy all
    if (t.offset >= 0)
        data.replace(int(t.offset), 2, tag_value(1, t.little_endian));
    write_parts(path, [&](int fd) { write_all(fd, data.constData(), data.size(), path); }, false, true);
}

static bool lossless_webp(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QByteArray head = f.read(64);
    return head.startsWith("RIFF") && head.mid(8, 4) == "WEBP" && head.contains("VP8L");
}

QString rotate_method(const QString &path)
{
    static const QSet<QString> LOSSLESS = {".png", ".bmp", ".ppm", ".pgm", ".pbm", ".tif", ".tiff"};
    static const QSet<QString> LOSSY = {".webp", ".heic", ".heif", ".hif", ".avif", ".jxl"};
    QString ext = ext_of(path);
    if (is_jpeg(path))
        return which("jpegtran") ? "jpegtran" : rotation_method(path).isEmpty() ? QString() : "tag";
    if (!can_write(path))
        return QString();
    if (LOSSLESS.contains(ext) || (ext == ".webp" && lossless_webp(path)))
        return "pixels";
    return LOSSY.contains(ext) ? "reencode" : QString();
}

void rotate_file(const QString &path, int rotation, bool flip, const QString &how, bool reencode)
{
    QString method = rotate_method(path);
    if (method == "jpegtran" && how != "tag") {
        metadata::OrientationTag tag = metadata::orientation_tag(path);
        turn_jpeg(path, compose(tag.offset >= 0 ? tag.value : 1, rotation, flip), how == "trim");
    } else if (method == "jpegtran" || method == "tag") {
        save_rotation(path, rotation, flip);
    } else if (method == "pixels") {
        save_edit(path, path, {Edit{rotation, flip, QRect(), QSize()}}, ext_of(path) == ".webp" ? 100 : -1, true);
    } else if (method == "reencode") {
        if (!reencode)
            throw Error(QString("%1: turning it means saving it again, which loses a little quality.").arg(basename(path)));
        save_edit(path, path, {Edit{rotation, flip, QRect(), QSize()}}, 92, true);
    } else {
        throw Error(QString("%1: Kestrel can't turn images in this format.").arg(basename(path)));
    }
}

bool can_straighten(const QString &path)
{
    metadata::OrientationTag tag = metadata::orientation_tag(path);
    return is_jpeg(path) && tag.offset >= 0 && tag.value > 1 && tag.value <= 8;
}

void straighten(const QString &path)
{
    metadata::OrientationTag tag = metadata::orientation_tag(path);
    if (!can_straighten(path))
        throw Error(basename(path) + ": not a JPEG turned by its orientation tag.");
    if (!which("jpegtran"))
        throw Error("Straightening a JPEG without losing quality needs jpegtran (package libjpeg-turbo-progs).");
    static const QMap<int, QStringList> OPS = {{2, {"-flip", "horizontal"}}, {3, {"-rotate", "180"}},
                                               {4, {"-flip", "vertical"}},   {5, {"-transpose"}},
                                               {6, {"-rotate", "90"}},       {7, {"-transverse"}},
                                               {8, {"-rotate", "270"}}};
    auto r = proc::run(QStringList{"jpegtran", "-copy", "all", "-perfect"} + OPS.value(tag.value) + QStringList{path}, 120000);
    if (r.rc != 0 || r.out.isEmpty()) {
        QString err = QString::fromUtf8(r.err).trimmed();
        if (err.contains("perfect", Qt::CaseInsensitive))
            err = "its width or height isn't a whole number of JPEG blocks, so turning it would lose a strip at the edge";
        throw Error(QString("Couldn't straighten %1: %2").arg(basename(path), err.isEmpty() ? QString("jpegtran failed") : err));
    }
    QByteArray data = r.out;
    metadata::OrientationTag t2 = metadata::orientation_tag_in(data);   // copied with -copy all
    if (t2.offset >= 0)
        data.replace(int(t2.offset), 2, tag_value(1, t2.little_endian));
    write_parts(path, [&](int fd) { write_all(fd, data.constData(), data.size(), path); }, false, true);
}

// ---------------------------------------------------------------- crop and resize

QImage apply(const QImage &src, const Edit &e)
{
    QImage img = e.flip ? src.mirrored(true, false) : src;
    int rot = (e.rotation % 360 + 360) % 360;
    if (rot)
        img = img.transformed(QTransform().rotate(rot));
    if (!e.crop.isEmpty())
        img = img.copy(e.crop.intersected(img.rect()));
    if (!e.size.isEmpty() && e.size != img.size())
        img = img.scaled(e.size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return img;
}

bool can_write(const QString &path) { return !writer_format(path).isEmpty(); }

QString new_name(const QString &src, const QString &what)
{
    auto [stem, ext] = split_ext(basename(src));
    if (!can_write(src))
        ext = ".jpg";
    for (int n = 1;; ++n) {
        QString p = join(dirname(src), QString("%1 (%2%3)%4").arg(stem, what, n > 1 ? " " + QString::number(n) : QString(), ext));
        if (!lexists(p))
            return p;
    }
}

QImage preview(const QImage &src, QSize full, const QList<Edit> &steps, QSize *real)
{
    QImage img = src;
    QSize r = full.isEmpty() ? src.size() : full;
    for (const Edit &e : steps) {
        img = apply(img, Edit{e.rotation, e.flip, QRect(), QSize()});
        if ((e.rotation % 180 + 180) % 180)
            r.transpose();
        double k = r.width() ? double(img.width()) / r.width() : 1;   // the copy's pixels per real pixel
        if (!e.crop.isEmpty()) {
            QRect c = e.crop.intersected(QRect(QPoint(0, 0), r));
            img = img.copy(QRect(qRound(c.x() * k), qRound(c.y() * k), std::max(1, qRound(c.width() * k)),
                                 std::max(1, qRound(c.height() * k))));
            r = c.size();
        }
        if (!e.size.isEmpty()) {
            QSize s(std::max(1, qRound(e.size.width() * k)), std::max(1, qRound(e.size.height() * k)));
            img = img.scaled(s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            r = e.size;
        }
    }
    if (real)
        *real = r;
    return img;
}

// An image Qt wrote (`data`, a file of type ext) with src's EXIF, XMP and IPTC copied in, its orientation tag set to 1
// (its pixels are already turned): exiftool on a copy in Kestrel's cache. Without exiftool, or if it fails: as it was.
static QByteArray with_meta(const QString &src, const QByteArray &data, const QString &ext)
{
    if (!which("exiftool"))
        return data;
    QString dir = join(APP_CACHE(), "edit-meta");
    QString tmp = join(dir, part_name() + ext);
    QByteArray out = data;
    try {
        makedirs(dir, true);
        write_text(tmp, data, true);
        auto r = proc::run({"exiftool", "-q", "-q", "-m", "-overwrite_original", "-tagsFromFile", src, "-all:all",
                            "-Orientation#=1", tmp},
                           120000);
        if (r.rc == 0)
            out = read_file(tmp);
    } catch (const OSError &) {
    }
    ::unlink(enc(tmp).constData());
    return out.isEmpty() ? data : out;
}

QByteArray with_metadata(const QString &src, const QByteArray &data, const QString &ext)
{
    bool jpeg_out = ext == ".jpg" || ext == ".jpeg";
    if (is_jpeg(src) && jpeg_out && !which("exiftool"))
        return with_app1(data, app1_segments(src));
    return with_meta(src, data, ext);
}

void save_edit(const QString &src, const QString &dst, const QList<Edit> &steps, int quality, bool keep_meta)
{
    bool replace = dst == src;
    QByteArray fmt = writer_format(dst);
    if (fmt.isEmpty())
        throw Error(QString("Kestrel can't write %1 files.").arg(ext_of(dst).mid(1).toUpper()));
    QImageReader reader(src);
    reader.setAutoTransform(true);
    QImage img = reader.read();
    if (img.isNull())
        throw Error(QString("Couldn't read %1: %2").arg(basename(src), reader.errorString()));
    for (const Edit &e : steps)
        img = apply(img, e);   // keeps a PNG's text chunks (generation prompts), which the writer writes again
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    QImageWriter w(&buf, fmt);
    if (quality >= 0)
        w.setQuality(quality);
    else if (fmt == "jpg" || fmt == "jpeg" || fmt == "webp")
        w.setQuality(95);
    if (!w.write(img))
        throw Error(QString("Couldn't write %1: %2").arg(basename(dst), w.errorString()));
    QByteArray data = buf.data();
    if (is_jpeg(src) && (fmt == "jpg" || fmt == "jpeg"))
        data = with_app1(data, app1_segments(src));
    else if (keep_meta)
        data = with_metadata(src, data, ext_of(dst));
    write_parts(dst, [&](int fd) { write_all(fd, data.constData(), data.size(), dst); }, !replace, replace);
}

}  // namespace imgedit
