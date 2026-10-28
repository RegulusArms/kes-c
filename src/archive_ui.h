// Compress / Extract dialogs and the flows that run archive jobs (see archive.h) in the status bar.
#pragma once

#include "archive.h"

#include <QDialog>
#include <QMap>
#include <QPointer>

#include <functional>

class MainWindow;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSlider;
class QSpinBox;

// Pick a format, tool and every option it supports; spec() describes the job for archive::compress().
class CompressDialog : public QDialog {
    Q_OBJECT
public:
    CompressDialog(QWidget *parent, const QStringList &paths);
    archive::Spec spec() const;

private:
    const archive::Format &format() const;
    QString key(const QString &name) const;
    void format_changed();
    void tool_changed();
    void level_changed();
    void browse();
    void update_preview();
    void ok();
    void save(const archive::Spec &sp);   // ok(), once a replace is confirmed

    QStringList paths, rels;
    QString base;
    bool one_file;
    QList<archive::Format> fmts;
    bool loading = true;
    QLineEdit *name, *dest, *pw, *pw2, *extra;
    QLabel *ext, *level_lbl, *level_hint, *enc_note, *no_enc, *zip_enc_lbl;
    QComboBox *fmt, *tool, *method, *zip_enc, *vol_size, *vol_unit;   // vol_size: a standard size or Custom…
    QSlider *level;
    QSpinBox *threads, *vol, *recovery;
    QGroupBox *enc, *split, *sums;   // sums: a checksum file made as it's written (tar formats)
    QMap<QString, QCheckBox *> sum_boxes;   // by algorithm
    QCheckBox *enc_names, *solid, *trash_after, *sum_one;
    QPlainTextEdit *preview;
    QList<QWidget *> tool_row, level_row, method_row, threads_row, recovery_row;
    qint64 volume_bytes() const;   // the size chosen (standard, or Custom…'s number and unit)
    void volume_choice_changed();
};

class ExtractDialog : public QDialog {
    Q_OBJECT
public:
    // several paths: Extract All To… (no password field: each one's is found or asked for as it comes).
    // offer_remember: a keyring is running, to save the password in
    ExtractDialog(QWidget *parent, const QStringList &paths, bool encrypted, bool offer_remember = false);
    QVariantMap options() const;

private:
    void browse();
    void ok();
    void save();   // ok(), once a missing folder is made
    QStringList paths;
    QString path;   // the first
    bool encrypted;
    QLineEdit *dest, *pw;
    QCheckBox *subfolder, *trash_after, *open_after, *keep_damaged = nullptr, *remember = nullptr;
    QComboBox *overwrite;
};

namespace archive_ui {

// Why the password is asked for: the first time, the one typed was wrong, the one typed for an earlier archive of
// Extract All doesn't open this one
enum class Ask { First, Wrong, NotSame };
// Asks for the archive's password: ok(password, remember) runs with it; cancelled() when it's closed, cancelled or left
// empty. offer_remember: a "Remember this password" box (a keyring is running). Returns the window, already shown.
QDialog *ask_password(QWidget *parent, const QString &path, Ask reason, bool offer_remember,
                      std::function<void(const QString &, bool)> ok, std::function<void()> cancelled = nullptr);
// Extracting runs one job for one or several archives (extract_all): each one's password is found the same way, first
// one typed in this job (or the dialog), then the one saved in the keyring for this archive, then the other saved ones
// (a quick test each), and only when none opens it is it asked for.
void extract_here(MainWindow *win, const QString &path);
void extract_dialog(MainWindow *win, const QString &path);
// The archives among paths, each split set (x.part1.rar … x.part3.rar) once, by its first part; in their order
QStringList archives_of(const QStringList &paths);
// Extract All Here / Extract All To…: each archive as Extract Here / Extract To… would, one after another in one job,
// and one summary at the end
void extract_all_here(MainWindow *win, const QStringList &archives);
void extract_all_dialog(MainWindow *win, const QStringList &archives);
// opts: dest (empty: next to each archive), subfolder, overwrite, password, remember, trash_after, open_after,
// keep_damaged
Task *extract_all(MainWindow *win, const QStringList &archives, const QVariantMap &opts);   // its job
// On a job's thread: the password that opens `path`: the one saved for it, the other saved ones (a quick test each),
// then asking, with a window in parent's place. Empty when asking was cancelled. tried: ones known not to open it.
QString job_password(Task *task, QPointer<QWidget> parent, const QString &path, QStringList &tried);
// Test Archive(s): each archive decompressed and checked, nothing written; one job, and one message at the end
Task *test_archives(MainWindow *win, const QStringList &archives);
// Open as Archive: a window listing what's inside, to extract single items (archivewin.h)
void browse(MainWindow *win, const QString &path);
// Tell the user which of the archive's symlinks were left out for leading outside the folder, and warn about any that
// couldn't be removed. dropped: [path, target, why] each (why empty: removed).
void say_dropped_links(QWidget *win, const QString &name, const QVariantList &dropped);
void compress_dialog(MainWindow *win, const QStringList &paths);
void run_compress(MainWindow *win, const archive::Spec &spec);
QString quick_compress_label(const QStringList &paths);   // empty if there are no usable last settings
void quick_compress(MainWindow *win, const QStringList &paths);

}  // namespace archive_ui
