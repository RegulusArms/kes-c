#include "archive_ui.h"

#include "app.h"
#include "fileops.h"
#include "hashcheck.h"
#include "keyring.h"
#include "windows.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMutex>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWaitCondition>

using namespace util;

static const char *ARGV_NOTE = TX_NOOP("%1 only accepts the password on its command line, so other users on this "
                                       "computer could see it in the process list while the job runs.");

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
    auto *show = new QCheckBox(tx("Show"));
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

QDialog *ask_password(QWidget *parent, const QString &path, Ask reason, bool offer_remember,
                      std::function<void(const QString &, bool)> ok, std::function<void()> cancelled)
{
    auto *dlg = new QDialog(parent);
    dlg->setObjectName("archive_password");
    dlg->setWindowTitle(tx("Password Required"));
    auto *lay = new QVBoxLayout(dlg);
    QString n = basename(path).toHtmlEscaped();
    QString msg = reason == Ask::Wrong     ? tx("<b>Wrong password</b> for “%1”. Try again:").arg(n)
                  : reason == Ask::NotSame ? tx("The password entered before doesn't open “%1”. Enter its password:").arg(n)
                                           : tx("“%1” is encrypted. Enter its password:").arg(n);
    auto *lbl = new QLabel(msg);
    lbl->setTextFormat(Qt::RichText);
    lbl->setWordWrap(true);
    lay->addWidget(lbl);
    auto *edit = new QLineEdit;
    lay->addLayout(password_row(edit));
    QCheckBox *remember = nullptr;
    if (offer_remember) {
        remember = new QCheckBox(tx("Remember this password"));
        remember->setObjectName("remember");
        lay->addWidget(remember);
        lay->addWidget(small(tx("Saved in your login keyring (Passwords and Keys), where any app you run can read it.")));
    }
    QString t = archive::extract_tool(path);
    if (argv_tool(t))
        lay->addWidget(small(tx(ARGV_NOTE).arg(t)));
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(tx("Extract"));
    QObject::connect(bb, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    QObject::connect(dlg, &QDialog::finished, dlg, [edit, remember, ok, cancelled](int r) {
        if (r == QDialog::Accepted && !edit->text().isEmpty())
            ok(edit->text(), remember && remember->isChecked());
        else if (cancelled)
            cancelled();
    });
    lay->addWidget(bb);
    dlg->resize(420, dlg->sizeHint().height());
    edit->setFocus();
    show_free(dlg);
    return dlg;
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
    setWindowTitle(tx("Compress"));
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
        def = tx("Archive");
    name = new QLineEdit(def);
    ext = new QLabel;
    auto *row = new QHBoxLayout;
    row->addWidget(name, 1);
    row->addWidget(ext);
    form->addRow(tx("Archive name:"), row);
    dest = new QLineEdit(base);
    auto *browse_btn = new QPushButton(tx("Browse…"));
    connect(browse_btn, &QPushButton::clicked, this, &CompressDialog::browse);
    row = new QHBoxLayout;
    row->addWidget(dest, 1);
    row->addWidget(browse_btn);
    form->addRow(tx("Location:"), row);

    fmt = new QComboBox;
    auto *fmt_model = qobject_cast<QStandardItemModel *>(fmt->model());
    for (const archive::Format &f : fmts) {
        bool usable = !f.available.isEmpty() && (!f.single || one_file);
        QString text = !f.available.isEmpty() ? tx(f.label) : tx("%1 — install %2").arg(tx(f.label), archive::install_hint(f.tools[0]));
        fmt->addItem(text, f.id);
        if (!usable)
            fmt_model->item(fmt->count() - 1)->setEnabled(false);
    }
    form->addRow(tx("Format:"), fmt);
    tool = new QComboBox;
    auto *tool_lbl = new QLabel(tx("Program:"));
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
    auto *level_title = new QLabel(tx("Compression level:"));
    form->addRow(level_title, level_box);
    level_row = {level_title, level_box};
    level_hint = small(tx(""));
    form->addRow(level_hint);
    method = new QComboBox;
    auto *method_lbl = new QLabel(tx("Method:"));
    form->addRow(method_lbl, method);
    method_row = {method_lbl, method};
    threads = new QSpinBox;
    threads->setRange(0, archive::cpu_count() * 2);
    threads->setSpecialValueText(tx("Auto (%1 cores)").arg(archive::cpu_count()));
    auto *threads_lbl = new QLabel(tx("CPU threads:"));
    form->addRow(threads_lbl, threads);
    threads_row = {threads_lbl, threads};

    // encryption
    enc = new QGroupBox(tx("Encrypt with a password"));
    enc->setCheckable(true);
    enc->setChecked(false);
    auto *ef = new QFormLayout(enc);
    pw = new QLineEdit;
    pw2 = new QLineEdit;
    QCheckBox *show = nullptr;
    ef->addRow(tx("Password:"), password_row(pw, &show));
    pw2->setEchoMode(QLineEdit::Password);
    connect(show, &QCheckBox::toggled, this,
            [this](bool on) { pw2->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
    ef->addRow(tx("Confirm:"), pw2);
    enc_names = new QCheckBox(tx("Also encrypt file names (nothing can be listed without the password)"));
    ef->addRow(enc_names);
    zip_enc = new QComboBox;
    zip_enc->addItem(tx("AES-256 (secure; needs 7-Zip, WinRAR or a recent unzip)"), "AES256");
    zip_enc->addItem(tx("ZipCrypto (weak, but opens everywhere)"), "ZipCrypto");
    zip_enc_lbl = new QLabel(tx("Zip encryption:"));
    ef->addRow(zip_enc_lbl, zip_enc);
    enc_note = small(tx(""));
    ef->addRow(enc_note);
    lay->addWidget(enc);
    no_enc = small(tx(""));
    lay->addWidget(no_enc);

    // checksums of the archive, made from its bytes as they're written (tar formats)
    sums = new QGroupBox(tx("Create a checksum file"));
    sums->setCheckable(true);
    auto *sl_grid = new QGridLayout(sums);
    QStringList saved_algos = settings().value("checksum_algorithms", "sha256").toString().split(',', Qt::SkipEmptyParts);
    int ai = 0;
    for (const QString &a : hashcheck::create_algorithms()) {
        auto *b = new QCheckBox(a == "crc32" ? QString("CRC32 (SFV)") : hashcheck::algo_label(a));
        b->setChecked(saved_algos.contains(a));
        sum_boxes[a] = b;
        sl_grid->addWidget(b, ai / 3, ai % 3);
        ai++;
    }
    sum_one = new QCheckBox(tx("One file with every algorithm (BSD tags)"));
    sum_one->setChecked(settings().value("checksum_one_file", false).toBool());
    sl_grid->addWidget(sum_one, (ai + 2) / 3, 0, 1, 3);
    sl_grid->addWidget(small(tx("Made from the archive as it's written, so it's ready when the archive is, without "
                             "reading it again.")),
                       (ai + 2) / 3 + 1, 0, 1, 3);
    lay->addWidget(sums);

    // splitting and archive structure
    split = new QGroupBox(tx("Split into volumes"));
    split->setCheckable(true);
    split->setChecked(false);
    auto *sl = new QHBoxLayout(split);
    vol_size = new QComboBox;
    for (const archive::VolumeSize &v : archive::volume_sizes()) {
        vol_size->addItem(tx(v.label), v.id);
        vol_size->setItemData(vol_size->count() - 1, txn("%Ln byte(s)", v.bytes), Qt::ToolTipRole);
    }
    vol_size->addItem(tx("Custom…"), "custom");
    vol = new QSpinBox;
    vol->setRange(1, 1000000);
    vol->setValue(4000);
    vol_unit = new QComboBox;
    for (const char *u : {TX_NOOP("KB"), TX_NOOP("MB"), TX_NOOP("GB")})   // binary: 1 MB = 1,048,576 bytes, as 7-Zip and rar count them
        vol_unit->addItem(tx(u), u);   // (the unit kept as its data)
    sl->addWidget(new QLabel(tx("Volume size:")));
    sl->addWidget(vol_size);
    sl->addWidget(vol);
    sl->addWidget(vol_unit);
    sl->addStretch(1);
    lay->addWidget(split);
    auto *opts = new QFormLayout;
    lay->addLayout(opts);
    solid = new QCheckBox(tx("Solid archive (smaller, but slower to extract single files)"));
    opts->addRow(solid);
    recovery = new QSpinBox;
    recovery->setRange(0, 10);
    recovery->setSuffix(" %");
    recovery->setSpecialValueText(tx("None"));
    auto *recovery_lbl = new QLabel(tx("Recovery record:"));
    opts->addRow(recovery_lbl, recovery);
    recovery_row = {recovery_lbl, recovery};
    extra = new QLineEdit;
    extra->setPlaceholderText(tx("Extra options passed to the program, e.g. -mfb=273 or --long=27"));
    opts->addRow(tx("Extra options:"), extra);
    trash_after = new QCheckBox(tx("Move the original files to the trash afterwards"));
    opts->addRow(trash_after);

    lay->addWidget(new QLabel(tx("Command:")));
    preview = new QPlainTextEdit;
    preview->setReadOnly(true);
    preview->setMaximumHeight(64);
    preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    lay->addWidget(preview);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(tx("Compress"));
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
    for (QGroupBox *w : {sums})
        connect(w, &QGroupBox::toggled, this, &CompressDialog::update_preview);
    for (QCheckBox *w : QList<QCheckBox *>{enc_names, solid, sum_one} + sum_boxes.values())
        connect(w, &QCheckBox::toggled, this, &CompressDialog::update_preview);
    for (QSpinBox *w : {vol, threads, recovery})
        connect(w, &QSpinBox::valueChanged, this, &CompressDialog::update_preview);
    connect(vol_size, &QComboBox::currentIndexChanged, this, &CompressDialog::volume_choice_changed);
    for (QComboBox *w : {vol_size, vol_unit, method, zip_enc})
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

qint64 CompressDialog::volume_bytes() const
{
    QString id = vol_size->currentData().toString();
    for (const archive::VolumeSize &v : archive::volume_sizes())
        if (v.id == id)
            return v.bytes;
    QString u = vol_unit->currentData().toString();
    return qint64(vol->value()) * (u == "GB" ? 1024 * 1024 * 1024LL : u == "MB" ? 1024 * 1024 : 1024);
}

void CompressDialog::volume_choice_changed()
{
    bool custom = vol_size->currentData() == "custom";
    vol->setVisible(custom);
    vol_unit->setVisible(custom);
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
    bool no_gpg = archive::is_tar(f) && archive::tool("gpg").isEmpty();   // the tar formats' password needs gpg
    enc->setEnabled(!no_gpg);
    if (no_gpg)
        enc->setChecked(false);
    no_enc->setVisible(!f.password || no_gpg);
    no_enc->setText(no_gpg ? tx("A password for a tar archive needs gpg. Install it with:  sudo apt install gnupg")
                           : tx("%1 can't be encrypted. Use 7z, zip, rar, zpaq or tar for a password-protected archive.")
                                 .arg(f.label.section(' ', 0, 0)));
    split->setVisible(f.volumes);
    solid->setVisible(f.solid);
    sums->setVisible(archive::is_tar(f));
    sums->setChecked(s.value(key("checksum"), false).toBool());
    show_row(recovery_row, f.recovery);
    enc_names->setVisible(f.encrypt_names);
    // restore this format's last options
    solid->setChecked(s.value(key("solid"), true).toBool());
    enc_names->setChecked(s.value(key("encrypt_names"), true).toBool());
    recovery->setValue(s.value(key("recovery"), 0).toInt());
    extra->setText(s.value(key("extra"), "").toString());
    split->setChecked(s.value(key("split"), false).toBool());
    vol->setValue(s.value(key("volume"), 4000).toInt());
    vol_unit->setCurrentIndex(std::max(0, vol_unit->findData(s.value(key("volume_unit"), "MB").toString())));
    // a standard size, or Custom… (also what a size saved before there were standard ones is)
    QString choice = s.value(key("volume_choice"), s.contains(key("volume")) ? "custom" : "fat32").toString();
    vol_size->setCurrentIndex(std::max(0, vol_size->findData(choice)));
    volume_choice_changed();
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
    if (archive::is_tar(f))
        enc_note->setText(tx("The archive is encrypted whole with gpg (AES-256) and saved as “%1.gpg”: nothing in it can "
                             "be listed without the password. The password is passed to gpg privately (not on its "
                             "command line).")
                              .arg(f.ext.mid(1)));
    else
        enc_note->setText(t == "zpaq" ? tx(ARGV_NOTE).arg(t)
                                      : tx("The password is passed to %1 privately (not on its command line).").arg(names.value(t, t)));
    level_changed();
}

void CompressDialog::level_changed()
{
    const archive::Format &f = format();
    QString t = tool->currentData().toString();
    int v = level->value();
    if (t == "7z" || f.id == "rar") {
        static const QMap<int, QString> rar_names = {{0, TX_NOOP("Store")}, {1, TX_NOOP("Fastest")}, {2, TX_NOOP("Fast")},
                                                     {3, TX_NOOP("Normal")}, {4, TX_NOOP("Good")},   {5, TX_NOOP("Best")}};
        const QMap<int, QString> &names = t == "7z" ? archive::LEVEL_NAMES_7Z : rar_names;
        QString n = names.value(v);
        level_lbl->setText(n.isEmpty() ? QString::number(v) : QString("%1 — %2").arg(QString::number(v), tx(n)));
    } else if (f.id == "zpaq") {
        static const QStringList zn = {"", TX_NOOP("Fast"), TX_NOOP("Normal"), TX_NOOP("Good"), TX_NOOP("Better"), TX_NOOP("Best")};
        level_lbl->setText(QString("%1 — %2").arg(QString::number(v), tx(zn.value(v))));
    } else if (t == "zstd" && v > 19) {
        level_lbl->setText(tx("%1 (ultra)").arg(v));
    } else {
        level_lbl->setText(QString::number(v));
    }
    QString hint;
    if (f.id == "zpaq") {
        hint = tx("zpaq doesn't store symbolic links; any in the selection are skipped.");
        if (v >= 4)
            hint = tx("Levels 4–5 are very slow (several minutes per few hundred MB, even on many cores) and use about "
                      "500 MB of RAM per thread. Progress is shown while files are read; after that the status bar shows "
                      "the elapsed time until zpaq finishes. %1")
                       .arg(hint);
    }
    level_hint->setText(hint);
    level_hint->setVisible(!hint.isEmpty());
    update_preview();
}

void CompressDialog::browse()
{
    choose_folder(this, tx("Save Archive In"), dest->text().isEmpty() ? base : dest->text(),
                  [this](const QString &d) { dest->setText(d); });
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
    QString n = name->text().trimmed();
    QString pass = f.password && enc->isEnabled() && enc->isChecked() ? pw->text() : QString();
    QString full_ext = f.ext + (archive::is_tar(f) && !pass.isEmpty() ? ".gpg" : "");
    if (n.endsWith(f.ext + ".gpg") && archive::is_tar(f))
        n.chop(f.ext.size() + 4);
    else if (n.endsWith(f.ext))
        n.chop(f.ext.size());
    QString d = dest->text().trimmed();
    archive::Spec sp;
    sp.format = f;
    sp.tool = t;
    sp.base = base;
    sp.rels = rels;
    sp.out = join(expanduser(d.isEmpty() ? base : d), n + full_ext);
    if (lv)
        sp.level = level->value();
    sp.method = method->currentText();
    sp.threads = archive::tool_threads(f, t) ? threads->value() : 0;
    sp.password = pass;
    sp.encrypt_names = f.encrypt_names ? enc_names->isChecked() : false;
    sp.zip_encryption = zip_enc->currentData().toString();
    sp.volume_bytes = split->isVisible() && split->isChecked() ? volume_bytes() : 0;
    if (f.solid)
        sp.solid = solid->isChecked();
    sp.recovery = f.recovery ? recovery->value() : 0;
    sp.extra = ex;
    sp.trash_originals = trash_after->isChecked();
    if (archive::is_tar(f) && sums->isChecked()) {
        for (const QString &a : hashcheck::create_algorithms())
            if (sum_boxes[a]->isChecked())
                sp.checksums << a;
        sp.checksum_one_file = sum_one->isChecked();
    }
    return sp;
}

void CompressDialog::update_preview()
{
    const archive::Format &f = format();   // a tar archive with a password is name.tar….gpg
    ext->setText(f.ext + (archive::is_tar(f) && enc->isEnabled() && enc->isChecked() ? ".gpg" : ""));
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
        warn(this, tx("Compress"), tx("Enter a name for the archive."));
        return;
    }
    if (!isdir(dirname(sp.out))) {
        warn(this, tx("Compress"), tx("“%1” is not a folder.").arg(dirname(sp.out)));
        return;
    }
    if (!sp.password.isEmpty() && pw->text() != pw2->text()) {
        warn(this, tx("Compress"), tx("The passwords don't match."));
        return;
    }
    if (enc->isVisible() && enc->isEnabled() && enc->isChecked() && sp.password.isEmpty()) {
        warn(this, tx("Compress"), tx("Enter a password, or turn off encryption."));
        return;
    }
    if (sums->isVisible() && sums->isChecked() && sp.checksums.isEmpty()) {
        warn(this, tx("Compress"), tx("Choose an algorithm for the checksum file, or turn it off."));
        return;
    }
    if (!extra->text().trimmed().isEmpty()) {
        bool parsed = false;
        QString err;
        shlex_split(extra->text(), &parsed, &err);
        if (!parsed) {
            warn(this, tx("Compress"), tx("Extra options: %1").arg(err));
            return;
        }
    }
    QStringList taken;   // the archive, and its checksum files
    for (const QString &p : QStringList{sp.out} + archive::checksum_paths(sp))
        if (exists(p))
            taken << "“" + basename(p) + "”";
    if (!taken.isEmpty())
        ask_yes(this, tx("Compress"),
                (taken.size() == 1 ? tx("%1 already exists. Replace it?") : tx("%1 already exist. Replace them?"))
                    .arg(QLocale().createSeparatedList(taken)),
                [this, sp]() { save(sp); });
    else
        save(sp);
}

void CompressDialog::save(const archive::Spec &sp)
{
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
    s.setValue(key("volume_choice"), vol_size->currentData().toString());
    if (vol_size->currentData() == "custom") {   // the last custom size stays when a standard one is chosen
        s.setValue(key("volume"), vol->value());
        s.setValue(key("volume_unit"), vol_unit->currentData().toString());
    }
    if (sums->isVisible()) {
        s.setValue(key("checksum"), sums->isChecked());
        if (sums->isChecked()) {   // the algorithms are Create Checksum File's too
            s.setValue("checksum_algorithms", sp.checksums.join(','));
            s.setValue("checksum_one_file", sp.checksum_one_file);
        }
    }
    accept();
}

// ---------------------------------------------------------------- extract

ExtractDialog::ExtractDialog(QWidget *parent, const QStringList &paths, bool encrypted, bool offer_remember)
    : QDialog(parent), paths(paths), path(paths.value(0)), encrypted(encrypted)
{
    bool many = paths.size() > 1;
    setWindowTitle(many ? tx("Extract All") : tx("Extract"));
    QSettings &s = settings();
    auto *form = new QFormLayout(this);
    QString t = archive::extract_tool(path);
    if (many) {
        QStringList names;
        for (const QString &p : paths)
            names << basename(p);
        auto *lbl = new QLabel(tx("%1 archives").arg(paths.size()));
        lbl->setToolTip(names.join('\n'));
        form->addRow(tx("Archives:"), lbl);
    } else {
        form->addRow(tx("Archive:"), new QLabel(tx("%1   (using %2)").arg(basename(path), t)));
    }
    dest = new QLineEdit(dirname(path));
    auto *browse_btn = new QPushButton(tx("Browse…"));
    connect(browse_btn, &QPushButton::clicked, this, &ExtractDialog::browse);
    auto *row = new QHBoxLayout;
    row->addWidget(dest, 1);
    row->addWidget(browse_btn);
    form->addRow(tx("Extract to:"), row);
    subfolder = new QCheckBox(many ? tx("Each into a new folder named after its archive")
                                   : tx("Into a new folder named “%1”").arg(archive::archive_stem(path)));
    subfolder->setChecked(s.value("archive/x_subfolder", true).toBool());
    form->addRow(subfolder);
    form->addRow(small(many ? tx("If everything in an archive is already inside one folder, that folder is used instead of "
                                 "nesting it.")
                            : tx("If everything in the archive is already inside one folder, that folder is used instead of "
                                 "nesting it.")));
    overwrite = new QComboBox;
    overwrite->addItem(tx("Keep both (rename)"), "rename");
    overwrite->addItem(tx("Replace"), "overwrite");
    overwrite->addItem(tx("Skip"), "skip");
    overwrite->setCurrentIndex(std::max(0, overwrite->findData(s.value("archive/x_overwrite", "rename"))));
    form->addRow(tx("Existing files:"), overwrite);
    connect(subfolder, &QCheckBox::toggled, this, [this](bool on) { overwrite->setEnabled(!on); });
    overwrite->setEnabled(!subfolder->isChecked());
    pw = new QLineEdit;
    if (many) {   // each archive's password is found or asked for as it comes (the first one typed is tried on the rest)
        form->addRow(small(tx("Encrypted archives: the first password you enter is tried on the ones after it, and you're "
                           "asked again only for one it doesn't open.")));
    } else {
        form->addRow(encrypted ? tx("Password:") : tx("Password (if any):"), password_row(pw));
        if (encrypted) {
            form->addRow(small(offer_remember ? tx("This archive is encrypted. Leave the password empty to use one you've "
                                                   "saved, or to be asked.")
                                              : tx("This archive is encrypted.")));
            if (offer_remember) {
                remember = new QCheckBox(tx("Remember this password"));
                remember->setObjectName("remember");
                form->addRow(remember);
                form->addRow(small(tx("Saved in your login keyring (Passwords and Keys), where any app you run can read "
                                   "it.")));
            }
        }
        if (argv_tool(t))
            form->addRow(small(QString(ARGV_NOTE).arg(t)));
    }
    if (std::any_of(paths.begin(), paths.end(), [](const QString &p) { return archive::can_keep_damaged(p); })) {
        // not remembered: keeping broken files is for the one damaged archive
        keep_damaged = new QCheckBox(tx("Keep damaged files"));
        form->addRow(keep_damaged);
        form->addRow(small(tx("Files that fail their checksum are kept as far as they could be extracted, instead of the "
                           "extraction failing. They're listed afterwards: expect them to be broken in places.")));
    }
    trash_after = new QCheckBox(many ? tx("Move the archives to the trash afterwards")
                                     : tx("Move the archive to the trash afterwards"));
    trash_after->setChecked(s.value("archive/x_trash", false).toBool());
    form->addRow(trash_after);
    open_after = new QCheckBox(many ? tx("Open the folder they're extracted to") : tx("Open the extracted folder"));
    open_after->setChecked(s.value("archive/x_open", false).toBool());
    form->addRow(open_after);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(many ? tx("Extract All") : tx("Extract"));
    connect(bb, &QDialogButtonBox::accepted, this, &ExtractDialog::ok);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(bb);
    (encrypted && !many ? pw : dest)->setFocus();
    resize(560, sizeHint().height());
}

void ExtractDialog::browse()
{
    choose_folder(this, tx("Extract To"), dest->text(), [this](const QString &d) { dest->setText(d); });
}

void ExtractDialog::ok()
{
    QString d = expanduser(dest->text().trimmed());
    if (!isdir(d)) {
        ask_yes(this, tx("Extract"), tx("“%1” doesn't exist. Create it?").arg(d), [this, d]() {
            try {
                makedirs(d);
            } catch (const OSError &e) {
                warn(this, tx("Extract"), e.message());
                return;
            }
            save();
        });
        return;
    }
    save();
}

void ExtractDialog::save()
{
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
            {"trash_after", trash_after->isChecked()},     {"open_after", open_after->isChecked()},
            {"keep_damaged", keep_damaged && keep_damaged->isChecked()}, {"remember", remember && remember->isChecked()}};
}

// ---------------------------------------------------------------- flows (called from the main window)

namespace archive_ui {

static bool check_tool(MainWindow *win, const QString &path)
{
    QString missing = archive::missing_extract_tool(path);
    if (!missing.isEmpty()) {
        warn(win, tx("Extract"),
             tx("Kestrel needs another program to open “%1”.\n\nInstall it with:  sudo apt install %2")
                 .arg(basename(path), missing));
        return false;
    }
    return true;
}

// check the archive (on a thread) and call then(encrypted, keyring): keyring, an encrypted one's password can be saved
static void probe_then(MainWindow *win, const QString &path, std::function<void(bool, bool)> then)
{
    if (!check_tool(win, path))
        return;
    QPointer<MainWindow> w(win);
    fileops::run_task(
        win, tx("Opening %1").arg(basename(path)),
        [path]() {
            archive::ProbeResult r = archive::probe(path);
            return QVariant(QVariantMap{{"encrypted", r.encrypted}, {"error", r.error},
                                        {"keyring", r.encrypted && keyring::available()}});
        },
        [w, then](const QVariant &res) {
            QVariantMap info = res.toMap();
            if (!res.isValid())
                return;
            if (!info["error"].toString().isEmpty())
                warn(w, tx("Extract"), info["error"].toString());
            else
                then(info["encrypted"].toBool(), info["keyring"].toBool());
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

// Tell the user which of the archive's symlinks were left out for leading outside the folder (an absolute target, or
// one climbing out with ".."), and warn about any that couldn't be removed. dropped: [path, target, why] each.
void say_dropped_links(QWidget *win, const QString &name, const QVariantList &dropped)
{
    QStringList removed, stuck;
    for (const QVariant &v : dropped) {
        QStringList l = v.toStringList();
        if (l.value(2).isEmpty())
            removed << l.value(0) + " → " + l.value(1);
        else
            stuck << l.value(0) + " → " + l.value(1) + " (" + l.value(2) + ")";
    }
    QString text;
    if (!removed.isEmpty())
        text += txn("Left out %Ln symbolic link(s) that led outside the folder (an absolute target, or one climbing "
                    "out with “..”):\n\n%1",
                    removed.size())
                    .arg(removed.mid(0, 20).join('\n')) +
                "\n\n";
    if (!stuck.isEmpty())
        text += tx("These symbolic links lead outside the folder and couldn't be removed. Opening, copying or deleting "
                   "through them reaches what they point to:\n\n%1")
                    .arg(stuck.mid(0, 20).join('\n'));
    if (stuck.isEmpty())
        inform(win, tx("Extracting %1").arg(name), text.trimmed());
    else
        warn(win, tx("Extracting %1").arg(name), text.trimmed());
}

// Tell the user which files were kept although they're damaged. damaged: their names in the archive.
static void say_damaged(QWidget *win, const QString &name, const QStringList &damaged, const QString &details,
                        bool with_password)
{
    auto *box = new QMessageBox(QMessageBox::Warning, tx("Extracting %1").arg(name), QString(), QMessageBox::Ok, win);
    QString text = txn("%Ln file(s) failed their checksum and were kept as far as they could be extracted. Expect them to "
                       "be broken in places:\n\n%1",
                       damaged.size())
                       .arg(damaged.mid(0, 20).join('\n'));
    if (damaged.size() > 20)
        text += "\n" + txn("… and %Ln more", damaged.size() - 20);
    if (with_password)
        text += "\n\n" + tx("In an older (RAR4) archive a wrong password looks like damage too.");
    box->setText(text);
    box->setDetailedText(details);
    show_free(box);
}

// ---- Extract All: one job for one or several archives

// A password asked for from the job's thread: the window opens in the UI thread while the job waits (it can still be
// cancelled; its window then closes).
struct Asking {
    QMutex m;
    QWaitCondition answered;
    bool done = false;
    QString password;
    bool remember = false;
    QPointer<QDialog> dlg;
};

// (password, remember), or an empty password when the window was cancelled or closed. Throws Cancelled.
static QPair<QString, bool> ask_from_job(Task *task, QPointer<QWidget> parent, const QString &path, Ask reason,
                                         bool offer_remember)
{
    auto a = std::make_shared<Asking>();
    task->report(0, 0, tx("Waiting for the password for %1").arg(basename(path)), true);
    auto finish = [a](const QString &pw, bool remember) {
        QMutexLocker l(&a->m);
        if (a->done)
            return;
        a->done = true;
        a->password = pw;
        a->remember = remember;
        a->answered.wakeAll();
    };
    QMetaObject::invokeMethod(
        qApp,
        [a, parent, path, reason, offer_remember, finish]() {
            QDialog *d = ask_password(parent, path, reason, offer_remember, finish, [finish]() { finish(QString(), false); });
            QObject::connect(d, &QObject::destroyed, [finish]() { finish(QString(), false); });   // with its window
            QMutexLocker l(&a->m);
            a->dlg = d;
        },
        Qt::QueuedConnection);
    QMutexLocker l(&a->m);
    while (!a->done) {
        a->answered.wait(&a->m, 200);
        if (task->cancelled && !a->done) {
            l.unlock();
            QMetaObject::invokeMethod(qApp, [a]() {
                QDialog *d;
                {
                    QMutexLocker l2(&a->m);
                    d = a->dlg;
                }
                if (d)
                    d->reject();
            }, Qt::QueuedConnection);
            task->check();   // throws
            l.relock();
        }
    }
    return {a->password, a->remember};
}

// What the job knows about passwords: the last one typed (tried first on every encrypted archive after it) and the
// keyring's, read once
struct Passwords {
    QString typed, typed_for;   // and the archive it was typed for
    std::optional<bool> keyring;   // a Secret Service answers (asked once, when the first encrypted archive comes)
    std::optional<QStringList> saved;
    bool has_keyring()
    {
        if (!keyring)
            keyring = keyring::available();
        return *keyring;
    }
    const QStringList &all_saved()
    {
        if (!saved)
            saved = has_keyring() ? keyring::all() : QStringList();
        return *saved;
    }
};

// The password that opens `path`: the one typed last, the one saved for it, the other saved ones (each a quick test),
// then asking until one opens it. Empty when asking was cancelled. `tried`: ones known not to open it (skipped).
// `remember`: ticked in the dialog for the password given in `pw.typed` (stored once it opens the archive).
static QString find_password(Task *task, QPointer<QWidget> parent, const QString &path, Passwords &pw,
                             QStringList &tried, bool &remember)
{
    QString fp;
    auto opens = [&](const QString &c) {
        if (c.isEmpty() || tried.contains(c))
            return false;
        task->check();
        if (archive::password_opens(task, path, c))
            return true;
        tried << c;
        return false;
    };
    auto keep = [&](const QString &c, bool ticked) {
        if (ticked && pw.has_keyring()) {
            if (fp.isEmpty())
                fp = keyring::fingerprint(path);
            keyring::store(fp, basename(path), c);
            if (pw.saved && !pw.saved->contains(c))
                pw.saved->append(c);
        }
    };
    bool typed_tried = !pw.typed.isEmpty() && !tried.contains(pw.typed);
    if (opens(pw.typed)) {
        keep(pw.typed, remember);
        remember = false;
        return pw.typed;
    }
    remember = false;
    if (pw.has_keyring()) {
        fp = keyring::fingerprint(path);
        if (QString c = keyring::lookup(fp); opens(c))
            return c;
        for (const QString &c : pw.all_saved())
            if (opens(c))
                return c;
    }
    Ask reason = typed_tried && pw.typed_for != path ? Ask::NotSame : tried.isEmpty() ? Ask::First : Ask::Wrong;
    for (;;) {
        auto [c, ticked] = ask_from_job(task, parent, path, reason, pw.has_keyring());
        if (c.isEmpty())
            return QString();
        pw.typed = c;   // the one tried on the rest
        pw.typed_for = path;
        if (opens(c)) {
            keep(c, ticked);
            return c;
        }
        reason = Ask::Wrong;
    }
}

// One archive, as Extract Here / Extract To… does it: into a new folder (or its single top-level folder), or into
// `dest` itself. Returns its outcome: status ok, password (asking was cancelled), missing (a tool), error.
static QVariantMap extract_one(Task *task, QPointer<QWidget> parent, const QString &path, const QVariantMap &opts,
                               Passwords &pw)
{
    QVariantMap res{{"path", path}, {"name", basename(path)}};
    auto failed = [&res](const QString &status, const QString &message = QString()) {
        res["status"] = status;
        res["message"] = message;
        return res;
    };
    QString missing = archive::missing_extract_tool(path);
    if (!missing.isEmpty())
        return failed("missing", missing);
    task->report(0, 0, tx("Opening %1").arg(basename(path)));
    archive::ProbeResult probe = archive::probe(path);
    if (!probe.error.isEmpty())
        return failed("error", probe.error);
    QStringList tried;
    bool remember = opts["remember"].toBool();
    QString password;
    if (probe.encrypted) {
        password = find_password(task, parent, path, pw, tried, remember);
        if (password.isEmpty())
            return failed("password");
    }
    QString dest = opts["dest"].toString().isEmpty() ? dirname(path) : opts["dest"].toString();
    bool sub = opts["subfolder"].toBool();
    for (;;) {
        task->check();
        QString out = dest;
        if (sub) {
            out = unique_path(dest, archive::archive_stem(path), "num");
            makedirs(out);
        }
        QList<archive::DroppedLink> dropped;
        archive::Damaged damaged;
        try {
            archive::extract(task, path, out, password, opts["overwrite"].toString(), 0, &dropped,
                             opts["keep_damaged"].toBool() ? &damaged : nullptr);
        } catch (const archive::WrongPassword &) {
            // the quick test can pass where extracting fails (or the archive didn't look encrypted): find another
            if (sub)
                rmtree(out);
            if (!password.isEmpty())
                tried << password;
            password = find_password(task, parent, path, pw, tried, remember);
            if (password.isEmpty())
                return failed("password");
            continue;
        } catch (const Cancelled &) {
            if (sub)
                rmtree(out);
            throw;
        } catch (const std::exception &e) {
            if (sub)
                rmtree(out);
            return failed("error", QString::fromStdString(e.what()));
        }
        QString result = sub ? collapse(out, dest) : out;
        if (opts["trash_after"].toBool()) {
            try {
                util::trash(path);
            } catch (const OSError &) {
            }
        }
        QVariantList links;
        for (const archive::DroppedLink &l : dropped)
            links << QVariant(QStringList{l.path, l.target, l.why});
        res.insert({{"status", "ok"}, {"out", result}, {"dropped", links}, {"damaged", damaged.files},
                    {"damaged_details", damaged.details}, {"with_password", !password.isEmpty()}});
        return res;
    }
}

QStringList archives_of(const QStringList &paths)
{
    QStringList out;
    for (const QString &p : paths) {
        if (!archive::can_extract(p) || isdir(p))
            continue;
        QString first = archive::first_volume(p);
        if (!out.contains(first))
            out << first;
    }
    return out;
}

// A single archive's outcome, as Extract Here / Extract To… has always told it
static void say_one(MainWindow *w, const QVariantMap &res, const QVariantMap &opts)
{
    QString name = res["name"].toString(), status = res["status"].toString();
    if (status == "password") {
        w->statusBar()->showMessage(tx("Extracting %1 cancelled").arg(name), 4000);
        return;
    }
    if (status == "missing") {
        warn(w, tx("Extract"), tx("Kestrel needs another program to open “%1”.\n\nInstall it with:  sudo apt install %2")
                                   .arg(name, res["message"].toString()));
        return;
    }
    if (status == "error") {
        warn(w, tx("Extracting %1").arg(name), res["message"].toString());
        return;
    }
    QString out = res["out"].toString();
    QStringList damaged = res["damaged"].toStringList();
    w->statusBar()->showMessage(damaged.isEmpty() ? tx("Extracted %1 to %2").arg(name, out)
                                                  : txn("Extracted %1 to %2, keeping %Ln damaged file(s)", damaged.size()).arg(name, out),
                                6000);
    if (!damaged.isEmpty())
        say_damaged(w, name, damaged, res["damaged_details"].toString(), res["with_password"].toBool());
    if (!res["dropped"].toList().isEmpty())
        say_dropped_links(w, name, res["dropped"].toList());
    if (opts["open_after"].toBool())
        w->navigate(out);
    else if (w->pane() && w->pane()->dir() == dirname(out))
        w->pane()->select_later(out);
}

// Several archives' outcomes: one summary of what was extracted and what wasn't
static void say_all(MainWindow *w, const QVariantList &results, const QStringList &archives, bool stopped,
                    const QVariantMap &opts)
{
    QStringList lines, details;
    int ok = 0;
    QString last_out;
    for (const QVariant &v : results) {
        QVariantMap r = v.toMap();
        QString name = r["name"].toString(), status = r["status"].toString();
        if (status == "ok") {
            ++ok;
            last_out = r["out"].toString();
            QString line = "✓ " + name + " → " + basename(last_out);   // notr: a name and where it went
            QStringList damaged = r["damaged"].toStringList();
            if (!damaged.isEmpty()) {
                line = txn("%1 (kept %Ln damaged file(s))", damaged.size()).arg(line);
                details << tx("%1: damaged files kept:\n  %2").arg(name, damaged.mid(0, 50).join("\n  ")) + "\n" +
                               r["damaged_details"].toString();
            }
            QVariantList dropped = r["dropped"].toList();
            if (!dropped.isEmpty()) {
                line = txn("%1 (left out %Ln link(s) leading outside it)", dropped.size()).arg(line);
                QStringList ls;
                for (const QVariant &d : dropped) {
                    QStringList l = d.toStringList();
                    QString link = l.value(0) + " → " + l.value(1);
                    ls << (l.value(2).isEmpty() ? link : tx("%1 (couldn't be removed: %2)").arg(link, l.value(2)));
                }
                details << tx("%1: symbolic links left out:\n  %2").arg(name, ls.join("\n  "));
            }
            lines << line;
        } else if (status == "password") {
            lines << tx("✗ %1 — no password (asking was cancelled)").arg(name);
        } else if (status == "missing") {
            lines << tx("✗ %1 — needs another program: sudo apt install %2").arg(name, r["message"].toString());
        } else {
            QString msg = r["message"].toString();
            lines << "✗ " + name + " — " + msg.section('\n', 0, 0);
            details << name + ":\n" + msg;
        }
    }
    for (int i = results.size(); i < archives.size(); ++i)
        lines << tx("– %1 — not extracted (stopped)").arg(basename(archives[i]));
    QString head = (stopped ? txn("Extracted %1 of %Ln archive(s) (stopped).", archives.size())
                            : txn("Extracted %1 of %Ln archive(s).", archives.size()))
                       .arg(QString::number(ok));
    w->statusBar()->showMessage(head, 6000);
    auto *box = new QMessageBox(ok == archives.size() && details.isEmpty() ? QMessageBox::Information : QMessageBox::Warning,
                                tx("Extract All"), head + "\n\n" + lines.join('\n'), QMessageBox::Ok, w);
    box->setObjectName("extract_all_summary");
    if (!details.isEmpty())
        box->setDetailedText(details.join("\n\n"));
    show_free(box);
    if (ok && opts["open_after"].toBool())
        w->navigate(opts["dest"].toString().isEmpty() ? dirname(last_out) : opts["dest"].toString());
}

Task *extract_all(MainWindow *win, const QStringList &archives, const QVariantMap &opts)
{
    if (archives.isEmpty())
        return nullptr;
    QPointer<QWidget> parent(win);
    auto work = [archives, opts, parent](Task *task) -> QVariant {
        Passwords pw;
        pw.typed = opts["password"].toString();   // typed in Extract To… for its archive
        pw.typed_for = archives[0];
        QVariantList results;
        try {
            for (int i = 0; i < archives.size(); ++i) {
                task->check();
                task->step = i;
                QVariantMap r = extract_one(task, parent, archives[i], opts, pw);
                if (r["status"] == "ok")
                    task->log(r["name"].toString() + " → " + r["out"].toString());
                else {   // (the Jobs window shows a job with these as done with errors)
                    task->log(tx("%1  failed: %2")
                                  .arg(r["name"].toString(),
                                       r["status"] == "password"  ? tx("no password (asking was cancelled)")
                                       : r["status"] == "missing" ? tx("needs %1").arg(r["message"].toString())
                                                                  : r["message"].toString().section('\n', 0, 0)));
                    task->had_errors = true;
                }
                results << r;
            }
        } catch (const Cancelled &) {
            if (archives.size() == 1)
                throw;
            task->was_cancelled = true;   // what was extracted before the ✕ is still told
            return QVariantMap{{"results", results}, {"stopped", true}};
        }
        return QVariantMap{{"results", results}, {"stopped", false}};
    };
    QPointer<MainWindow> w(win);
    QString title = archives.size() == 1 ? tx("Extracting %1").arg(basename(archives[0]))
                                         : txn("Extracting %Ln archive(s)", archives.size());
    auto done = [w, archives, opts, title](const QVariant &r) {
        if (!w)
            return;
        if (!r.isValid()) {   // cancelled
            w->statusBar()->showMessage(tx("%1 cancelled").arg(title), 4000);
            return;
        }
        QVariantMap m = r.toMap();
        QVariantList results = m["results"].toList();
        if (archives.size() == 1 && results.size() == 1)
            say_one(w, results[0].toMap(), opts);
        else
            say_all(w, results, archives, m["stopped"].toBool(), opts);
    };
    QStringList lanes = archives;
    QStringList steps;
    for (const QString &a : archives)
        steps << basename(a);
    lanes << (opts["dest"].toString().isEmpty() ? dirname(archives[0]) : opts["dest"].toString());
    return fileops::queue_job(win, title, lanes, work, done, true, archives.size() > 1 ? steps : QStringList());
}

QString job_password(Task *task, QPointer<QWidget> parent, const QString &path, QStringList &tried)
{
    Passwords pw;
    bool remember = false;
    return find_password(task, parent, path, pw, tried, remember);
}

// ---- Test Archive: one job for one or several archives

// One archive tested: status ok, damaged, password (asking was cancelled), missing (a tool), error
static QVariantMap test_one(Task *task, QPointer<QWidget> parent, const QString &path, Passwords &pw)
{
    QVariantMap res{{"path", path}, {"name", basename(path)}};
    auto failed = [&res](const QString &status, const QString &message = QString()) {
        res["status"] = status;
        res["message"] = message;
        return res;
    };
    QString missing = archive::missing_extract_tool(path);
    if (!missing.isEmpty())
        return failed("missing", missing);
    task->report(0, 0, tx("Opening %1").arg(basename(path)));
    archive::ProbeResult probe = archive::probe(path);
    if (!probe.error.isEmpty())
        return failed("damaged", probe.error);
    QStringList tried;
    bool remember = false;
    QString password;
    if (probe.encrypted) {
        password = find_password(task, parent, path, pw, tried, remember);
        if (password.isEmpty())
            return failed("password");
    }
    for (;;) {
        try {
            archive::TestResult r = archive::test(task, path, password);
            if (r.ok)
                return failed("ok");
            res["damaged"] = r.damaged;
            return failed("damaged", r.details);
        } catch (const archive::WrongPassword &) {
            if (!password.isEmpty())
                tried << password;
            password = find_password(task, parent, path, pw, tried, remember);
            if (password.isEmpty())
                return failed("password");
        } catch (const Cancelled &) {
            throw;
        } catch (const std::exception &e) {
            return failed("error", QString::fromStdString(e.what()));
        }
    }
}

static void say_tests(MainWindow *w, const QVariantList &results, const QStringList &archives, bool stopped)
{
    QStringList lines, details;
    int ok = 0;
    for (const QVariant &v : results) {
        QVariantMap r = v.toMap();
        QString name = r["name"].toString(), status = r["status"].toString(), msg = r["message"].toString();
        QStringList damaged = r["damaged"].toStringList();
        if (status == "ok") {
            ++ok;
            lines << tx("✓ %1 — OK").arg(name);
        } else if (status == "damaged") {
            lines << (damaged.isEmpty() ? tx("✗ %1 — damaged").arg(name)
                                        : txn("✗ %1 — damaged: %Ln file(s) failed their check", damaged.size()).arg(name));
            details << name + ":\n" + (damaged.isEmpty() ? QString() : "  " + damaged.mid(0, 50).join("\n  ") + "\n") + msg;
        } else if (status == "password") {
            lines << tx("✗ %1 — not tested: no password (asking was cancelled)").arg(name);
        } else if (status == "missing") {
            lines << tx("✗ %1 — needs another program: sudo apt install %2").arg(name, msg);
        } else {
            lines << "✗ " + name + " — " + msg.section('\n', 0, 0);
            details << name + ":\n" + msg;
        }
    }
    for (int i = results.size(); i < archives.size(); ++i)
        lines << tx("– %1 — not tested (stopped)").arg(basename(archives[i]));
    QString head;
    if (archives.size() == 1 && results.size() == 1) {
        QVariantMap r = results[0].toMap();
        head = r["status"] == "ok" ? tx("“%1” is OK: everything in it passed its check.").arg(r["name"].toString())
                                   : lines.value(0).mid(2);
        lines.clear();
    } else {
        head = (stopped ? txn("%1 of %Ln archive(s) are OK (stopped).", archives.size())
                        : txn("%1 of %Ln archive(s) are OK.", archives.size()))
                   .arg(QString::number(ok));
    }
    w->statusBar()->showMessage(head, 6000);
    auto *box = new QMessageBox(ok == archives.size() ? QMessageBox::Information : QMessageBox::Warning,
                                archives.size() == 1 ? tx("Test Archive") : tx("Test Archives"),
                                lines.isEmpty() ? head : head + "\n\n" + lines.join('\n'), QMessageBox::Ok, w);
    box->setObjectName("test_summary");
    if (!details.isEmpty())
        box->setDetailedText(details.join("\n\n"));
    show_free(box);
}

Task *test_archives(MainWindow *win, const QStringList &archives)
{
    if (archives.isEmpty())
        return nullptr;
    QPointer<QWidget> parent(win);
    auto work = [archives, parent](Task *task) -> QVariant {
        Passwords pw;
        QVariantList results;
        try {
            for (int i = 0; i < archives.size(); ++i) {
                task->check();
                task->step = i;
                QVariantMap r = test_one(task, parent, archives[i], pw);
                if (r["status"] == "ok")
                    task->log(tx("%1: OK").arg(r["name"].toString()));
                else {
                    task->log(tx("%1  failed: %2")
                                  .arg(r["name"].toString(), r["status"] == "password" ? tx("no password (asking was cancelled)")
                                                                                       : r["message"].toString().section('\n', 0, 0)));
                    task->had_errors = true;
                }
                results << r;
            }
        } catch (const Cancelled &) {
            if (archives.size() == 1)
                throw;
            task->was_cancelled = true;
            return QVariantMap{{"results", results}, {"stopped", true}};
        }
        return QVariantMap{{"results", results}, {"stopped", false}};
    };
    QPointer<MainWindow> w(win);
    QString title = archives.size() == 1 ? tx("Testing %1").arg(basename(archives[0]))
                                         : txn("Testing %Ln archive(s)", archives.size());
    auto done = [w, archives, title](const QVariant &r) {
        if (!w)
            return;
        if (!r.isValid()) {
            w->statusBar()->showMessage(tx("%1 cancelled").arg(title), 4000);
            return;
        }
        QVariantMap m = r.toMap();
        say_tests(w, m["results"].toList(), archives, m["stopped"].toBool());
    };
    QStringList steps;
    for (const QString &a : archives)
        steps << basename(a);
    return fileops::queue_job(win, title, archives, work, done, true, archives.size() > 1 ? steps : QStringList());
}

static QVariantMap here_options()
{
    return {{"dest", QString()}, {"subfolder", true}, {"overwrite", "rename"}, {"trash_after", false}, {"open_after", false}};
}

void extract_here(MainWindow *win, const QString &path)
{
    // next to the archive, into a new folder (or its single top-level folder); its password is found or asked for
    if (check_tool(win, path))
        extract_all(win, {path}, here_options());
}

void extract_all_here(MainWindow *win, const QStringList &archives) { extract_all(win, archives, here_options()); }

void extract_dialog(MainWindow *win, const QString &path)
{
    QPointer<MainWindow> w(win);
    probe_then(win, path, [w, path](bool encrypted, bool keyring) {
        if (!w)
            return;
        auto *dlg = new ExtractDialog(w, {path}, encrypted, keyring);
        QObject::connect(dlg, &QDialog::accepted, w, [w, dlg, path]() { extract_all(w, {path}, dlg->options()); });
        show_free(dlg);
    });
}

void extract_all_dialog(MainWindow *win, const QStringList &archives)
{
    auto *dlg = new ExtractDialog(win, archives, false);
    QPointer<MainWindow> w(win);
    QObject::connect(dlg, &QDialog::accepted, win, [w, dlg, archives]() { extract_all(w, archives, dlg->options()); });
    show_free(dlg);
}

void compress_dialog(MainWindow *win, const QStringList &paths)
{
    auto *dlg = new CompressDialog(win, paths);
    QObject::connect(dlg, &QDialog::accepted, win, [win, dlg]() { run_compress(win, dlg->spec()); });
    show_free(dlg);
}

void run_compress(MainWindow *win, const archive::Spec &spec_in)
{
    QString name = basename(spec_in.out);
    QStringList paths;
    for (const QString &r : spec_in.rels)
        paths << join(spec_in.base, r);
    auto work = [spec_in, paths](Task *task) -> QVariant {
        task->report(0, 0, tx("Measuring…"));
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
    bool with_sums = !archive::checksum_paths(spec_in).isEmpty();
    auto done = [w, name, with_sums](const QVariant &r) {
        if (!w)
            return;
        if (!r.isValid()) {
            w->statusBar()->showMessage(tx("Compressing %1 cancelled").arg(name), 4000);
            return;
        }
        QString out = r.toString();
        w->statusBar()->showMessage(with_sums ? tx("Created %1 and its checksum file").arg(basename(out))
                                              : tx("Created %1").arg(basename(out)),
                                    6000);
        if (w->pane() && w->pane()->dir() == dirname(out))
            w->pane()->select_later(out);
    };
    fileops::queue_job(win, tx("Compressing %1").arg(name), paths + QStringList{dirname(spec_in.out)}, work, done);
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
                base = tx("Archive");
        }
        return tx("Compress to “%1”").arg(base + f.ext);
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
