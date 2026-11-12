// Converting media files: images (Qt's image plugins, heif-enc, ffmpeg to decode what Qt can't), videos, the audio
// in videos and audio files (ffmpeg, ffprobe). What's built here is pure (the ffmpeg options from the settings, the
// reading of ffprobe's and ffmpeg's answers) so it's tested without running anything; run() runs one conversion in
// a job.
//
// Every output is written under a hidden part name beside where it goes (util part_name()) and given its real name
// only when whole, never replacing a file (put_new): a cancel, a failure or a crash leaves no half-made file.
#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QSize>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

class Task;

namespace media {

bool have_ffmpeg();   // ffmpeg and ffprobe are installed

// ---------------------------------------------------------------- what a file holds (ffprobe)

struct Stream {
    int index = 0;          // in the file
    QString type;           // "video", "audio", "subtitle"…
    QString codec;          // ffmpeg's name: h264, hevc, aac, opus, mp3…
    QString lang, title;
    int width = 0, height = 0;
    double fps = 0;
    int channels = 0, rate = 0, bits = 0;
    qint64 bitrate = 0;
    bool cover = false;     // a cover picture attached to audio (attached_pic)
};

struct Probe {
    bool ok = false;
    QString error;
    double duration = 0;    // seconds
    qint64 size = 0, bitrate = 0;
    QString format;
    QList<Stream> streams;
    const Stream *video() const;    // the first real video stream (not a cover picture), or null
    QList<Stream> audio() const;
};
Probe parse_probe(const QByteArray &json);   // ffprobe -print_format json -show_format -show_streams
Probe probe(const QString &path);            // runs ffprobe (blocks)
QString describe(const Probe &p);            // "1:23 · 1920×1080 · H.264 / AAC · 45 MB"
QString codec_name(const QString &codec);    // "H.264", "AAC"…

// The encoders the installed ffmpeg has (ffmpeg -encoders; asked once)
QSet<QString> encoders();
QSet<QString> parse_encoders(const QByteArray &out);
// Hardware encoding that might work here: "vaapi" (a render node and a *_vaapi encoder), "nvenc"
QStringList hardware(const QSet<QString> &encs);

double parse_time(const QString &text, bool *ok = nullptr);   // "83.5", "1:23", "1:02:03.5" → seconds
QString clock(double secs);                                   // 83.5 → "1:23"

// ---------------------------------------------------------------- videos

struct VideoSpec {
    QString container = "mp4";   // mp4 mkv webm mov gif, or audio only: mp3 m4a opus
    QString vcodec = "h264";     // h264 hevc vp9 av1, or copy
    int quality = 60;            // 0 (smallest file) … 100 (best quality); CRF underneath
    double target_mb = 0;        // > 0: aim for this size instead of a quality
    int height = 0;              // 0: keep; else the shorter side (1080, 720…), never upscaled
    int fps = 0;                 // 0: keep
    QString audio = "keep";      // keep (copied when the container takes it), encode, remove
    int abitrate = 160;          // kbps, when encoding audio
    int speed = 2;               // 0 fastest … 4 smallest file (ffmpeg's preset)
    QString start, end;          // trim; empty: from the start / to the end
    int rotate = 0;              // 0 90 180 270, clockwise
    bool flip = false;
    QString hw;                  // "", "vaapi", "nvenc"
    QString extra;               // options passed to ffmpeg as they are
    QJsonObject to_json() const;
    static VideoSpec from_json(const QJsonObject &o);
    bool operator==(const VideoSpec &o) const { return to_json() == o.to_json(); }
};
struct Preset {
    QString id, name;
    VideoSpec spec;
};
QList<Preset> video_presets(const QString &source_ext);   // "Same format, just smaller" follows the source's type
bool audio_only(const QString &container);
QString video_ext(const VideoSpec &s);                     // ".mp4"
QString video_encoder(const VideoSpec &s, const QSet<QString> &encs);   // libx264…; empty: not installed
QStringList video_containers();                            // in the order offered
// ffmpeg's options for a video: those before -i (trimming, hardware) and after it; *error: what can't be done
struct Args {
    QStringList input, output;
    QString error;
};
Args video_args(const VideoSpec &s, const Probe &p, const QSet<QString> &encs);
double estimate_mb(const VideoSpec &s, const Probe &p);   // a rough guess; < 0: none

// ---------------------------------------------------------------- audio (from videos, and audio files)

struct AudioSpec {
    QString format = "copy";   // copy (as it is), mp3 m4a opus ogg flac wav alac
    int bitrate = 192;         // kbps, for lossy formats
    int rate = 0;              // Hz; 0: keep
    int channels = 0;          // 0: keep, 1 mono, 2 stereo
    bool loudnorm = false;     // two passes of ffmpeg's loudnorm, so a batch plays at the same volume
    bool cover = true;         // keep tags and a cover picture where the format takes one
};
QStringList audio_formats(const QSet<QString> &encs);   // the formats that can be written here
QString audio_ext(const QString &format, const QString &source_codec);   // copy: aac → .m4a, opus → .opus, others → .mka
bool lossless(const QString &format);
// The options for audio stream `track` (its index among the audio streams; -1: the first) of p
Args audio_args(const AudioSpec &s, const Probe &p, int track, const QString &loudnorm_measured = QString());
QString loudnorm_filter(const QString &measured_json);   // the second pass's filter from the first's measurements

QString muxer(const QString &ext);   // ffmpeg's -f for an extension (".m4a" → "ipod"); a part file has none of its own

// ---------------------------------------------------------------- running

// A free name in dir for src's stem + suffix + ext ("clip.converted.mp4", "clip.converted (2).mp4")
QString output_path(const QString &src, const QString &dir, const QString &suffix, const QString &ext);
// The command, as shown in the window (the real one writes to a part file and reports its progress)
QStringList command(const QString &src, const Args &a, const QString &dst);
// Runs ffmpeg for src into dst (which must stay free): a part file in dst's folder, renamed when finished; progress
// reported on task (base..base+share of the job), `label` in its text. Raises Error (with ffmpeg's last lines);
// Cancelled when stopped. Returns where it went (dst, or the next free name if dst was taken meanwhile).
QString run(Task *task, const QString &src, const Args &a, const QString &dst, double duration, const QString &label,
            double base = 0, double share = 1);
QString measure_loudness(Task *task, const QString &src, int track);   // the first loudnorm pass's measurements

// Progress lines from ffmpeg's -progress: seconds done so far, or -1; *speed: its speed (×) when given
double progress_time(const QString &line, double *speed = nullptr);

// ---------------------------------------------------------------- images

struct ImageTarget {
    QString id, name, ext;   // "jpg", "JPEG", ".jpg"
    bool lossy;
};
QList<ImageTarget> image_targets();   // the formats that can be written here (Qt's writers, heif-enc)
struct ImageSpec {
    QString format = "jpg";
    int quality = 90;          // lossy formats
    QString resize = "keep";   // keep, long (the longest side, in pixels), percent
    int size = 0;
    bool keep_meta = true;     // EXIF, XMP (camera, date, location); off: none
    QString background = "#ffffff";   // for transparency when the format has none
};
QSize resized(QSize from, const ImageSpec &s);
// Reads src (Qt's plugins, else ffmpeg), turned as its orientation says, resized, and writes it to dst (a new name)
void convert_image(const QString &src, const QString &dst, const ImageSpec &s);

}  // namespace media

Q_DECLARE_METATYPE(media::Probe)
