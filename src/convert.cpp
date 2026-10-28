#include "convert.h"

#include "app.h"
#include "fileops.h"
#include "imgedit.h"
#include "pane.h"
#include "thumbs.h"
#include "util.h"
#include "windows.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>

using namespace util;

namespace convert {

static const QSet<QString> AUDIO_EXTS = {".mp3", ".flac", ".wav", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".wma",
                                         ".aiff", ".aif", ".ape", ".wv", ".alac", ".mka", ".ac3", ".dts"};

bool is_audio(const QString &path) { return AUDIO_EXTS.contains(ext_of(path)); }

QStringList audio_files(const QStringList &paths)
{
    QStringList out;
    for (const QString &p : paths) {
        if (isdir(p)) {
            walk(p, [&](const QString &root, QStringList &dirs, QStringList &files) {
                dirs.erase(std::remove_if(dirs.begin(), dirs.end(), [](const QString &d) { return d.startsWith('.'); }),
                           dirs.end());
                files.sort();
                for (const QString &f : files)
                    if (is_audio(f) && !f.startsWith('.'))
                        out << join(root, f);
                return true;
            });
        } else if (is_audio(p)) {
            out << p;
        }
    }
    return out;
}

// the settings a window was last used with (kept state, as JSON)
static QJsonObject remembered(const QString &key)
{
    return QJsonDocument::fromJson(settings().value(key).toString().toUtf8()).object();
}

static void remember(const QString &key, const QJsonObject &o)
{
    settings().setValue(key, QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

// ---------------------------------------------------------------- one job for a batch

// one(task, i, base, share) converts sources[i] and returns what it made; an Error fails that file only, ✕ stops the
// batch (the files made so far stay). Then one summary; the originals of the files done go to the trash if asked.
using One = std::function<QStringList(Task *, int, double, double)>;

static void run_batch(MainWindow *win, const QString &title, const QString &what, const QStringList &sources,
                      const QStringList &lanes, const QStringList &intro, One one, bool trash_after)
{
    QPointer<MainWindow> self(win);
    auto work = [sources, one](Task *task) -> QVariant {
        QStringList made, done;
        QVariantList failed;
        int n = int(sources.size());
        for (int i = 0; i < n; ++i) {
            if (task->cancelled)
                break;
            task->step = i;
            task->report(double(i) / n, 1, QString("%1 of %2 — %3").arg(QString::number(i + 1), QString::number(n), basename(sources[i])));
            try {
                QStringList m = one(task, i, double(i) / n, 1.0 / n);
                for (const QString &x : m)
                    task->log("Made: " + x);
                made << m;
                done << sources[i];
            } catch (const Cancelled &) {
                break;
            } catch (const Error &e) {
                failed << QVariant(QStringList{sources[i], e.message()});
                task->log("Failed: " + sources[i] + ": " + e.message());
            }
        }
        task->was_cancelled = task->cancelled;
        return QVariantMap{{"made", made}, {"done", done}, {"failed", failed}, {"stopped", bool(task->cancelled)}};
    };
    auto finished = [self, title, what, trash_after, n = sources.size()](const QVariant &res) {
        QVariantMap m = res.toMap();
        QStringList made = m.value("made").toStringList(), done = m.value("done").toStringList();
        QVariantList failed = m.value("failed").toList();
        if (!self)
            return;
        if (Pane *p = self->pane())
            for (const QString &x : made)
                if (dirname(x) == p->dir())
                    p->select_later(x);
        if (trash_after && !done.isEmpty())
            self->trash_paths(done);
        QString head = QString("%1 of %2 %3 done%4.")
                           .arg(QString::number(done.size()), QString::number(n), what,
                                m.value("stopped").toBool() ? QString(" (stopped)") : QString());
        if (!failed.isEmpty()) {
            QStringList lines;
            for (const QVariant &f : failed.mid(0, 8))
                lines << basename(f.toStringList().value(0)) + ": " + f.toStringList().value(1);
            warn(self, title, head + "\n\n" + lines.join("\n\n"));
        } else if (res.isValid()) {
            self->statusBar()->showMessage(head, 6000);
        }
    };
    QStringList steps;
    for (const QString &s : sources)
        steps << basename(s);
    fileops::queue_job(win, title, lanes, work, finished, true, sources.size() > 1 ? steps : QStringList(), false, intro,
                       true);
}

static QStringList lanes_for(const QStringList &sources, const QString &folder)
{
    QStringList l = sources;
    if (!folder.isEmpty())
        l << folder;
    return l;
}

static QString some(int n, const QString &one, const QString &many) { return n == 1 ? one : QString("%1 %2").arg(n).arg(many); }

// ---------------------------------------------------------------- rotate

void rotate(MainWindow *win, const QStringList &paths, int rotation, bool flip, const QString &how, bool reencode)
{
    QStringList imgs;
    for (const QString &p : paths)
        if (is_image(p))
            imgs << p;
    if (imgs.isEmpty())
        return;
    QPointer<MainWindow> self(win);
    auto work = [imgs, rotation, flip, how, reencode](Task *task) -> QVariant {
        QStringList done, not_whole, lossy;
        QVariantList failed;
        for (int i = 0; i < imgs.size(); ++i) {
            if (task->cancelled)
                break;   // the images turned so far stay turned
            task->step = i;
            task->report(i, imgs.size(), basename(imgs[i]));
            try {
                imgedit::rotate_file(imgs[i], rotation, flip, how, reencode);
                done << imgs[i];
                task->log("Turned: " + imgs[i]);
            } catch (const imgedit::NotWholeBlocks &) {
                not_whole << imgs[i];
            } catch (const Error &e) {
                if (!reencode && imgedit::rotate_method(imgs[i]) == "reencode")
                    lossy << imgs[i];
                else
                    failed << QVariant(QStringList{imgs[i], e.message()});
                task->log("Not turned: " + imgs[i] + ": " + e.message());
            }
        }
        task->was_cancelled = task->cancelled;
        return QVariantMap{{"done", done}, {"not_whole", not_whole}, {"lossy", lossy}, {"failed", failed}};
    };
    auto finished = [self, rotation, flip](const QVariant &res) {
        QVariantMap m = res.toMap();
        if (!self)
            return;
        for (const QString &p : m.value("done").toStringList())
            self->thumbs->invalidate(p);
        QStringList not_whole = m.value("not_whole").toStringList(), lossy = m.value("lossy").toStringList();
        if (!not_whole.isEmpty()) {
            auto *box = new QMessageBox(QMessageBox::Question, "Rotate",
                                        some(int(not_whole.size()), "“" + basename(not_whole.first()) + "” isn't",
                                             "JPEGs aren't") +
                                            " a whole number of JPEG blocks wide or high (8 or 16 pixels), so it can't "
                                            "be turned exactly without losing quality.\n\nTrim the Edge cuts off the "
                                            "odd pixels at its right and bottom (at most 15). Set the Orientation Tag "
                                            "only marks how to show it: the pixels stay, and most apps turn it.",
                                        QMessageBox::Cancel, self);
            QAbstractButton *trim = box->addButton("Trim the Edge", QMessageBox::AcceptRole);
            QAbstractButton *tag = box->addButton("Set the Orientation Tag", QMessageBox::AcceptRole);
            ask_box(box, [self, not_whole, rotation, flip, trim, tag](QAbstractButton *b) {
                if (self && (b == trim || b == tag))
                    rotate(self, not_whole, rotation, flip, b == trim ? "trim" : "tag");
            });
        }
        if (!lossy.isEmpty()) {
            auto *box = new QMessageBox(QMessageBox::Question, "Rotate",
                                        "Turning " + some(int(lossy.size()), "“" + basename(lossy.first()) + "”", "of these images") +
                                            " means saving it again in its own format (HEIC, AVIF or lossy WebP), "
                                            "which loses a little quality.",
                                        QMessageBox::Cancel, self);
            QAbstractButton *again = box->addButton("Save Again", QMessageBox::AcceptRole);
            ask_box(box, [self, lossy, rotation, flip, again](QAbstractButton *b) {
                if (self && b == again)
                    rotate(self, lossy, rotation, flip, QString(), true);
            });
        }
        QVariantList failed = m.value("failed").toList();
        if (!failed.isEmpty()) {
            QStringList lines;
            for (const QVariant &f : failed.mid(0, 8))
                lines << f.toStringList().value(1);
            warn(self, "Rotate", QString("%1 couldn't be turned:\n\n%2").arg(some(int(failed.size()), "An image", "images"), lines.join('\n')));
        }
    };
    QString verb = flip ? "Flipping" : "Rotating";
    QString title = imgs.size() == 1 ? verb + " “" + basename(imgs.first()) + "”" : QString("%1 %2 images").arg(verb).arg(imgs.size());
    QStringList intro{QString("%1: %2").arg(flip ? "Flip" : QString("Rotate %1°").arg(rotation), some(int(imgs.size()), "1 image", "images")),
                      "JPEGs are turned without re-compressing them (jpegtran); each file is written whole under a "
                      "hidden name and then renamed over the original."};
    QStringList steps;
    for (const QString &p : imgs)
        steps << basename(p);
    fileops::queue_job(win, title, imgs, work, finished, true, imgs.size() > 1 ? steps : QStringList(), false, intro, true);
}

// ---------------------------------------------------------------- where the files go

OutputRow::OutputRow(const QString &beside_text, QWidget *parent) : QWidget(parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    beside = new QRadioButton(beside_text);
    into = new QRadioButton("Into this folder:");
    beside->setChecked(true);
    auto *h = new QHBoxLayout;
    path = new QLineEdit;
    path->setPlaceholderText("Choose a folder…");
    choose = new QPushButton("Choose…");
    h->addWidget(into);
    h->addWidget(path, 1);
    h->addWidget(choose);
    trash = new QCheckBox("Move the originals to the trash afterwards");
    v->addWidget(beside);
    v->addLayout(h);
    v->addWidget(trash);
    connect(choose, &QPushButton::clicked, this, [this]() {
        QPointer<OutputRow> me(this);
        choose_folder(this, "Where the Files Go", path->text().isEmpty() ? HOME() : path->text(), [me](const QString &d) {
            if (!me)
                return;
            me->path->setText(d);
            me->into->setChecked(true);
        });
    });
    connect(path, &QLineEdit::textEdited, this, [this]() { into->setChecked(true); });
    for (QRadioButton *b : {beside, into})
        connect(b, &QRadioButton::toggled, this, &OutputRow::changed);
    connect(path, &QLineEdit::textChanged, this, &OutputRow::changed);
}

QString OutputRow::folder() const { return into->isChecked() ? path->text().trimmed() : QString(); }

// a dialog's Convert / Cancel row
static QPushButton *buttons(QDialog *d, QVBoxLayout *lay, const QString &label)
{
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *go = bb->addButton(label, QDialogButtonBox::AcceptRole);
    go->setDefault(true);
    QObject::connect(bb, &QDialogButtonBox::rejected, d, &QDialog::close);
    lay->addWidget(bb);
    return go;
}

static QPlainTextEdit *command_box()
{
    auto *p = new QPlainTextEdit;
    p->setReadOnly(true);
    p->setMaximumHeight(72);
    p->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    p->setAccessibleName("The command");
    return p;
}

static QString shell_join(const QStringList &argv)
{
    QStringList out;
    static const QRegularExpression plain("^[A-Za-z0-9_./:=+,@%-]+$");
    for (QString a : argv)
        out << (plain.match(a).hasMatch() ? a : "'" + a.replace("'", "'\\''") + "'");
    return out.join(' ');
}

// the output folder chosen, made if it isn't there; empty when it can't be (said in a message)
static bool output_ok(QWidget *d, OutputRow *o)
{
    if (!o->into->isChecked())
        return true;
    QString f = o->folder();
    if (f.isEmpty()) {
        warn(d, "Convert", "Choose the folder the files go into.");
        return false;
    }
    try {
        makedirs(f, true);
    } catch (const OSError &e) {
        warn(d, "Convert", QString("Couldn't make the folder “%1”: %2").arg(f, e.message()));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- images

ImageDialog::ImageDialog(MainWindow *win, const QStringList &paths_) : QDialog(win), paths(paths_), win(win)
{
    setWindowTitle(paths.size() == 1 ? "Convert Image — " + basename(paths.first())
                                     : QString("Convert %1 Images").arg(paths.size()));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    lay->addLayout(form);
    format = new QComboBox;
    for (const media::ImageTarget &t : media::image_targets())
        format->addItem(t.name + (t.lossy ? "" : "  (lossless)"), t.id);
    form->addRow("Convert to:", format);
    auto *qrow = new QHBoxLayout;
    quality = new QSlider(Qt::Horizontal);
    quality->setRange(30, 100);
    auto *qval = new QLabel;
    qrow->addWidget(new QLabel("Smaller file"));
    qrow->addWidget(quality, 1);
    qrow->addWidget(new QLabel("Better quality"));
    qrow->addWidget(qval);
    connect(quality, &QSlider::valueChanged, qval, [qval](int v) { qval->setText(QString::number(v)); });
    form->addRow("Quality:", qrow);
    auto *rrow = new QHBoxLayout;
    resize = new QComboBox;
    resize->addItem("Keep the size", "keep");
    resize->addItem("Longest side, in pixels:", "long");
    resize->addItem("Percentage of the size:", "percent");
    size = new QSpinBox;
    size->setRange(1, 20000);
    rrow->addWidget(resize);
    rrow->addWidget(size);
    rrow->addStretch(1);
    form->addRow("Size:", rrow);
    keep_meta = new QCheckBox("Keep the metadata (camera, date taken, location)");
    form->addRow("", keep_meta);
    background = new QComboBox;
    background->addItem("White", "#ffffff");
    background->addItem("Black", "#000000");
    background->addItem("Grey", "#808080");
    form->addRow("Transparent parts:", background);
    output = new OutputRow("Beside the originals (never replacing a file)");
    form->addRow("Save:", output);
    note = new QLabel;
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    lay->addWidget(note);
    QPushButton *go = buttons(this, lay, "Convert");
    connect(go, &QPushButton::clicked, this, &ImageDialog::start);

    QJsonObject r = remembered("convert/image");
    int fi = format->findData(r.value("format").toString("jpg"));
    format->setCurrentIndex(fi >= 0 ? fi : 0);
    quality->setValue(r.value("quality").toInt(90));
    resize->setCurrentIndex(std::max(0, resize->findData(r.value("resize").toString("keep"))));
    size->setValue(r.value("size").toInt(resize->currentData() == "percent" ? 50 : 2048));
    keep_meta->setChecked(r.value("keep_meta").toBool(true));
    background->setCurrentIndex(std::max(0, background->findData(r.value("background").toString("#ffffff"))));
    for (QComboBox *c : {format, resize})
        connect(c, &QComboBox::currentIndexChanged, this, &ImageDialog::update_state);
    connect(keep_meta, &QCheckBox::toggled, this, &ImageDialog::update_state);
    update_state();
}

media::ImageSpec ImageDialog::spec() const
{
    media::ImageSpec s;
    s.format = format->currentData().toString();
    s.quality = quality->value();
    s.resize = resize->currentData().toString();
    s.size = size->value();
    s.keep_meta = keep_meta->isChecked();
    s.background = background->currentData().toString();
    return s;
}

void ImageDialog::update_state()
{
    media::ImageSpec s = spec();
    QList<media::ImageTarget> targets = media::image_targets();
    bool lossy = std::any_of(targets.begin(), targets.end(), [&](const auto &t) { return t.id == s.format && t.lossy; });
    quality->setEnabled(lossy);
    size->setVisible(s.resize != "keep");
    size->setSuffix(s.resize == "percent" ? " %" : " px");
    background->setEnabled(s.format == "jpg");
    QStringList notes;
    if (format->count() == 0)
        notes << "Nothing here can write images (Qt's image plugins are missing).";
    if (s.keep_meta && !which("exiftool") && s.format != "jpg")
        notes << "Without exiftool (libimage-exiftool-perl) the metadata is only kept from JPEG to JPEG.";
    notes << "Photos are turned upright as their orientation says, and keep their colour profile where the format has one.";
    note->setText(notes.join(' ').trimmed());
}

void ImageDialog::start()
{
    if (format->count() == 0 || !output_ok(this, output))
        return;
    media::ImageSpec s = spec();
    remember("convert/image", {{"format", s.format}, {"quality", s.quality}, {"resize", s.resize}, {"size", s.size},
                               {"keep_meta", s.keep_meta}, {"background", s.background}});
    QString folder = output->folder(), ext;
    for (const media::ImageTarget &t : media::image_targets())
        if (t.id == s.format)
            ext = t.ext;
    QStringList srcs = paths;
    QString name = format->currentText().section("  (", 0, 0);
    auto one = [srcs, s, folder, ext](Task *, int i, double, double) -> QStringList {
        QString dst = media::output_path(srcs[i], folder.isEmpty() ? dirname(srcs[i]) : folder, "", ext);
        media::convert_image(srcs[i], dst, s);
        return {dst};
    };
    QString title = srcs.size() == 1 ? "Converting “" + basename(srcs.first()) + "” to " + name
                                     : QString("Converting %1 images to %2").arg(QString::number(srcs.size()), name);
    run_batch(win, title, "images", srcs, lanes_for(srcs, folder),
              {QString("Convert Image: %1 to %2").arg(some(int(srcs.size()), "1 image", "images"), name),
               "Each new image is written whole under a hidden name and then given its name; no file is replaced."},
              one, output->trash->isChecked());
    close();
}

// ---------------------------------------------------------------- videos

static const QList<QPair<QString, QString>> CONTAINERS = {
    {"mp4", "MP4"}, {"mkv", "MKV"}, {"webm", "WebM"}, {"mov", "MOV"}, {"gif", "GIF (animation)"},
    {"mp3", "MP3 (sound only)"}, {"m4a", "M4A (sound only)"}, {"opus", "Opus (sound only)"}};

VideoDialog::VideoDialog(MainWindow *win, const QStringList &paths_) : QDialog(win), paths(paths_), win(win)
{
    setWindowTitle(paths.size() == 1 ? "Convert Video — " + basename(paths.first())
                                     : QString("Convert %1 Videos").arg(paths.size()));
    setAttribute(Qt::WA_DeleteOnClose);
    presets = media::video_presets(ext_of(paths.value(0)));
    auto *lay = new QVBoxLayout(this);
    details = new QLabel("Reading the video's details…");
    details->setWordWrap(true);
    lay->addWidget(details);
    auto *form = new QFormLayout;
    lay->addLayout(form);
    preset = new QComboBox;
    for (const media::Preset &p : presets)
        preset->addItem(p.name, p.id);
    preset->addItem("Custom", "custom");
    form->addRow("Preset:", preset);
    container = new QComboBox;
    for (const auto &[id, n] : CONTAINERS)
        container->addItem(n, id);
    form->addRow("Format:", container);
    vcodec = new QComboBox;
    form->addRow("Video:", vcodec);
    auto *qrow = new QHBoxLayout;
    quality = new QSlider(Qt::Horizontal);
    quality->setRange(0, 100);
    qrow->addWidget(new QLabel("Smaller file"));
    qrow->addWidget(quality, 1);
    qrow->addWidget(new QLabel("Better quality"));
    form->addRow("Quality:", qrow);
    auto *trow = new QHBoxLayout;
    target = new QCheckBox("Aim for a size instead:");
    target_mb = new QDoubleSpinBox;
    target_mb->setRange(1, 100000);
    target_mb->setSuffix(" MB");
    target_mb->setValue(50);
    trow->addWidget(target);
    trow->addWidget(target_mb);
    trow->addStretch(1);
    form->addRow("", trow);
    resolution = new QComboBox;
    for (const auto &[n, h] : QList<QPair<QString, int>>{{"Keep", 0}, {"4K (2160p)", 2160}, {"1080p", 1080}, {"720p", 720},
                                                          {"480p", 480}, {"360p", 360}})
        resolution->addItem(n, h);
    form->addRow("Resolution:", resolution);
    fps = new QComboBox;
    for (int f : {0, 60, 30, 25, 24, 15, 12})
        fps->addItem(f ? QString("%1 a second").arg(f) : QString("Keep"), f);
    form->addRow("Frame rate:", fps);
    auto *arow = new QHBoxLayout;
    audio = new QComboBox;
    audio->addItem("Keep it (copied as it is when the format takes it)", "keep");
    audio->addItem("Convert it", "encode");
    audio->addItem("Remove it", "remove");
    abitrate = new QComboBox;
    for (int b : {320, 256, 192, 160, 128, 96, 64})
        abitrate->addItem(QString("%1 kbps").arg(b), b);
    arow->addWidget(audio, 1);
    arow->addWidget(abitrate);
    form->addRow("Sound:", arow);
    auto *srow = new QHBoxLayout;
    speed = new QSlider(Qt::Horizontal);
    speed->setRange(0, 4);
    speed->setPageStep(1);
    srow->addWidget(new QLabel("Faster"));
    srow->addWidget(speed, 1);
    srow->addWidget(new QLabel("Smaller file"));
    form->addRow("Speed:", srow);
    auto *cut = new QHBoxLayout;
    start_time = new QLineEdit;
    start_time->setPlaceholderText("the start");
    end_time = new QLineEdit;
    end_time->setPlaceholderText("the end");
    cut->addWidget(new QLabel("From"));
    cut->addWidget(start_time);
    cut->addWidget(new QLabel("to"));
    cut->addWidget(end_time);
    form->addRow("Trim:", cut);
    auto *turn = new QHBoxLayout;
    rotate = new QComboBox;
    rotate->addItem("Not turned", 0);
    rotate->addItem("90° clockwise", 90);
    rotate->addItem("180°", 180);
    rotate->addItem("90° counter-clockwise", 270);
    flip = new QCheckBox("Mirrored");
    turn->addWidget(rotate);
    turn->addWidget(flip);
    turn->addStretch(1);
    form->addRow("Turn:", turn);
    hw = new QCheckBox("Use the graphics card (faster, usually larger files)");
    hw->hide();
    form->addRow("", hw);
    extra = new QLineEdit;
    extra->setPlaceholderText("other ffmpeg options, as they are (optional)");
    form->addRow("Extra options:", extra);
    name = new QLineEdit;
    if (paths.size() == 1)
        form->addRow("Name:", name);
    else
        name->hide();
    output = new OutputRow("Beside the originals");
    form->addRow("Save:", output);
    estimate = new QLabel;
    lay->addWidget(estimate);
    problem = new QLabel;
    problem->setWordWrap(true);
    problem->setStyleSheet("color: #c01c28");
    lay->addWidget(problem);
    preview = command_box();
    lay->addWidget(preview);
    go = buttons(this, lay, "Convert");
    connect(go, &QPushButton::clicked, this, &VideoDialog::start);

    // the last preset used; Custom with the settings it had
    QJsonObject r = remembered("convert/video");
    int pi = preset->findData(r.value("preset").toString("mp4"));
    filling = true;
    preset->setCurrentIndex(pi >= 0 ? pi : 0);
    filling = false;
    if (preset->currentData() == "custom")
        set_spec(media::VideoSpec::from_json(r.value("custom").toObject()));
    else
        choose_preset(preset->currentIndex());
    connect(preset, &QComboBox::currentIndexChanged, this, &VideoDialog::choose_preset);
    for (QComboBox *c : {container, vcodec, resolution, fps, audio, abitrate, rotate})
        connect(c, &QComboBox::currentIndexChanged, this, &VideoDialog::changed);
    for (QSlider *s : {quality, speed})
        connect(s, &QSlider::valueChanged, this, &VideoDialog::changed);
    for (QCheckBox *c : {target, flip, hw})
        connect(c, &QCheckBox::toggled, this, &VideoDialog::changed);
    connect(target_mb, &QDoubleSpinBox::valueChanged, this, &VideoDialog::changed);
    for (QLineEdit *e : {start_time, end_time, extra, name})
        connect(e, &QLineEdit::textEdited, this, &VideoDialog::changed);
    connect(output, &OutputRow::changed, this, &VideoDialog::update_preview);
    // the first video's details and what this ffmpeg can do, read on a worker
    QPointer<VideoDialog> me(this);
    QString src = paths.value(0);
    fileops::run_task(
        this, "",
        [src]() {
            media::encoders();
            return QVariant::fromValue(media::probe(src));
        },
        [me](const QVariant &res) {
            if (!me)
                return;
            me->encs = media::encoders();
            me->first = res.value<media::Probe>();
            int n = int(me->paths.size());
            me->details->setText((n > 1 ? QString("%1 videos; the first: ").arg(n) : QString()) + media::describe(me->first));
            QStringList hws = media::hardware(me->encs);
            me->hw->setVisible(!hws.isEmpty());
            me->set_spec(me->spec());   // the codecs this ffmpeg has
        },
        true);
}

media::VideoSpec VideoDialog::spec() const
{
    media::VideoSpec s;
    s.container = container->currentData().toString();
    s.vcodec = s.container == "gif" ? QString("gif") : vcodec->currentData().toString();
    if (s.vcodec.isEmpty())
        s.vcodec = "h264";
    s.quality = quality->value();
    s.target_mb = target->isChecked() ? target_mb->value() : 0;
    s.height = resolution->currentData().toInt();
    s.fps = fps->currentData().toInt();
    s.audio = audio->currentData().toString();
    s.abitrate = abitrate->currentData().toInt();
    s.speed = speed->value();
    s.start = start_time->text().trimmed();
    s.end = end_time->text().trimmed();
    s.rotate = rotate->currentData().toInt();
    s.flip = flip->isChecked();
    QStringList hws = media::hardware(encs);
    s.hw = hw->isChecked() && !hws.isEmpty() ? hws.first() : QString();
    s.extra = extra->text().trimmed();
    return s;
}

void VideoDialog::set_spec(const media::VideoSpec &s)
{
    bool was = filling;
    filling = true;
    container->setCurrentIndex(std::max(0, container->findData(s.container)));
    vcodec->clear();
    for (const auto &[id, n] : QList<QPair<QString, QString>>{{"h264", "H.264 (plays everywhere)"},
                                                              {"hevc", "H.265 (smaller, newer devices)"},
                                                              {"vp9", "VP9 (the web)"},
                                                              {"av1", "AV1 (smallest, slow)"},
                                                              {"copy", "Copy as it is (no re-encoding)"}}) {
        media::VideoSpec t;
        t.vcodec = id;
        if (encs.isEmpty() || id == "copy" || !media::video_encoder(t, encs).isEmpty())
            vcodec->addItem(n, id);
    }
    vcodec->setCurrentIndex(std::max(0, vcodec->findData(s.vcodec)));
    quality->setValue(s.quality);
    target->setChecked(s.target_mb > 0);
    if (s.target_mb > 0)
        target_mb->setValue(s.target_mb);
    resolution->setCurrentIndex(std::max(0, resolution->findData(s.height)));
    fps->setCurrentIndex(std::max(0, fps->findData(s.fps)));
    audio->setCurrentIndex(std::max(0, audio->findData(s.audio)));
    abitrate->setCurrentIndex(std::max(0, abitrate->findData(s.abitrate)));
    speed->setValue(s.speed);
    rotate->setCurrentIndex(std::max(0, rotate->findData(s.rotate)));
    flip->setChecked(s.flip);
    hw->setChecked(!s.hw.isEmpty());
    extra->setText(s.extra);
    filling = was;
    update_preview();
}

void VideoDialog::choose_preset(int i)
{
    if (filling || i < 0 || i >= presets.size())
        return;   // (Custom: the settings as they are)
    media::VideoSpec s = presets[i].spec;
    s.start = start_time->text().trimmed();
    s.end = end_time->text().trimmed();
    set_spec(s);
}

void VideoDialog::changed()
{
    if (filling)
        return;
    // any change from the preset's settings (but the trim, which is each video's own) makes it Custom
    media::VideoSpec s = spec();
    s.start.clear(), s.end.clear();
    int i = preset->currentIndex();
    if (i >= 0 && i < presets.size() && !(presets[i].spec == s)) {
        filling = true;
        preset->setCurrentIndex(preset->count() - 1);
        filling = false;
    }
    update_preview();
}

QString VideoDialog::out_path(const QString &src) const
{
    QString dir = output->folder().isEmpty() ? dirname(src) : output->folder();
    media::VideoSpec s = spec();
    if (paths.size() == 1 && !name->text().trimmed().isEmpty())
        return join(dir, name->text().trimmed());
    return media::output_path(src, dir, media::audio_only(s.container) ? QString() : QString(".converted"),
                              media::video_ext(s));
}

void VideoDialog::update_preview()
{
    media::VideoSpec s = spec();
    bool sound_only = media::audio_only(s.container), gif = s.container == "gif", copy = s.vcodec == "copy";
    for (QWidget *w : QList<QWidget *>{vcodec, resolution, fps, rotate, flip, speed, quality, target, target_mb, hw})
        w->setEnabled(!sound_only);
    vcodec->setEnabled(!sound_only && !gif);
    for (QWidget *w : QList<QWidget *>{quality, resolution, rotate, flip, speed, target, target_mb, hw})
        w->setEnabled(w->isEnabled() && !copy);
    quality->setEnabled(quality->isEnabled() && !target->isChecked() && !gif);
    target_mb->setEnabled(target->isChecked() && target->isEnabled());
    audio->setEnabled(!gif && !sound_only);
    abitrate->setEnabled(!gif && (sound_only || s.audio == "encode"));
    fps->setEnabled(!sound_only && !copy);
    if (paths.size() == 1) {
        QString wanted = basename(media::output_path(paths.first(), dirname(paths.first()),
                                                     sound_only ? QString() : QString(".converted"), media::video_ext(s)));
        if (!name->isModified())
            name->setText(wanted);
        else if (ext_of(name->text()) != media::video_ext(s))   // a new format: the name follows it
            name->setText(split_ext(name->text()).first + media::video_ext(s));
    }
    media::Args a = media::video_args(s, first, encs.isEmpty() ? QSet<QString>{"libx264", "libx265", "libvpx-vp9", "libsvtav1", "aac", "libopus", "libmp3lame", "gif"} : encs);
    problem->setText(a.error);
    go->setEnabled(a.error.isEmpty());
    double mb = media::estimate_mb(s, first);
    estimate->setText(mb > 0 ? QString("Roughly %1 for the first video%2.")
                                   .arg(human_size(qint64(mb * 1e6)), s.target_mb > 0 || sound_only || copy ? QString() : QString(" (it depends on what's in it)"))
                             : QString());
    preview->setPlainText(a.error.isEmpty() && !paths.isEmpty() ? shell_join(media::command(paths.first(), a, out_path(paths.first())))
                                                                : QString());
}

void VideoDialog::start()
{
    if (!output_ok(this, output))
        return;
    media::VideoSpec s = spec();
    QJsonObject r = remembered("convert/video");
    r["preset"] = preset->currentData().toString();
    if (preset->currentData() == "custom") {
        media::VideoSpec keep = s;
        keep.start.clear(), keep.end.clear();
        r["custom"] = keep.to_json();
    }
    remember("convert/video", r);
    QSet<QString> e = encs.isEmpty() ? media::encoders() : encs;
    QStringList srcs = paths;
    QStringList dsts;
    for (const QString &src : srcs)
        dsts << out_path(src);
    auto one = [srcs, dsts, s, e](Task *task, int i, double base, double share) -> QStringList {
        media::Probe p = media::probe(srcs[i]);
        if (!p.ok)
            throw Error(p.error);
        media::Args a = media::video_args(s, p, e);
        if (!a.error.isEmpty())
            throw Error(a.error);
        bool ok = true;
        double start = media::parse_time(s.start, &ok);
        double length = s.end.isEmpty() ? p.duration - start : media::parse_time(s.end, &ok) - start;
        QString dst = lexists(dsts[i]) ? unique_path(dirname(dsts[i]), basename(dsts[i]), "num") : dsts[i];
        return {media::run(task, srcs[i], a, dst, length, basename(srcs[i]), base, share)};
    };
    QString what = preset->currentText();
    QString title = srcs.size() == 1 ? "Converting “" + basename(srcs.first()) + "”"
                                     : QString("Converting %1 videos").arg(srcs.size());
    run_batch(win, title, "videos", srcs, lanes_for(srcs, output->folder()),
              {QString("Convert Video (%1): %2").arg(what, some(int(srcs.size()), "1 video", "videos")),
               "ffmpeg writes each video under a hidden name, given its name when it's finished; a stop or a failure "
               "leaves no half-made video, and no file is replaced."},
              one, output->trash->isChecked());
    close();
}

// ---------------------------------------------------------------- audio

static const QHash<QString, QString> FORMAT_NAMES = {
    {"mp3", "MP3"},  {"m4a", "M4A (AAC)"}, {"opus", "Opus"}, {"ogg", "OGG (Vorbis)"},
    {"flac", "FLAC (lossless)"}, {"wav", "WAV (lossless, uncompressed)"}, {"alac", "ALAC (lossless, Apple)"}};

AudioDialog::AudioDialog(MainWindow *win, const QStringList &paths_, bool from_video_)
    : QDialog(win), from_video(from_video_), win(win)
{
    if (from_video) {
        paths = paths_;
    } else {
        paths = audio_files(paths_);
        for (const QString &p : paths_)
            if (isdir(p))
                roots << p;
    }
    QString first_name = basename(paths.value(0));
    setWindowTitle(from_video ? (paths.size() == 1 ? "Extract Audio — " + first_name : QString("Extract Audio from %1 Videos").arg(paths.size()))
                              : (paths.size() == 1 ? "Convert Audio — " + first_name : QString("Convert %1 Audio Files").arg(paths.size())));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *lay = new QVBoxLayout(this);
    details = new QLabel(paths.isEmpty() ? QString("There are no audio files there.") : QString("Reading the details…"));
    details->setWordWrap(true);
    lay->addWidget(details);
    auto *form = new QFormLayout;
    lay->addLayout(form);
    tracks = new QComboBox;
    if (from_video)
        form->addRow("Sound track:", tracks);
    else
        tracks->hide();
    keep = new QRadioButton("Keep the sound as it is (instant, no quality lost)");
    convert = new QRadioButton("Convert it:");
    auto *group = new QButtonGroup(this);
    group->addButton(keep);
    group->addButton(convert);
    if (from_video) {
        form->addRow("", keep);
        form->addRow("", convert);
    } else {
        keep->hide();
        convert->hide();
    }
    format = new QComboBox;
    form->addRow("Format:", format);
    bitrate = new QComboBox;
    for (const auto &[n, b] : QList<QPair<QString, int>>{{"Best (320 kbps)", 320}, {"Very good (256 kbps)", 256},
                                                          {"Good (192 kbps)", 192}, {"Smaller (128 kbps)", 128},
                                                          {"Smallest (96 kbps)", 96}, {"Speech (64 kbps)", 64}})
        bitrate->addItem(n, b);
    form->addRow("Quality:", bitrate);
    rate = new QComboBox;
    rate->addItem("Keep", 0);
    rate->addItem("44.1 kHz (CD)", 44100);
    rate->addItem("48 kHz", 48000);
    channels = new QComboBox;
    channels->addItem("Keep", 0);
    channels->addItem("Stereo", 2);
    channels->addItem("Mono", 1);
    if (!from_video) {
        form->addRow("Sample rate:", rate);
        form->addRow("Channels:", channels);
    } else {
        rate->hide();
        channels->hide();
    }
    loudnorm = new QCheckBox("Even out the volume, so they all play as loud (slower: each file is read twice)");
    if (!from_video)
        form->addRow("", loudnorm);
    else
        loudnorm->hide();
    output = new OutputRow("Beside the originals");
    if (!roots.isEmpty()) {
        output->into->setChecked(true);
        output->path->setText(unique_path(dirname(roots.first()), basename(roots.first()) + " (converted)", "num"));
    }
    form->addRow("Save:", output);
    auto *tags = new QLabel(from_video ? QString() : QString("Tags (title, artist, album, track) and cover pictures are kept where the format takes them."));
    tags->setWordWrap(true);
    tags->setForegroundRole(QPalette::PlaceholderText);
    lay->addWidget(tags);
    preview = command_box();
    lay->addWidget(preview);
    go = buttons(this, lay, from_video ? "Extract" : "Convert");
    go->setEnabled(!paths.isEmpty());
    connect(go, &QPushButton::clicked, this, &AudioDialog::start);

    QJsonObject r = remembered(from_video ? "convert/extract" : "convert/audio");
    (r.value("format").toString("copy") == "copy" && from_video ? keep : convert)->setChecked(true);
    bitrate->setCurrentIndex(std::max(0, bitrate->findData(r.value("bitrate").toInt(192))));
    rate->setCurrentIndex(std::max(0, rate->findData(r.value("rate").toInt())));
    channels->setCurrentIndex(std::max(0, channels->findData(r.value("channels").toInt())));
    loudnorm->setChecked(r.value("loudnorm").toBool());
    QString want = r.value("format").toString(from_video ? "mp3" : "mp3");
    auto fill_formats = [this, want](const QSet<QString> &encs) {
        format->clear();
        for (const QString &f : media::audio_formats(encs))
            if (f != "copy")
                format->addItem(FORMAT_NAMES.value(f, f), f);
        format->setCurrentIndex(std::max(0, format->findData(want)));
    };
    fill_formats({"libmp3lame", "aac", "libopus", "flac", "pcm_s16le", "alac"});
    for (QComboBox *c : {tracks, format, bitrate, rate, channels})
        connect(c, &QComboBox::currentIndexChanged, this, &AudioDialog::update_preview);
    for (QAbstractButton *b : QList<QAbstractButton *>{keep, convert, loudnorm})
        connect(b, &QAbstractButton::toggled, this, &AudioDialog::update_preview);
    connect(output, &OutputRow::changed, this, &AudioDialog::update_preview);
    QPointer<AudioDialog> me(this);
    QString src = paths.value(0);
    if (src.isEmpty())
        return;
    fileops::run_task(
        this, "",
        [src]() {
            media::encoders();
            return QVariant::fromValue(media::probe(src));
        },
        [me, fill_formats](const QVariant &res) {
            if (!me)
                return;
            QSignalBlocker quiet(me->format);
            fill_formats(media::encoders());
            me->first = res.value<media::Probe>();
            int n = int(me->paths.size());
            QString what = me->from_video ? "videos" : "files";
            me->details->setText((n > 1 ? QString("%1 %2; the first: ").arg(QString::number(n), what) : QString()) +
                                 media::describe(me->first));
            QSignalBlocker quiet_tracks(me->tracks);
            me->tracks->clear();
            QList<media::Stream> audio = me->first.audio();
            if (n == 1) {
                for (int i = 0; i < audio.size(); ++i) {
                    const media::Stream &a = audio[i];
                    QStringList bits{media::codec_name(a.codec)};
                    if (a.channels)
                        bits << (a.channels == 1 ? QString("mono") : a.channels == 2 ? QString("stereo") : QString("%1 channels").arg(a.channels));
                    if (!a.lang.isEmpty() && a.lang != "und")
                        bits << a.lang;
                    if (!a.title.isEmpty())
                        bits << a.title;
                    me->tracks->addItem(QString("%1: %2").arg(QString::number(i + 1), bits.join(", ")), i);
                }
            } else {
                me->tracks->addItem("The first track", 0);
            }
            if (audio.size() > 1 || n > 1)
                me->tracks->addItem("Every track (a file for each)", -1);
            me->update_preview();
        },
        true);
}

media::AudioSpec AudioDialog::spec() const
{
    media::AudioSpec s;
    s.format = from_video && keep->isChecked() ? QString("copy") : format->currentData().toString();
    s.bitrate = bitrate->currentData().toInt();
    s.rate = from_video ? 0 : rate->currentData().toInt();
    s.channels = from_video ? 0 : channels->currentData().toInt();
    s.loudnorm = !from_video && loudnorm->isChecked();
    return s;
}

int AudioDialog::track() const { return tracks->count() ? tracks->currentData().toInt() : 0; }

// where a file's sound goes: dir, or the chosen folder (with the file's subfolders under the folder it was found in)
static QString audio_dir(const QString &src, const QString &folder, const QStringList &roots)
{
    if (folder.isEmpty())
        return dirname(src);
    for (const QString &r : roots)
        if (src.startsWith(r + "/"))
            return join(folder, relpath(dirname(src), r));
    return folder;
}

// the outputs' names: "clip.m4a"; one per track: "clip (track 2, eng).m4a"
static QString track_suffix(const media::Stream &a, int i, bool several)
{
    if (!several)
        return QString();
    return QString(" (track %1%2)").arg(QString::number(i + 1), a.lang.isEmpty() || a.lang == "und" ? QString() : ", " + a.lang);
}

void AudioDialog::update_preview()
{
    media::AudioSpec s = spec();
    bool lossy = !media::lossless(s.format);
    format->setEnabled(!from_video || convert->isChecked());
    bitrate->setEnabled(format->isEnabled() && lossy);
    if (paths.isEmpty() || !first.ok) {
        preview->clear();
        return;
    }
    QList<media::Stream> audio = first.audio();
    int t = std::max(0, track());
    QString codec = audio.value(t).codec;
    keep->setText(QString("Keep the sound as it is (instant, no quality lost)%1")
                      .arg(codec.isEmpty() ? QString() : QString(": %1 → %2").arg(media::codec_name(codec), media::audio_ext("copy", codec))));
    media::Args a = media::audio_args(s, first, t, s.loudnorm ? QString("{}") : QString());
    if (!a.error.isEmpty()) {
        preview->setPlainText(a.error);
        return;
    }
    QString dir = audio_dir(paths.first(), output->folder(), roots);
    QString dst = media::output_path(paths.first(), dir, track_suffix(audio.value(t), t, track() < 0),
                                     media::audio_ext(s.format, codec));
    preview->setPlainText(shell_join(media::command(paths.first(), a, dst)) +
                          (s.loudnorm ? QString("\n(after a first pass measuring its loudness)") : QString()));
}

void AudioDialog::start()
{
    if (paths.isEmpty() || !output_ok(this, output))
        return;
    media::AudioSpec s = spec();
    QJsonObject r{{"format", s.format}, {"bitrate", s.bitrate}, {"rate", s.rate}, {"channels", s.channels},
                  {"loudnorm", s.loudnorm}};
    remember(from_video ? "convert/extract" : "convert/audio", r);
    QStringList srcs = paths, rs = roots;
    QString folder = output->folder();
    int which_track = track();
    auto one = [srcs, rs, folder, s, which_track](Task *task, int i, double base, double share) -> QStringList {
        media::Probe p = media::probe(srcs[i]);
        if (!p.ok)
            throw Error(p.error);
        QList<media::Stream> audio = p.audio();
        if (audio.isEmpty())
            throw Error("It has no sound.");
        QList<int> picks;
        if (which_track < 0)
            for (int t = 0; t < audio.size(); ++t)
                picks << t;
        else
            picks << std::min(which_track, int(audio.size()) - 1);
        QString dir = audio_dir(srcs[i], folder, rs);
        makedirs(dir, true);
        QStringList made;
        for (int k = 0; k < picks.size(); ++k) {
            int t = picks[k];
            QString measured = s.loudnorm ? media::measure_loudness(task, srcs[i], t) : QString();
            media::Args a = media::audio_args(s, p, t, measured);
            if (!a.error.isEmpty())
                throw Error(a.error);
            QString dst = media::output_path(srcs[i], dir, track_suffix(audio[t], t, picks.size() > 1),
                                             media::audio_ext(s.format, audio[t].codec));
            double part = share / picks.size();
            made << media::run(task, srcs[i], a, dst, p.duration, basename(srcs[i]), base + k * part, part);
        }
        return made;
    };
    QString title = from_video ? (srcs.size() == 1 ? "Extracting the sound of “" + basename(srcs.first()) + "”"
                                                   : QString("Extracting the sound of %1 videos").arg(srcs.size()))
                               : (srcs.size() == 1 ? "Converting “" + basename(srcs.first()) + "”"
                                                   : QString("Converting %1 audio files").arg(srcs.size()));
    QString how = s.format == "copy" ? QString("as it is") : FORMAT_NAMES.value(s.format, s.format);
    run_batch(win, title, from_video ? "videos" : "files", srcs, lanes_for(srcs, folder),
              {QString("%1: %2, %3").arg(from_video ? "Extract Audio" : "Convert Audio",
                                         some(int(srcs.size()), "1 file", "files"), how),
               "ffmpeg writes each file under a hidden name, given its name when it's finished; a stop or a failure "
               "leaves no half-made file, and no file is replaced."},
              one, output->trash->isChecked());
    close();
}

// ---------------------------------------------------------------- opening them

void image_dialog(MainWindow *win, const QStringList &paths)
{
    QStringList imgs;
    for (const QString &p : paths)
        if (is_image(p))
            imgs << p;
    if (!imgs.isEmpty())
        show_free(new ImageDialog(win, imgs));
}

void video_dialog(MainWindow *win, const QStringList &paths)
{
    QStringList v;
    for (const QString &p : paths)
        if (is_video(p))
            v << p;
    if (!v.isEmpty())
        show_free(new VideoDialog(win, v));
}

void extract_audio_dialog(MainWindow *win, const QStringList &paths)
{
    QStringList v;
    for (const QString &p : paths)
        if (is_video(p))
            v << p;
    if (!v.isEmpty())
        show_free(new AudioDialog(win, v, true));
}

void audio_dialog(MainWindow *win, const QStringList &paths) { show_free(new AudioDialog(win, paths, false)); }

}  // namespace convert
