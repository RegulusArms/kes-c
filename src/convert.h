// The right-click menu's image and media tools (media.h does the work):
//   Rotate ▸        images turned in their files, without losing quality where the format allows (imgedit)
//   Convert Image…  to JPEG, PNG, WebP, AVIF, HEIC or TIFF
//   Convert Video…  with ffmpeg: presets, then every option in plain terms, the command shown
//   Extract Audio…  a video's sound, as it is or converted
//   Convert Audio…  audio files, or every audio file in folders (their subfolders kept)
// Each is one queued job with progress and ✕, the files done one after another, and one summary at the end. Outputs
// go beside the originals (or into a chosen folder), never over a file; the originals can go to the trash after.
#pragma once

#include <QDialog>
#include <QStringList>

#include "media.h"

class MainWindow;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;

namespace convert {

bool is_audio(const QString &path);
QStringList audio_files(const QStringList &paths);   // the audio files among paths and in the folders among them

// how, reencode: as imgedit::rotate_file. A JPEG jpegtran won't turn exactly, or a lossy file, is asked about once
// for all of them, and turned again as answered.
void rotate(MainWindow *win, const QStringList &paths, int rotation, bool flip, const QString &how = QString(),
            bool reencode = false);

void image_dialog(MainWindow *win, const QStringList &paths);
void video_dialog(MainWindow *win, const QStringList &paths);
void extract_audio_dialog(MainWindow *win, const QStringList &paths);
void audio_dialog(MainWindow *win, const QStringList &paths);

// Where the files go: beside each original, or into one folder
class OutputRow : public QWidget {
    Q_OBJECT
public:
    explicit OutputRow(const QString &beside_text, QWidget *parent = nullptr);
    QString folder() const;   // empty: beside each original
    QRadioButton *beside, *into;
    QLineEdit *path;
    QPushButton *choose;
    QCheckBox *trash;         // move the originals to the trash afterwards
Q_SIGNALS:
    void changed();
};

class ImageDialog : public QDialog {
    Q_OBJECT
public:
    ImageDialog(MainWindow *win, const QStringList &paths);
    media::ImageSpec spec() const;
    void start();
    QStringList paths;
    QComboBox *format, *resize, *background;
    QSlider *quality;
    QSpinBox *size;
    QCheckBox *keep_meta;
    OutputRow *output;
    QLabel *note;

private:
    void update_state();
    MainWindow *win;
};

class VideoDialog : public QDialog {
    Q_OBJECT
public:
    VideoDialog(MainWindow *win, const QStringList &paths);
    media::VideoSpec spec() const;
    void set_spec(const media::VideoSpec &s);
    void choose_preset(int i);
    void start();
    QStringList paths;
    media::Probe first;    // the first video's details (read on a worker)
    QSet<QString> encs;
    QList<media::Preset> presets;
    QComboBox *preset, *container, *vcodec, *resolution, *fps, *audio, *abitrate, *rotate;
    QSlider *quality, *speed;
    QCheckBox *target, *flip, *hw;
    QDoubleSpinBox *target_mb;
    QLineEdit *start_time, *end_time, *extra, *name;
    QLabel *details, *estimate, *problem;
    OutputRow *output;
    QPlainTextEdit *preview;
    QPushButton *go;

private:
    void changed();
    void update_preview();
    QString out_path(const QString &src) const;
    MainWindow *win;
    bool filling = false;
};

class AudioDialog : public QDialog {
    Q_OBJECT
public:
    // from_video: Extract Audio… (keep as it is, or convert; which track); else Convert Audio…
    AudioDialog(MainWindow *win, const QStringList &paths, bool from_video);
    media::AudioSpec spec() const;
    int track() const;   // -1: every track
    void start();
    QStringList paths;   // Convert Audio…: the audio files (folders walked)
    QStringList roots;   // ...and the folders they were found in, to keep their subfolders
    bool from_video;
    media::Probe first;
    QComboBox *tracks, *format, *bitrate, *rate, *channels;
    QRadioButton *keep, *convert;
    QCheckBox *loudnorm;
    QLabel *details;
    OutputRow *output;
    QPlainTextEdit *preview;
    QPushButton *go;

private:
    void update_preview();
    MainWindow *win;
};

}  // namespace convert
