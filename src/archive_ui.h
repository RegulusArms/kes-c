// Compress / Extract dialogs and the flows that run archive jobs (see archive.h) in the status bar.
#pragma once

#include "archive.h"

#include <QDialog>

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

    QStringList paths, rels;
    QString base;
    bool one_file;
    QList<archive::Format> fmts;
    bool loading = true;
    QLineEdit *name, *dest, *pw, *pw2, *extra;
    QLabel *ext, *level_lbl, *level_hint, *enc_note, *no_enc, *zip_enc_lbl;
    QComboBox *fmt, *tool, *method, *zip_enc, *vol_unit;
    QSlider *level;
    QSpinBox *threads, *vol, *recovery;
    QGroupBox *enc, *split;
    QCheckBox *enc_names, *solid, *trash_after;
    QPlainTextEdit *preview;
    QList<QWidget *> tool_row, level_row, method_row, threads_row, recovery_row;
};

class ExtractDialog : public QDialog {
    Q_OBJECT
public:
    ExtractDialog(QWidget *parent, const QString &path, bool encrypted);
    QVariantMap options() const;

private:
    void browse();
    void ok();
    QString path;
    bool encrypted;
    QLineEdit *dest, *pw;
    QCheckBox *subfolder, *trash_after, *open_after;
    QComboBox *overwrite;
};

namespace archive_ui {

QString ask_password(QWidget *parent, const QString &path, bool wrong = false);   // null if cancelled
void extract_here(MainWindow *win, const QString &path);
void extract_dialog(MainWindow *win, const QString &path);
// Tell the user which of the archive's symlinks were left out for leading outside the folder, and warn about any that
// couldn't be removed. dropped: [path, target, why] each (why empty: removed).
void say_dropped_links(QWidget *win, const QString &name, const QVariantList &dropped);
void compress_dialog(MainWindow *win, const QStringList &paths);
void run_compress(MainWindow *win, const archive::Spec &spec);
QString quick_compress_label(const QStringList &paths);   // empty if there are no usable last settings
void quick_compress(MainWindow *win, const QStringList &paths);

}  // namespace archive_ui
