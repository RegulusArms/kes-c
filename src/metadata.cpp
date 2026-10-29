#include "metadata.h"

#include "convert.h"
#include "proc.h"
#include "util.h"

#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

#include <cmath>
#include <sys/stat.h>
#include <mutex>

using namespace util;

namespace metadata {

// ---------------------------------------------------------------- reading EXIF and PNG text

namespace {

struct ExifVal {
    int type = 0;
    QByteArray raw;      // BYTE / UNDEFINED / ASCII bytes
    QList<double> nums;  // numeric types (rationals as num/den)
    QString text() const;
    bool numeric() const { return !nums.isEmpty(); }
    double num() const { return nums.isEmpty() ? 0 : nums.first(); }
};

QString fmt_num(double v)
{
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15)
        return QString::number(qint64(v));
    return QString::number(v, 'g', 10);
}

QString ExifVal::text() const
{
    if (type == 2) {
        QByteArray s = raw;
        while (s.endsWith('\0'))
            s.chop(1);
        return QString::fromUtf8(s);
    }
    if (type == 1 || type == 6 || type == 7)
        return txn("(binary, %Ln byte(s))", raw.size());
    if (nums.size() == 1)
        return fmt_num(nums.first());
    QStringList parts;
    for (double v : nums)
        parts << fmt_num(v);
    return "(" + parts.join(", ") + ")";
}

using Ifd = QMap<int, ExifVal>;

struct Exif {
    Ifd ifd0, exif, gps;
    bool valid = false;
};

class TiffReader {
public:
    explicit TiffReader(const QByteArray &d) : d(d) {}
    Exif parse()
    {
        Exif out;
        if (d.size() < 8)
            return out;
        if (d.startsWith("II"))
            le = true;
        else if (d.startsWith("MM"))
            le = false;
        else
            return out;
        if (u16(2) != 42)
            return out;
        out.valid = true;
        out.ifd0 = ifd(u32(4));
        if (out.ifd0.contains(0x8769))
            out.exif = ifd(quint32(out.ifd0[0x8769].num()));
        if (out.ifd0.contains(0x8825))
            out.gps = ifd(quint32(out.ifd0[0x8825].num()));
        return out;
    }

private:
    const QByteArray &d;
    bool le = true;

    bool ok(qint64 off, qint64 n) const { return off >= 0 && n >= 0 && off + n <= d.size(); }
    quint16 u16(qint64 o) const
    {
        if (!ok(o, 2))
            return 0;
        auto b = reinterpret_cast<const uchar *>(d.constData() + o);
        return le ? quint16(b[0] | (b[1] << 8)) : quint16((b[0] << 8) | b[1]);
    }
    quint32 u32(qint64 o) const
    {
        if (!ok(o, 4))
            return 0;
        auto b = reinterpret_cast<const uchar *>(d.constData() + o);
        return le ? quint32(b[0] | (b[1] << 8) | (b[2] << 16) | (quint32(b[3]) << 24))
                  : quint32((quint32(b[0]) << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
    }

    Ifd ifd(quint32 off) const
    {
        Ifd out;
        if (!ok(off, 2))
            return out;
        int n = u16(off);
        static const int sizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
        for (int i = 0; i < n && i < 1000; ++i) {
            qint64 e = off + 2 + qint64(i) * 12;
            if (!ok(e, 12))
                break;
            int tag = u16(e), type = u16(e + 2);
            quint32 count = u32(e + 4);
            if (type < 1 || type > 12 || count > 10000000)
                continue;
            qint64 size = qint64(sizes[type]) * count;
            qint64 vo = size <= 4 ? e + 8 : qint64(u32(e + 8));
            if (!ok(vo, size))
                continue;
            ExifVal v;
            v.type = type;
            if (type == 1 || type == 2 || type == 6 || type == 7) {
                v.raw = d.mid(vo, size);
                if (type == 1 && count <= 4)
                    for (quint32 k = 0; k < count; ++k)
                        v.nums << uchar(d[vo + k]);
                if (type == 1 && count > 4)
                    v.type = 7;
            } else {
                for (quint32 k = 0; k < std::min<quint32>(count, 256); ++k) {
                    qint64 p = vo + qint64(k) * sizes[type];
                    switch (type) {
                    case 3: v.nums << u16(p); break;
                    case 4: v.nums << u32(p); break;
                    case 8: v.nums << qint16(u16(p)); break;
                    case 9: v.nums << qint32(u32(p)); break;
                    case 5: {
                        quint32 a = u32(p), b = u32(p + 4);
                        v.nums << (b ? double(a) / b : 0.0);
                        break;
                    }
                    case 10: {
                        qint32 a = qint32(u32(p)), b = qint32(u32(p + 4));
                        v.nums << (b ? double(a) / b : 0.0);
                        break;
                    }
                    case 11: {
                        quint32 bits = u32(p);
                        float f;
                        memcpy(&f, &bits, 4);
                        v.nums << f;
                        break;
                    }
                    case 12: {
                        quint64 bits = le ? (quint64(u32(p + 4)) << 32) | u32(p) : (quint64(u32(p)) << 32) | u32(p + 4);
                        double f;
                        memcpy(&f, &bits, 8);
                        v.nums << f;
                        break;
                    }
                    }
                }
            }
            out.insert(tag, v);
        }
        return out;
    }
};

struct FileMeta {
    QByteArray exif_blob;                   // TIFF-structured EXIF data
    qint64 exif_offset = -1;                // where exif_blob starts in the file (JPEG and TIFF files)
    QList<QPair<QString, QString>> text;    // PNG text chunks, in order
};

quint32 be32(const char *p)
{
    auto b = reinterpret_cast<const uchar *>(p);
    return (quint32(b[0]) << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}

quint32 le32(const char *p)
{
    auto b = reinterpret_cast<const uchar *>(p);
    return quint32(b[0] | (b[1] << 8) | (b[2] << 16) | (quint32(b[3]) << 24));
}

QByteArray inflate(const QByteArray &z)
{
    QByteArray with_size(4, 0);
    quint32 guess = quint32(std::min<qint64>(qint64(z.size()) * 8 + 1024, 64 << 20));
    with_size[0] = char(guess >> 24);
    with_size[1] = char(guess >> 16);
    with_size[2] = char(guess >> 8);
    with_size[3] = char(guess);
    return qUncompress(with_size + z);
}

QByteArray strip_exif_header(const QByteArray &b) { return b.startsWith(QByteArray("Exif\0\0", 6)) ? b.mid(6) : b; }

FileMeta read_file_meta(QIODevice &f, qint64 max_tiff = 32 << 20)   // f: open for reading
{
    FileMeta m;
    QByteArray head = f.read(16);
    if (head.startsWith("\x89PNG\r\n\x1a\n")) {
        f.seek(8);
        for (;;) {
            QByteArray h = f.read(8);
            if (h.size() < 8)
                break;
            quint32 len = be32(h.constData());
            QByteArray type = h.mid(4, 4);
            if (type == "IEND" || len > (256u << 20))
                break;
            if (type == "tEXt" || type == "zTXt" || type == "iTXt" || type == "eXIf") {
                QByteArray data = f.read(len);
                f.seek(f.pos() + 4);
                if (type == "eXIf") {
                    m.exif_blob = strip_exif_header(data);
                    continue;
                }
                int nul = data.indexOf('\0');
                if (nul < 0)
                    continue;
                QString key = QString::fromLatin1(data.left(nul));
                QString value;
                if (type == "tEXt") {
                    value = QString::fromLatin1(data.mid(nul + 1));
                } else if (type == "zTXt") {
                    value = QString::fromLatin1(inflate(data.mid(nul + 2)));
                } else {
                    if (data.size() < nul + 3)
                        continue;
                    bool compressed = data[nul + 1] != 0;
                    int lang_end = data.indexOf('\0', nul + 3);
                    int tk_end = lang_end < 0 ? -1 : data.indexOf('\0', lang_end + 1);
                    if (tk_end < 0)
                        continue;
                    QByteArray t = data.mid(tk_end + 1);
                    value = QString::fromUtf8(compressed ? inflate(t) : t);
                }
                m.text << qMakePair(key, value);
            } else {
                f.seek(f.pos() + qint64(len) + 4);
            }
        }
    } else if (head.startsWith("\xff\xd8")) {
        f.seek(2);
        for (;;) {
            QByteArray h = f.read(4);
            if (h.size() < 4 || uchar(h[0]) != 0xFF)
                break;
            uchar marker = uchar(h[1]);
            if (marker == 0xDA || marker == 0xD9)
                break;
            int len = (uchar(h[2]) << 8) | uchar(h[3]);
            if (len < 2)
                break;
            if (marker == 0xE1 && m.exif_blob.isEmpty()) {
                QByteArray data = f.read(len - 2);
                if (data.startsWith(QByteArray("Exif\0\0", 6))) {
                    m.exif_blob = data.mid(6);
                    m.exif_offset = f.pos() - data.size() + 6;
                }
            } else {
                f.seek(f.pos() + len - 2);
            }
        }
    } else if (head.startsWith("RIFF") && head.mid(8, 4) == "WEBP") {
        f.seek(12);
        for (;;) {
            QByteArray h = f.read(8);
            if (h.size() < 8)
                break;
            quint32 len = le32(h.constData() + 4);
            if (h.left(4) == "EXIF") {
                m.exif_blob = strip_exif_header(f.read(len));
                break;
            }
            f.seek(f.pos() + len + (len & 1));
        }
    } else if (head.startsWith("II*\0") || head.startsWith("MM\0*")) {
        f.seek(0);
        m.exif_blob = f.read(max_tiff);
        m.exif_offset = 0;
    }
    return m;
}

FileMeta read_file_meta(const QString &path, qint64 max_tiff = 32 << 20)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return FileMeta();
    return read_file_meta(f, max_tiff);
}

const QHash<int, QString> &exif_tag_names()
{
    static const QHash<int, QString> t = {
        {0x0100, "ImageWidth"}, {0x0101, "ImageLength"}, {0x0102, "BitsPerSample"}, {0x0103, "Compression"},
        {0x0106, "PhotometricInterpretation"}, {0x010E, "ImageDescription"}, {0x010F, "Make"}, {0x0110, "Model"},
        {0x0112, "Orientation"}, {0x0115, "SamplesPerPixel"}, {0x011A, "XResolution"}, {0x011B, "YResolution"},
        {0x011C, "PlanarConfiguration"}, {0x0128, "ResolutionUnit"}, {0x0131, "Software"}, {0x0132, "DateTime"},
        {0x013B, "Artist"}, {0x013E, "WhitePoint"}, {0x013F, "PrimaryChromaticities"}, {0x0201, "JpegIFOffset"},
        {0x0202, "JpegIFByteCount"}, {0x0211, "YCbCrCoefficients"}, {0x0213, "YCbCrPositioning"},
        {0x0214, "ReferenceBlackWhite"}, {0x02BC, "XMLPacket"}, {0x4746, "Rating"}, {0x4749, "RatingPercent"},
        {0x8298, "Copyright"}, {0x829A, "ExposureTime"}, {0x829D, "FNumber"}, {0x83BB, "IPTCNAA"},
        {0x8769, "ExifOffset"}, {0x8773, "InterColorProfile"}, {0x8822, "ExposureProgram"},
        {0x8824, "SpectralSensitivity"}, {0x8825, "GPSInfo"}, {0x8827, "ISOSpeedRatings"},
        {0x8830, "SensitivityType"}, {0x8832, "RecommendedExposureIndex"}, {0x9000, "ExifVersion"},
        {0x9003, "DateTimeOriginal"}, {0x9004, "DateTimeDigitized"}, {0x9010, "OffsetTime"},
        {0x9011, "OffsetTimeOriginal"}, {0x9012, "OffsetTimeDigitized"}, {0x9101, "ComponentsConfiguration"},
        {0x9102, "CompressedBitsPerPixel"}, {0x9201, "ShutterSpeedValue"}, {0x9202, "ApertureValue"},
        {0x9203, "BrightnessValue"}, {0x9204, "ExposureBiasValue"}, {0x9205, "MaxApertureValue"},
        {0x9206, "SubjectDistance"}, {0x9207, "MeteringMode"}, {0x9208, "LightSource"}, {0x9209, "Flash"},
        {0x920A, "FocalLength"}, {0x9214, "SubjectArea"}, {0x927C, "MakerNote"}, {0x9286, "UserComment"},
        {0x9290, "SubsecTime"}, {0x9291, "SubsecTimeOriginal"}, {0x9292, "SubsecTimeDigitized"},
        {0x9C9B, "XPTitle"}, {0x9C9C, "XPComment"}, {0x9C9D, "XPAuthor"}, {0x9C9E, "XPKeywords"},
        {0x9C9F, "XPSubject"}, {0xA000, "FlashPixVersion"}, {0xA001, "ColorSpace"}, {0xA002, "ExifImageWidth"},
        {0xA003, "ExifImageHeight"}, {0xA004, "RelatedSoundFile"}, {0xA005, "ExifInteroperabilityOffset"},
        {0xA20E, "FocalPlaneXResolution"}, {0xA20F, "FocalPlaneYResolution"}, {0xA210, "FocalPlaneResolutionUnit"},
        {0xA215, "ExposureIndex"}, {0xA217, "SensingMethod"}, {0xA300, "FileSource"}, {0xA301, "SceneType"},
        {0xA302, "CFAPattern"}, {0xA401, "CustomRendered"}, {0xA402, "ExposureMode"}, {0xA403, "WhiteBalance"},
        {0xA404, "DigitalZoomRatio"}, {0xA405, "FocalLengthIn35mmFilm"}, {0xA406, "SceneCaptureType"},
        {0xA407, "GainControl"}, {0xA408, "Contrast"}, {0xA409, "Saturation"}, {0xA40A, "Sharpness"},
        {0xA40B, "DeviceSettingDescription"}, {0xA40C, "SubjectDistanceRange"}, {0xA420, "ImageUniqueID"},
        {0xA430, "CameraOwnerName"}, {0xA431, "BodySerialNumber"}, {0xA432, "LensSpecification"},
        {0xA433, "LensMake"}, {0xA434, "LensModel"}, {0xA435, "LensSerialNumber"}, {0xA500, "Gamma"},
        {0xC4A5, "PrintImageMatching"}};
    return t;
}

const QHash<int, QString> &gps_tag_names()
{
    static const QHash<int, QString> t = {
        {0, "GPSVersionID"}, {1, "GPSLatitudeRef"}, {2, "GPSLatitude"}, {3, "GPSLongitudeRef"},
        {4, "GPSLongitude"}, {5, "GPSAltitudeRef"}, {6, "GPSAltitude"}, {7, "GPSTimeStamp"},
        {8, "GPSSatellites"}, {9, "GPSStatus"}, {10, "GPSMeasureMode"}, {11, "GPSDOP"}, {12, "GPSSpeedRef"},
        {13, "GPSSpeed"}, {14, "GPSTrackRef"}, {15, "GPSTrack"}, {16, "GPSImgDirectionRef"},
        {17, "GPSImgDirection"}, {18, "GPSMapDatum"}, {19, "GPSDestLatitudeRef"}, {20, "GPSDestLatitude"},
        {21, "GPSDestLongitudeRef"}, {22, "GPSDestLongitude"}, {23, "GPSDestBearingRef"}, {24, "GPSDestBearing"},
        {25, "GPSDestDistanceRef"}, {26, "GPSDestDistance"}, {27, "GPSProcessingMethod"},
        {28, "GPSAreaInformation"}, {29, "GPSDateStamp"}, {30, "GPSDifferential"}, {31, "GPSHPositioningError"}};
    return t;
}

QString tag_name(const QHash<int, QString> &names, int tag)
{
    return names.value(tag, QString("0x%1").arg(tag, 0, 16));
}

QString mode_of(QImage::Format f)
{
    switch (f) {
    case QImage::Format_Mono:
    case QImage::Format_MonoLSB: return "1";
    case QImage::Format_Indexed8: return "P";
    case QImage::Format_Grayscale8: return "L";
    case QImage::Format_Grayscale16: return "I;16";
    case QImage::Format_RGB32:
    case QImage::Format_RGB888:
    case QImage::Format_RGBX8888: return "RGB";
    case QImage::Format_Invalid: return QString();
    default: return QImage::toPixelFormat(f).alphaUsage() == QPixelFormat::UsesAlpha ? "RGBA" : "RGB";
    }
}

// When an MP4 / MOV / 3GP file was recorded: its movie header's creation time (seconds since 1904, UTC). Invalid when
// there's none, or it's 0 (cameras that don't keep the time).
QDateTime movie_created(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QDateTime();
    // the box `type` among the boxes in [from, to): its contents' start and end
    auto find = [&f](qint64 from, qint64 to, const char *type, qint64 *body, qint64 *body_end) {
        for (qint64 pos = from; pos + 8 <= to;) {
            f.seek(pos);
            QByteArray h = f.read(8);
            if (h.size() < 8)
                return false;
            quint64 size = be32(h.constData());
            qint64 hdr = 8;
            if (size == 1) {
                QByteArray ext = f.read(8);
                if (ext.size() < 8)
                    return false;
                size = (quint64(be32(ext.constData())) << 32) | be32(ext.constData() + 4);
                hdr = 16;
            } else if (size == 0) {
                size = quint64(to - pos);   // to the end
            }
            if (size < quint64(hdr) || size > quint64(to - pos))
                return false;
            if (h.mid(4, 4) == type) {
                *body = pos + hdr;
                *body_end = pos + qint64(size);
                return true;
            }
            pos += qint64(size);
        }
        return false;
    };
    qint64 moov, moov_end, mvhd, mvhd_end;
    if (!find(0, f.size(), "moov", &moov, &moov_end) || !find(moov, moov_end, "mvhd", &mvhd, &mvhd_end))
        return QDateTime();
    f.seek(mvhd);
    QByteArray b = f.read(12);
    if (b.size() < 12)
        return QDateTime();
    quint64 secs = b[0] == 1 ? (quint64(be32(b.constData() + 4)) << 32) | be32(b.constData() + 8)
                             : be32(b.constData() + 4);
    const quint64 to_unix = 2082844800;   // 1904-01-01 to 1970-01-01
    if (secs <= to_unix)
        return QDateTime();
    return QDateTime::fromSecsSinceEpoch(qint64(secs - to_unix));   // local time
}

}  // namespace

bool may_have_taken(const QString &path)
{
    static const QSet<QString> exts = {"jpg", "jpeg", "jpe", "png", "webp", "tif", "tiff", "dng",
                                       "mp4", "m4v", "mov", "3gp"};
    QString name = basename(path);
    int dot = name.lastIndexOf('.');
    return dot > 0 && exts.contains(name.mid(dot + 1).toLower());
}

QDateTime taken(const QString &path)
{
    if (!may_have_taken(path))
        return QDateTime();
    QString ext = basename(path).section('.', -1).toLower();
    if (ext == "mp4" || ext == "m4v" || ext == "mov" || ext == "3gp")
        return movie_created(path);
    FileMeta fm = read_file_meta(path, 1 << 20);   // a TIFF's tags are near its start
    if (!fm.exif_blob.isEmpty()) {
        Exif exif = TiffReader(fm.exif_blob).parse();
        for (int tag : {0x9003, 0x9004}) {   // DateTimeOriginal, DateTimeDigitized: "YYYY:MM:DD HH:MM:SS", local
            QString t = exif.exif.value(tag).text().trimmed();
            QDateTime dt = QDateTime::fromString(t.left(19), "yyyy:MM:dd HH:mm:ss");
            if (dt.isValid())
                return dt;
        }
    }
    for (const auto &[key, value] : fm.text)   // PNG: "Creation Time", in whatever form the writer chose
        if (key == "Creation Time")
            for (Qt::DateFormat form : {Qt::ISODate, Qt::RFC2822Date, Qt::TextDate}) {
                QDateTime dt = QDateTime::fromString(value.trimmed(), form);
                if (dt.isValid())
                    return dt.toLocalTime();
            }
    return QDateTime();
}

// The GPS position in an EXIF block (degrees, south and west negative); false if it has none or it's nonsense
static bool gps_of(const Exif &exif, double *lat, double *lon)
{
    ExifVal la = exif.gps.value(2), lo = exif.gps.value(4);
    if (la.nums.size() < 3 || lo.nums.size() < 3)
        return false;
    auto deg = [](const ExifVal &v) { return v.nums[0] + v.nums[1] / 60 + v.nums[2] / 3600; };
    double y = deg(la), x = deg(lo);
    if (exif.gps.value(1).text().trimmed().startsWith('S'))
        y = -y;
    if (exif.gps.value(3).text().trimmed().startsWith('W'))
        x = -x;
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(y) > 90 || std::fabs(x) > 180 || (x == 0 && y == 0))
        return false;   // 0, 0: a camera that had no fix
    *lat = y;
    *lon = x;
    return true;
}

Rows basic_info(const QString &path)
{
    Rows out;
    QImageReader reader(path);
    if (!reader.canRead())
        return out;
    QSize size = reader.size();
    if (size.isValid())
        out << qMakePair(tx("Dimensions"), QString("%1 × %2").arg(size.width()).arg(size.height()));
    QString mode = mode_of(reader.imageFormat());
    QString fmt = QString::fromLatin1(reader.format()).toUpper();
    out << qMakePair(tx("Format"), mode.isEmpty() ? fmt : QString("%1 (%2)").arg(fmt, mode));
    int n = reader.imageCount();
    if (n > 1)
        out << qMakePair(tx("Frames"), i18n::number(n));
    FileMeta fm = read_file_meta(path);
    if (fm.exif_blob.isEmpty())
        return out;
    Exif exif = TiffReader(fm.exif_blob).parse();
    auto g = [&](int tag) -> const ExifVal * {
        if (exif.exif.contains(tag))
            return &exif.exif[tag];
        if (exif.ifd0.contains(tag))
            return &exif.ifd0[tag];
        return nullptr;
    };
    auto text = [&](int tag) { return g(tag) ? g(tag)->text().trimmed() : QString(); };
    QString make = text(0x010F), model = text(0x0110);
    if (!model.isEmpty())
        out << qMakePair(tx("Camera"), (make + " " + model).trimmed());
    if (!text(0xA434).isEmpty())
        out << qMakePair(tx("Lens"), text(0xA434));
    QString taken = !text(0x9003).isEmpty() ? text(0x9003) : text(0x0132);
    if (!taken.isEmpty())
        out << qMakePair(tx("Taken"), taken);
    if (const ExifVal *v = g(0x829A); v && v->numeric() && v->num() > 0) {
        double exp = v->num();
        out << qMakePair(tx("Exposure"), exp < 1 ? tx("1/%1 s").arg(qRound(1 / exp)) : tx("%1 s").arg(QLocale().toString(exp, 'g', 6)));
    }
    if (const ExifVal *v = g(0x829D); v && v->numeric() && v->num() > 0)
        out << qMakePair(tx("Aperture"), "f/" + QLocale().toString(v->num(), 'g', 6));
    if (const ExifVal *v = g(0x8827); v && v->numeric() && v->num() > 0)
        out << qMakePair(tx("ISO"), v->text());
    if (const ExifVal *v = g(0x920A); v && v->numeric() && v->num() > 0)
        out << qMakePair(tx("Focal length"), tx("%1 mm").arg(QLocale().toString(v->num(), 'g', 6)));
    if (double lat, lon; gps_of(exif, &lat, &lon))
        out << qMakePair(tx("GPS"), QString("%1, %2").arg(QString::number(lat, 'f', 6), QString::number(lon, 'f', 6)));
    else if (!exif.gps.isEmpty())
        out << qMakePair(tx("GPS"), tx("yes"));
    return out;
}

bool gps_position(const QString &path, double *lat, double *lon)
{
    FileMeta fm = read_file_meta(path);
    return !fm.exif_blob.isEmpty() && gps_of(TiffReader(fm.exif_blob).parse(), lat, lon);
}

static OrientationTag orientation_of(const FileMeta &fm)
{
    OrientationTag out;
    const QByteArray &d = fm.exif_blob;
    if (fm.exif_offset < 0 || d.size() < 8 || !(d.startsWith("II") || d.startsWith("MM")))
        return out;
    bool le = d.startsWith("II");
    auto byte = [&](qint64 o) { return quint32(uchar(d[o])); };
    auto u16 = [&](qint64 o) { return o + 2 > d.size() ? 0u : le ? byte(o) | byte(o + 1) << 8 : byte(o) << 8 | byte(o + 1); };
    auto u32 = [&](qint64 o) {
        return o + 4 > d.size() ? 0u : le ? u16(o) | u16(o + 2) << 16 : u16(o) << 16 | u16(o + 2);
    };
    qint64 ifd = u32(4);
    int n = int(u16(ifd));
    for (int i = 0; i < n && i < 1000; ++i) {
        qint64 e = ifd + 2 + qint64(i) * 12;
        if (e + 12 > d.size())
            break;
        if (u16(e) == 0x0112 && u16(e + 2) == 3 && u32(e + 4) == 1) {   // SHORT, one value: in the entry itself
            out.offset = fm.exif_offset + e + 8;
            out.little_endian = le;
            out.value = int(u16(e + 8));
            break;
        }
    }
    return out;
}

OrientationTag orientation_tag(const QString &path) { return orientation_of(read_file_meta(path)); }

OrientationTag orientation_tag_in(const QByteArray &file)
{
    QBuffer b;
    b.setData(file);
    b.open(QIODevice::ReadOnly);
    return orientation_of(read_file_meta(b));
}


static void set_default(Rows &rows, const QString &k, const QString &v)
{
    for (const auto &r : rows)
        if (r.first == k)
            return;
    rows << qMakePair(k, v);
}

static bool has_key(const Rows &rows, const QString &k)
{
    for (const auto &r : rows)
        if (r.first == k)
            return true;
    return false;
}

Rows ai_info(const QString &path)
{
    FileMeta fm = read_file_meta(path);
    QHash<QString, QString> info;
    for (const auto &[k, v] : fm.text)
        if (!info.contains(k))
            info[k] = v;
    if (!fm.exif_blob.isEmpty() && !info.contains("parameters")) {
        Exif exif = TiffReader(fm.exif_blob).parse();
        if (exif.exif.contains(0x9286)) {
            QByteArray uc = exif.exif[0x9286].raw;
            if (!uc.isEmpty()) {
                QByteArray body = uc.mid(8);
                QString text;
                if (uc.startsWith("UNICODE")) {
                    for (int i = 0; i + 1 < body.size(); i += 2)
                        text += QChar(ushort((uchar(body[i]) << 8) | uchar(body[i + 1])));
                } else {
                    text = QString::fromUtf8(body);
                }
                text.remove(QChar(0));
                info["parameters"] = text;
            }
        }
    }
    Rows res;
    QString params = info.value("parameters");
    if (!params.trimmed().isEmpty()) {
        QString text = params.trimmed();
        QString prompt = text, neg, settings;
        if (text.contains("\nSteps:") || text.startsWith("Steps:")) {
            int idx = text.lastIndexOf("Steps:");
            prompt = text.left(idx).trimmed();
            settings = text.mid(idx).trimmed();
        }
        int ni = prompt.indexOf("Negative prompt:");
        if (ni >= 0) {
            neg = prompt.mid(ni + 16);
            prompt = prompt.left(ni);
        }
        res << qMakePair(tx("Prompt"), prompt.trimmed());
        if (!neg.trimmed().isEmpty())
            res << qMakePair(tx("Negative prompt"), neg.trimmed());
        if (!settings.isEmpty())
            res << qMakePair(tx("Settings"), settings);
    }
    if (info.contains("prompt")) {
        QByteArray json = info["prompt"].toUtf8();
        if (QJsonDocument::fromJson(json).isObject()) {
            QStringList texts, settings;
            // the nodes in the order they were written (QJsonObject would sort them by id)
            for (const auto &[id, value] : ordered_object(json)) {
                QJsonObject node = value.toObject();
                QString ct = node.value("class_type").toString();
                QJsonObject inputs = node.value("inputs").toObject();
                if (ct.contains("TextEncode") || ct == "CLIPTextEncode" || ct == "PrimitiveStringMultiline" ||
                    ct == "String Literal") {
                    for (const char *k : {"text", "text_g", "text_l", "value", "string"}) {
                        QJsonValue v = inputs.value(k);
                        if (v.isString() && !v.toString().trimmed().isEmpty())
                            texts << v.toString().trimmed();
                    }
                }
                if (ct.startsWith("KSampler") || ct.contains("Sampler")) {
                    QStringList parts;
                    for (const char *k : {"seed", "noise_seed", "steps", "cfg", "sampler_name", "scheduler", "denoise"}) {
                        if (!inputs.contains(k) || inputs.value(k).isArray())
                            continue;
                        QJsonValue v = inputs.value(k);
                        QString s = v.isString() ? v.toString()
                                    : v.isDouble() ? fmt_num(v.toDouble())
                                    : v.isBool()   ? (v.toBool() ? "True" : "False")
                                                   : QString();
                        parts << QString("%1: %2").arg(k, s);
                    }
                    if (!parts.isEmpty())
                        settings << parts.join(", ");
                }
                if (ct.contains("CheckpointLoader") || ct == "UNETLoader" || ct == "LoraLoader") {
                    for (const char *k : {"ckpt_name", "unet_name", "lora_name"})
                        if (inputs.value(k).isString())
                            settings << QString("%1: %2").arg(k, inputs.value(k).toString());
                }
            }
            if (!texts.isEmpty()) {
                set_default(res, tx("Prompt"), texts.first());
                if (texts.size() > 1)
                    set_default(res, tx("Other prompts"), texts.mid(1).join("\n---\n"));
            }
            if (!settings.isEmpty())
                set_default(res, tx("Settings"), settings.join('\n'));
        }
    }
    if (info.contains("workflow")) {
        bool found = false;
        for (auto &r : res)
            if (r.first == tx("ComfyUI workflow")) {
                r.second = tx("embedded (see Metadata tab)");
                found = true;
            }
        if (!found)
            res << qMakePair(tx("ComfyUI workflow"), tx("embedded (see Metadata tab)"));
    }
    for (const char *k : {TX_NOOP("Description"), TX_NOOP("Comment"), TX_NOOP("Software")}) {   // (the PNG's own keys)
        QString v = info.value(k);
        if (v.isEmpty())
            v = info.value(QString(k).toLower());
        if (!v.trimmed().isEmpty() && !has_key(res, tx(k)))
            res << qMakePair(tx(k), v.trimmed());
    }
    return res;
}

// json.dumps-like text of a JSON value (lists as ["a", "b"])
static QString dumps(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::String: {
        QByteArray s = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
        return QString::fromUtf8(s.mid(1, s.size() - 2));
    }
    case QJsonValue::Double: return fmt_num(v.toDouble());
    case QJsonValue::Bool: return v.toBool() ? "true" : "false";
    case QJsonValue::Null: return "null";
    case QJsonValue::Array: {
        QStringList parts;
        for (const QJsonValue &x : v.toArray())
            parts << dumps(x);
        return "[" + parts.join(", ") + "]";
    }
    case QJsonValue::Object: {
        QStringList parts;
        QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            parts << dumps(QJsonValue(it.key())) + ": " + dumps(it.value());
        return "{" + parts.join(", ") + "}";
    }
    default: return QString();
    }
}

// An object's keys in the order they were written (QJsonObject sorts them), with their values: the object `json` is,
// or the first one in an array of them (exiftool's [{…}]). As Python's json.loads: each key once, where it first
// appears, with its last value. Qt's parser reads the JSON (empty if it isn't valid); a small tokenizer then only
// reads off the keys' order, which it can do simply because the text is known to be valid.
QList<QPair<QString, QJsonValue>> ordered_object(const QByteArray &json)
{
    QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonObject obj;
    if (doc.isObject())
        obj = doc.object();
    else if (doc.isArray() && doc.array().first().isObject())
        obj = doc.array().first().toObject();
    if (obj.isEmpty())
        return {};
    qsizetype i = 0;
    auto skip_ws = [&]() {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r'))
            ++i;
    };
    auto skip_string = [&]() {   // at the opening quote; ends after the closing one
        for (++i; i < json.size() && json[i] != '"'; ++i)
            if (json[i] == '\\')
                ++i;
        ++i;
    };
    auto skip_value = [&]() {   // a string, an object or array (nested), or a number / true / false / null
        skip_ws();
        if (json[i] == '"') {
            skip_string();
            return;
        }
        int depth = 0;
        for (; i < json.size(); ++i) {
            char c = json[i];
            if (c == '"') {
                skip_string();
                --i;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (depth == 0)
                    return;
                if (--depth == 0) {
                    ++i;
                    return;
                }
            } else if (c == ',' && depth == 0) {
                return;
            }
        }
    };
    skip_ws();
    if (json[i] == '[') {
        ++i;
        skip_ws();
    }
    ++i;   // the object's {
    QList<QPair<QString, QJsonValue>> out;
    QSet<QString> seen;
    for (;;) {
        skip_ws();
        if (i >= json.size() || json[i] != '"')
            break;
        qsizetype start = i;
        skip_string();
        QString key = QJsonDocument::fromJson("[" + json.mid(start, i - start) + "]").array().first().toString();
        if (!seen.contains(key)) {
            seen << key;
            out << qMakePair(key, obj.value(key));
        }
        skip_ws();
        ++i;   // :
        skip_value();
        skip_ws();
        if (i >= json.size() || json[i] != ',')
            break;
        ++i;
    }
    return out;
}

QList<MetaRow> full_metadata(const QString &path)
{
    QList<MetaRow> rows;
    if (which("exiftool")) {
        auto r = proc::run({"exiftool", "-j", "-G1", "-a", "-charset", "filename=utf8", path}, 30000);
        if (r.rc == 0 || !r.out.isEmpty()) {
            for (const auto &[key, val] : ordered_object(r.out)) {
                if (key == "SourceFile")
                    continue;
                int c = key.indexOf(':');
                QString group = c < 0 ? QString() : key.left(c);
                QString tag = c < 0 ? key : key.mid(c + 1);
                rows << MetaRow{group, tag, val.isString() ? val.toString() : dumps(val)};
            }
            if (!rows.isEmpty())
                return rows;
        }
    }
    QImageReader reader(path);
    if (!reader.canRead()) {
        rows << MetaRow{tx("Error"), "", reader.errorString()};
        return rows;
    }
    QSize size = reader.size();
    rows << MetaRow{"Image", "Format", QString::fromLatin1(reader.format()).toUpper()}
         << MetaRow{"Image", "Mode", mode_of(reader.imageFormat())}
         << MetaRow{"Image", "Size", QString("%1 × %2").arg(size.width()).arg(size.height())};
    FileMeta fm = read_file_meta(path);
    for (const auto &[k, v] : fm.text)
        rows << MetaRow{"Info", k, v};
    if (!fm.exif_blob.isEmpty()) {
        Exif exif = TiffReader(fm.exif_blob).parse();
        for (auto it = exif.ifd0.begin(); it != exif.ifd0.end(); ++it)
            rows << MetaRow{"EXIF", tag_name(exif_tag_names(), it.key()), it.value().text()};
        for (auto it = exif.exif.begin(); it != exif.exif.end(); ++it)
            rows << MetaRow{"EXIF", tag_name(exif_tag_names(), it.key()), it.value().text()};
        for (auto it = exif.gps.begin(); it != exif.gps.end(); ++it)
            rows << MetaRow{"GPS", tag_name(gps_tag_names(), it.key()), it.value().text()};
    }
    return rows;
}

// ---------------------------------------------------------------- editing (exiftool)

// exiftool groups that describe the file itself or are computed; writing File:FileName etc. would rename/move it
static const QSet<QString> READONLY_GROUPS = {"", "ExifTool", "System", "File", "Composite"};

bool can_edit() { return which("exiftool"); }

bool viewable(const QString &path)
{
    static const QSet<QString> DOCS = {".pdf", ".epub", ".mobi", ".azw3", ".djvu", ".docx", ".xlsx", ".pptx", ".doc",
                                       ".xls", ".ppt", ".odt", ".ods", ".odp", ".rtf", ".psd", ".svg", ".xmp"};
    if (is_image(path))
        return true;
    return can_edit() && (is_video(path) || convert::is_audio(path) || DOCS.contains(ext_of(path)));
}

bool is_editable(const QString &group, const QString &value, const QString &tag)
{
    if (group == "File" && tag == "Comment")   // JPEG comment segment
        return true;
    return !READONLY_GROUPS.contains(group) && !value.startsWith("(Binary data");
}

QStringList as_list(const QString &value, bool *ok)
{
    *ok = false;
    if (!value.startsWith('['))
        return {};
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(value.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return {};
    *ok = true;
    QStringList items;
    for (const QJsonValue &v : doc.array())
        items << (v.isString() ? v.toString() : dumps(v));
    return items;
}

// Run an exiftool write in place; raises Error on failure, returns a list of warnings.
static QStringList exiftool_write(const QString &path, const QStringList &args)
{
    QStringList argv{"exiftool", "-overwrite_original", "-charset", "filename=utf8"};
    argv += args;
    argv << abspath(path);
    auto r = proc::run(argv, 300000);
    QString out = QString::fromUtf8(r.out + r.err);
    QStringList msgs;
    for (const QString &ln : out.split('\n')) {
        QString t = ln.trimmed();
        if (t.startsWith("Warning:") || t.startsWith("Error:"))
            msgs << t;
    }
    if (r.failed)
        throw Error(tx("exiftool failed"));
    if (r.rc != 0 || out.contains("0 image files updated") || out.contains("Nothing to do"))
        throw Error(!msgs.isEmpty() ? msgs.join('\n') : !out.trimmed().isEmpty() ? out.trimmed() : tx("exiftool failed"));
    return msgs;
}

QStringList set_tags(const QString &path, const QList<Change> &changes)
{
    QStringList args;
    for (const Change &c : changes) {
        if (c.remove) {
            args << QString("-%1=").arg(c.key);
        } else if (c.is_list) {
            if (c.values.isEmpty())
                args << QString("-%1=").arg(c.key);
            for (const QString &v : c.values)
                args << QString("-%1=%2").arg(c.key, v);
        } else {
            args << QString("-%1=%2").arg(c.key, c.values.value(0));
        }
    }
    return exiftool_write(path, args);
}

QStringList clear_all(const QString &path, bool keep_basic)
{
    QStringList args{"-all="};
    if (keep_basic)
        args << "-tagsfromfile" << "@" << "-icc_profile" << "-orientation";
    QStringList out;
    for (const QString &m : exiftool_write(path, args))
        if (!m.contains("No writable tags set"))
            out << m;
    return out;
}

bool can_wipe(const QString &path)
{
    static const QSet<QString> EXTS = {".jpg", ".jpeg", ".jpe", ".png", ".apng", ".webp", ".heic", ".heif", ".hif",
                                       ".avif", ".jxl", ".gif", ".tif", ".tiff"};
    return EXTS.contains(ext_of(path)) && isfile(path);
}

QString wipe(const QString &path, bool copy, bool keep_basic)
{
    // exiftool writes the wiped file to its output, which Kestrel puts in place itself (exiftool's own in-place write
    // isn't fsynced, and a copy has to claim its name without replacing anything)
    QStringList argv{"exiftool", "-charset", "filename=utf8", "-all="};
    if (keep_basic)
        argv << "-tagsfromfile" << "@" << "-icc_profile" << "-orientation";
    argv << "-o" << "-" << abspath(path);
    auto r = proc::run(argv, 300000);
    if (r.failed)
        throw Error(tx("Wiping metadata needs exiftool (package libimage-exiftool-perl)."));
    if (r.rc != 0 || r.out.isEmpty()) {
        QString err = QString::fromUtf8(r.err).trimmed();
        throw Error(tx("Couldn't wipe the metadata of %1: %2").arg(basename(path), err.isEmpty() ? tx("exiftool failed") : err));
    }
    struct stat st;
    bool have_mode = ::stat(enc(path).constData(), &st) == 0;
    auto fill = [&](int fd) {
        if (copy && have_mode)
            ::fchmod(fd, st.st_mode & 0777);   // a copy of a private photo stays private
        write_all(fd, r.out.constData(), r.out.size(), path);
    };
    if (!copy) {
        write_parts(path, fill, false, true);
        return path;
    }
    auto [stem, ext] = split_ext(basename(path));
    for (int n = 1;; ++n) {
        QString to = join(dirname(path), stem + " (no metadata" + (n > 1 ? " " + QString::number(n) : QString()) + ")" + ext);
        if (lexists(to))
            continue;
        try {
            write_parts(to, fill, true);
            return to;
        } catch (const OSError &e) {
            if (e.code != EEXIST)   // taken meanwhile: the next name
                throw;
        }
    }
}

// ---------------------------------------------------------------- tag catalog for "Add Tag"

// metadata standards ("families") exiftool can write into each file type
static const QHash<QString, QStringList> &families_by_ext()
{
    static const QHash<QString, QStringList> m = [] {
        QHash<QString, QStringList> out;
        const QList<QPair<const char *, QStringList>> table = {
            {"jpg jpeg jpe mpo", {"EXIF", "XMP", "IPTC", "File"}},
            {"tif tiff dng cr2 cr3 nef nrw arw sr2 orf rw2 raf pef srw erf mrw x3f iiq 3fr psd psb",
             {"EXIF", "XMP", "IPTC"}},
            {"png apng", {"PNG", "EXIF", "XMP"}},
            {"webp heic heif hif avif jxl jp2 j2k jpx", {"EXIF", "XMP"}},
            {"gif", {"GIF", "XMP"}},
            {"mp4 mov m4v qt 3gp 3g2 lrv insv", {"QuickTime", "XMP"}},
            {"m4a m4b aax", {"QuickTime"}},
            {"pdf ai", {"PDF", "XMP"}}};
        for (const auto &[exts, fams] : table)
            for (const QString &e : QString(exts).split(' '))
                out[e] = fams;
        return out;
    }();
    return m;
}

// exiftool groups listed under "All … tags" for each family
static const QList<QPair<QString, QStringList>> FAMILY_GROUPS = {
    {"EXIF", {"EXIF"}}, {"IPTC", {"IPTC"}}, {"PNG", {"PNG"}}, {"QuickTime", {"QuickTime"}}, {"PDF", {"PDF"}},
    {"GIF", {"GIF"}},
    {"XMP", {"XMP-dc", "XMP-xmp", "XMP-photoshop", "XMP-iptcCore", "XMP-iptcExt", "XMP-xmpRights", "XMP-lr"}}};

static QStringList family_groups(const QString &fam)
{
    for (const auto &[f, gs] : FAMILY_GROUPS)
        if (f == fam)
            return gs;
    return {};
}

// (family, "Group:Tag", what it's for) — the common tags offered first
const QList<CatalogEntry> TAG_CATALOG = {
    {"EXIF", "EXIF:ImageDescription", TX_NOOP("Title or caption describing the image.")},
    {"EXIF", "EXIF:Artist", TX_NOOP("Name of the photographer or creator.")},
    {"EXIF", "EXIF:Copyright", TX_NOOP("Copyright notice, e.g. “© 2026 Jane Doe. All rights reserved.”")},
    {"EXIF", "EXIF:UserComment", TX_NOOP("Free-form comment. Stable Diffusion tools store generation parameters here in JPEGs.")},
    {"EXIF", "EXIF:DateTimeOriginal", TX_NOOP("When the photo was taken. Format: YYYY:MM:DD HH:MM:SS.")},
    {"EXIF", "EXIF:CreateDate", TX_NOOP("When the image was digitised (normally the same as when it was taken). Format: YYYY:MM:DD HH:MM:SS.")},
    {"EXIF", "EXIF:ModifyDate", TX_NOOP("When the image was last edited. Format: YYYY:MM:DD HH:MM:SS.")},
    {"EXIF", "EXIF:OffsetTimeOriginal", TX_NOOP("Time-zone offset for DateTimeOriginal, e.g. +02:00.")},
    {"EXIF", "EXIF:Orientation", TX_NOOP("How viewers should rotate or flip the image when showing it.")},
    {"EXIF", "EXIF:Make", TX_NOOP("Manufacturer of the camera or phone.")},
    {"EXIF", "EXIF:Model", TX_NOOP("Model name of the camera or phone.")},
    {"EXIF", "EXIF:LensMake", TX_NOOP("Manufacturer of the lens.")},
    {"EXIF", "EXIF:LensModel", TX_NOOP("Lens used to take the photo.")},
    {"EXIF", "EXIF:SerialNumber", TX_NOOP("Serial number of the camera body.")},
    {"EXIF", "EXIF:Software", TX_NOOP("Program used to create or last edit the image.")},
    {"EXIF", "EXIF:ExposureTime", TX_NOOP("Shutter speed in seconds, e.g. 1/250.")},
    {"EXIF", "EXIF:FNumber", TX_NOOP("Aperture f-number, e.g. 2.8.")},
    {"EXIF", "EXIF:ISO", TX_NOOP("Sensor sensitivity (ISO speed), e.g. 400.")},
    {"EXIF", "EXIF:FocalLength", TX_NOOP("Lens focal length in mm, e.g. 50.")},
    {"EXIF", "EXIF:GPSLatitude", TX_NOOP("Latitude in degrees, e.g. 48.8584. Also set GPSLatitudeRef (North/South).")},
    {"EXIF", "EXIF:GPSLatitudeRef", TX_NOOP("Hemisphere for GPSLatitude: N (north) or S (south).")},
    {"EXIF", "EXIF:GPSLongitude", TX_NOOP("Longitude in degrees, e.g. 2.2945. Also set GPSLongitudeRef (East/West).")},
    {"EXIF", "EXIF:GPSLongitudeRef", TX_NOOP("Hemisphere for GPSLongitude: E (east) or W (west).")},
    {"EXIF", "EXIF:GPSAltitude", TX_NOOP("Altitude in metres above sea level.")},
    {"EXIF", "EXIF:XPTitle", TX_NOOP("Title shown in Windows Explorer's Details tab.")},
    {"EXIF", "EXIF:XPComment", TX_NOOP("Comments shown in Windows Explorer's Details tab.")},
    {"EXIF", "EXIF:XPKeywords", TX_NOOP("Tags shown in Windows Explorer, separated by semicolons.")},
    {"EXIF", "EXIF:XPAuthor", TX_NOOP("Authors shown in Windows Explorer, separated by semicolons.")},
    {"EXIF", "EXIF:XPSubject", TX_NOOP("Subject shown in Windows Explorer's Details tab.")},
    {"File", "File:Comment", TX_NOOP("JPEG comment segment — a plain-text note many tools display.")},
    {"XMP", "XMP-dc:Title", TX_NOOP("Title of the work (Lightroom, Bridge, digiKam, darktable…).")},
    {"XMP", "XMP-dc:Description", TX_NOOP("Caption or description of the content.")},
    {"XMP", "XMP-dc:Creator", TX_NOOP("Creator(s) or author(s); one name per line.")},
    {"XMP", "XMP-dc:Subject", TX_NOOP("Keywords; one per line. The standard keyword field for photo managers.")},
    {"XMP", "XMP-dc:Rights", TX_NOOP("Copyright / usage rights statement.")},
    {"XMP", "XMP-lr:HierarchicalSubject", TX_NOOP("Nested keywords, e.g. “Places|France|Paris”; one per line.")},
    {"XMP", "XMP-xmp:Rating", TX_NOOP("Star rating 0–5 (−1 = rejected), used by Lightroom, digiKam, Windows and others.")},
    {"XMP", "XMP-xmp:Label", TX_NOOP("Colour label, e.g. Red, Green, Blue.")},
    {"XMP", "XMP-xmp:CreateDate", TX_NOOP("When the resource was created. Format: YYYY:MM:DD HH:MM:SS.")},
    {"XMP", "XMP-xmp:ModifyDate", TX_NOOP("When the resource was last modified. Format: YYYY:MM:DD HH:MM:SS.")},
    {"XMP", "XMP-xmp:CreatorTool", TX_NOOP("Application that created the file.")},
    {"XMP", "XMP-photoshop:Headline", TX_NOOP("Short headline summarising the content.")},
    {"XMP", "XMP-photoshop:DateCreated", TX_NOOP("When the content was created (date or date-time).")},
    {"XMP", "XMP-photoshop:City", TX_NOOP("City where the photo was taken.")},
    {"XMP", "XMP-photoshop:State", TX_NOOP("State or province where the photo was taken.")},
    {"XMP", "XMP-photoshop:Country", TX_NOOP("Country where the photo was taken.")},
    {"XMP", "XMP-photoshop:Credit", TX_NOOP("Credit line required when publishing.")},
    {"XMP", "XMP-photoshop:Source", TX_NOOP("Original owner or supplier of the content.")},
    {"XMP", "XMP-photoshop:Instructions", TX_NOOP("Special instructions, e.g. embargoes or usage restrictions.")},
    {"XMP", "XMP-iptcCore:Location", TX_NOOP("Sub-location (venue, landmark or neighbourhood).")},
    {"XMP", "XMP-iptcCore:CountryCode", TX_NOOP("ISO country code, e.g. FR or USA.")},
    {"XMP", "XMP-xmpRights:UsageTerms", TX_NOOP("Licence or terms for using the content.")},
    {"XMP", "XMP-xmpRights:WebStatement", TX_NOOP("URL of a web page with copyright / licence details.")},
    {"XMP", "XMP-xmpRights:Marked", TX_NOOP("True if the content is copyrighted, False if public domain.")},
    {"IPTC", "IPTC:ObjectName", TX_NOOP("Short title (IPTC). Used by news agencies and older photo software.")},
    {"IPTC", "IPTC:Caption-Abstract", TX_NOOP("Caption / description (IPTC).")},
    {"IPTC", "IPTC:Keywords", TX_NOOP("Keywords (IPTC); one per line.")},
    {"IPTC", "IPTC:By-line", TX_NOOP("Photographer / creator (IPTC).")},
    {"IPTC", "IPTC:CopyrightNotice", TX_NOOP("Copyright notice (IPTC).")},
    {"IPTC", "IPTC:Headline", TX_NOOP("Short headline (IPTC).")},
    {"IPTC", "IPTC:Credit", TX_NOOP("Credit line (IPTC).")},
    {"IPTC", "IPTC:Source", TX_NOOP("Original owner of the content (IPTC).")},
    {"IPTC", "IPTC:City", TX_NOOP("City (IPTC).")},
    {"IPTC", "IPTC:Province-State", TX_NOOP("State or province (IPTC).")},
    {"IPTC", "IPTC:Country-PrimaryLocationName", TX_NOOP("Country (IPTC).")},
    {"IPTC", "IPTC:DateCreated", TX_NOOP("Date the content was created (IPTC). Format: YYYY:MM:DD.")},
    {"IPTC", "IPTC:SpecialInstructions", TX_NOOP("Usage instructions or restrictions (IPTC).")},
    {"PNG", "PNG:Title", TX_NOOP("Short title of the image (PNG text chunk).")},
    {"PNG", "PNG:Author", TX_NOOP("Name of the image's creator.")},
    {"PNG", "PNG:Description", TX_NOOP("Description of the image.")},
    {"PNG", "PNG:Comment", TX_NOOP("Miscellaneous comment.")},
    {"PNG", "PNG:Copyright", TX_NOOP("Copyright notice.")},
    {"PNG", "PNG:CreationTime", TX_NOOP("When the original image was created. Format: YYYY:MM:DD HH:MM:SS.")},
    {"PNG", "PNG:Software", TX_NOOP("Software used to create the image.")},
    {"PNG", "PNG:Source", TX_NOOP("Device used to create the image.")},
    {"PNG", "PNG:Disclaimer", TX_NOOP("Legal disclaimer.")},
    {"PNG", "PNG:Parameters", TX_NOOP("Stable Diffusion (A1111/Forge) generation parameters: prompt, negative prompt, settings.")},
    {"QuickTime", "QuickTime:Title", TX_NOOP("Title shown by media players.")},
    {"QuickTime", "QuickTime:Artist", TX_NOOP("Artist or creator.")},
    {"QuickTime", "QuickTime:Author", TX_NOOP("Author of the video.")},
    {"QuickTime", "QuickTime:Director", TX_NOOP("Director of the video.")},
    {"QuickTime", "QuickTime:Album", TX_NOOP("Album or collection the video/track belongs to.")},
    {"QuickTime", "QuickTime:Genre", TX_NOOP("Genre, e.g. Documentary.")},
    {"QuickTime", "QuickTime:Year", TX_NOOP("Release year.")},
    {"QuickTime", "QuickTime:Comment", TX_NOOP("Free-form comment.")},
    {"QuickTime", "QuickTime:Description", TX_NOOP("Description of the content.")},
    {"QuickTime", "QuickTime:Keywords", TX_NOOP("Keywords, separated by commas.")},
    {"QuickTime", "QuickTime:Copyright", TX_NOOP("Copyright notice.")},
    {"QuickTime", "QuickTime:Rating", TX_NOOP("Content rating.")},
    {"QuickTime", "QuickTime:CreateDate", TX_NOOP("When the video was recorded (stored as UTC). Format: YYYY:MM:DD HH:MM:SS.")},
    {"QuickTime", "QuickTime:ContentCreateDate",
     "When the content was created, with time zone, e.g. 2026:10:02 14:00:00+02:00."},
    {"QuickTime", "QuickTime:GPSCoordinates",
     "Recording location: “latitude longitude [altitude]”, e.g. 48.8584 2.2945 35."},
    {"QuickTime", "QuickTime:Encoder", TX_NOOP("Software used to encode the video.")},
    {"PDF", "PDF:Title", TX_NOOP("Document title shown by PDF viewers.")},
    {"PDF", "PDF:Author", TX_NOOP("Document author.")},
    {"PDF", "PDF:Subject", TX_NOOP("Document subject.")},
    {"PDF", "PDF:Keywords", TX_NOOP("Keywords for searching.")},
    {"PDF", "PDF:Creator", TX_NOOP("Application that created the original document.")},
    {"PDF", "PDF:Producer", TX_NOOP("Application that produced the PDF.")},
    {"PDF", "PDF:CreateDate", TX_NOOP("When the document was created. Format: YYYY:MM:DD HH:MM:SS.")},
    {"PDF", "PDF:ModifyDate", TX_NOOP("When the document was last modified. Format: YYYY:MM:DD HH:MM:SS.")},
    {"GIF", "GIF:Comment", TX_NOOP("Plain-text comment stored in the GIF.")},
};

QString tag_help(const QString &key)
{
    for (const CatalogEntry &e : TAG_CATALOG)
        if (e.key == key)
            return tx(e.blurb);
    return QString();
}

// list-type tags offered in the catalog: the editor writes one item per line
bool is_list_tag(const QString &key)
{
    static const QSet<QString> tags = {"XMP-dc:Creator", "XMP-dc:Subject", "XMP-lr:HierarchicalSubject",
                                       "IPTC:Keywords", "IPTC:By-line"};
    return tags.contains(key);
}

// Normalise "Group:Tag" so a catalog key (EXIF:Artist) matches what full_metadata shows (IFD0:Artist).
QPair<QString, QString> tag_identity(const QString &key)
{
    static const QSet<QString> exif_groups = {"IFD0", "IFD1", "ExifIFD", "GPS", "InteropIFD", "SubIFD", "EXIF"};
    static const QSet<QString> qt_groups = {"QuickTime", "ItemList", "Keys", "UserData"};
    int c = key.lastIndexOf(':');
    QString group = c < 0 ? QString() : key.left(c);
    QString tag = c < 0 ? key : key.mid(c + 1);
    if (exif_groups.contains(group))
        group = "EXIF";
    else if (qt_groups.contains(group))
        group = "QuickTime";
    else if (group.startsWith("PNG"))
        group = "PNG";
    return {group, tag.toLower()};
}

static QSet<QString> writable_exts()
{
    static std::once_flag once;
    static QSet<QString> exts;
    std::call_once(once, [] {
        if (!can_edit())
            return;
        auto r = proc::run({"exiftool", "-listwf"}, 30000);
        QStringList words = QString::fromUtf8(r.out).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        for (const QString &w : words.mid(3))   // skip "Writable file extensions:"
            exts << w;
    });
    return exts;
}

QStringList tag_families(const QString &path)
{
    QString ext = ext_of(path).mid(1);
    if (families_by_ext().contains(ext))
        return families_by_ext().value(ext);
    return writable_exts().contains(ext.toUpper()) ? QStringList{"XMP"} : QStringList();
}

static std::mutex db_lock;
static bool db_loaded = false;
static TagDb db_value;

const TagDb *tag_db_if_loaded()
{
    std::lock_guard<std::mutex> g(db_lock);
    return db_loaded ? &db_value : nullptr;
}

static QString kind_of(const QString &group, const QString &name, const QString &typ, const QString &g2, bool has_values)
{
    // what a tag accepts, in plain words: Text, Numbers only, Date/time, Choice or True/False
    static const QRegularExpression number_types(
        "^(int\\d+[su]|int16uRev|rational\\d*[su]?|real|float|double|integer|digits|fixed32[su])$");
    // (kept in English in the tag list's cache: tag_choices translates them)
    if (has_values)
        return TX_NOOP("Choice");
    if (typ == "boolean")
        return TX_NOOP("True/False");
    if (typ == "date" || g2 == "Time")
        return TX_NOOP("Date/time");
    if (typ == "struct")
        return TX_NOOP("Structured");
    if (group == "EXIF" && name.startsWith("XP"))   // stored as UCS-2 bytes but written as text
        return TX_NOOP("Text");
    if (number_types.match(typ).hasMatch())
        return TX_NOOP("Numbers only");
    return TX_NOOP("Text");
}

static QString split_name(const QString &name)
{
    // "DateTimeOriginal" -> "Date Time Original"
    static const QRegularExpression re("(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])");
    QString out = name;
    return out.replace(re, " ");
}

struct ListxInfo {
    QString desc, kind;
    QStringList values;
    QString category, typ;
};

// {(group, name): info} from `exiftool -listx` for the given groups
static QHash<QPair<QString, QString>, ListxInfo> parse_listx(const QByteArray &xml, const QSet<QString> &wanted)
{
    static const QHash<QString, QString> category = {
        {"Author", TX_NOOP("Author / rights info")}, {"Camera", TX_NOOP("Camera setting")}, {"Location", TX_NOOP("Location")},
        {"Time", TX_NOOP("Date / time")},   {"Image", TX_NOOP("Image info")}, {"Video", TX_NOOP("Video info")},
        {"Audio", TX_NOOP("Audio info")},   {"Document", TX_NOOP("Document info")}, {"Preview", TX_NOOP("Embedded preview")}};
    QHash<QPair<QString, QString>, ListxInfo> out;
    QXmlStreamReader r(xml);
    QString tg0, tg1;
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement())
            continue;
        if (r.name() == u"table") {
            tg0 = r.attributes().value("g0").toString();
            tg1 = r.attributes().value("g1").toString();
            continue;
        }
        if (r.name() != u"tag")
            continue;
        QXmlStreamAttributes a = r.attributes();
        QString name = a.value("name").toString(), typ = a.value("type").toString();
        QString g0 = a.hasAttribute("g0") ? a.value("g0").toString() : tg0;
        QString g1 = a.hasAttribute("g1") ? a.value("g1").toString() : tg1;
        QString g2 = a.value("g2").toString();
        bool writable = a.value("writable") == u"true";
        QString desc;
        bool have_desc = false;
        QStringList values;
        // children: <desc>, <values><key><val>…
        int depth = 1;
        QStringList path;
        while (depth > 0 && !r.atEnd()) {
            r.readNext();
            if (r.isStartElement()) {
                ++depth;
                path << r.name().toString();
                if (path.size() == 1 && path[0] == "desc" && !have_desc) {
                    desc = r.readElementText();
                    have_desc = true;
                    --depth;
                    path.removeLast();
                } else if (path == QStringList{"values", "key", "val"}) {
                    QString v = r.readElementText();
                    if (!v.isEmpty())
                        values << v;
                    --depth;
                    path.removeLast();
                }
            } else if (r.isEndElement()) {
                --depth;
                if (!path.isEmpty())
                    path.removeLast();
            }
        }
        QSet<QString> gs{g0, g1};
        for (const QString &g : gs) {
            if (!wanted.contains(g))
                continue;
            auto prev = out.constFind({g, name});
            if (!writable || (prev != out.constEnd() && prev->typ != "?"))
                continue;
            out[{g, name}] = ListxInfo{have_desc ? desc : name, kind_of(g, name, typ, g2, !values.isEmpty()),
                                       values.mid(0, 40), category.value(g2), typ};
        }
    }
    return out;
}

const TagDb &tag_db()
{
    {
        std::lock_guard<std::mutex> g(db_lock);
        if (db_loaded)
            return db_value;
    }
    TagDb db;
    auto finish = [&]() -> const TagDb & {
        std::lock_guard<std::mutex> g(db_lock);
        if (!db_loaded) {
            db_value = db;
            db_loaded = true;
        }
        return db_value;
    };
    auto ver_r = proc::run({"exiftool", "-ver"}, 30000);
    if (ver_r.failed || ver_r.rc != 0)
        return finish();
    QString ver = QString::fromUtf8(ver_r.out).trimmed();
    QString cache = join(APP_CACHE(), QString("exiftool-tagdb-%1.json").arg(ver));
    bool ok = false;
    QByteArray cached = read_file(cache, &ok);
    if (ok) {
        QJsonDocument doc = QJsonDocument::fromJson(cached);
        if (doc.isObject()) {
            QJsonObject o = doc.object();
            for (auto it = o.begin(); it != o.end(); ++it) {
                QList<DbRow> rows;
                for (const QJsonValue &rv : it.value().toArray()) {
                    QJsonArray a = rv.toArray();
                    QStringList vals;
                    for (const QJsonValue &v : a.at(3).toArray())
                        vals << v.toString();
                    rows << DbRow{a.at(0).toString(), a.at(1).toString(), a.at(2).toString(), vals, a.at(4).toString()};
                }
                db[it.key()] = rows;
            }
            return finish();
        }
    }
    QStringList groups;
    for (const auto &[f, gs] : FAMILY_GROUPS)
        groups += gs;
    QStringList args{"exiftool"};
    for (const QString &g : groups)
        args << "-listw" << QString("-%1:All").arg(g) << "-execute";
    args.removeLast();
    auto listw = proc::run(args, 120000);
    QHash<QString, QStringList> names;
    QString cur;
    bool have_cur = false;
    static const QRegularExpression head("^Writable (\\S+) tags:");
    for (const QString &line : QString::fromUtf8(listw.out).split('\n')) {
        auto m = head.match(line);
        if (m.hasMatch()) {
            cur = m.captured(1);
            names[cur];
            have_cur = true;
        } else if (have_cur) {
            for (const QString &t : line.split(' ', Qt::SkipEmptyParts))
                if (!t.startsWith("Unknown"))
                    names[cur] << t;
        }
    }
    auto listx = proc::run({"exiftool", "-listx", "-lang", "en"}, 120000);
    auto info = parse_listx(listx.out, QSet<QString>(groups.begin(), groups.end()));
    QJsonObject json;
    for (auto it = names.begin(); it != names.end(); ++it) {
        QStringList ns(QSet<QString>(it.value().begin(), it.value().end()).values());
        std::sort(ns.begin(), ns.end(), [](const QString &a, const QString &b) { return a.toLower() < b.toLower(); });
        QList<DbRow> rows;
        QJsonArray jrows;
        for (const QString &n : ns) {
            auto f = info.constFind({it.key(), n});
            DbRow row = f != info.constEnd() ? DbRow{n, f->desc, f->kind, f->values, f->category}
                                             : DbRow{n, split_name(n), "Text", {}, ""};
            rows << row;
            jrows.append(QJsonArray{row.name, row.desc, row.kind, QJsonArray::fromStringList(row.values), row.category});
        }
        db[it.key()] = rows;
        json[it.key()] = jrows;
    }
    try {
        makedirs(dirname(cache), true);
        write_text(cache, QJsonDocument(json).toJson(QJsonDocument::Compact));
    } catch (const OSError &) {
    }
    return finish();
}

// best-effort description for tags without a hand-written one, from exiftool's name, category and values
static QString auto_blurb(const QString &desc, const QStringList &values, const QString &category)
{
    QString text = category.isEmpty() ? desc : tx("%1: %2", "a tag's category and its description").arg(tx(category), desc);
    while (text.endsWith('.'))
        text.chop(1);
    text += ".";
    if (!values.isEmpty())
        text += " " + tx("One of: %1.").arg(values.mid(0, 6).join(", ") + (values.size() > 6 ? "…" : ""));
    return text;
}

QList<Section> tag_choices(const QString &path, const TagDb *db)
{
    QStringList fams = tag_families(path);
    QHash<QString, const DbRow *> info;
    if (db)
        for (auto it = db->begin(); it != db->end(); ++it)
            for (const DbRow &row : it.value())
                info[it.key() + ":" + row.name] = &row;
    QList<Section> out;
    QList<TagChoice> common;
    for (const CatalogEntry &e : TAG_CATALOG) {
        if (!fams.contains(e.family))
            continue;
        const DbRow *row = info.value(e.key);
        QString kind = row ? row->kind : "Text";
        common << TagChoice{e.key, tx(e.blurb), is_list_tag(e.key) ? tx("Text (list)") : tx(kind), row ? row->values : QStringList()};
    }
    if (!common.isEmpty())
        out << Section(tx("Common tags"), common);
    QSet<QString> seen;
    for (const TagChoice &c : common)
        seen << c.key;
    for (const QString &fam : fams) {
        for (const QString &g : family_groups(fam)) {
            QList<TagChoice> items;
            if (db)
                for (const DbRow &row : db->value(g)) {
                    QString key = g + ":" + row.name;
                    if (!seen.contains(key))
                        items << TagChoice{key, auto_blurb(row.desc, row.values, row.category), tx(row.kind), row.values};
                }
            if (!items.isEmpty())
                out << Section(tx("All %1 tags").arg(g), items);
        }
    }
    return out;
}

}  // namespace metadata
