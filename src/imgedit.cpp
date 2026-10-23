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

void save_edit(const QString &src, const QString &dst, const QList<Edit> &steps)
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
    if (fmt == "jpg" || fmt == "jpeg" || fmt == "webp")
        w.setQuality(95);
    if (!w.write(img))
        throw Error(QString("Couldn't write %1: %2").arg(basename(dst), w.errorString()));
    QByteArray data = buf.data();
    if (is_jpeg(src) && (fmt == "jpg" || fmt == "jpeg"))
        data = with_app1(data, app1_segments(src));
    write_parts(dst, [&](int fd) { write_all(fd, data.constData(), data.size(), dst); }, !replace, replace);
}

}  // namespace imgedit
