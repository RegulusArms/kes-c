#include "dialogs.h"

#include "admin.h"
#include "fileops.h"
#include "undo.h"
#include "thumbs.h"
#include "uwp.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStorageInfo>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

using namespace util;

static QLabel *sel_label(const QString &text = QString())
{
    auto *lab = new QLabel(text);
    lab->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lab->setWordWrap(true);
    return lab;
}

static QString fmt_ts(qint64 ts) { return fmt_time(ts, "%a %d %b %Y, %H:%M:%S"); }

// stat.filemode(): "drwxr-xr-x"
static QString filemode(mode_t m)
{
    QString s;
    s += S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : '-';
    const char *rwx = "rwx";
    for (int who = 0; who < 3; ++who) {
        for (int k = 0; k < 3; ++k) {
            mode_t bit = mode_t(0400) >> (who * 3 + k);
            QChar c = (m & bit) ? QChar(rwx[k]) : QChar('-');
            if (k == 2) {
                mode_t special = who == 0 ? S_ISUID : who == 1 ? S_ISGID : S_ISVTX;
                if (m & special)
                    c = who == 2 ? ((m & bit) ? 't' : 'T') : ((m & bit) ? 's' : 'S');
            }
            s += c;
        }
    }
    return s;
}

TextDialog::TextDialog(QWidget *parent, const QString &title, const QString &text) : QDialog(parent)
{
    setWindowTitle(title);
    resize(700, 500);
    auto *lay = new QVBoxLayout(this);
    auto *ed = new QPlainTextEdit(text);
    ed->setReadOnly(true);
    lay->addWidget(ed);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    QPushButton *copy = bb->addButton("Copy", QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, this, [text]() { QGuiApplication::clipboard()->setText(text); });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
}

// ---------------------------------------------------------------- metadata tag editor

MetaTagDialog::MetaTagDialog(QWidget *parent, const QString &key, const QString &value_text, const QString &path,
                             const QHash<QString, QString> &existing_in)
    : QDialog(parent)
{
    adding = key.isEmpty();
    items = metadata::as_list(value_text, &value_is_list);
    for (auto it = existing_in.begin(); it != existing_in.end(); ++it)
        existing.insert(metadata::tag_identity(it.key()), it.value());
    setWindowTitle(adding ? "Add Tag" : "Edit Tag");
    resize(600, 400);
    auto *form = new QFormLayout(this);
    if (adding) {
        key_combo = new QComboBox;
        key_combo->setEditable(true);
        key_combo->setInsertPolicy(QComboBox::NoInsert);
        key_combo->setMaxVisibleItems(20);
        key_combo->lineEdit()->setPlaceholderText("Choose or type a tag, e.g. Artist or XMP-dc:Description");
        fill(metadata::tag_choices(path, metadata::tag_db_if_loaded()));
        if (!metadata::tag_db_if_loaded()) {   // the full list takes a few seconds the first time
            fileops::run_task(
                this, "Loading tags…",
                []() {
                    metadata::tag_db();
                    return QVariant(true);
                },
                [this, path](const QVariant &) { fill(metadata::tag_choices(path, metadata::tag_db_if_loaded())); },
                true);
        }
        connect(key_combo, &QComboBox::currentTextChanged, this, &MetaTagDialog::key_changed);
        form->addRow("Tag:", key_combo);
    } else {
        key_edit = new QLineEdit(key);
        key_edit->setReadOnly(true);
        form->addRow("Tag:", key_edit);
        if (!path.isEmpty() && metadata::tag_db_if_loaded()) {
            for (const auto &section : metadata::tag_choices(path, metadata::tag_db_if_loaded()))
                for (const metadata::TagChoice &c : section.second) {
                    auto id = metadata::tag_identity(c.key);
                    if (!kinds.contains(id))
                        kinds.insert(id, {c.kind, c.values});
                }
        }
    }
    blurb = new QLabel;
    blurb->setWordWrap(true);
    blurb->setTextFormat(Qt::PlainText);
    form->addRow("", blurb);
    value = new QPlainTextEdit(value_is_list ? items.join('\n') : value_text);
    form->addRow("Value:", value);
    note = new QLabel;
    note->setWordWrap(true);
    form->addRow("", note);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &MetaTagDialog::ok);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(bb);
    if (adding) {
        key_combo->setCurrentIndex(-1);
        key_combo->setEditText("");
        key_combo->setFocus();
        blurb->hide();
        note->hide();
    } else {
        blurb->setText(metadata::tag_help(catalog_key(key)));
        blurb->setVisible(!blurb->text().isEmpty());
        update_note(key);
        value->setFocus();
    }
}

static const QStringList COLUMNS = {"Tag", "What it's for", "Accepts"};

void MetaTagDialog::fill(const QList<metadata::Section> &sections)
{
    // (Re)build the tag dropdown as a 3-column list (tag, description, value type) with a heading per section.
    // Typing filters a matching flat list of tags without the headings.
    QString text = key_combo->currentText();
    key_combo->blockSignals(true);
    auto *model = new QStandardItemModel(0, 3, this);
    auto *flat = new QStandardItemModel(0, 3, this);
    model->setHorizontalHeaderLabels(COLUMNS);
    flat->setHorizontalHeaderLabels(COLUMNS);
    QList<int> headings;
    for (const auto &[heading, choices] : sections) {
        auto *h = new QStandardItem(heading);
        h->setEnabled(false);
        QFont f = h->font();
        f.setBold(true);
        h->setFont(f);
        headings << model->rowCount();
        model->appendRow({h, new QStandardItem, new QStandardItem});
        for (const metadata::TagChoice &c : choices) {
            help.insert(c.key, c.blurb);
            auto id = metadata::tag_identity(c.key);
            if (!kinds.contains(id))
                kinds.insert(id, {c.kind, c.values});
            for (QStandardItemModel *m : {model, flat}) {
                QList<QStandardItem *> row = {new QStandardItem(c.key), new QStandardItem(c.blurb),
                                              new QStandardItem(c.kind)};
                for (QStandardItem *it : row) {
                    it->setEditable(false);
                    it->setToolTip(QString("%1\n%2\nAccepts: %3").arg(c.key, c.blurb, c.kind));
                }
                m->appendRow(row);
            }
        }
    }
    QTreeView *view = tag_view();
    key_combo->setView(view);
    key_combo->setModel(model);
    for (int r : headings)
        view->setFirstColumnSpanned(r, QModelIndex(), true);
    size_columns(view);
    auto *comp = new QCompleter(flat, key_combo);
    comp->setCompletionColumn(0);
    comp->setCaseSensitivity(Qt::CaseInsensitive);
    comp->setFilterMode(Qt::MatchContains);
    comp->setMaxVisibleItems(15);
    QTreeView *popup = tag_view();
    comp->setPopup(popup);
    size_columns(popup);
    key_combo->setCompleter(comp);
    key_combo->setCurrentIndex(-1);
    key_combo->setEditText(text);
    key_combo->blockSignals(false);
}

QTreeView *MetaTagDialog::tag_view()
{
    auto *v = new QTreeView;
    v->setRootIsDecorated(false);
    v->setUniformRowHeights(true);
    v->setAlternatingRowColors(true);
    v->setTextElideMode(Qt::ElideRight);
    v->setSelectionBehavior(QAbstractItemView::SelectRows);
    v->setMinimumWidth(860);
    return v;
}

void MetaTagDialog::size_columns(QTreeView *view)
{
    QHeaderView *h = view->header();
    h->setStretchLastSection(false);
    h->resizeSection(0, 260);
    h->setSectionResizeMode(1, QHeaderView::Stretch);
    h->resizeSection(2, 110);
}

QString MetaTagDialog::catalog_key(const QString &key) const
{
    // the catalog entry for `key`, matching e.g. IFD0:Artist or plain "Artist" to EXIF:Artist
    if (!metadata::tag_help(key).isEmpty())
        return key;
    auto ident = metadata::tag_identity(key);
    for (const metadata::CatalogEntry &e : metadata::TAG_CATALOG) {
        auto kid = metadata::tag_identity(e.key);
        if (kid == ident || (!key.contains(':') && kid.second == ident.second))
            return e.key;
    }
    return key;
}

void MetaTagDialog::key_changed(const QString &key_in)
{
    QString key = key_in.trimmed();
    QString b = help.value(key);
    if (b.isEmpty())
        b = metadata::tag_help(catalog_key(key));
    QString group = key.contains(':') ? key.section(':', 0, 0) : QString();
    if (!group.isEmpty() && !metadata::is_editable(group, "", key.section(':', 1, 1))) {
        b = QString("%1 tags describe the file itself and can't be edited.").arg(group);
    } else if (b.isEmpty() && !key.isEmpty()) {
        b = help.contains(key) ? "No description available for this tag."
                               : "Custom tag — written exactly as typed. Prefix a group (e.g. XMP-dc:) to choose where "
                                 "it goes.";
    }
    blurb->setText(b);
    blurb->setVisible(!b.isEmpty());
    if (!key.isEmpty()) {
        auto it = existing.constFind(metadata::tag_identity(key));
        if (it != existing.constEnd() && value->toPlainText().trimmed().isEmpty()) {
            bool is_l = false;
            QStringList l = metadata::as_list(*it, &is_l);
            value->setPlainText(is_l ? l.join('\n') : *it);
        }
    }
    update_note(key);
}

bool MetaTagDialog::is_list(const QString &key) const
{
    return value_is_list || metadata::is_list_tag(catalog_key(key));
}

void MetaTagDialog::update_note(const QString &key)
{
    QStringList notes;
    QPair<QString, QStringList> kv;
    if (!key.isEmpty())
        kv = kinds.value(metadata::tag_identity(key));
    if (!kv.first.isEmpty())
        notes << QString("<b>Accepts:</b> %1.").arg(kv.first);
    if (!kv.second.isEmpty())
        notes << "<b>Allowed values:</b> " + kv.second.join(", ").toHtmlEscaped() + ".";
    if (is_list(key))
        notes << "List tag — one item per line.";
    if (adding && !key.isEmpty() && existing.contains(metadata::tag_identity(key)))
        notes << "This tag is already set; saving replaces its current value.";
    note->setText(notes.isEmpty() ? QString() : "<small>" + notes.join(' ') + "</small>");
    note->setVisible(!notes.isEmpty());
}

QString MetaTagDialog::tag() const { return (adding ? key_combo->currentText() : key_edit->text()).trimmed(); }

void MetaTagDialog::ok()
{
    QString key = tag();
    static const QRegularExpression re("^[\\w-]+(:[\\w-]+)?$", QRegularExpression::UseUnicodePropertiesOption);
    if (key.isEmpty() || !re.match(key).hasMatch()) {
        QMessageBox::warning(this, windowTitle(), "Choose a tag, or type one like “Artist” or “XMP-dc:Description”.");
        return;
    }
    QString group = key.contains(':') ? key.section(':', 0, 0) : QString();
    if (!group.isEmpty() && !metadata::is_editable(group, "", key.section(':', 1, 1))) {
        QMessageBox::warning(this, windowTitle(), QString("%1 tags can't be edited.").arg(group));
        return;
    }
    accept();
}

metadata::Change MetaTagDialog::result_change() const
{
    metadata::Change c;
    c.key = tag();
    QString text = value->toPlainText();
    if (is_list(c.key)) {
        c.is_list = true;
        for (const QString &ln : text.split('\n'))
            if (!ln.trimmed().isEmpty())
                c.values << ln;
    } else {
        c.values << text;
    }
    return c;
}

std::optional<metadata::Change> MetaTagDialog::ask(QWidget *parent, const QString &key, const QString &value,
                                                   const QString &path, const QHash<QString, QString> &existing)
{
    MetaTagDialog d(parent, key, value, path, existing);
    if (d.exec() != QDialog::Accepted)
        return std::nullopt;
    return d.result_change();
}

// ---------------------------------------------------------------- properties

PropertiesDialog::PropertiesDialog(QWidget *parent, const QStringList &paths_)
    : QDialog(parent), paths(paths_), single(paths_.size() == 1), path(paths_.first())
{
    QString name = basename(rstrip(path, '/'));
    if (name.isEmpty())
        name = path;
    setWindowTitle(single ? name + " — Properties" : QString("%1 items — Properties").arg(paths.size()));
    resize(560, 600);
    auto *lay = new QVBoxLayout(this);
    tabs = new QTabWidget;
    lay->addWidget(tabs);
    tabs->addTab(general_tab(), "General");
    if (single) {
        tabs->addTab(perm_tab(), "Permissions");
        if (is_image(path))
            tabs->addTab(image_tab(), "Image");
        if (isfile(path)) {
            meta_tab_widget = meta_tab();
            tabs->addTab(meta_tab_widget, "Metadata");
            tabs->addTab(checksum_tab(), "Checksums");
        }
    }
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &PropertiesDialog::apply);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
}

void PropertiesDialog::run(std::function<QVariant()> fn, std::function<void(const QVariant &)> cb, const QString &title)
{
    // background job; shown in the main window's status bar only when given a title
    fileops::run_task(this, title, std::move(fn), std::move(cb), title.isEmpty());
}

QWidget *PropertiesDialog::general_tab()
{
    auto *w = new QWidget;
    auto *form = new QFormLayout(w);
    auto *icon = new QLabel;
    icon->setFixedSize(128, 128);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(icon_for_path(path).pixmap(96, 96));
    form->addRow(icon);
    if (ThumbnailManager *t = ThumbnailManager::instance(); single && isdir(path) && t) {
        QPixmap pm = t->folder_pixmap(path, QFileInfo(path).lastModified().toSecsSinceEpoch(), 128);
        if (!pm.isNull())
            icon->setPixmap(t->scaled(pm, 128));
    }
    if (single && !isdir(path) && thumbs::can_thumbnail(path)) {
        struct stat st;
        if (stat_(path, st)) {
            QString p = path;
            qint64 mtime = st.st_mtime;
            QPointer<QLabel> ic(icon);
            run([p, mtime]() { return QVariant(thumbs::file_thumb(p, mtime, 128)); },
                [ic](const QVariant &img) {
                    if (ic && !img.value<QImage>().isNull())
                        ic->setPixmap(QPixmap::fromImage(img.value<QImage>()));
                });
        }
    }
    if (single) {
        struct stat st;
        lstat_(path, st);
        bool is_dir = isdir(path);
        name_edit = new QLineEdit(basename(rstrip(path, '/')));
        form->addRow("Name:", name_edit);
        QMimeType mime = isfile(path) ? mime_for_content(path) : mime_for(path);
        form->addRow("Type:", sel_label(QString("%1 (%2)").arg(mime.comment(), mime.name())));
        form->addRow("Location:", sel_label(dirname(abspath(path))));
        if (S_ISLNK(st.st_mode)) {
            QString target;
            try {
                target = util::readlink(path);
            } catch (const OSError &) {
            }
            form->addRow("Link target:", sel_label(target + (exists(path) ? "" : "  (broken link)")));
        }
        size_label = sel_label(is_dir ? QString("Calculating…")
                                      : QString("%1 (%2 bytes)").arg(human_size(qint64(st.st_size)),
                                                                     group_digits(st.st_size)));
        form->addRow("Size:", size_label);
        if (is_dir) {
            QString p = path;
            run([p]() { return QVariant::fromValue(fileops::dir_stats(p)); },
                [this](const QVariant &r) { show_dir_size(r); });
        }
        QDateTime bt = QFileInfo(path).birthTime();
        form->addRow("Created:", sel_label(bt.isValid() ? fmt_ts(bt.toSecsSinceEpoch())
                                                        : QString("unknown (not supported by filesystem)")));
        form->addRow("Modified:", sel_label(fmt_ts(st.st_mtime)));
        form->addRow("Accessed:", sel_label(fmt_ts(st.st_atime)));
        form->addRow("Changed:", sel_label(fmt_ts(st.st_ctime) + "  (metadata)"));
        form->addRow("Inode / links:", sel_label(QString("%1 / %2  (device %3)")
                                                     .arg(qulonglong(st.st_ino))
                                                     .arg(qulonglong(st.st_nlink))
                                                     .arg(qulonglong(st.st_dev))));
        if (is_dir)
            folder_style_rows(form);
        if (isfile(path)) {
            AppRef app = default_app(path);
            auto *row = new QHBoxLayout;
            app_label = new QLabel(app ? app_name(app) : QString("—"));
            row->addWidget(app_label, 1);
            auto *btn = new QPushButton("Change…");
            connect(btn, &QPushButton::clicked, this, &PropertiesDialog::change_app);
            row->addWidget(btn);
            form->addRow("Opens with:", row);
        }
    } else {
        form->addRow("Items:", sel_label(QString("%1 selected").arg(paths.size())));
        form->addRow("Location:", sel_label(dirname(path)));
        size_label = sel_label("Calculating…");
        form->addRow("Total size:", size_label);
        QStringList ps = paths;
        run(
            [ps]() {
                fileops::DirStats total;
                for (const QString &p : ps) {
                    if (isdir(p) && !islink(p)) {
                        fileops::DirStats s = fileops::dir_stats(p);
                        total.size += s.size;
                        total.files += s.files;
                        total.dirs += s.dirs + 1;
                    } else {
                        struct stat st;
                        if (lstat_(p, st))
                            total.size += st.st_size;
                        total.files += 1;
                    }
                }
                return QVariant::fromValue(total);
            },
            [this](const QVariant &r) { show_dir_size(r); });
    }
    QStorageInfo vol(path);
    if (vol.isValid())
        form->addRow("Volume:", sel_label(QString("%1 (%2) — %3 free of %4")
                                              .arg(vol.rootPath(), QString::fromUtf8(vol.fileSystemType()),
                                                   human_size(vol.bytesAvailable()), human_size(vol.bytesTotal()))));
    return w;
}

void PropertiesDialog::folder_style_rows(QFormLayout *form)
{
    // folder colour and image previews for this folder (also in the folder's right-click menu)
    ThumbnailManager *t = ThumbnailManager::instance();
    if (!t)
        return;
    style_color = new QComboBox;
    style_color->addItem(thumbs::color_swatch(t->folder_color), "Default", QString());
    for (const auto &[name, color] : thumbs::FOLDER_COLORS)
        style_color->addItem(thumbs::color_swatch(color), name, color);
    QString cur = t->custom_color(path);
    if (!cur.isEmpty() && style_color->findData(cur) < 0)
        style_color->addItem(thumbs::color_swatch(cur), QString("Custom (%1)").arg(cur), cur);
    style_color->setCurrentIndex(std::max(0, style_color->findData(cur)));
    form->addRow("Folder colour:", style_color);
    style_previews = new QCheckBox("Show image previews on this folder's icon");
    style_previews->setChecked(t->previews_for(path));
    form->addRow("", style_previews);
}

void PropertiesDialog::apply_folder_style()
{
    ThumbnailManager *t = ThumbnailManager::instance();
    if (!t || !style_color)
        return;
    QString color = style_color->currentData().toString();
    if (color != t->custom_color(path))
        t->set_folder_color({path}, color);
    bool on = style_previews->isChecked();
    if (on != t->previews_for(path))
        t->set_folder_previews({path}, on);
}

void PropertiesDialog::show_dir_size(const QVariant &res)
{
    auto s = res.value<fileops::DirStats>();
    size_label->setText(QString("%1 (%2 bytes)\n%3 files, %4 folders")
                            .arg(human_size(s.size), group_digits(s.size), group_digits(s.files), group_digits(s.dirs)));
}

void PropertiesDialog::change_app()
{
    OpenWithDialog dlg(this, {path}, false);
    dlg.default_box->setChecked(true);
    if (dlg.exec() && dlg.selected_app())
        app_label->setText(app_name(dlg.selected_app()));
}

QWidget *PropertiesDialog::perm_tab()
{
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    struct stat st;
    lstat_(path, st);
    orig_mode = st.st_mode & 07777;
    auto *form = new QFormLayout;
    struct passwd *pw = getpwuid(st.st_uid);
    struct group *gr = getgrgid(st.st_gid);
    QString owner = pw ? QString::fromLocal8Bit(pw->pw_name) : QString::number(st.st_uid);
    QString group = gr ? QString::fromLocal8Bit(gr->gr_name) : QString::number(st.st_gid);
    form->addRow("Owner:", new QLabel(QString("%1 (%2)").arg(owner, QString::number(st.st_uid))));
    form->addRow("Group:", new QLabel(QString("%1 (%2)").arg(group, QString::number(st.st_gid))));
    lay->addLayout(form);
    auto *grid = new QGridLayout;
    bool is_dir = isdir(path);
    QStringList cols = {"Read", "Write", is_dir ? "Access" : "Execute"};
    for (int c = 0; c < 3; ++c)
        grid->addWidget(new QLabel("<b>" + cols[c] + "</b>"), 0, c + 1);
    const mode_t bits[3][3] = {{S_IRUSR, S_IWUSR, S_IXUSR}, {S_IRGRP, S_IWGRP, S_IXGRP}, {S_IROTH, S_IWOTH, S_IXOTH}};
    const QStringList who = {"Owner", "Group", "Others"};
    for (int r = 0; r < 3; ++r) {
        grid->addWidget(new QLabel(who[r]), r + 1, 0);
        for (int c = 0; c < 3; ++c) {
            auto *cb = new QCheckBox;
            cb->setChecked(orig_mode & bits[r][c]);
            connect(cb, &QCheckBox::toggled, this, &PropertiesDialog::update_octal);
            perm_boxes.insert(bits[r][c], cb);
            grid->addWidget(cb, r + 1, c + 1);
        }
    }
    lay->addLayout(grid);
    auto *special = new QHBoxLayout;
    const QList<QPair<mode_t, QString>> specials = {{S_ISUID, "Set UID"}, {S_ISGID, "Set GID"}, {S_ISVTX, "Sticky"}};
    for (const auto &[bit, label] : specials) {
        auto *cb = new QCheckBox(label);
        cb->setChecked(orig_mode & bit);
        connect(cb, &QCheckBox::toggled, this, &PropertiesDialog::update_octal);
        perm_boxes.insert(bit, cb);
        special->addWidget(cb);
    }
    lay->addLayout(special);
    octal = new QLabel;
    lay->addWidget(octal);
    if (!is_dir) {
        auto *exe = new QCheckBox("Allow executing file as program");
        exe->setChecked(orig_mode & S_IXUSR);
        connect(exe, &QCheckBox::toggled, this, &PropertiesDialog::toggle_exec);
        lay->addWidget(exe);
    }
    bool editable = st.st_uid == ::getuid() || ::getuid() == 0;
    if (!editable) {
        lay->addWidget(new QLabel("<i>You are not the owner, so you cannot change these permissions.</i>"));
        for (QCheckBox *cb : perm_boxes)
            cb->setEnabled(false);
    }
    lay->addStretch(1);
    update_octal();
    return w;
}

void PropertiesDialog::toggle_exec(bool on)
{
    const QList<QPair<mode_t, mode_t>> pairs = {{S_IRUSR, S_IXUSR}, {S_IRGRP, S_IXGRP}, {S_IROTH, S_IXOTH}};
    for (const auto &[r, x] : pairs)
        perm_boxes[x]->setChecked(on && (x == S_IXUSR || perm_boxes[r]->isChecked()));
}

mode_t PropertiesDialog::mode() const
{
    mode_t m = 0;
    for (auto it = perm_boxes.begin(); it != perm_boxes.end(); ++it)
        if (it.value()->isChecked())
            m |= it.key();
    return m;
}

void PropertiesDialog::update_octal()
{
    mode_t m = mode();
    octal->setText(QString("Octal: <b>%1</b>    Symbolic: <b>%2</b>")
                       .arg(QString::number(m, 8).rightJustified(4, '0'),
                            filemode(m | (isdir(path) ? S_IFDIR : S_IFREG))));
}

QWidget *PropertiesDialog::image_tab()
{
    auto *w = new QWidget;
    auto *form = new QFormLayout(w);
    for (const auto &[k, v] : metadata::basic_info(path))
        form->addRow(k + ":", sel_label(v));
    for (const auto &[k, v] : metadata::ai_info(path)) {
        if (v.size() > 80 || v.contains('\n')) {
            auto *ed = new QPlainTextEdit(v);
            ed->setReadOnly(true);
            ed->setMaximumHeight(110);
            form->addRow(k + ":", ed);
        } else {
            form->addRow(k + ":", sel_label(v));
        }
    }
    auto *btn = new QPushButton(uwp::wallpaper_label());
    QString p = path;
    connect(btn, &QPushButton::clicked, this, [p]() { set_wallpaper(p); });
    form->addRow(btn);
    return w;
}

QWidget *PropertiesDialog::meta_tab()
{
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    meta_filter = new QLineEdit;
    meta_filter->setPlaceholderText("Filter tags…");
    connect(meta_filter, &QLineEdit::textChanged, this, &PropertiesDialog::filter_meta);
    lay->addWidget(meta_filter);
    meta_tree = new QTreeWidget;
    meta_tree->setHeaderLabels({"Group", "Tag", "Value"});
    meta_tree->setRootIsDecorated(false);
    meta_tree->setAlternatingRowColors(true);
    meta_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    meta_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    connect(meta_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) { meta_edit(it); });
    connect(meta_tree, &QTreeWidget::itemSelectionChanged, this, &PropertiesDialog::meta_buttons);
    meta_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(meta_tree, &QWidget::customContextMenuRequested, this, &PropertiesDialog::meta_menu);
    lay->addWidget(meta_tree);
    meta_editable = metadata::can_edit() && !metadata::tag_families(path).isEmpty();
    auto *row = new QHBoxLayout;
    meta_add_btn = new QPushButton("Add Tag…");
    connect(meta_add_btn, &QPushButton::clicked, this, &PropertiesDialog::meta_add);
    meta_edit_btn = new QPushButton("Edit…");
    connect(meta_edit_btn, &QPushButton::clicked, this, [this]() { meta_edit(meta_tree->currentItem()); });
    meta_del_btn = new QPushButton("Remove");
    connect(meta_del_btn, &QPushButton::clicked, this, &PropertiesDialog::meta_remove);
    meta_clear_btn = new QPushButton("Clear All Metadata…");
    connect(meta_clear_btn, &QPushButton::clicked, this, &PropertiesDialog::meta_clear);
    for (QPushButton *b : {meta_add_btn, meta_edit_btn, meta_del_btn})
        row->addWidget(b);
    row->addStretch(1);
    row->addWidget(meta_clear_btn);
    lay->addLayout(row);
    meta_add_btn->setEnabled(meta_editable);
    meta_clear_btn->setEnabled(meta_editable);
    meta_buttons();
    lay->addWidget(new QLabel(
        meta_editable ? "<small>Double-click a row to edit it (greyed-out rows are read-only). "
                        "Changes are written to the file immediately.</small>"
                      : QString("<small>Double-click a row to see the full value. ") +
                            (metadata::can_edit() ? "This file type's metadata can't be edited.</small>"
                                                  : "Install exiftool (libimage-exiftool-perl) to edit metadata.</small>")));
    meta_loaded = false;
    connect(tabs, &QTabWidget::currentChanged, this, &PropertiesDialog::maybe_load_meta);
    return w;
}

bool PropertiesDialog::meta_row_editable(QTreeWidgetItem *it) const
{
    return meta_editable && it && it->data(0, Qt::UserRole).toBool();
}

QList<QTreeWidgetItem *> PropertiesDialog::meta_selected() const
{
    QList<QTreeWidgetItem *> out;
    for (QTreeWidgetItem *it : meta_tree->selectedItems())
        if (meta_row_editable(it))
            out << it;
    return out;
}

void PropertiesDialog::meta_buttons()
{
    auto sel = meta_selected();
    QTreeWidgetItem *cur = meta_tree->currentItem();
    meta_edit_btn->setEnabled(meta_row_editable(cur) && cur->isSelected());
    meta_del_btn->setEnabled(!sel.isEmpty());
    meta_del_btn->setText(sel.size() > 1 ? QString("Remove %1 Tags").arg(sel.size()) : QString("Remove"));
}

QString PropertiesDialog::meta_key(QTreeWidgetItem *it) const
{
    return it->text(0).isEmpty() ? it->text(1) : it->text(0) + ":" + it->text(1);
}

void PropertiesDialog::meta_write(std::function<QStringList()> fn)
{
    // run a metadata write in the background, then reload the tag list
    run([fn]() { return QVariant(fn()); },
        [this](const QVariant &warnings) {
            if (!warnings.toStringList().isEmpty())
                QMessageBox::information(this, "Metadata", warnings.toStringList().join('\n'));
            reload_meta();
        },
        "Writing metadata");
}

static QVariant meta_rows(const QString &path)
{
    QVariantList rows;
    for (const metadata::MetaRow &r : metadata::full_metadata(path))
        rows << QVariant(QStringList{r.group, r.tag, r.value});
    return rows;
}

void PropertiesDialog::reload_meta()
{
    meta_loaded = true;
    QString p = path;
    run([p]() { return meta_rows(p); }, [this](const QVariant &r) { show_meta(r); });
}

void PropertiesDialog::meta_edit(QTreeWidgetItem *it)
{
    if (!it)
        return;
    QString val = it->data(2, Qt::UserRole).toString();
    if (!meta_row_editable(it)) {
        TextDialog(this, it->text(1), val).exec();
        return;
    }
    auto res = MetaTagDialog::ask(this, meta_key(it), val, path);
    if (res) {
        QString p = path;
        metadata::Change c = *res;
        meta_write([p, c]() { return metadata::set_tags(p, {c}); });
    }
}

void PropertiesDialog::meta_add()
{
    QHash<QString, QString> existing;
    for (int i = 0; i < meta_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = meta_tree->topLevelItem(i);
        existing.insert(meta_key(it), it->data(2, Qt::UserRole).toString());
    }
    auto res = MetaTagDialog::ask(this, "", "", path, existing);
    if (res) {
        QString p = path;
        metadata::Change c = *res;
        meta_write([p, c]() { return metadata::set_tags(p, {c}); });
    }
}

void PropertiesDialog::meta_remove()
{
    auto sel = meta_selected();
    if (sel.isEmpty())
        return;
    QStringList keys;
    for (QTreeWidgetItem *it : sel)
        keys << meta_key(it);
    QString what = keys.size() == 1 ? keys.first() : QString("%1 tags").arg(keys.size());
    if (QMessageBox::question(this, "Remove Metadata", QString("Remove %1 from this file?\n\nThis can't be undone.").arg(what)) !=
        QMessageBox::Yes)
        return;
    QList<metadata::Change> changes;
    for (const QString &k : keys) {
        metadata::Change c;
        c.key = k;
        c.remove = true;
        changes << c;
    }
    QString p = path;
    meta_write([p, changes]() { return metadata::set_tags(p, changes); });
}

void PropertiesDialog::meta_clear()
{
    QMessageBox box(QMessageBox::Warning, "Clear All Metadata",
                    QString("Remove all metadata (EXIF, XMP, IPTC, GPS, comments, image-generation prompts…) from “%1”?"
                            "\n\nThis can't be undone.")
                        .arg(basename(path)),
                    QMessageBox::Cancel, this);
    QPushButton *clear = box.addButton("Clear All", QMessageBox::DestructiveRole);
    auto *keep = new QCheckBox("Keep orientation and colour profile");
    keep->setChecked(true);
    box.setCheckBox(keep);
    box.exec();
    if (box.clickedButton() != clear)
        return;
    bool k = keep->isChecked();
    QString p = path;
    meta_write([p, k]() { return metadata::clear_all(p, k); });
}

void PropertiesDialog::maybe_load_meta(int idx)
{
    if (meta_loaded || tabs->widget(idx) != meta_tab_widget)
        return;
    meta_loaded = true;
    if (meta_editable && !metadata::tag_db_if_loaded())
        fileops::run_task(
            this, "Loading tags…",
            []() {
                metadata::tag_db();
                return QVariant(true);
            },
            nullptr, true);
    meta_tree->addTopLevelItem(new QTreeWidgetItem(QStringList{"", "Loading…", ""}));
    QString p = path;
    run([p]() { return meta_rows(p); }, [this](const QVariant &r) { show_meta(r); });
}

void PropertiesDialog::show_meta(const QVariant &rows)
{
    meta_tree->clear();
    QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
    for (const QVariant &rv : rows.toList()) {
        QStringList r = rv.toStringList();
        QString group = r.value(0), tag = r.value(1), val = r.value(2);
        QString short_val = val.size() < 300 ? val : val.left(300).replace('\n', ' ') + " …";
        auto *it = new QTreeWidgetItem(QStringList{group, tag, QString(short_val).replace('\n', ' ')});
        it->setData(2, Qt::UserRole, val);
        bool editable = metadata::is_editable(group, val, tag);
        it->setData(0, Qt::UserRole, editable);
        it->setToolTip(2, short_val);
        if (!editable)
            for (int c = 0; c < 3; ++c)
                it->setForeground(c, dim);
        meta_tree->addTopLevelItem(it);
    }
    meta_tree->resizeColumnToContents(0);
    meta_tree->resizeColumnToContents(1);
    filter_meta(meta_filter->text());
    meta_buttons();
}

void PropertiesDialog::filter_meta(const QString &text)
{
    QString t = text.toLower();
    for (int i = 0; i < meta_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = meta_tree->topLevelItem(i);
        QString hay = QString("%1 %2 %3").arg(it->text(0), it->text(1), it->data(2, Qt::UserRole).toString()).toLower();
        it->setHidden(!t.isEmpty() && !hay.contains(t));
    }
}

void PropertiesDialog::meta_menu(const QPoint &pos)
{
    QTreeWidgetItem *it = meta_tree->itemAt(pos);
    if (!it)
        return;
    QString val = it->data(2, Qt::UserRole).toString();
    QString row = QString("%1:%2 = %3").arg(it->text(0), it->text(1), val);
    QString tag = it->text(1);
    QMenu m(this);
    m.addAction("Copy Value", this, [val]() { QGuiApplication::clipboard()->setText(val); });
    m.addAction("Copy Row", this, [row]() { QGuiApplication::clipboard()->setText(row); });
    m.addAction("View Value…", this, [this, tag, val]() { TextDialog(this, tag, val).exec(); });
    if (meta_editable) {
        m.addSeparator();
        if (meta_row_editable(it))
            m.addAction("Edit Value…", this, [this, it]() { meta_edit(it); });
        auto sel = meta_selected();
        if (!sel.isEmpty())
            m.addAction(sel.size() > 1 ? QString("Remove %1 Tags…").arg(sel.size()) : QString("Remove Tag…"), this,
                        &PropertiesDialog::meta_remove);
        m.addAction("Add Tag…", this, &PropertiesDialog::meta_add);
    }
    m.exec(meta_tree->viewport()->mapToGlobal(pos));
}

QWidget *PropertiesDialog::checksum_tab()
{
    auto *w = new QWidget;
    auto *form = new QFormLayout(w);
    auto outputs = std::make_shared<QList<QLineEdit *>>();
    const QList<QPair<QString, QCryptographicHash::Algorithm>> algos = {
        {"md5", QCryptographicHash::Md5}, {"sha1", QCryptographicHash::Sha1}, {"sha256", QCryptographicHash::Sha256}};
    for (const auto &[name, algo] : algos) {
        auto *row = new QHBoxLayout;
        auto *out = new QLineEdit;
        out->setReadOnly(true);
        *outputs << out;
        auto *btn = new QPushButton("Compute");
        row->addWidget(out, 1);
        row->addWidget(btn);
        form->addRow(name.toUpper() + ":", row);
        QString n = name;
        QCryptographicHash::Algorithm a = algo;
        connect(btn, &QPushButton::clicked, this, [this, n, a, out, btn]() {
            btn->setEnabled(false);
            out->setText("Computing…");
            QString p = path;
            run(
                [p, a]() -> QVariant {
                    QFile f(p);
                    if (!f.open(QIODevice::ReadOnly))
                        throw OSError(EACCES, f.errorString());
                    QCryptographicHash h(a);
                    while (!f.atEnd())
                        h.addData(f.read(4 << 20));
                    return QString::fromLatin1(h.result().toHex());
                },
                [out](const QVariant &r) { out->setText(r.toString()); }, "Computing " + n.toUpper());
        });
    }
    auto *verify = new QLineEdit;
    verify->setPlaceholderText("Paste a checksum to compare…");
    auto *result = new QLabel;
    connect(verify, &QLineEdit::textChanged, this, [outputs, result](const QString &text) {
        QString t = text.trimmed().toLower();
        bool match = false;
        for (QLineEdit *o : *outputs)
            match = match || o->text().toLower() == t;
        result->setText(t.isEmpty() ? QString() : (match ? "✔ Match" : "✘ No match (compute first)"));
    });
    form->addRow("Verify:", verify);
    form->addRow("", result);
    return w;
}

void PropertiesDialog::apply()
{
    if (single) {
        apply_folder_style();   // before a rename: styles are kept by path
        QWidget *owner = parentWidget() ? parentWidget() : this;
        if (!perm_boxes.isEmpty()) {
            mode_t m = mode();
            if (m != orig_mode) {
                if (::chmod(enc(path).constData(), m) != 0) {
                    int e = errno;
                    if (e == EACCES || e == EPERM) {
                        QString p = path;
                        admin::retry_as_admin(
                            owner, "Permissions",
                            QString("You don't have permission to change the permissions of “%1”.").arg(basename(path)),
                            [p, m](Task *task) {
                                admin::session().call(task, "chmod", {{"path", p}, {"mode", int(m)}});
                                return QStringList();
                            });
                    } else {
                        QMessageBox::warning(this, "Permissions", errno_text(e, path));
                    }
                }
            }
        }
        QString new_name = name_edit->text().trimmed();
        if (!new_name.isEmpty() && new_name != basename(rstrip(path, '/'))) {
            auto res = dialogs::do_rename(path, new_name);
            if (!res.denied && res.error.isEmpty())
                undo::record("rename", "Rename", {qMakePair(path, join(dirname(path), new_name))});
            if (res.denied) {
                QString p = path, target = join(dirname(path), new_name);
                admin::retry_as_admin(owner, "Rename",
                                      QString("You don't have permission to rename “%1”.").arg(basename(path)),
                                      [p, target](Task *task) {
                                          admin::session().call(task, "rename", {{"src", p}, {"dst", target}});
                                          return QStringList();
                                      });
            } else if (!res.error.isEmpty()) {
                QMessageBox::warning(this, "Rename", res.error);
                return;
            }
        }
    }
    accept();
}

// ---------------------------------------------------------------- open with

OpenWithDialog::OpenWithDialog(QWidget *parent, const QStringList &paths, bool launch)
    : QDialog(parent), paths(paths), launch(launch)
{
    setWindowTitle("Open With");
    resize(420, 520);
    auto *lay = new QVBoxLayout(this);
    QString ct = content_type(paths.first());
    lay->addWidget(new QLabel(QString("Choose an application for “%1”\n<%2>").arg(basename(paths.first()), ct)));
    search = new QLineEdit;
    search->setPlaceholderText("Search applications…");
    connect(search, &QLineEdit::textChanged, this, &OpenWithDialog::filter);
    lay->addWidget(search);
    list = new QListWidget;
    connect(list, &QListWidget::itemDoubleClicked, this, [this]() { ok(); });
    lay->addWidget(list);
    auto [rec, others] = apps_for(paths.first());
    const QList<QPair<QString, QList<AppRef>>> groups = {{"Recommended", rec}, {"Other applications", others}};
    for (const auto &[group, apps] : groups) {
        if (apps.isEmpty())
            continue;
        auto *h = new QListWidgetItem(group);
        h->setFlags(Qt::NoItemFlags);
        QFont f = h->font();
        f.setBold(true);
        h->setFont(f);
        list->addItem(h);
        for (const AppRef &app : apps) {
            auto *it = new QListWidgetItem(app_icon(app), app_name(app));
            it->setData(Qt::UserRole, QVariant::fromValue(app));
            list->addItem(it);
        }
    }
    if (list->count() > 1)
        list->setCurrentRow(1);
    default_box = new QCheckBox("Always use for this file type");
    lay->addWidget(default_box);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &OpenWithDialog::ok);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
}

void OpenWithDialog::filter(const QString &text)
{
    QString t = text.toLower();
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *it = list->item(i);
        if (it->data(Qt::UserRole).isValid())
            it->setHidden(!t.isEmpty() && !it->text().toLower().contains(t));
    }
}

AppRef OpenWithDialog::selected_app() const
{
    QListWidgetItem *it = list->currentItem();
    return it ? it->data(Qt::UserRole).value<AppRef>() : AppRef();
}

void OpenWithDialog::ok()
{
    AppRef app = selected_app();
    if (!app)
        return;
    try {
        if (default_box->isChecked())
            set_default_for_type(app, content_type(paths.first()));
        if (launch)
            launch_app(app, paths);
    } catch (const Error &e) {
        QMessageBox::warning(this, "Open With", e.message());
    }
    accept();
}

// ---------------------------------------------------------------- batch rename

BatchRenameDialog::BatchRenameDialog(QWidget *parent, const QStringList &paths) : QDialog(parent), paths(paths)
{
    setWindowTitle(QString("Rename %1 Items").arg(paths.size()));
    resize(700, 520);
    auto *lay = new QVBoxLayout(this);
    auto *modes = new QHBoxLayout;
    r_tmpl = new QRadioButton("Rename using a template");
    r_repl = new QRadioButton("Find and replace text");
    r_tmpl->setChecked(true);
    auto *grp = new QButtonGroup(this);
    grp->addButton(r_tmpl);
    grp->addButton(r_repl);
    modes->addWidget(r_tmpl);
    modes->addWidget(r_repl);
    lay->addLayout(modes);
    auto *form = new QFormLayout;
    tmpl = new QLineEdit("[Name] ###");
    tmpl->setToolTip("[Name] = original name, # = number (### pads to 3 digits), [Date] = modified date");
    start = new QSpinBox;
    start->setRange(0, 1000000);
    start->setValue(1);
    find = new QLineEdit;
    repl = new QLineEdit;
    regex = new QCheckBox("Regular expression");
    keep_ext = new QCheckBox("Keep file extension");
    keep_ext->setChecked(true);
    form->addRow("Template:", tmpl);
    form->addRow("Start number:", start);
    form->addRow("Find:", find);
    form->addRow("Replace with:", repl);
    form->addRow("", regex);
    form->addRow("", keep_ext);
    lay->addLayout(form);
    lay->addWidget(new QLabel("<small>Template tokens: <b>[Name]</b> original name, <b>#</b> counter "
                              "(<b>###</b> = 001), <b>[Date]</b> modified date (YYYY-MM-DD)</small>"));
    table = new QTableWidget(0, 2);
    table->setHorizontalHeaderLabels({"Original", "New name"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(table);
    status = new QLabel;
    lay->addWidget(status);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ok_btn = bb->button(QDialogButtonBox::Ok);
    ok_btn->setText("Rename");
    connect(bb, &QDialogButtonBox::accepted, this, &BatchRenameDialog::apply);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
    for (QLineEdit *w : {tmpl, find, repl})
        connect(w, &QLineEdit::textChanged, this, &BatchRenameDialog::preview);
    for (QAbstractButton *w : std::initializer_list<QAbstractButton *>{r_tmpl, r_repl, regex, keep_ext})
        connect(w, &QAbstractButton::toggled, this, &BatchRenameDialog::preview);
    connect(start, &QSpinBox::valueChanged, this, &BatchRenameDialog::preview);
    preview();
}

QStringList BatchRenameDialog::new_names() const
{
    QStringList out;
    int n = start->value();
    static const QRegularExpression hashes("#+");
    for (const QString &p : paths) {
        QString name = basename(p);
        QString stem = name, ext;
        if (!(isdir(p) || !keep_ext->isChecked())) {
            auto se = split_ext(name);
            stem = se.first;
            ext = se.second;
        }
        QString nw;
        if (r_tmpl->isChecked()) {
            QString t = tmpl->text();
            t.replace("[Name]", stem);
            struct stat st;
            lstat_(p, st);
            t.replace("[Date]", fmt_time(st.st_mtime, "%Y-%m-%d"));
            QString res;
            int last = 0;
            auto it = hashes.globalMatch(t);
            while (it.hasNext()) {
                auto m = it.next();
                res += t.mid(last, m.capturedStart() - last);
                res += QString::number(n).rightJustified(m.capturedLength(), '0');
                last = m.capturedEnd();
            }
            res += t.mid(last);
            nw = res + ext;
        } else {
            QString f = find->text();
            if (f.isEmpty()) {
                nw = name;
            } else if (regex->isChecked()) {
                QRegularExpression re(f);
                if (!re.isValid()) {
                    nw = name;
                } else {
                    QString s = stem;
                    nw = s.replace(re, repl->text()) + ext;
                }
            } else {
                QString s = stem;
                nw = s.replace(f, repl->text()) + ext;
            }
        }
        out << nw;
        n += 1;
    }
    return out;
}

void BatchRenameDialog::preview()
{
    QStringList names = new_names();
    table->setRowCount(names.size());
    int problems = 0;
    QSet<QString> dupes;
    QHash<QString, int> counts;
    for (const QString &n : names)
        counts[n] += 1;
    for (auto it = counts.begin(); it != counts.end(); ++it)
        if (it.value() > 1)
            dupes << it.key();
    for (int i = 0; i < names.size(); ++i) {
        const QString &p = paths[i], &nw = names[i];
        table->setItem(i, 0, new QTableWidgetItem(basename(p)));
        auto *it = new QTableWidgetItem(nw);
        QString target = join(dirname(p), nw);
        bool bad = nw.trimmed().isEmpty() || nw.contains('/') || dupes.contains(nw) ||
                   (nw != basename(p) && lexists(target) && !paths.contains(target));
        if (bad) {
            it->setForeground(error_color());
            problems += 1;
        }
        table->setItem(i, 1, it);
    }
    status->setText(problems ? QString("%1 conflicting name(s)").arg(problems) : QString());
    ok_btn->setEnabled(problems == 0);
}

void BatchRenameDialog::apply()
{
    QStringList names = new_names();
    QList<QPair<QString, QString>> temps;
    try {
        for (const QString &p : paths) {   // two-phase rename so swaps/overlaps work
            QString tmp = join(dirname(p), QString(".fe-rename-%1-%2").arg(::getpid()).arg(temps.size()));
            util::rename(p, tmp);
            temps << qMakePair(tmp, p);
        }
        for (int i = 0; i < temps.size(); ++i)
            util::rename(temps[i].first, join(dirname(temps[i].second), names[i]));
        QList<QPair<QString, QString>> renamed;
        for (int i = 0; i < temps.size(); ++i)
            if (basename(temps[i].second) != names[i])
                renamed << qMakePair(temps[i].second, join(dirname(temps[i].second), names[i]));
        undo::record("rename", QString("Rename %1 Items").arg(names.size()), renamed);
    } catch (const OSError &e) {
        for (const auto &[tmp, p] : temps)
            if (exists(tmp))
                ::rename(enc(tmp).constData(), enc(p).constData());
        QMessageBox::warning(this, "Rename", e.message());
        return;
    }
    accept();
}

// ---------------------------------------------------------------- preferences

PreferencesDialog::PreferencesDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Preferences");
    QSettings &s = settings();
    auto *form = new QFormLayout(this);
    // homepage: overview page, home folder, or any folder / network location
    QString hp = s.value("homepage", "overview").toString();
    hp_mode = new QComboBox;
    hp_mode->addItems({"Overview (drives, network & bookmarks)", "Home folder", "Custom location…"});
    hp_mode->setCurrentIndex(hp == "overview" ? 0 : hp == "home" ? 1 : 2);
    hp_edit = new QLineEdit(hp == "overview" || hp == "home" ? QString() : hp);
    hp_edit->setPlaceholderText("/path/to/folder  or  smb://server/share, sftp://user@host/path");
    auto *browse = new QPushButton("Browse…");
    connect(browse, &QPushButton::clicked, this, &PreferencesDialog::browse_home);
    auto *hp_row = new QHBoxLayout;
    hp_row->addWidget(hp_edit, 1);
    hp_row->addWidget(browse);
    hp_custom = new QWidget;
    hp_custom->setLayout(hp_row);
    hp_row->setContentsMargins(0, 0, 0, 0);
    connect(hp_mode, &QComboBox::currentIndexChanged, this, [this](int i) { hp_custom->setEnabled(i == 2); });
    hp_custom->setEnabled(hp_mode->currentIndex() == 2);
    form->addRow("Homepage:", hp_mode);
    form->addRow("", hp_custom);
    form->addRow(new QLabel("<small>Used when the app starts and for the Home button (Alt+Home). "
                            "Network locations are connected automatically.</small>"));
    count = new QSpinBox;
    count->setRange(1, 4);
    count->setValue(s.value("folder_count", 4).toInt());
    order = new QComboBox;
    order->addItems({"First by name", "Newest first"});
    order->setCurrentIndex(s.value("folder_order", "name").toString() == "newest" ? 1 : 0);
    QString color_setting = s.value("folder_color", "accent").toString();
    color_accent = new QCheckBox("Use the desktop's accent colour");
    color_accent->setChecked(thumbs::follows_accent(color_setting));
    color = color_accent->isChecked() ? accent_color() : QColor(color_setting);
    color_btn = new QPushButton;
    paint_color();
    connect(color_btn, &QPushButton::clicked, this, &PreferencesDialog::pick_color);
    connect(color_accent, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            color = accent_color();
        paint_color();
    });
    auto *color_row = new QHBoxLayout;
    color_row->addWidget(color_btn);
    color_row->addWidget(color_accent, 1);
    max_mb = new QSpinBox;
    max_mb->setRange(1, 10000);
    max_mb->setSuffix(" MB");
    max_mb->setValue(s.value("thumb_max_mb", 200).toInt());
    img_opener = opener_combo("image/jpeg", s.value("image_opener", "system").toString(), true);
    vid_opener = opener_combo("video/mp4", s.value("video_opener", "system").toString());
    single_click = new QCheckBox("Single-click to open items");
    single_click->setChecked(s.value("single_click", false).toBool());
    list_previews = new QCheckBox("Previews for folders in list view");
    list_previews->setChecked(s.value("list_folder_previews", false).toBool());
    slide = new QSpinBox;
    slide->setRange(1, 120);
    slide->setSuffix(" s");
    slide->setValue(s.value("slideshow_secs", 4).toInt());
    play_gifs = new QCheckBox("Play animated GIFs in the file view");
    play_gifs->setChecked(s.value("play_gifs", false).toBool());
    play_webm = new QCheckBox("Play WebM videos in the file view (silent looping previews)");
    play_webm->setChecked(s.value("play_webm", false).toBool());
    if (!which("ffmpeg")) {
        play_webm->setEnabled(false);
        play_webm->setToolTip("Needs ffmpeg:  sudo apt install ffmpeg");
    }
    shared_undo = new QCheckBox("Share undo between all Kestrel windows");
    shared_undo->setChecked(s.value("shared_undo", false).toBool());
    shared_undo->setToolTip("On: Ctrl+Z in any Kestrel window undoes the newest action from any of them.\nOff: each Kestrel undoes only what was done in it.");
    open_in_tabs = new QCheckBox("Open folders from other apps as tabs in an open Kestrel window");
    open_in_tabs->setChecked(s.value("open_in_tabs", false).toBool());
    open_in_tabs->setToolTip("On: a folder opened from another app (or with “Show in folder”) becomes a tab in the "
                             "Kestrel window you used last.\nOff: it opens in a new window.");
    form->addRow("Images in folder previews:", count);
    form->addRow("Folder preview picks:", order);
    form->addRow("Folder colour:", color_row);
    form->addRow("Don't thumbnail files larger than:", max_mb);
    form->addRow("Slideshow interval:", slide);
    form->addRow("Open images with:", img_opener);
    form->addRow("Open videos with:", vid_opener);
    form->addRow(single_click);
    form->addRow(list_previews);
    form->addRow(play_gifs);
    form->addRow(play_webm);
    form->addRow(shared_undo);
    form->addRow(open_in_tabs);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &PreferencesDialog::save);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(bb);
}

QComboBox *PreferencesDialog::opener_combo(const QString &ct, const QString &current, bool builtin)
{
    // system default / built-in viewer / any installed app that handles `ct`; item data is the setting value
    auto *cb = new QComboBox;
    AppRef def = default_app_for_type(ct);
    cb->addItem(def ? QString("System default (%1)").arg(app_name(def)) : QString("System default"), "system");
    if (builtin)
        cb->addItem(QGuiApplication::windowIcon(), "Kestrel image viewer", "builtin");
    QList<AppRef> apps = apps_for_type(ct);
    if (current != "system" && current != "builtin") {
        bool known = false;
        for (const AppRef &a : apps)
            known = known || app_id(a) == current;
        if (!known)
            if (AppRef a = app_by_id(current))
                apps << a;
    }
    for (const AppRef &a : apps)
        cb->addItem(app_icon(a), app_name(a), app_id(a));
    int i = cb->findData(current);
    cb->setCurrentIndex(i >= 0 ? i : 0);
    return cb;
}

void PreferencesDialog::browse_home()
{
    QString start = hp_edit->text().startsWith('/') ? hp_edit->text() : HOME();
    QString d = QFileDialog::getExistingDirectory(this, "Choose Homepage Folder", start);
    if (!d.isEmpty())
        hp_edit->setText(d);
}

void PreferencesDialog::paint_color()
{
    color_btn->setStyleSheet(QString("background:%1; min-width:60px; min-height:20px").arg(color.name()));
    color_btn->setEnabled(!color_accent->isChecked());
    color_btn->setToolTip(color_accent->isChecked() ? "Follows the desktop's accent colour" : "Choose the colour");
}

void PreferencesDialog::pick_color()
{
    QColor c = QColorDialog::getColor(color, this, "Folder colour");
    if (c.isValid()) {
        color = c;
        paint_color();
    }
}

void PreferencesDialog::save()
{
    QSettings &s = settings();
    int m = hp_mode->currentIndex();
    QString custom = hp_edit->text().trimmed();
    if (m == 2) {
        if (custom.isEmpty()) {
            QMessageBox::warning(this, "Homepage", "Enter a folder or network location for the homepage.");
            return;
        }
        if (custom.startsWith('~'))
            custom = expanduser(custom);
        if (custom.startsWith('/') && !isdir(custom)) {
            QMessageBox::warning(this, "Homepage", QString("“%1” is not a folder.").arg(custom));
            return;
        }
        s.setValue("homepage", custom);
    } else {
        s.setValue("homepage", m == 0 ? "overview" : "home");
    }
    s.setValue("folder_count", count->value());
    s.setValue("folder_order", order->currentIndex() == 1 ? "newest" : "name");
    s.setValue("folder_color", color_accent->isChecked() ? QString("accent") : color.name());
    s.setValue("thumb_max_mb", max_mb->value());
    s.setValue("image_opener", img_opener->currentData());
    s.setValue("video_opener", vid_opener->currentData());
    s.setValue("single_click", single_click->isChecked());
    s.setValue("list_folder_previews", list_previews->isChecked());
    s.setValue("slideshow_secs", slide->value());
    s.setValue("play_gifs", play_gifs->isChecked());
    s.setValue("play_webm", play_webm->isChecked());
    s.setValue("shared_undo", shared_undo->isChecked());
    s.setValue("open_in_tabs", open_in_tabs->isChecked());
    accept();
}

// ---------------------------------------------------------------- free functions

namespace dialogs {

RenameResult do_rename(const QString &path, const QString &new_name)
{
    if (new_name.contains('/') || new_name == "." || new_name == "..")
        return {"Invalid name.", false};
    QString target = join(dirname(path), new_name);
    if (lexists(target) && realpath(target) != realpath(path))
        return {QString("“%1” already exists.").arg(new_name), false};
    try {
        util::rename(path, target);
    } catch (const OSError &e) {
        if (e.permission())
            return {QString(), true};   // callers offer to retry as administrator
        return {e.message(), false};
    }
    return {};
}

QString ask_rename(QWidget *parent, const QString &path)
{
    QDialog dlg(parent);
    bool is_dir = isdir(path);
    dlg.setWindowTitle(is_dir ? "Rename Folder" : "Rename File");
    auto *lay = new QVBoxLayout(&dlg);
    QString old = basename(path);
    auto *edit = new QLineEdit(old);
    edit->setMinimumWidth(380);
    lay->addWidget(edit);
    auto *err = new QLabel;
    err->setStyleSheet(QString("color: %1").arg(error_color().name()));
    lay->addWidget(err);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText("Rename");
    lay->addWidget(bb);
    int stem_len = is_dir ? old.size() : split_ext(old).first.size();
    QTimer::singleShot(0, edit, [edit, stem_len]() { edit->setSelection(0, stem_len); });
    QObject::connect(edit, &QLineEdit::textChanged, &dlg, [=](const QString &text) {
        QString t = text.trimmed();
        QString msg;
        if (t.isEmpty())
            msg = " ";
        else if (t.contains('/'))
            msg = "Names cannot contain “/”.";
        else if (t != old && lexists(join(dirname(path), t)))
            msg = "An item with that name already exists.";
        err->setText(msg.trimmed());
        bb->button(QDialogButtonBox::Ok)->setEnabled(msg.isEmpty());
    });
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() && edit->text().trimmed() != old)
        return edit->text().trimmed();
    return QString();
}

QString choose_dir(QWidget *parent, const QString &title, const QString &start)
{
    return QFileDialog::getExistingDirectory(parent, title, start);
}

bool edit_bookmark(QWidget *parent, const QString &target)
{
    auto bms = read_bookmarks();
    int idx = -1;
    for (int i = 0; i < bms.size(); ++i)
        if (bms[i].first == target) {
            idx = i;
            break;
        }
    if (idx < 0)
        return false;
    QDialog d(parent);
    d.setWindowTitle("Edit Bookmark");
    d.setMinimumWidth(460);
    auto *form = new QFormLayout(&d);
    auto *name = new QLineEdit(bms[idx].second);
    auto *path = new QLineEdit(bms[idx].first);
    auto *browse = new QPushButton("Browse…");
    QObject::connect(browse, &QPushButton::clicked, &d, [&d, path]() {
        QString start = path->text().trimmed();
        QString p = choose_dir(&d, "Bookmark Location", isdir(start) ? start : HOME());
        if (!p.isEmpty())
            path->setText(p);
    });
    auto *row = new QHBoxLayout;
    row->addWidget(path, 1);
    row->addWidget(browse);
    form->addRow("Name:", name);
    form->addRow("Location:", row);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, [&d, path]() {
        QString p = path->text().trimmed();
        if (p.startsWith('~'))
            p = expanduser(p);
        if (p.isEmpty()) {
            QMessageBox::warning(&d, "Edit Bookmark", "Location can't be empty.");
            return;
        }
        if (!p.contains("://") && !isdir(p)) {
            if (QMessageBox::question(&d, "Edit Bookmark", QString("“%1” is not an existing folder. Save anyway?").arg(p)) !=
                QMessageBox::Yes)
                return;
        }
        d.accept();
    });
    name->selectAll();
    if (d.exec() != QDialog::Accepted)
        return false;
    QString p = path->text().trimmed();
    if (p.startsWith('~'))
        p = expanduser(p);
    if (!p.contains("://"))
        p = normpath(abspath(p));
    QString label = name->text().trimmed();
    if (label.isEmpty())
        label = basename(rstrip(p, '/'));
    if (label.isEmpty())
        label = p;
    bms[idx] = qMakePair(p, label);
    write_bookmarks(bms);
    return true;
}

}  // namespace dialogs
