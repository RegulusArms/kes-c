#include "media.h"

#include "fileops.h"
#include "imgedit.h"
#include "proc.h"
#include "util.h"

#include <QBuffer>
#include <QColorSpace>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>

#include <cmath>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

using namespace util;

namespace media {

bool have_ffmpeg() { return which("ffmpeg") && which("ffprobe"); }

// ---------------------------------------------------------------- probe

const Stream *Probe::video() const
{
    for (const Stream &s : streams)
        if (s.type == "video" && !s.cover)
            return &s;
    return nullptr;
}

QList<Stream> Probe::audio() const
{
    QList<Stream> out;
    for (const Stream &s : streams)
        if (s.type == "audio")
            out << s;
    return out;
}

static double rate_of(const QString &r)   // "30000/1001"
{
    QStringList f = r.split('/');
    double a = f.value(0).toDouble(), b = f.size() > 1 ? f[1].toDouble() : 1;
    return b > 0 ? a / b : 0;
}

Probe parse_probe(const QByteArray &json)
{
    Probe p;
    QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        p.error = tx("ffprobe couldn't read it");
        return p;
    }
    QJsonObject fmt = doc.object().value("format").toObject();
    p.duration = fmt.value("duration").toString().toDouble();
    p.size = fmt.value("size").toString().toLongLong();
    p.bitrate = fmt.value("bit_rate").toString().toLongLong();
    p.format = fmt.value("format_name").toString();
    for (const QJsonValue &v : doc.object().value("streams").toArray()) {
        QJsonObject o = v.toObject();
        Stream s;
        s.index = o.value("index").toInt();
        s.type = o.value("codec_type").toString();
        s.codec = o.value("codec_name").toString();
        QJsonObject tags = o.value("tags").toObject();
        s.lang = tags.value("language").toString();
        s.title = tags.value("title").toString();
        s.width = o.value("width").toInt();
        s.height = o.value("height").toInt();
        s.fps = rate_of(o.value("avg_frame_rate").toString());
        if (s.fps <= 0 || s.fps > 1000)
            s.fps = rate_of(o.value("r_frame_rate").toString());
        s.channels = o.value("channels").toInt();
        s.rate = o.value("sample_rate").toString().toInt();
        s.bits = o.value("bits_per_raw_sample").toString().toInt();
        s.bitrate = o.value("bit_rate").toString().toLongLong();
        s.cover = o.value("disposition").toObject().value("attached_pic").toInt() == 1;
        if (p.duration <= 0)
            p.duration = o.value("duration").toString().toDouble();
        p.streams << s;
    }
    p.ok = !p.streams.isEmpty();
    if (!p.ok)
        p.error = tx("it holds no video or audio");
    return p;
}

Probe probe(const QString &path)
{
    auto r = proc::run({"ffprobe", "-v", "error", "-print_format", "json", "-show_format", "-show_streams", path}, 60000);
    Probe p = parse_probe(r.out);
    if (!p.ok && r.rc != 0)
        p.error = QString::fromUtf8(r.err).trimmed().section('\n', -1);
    QFileInfo fi(path);
    if (p.size <= 0)
        p.size = fi.size();
    return p;
}

QString codec_name(const QString &c)
{
    static const QHash<QString, QString> names = {
        {"h264", "H.264"}, {"hevc", "H.265"},   {"vp9", "VP9"},   {"vp8", "VP8"},       {"av1", "AV1"},
        {"mpeg4", "MPEG-4"}, {"mpeg2video", "MPEG-2"}, {"prores", "ProRes"}, {"aac", "AAC"}, {"mp3", "MP3"},
        {"opus", "Opus"},  {"vorbis", "Vorbis"}, {"flac", "FLAC"}, {"alac", "ALAC"},     {"ac3", "AC-3"},
        {"eac3", "E-AC-3"}, {"dts", "DTS"},     {"truehd", "TrueHD"}, {"gif", "GIF"}};
    if (names.contains(c))
        return names[c];
    return c.startsWith("pcm_") ? QString("PCM") : c.toUpper();
}

QString clock(double secs)
{
    qint64 s = qint64(secs + 0.5);
    return s >= 3600 ? QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))
                     : QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

QString describe(const Probe &p)
{
    if (!p.ok)
        return p.error;
    QStringList parts;
    if (p.duration > 0)
        parts << clock(p.duration);
    const Stream *v = p.video();
    if (v && v->width)
        parts << QString("%1×%2").arg(v->width).arg(v->height);
    QStringList codecs;
    if (v)
        codecs << codec_name(v->codec);
    QList<Stream> a = p.audio();
    if (!a.isEmpty())
        codecs << (a.size() > 1 ? txn("%1 (%Ln tracks)", a.size()).arg(codec_name(a.first().codec)) : codec_name(a.first().codec));
    if (!codecs.isEmpty())
        parts << codecs.join(" / ");
    if (p.size > 0)
        parts << human_size(p.size);
    return parts.join("  ·  ");
}

QSet<QString> parse_encoders(const QByteArray &out)
{
    // " V....D libx264              libx264 H.264 / AVC / MPEG-4 AVC…"
    QSet<QString> encs;
    static const QRegularExpression line("^\\s*[VAS][A-Z.]{5}\\s+(\\S+)\\s");
    for (const QString &l : QString::fromUtf8(out).split('\n')) {
        auto m = line.match(l);
        if (m.hasMatch() && m.captured(1) != "=")
            encs << m.captured(1);
    }
    return encs;
}

QSet<QString> encoders()
{
    static QMutex lock;
    static bool asked = false;
    static QSet<QString> encs;
    QMutexLocker l(&lock);
    if (!asked) {
        asked = true;
        if (which("ffmpeg"))
            encs = parse_encoders(proc::run({"ffmpeg", "-hide_banner", "-encoders"}, 20000).out);
    }
    return encs;
}

QStringList hardware(const QSet<QString> &encs)
{
    QStringList out;
    if (encs.contains("h264_vaapi") && QFileInfo::exists("/dev/dri/renderD128"))
        out << "vaapi";
    if (encs.contains("h264_nvenc") && QFileInfo::exists("/dev/nvidia0"))
        out << "nvenc";
    return out;
}

double parse_time(const QString &text, bool *ok)
{
    QString t = text.trimmed();
    if (ok)
        *ok = false;
    if (t.isEmpty())
        return 0;
    double secs = 0;
    for (const QString &part : t.split(':')) {
        bool good = false;
        double v = part.toDouble(&good);
        if (!good || v < 0)
            return 0;
        secs = secs * 60 + v;
    }
    if (ok)
        *ok = t.split(':').size() <= 3;
    return secs;
}

// ---------------------------------------------------------------- videos

QJsonObject VideoSpec::to_json() const
{
    return {{"container", container}, {"vcodec", vcodec}, {"quality", quality},     {"target_mb", target_mb},
            {"height", height},       {"fps", fps},       {"audio", audio},         {"abitrate", abitrate},
            {"speed", speed},         {"start", start},   {"end", end},             {"rotate", rotate},
            {"flip", flip},           {"hw", hw},         {"extra", extra}};
}

VideoSpec VideoSpec::from_json(const QJsonObject &o)
{
    VideoSpec s;
    s.container = o.value("container").toString(s.container);
    s.vcodec = o.value("vcodec").toString(s.vcodec);
    s.quality = std::clamp(o.value("quality").toInt(s.quality), 0, 100);
    s.target_mb = std::max(0.0, o.value("target_mb").toDouble());
    s.height = o.value("height").toInt();
    s.fps = o.value("fps").toInt();
    s.audio = o.value("audio").toString(s.audio);
    s.abitrate = o.value("abitrate").toInt(s.abitrate);
    s.speed = std::clamp(o.value("speed").toInt(s.speed), 0, 4);
    s.start = o.value("start").toString();
    s.end = o.value("end").toString();
    s.rotate = o.value("rotate").toInt();
    s.flip = o.value("flip").toBool();
    s.hw = o.value("hw").toString();
    s.extra = o.value("extra").toString();
    return s;
}

bool audio_only(const QString &c) { return c == "mp3" || c == "m4a" || c == "opus"; }

QStringList video_containers() { return {"mp4", "mkv", "webm", "mov", "gif", "mp3", "m4a", "opus"}; }

QString video_ext(const VideoSpec &s) { return "." + s.container; }

QList<Preset> video_presets(const QString &source_ext)
{
    QList<Preset> l;
    VideoSpec mp4;
    l << Preset{"mp4", TX_NOOP("MP4 (H.264), plays everywhere"), mp4};
    VideoSpec small = mp4;
    small.vcodec = "hevc", small.quality = 50, small.abitrate = 128;
    l << Preset{"smaller", TX_NOOP("Smaller file (H.265)"), small};
    VideoSpec webm;
    webm.container = "webm", webm.vcodec = "vp9", webm.audio = "encode", webm.abitrate = 128;
    l << Preset{"webm", TX_NOOP("WebM for the web"), webm};
    for (const auto &[id, name] : QList<QPair<QString, QString>>{{"mp3", TX_NOOP("Audio only (MP3)")},
                                                                 {"m4a", TX_NOOP("Audio only (M4A)")},
                                                                 {"opus", TX_NOOP("Audio only (Opus)")}}) {
        VideoSpec a;
        a.container = id, a.audio = "encode", a.abitrate = id == "opus" ? 128 : 192;
        l << Preset{"audio_" + id, name, a};
    }
    VideoSpec gif;
    gif.container = "gif", gif.vcodec = "gif", gif.height = 480, gif.fps = 12, gif.audio = "remove";
    l << Preset{"gif", "GIF", gif};
    VideoSpec same;
    QString ext = source_ext.mid(1).toLower();
    same.container = QStringList{"mp4", "mkv", "webm", "mov"}.contains(ext) ? ext : QString("mkv");
    same.vcodec = same.container == "webm" ? "vp9" : "hevc";
    same.quality = 45;
    l << Preset{"same", TX_NOOP("Same format, just smaller"), same};
    return l;
}

namespace {
struct Codec {
    QString sw;             // the software encoder
    QString alt;            // another one, if the first isn't there
    int best, worst;        // CRF at quality 100 and 0
    QStringList presets;    // speed 0..4
};
const QHash<QString, Codec> &codecs()
{
    static const QHash<QString, Codec> c = {
        {"h264", {"libx264", "", 16, 35, {"veryfast", "faster", "medium", "slow", "slower"}}},
        {"hevc", {"libx265", "", 18, 38, {"veryfast", "faster", "medium", "slow", "slower"}}},
        {"vp9", {"libvpx-vp9", "", 18, 50, {"5", "4", "2", "1", "0"}}},
        {"av1", {"libsvtav1", "libaom-av1", 20, 55, {"12", "10", "8", "6", "4"}}},
    };
    return c;
}

int crf(const Codec &c, int quality) { return int(std::lround(c.worst + (c.best - c.worst) * quality / 100.0)); }

// the audio codecs each container takes as they are
bool takes_audio(const QString &container, const QString &codec)
{
    if (container == "mkv")
        return true;
    if (container == "webm")
        return codec == "opus" || codec == "vorbis";
    if (container == "mp4" || container == "mov")
        return QStringList{"aac", "mp3", "ac3", "eac3", "alac", "opus"}.contains(codec);
    return false;
}
}  // namespace

QString video_encoder(const VideoSpec &s, const QSet<QString> &encs)
{
    if (s.vcodec == "copy" || audio_only(s.container))
        return s.vcodec == "copy" ? QString("copy") : QString();
    if (s.container == "gif")
        return encs.contains("gif") ? QString("gif") : QString();
    if (!codecs().contains(s.vcodec))
        return QString();
    if (s.hw == "vaapi" || s.hw == "nvenc") {
        QString e = s.vcodec + "_" + s.hw;
        if (encs.contains(e))
            return e;
    }
    const Codec &c = codecs()[s.vcodec];
    if (encs.contains(c.sw))
        return c.sw;
    return !c.alt.isEmpty() && encs.contains(c.alt) ? c.alt : QString();
}

static QString audio_encoder_for(const QString &container)
{
    return container == "webm" || container == "opus" ? "libopus" : container == "mp3" ? "libmp3lame" : "aac";
}

Args video_args(const VideoSpec &s, const Probe &p, const QSet<QString> &encs)
{
    Args a;
    // trimming: -ss before -i (fast, and exact when re-encoding), -t for the length
    bool ok_start = true, ok_end = true;
    double start = parse_time(s.start, &ok_start), end = parse_time(s.end, &ok_end);
    if ((!s.start.trimmed().isEmpty() && !ok_start) || (!s.end.trimmed().isEmpty() && !ok_end)) {
        a.error = tx("The start and end times are written as seconds or minutes:seconds (1:23).");
        return a;
    }
    if (!s.end.trimmed().isEmpty() && end <= start) {
        a.error = tx("The end time comes before the start.");
        return a;
    }
    if (start > 0)
        a.input << "-ss" << QString::number(start, 'f', 3);
    if (!s.end.trimmed().isEmpty())
        a.output << "-t" << QString::number(end - start, 'f', 3);
    QList<Stream> audio = p.audio();
    const Stream *v = p.video();
    double length = std::max(0.0, (!s.end.trimmed().isEmpty() ? end : p.duration) - start);

    if (audio_only(s.container)) {
        if (audio.isEmpty()) {
            a.error = tx("It has no sound to keep.");
            return a;
        }
        QString enc = audio_encoder_for(s.container);
        if (!encs.contains(enc)) {
            a.error = tx("This ffmpeg can't write %1 (%2).").arg(s.container.toUpper(), enc);
            return a;
        }
        a.output << "-vn" << "-map" << "0:a:0" << "-c:a" << enc << "-b:a" << QString("%1k").arg(s.abitrate);
        if (s.container == "mp3")
            a.output << "-id3v2_version" << "3";
        a.output << "-map_metadata" << "0";
    } else {
        QString enc = video_encoder(s, encs);
        if (enc.isEmpty()) {
            a.error = s.container == "gif" ? tx("This ffmpeg can't write GIFs.")
                                           : tx("This ffmpeg has no %1 encoder.").arg(codec_name(s.vcodec));
            return a;
        }
        if (!v) {
            a.error = tx("It has no video.");
            return a;
        }
        bool copy = enc == "copy";
        QStringList filters;
        if (!copy) {
            if (s.flip)
                filters << "hflip";
            if (s.rotate == 90)
                filters << "transpose=1";
            else if (s.rotate == 270)
                filters << "transpose=2";
            else if (s.rotate == 180)
                filters << "hflip" << "vflip";
            if (s.container == "gif" && s.fps > 0)
                filters << QString("fps=%1").arg(s.fps);
            int w = v->width, h = v->height;
            if (s.rotate == 90 || s.rotate == 270)
                std::swap(w, h);
            if (s.height > 0 && std::min(w, h) > s.height)   // the shorter side, never upscaled
                filters << (w >= h ? QString("scale=-2:%1").arg(s.height) : QString("scale=%1:-2").arg(s.height)) +
                               (s.container == "gif" ? ":flags=lanczos" : "");
        }
        if (s.container == "gif") {
            QString chain = filters.isEmpty() ? QString() : filters.join(',') + ",";
            a.output << "-vf" << chain + "split[a][b];[a]palettegen[p];[b][p]paletteuse" << "-loop" << "0" << "-an";
            a.output << "-map" << "0:v:0";
            return a;
        }
        a.output << "-map" << "0:v:0";
        if (s.audio != "remove" && !audio.isEmpty())
            a.output << "-map" << "0:a?";
        if (copy) {
            a.output << "-c:v" << "copy";
        } else {
            if (s.hw == "vaapi" && enc.endsWith("_vaapi")) {
                a.input.prepend("/dev/dri/renderD128");
                a.input.prepend("-vaapi_device");
                filters << "format=nv12" << "hwupload";
            }
            if (!filters.isEmpty())
                a.output << "-vf" << filters.join(',');
            a.output << "-c:v" << enc;
            const Codec &c = codecs()[s.vcodec];
            int q = crf(c, s.quality);
            if (s.target_mb > 0 && length > 0) {
                double audio_kbps = s.audio == "remove" || audio.isEmpty()
                                        ? 0
                                        : s.audio == "encode" ? s.abitrate : std::max<qint64>(audio.first().bitrate / 1000, 128);
                double kbps = std::max(100.0, s.target_mb * 8 * 1024 * 1024 / 1000 / length - audio_kbps);
                a.output << "-b:v" << QString("%1k").arg(qint64(kbps));
            } else if (enc.endsWith("_vaapi")) {
                a.output << "-qp" << QString::number(q);
            } else if (enc.endsWith("_nvenc")) {
                a.output << "-cq" << QString::number(q);
            } else {
                a.output << "-crf" << QString::number(q);
                if (enc == "libvpx-vp9" || enc == "libaom-av1")
                    a.output << "-b:v" << "0";
            }
            int sp = std::clamp(s.speed, 0, 4);
            if (enc == "libx264" || enc == "libx265")
                a.output << "-preset" << c.presets[sp];
            else if (enc == "libvpx-vp9")
                a.output << "-deadline" << "good" << "-cpu-used" << c.presets[sp] << "-row-mt" << "1";
            else if (enc == "libsvtav1")
                a.output << "-preset" << c.presets[sp];
            else if (enc == "libaom-av1")
                a.output << "-cpu-used" << QString::number(8 - sp) << "-row-mt" << "1";
            else if (enc.endsWith("_nvenc"))
                a.output << "-preset" << QString("p%1").arg(QStringList{"1", "3", "4", "6", "7"}[sp]);
            if (!enc.endsWith("_vaapi") && !enc.endsWith("_nvenc"))
                a.output << "-pix_fmt" << "yuv420p";   // (plays everywhere: 10-bit and 4:4:4 sources too)
            if (s.vcodec == "hevc" && (s.container == "mp4" || s.container == "mov"))
                a.output << "-tag:v" << "hvc1";   // (QuickTime and Apple devices want it)
            if (s.fps > 0)
                a.output << "-r" << QString::number(s.fps);
        }
        if (s.audio == "remove" || audio.isEmpty()) {
            a.output << "-an";
        } else {
            bool fits = std::all_of(audio.begin(), audio.end(),
                                    [&](const Stream &x) { return takes_audio(s.container, x.codec); });
            if (s.audio == "keep" && fits) {
                a.output << "-c:a" << "copy";
            } else {
                QString aenc = audio_encoder_for(s.container);
                a.output << "-c:a" << aenc << "-b:a" << QString("%1k").arg(s.abitrate);
            }
        }
        a.output << "-map_metadata" << "0";
        if (s.container == "mp4" || s.container == "mov")
            a.output << "-movflags" << "+faststart";
    }
    if (!s.extra.trimmed().isEmpty())
        a.output << QProcess::splitCommand(s.extra);
    return a;
}

double estimate_mb(const VideoSpec &s, const Probe &p)
{
    if (!p.ok || p.duration <= 0)
        return -1;
    bool ok = true;
    double start = parse_time(s.start, &ok);
    double end = s.end.trimmed().isEmpty() ? p.duration : parse_time(s.end, &ok);
    double length = std::max(0.0, std::min(end, p.duration) - start);
    if (length <= 0)
        return -1;
    double audio_kbps = 0;
    QList<Stream> audio = p.audio();
    if (!audio.isEmpty() && s.audio != "remove" && s.container != "gif")
        audio_kbps = s.audio == "encode" || audio_only(s.container) ? s.abitrate
                                                                    : std::max<qint64>(audio.first().bitrate / 1000, 128);
    if (audio_only(s.container))
        return audio_kbps * 1000 / 8 * length / 1e6;
    if (s.target_mb > 0)
        return s.target_mb;
    const Stream *v = p.video();
    if (!v)
        return -1;
    if (s.vcodec == "copy")
        return (p.bitrate > 0 ? p.bitrate / 8.0 * length : p.size * length / p.duration) / 1e6;
    int w = v->width, h = v->height;
    if (s.height > 0 && std::min(w, h) > s.height) {
        double k = double(s.height) / std::min(w, h);
        w = int(w * k), h = int(h * k);
    }
    double fps = s.fps > 0 ? s.fps : (v->fps > 0 ? v->fps : 30);
    if (s.container == "gif")
        return w * h * fps * length * 0.6 / 8 / 1e6;   // (GIFs are big: well over a bit per pixel)
    // bits per pixel at each end of the quality slider, for H.264; the newer codecs need less for the same look
    double bpp = 0.02 * std::pow(0.18 / 0.02, s.quality / 100.0);
    double k = s.vcodec == "hevc" ? 0.6 : s.vcodec == "vp9" ? 0.65 : s.vcodec == "av1" ? 0.5 : 1.0;
    double video_bps = std::min(w * h * fps * bpp * k, v->bitrate > 0 ? v->bitrate * 1.3 : 1e12);
    return (video_bps / 8 + audio_kbps * 1000 / 8) * length / 1e6;
}

// ---------------------------------------------------------------- audio

QStringList audio_formats(const QSet<QString> &encs)
{
    QStringList out{"copy"};
    static const QList<QPair<QString, QString>> need = {{"mp3", "libmp3lame"}, {"m4a", "aac"},   {"opus", "libopus"},
                                                        {"ogg", "libvorbis"},  {"flac", "flac"}, {"wav", "pcm_s16le"},
                                                        {"alac", "alac"}};
    for (const auto &[f, enc] : need)
        if (encs.contains(enc))
            out << f;
    return out;
}

bool lossless(const QString &f) { return f == "flac" || f == "wav" || f == "alac" || f == "copy"; }

QString audio_ext(const QString &format, const QString &codec)
{
    if (format == "copy") {
        static const QHash<QString, QString> by = {{"aac", ".m4a"},  {"alac", ".m4a"}, {"mp3", ".mp3"}, {"opus", ".opus"},
                                                   {"vorbis", ".ogg"}, {"flac", ".flac"}};
        return by.value(codec, ".mka");
    }
    return format == "alac" ? QString(".m4a") : "." + format;
}

QString muxer(const QString &ext)
{
    static const QHash<QString, QString> m = {{".mp4", "mp4"},   {".mkv", "matroska"}, {".mka", "matroska"},
                                              {".webm", "webm"}, {".mov", "mov"},      {".gif", "gif"},
                                              {".mp3", "mp3"},   {".m4a", "ipod"},     {".opus", "opus"},
                                              {".ogg", "ogg"},   {".flac", "flac"},    {".wav", "wav"}};
    return m.value(ext.toLower());
}

QString loudnorm_filter(const QString &measured)
{
    QJsonObject m = QJsonDocument::fromJson(measured.toUtf8()).object();
    QString f = "loudnorm=I=-16:TP=-1.5:LRA=11";
    if (m.isEmpty())
        return f;
    return f + QString(":measured_I=%1:measured_TP=%2:measured_LRA=%3:measured_thresh=%4:offset=%5:linear=true")
                   .arg(m.value("input_i").toString(), m.value("input_tp").toString(), m.value("input_lra").toString(),
                        m.value("input_thresh").toString(), m.value("target_offset").toString());
}

Args audio_args(const AudioSpec &s, const Probe &p, int track, const QString &measured)
{
    Args a;
    QList<Stream> audio = p.audio();
    if (audio.isEmpty()) {
        a.error = tx("It has no sound.");
        return a;
    }
    int t = std::clamp(track < 0 ? 0 : track, 0, int(audio.size()) - 1);
    const Stream &src = audio[t];
    QString ext = audio_ext(s.format, src.codec);
    bool cover_ok = s.cover && (ext == ".mp3" || ext == ".m4a" || ext == ".flac");
    a.output << "-map" << QString("0:a:%1").arg(t);
    const Stream *pic = nullptr;
    for (const Stream &x : p.streams)
        if (x.cover)
            pic = &x;
    if (cover_ok && pic)
        a.output << "-map" << QString("0:%1").arg(pic->index) << "-c:v" << "copy" << "-disposition:v:0" << "attached_pic";
    else
        a.output << "-vn";
    if (s.format == "copy") {
        a.output << "-c:a" << "copy";
    } else {
        static const QHash<QString, QString> enc = {{"mp3", "libmp3lame"}, {"m4a", "aac"},   {"opus", "libopus"},
                                                    {"ogg", "libvorbis"},  {"flac", "flac"}, {"alac", "alac"}};
        QString e = s.format == "wav" ? (src.bits > 16 ? QString("pcm_s24le") : QString("pcm_s16le")) : enc.value(s.format);
        a.output << "-c:a" << e;
        if (!lossless(s.format))
            a.output << "-b:a" << QString("%1k").arg(s.bitrate);
        if (s.format == "opus" && s.rate == 0 && src.rate && src.rate != 48000 && src.rate != 24000 && src.rate != 16000)
            a.output << "-ar" << "48000";   // (Opus only takes these rates)
        else if (s.rate > 0)
            a.output << "-ar" << QString::number(s.rate);
        else if (s.loudnorm && src.rate > 0)
            a.output << "-ar" << QString::number(src.rate);   // (loudnorm works at 192 kHz: back to the source's)
        if (s.channels > 0)
            a.output << "-ac" << QString::number(s.channels);
        if (s.loudnorm)
            a.output << "-af" << loudnorm_filter(measured);
    }
    a.output << "-map_metadata" << "0";
    if (ext == ".mp3")
        a.output << "-id3v2_version" << "3";
    return a;
}

// ---------------------------------------------------------------- running

QString output_path(const QString &src, const QString &dir, const QString &suffix, const QString &ext)
{
    auto [stem, old_ext] = split_ext(basename(src));
    return unique_path(dir, stem + suffix + ext, "num");
}

QStringList command(const QString &src, const Args &a, const QString &dst)
{
    return QStringList{"ffmpeg", "-hide_banner"} + a.input + QStringList{"-i", src} + a.output + QStringList{dst};
}

double progress_time(const QString &line, double *speed)
{
    if (speed && line.startsWith("speed=")) {
        QString v = line.mid(6).trimmed();
        v.chop(v.endsWith('x') ? 1 : 0);
        bool ok = false;
        double sp = v.toDouble(&ok);
        if (ok)
            *speed = sp;
        return -1;
    }
    if (line.startsWith("out_time_us=") || line.startsWith("out_time_ms=")) {   // (both are microseconds)
        bool ok = false;
        qint64 us = line.section('=', 1).trimmed().toLongLong(&ok);
        return ok && us >= 0 ? us / 1e6 : -1;
    }
    return -1;
}

// ffmpeg with its progress on stdout and its messages on stderr; *err gets the messages' last lines
static int run_ffmpeg(Task *task, const QStringList &argv, double duration, const QString &label, double base,
                      double share, QByteArray *err)
{
    proc::Options o;
    o.c_utf8 = true;
    auto p = proc::spawn(argv, o);
    QByteArray out_buf;
    double speed = 0, done = 0;
    auto take = [&](int fd, bool is_out) {
        char buf[16384];
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0)
            return false;
        QByteArray chunk(buf, int(n));
        if (!is_out) {
            task->log_output(chunk);
            *err += chunk;
            if (err->size() > 65536)
                err->remove(0, err->size() - 16384);
            return true;
        }
        out_buf += chunk;
        int nl;
        while ((nl = out_buf.indexOf('\n')) >= 0) {
            QString line = QString::fromUtf8(out_buf.left(nl)).trimmed();
            out_buf.remove(0, nl + 1);
            double t = progress_time(line, &speed);
            if (t >= 0)
                done = t;
            if (line.startsWith("progress=") && duration > 0) {
                double f = std::clamp(done / duration, 0.0, 1.0);
                QString text = QString("%1: %2%").arg(label, QString::number(int(f * 100)));
                if (speed > 0 && f < 1)
                    text += "  ·  " + fileops::wait_text(qint64((duration - done) / speed * 1000)) + " left";
                if (speed > 0)
                    text += QString("  ·  %1×").arg(speed, 0, 'f', 1);
                task->report(base + share * f, 1, text);
            }
        }
        return true;
    };
    bool out_open = true, err_open = true;
    while (out_open || err_open) {
        task->check();   // a stop throws: p goes, and ffmpeg with it
        fd_set rf;
        FD_ZERO(&rf);
        int top = 0;
        if (out_open) {
            FD_SET(p->out, &rf);
            top = std::max(top, p->out);
        }
        if (err_open) {
            FD_SET(p->err, &rf);
            top = std::max(top, p->err);
        }
        struct timeval tv = {0, 200000};
        int r = ::select(top + 1, &rf, nullptr, nullptr, &tv);
        if (r <= 0)
            continue;
        if (out_open && FD_ISSET(p->out, &rf))
            out_open = take(p->out, true);
        if (err_open && FD_ISSET(p->err, &rf))
            err_open = take(p->err, false);
    }
    return p->wait();
}

static QString last_lines(const QByteArray &err)
{
    QStringList lines = QString::fromUtf8(err).trimmed().split('\n', Qt::SkipEmptyParts);
    return lines.mid(std::max(0, int(lines.size()) - 3)).join('\n').trimmed();
}

QString run(Task *task, const QString &src, const Args &a, const QString &dst_in, double duration, const QString &label,
            double base, double share)
{
    QString dst = dst_in;
    QString part = join(dirname(dst), part_name());
    QString fmt = muxer(ext_of(dst));
    QStringList argv = QStringList{"ffmpeg", "-hide_banner", "-nostdin", "-nostats", "-progress", "pipe:1", "-n"} +
                       a.input + QStringList{"-i", src} + a.output;
    if (!fmt.isEmpty())
        argv << "-f" << fmt;
    argv << part;
    task->log("  " + command(src, a, dst).join(' '));
    struct Cleanup {
        QString part;
        bool keep = false;
        ~Cleanup()
        {
            if (!keep)
                ::unlink(enc(part).constData());   // a cancel or a failure: no half-made file
        }
    } cleanup{part};
    QByteArray err;
    int rc = run_ffmpeg(task, argv, duration, label, base, share, &err);
    if (rc != 0)
        throw Error(tx("ffmpeg couldn't convert %1:\n%2").arg(basename(src), last_lines(err)));
    try {
        put_new(part, dst);
    } catch (const OSError &e) {
        if (e.code != EEXIST)
            throw;
        dst = unique_path(dirname(dst), basename(dst), "num");   // taken meanwhile: the next free name
        put_new(part, dst);
    }
    cleanup.keep = true;
    return dst;
}

QString measure_loudness(Task *task, const QString &src, int track)
{
    QStringList argv = {"ffmpeg", "-hide_banner", "-nostdin", "-nostats", "-i", src, "-map",
                        QString("0:a:%1").arg(std::max(0, track)), "-af", "loudnorm=I=-16:TP=-1.5:LRA=11:print_format=json",
                        "-f", "null", "-"};
    QByteArray err;
    int rc = run_ffmpeg(task, argv, 0, QString(), 0, 0, &err);
    if (rc != 0)
        throw Error(tx("ffmpeg couldn't measure the loudness of %1:\n%2").arg(basename(src), last_lines(err)));
    QString text = QString::fromUtf8(err);
    int open = text.lastIndexOf('{'), close = text.lastIndexOf('}');
    return open >= 0 && close > open ? text.mid(open, close - open + 1) : QString();
}

// ---------------------------------------------------------------- images

QList<ImageTarget> image_targets()
{
    QList<QByteArray> qt = QImageWriter::supportedImageFormats();
    bool heif_enc = which("heif-enc");
    QList<ImageTarget> out;
    auto add = [&](const QString &id, const QString &name, const QString &ext, bool lossy, bool can) {
        if (can)
            out << ImageTarget{id, name, ext, lossy};
    };
    add("jpg", "JPEG", ".jpg", true, qt.contains("jpg"));
    add("png", "PNG", ".png", false, qt.contains("png"));
    add("webp", "WebP", ".webp", true, qt.contains("webp"));
    add("avif", "AVIF", ".avif", true, qt.contains("avif") || heif_enc);
    add("heic", "HEIC", ".heic", true, qt.contains("heic") || qt.contains("heif") || heif_enc);
    add("tiff", "TIFF", ".tiff", false, qt.contains("tiff"));
    return out;
}

QSize resized(QSize from, const ImageSpec &s)
{
    if (s.resize == "long" && s.size > 0 && std::max(from.width(), from.height()) > s.size)
        return from.scaled(s.size, s.size, Qt::KeepAspectRatio);
    if (s.resize == "percent" && s.size > 0 && s.size != 100)
        return QSize(std::max(1, from.width() * s.size / 100), std::max(1, from.height() * s.size / 100));
    return from;
}

static QImage decode(const QString &src)
{
    QImageReader reader(src);
    reader.setAutoTransform(true);
    QImage img = reader.read();
    if (!img.isNull() || !which("ffmpeg"))
        return img;
    // what Qt's plugins can't read: ffmpeg's first frame, as PNG
    auto r = proc::run({"ffmpeg", "-v", "error", "-nostdin", "-i", src, "-frames:v", "1", "-f", "image2pipe", "-c:v",
                        "png", "-"},
                       120000);
    return r.rc == 0 ? QImage::fromData(r.out, "PNG") : QImage();
}

void convert_image(const QString &src, const QString &dst, const ImageSpec &s)
{
    QImage img = decode(src);
    if (img.isNull())
        throw Error(tx("Couldn't read %1.").arg(basename(src)));
    QSize to = resized(img.size(), s);
    if (to != img.size())
        img = img.scaled(to, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    bool alpha_ok = s.format == "png" || s.format == "webp" || s.format == "avif" || s.format == "heic" ||
                    s.format == "tiff";
    if (img.hasAlphaChannel() && !alpha_ok) {   // flattened onto the background chosen
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.setColorSpace(img.colorSpace());
        flat.fill(QColor(s.background));
        QPainter p(&flat);
        p.drawImage(0, 0, img);
        p.end();
        img = flat;
    }
    if (!s.keep_meta) {   // a PNG's text chunks (prompts…) go too: a copy without them
        QImage bare(img.constBits(), img.width(), img.height(), img.bytesPerLine(), img.format());
        QImage copy = bare.copy();
        copy.setColorSpace(img.colorSpace());
        img = copy;
    }
    QByteArray data;
    QList<QByteArray> qt = QImageWriter::supportedImageFormats();
    QByteArray fmt = s.format == "heic" ? QByteArray(qt.contains("heic") ? "heic" : qt.contains("heif") ? "heif" : "")
                                        : s.format.toLatin1();
    if (!fmt.isEmpty() && qt.contains(fmt)) {
        QBuffer buf(&data);
        buf.open(QIODevice::WriteOnly);
        QImageWriter w(&buf, fmt);
        bool lossy = s.format == "jpg" || s.format == "webp" || s.format == "avif" || s.format == "heic";
        if (lossy)
            w.setQuality(s.quality);
        if (!w.write(img))
            throw Error(tx("Couldn't write %1: %2").arg(basename(dst), w.errorString()));
    } else if (which("heif-enc") && (s.format == "heic" || s.format == "avif")) {
        // heif-enc (libheif) from a lossless PNG in Kestrel's cache
        QString dir = join(APP_CACHE(), "convert");
        QString in = join(dir, part_name() + ".png"), out = join(dir, part_name() + "." + s.format);
        makedirs(dir, true);
        struct Gone {
            QStringList files;
            ~Gone()
            {
                for (const QString &f : files)
                    ::unlink(enc(f).constData());
            }
        } gone{{in, out}};
        if (!img.save(in, "PNG"))
            throw Error(tx("Couldn't prepare %1.").arg(basename(src)));
        QStringList argv = {"heif-enc", "-q", QString::number(s.quality)};
        if (s.format == "avif")
            argv << "-A";
        auto r = proc::run(argv + QStringList{"-o", out, in}, 600000);
        if (r.rc != 0)
            throw Error(tx("heif-enc couldn't write %1: %2").arg(basename(dst), QString::fromUtf8(r.err).trimmed()));
        data = read_file(out);
    } else {
        throw Error(tx("Nothing installed here can write %1.").arg(s.format.toUpper()));
    }
    if (s.keep_meta)
        data = imgedit::with_metadata(src, data, ext_of(dst));
    write_parts(dst, [&](int fd) { write_all(fd, data.constData(), data.size(), dst); }, true);
}

}  // namespace media
