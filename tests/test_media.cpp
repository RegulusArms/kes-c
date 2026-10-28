// Converting media (media.h, convert.h): rotating images in their files (jpegtran, the orientation tag, lossless
// formats), Convert Image…, and with ffmpeg: Convert Video…, Extract Audio… and Convert Audio… (the options built
// from the settings, ffprobe's and ffmpeg's answers read, and real conversions of small files the test makes).
#include "common.h"

#include "convert.h"
#include "imgedit.h"
#include "media.h"
#include "metadata.h"

#include <QCheckBox>
#include <QComboBox>
#include <QImageWriter>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>

#include <csignal>
#include <sys/resource.h>

using namespace test;

static QStringList parts_in(const QString &dir)
{
    QStringList out;
    for (const QString &n : QDir(dir).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot))
        if (n.startsWith(".kes-"))
            out << n;
    return out;
}

static QString arg_after(const QStringList &argv, const QString &opt)
{
    int i = int(argv.indexOf(opt));
    return i >= 0 && i + 1 < argv.size() ? argv[i + 1] : QString();
}

// 64 × 48 (or w × h): red, green / blue, white quarters
static QImage quarters(int w = 64, int h = 48)
{
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.setPixel(x, y, y < h / 2 ? (x < w / 2 ? qRgb(255, 0, 0) : qRgb(0, 255, 0))
                                         : (x < w / 2 ? qRgb(0, 0, 255) : qRgb(255, 255, 255)));
    return img;
}

static QImage shown(const QString &path)
{
    QImageReader r(path);
    r.setAutoTransform(true);
    return r.read();
}

// the colours of the four corners: R G B W (K: none)
static QString corners(const QImage &img)
{
    if (img.isNull())
        return "null";
    auto name = [&](int x, int y) {
        QColor c = img.pixelColor(x, y);
        int r = c.red() > 128, g = c.green() > 128, b = c.blue() > 128;
        return r && g && b ? 'W' : r ? 'R' : g ? 'G' : b ? 'B' : 'K';
    };
    int w = img.width() - 3, h = img.height() - 3;
    return QString(QChar(name(2, 2))) + QChar(name(w, 2)) + QChar(name(2, h)) + QChar(name(w, h));
}

static Task *job_titled(const QString &start)
{
    for (Task *t : TaskBoard::instance()->mine())
        if (t->title.startsWith(start))
            return t;
    return nullptr;
}

static bool jobs_done()
{
    return TaskBoard::instance()->mine().isEmpty();
}

static QAbstractButton *question_button(const QString &text)
{
    for (QWidget *t : QApplication::topLevelWidgets())
        if (auto *box = qobject_cast<QMessageBox *>(t); box && box->isVisible())
            for (QAbstractButton *b : box->buttons())
                if (b->text() == text)
                    return b;
    return nullptr;
}

static media::Probe probe_json(const QString &json) { return media::parse_probe(json.toUtf8()); }

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    setup_app();
    std::signal(SIGXFSZ, SIG_IGN);
    QString dir = home_path("media");
    makedirs(dir);
    MainWindow *w = open_window({dir});

    // ================================================================ what's built (no programs run)

    media::Probe film = probe_json(R"({"format": {"duration": "100.0", "size": "50000000", "bit_rate": "4000000",
        "format_name": "mov,mp4,m4a"},
        "streams": [{"index": 0, "codec_type": "video", "codec_name": "h264", "width": 1920, "height": 1080,
                     "avg_frame_rate": "30000/1001", "bit_rate": "3800000"},
                    {"index": 1, "codec_type": "audio", "codec_name": "aac", "channels": 2, "sample_rate": "48000",
                     "bit_rate": "128000", "tags": {"language": "eng"}},
                    {"index": 2, "codec_type": "audio", "codec_name": "truehd", "channels": 6, "sample_rate": "48000",
                     "tags": {"language": "fra", "title": "Surround"}}]})");
    check(film.ok && film.duration == 100 && film.video() && film.video()->width == 1920 &&
              std::abs(film.video()->fps - 29.97) < 0.01 && film.audio().size() == 2 && film.audio()[1].lang == "fra",
          "ffprobe's answer is read: length, size, the video's size and frame rate, each sound track");
    check(media::describe(film) == "1:40  ·  1920×1080  ·  H.264 / AAC (2 tracks)  ·  50.0 MB",
          "a video's details in plain words (" + media::describe(film) + ")");
    QSet<QString> encs = media::parse_encoders(" V....D libx264              libx264 H.264\n"
                                               " V....D libx265              libx265 H.265 / HEVC\n"
                                               " V....D libvpx-vp9           libvpx VP9\n"
                                               " A....D aac                  AAC (Advanced Audio Coding)\n"
                                               " A....D libopus              libopus Opus\n"
                                               " A....D libmp3lame           libmp3lame MP3\n"
                                               " V....D gif                  GIF\n"
                                               " ------\n");
    check(encs == QSet<QString>({"libx264", "libx265", "libvpx-vp9", "aac", "libopus", "libmp3lame", "gif"}),
          "ffmpeg's list of encoders is read");
    bool ok = false;
    check(media::parse_time("83.5", &ok) == 83.5 && ok && media::parse_time("1:23") == 83 &&
              media::parse_time("1:02:03") == 3723 && (media::parse_time("1:x", &ok), !ok) && media::clock(83.4) == "1:23",
          "times are read as seconds, m:ss or h:mm:ss, and shown as m:ss");

    media::VideoSpec mp4;
    media::Args a = media::video_args(mp4, film, encs);
    check(a.error.isEmpty() && arg_after(a.output, "-c:v") == "libx264" && arg_after(a.output, "-crf") == "24" &&
              arg_after(a.output, "-preset") == "medium" && a.output.contains("+faststart") &&
              arg_after(a.output, "-c:a") == "aac" && a.input.isEmpty(),
          "MP4 (H.264): libx264 at a CRF from the quality, the preset from the speed, the sound converted when a track "
          "doesn't fit MP4 as it is (" + a.output.join(' ') + ")");
    media::VideoSpec mkv = mp4;
    mkv.container = "mkv";
    check(arg_after(media::video_args(mkv, film, encs).output, "-c:a") == "copy", "...and copied as it is when it fits (MKV)");
    media::VideoSpec small = mp4;
    small.vcodec = "hevc";
    check(media::video_args(small, film, encs).output.contains("hvc1"), "H.265 in MP4 is tagged for Apple devices");
    media::VideoSpec cut = mp4;
    cut.start = "1:00", cut.end = "1:30";
    a = media::video_args(cut, film, encs);
    check(a.input == QStringList({"-ss", "60.000"}) && arg_after(a.output, "-t") == "30.000",
          "a trim starts the reading at its start (before -i) and keeps its length");
    cut.end = "0:30";
    check(!media::video_args(cut, film, encs).error.isEmpty(), "an end before the start is refused, saying why");
    media::VideoSpec turn = mp4;
    turn.rotate = 90, turn.height = 720;
    check(arg_after(media::video_args(turn, film, encs).output, "-vf") == "transpose=1,scale=720:-2",
          "turned a quarter, the shorter side of the turned picture is scaled to 720");
    media::VideoSpec sized = mp4;
    sized.target_mb = 10, sized.audio = "encode", sized.abitrate = 128;
    check(arg_after(media::video_args(sized, film, encs).output, "-b:v") == "710k",
          "aiming for a size: the video's bit rate is what's left of it after the sound");
    media::VideoSpec copy = mp4;
    copy.vcodec = "copy", copy.rotate = 90;
    a = media::video_args(copy, film, encs);
    check(arg_after(a.output, "-c:v") == "copy" && !a.output.contains("-vf"), "copied as it is: nothing is filtered");
    media::VideoSpec av1 = mp4;
    av1.vcodec = "av1";
    check(media::video_args(av1, film, encs).error.contains("AV1"), "a codec this ffmpeg has no encoder for is refused");
    media::VideoSpec mp3;
    mp3.container = "mp3", mp3.abitrate = 192;
    a = media::video_args(mp3, film, encs);
    check(a.output.contains("-vn") && arg_after(a.output, "-c:a") == "libmp3lame" && arg_after(a.output, "-b:a") == "192k",
          "sound only (MP3): no video, the first track at the bit rate chosen");
    check(std::abs(media::estimate_mb(mp3, film) - 2.4) < 0.01, "...and its size is estimated from the bit rate");
    media::VideoSpec gif = media::video_presets(".mp4").value(6).spec;
    check(arg_after(media::video_args(gif, film, encs).output, "-vf").contains("palettegen"), "GIF: through a palette");
    QList<media::Preset> presets = media::video_presets(".webm");
    check(presets.last().spec.container == "webm" && presets.last().spec.vcodec == "vp9" &&
              media::video_presets(".avi").last().spec.container == "mkv",
          "Same format, just smaller follows the source's type (MKV for one that isn't offered)");
    media::VideoSpec custom = mp4;
    custom.quality = 33, custom.start = "5";
    check(media::VideoSpec::from_json(custom.to_json()) == custom, "the settings are kept and read back whole");

    check(media::audio_ext("copy", "aac") == ".m4a" && media::audio_ext("copy", "opus") == ".opus" &&
              media::audio_ext("copy", "truehd") == ".mka" && media::audio_ext("alac", "flac") == ".m4a" &&
              media::muxer(".m4a") == "ipod" && media::muxer(".mka") == "matroska",
          "the sound kept as it is goes into its own type of file (AAC → .m4a, anything else → .mka)");
    media::AudioSpec as;
    as.format = "copy";
    a = media::audio_args(as, film, 1);
    check(arg_after(a.output, "-map") == "0:a:1" && arg_after(a.output, "-c:a") == "copy", "a chosen track, copied");
    media::Probe song = probe_json(R"({"format": {"duration": "200"}, "streams": [
        {"index": 0, "codec_type": "audio", "codec_name": "flac", "sample_rate": "44100", "bits_per_raw_sample": "24"},
        {"index": 1, "codec_type": "video", "codec_name": "png", "disposition": {"attached_pic": 1}}]})");
    as.format = "mp3", as.bitrate = 256;
    a = media::audio_args(as, song, 0);
    check(a.output.contains("attached_pic") && a.output.contains("0:1") && arg_after(a.output, "-b:a") == "256k" &&
              a.output.contains("-map_metadata"),
          "to MP3: the cover picture and the tags kept");
    as.format = "opus";
    a = media::audio_args(as, song, 0);
    check(a.output.contains("-vn") && arg_after(a.output, "-ar") == "48000", "to Opus: no cover, and 48 kHz (Opus's rate)");
    as.format = "wav";
    check(arg_after(media::audio_args(as, song, 0).output, "-c:a") == "pcm_s24le", "to WAV: a 24-bit source stays 24-bit");
    check(media::loudnorm_filter(R"({"input_i": "-20.1", "input_tp": "-3.0", "input_lra": "5.0", "input_thresh": "-30.5",
        "target_offset": "0.2"})")
              .contains("measured_I=-20.1:measured_TP=-3.0") &&
              media::loudnorm_filter("").startsWith("loudnorm=I=-16"),
          "the second loudness pass uses the first's measurements");
    double sp = 0;
    check(media::progress_time("out_time_us=1500000") == 1.5 && media::progress_time("speed=2.5x", &sp) < 0 && sp == 2.5 &&
              media::progress_time("frame=10") < 0,
          "ffmpeg's progress is read (the time done, its speed)");
    write_text(join(dir, "clip.converted.mp4"), "taken");
    check(basename(media::output_path(join(dir, "clip.mov"), dir, ".converted", ".mp4")) == "clip.converted (2).mp4",
          "an output never takes a name that's there");
    ::unlink(enc(join(dir, "clip.converted.mp4")).constData());

    // ================================================================ rotating image files

    bool jpegtran = which("jpegtran");
    QString jpg = join(dir, "turn.jpg");
    quarters().save(jpg, "JPEG", 98);
    if (jpegtran) {
        check(imgedit::rotate_method(jpg) == "jpegtran", "a JPEG is turned with jpegtran when it's installed");
        imgedit::rotate_file(jpg, 90, false);
        check(corners(shown(jpg)) == "BRWG" && shown(jpg).size() == QSize(48, 64) && metadata::orientation_tag(jpg).offset < 0,
              "Rotate 90° Clockwise turns a JPEG's pixels (" + corners(shown(jpg)) + ")");
        // one turned by its orientation tag: the pixels are turned to show the same, the tag set to 1
        QString tagged = join(dir, "tagged.jpg");
        quarters().save(tagged, "JPEG", 98);
        if (which("exiftool")) {
            proc::run({"exiftool", "-q", "-overwrite_original", "-Orientation#=6", tagged}, 60000);
            QString before = corners(shown(tagged));
            imgedit::rotate_file(tagged, 90, false);
            QImageReader raw(tagged);
            raw.setAutoTransform(false);
            check(metadata::orientation_tag(tagged).value == 1 && corners(raw.read()) == corners(shown(tagged)) &&
                      shown(tagged).size() == QSize(64, 48) && before != corners(shown(tagged)),
                  "a JPEG shown turned by its tag is turned on from how it's shown, its tag set back to 1");
        } else {
            skip("a JPEG turned by its orientation tag (needs exiftool)");
        }
        QString odd = join(dir, "odd.jpg");
        quarters(50, 30).save(odd, "JPEG", 98);
        bool refused = false;
        try {
            imgedit::rotate_file(odd, 90, false);
        } catch (const imgedit::NotWholeBlocks &) {
            refused = true;
        }
        check(refused && shown(odd).size() == QSize(50, 30), "a JPEG that isn't whole blocks isn't turned unasked");
        convert::rotate(w, {odd}, 90, false);
        QAbstractButton *trim = nullptr;
        wait_for([&]() { return (trim = question_button("Trim the Edge")); });
        if (trim)
            trim->click();
        auto upright = [&]() {   // turned: higher than wide now, with a few pixels at most cut off
            QSize s = shown(odd).size();
            return s.width() < s.height() && s.width() <= 30 && s.height() <= 50 && s.height() >= 32;
        };
        check(trim && wait_for(upright),
              "Rotate asks about it, and Trim the Edge turns it, cutting the odd pixels off");
        // failing part-way: the file stays as it was, no part file
        QByteArray was = read_file(jpg);
        struct rlimit old_limit;
        getrlimit(RLIMIT_FSIZE, &old_limit);
        struct rlimit tiny = {200, old_limit.rlim_max};
        setrlimit(RLIMIT_FSIZE, &tiny);
        bool failed = false;
        try {
            imgedit::rotate_file(jpg, 180, false);
        } catch (const Error &) {
            failed = true;
        }
        setrlimit(RLIMIT_FSIZE, &old_limit);
        check(failed && read_file(jpg) == was && parts_in(dir).isEmpty(),
              "a turn that fails part-way (a file-size limit) leaves the JPEG as it was, and no part file");
    } else {
        skip("turning JPEGs (needs jpegtran)");
    }
    QString png = join(dir, "turn.png");
    QImage pimg = quarters();
    pimg.setText("parameters", "a prompt");
    pimg.save(png);
    convert::rotate(w, {png}, 0, true);   // Flip Horizontally
    check(wait_for([&]() { return corners(shown(png)) == "GRWB"; }) && QImage(png).text("parameters") == "a prompt",
          "a PNG is flipped by writing its pixels again (losslessly), its text kept");

    // ================================================================ Convert Image…

    QString clear = join(dir, "clear.png");
    QImage alpha(80, 40, QImage::Format_ARGB32);
    alpha.fill(Qt::transparent);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x)
            alpha.setPixel(x, y, qRgba(255, 0, 0, 255));
    alpha.save(clear);
    convert::image_dialog(w, {clear});
    auto *idlg = shown_window<convert::ImageDialog>();
    if (idlg) {
        idlg->format->setCurrentIndex(idlg->format->findData("jpg"));
        idlg->resize->setCurrentIndex(idlg->resize->findData("long"));
        idlg->size->setValue(40);
        check(is_free_window(idlg) && idlg->background->isEnabled(), "Convert Image… is a free window; for JPEG it asks what goes under transparency");
        idlg->start();
    }
    QString out = join(dir, "clear.jpg");
    check(wait_for([&]() { return lexists(out) && jobs_done(); }) && QImage(out).size() == QSize(40, 20) &&
              QImageReader(out).format() == "jpeg" && QColor(QImage(out).pixel(30, 10)).lightness() > 240 && lexists(clear),
          "a PNG converted to JPEG: resized to its longest side, white under what was transparent, the original kept");
    convert::image_dialog(w, {clear});
    if ((idlg = shown_window<convert::ImageDialog>())) {
        idlg->format->setCurrentIndex(idlg->format->findData("png"));
        idlg->resize->setCurrentIndex(0);
        idlg->start();
    }
    check(wait_for([&]() { return lexists(join(dir, "clear (2).png")) && jobs_done(); }) && QImage(clear).size() == QSize(80, 40),
          "converting to its own format makes a new file beside it, never replacing it");
    if (which("exiftool")) {
        QString cam = join(dir, "camera.jpg");
        quarters().save(cam, "JPEG", 95);
        proc::run({"exiftool", "-q", "-overwrite_original", "-Make=TestCam", "-Orientation#=6", cam}, 60000);
        convert::image_dialog(w, {cam});
        if ((idlg = shown_window<convert::ImageDialog>())) {
            idlg->format->setCurrentIndex(idlg->format->findData("png"));
            idlg->keep_meta->setChecked(true);
            idlg->start();
        }
        QString cam_png = join(dir, "camera.png");
        wait_for([&]() { return lexists(cam_png) && jobs_done(); });
        QString make = QString::fromUtf8(proc::run({"exiftool", "-s3", "-Make", cam_png}, 60000).out).trimmed();
        check(make == "TestCam" && QImage(cam_png).size() == QSize(48, 64),
              "converted with its metadata kept: the camera's details carried into the PNG, the photo turned upright (" + make + ")");
    } else {
        skip("metadata kept by Convert Image (needs exiftool)");
    }

    // ================================================================ ffmpeg

    if (!media::have_ffmpeg() || !media::encoders().contains("libx264") || !media::encoders().contains("libmp3lame")) {
        skip("Convert Video, Extract Audio and Convert Audio (need ffmpeg with libx264 and libmp3lame)");
        finish();
    }
    QString clip = join(dir, "clip.mp4");
    proc::run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=duration=3:size=320x240:rate=25", "-f", "lavfi",
               "-i", "sine=frequency=440:duration=3", "-c:v", "libx264", "-c:a", "aac", "-shortest", clip},
              60000);
    media::Probe cp = media::probe(clip);
    check(cp.ok && cp.video() && cp.audio().size() == 1 && std::abs(cp.duration - 3) < 0.2, "ffprobe reads the test video");

    convert::video_dialog(w, {clip});
    auto *vd = shown_window<convert::VideoDialog>();
    check(vd && is_free_window(vd) && wait_for([&]() { return vd->first.ok; }) && vd->details->text().contains("320×240"),
          "Convert Video… is a free window showing the video's details");
    if (vd) {
        vd->preset->setCurrentIndex(vd->preset->findData("mp4"));
        vd->resolution->setCurrentIndex(vd->resolution->findData(0));
        vd->quality->setValue(vd->quality->value() == 30 ? 31 : 30);
        check(vd->preset->currentData() == "custom", "changing an option makes the preset Custom");
        check(vd->preview->toPlainText().startsWith("ffmpeg -hide_banner -i " + clip) &&
                  vd->preview->toPlainText().endsWith("clip.converted.mp4"),
              "the command is shown as it will run (" + vd->preview->toPlainText() + ")");
        vd->start();
    }
    QString conv = join(dir, "clip.converted.mp4");
    check(wait_for([&]() { return lexists(conv) && jobs_done(); }, 60000) && media::probe(conv).video() &&
              media::probe(conv).video()->codec == "h264" && parts_in(dir).isEmpty(),
          "the video is converted beside it, under its new name, with no part file left");
    check(QJsonDocument::fromJson(settings().value("convert/video").toString().toUtf8()).object().value("preset") == "custom",
          "the settings used are remembered for next time");

    // stopped part-way: no half-made video
    QString longer = join(dir, "long.mkv");
    proc::run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=duration=60:size=640x480:rate=30", "-c:v",
               "libx264", "-preset", "ultrafast", longer},
              120000);
    convert::video_dialog(w, {longer});
    if ((vd = shown_window<convert::VideoDialog>())) {
        wait_for([&]() { return vd->first.ok; });
        vd->preset->setCurrentIndex(vd->preset->findData("mp4"));
        vd->speed->setValue(4);
        vd->start();
    }
    Task *t = nullptr;
    bool moving = wait_for([&]() { return (t = job_titled("Converting “long.mkv”")) && t->fraction > 0.02; }, 30000);
    if (t)
        t->cancel();
    check(moving && wait_for(jobs_done, 20000) && !lexists(join(dir, "long.converted.mp4")) && parts_in(dir).isEmpty(),
          "a conversion stopped part-way, with its progress shown, leaves no half-made video and no part file");

    // a full disk part-way: the same
    struct rlimit old_limit;
    getrlimit(RLIMIT_FSIZE, &old_limit);
    struct rlimit tiny = {64 * 1024, old_limit.rlim_max};
    convert::video_dialog(w, {longer});
    if ((vd = shown_window<convert::VideoDialog>())) {
        wait_for([&]() { return vd->first.ok; });
        vd->preset->setCurrentIndex(vd->preset->findData("mp4"));
        setrlimit(RLIMIT_FSIZE, &tiny);
        vd->start();
    }
    bool ended = wait_for(jobs_done, 60000);
    setrlimit(RLIMIT_FSIZE, &old_limit);
    QMessageBox *msg = shown_window<QMessageBox>();
    check(ended && !lexists(join(dir, "long.converted.mp4")) && parts_in(dir).isEmpty() && msg &&
              msg->text().contains("0 of 1"),
          "a conversion that fails part-way (a file-size limit) leaves nothing behind, and says so");
    if (msg)
        msg->close();

    // Extract Audio…: as it is, then converted
    convert::extract_audio_dialog(w, {clip});
    auto *ad = shown_window<convert::AudioDialog>();
    check(ad && wait_for([&]() { return ad->first.ok; }) && ad->keep->isChecked() && ad->keep->text().contains(".m4a"),
          "Extract Audio… keeps the sound as it is by default (AAC → .m4a)");
    if (ad)
        ad->start();
    QString m4a = join(dir, "clip.m4a");
    check(wait_for([&]() { return lexists(m4a) && jobs_done(); }, 30000) && media::probe(m4a).audio().value(0).codec == "aac" &&
              !media::probe(m4a).video(),
          "the sound is copied out unchanged, without the video");
    convert::extract_audio_dialog(w, {clip});
    if ((ad = shown_window<convert::AudioDialog>())) {
        wait_for([&]() { return ad->first.ok; });
        ad->convert->setChecked(true);
        ad->format->setCurrentIndex(ad->format->findData("mp3"));
        ad->output->trash->setChecked(true);
        ad->start();
    }
    QString mp3_out = join(dir, "clip.mp3");
    check(wait_for([&]() { return lexists(mp3_out) && jobs_done(); }, 30000) && media::probe(mp3_out).audio().value(0).codec == "mp3" &&
              wait_for([&]() { return !lexists(clip); }),
          "...or converted (MP3), and the original moved to the trash when asked");

    // Convert Audio…: a folder, its subfolders kept, tags and cover carried over, the volume evened out
    QString music = join(dir, "music"), sub = join(music, "album");
    makedirs(sub, true);
    QString cover = join(dir, "cover.png");
    quarters().save(cover);
    QString flac = join(sub, "song.flac");
    proc::run({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=330:duration=2", "-i", cover, "-map", "0",
               "-map", "1", "-c:a", "flac", "-c:v", "png", "-disposition:v", "attached_pic", "-metadata", "title=Test Song",
               flac},
              60000);
    convert::audio_dialog(w, {music});
    ad = shown_window<convert::AudioDialog>();
    check(ad && ad->paths == QStringList{flac} && ad->output->into->isChecked() &&
              ad->output->path->text() == join(dir, "music (converted)"),
          "Convert Audio… on a folder finds its audio files and offers a folder beside it for them");
    if (ad) {
        wait_for([&]() { return ad->first.ok; });
        ad->format->setCurrentIndex(ad->format->findData("mp3"));
        ad->loudnorm->setChecked(true);
        ad->start();
    }
    QString song_mp3 = join(dir, "music (converted)/album/song.mp3");
    media::Probe sp3;
    bool converted = wait_for([&]() { return lexists(song_mp3) && jobs_done(); }, 60000);
    if (converted)
        sp3 = media::probe(song_mp3);
    QString title = QJsonDocument::fromJson(proc::run({"ffprobe", "-v", "quiet", "-print_format", "json", "-show_format", song_mp3}, 30000).out)
                        .object().value("format").toObject().value("tags").toObject().value("title").toString();
    check(converted && sp3.audio().value(0).codec == "mp3" && title == "Test Song" &&
              std::any_of(sp3.streams.begin(), sp3.streams.end(), [](const media::Stream &s) { return s.cover; }),
          "the album's song is converted into the same subfolder there, with its title and cover (title: " + title + ")");
    finish();
}
