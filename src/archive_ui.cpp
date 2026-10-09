#include "archive_ui.h"

#include "app.h"
#include "fileops.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QVBoxLayout>

using namespace util;

static const char *ARGV_NOTE = "%1 only accepts the password on its command line, so other users on this computer "
                               "could see it in the process list while the job runs.";

static QLabel *small(const QString &text)
{
    auto *lbl = new QLabel(text);
    lbl->setWordWrap(true);
    QFont f = lbl->font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    lbl->setFont(f);
    lbl->setStyleSheet("color: palette(placeholder-text)");
    return lbl;
}

// password field with a "Show" toggle
static QHBoxLayout *password_row(QLineEdit *edit, QCheckBox **show_out = nullptr)
{
    edit->setEchoMode(QLineEdit::Password);
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(edit, 1);
    auto *show = new QCheckBox("Show");
    QObject::connect(show, &QCheckBox::toggled, edit,
                     [edit](bool on) { edit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
    row->addWidget(show);
    if (show_out)
        *show_out = show;
    return row;
}

// the only tool that gets the password on its command line (unrar reads it from stdin, unzip from $UNZIP)
static bool argv_tool(const QString &t) { return t == "zpaq"; }

namespace archive_ui {

QString ask_password(QWidget *parent, const QString &path, bool wrong)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("Password Required");
    auto *lay = new QVBoxLayout(&dlg);
    QString msg = wrong ? QString("<b>Wrong password</b> for “%1”. Try again:").arg(basename(path).toHtmlEscaped())
                        : QString("“%1” is encrypted. Enter its password:").arg(basename(path));
    auto *lbl = new QLabel(msg);
    lbl->setTextFormat(wrong ? Qt::RichText : Qt::PlainText);
    lbl->setWordWrap(true);
    lay->addWidget(lbl);
    auto *edit = new QLineEdit;
    lay->addLayout(password_row(edit));
    QString t = archive::extract_tool(path);
    if (argv_tool(t))
        lay->addWidget(small(QString(ARGV_NOTE).arg(t)));
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText("Extract");
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    dlg.resize(420, dlg.sizeHint().height());
    edit->setFocus();
    if (dlg.exec() && !edit->text().isEmpty())
        return edit->text();
    return QString();
}

}  // namespace archive_ui

// ---------------------------------------------------------------- compress

static void show_row(const QList<QWidget *> &row, bool on)
{
    for (QWidget *w : row)
        w->setVisible(on);
}

CompressDialog::CompressDialog(QWidget *parent, const QStringList &paths_) : QDialog(parent), paths(paths_)
{
    setWindowTitle("Compress");
    QStringList parents;
    for (const QString &p : paths)
        parents << dirname(p);
    base = commonpath(parents);
    for (const QString &p : paths)
        rels << relpath(p, base);
    one_file = paths.size() == 1 && isfile(paths[0]) && !islink(paths[0]);
    fmts = archive::formats();
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    lay->addLayout(form);

    QString def = paths.size() == 1 ? basename(rstrip(paths[0], '/')) : basename(base);
    if (def.isEmpty())
        def = "Archive";
    name = new QLineEdit(def);
    ext = new QLabel;
    auto *row = new QHBoxLayout;
    row->addWidget(name, 1);
    row->addWidget(ext);
    form->addRow("Archive name:", row);
    dest = new QLineEdit(base);
    auto *browse_btn = new QPushButton("Browse…");
    connect(browse_btn, &QPushButton::clicked, this, &CompressDialog::browse);
    row = new QHBoxLayout;
    row->addWidget(dest, 1);
    row->addWidget(browse_btn);
    form->addRow("Location:", row);

    fmt = new QComboBox;
    auto *fmt_model = qobject_cast<QStandardItemModel *>(fmt->model());
    for (const archive::Format &f : fmts) {
        bool usable = !f.available.isEmpty() && (!f.single || one_file);
        QString text = !f.available.isEmpty() ? f.label : QString("%1 — install %2").arg(f.label, archive::install_hint(f.tools[0]));
        fmt->addItem(text, f.id);
        if (!usable)
            fmt_model->item(fmt->count() - 1)->setEnabled(false);
    }
    form->addRow("Format:", fmt);
    tool = new QComboBox;
    auto *tool_lbl = new QLabel("Program:");
    form->addRow(tool_lbl, tool);
    tool_row = {tool_lbl, tool};

    level = new QSlider(Qt::Horizontal);
    level_lbl = new QLabel;
    level_lbl->setMinimumWidth(90);
    auto *level_box = new QWidget;
    row = new QHBoxLayout(level_box);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(level, 1);
    row->addWidget(level_lbl);
    auto *level_title = new QLabel("Compression level:");
    form->addRow(level_title, level_box);
    level_row = {level_title, level_box};
    level_hint = small("");
    form->addRow(level_hint);
    method = new QComboBox;
    auto *method_lbl = new QLabel("Method:");
    form->addRow(method_lbl, method);
    method_row = {method_lbl, method};
    threads = new QSpinBox;
    threads->setRange(0, archive::cpu_count() * 2);
    threads->setSpecialValueText(QString("Auto (%1 cores)").arg(archive::cpu_count()));
    auto *threads_lbl = new QLabel("CPU threads:");
    form->addRow(threads_lbl, threads);
    threads_row = {threads_lbl, threads};

    // encryption
    enc = new QGroupBox("Encrypt with a password");
    enc->setCheckable(true);
    enc->setChecked(false);
    auto *ef = new QFormLayout(enc);
    pw = new QLineEdit;
    pw2 = new QLineEdit;
    QCheckBox *show = nullptr;
    ef->addRow("Password:", password_row(pw, &show));
    pw2->setEchoMode(QLineEdit::Password);
    connect(show, &QCheckBox::toggled, this,
            [this](bool on) { pw2->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
    ef->addRow("Confirm:", pw2);
    enc_names = new QCheckBox("Also encrypt file names (nothing can be listed without the password)");
    ef->addRow(enc_names);
    zip_enc = new QComboBox;
    zip_enc->addItem("AES-256 (secure; needs 7-Zip, WinRAR or a recent unzip)", "AES256");
    zip_enc->addItem("ZipCrypto (weak, but opens everywhere)", "ZipCrypto");
    zip_enc_lbl = new QLabel("Zip encryption:");
    ef->addRow(zip_enc_lbl, zip_enc);
    enc_note = small("");
    ef->addRow(enc_note);
    lay->addWidget(enc);
    no_enc = small("");
    lay->addWidget(no_enc);

    // splitting and archive structure
    split = new QGroupBox("Split into volumes");
    split->setCheckable(true);
    split->setChecked(false);
    auto *sl = new QHBoxLayout(split);
    vol = new QSpinBox;
    vol->setRange(1, 1000000);
    vol->setValue(4000);
    vol_unit = new QComboBox;
    vol_unit->addItems({"MB", "GB"});
    sl->addWidget(new QLabel("Volume size:"));
    sl->addWidget(vol);
    sl->addWidget(vol_unit);
    sl->addStretch(1);
    lay->addWidget(split);
    auto *opts = new QFormLayout;
    lay->addLayout(opts);
    solid = new QCheckBox("Solid archive (smaller, but slower to extract single files)");
    opts->addRow(solid);
    recovery = new QSpinBox;
    recovery->setRange(0, 10);
    recovery->setSuffix(" %");
    recovery->setSpecialValueText("None");
    auto *recovery_lbl = new QLabel("Recovery record:");
    opts->addRow(recovery_lbl, recovery);
    recovery_row = {recovery_lbl, recovery};
    extra = new QLineEdit;
    extra->setPlaceholderText("Extra options passed to the program, e.g. -mfb=273 or --long=27");
    opts->addRow("Extra options:", extra);
    trash_after = new QCheckBox("Move the original files to the trash afterwards");
    opts->addRow(trash_after);

    lay->addWidget(new QLabel("Command:"));
    preview = new QPlainTextEdit;
    preview->setReadOnly(true);
    preview->setMaximumHeight(64);
    preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    lay->addWidget(preview);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText("Compress");
    connect(bb, &QDialogButtonBox::accepted, this, &CompressDialog::ok);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);

    loading = true;
    QString last = settings().value("archive/format", archive::tool("7z").isEmpty() ? "tar.gz" : "7z").toString();
    int i = fmt->findData(last);
    if (i < 0 || !fmt_model->item(i)->isEnabled()) {
        for (int j = 0; j < fmt->count(); ++j)
            if (fmt_model->item(j)->isEnabled()) {
                i = j;
                break;
            }
    }
    fmt->setCurrentIndex(std::max(i, 0));
    format_changed();
    loading = false;
    for (QLineEdit *w : {name, dest, pw, extra})
        connect(w, &QLineEdit::textChanged, this, &CompressDialog::update_preview);
    for (QGroupBox *w : {enc, split})
        connect(w, &QGroupBox::toggled, this, &CompressDialog::update_preview);
    for (QCheckBox *w : {enc_names, solid})
        connect(w, &QCheckBox::toggled, this, &CompressDialog::update_preview);
    for (QSpinBox *w : {vol, threads, recovery})
        connect(w, &QSpinBox::valueChanged, this, &CompressDialog::update_preview);
    for (QComboBox *w : {vol_unit, method, zip_enc})
        connect(w, &QComboBox::currentIndexChanged, this, &CompressDialog::update_preview);
    connect(fmt, &QComboBox::currentIndexChanged, this, &CompressDialog::format_changed);
    connect(tool, &QComboBox::currentIndexChanged, this, &CompressDialog::tool_changed);
    connect(level, &QSlider::valueChanged, this, &CompressDialog::level_changed);
    update_preview();
    resize(640, sizeHint().height());
}

const archive::Format &CompressDialog::format() const
{
    QString id = fmt->currentData().toString();
    for (const archive::Format &f : fmts)
        if (f.id == id)
            return f;
    return fmts.first();
}

QString CompressDialog::key(const QString &n) const { return QString("archive/%1/%2").arg(format().id, n); }

void CompressDialog::format_changed()
{
    const archive::Format &f = format();
    QSettings &s = settings();
    ext->setText(f.ext);
    QString first = basename(paths[0]);
    if (f.single && name->text() == first.left(first.size() - splitext_ext(first).size()))
        name->setText(first);
    tool->blockSignals(true);
    tool->clear();
    for (const QString &t : f.available)
        tool->addItem(t, t);
    QString saved = s.value(key("tool"), "").toString();
    if (tool->findData(saved) >= 0)
        tool->setCurrentIndex(tool->findData(saved));
    tool->blockSignals(false);
    show_row(tool_row, f.available.size() > 1);
    enc->setVisible(f.password);
    no_enc->setVisible(!f.password);
    no_enc->setText(QString("%1 can't be encrypted. Use 7z, zip, rar or zpaq for a password-protected archive.")
                        .arg(f.label.section(' ', 0, 0)));
    split->setVisible(f.volumes);
    solid->setVisible(f.solid);
    show_row(recovery_row, f.recovery);
    enc_names->setVisible(f.encrypt_names);
    // restore this format's last options
    solid->setChecked(s.value(key("solid"), true).toBool());
    enc_names->setChecked(s.value(key("encrypt_names"), true).toBool());
    recovery->setValue(s.value(key("recovery"), 0).toInt());
    extra->setText(s.value(key("extra"), "").toString());
    split->setChecked(s.value(key("split"), false).toBool());
    vol->setValue(s.value(key("volume"), 4000).toInt());
    vol_unit->setCurrentText(s.value(key("volume_unit"), "MB").toString());
    threads->setValue(s.value(key("threads"), 0).toInt());
    tool_changed();
}

void CompressDialog::tool_changed()
{
    const archive::Format &f = format();
    QString t = tool->currentData().toString();
    if (t.isEmpty())
        return;
    auto lv = archive::tool_levels(f, t);
    show_row(level_row, bool(lv));
    if (lv) {
        level->blockSignals(true);
        level->setRange(lv->min, lv->max);
        level->setValue(settings().value(key("level_" + t), lv->def).toInt());
        level->blockSignals(false);
    }
    QStringList methods = t == "7z" ? f.methods : QStringList();
    method->blockSignals(true);
    method->clear();
    method->addItems(methods);
    if (!methods.isEmpty())
        method->setCurrentText(settings().value(key("method"), methods.first()).toString());
    method->blockSignals(false);
    show_row(method_row, !methods.isEmpty());
    show_row(threads_row, archive::tool_threads(f, t));
    bool zip7 = f.id == "zip" && t == "7z";
    zip_enc->setVisible(zip7);
    zip_enc_lbl->setVisible(zip7);
    split->setVisible(f.volumes && !(f.id == "zip" && t == "zip"));
    static const QMap<QString, QString> names = {{"7z", "7-Zip"}, {"rar", "rar"}, {"zip", "zip"}};
    enc_note->setText(t == "zpaq" ? QString(ARGV_NOTE).arg(t)
                                  : "The password is passed to " + names.value(t, t) +
                                        " privately (not on its command line).");
    level_changed();
}

void CompressDialog::level_changed()
{
    const archive::Format &f = format();
    QString t = tool->currentData().toString();
    int v = level->value();
    if (t == "7z" || f.id == "rar") {
        static const QMap<int, QString> rar_names = {{0, "Store"}, {1, "Fastest"}, {2, "Fast"},
                                                     {3, "Normal"}, {4, "Good"},   {5, "Best"}};
        const QMap<int, QString> &names = t == "7z" ? archive::LEVEL_NAMES_7Z : rar_names;
        QString n = names.value(v);
        level_lbl->setText(n.isEmpty() ? QString::number(v) : QString("%1 — %2").arg(QString::number(v), n));
    } else if (f.id == "zpaq") {
        static const QStringList zn = {"", "Fast", "Normal", "Good", "Better", "Best"};
        level_lbl->setText(QString("%1 — %2").arg(QString::number(v), zn.value(v)));
    } else if (t == "zstd" && v > 19) {
        level_lbl->setText(QString("%1 (ultra)").arg(v));
    } else {
        level_lbl->setText(QString::number(v));
    }
    QString hint;
    if (f.id == "zpaq") {
        hint = "zpaq doesn't store symbolic links; any in the selection are skipped.";
        if (v >= 4)
            hint = "Levels 4–5 are very slow (several minutes per few hundred MB, even on many cores) and use about "
                   "500 MB of RAM per thread. Progress is shown while files are read; after that the status bar shows "
                   "the elapsed time until zpaq finishes. " +
                   hint;
    }
    level_hint->setText(hint);
    level_hint->setVisible(!hint.isEmpty());
    update_preview();
}

void CompressDialog::browse()
{
    QString d = QFileDialog::getExistingDirectory(this, "Save Archive In", dest->text().isEmpty() ? base : dest->text());
    if (!d.isEmpty())
        dest->setText(d);
}

archive::Spec CompressDialog::spec() const
{
    const archive::Format &f = format();
    QString t = tool->currentData().toString();
    auto lv = archive::tool_levels(f, t);
    bool ok = false;
    QStringList ex = shlex_split(extra->text(), &ok);
    if (!ok)
        ex.clear();
    qint64 v = vol->value() * (vol_unit->currentText() == "GB" ? 1024 : 1);
    QString n = name->text().trimmed();
    if (n.endsWith(f.ext))
        n.chop(f.ext.size());
    QString d = dest->text().trimmed();
    archive::Spec sp;
    sp.format = f;
    sp.tool = t;
    sp.base = base;
    sp.rels = rels;
    sp.out = join(expanduser(d.isEmpty() ? base : d), n + f.ext);
    if (lv)
        sp.level = level->value();
    sp.method = method->currentText();
    sp.threads = archive::tool_threads(f, t) ? threads->value() : 0;
    sp.password = f.password && enc->isChecked() ? pw->text() : QString();
    sp.encrypt_names = f.encrypt_names ? enc_names->isChecked() : false;
    sp.zip_encryption = zip_enc->currentData().toString();
    sp.volume_mb = split->isVisible() && split->isChecked() ? v : 0;
    if (f.solid)
        sp.solid = solid->isChecked();
    sp.recovery = f.recovery ? recovery->value() : 0;
    sp.extra = ex;
    sp.trash_originals = trash_after->isChecked();
    return sp;
}

void CompressDialog::update_preview()
{
    if (loading || tool->currentData().toString().isEmpty())
        return;
    try {
        preview->setPlainText(archive::command_preview(spec()));
    } catch (const std::exception &e) {
        preview->setPlainText(QString("(%1)").arg(QString::fromStdString(e.what())));
    }
}

void CompressDialog::ok()
{
    archive::Spec sp = spec();
    QString out_name = basename(sp.out);
    if (name->text().trimmed().isEmpty() || name->text().contains('/')) {
        QMessageBox::warning(this, "Compress", "Enter a name for the archive.");
        return;
    }
    if (!isdir(dirname(sp.out))) {
        QMessageBox::warning(this, "Compress", QString("“%1” is not a folder.").arg(dirname(sp.out)));
        return;
    }
    if (!sp.password.isEmpty() && pw->text() != pw2->text()) {
        QMessageBox::warning(this, "Compress", "The passwords don't match.");
        return;
    }
    if (enc->isVisible() && enc->isChecked() && sp.password.isEmpty()) {
        QMessageBox::warning(this, "Compress", "Enter a password, or turn off encryption.");
        return;
    }
    if (!extra->text().trimmed().isEmpty()) {
        bool parsed = false;
        QString err;
        shlex_split(extra->text(), &parsed, &err);
        if (!parsed) {
            QMessageBox::warning(this, "Compress", "Extra options: " + err);
            return;
        }
    }
    if (exists(sp.out) &&
        QMessageBox::question(this, "Compress", QString("“%1” already exists. Replace it?").arg(out_name)) !=
            QMessageBox::Yes)
        return;
    QSettings &s = settings();
    s.setValue("archive/format", sp.format.id);
    s.setValue(key("tool"), sp.tool);
    if (sp.level)
        s.setValue(key("level_" + sp.tool), *sp.level);
    if (!sp.method.isEmpty())
        s.setValue(key("method"), sp.method);
    s.setValue(key("threads"), threads->value());
    s.setValue(key("solid"), solid->isChecked());
    s.setValue(key("encrypt_names"), enc_names->isChecked());
    s.setValue(key("recovery"), recovery->value());
    s.setValue(key("extra"), extra->text());
    s.setValue(key("split"), split->isChecked());
    s.setValue(key("volume"), vol->value());
    s.setValue(key("volume_unit"), vol_unit->currentText());
    accept();
}

// ---------------------------------------------------------------- extract

ExtractDialog::ExtractDialog(QWidget *parent, const QString &path, bool encrypted)
    : QDialog(parent), path(path), encrypted(encrypted)
{
    setWindowTitle("Extract");
    QSettings &s = settings();
    auto *form = new QFormLayout(this);
    QString t = archive::extract_tool(path);
    form->addRow("Archive:", new QLabel(QString("%1   (using %2)").arg(basename(path), t)));
    dest = new QLineEdit(dirname(path));
    auto *browse_btn = new QPushButton("Browse…");
    connect(browse_btn, &QPushButton::clicked, this, &ExtractDialog::browse);
    auto *row = new QHBoxLayout;
    row->addWidget(dest, 1);
    row->addWidget(browse_btn);
    form->addRow("Extract to:", row);
    subfolder = new QCheckBox(QString("Into a new folder named “%1”").arg(archive::archive_stem(path)));
    subfolder->setChecked(s.value("archive/x_subfolder", true).toBool());
    form->addRow(subfolder);
    form->addRow(small("If everything in the archive is already inside one folder, that folder is used instead of "
                       "nesting it."));
    overwrite = new QComboBox;
    overwrite->addItem("Keep both (rename)", "rename");
    overwrite->addItem("Replace", "overwrite");
    overwrite->addItem("Skip", "skip");
    overwrite->setCurrentIndex(std::max(0, overwrite->findData(s.value("archive/x_overwrite", "rename"))));
    form->addRow("Existing files:", overwrite);
    connect(subfolder, &QCheckBox::toggled, this, [this](bool on) { overwrite->setEnabled(!on); });
    overwrite->setEnabled(!subfolder->isChecked());
    pw = new QLineEdit;
    form->addRow(encrypted ? "Password:" : "Password (if any):", password_row(pw));
    if (encrypted)
        form->addRow(small("This archive is encrypted."));
    if (argv_tool(t))
        form->addRow(small(QString(ARGV_NOTE).arg(t)));
    trash_after = new QCheckBox("Move the archive to the trash afterwards");
    trash_after->setChecked(s.value("archive/x_trash", false).toBool());
    form->addRow(trash_after);
    open_after = new QCheckBox("Open the extracted folder");
    open_after->setChecked(s.value("archive/x_open", false).toBool());
    form->addRow(open_after);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText("Extract");
    connect(bb, &QDialogButtonBox::accepted, this, &ExtractDialog::ok);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(bb);
    (encrypted ? pw : dest)->setFocus();
    resize(560, sizeHint().height());
}

void ExtractDialog::browse()
{
    QString d = QFileDialog::getExistingDirectory(this, "Extract To", dest->text());
    if (!d.isEmpty())
        dest->setText(d);
}

void ExtractDialog::ok()
{
    QString d = expanduser(dest->text().trimmed());
    if (!isdir(d)) {
        if (QMessageBox::question(this, "Extract", QString("“%1” doesn't exist. Create it?").arg(d)) != QMessageBox::Yes)
            return;
        try {
            makedirs(d);
        } catch (const OSError &e) {
            QMessageBox::warning(this, "Extract", e.message());
            return;
        }
    }
    if (encrypted && pw->text().isEmpty()) {
        QMessageBox::warning(this, "Extract", "This archive is encrypted. Enter its password.");
        return;
    }
    QSettings &s = settings();
    s.setValue("archive/x_subfolder", subfolder->isChecked());
    s.setValue("archive/x_overwrite", overwrite->currentData());
    s.setValue("archive/x_trash", trash_after->isChecked());
    s.setValue("archive/x_open", open_after->isChecked());
    accept();
}

QVariantMap ExtractDialog::options() const
{
    return {{"dest", expanduser(dest->text().trimmed())}, {"subfolder", subfolder->isChecked()},
            {"overwrite", overwrite->currentData()},       {"password", pw->text()},
            {"trash_after", trash_after->isChecked()},     {"open_after", open_after->isChecked()}};
}

// ---------------------------------------------------------------- flows (called from the main window)

namespace archive_ui {

static bool check_tool(MainWindow *win, const QString &path)
{
    QString missing = archive::missing_extract_tool(path);
    if (!missing.isEmpty()) {
        QMessageBox::warning(win, "Extract",
                             QString("Kestrel needs another program to open “%1”.\n\nInstall it with:  sudo apt install %2")
                                 .arg(basename(path), missing));
        return false;
    }
    return true;
}

// check the archive (on a thread) and call then(encrypted)
static void probe_then(MainWindow *win, const QString &path, std::function<void(bool)> then)
{
    if (!check_tool(win, path))
        return;
    QPointer<MainWindow> w(win);
    fileops::run_task(
        win, "Opening " + basename(path),
        [path]() {
            archive::ProbeResult r = archive::probe(path);
            return QVariant(QVariantMap{{"encrypted", r.encrypted}, {"error", r.error}});
        },
        [w, then](const QVariant &res) {
            QVariantMap info = res.toMap();
            if (!res.isValid())
                return;
            if (!info["error"].toString().isEmpty())
                QMessageBox::warning(w, "Extract", info["error"].toString());
            else
                then(info["encrypted"].toBool());
        });
}

// if `out` holds exactly one folder, use that folder in its place (no needless nesting)
static QString collapse(const QString &out, const QString &dest_parent)
{
    QStringList entries = listdir(out);
    if (entries.size() == 1 && isdir(join(out, entries[0])) && !islink(join(out, entries[0]))) {
        QString tmp = unique_path(dest_parent, "." + entries[0] + ".kestrel-tmp", "num");
        util::rename(join(out, entries[0]), tmp);
        util::rmdir(out);   // free the name first, so "x/x" becomes "x" rather than "x (2)"
        QString final_path = unique_path(dest_parent, entries[0], "num");
        util::rename(tmp, final_path);
        return final_path;
    }
    return out;
}

static void run_extract(MainWindow *win, const QString &path, const QVariantMap &opts)
{
    QString name = basename(path);
    auto work = [path, opts](Task *task) -> QVariant {
        QString dest = opts["dest"].toString();
        bool sub = opts["subfolder"].toBool();
        QString out = dest;
        if (sub) {
            out = unique_path(dest, archive::archive_stem(path), "num");
            makedirs(out);
        }
        try {
            archive::extract(task, path, out, opts["password"].toString(), opts["overwrite"].toString());
        } catch (const archive::WrongPassword &) {
            if (sub)
                rmtree(out);
            return QVariantMap{{"wrong_password", true}};
        } catch (...) {
            if (sub)
                rmtree(out);
            throw;
        }
        QString result = sub ? collapse(out, dest) : out;
        if (opts["trash_after"].toBool()) {
            try {
                util::trash(path);
            } catch (const OSError &) {
            }
        }
        return QVariantMap{{"out", result}};
    };
    QPointer<MainWindow> w(win);
    auto done = [w, path, name, opts](const QVariant &r) {
        if (!w)
            return;
        if (!r.isValid()) {   // cancelled
            w->statusBar()->showMessage(QString("Extracting %1 cancelled").arg(name), 4000);
            return;
        }
        QVariantMap res = r.toMap();
        if (res.value("wrong_password").toBool()) {
            QString pw = ask_password(w, path, true);
            if (!pw.isNull()) {
                QVariantMap again = opts;
                again["password"] = pw;
                run_extract(w, path, again);
            }
            return;
        }
        QString out = res["out"].toString();
        w->statusBar()->showMessage(QString("Extracted %1 to %2").arg(name, out), 6000);
        if (opts["open_after"].toBool())
            w->navigate(out);
        else if (w->pane() && w->pane()->dir() == dirname(out))
            w->pane()->select_later(out);
    };
    fileops::run_job(win, "Extracting " + name, work, done);
}

void extract_here(MainWindow *win, const QString &path)
{
    // extract next to the archive, into a new folder (or its single top-level folder); asks for a password when the
    // archive is encrypted
    QPointer<MainWindow> w(win);
    probe_then(win, path, [w, path](bool encrypted) {
        if (!w)
            return;
        QString pw;
        if (encrypted) {
            pw = ask_password(w, path);
            if (pw.isNull())
                return;
        }
        run_extract(w, path,
                    {{"dest", dirname(path)}, {"subfolder", true}, {"overwrite", "rename"}, {"password", pw},
                     {"trash_after", false}, {"open_after", false}});
    });
}

void extract_dialog(MainWindow *win, const QString &path)
{
    QPointer<MainWindow> w(win);
    probe_then(win, path, [w, path](bool encrypted) {
        if (!w)
            return;
        ExtractDialog dlg(w, path, encrypted);
        if (dlg.exec())
            run_extract(w, path, dlg.options());
    });
}

void compress_dialog(MainWindow *win, const QStringList &paths)
{
    CompressDialog dlg(win, paths);
    if (dlg.exec())
        run_compress(win, dlg.spec());
}

void run_compress(MainWindow *win, const archive::Spec &spec_in)
{
    QString name = basename(spec_in.out);
    QStringList paths;
    for (const QString &r : spec_in.rels)
        paths << join(spec_in.base, r);
    auto work = [spec_in, paths](Task *task) -> QVariant {
        task->report(0, 0, "Measuring…");
        qint64 total = 0;
        for (const QString &p : paths) {
            task->check();
            if (isdir(p) && !islink(p)) {
                total += fileops::dir_stats(p, [task]() { return bool(task->cancelled); }).size;
            } else {
                struct stat st;
                if (stat_(p, st))
                    total += st.st_size;
            }
        }
        task->check();
        archive::Spec sp = spec_in;   // an older archive of that name is replaced only once the new one is made
        sp.total = std::max<qint64>(total, 1);
        QString out = archive::compress(task, sp);
        if (spec_in.trash_originals) {
            for (const QString &p : paths) {
                try {
                    util::trash(p);
                } catch (const OSError &) {
                }
            }
        }
        return out;
    };
    QPointer<MainWindow> w(win);
    auto done = [w, name](const QVariant &r) {
        if (!w)
            return;
        if (!r.isValid()) {
            w->statusBar()->showMessage(QString("Compressing %1 cancelled").arg(name), 4000);
            return;
        }
        QString out = r.toString();
        w->statusBar()->showMessage("Created " + basename(out), 6000);
        if (w->pane() && w->pane()->dir() == dirname(out))
            w->pane()->select_later(out);
    };
    fileops::run_job(win, "Compressing " + name, work, done);
}

QString quick_compress_label(const QStringList &paths)
{
    QString fid = settings().value("archive/format", "").toString();
    for (const archive::Format &f : archive::formats()) {
        if (f.id != fid || f.available.isEmpty())
            continue;
        if (f.single && !(paths.size() == 1 && isfile(paths[0])))
            return QString();
        QString base;
        if (paths.size() == 1) {
            base = basename(rstrip(paths[0], '/'));
        } else {
            QStringList parents;
            for (const QString &p : paths)
                parents << dirname(p);
            base = basename(commonpath(parents));
            if (base.isEmpty())
                base = "Archive";
        }
        return QString("Compress to “%1%2”").arg(base, f.ext);
    }
    return QString();
}

void quick_compress(MainWindow *win, const QStringList &paths)
{
    // compress with the last-used format and options, next to the files, without a password
    CompressDialog dlg(win, paths);   // loads the saved options; never shown
    archive::Spec sp = dlg.spec();
    sp.password.clear();
    sp.trash_originals = false;
    if (exists(sp.out)) {
        QString stem = sp.out.left(sp.out.size() - sp.format.ext.size());
        sp.out = unique_path(dirname(stem), basename(stem) + sp.format.ext, "num");
    }
    run_compress(win, sp);
}

}  // namespace archive_ui
