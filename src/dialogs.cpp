#include "dialogs.h"

#include "admin.h"
#include "drives.h"
#include "fileops.h"
#include "hashcheck.h"
#include "health.h"
#include "prefs.h"
#include "undo.h"
#include "thumbs.h"
#include "uwp.h"
#include "windows.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFile>
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
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWindow>

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

static QString fmt_ts(qint64 ts) { return i18n::date_time_secs(QDateTime::fromSecsSinceEpoch(ts)); }

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
    QPushButton *copy = bb->addButton(tx("Copy"), QDialogButtonBox::ActionRole);
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
    setWindowTitle(adding ? tx("Add Tag") : tx("Edit Tag"));
    resize(600, 400);
    auto *form = new QFormLayout(this);
    if (adding) {
        key_combo = new QComboBox;
        key_combo->setEditable(true);
        key_combo->setInsertPolicy(QComboBox::NoInsert);
        key_combo->setMaxVisibleItems(20);
        key_combo->lineEdit()->setPlaceholderText(tx("Choose or type a tag, e.g. Artist or XMP-dc:Description"));
        fill(metadata::tag_choices(path, metadata::tag_db_if_loaded()));
        if (!metadata::tag_db_if_loaded()) {   // the full list takes a few seconds the first time
            fileops::run_task(
                this, tx("Loading tags…"),
                []() {
                    metadata::tag_db();
                    return QVariant(true);
                },
                [this, path](const QVariant &) { fill(metadata::tag_choices(path, metadata::tag_db_if_loaded())); },
                true);
        }
        connect(key_combo, &QComboBox::currentTextChanged, this, &MetaTagDialog::key_changed);
        form->addRow(tx("Tag:"), key_combo);
    } else {
        key_edit = new QLineEdit(key);
        key_edit->setReadOnly(true);
        form->addRow(tx("Tag:"), key_edit);
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
    form->addRow(tx("Value:"), value);
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

static const QStringList COLUMNS = {TX_NOOP("Tag"), TX_NOOP("What it's for"), TX_NOOP("Accepts")};

void MetaTagDialog::fill(const QList<metadata::Section> &sections)
{
    // (Re)build the tag dropdown as a 3-column list (tag, description, value type) with a heading per section.
    // Typing filters a matching flat list of tags without the headings.
    QString text = key_combo->currentText();
    key_combo->blockSignals(true);
    auto *model = new QStandardItemModel(0, 3, this);
    auto *flat = new QStandardItemModel(0, 3, this);
    QStringList columns;
    for (const QString &c : COLUMNS)
        columns << tx(c);
    model->setHorizontalHeaderLabels(columns);
    flat->setHorizontalHeaderLabels(columns);
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
                    it->setToolTip(tx("%1\n%2\nAccepts: %3").arg(c.key, c.blurb, c.kind));
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
        b = tx("%1 tags describe the file itself and can't be edited.").arg(group);
    } else if (b.isEmpty() && !key.isEmpty()) {
        b = help.contains(key) ? tx("No description available for this tag.")
                               : tx("Custom tag — written exactly as typed. Prefix a group (e.g. XMP-dc:) to choose where "
                                    "it goes.");
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
        notes << tx("<b>Accepts:</b> %1.").arg(kv.first);
    if (!kv.second.isEmpty())
        notes << tx("<b>Allowed values:</b> %1.").arg(kv.second.join(", ").toHtmlEscaped());
    if (is_list(key))
        notes << tx("List tag — one item per line.");
    if (adding && !key.isEmpty() && existing.contains(metadata::tag_identity(key)))
        notes << tx("This tag is already set; saving replaces its current value.");
    note->setText(notes.isEmpty() ? QString() : "<small>" + notes.join(' ') + "</small>");
    note->setVisible(!notes.isEmpty());
}

QString MetaTagDialog::tag() const { return (adding ? key_combo->currentText() : key_edit->text()).trimmed(); }

void MetaTagDialog::ok()
{
    QString key = tag();
    static const QRegularExpression re("^[\\w-]+(:[\\w-]+)?$", QRegularExpression::UseUnicodePropertiesOption);
    if (key.isEmpty() || !re.match(key).hasMatch()) {
        warn(this, windowTitle(), tx("Choose a tag, or type one like “Artist” or “XMP-dc:Description”."));
        return;
    }
    QString group = key.contains(':') ? key.section(':', 0, 0) : QString();
    if (!group.isEmpty() && !metadata::is_editable(group, "", key.section(':', 1, 1))) {
        warn(this, windowTitle(), tx("%1 tags can't be edited.").arg(group));
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

// ---------------------------------------------------------------- properties

PropertiesDialog::PropertiesDialog(QWidget *parent, const QStringList &paths_)
    : QDialog(parent), paths(paths_), single(paths_.size() == 1), path(paths_.first())
{
    QString name = basename(rstrip(path, '/'));
    if (name.isEmpty())
        name = path;
    setWindowTitle(single ? tx("%1 — Properties").arg(name) : txn("%Ln item(s) — Properties", paths.size()));
    resize(560, 600);
    auto *lay = new QVBoxLayout(this);
    tabs = new QTabWidget;
    lay->addWidget(tabs);
    tabs->addTab(general_tab(), tx("General"));
    if (single) {
        tabs->addTab(perm_tab(), tx("Permissions"));
        if (is_image(path))
            tabs->addTab(image_tab(), tx("Image"));
        if (isfile(path)) {
            meta_tab_widget = meta_tab();
            tabs->addTab(meta_tab_widget, tx("Metadata"));
            tabs->addTab(checksum_tab(), tx("Checksums"));
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
        QPixmap pm = t->folder_pixmap(path, QFileInfo(path).lastModified().toSecsSinceEpoch(), 128, true, this);
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
        form->addRow(tx("Name:"), name_edit);
        // its type from its contents, but not on a tape or floppy (reading it makes the drive seek: by its name there)
        QMimeType mime = isfile(path) && drives::reads_allowed(path) ? mime_for_content(path) : mime_for(path);
        form->addRow(tx("Type:"), sel_label(QString("%1 (%2)").arg(mime.comment(), mime.name())));
        form->addRow(tx("Location:"), sel_label(dirname(abspath(path))));
        if (S_ISLNK(st.st_mode)) {
            QString target;
            try {
                target = util::readlink(path);
            } catch (const OSError &) {
            }
            form->addRow(tx("Link target:"), sel_label(exists(path) ? target : tx("%1  (broken link)").arg(target)));
        }
        size_label = sel_label(is_dir ? tx("Calculating…")
                                      : txn("%1 (%Ln byte(s))", st.st_size).arg(human_size(qint64(st.st_size))));
        form->addRow(tx("Size:"), is_dir && !S_ISLNK(st.st_mode) ? size_row() : size_label);
        if (is_dir && !S_ISLNK(st.st_mode))
            start_size(false);
        QDateTime bt = QFileInfo(path).birthTime();
        form->addRow(tx("Created:"), sel_label(bt.isValid() ? fmt_ts(bt.toSecsSinceEpoch())
                                                        : tx("unknown (not supported by filesystem)")));
        form->addRow(tx("Modified:"), sel_label(fmt_ts(st.st_mtime)));
        form->addRow(tx("Accessed:"), sel_label(fmt_ts(st.st_atime)));
        form->addRow(tx("Changed:"), sel_label(tx("%1  (metadata)").arg(fmt_ts(st.st_ctime))));
        form->addRow(tx("Inode / links:"), sel_label(tx("%1 / %2  (device %3)")
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
            auto *btn = new QPushButton(tx("Change…"));
            connect(btn, &QPushButton::clicked, this, &PropertiesDialog::change_app);
            row->addWidget(btn);
            form->addRow(tx("Opens with:"), row);
        }
    } else {
        form->addRow(tx("Items:"), sel_label(tx("%1 selected").arg(group_digits(paths.size()))));
        form->addRow(tx("Location:"), sel_label(dirname(path)));
        size_label = sel_label(tx("Calculating…"));
        form->addRow(tx("Total size:"), size_row());
        start_size(false);
    }
    // the drive's size from drives::Sizes (statfs can wait on a slow drive): filled in when it comes
    QString root = drives::mount_root(path);
    QString fstype, device;
    for (const drives::Mount &mt : drives::mounts())
        if (mt.root == root)
            fstype = mt.fstype, device = mt.source;
    QLabel *vol = sel_label(root);
    form->addRow(tx("Volume:"), vol);
    QPointer<QLabel> vp(vol);
    auto show_vol = [vp, root, fstype]() {
        if (!vp)
            return;
        drives::Sizes::Info info = drives::Sizes::instance()->get(root);
        QString head = fstype.isEmpty() ? root : QString("%1 (%2)").arg(root, fstype);
        vp->setText(info.total >= 0 ? tx("%1 — %2 free of %3").arg(head, human_size(info.free), human_size(info.total))
                                    : tx("%1 — reading the size…").arg(head));
    };
    show_vol();
    connect(drives::Sizes::instance(), &drives::Sizes::changed, vol, [show_vol, root](const QString &r) {
        if (r == root)
            show_vol();
    });
    // a drive's own Properties (its top folder): its health, from udisks on a worker (health.h)
    if (paths.size() == 1 && path == root && device.startsWith("/dev/") && prefs::get("overview_health").toBool()) {
        QLabel *hl = sel_label(tx("Reading…"));
        hl->setObjectName("health");
        form->addRow(tx("Health:"), hl);
        QPointer<QLabel> hp(hl);
        QString name = basename(root).isEmpty() ? root : basename(root);
        run([device]() { return QVariant::fromValue(health::read({device}).value(device, health::Info{})); },
            [this, hp, name, device](const QVariant &v) {
                if (!hp)
                    return;
                health::Info h = v.value<health::Info>();
                if (h.drive.isEmpty()) {   // (udisks doesn't know it, or can't be reached)
                    hp->setText(tx("Not available"));
                    return;
                }
                hp->setTextFormat(Qt::RichText);
                hp->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
                hp->setText("<a href=\"health\">" + health::line(h).toHtmlEscaped() + "</a>");   // notr: markup
                hp->setToolTip(tx("Drive Health… (what the drive reports about itself)"));
                connect(hp, &QLabel::linkActivated, this, [this, name, h]() { health::open_window(this, name, h); });
            });
    }
    return w;
}

void PropertiesDialog::folder_style_rows(QFormLayout *form)
{
    // folder colour and image previews for this folder (also in the folder's right-click menu)
    ThumbnailManager *t = ThumbnailManager::instance();
    if (!t)
        return;
    style_color = new QComboBox;
    style_color->addItem(thumbs::color_swatch(t->folder_color), tx("Default"), QString());
    for (const auto &[name, color] : thumbs::FOLDER_COLORS)
        style_color->addItem(thumbs::color_swatch(color), tx(name), color);
    QString cur = t->custom_color(path);
    if (!cur.isEmpty() && style_color->findData(cur) < 0)
        style_color->addItem(thumbs::color_swatch(cur), tx("Custom (%1)").arg(cur), cur);
    style_color->setCurrentIndex(std::max(0, style_color->findData(cur)));
    form->addRow(tx("Folder colour:"), style_color);
    style_previews = new QCheckBox(tx("Show image previews on this folder's icon"));
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

QWidget *PropertiesDialog::size_row()
{
    auto *box = new QWidget;
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(size_label);
    auto *h = new QHBoxLayout;
    size_age = new QLabel;
    size_age->setStyleSheet("color: palette(placeholder-text)");
    recalc = new QPushButton(tx("Recalculate"));
    recalc->setToolTip(tx("Count the size again (a job in the Jobs window)"));
    connect(recalc, &QPushButton::clicked, this, [this]() { start_size(true); });
    h->addWidget(size_age, 1);
    h->addWidget(recalc);
    v->addLayout(h);
    return box;
}

void PropertiesDialog::start_size(bool fresh)
{
    // remembered: the folders counted before (unless Recalculate); the rest is counted
    QHash<QString, fileops::dirsizes::Entry> known;
    QStringList dirs;
    for (const QString &p : paths)
        if (isdir(p) && !islink(p)) {
            dirs << p;
            if (!fresh)
                if (auto e = fileops::dirsizes::get(p))
                    known.insert(p, *e);
        }
    qint64 oldest = 0;
    for (const auto &e : known)
        oldest = oldest ? std::min(oldest, e.measured) : e.measured;
    QStringList ps = paths;
    auto total_of = [ps, known](const QHash<QString, fileops::DirStats> &counted) {
        fileops::DirStats total;
        for (const QString &p : ps) {
            if (known.contains(p) || counted.contains(p)) {
                fileops::DirStats s = known.contains(p) ? known[p].stats : counted[p];
                total.size += s.size;
                total.files += s.files;
                total.dirs += s.dirs + (ps.size() > 1 ? 1 : 0);   // several: the folder itself counts too
            } else {
                struct stat st;
                if (lstat_(p, st))
                    total.size += st.st_size;
                total.files += 1;
            }
        }
        return total;
    };
    bool all_known = std::all_of(dirs.begin(), dirs.end(), [&](const QString &d) { return known.contains(d); });
    if (all_known && !dirs.isEmpty()) {
        show_dir_size(QVariant::fromValue(total_of({})));
        size_age->setText(fileops::dirsizes::age_text(oldest, QDateTime::currentSecsSinceEpoch()));
        recalc->setEnabled(true);
        return;
    }
    QStringList to_count;
    for (const QString &d : dirs)
        if (!known.contains(d))
            to_count << d;
    size_label->setText(tx("Calculating…"));
    size_age->setText(known.isEmpty() ? QString() : tx("Partly remembered"));
    recalc->setEnabled(false);
    auto work = [to_count, total_of](Task *task) {
        QHash<QString, fileops::DirStats> counted;
        for (const QString &d : to_count) {
            task->report(0, 0, basename(d));
            fileops::DirStats s = fileops::dir_stats(d, [task]() { return task->cancelled.load(); });
            task->check();   // stopped: nothing is stored
            counted.insert(d, s);
            // remembered at once, also if the window has been closed meanwhile (on the UI thread: dirsizes is)
            QMetaObject::invokeMethod(qApp, [d, s]() { fileops::dirsizes::put(d, s); });
        }
        return QVariant::fromValue(total_of(counted));
    };
    QPointer<PropertiesDialog> self(this);
    auto done = [self, oldest, known](const QVariant &res) {
        if (!self)
            return;
        self->recalc->setEnabled(true);
        if (!res.isValid()) {
            self->size_label->setText(tx("Not calculated (stopped)"));
            return;
        }
        self->show_dir_size(res);
        // the oldest figure in it says how old the total is
        self->size_age->setText(fileops::dirsizes::age_text(known.isEmpty() ? QDateTime::currentSecsSinceEpoch() : oldest,
                                                            QDateTime::currentSecsSinceEpoch()));
    };
    QString title = paths.size() == 1 ? tx("Counting the size of “%1”").arg(basename(rstrip(path, '/')))
                                      : txn("Counting the size of %Ln item(s)", paths.size());
    // Recalculate: a job of its own (in the Jobs window, with Stop); the first count is quiet, as before
    fileops::queue_job(this, title, to_count, work, done, true, {}, !fresh);
}

void PropertiesDialog::calculate_size()
{
    if (recalc && recalc->isEnabled())   // a remembered size: counted afresh (a count under way is fresh already)
        start_size(true);
}

void PropertiesDialog::show_metadata()
{
    if (meta_tab_widget)
        tabs->setCurrentWidget(meta_tab_widget);   // (loads it: maybe_load_meta)
}

void PropertiesDialog::show_dir_size(const QVariant &res)
{
    auto s = res.value<fileops::DirStats>();
    size_label->setText(tx("%1 (%2 bytes)\n%3 files, %4 folders")
                            .arg(human_size(s.size), group_digits(s.size), group_digits(s.files), group_digits(s.dirs)));
}

void PropertiesDialog::change_app()
{
    auto *dlg = new OpenWithDialog(this, {path}, false);
    dlg->default_box->setChecked(true);
    connect(dlg, &QDialog::accepted, this, [this, dlg]() {
        if (dlg->selected_app())
            app_label->setText(app_name(dlg->selected_app()));
    });
    show_free(dlg);
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
    form->addRow(tx("Owner:"), new QLabel(QString("%1 (%2)").arg(owner, QString::number(st.st_uid))));
    form->addRow(tx("Group:"), new QLabel(QString("%1 (%2)").arg(group, QString::number(st.st_gid))));
    lay->addLayout(form);
    auto *grid = new QGridLayout;
    bool is_dir = isdir(path);
    QStringList cols = {tx("Read"), tx("Write"), is_dir ? tx("Access") : tx("Execute")};
    for (int c = 0; c < 3; ++c)
        grid->addWidget(new QLabel("<b>" + cols[c] + "</b>"), 0, c + 1);
    const mode_t bits[3][3] = {{S_IRUSR, S_IWUSR, S_IXUSR}, {S_IRGRP, S_IWGRP, S_IXGRP}, {S_IROTH, S_IWOTH, S_IXOTH}};
    const QStringList who = {tx("Owner"), tx("Group"), tx("Others")};
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
    const QList<QPair<mode_t, QString>> specials = {{S_ISUID, tx("Set UID")}, {S_ISGID, tx("Set GID")}, {S_ISVTX, tx("Sticky")}};
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
        auto *exe = new QCheckBox(tx("Allow executing file as program"));
        exe->setChecked(orig_mode & S_IXUSR);
        connect(exe, &QCheckBox::toggled, this, &PropertiesDialog::toggle_exec);
        lay->addWidget(exe);
    }
    bool editable = st.st_uid == ::getuid() || ::getuid() == 0;
    if (!editable) {
        lay->addWidget(new QLabel(tx("<i>You are not the owner, so you cannot change these permissions.</i>")));
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
    octal->setText(tx("Octal: <b>%1</b>    Symbolic: <b>%2</b>")
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
    meta_filter->setPlaceholderText(tx("Filter tags…"));
    connect(meta_filter, &QLineEdit::textChanged, this, &PropertiesDialog::filter_meta);
    lay->addWidget(meta_filter);
    meta_tree = new QTreeWidget;
    meta_tree->setHeaderLabels({tx("Group"), tx("Tag"), tx("Value")});
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
    meta_add_btn = new QPushButton(tx("Add Tag…"));
    connect(meta_add_btn, &QPushButton::clicked, this, &PropertiesDialog::meta_add);
    meta_edit_btn = new QPushButton(tx("Edit…"));
    connect(meta_edit_btn, &QPushButton::clicked, this, [this]() { meta_edit(meta_tree->currentItem()); });
    meta_del_btn = new QPushButton(tx("Remove"));
    connect(meta_del_btn, &QPushButton::clicked, this, &PropertiesDialog::meta_remove);
    meta_clear_btn = new QPushButton(tx("Clear All Metadata…"));
    connect(meta_clear_btn, &QPushButton::clicked, this, &PropertiesDialog::meta_clear);
    for (QPushButton *b : {meta_add_btn, meta_edit_btn, meta_del_btn})
        row->addWidget(b);
    row->addStretch(1);
    row->addWidget(meta_clear_btn);
    lay->addLayout(row);
    meta_add_btn->setEnabled(meta_editable);
    meta_clear_btn->setEnabled(meta_editable);
    meta_buttons();
    lay->addWidget(new QLabel(   // notr: markup around the texts
        "<small>" +
        (meta_editable          ? tx("Double-click a row to edit it (greyed-out rows are read-only). Changes are written to "
                                     "the file immediately.")
         : metadata::can_edit() ? tx("Double-click a row to see the full value. This file type's metadata can't be edited.")
                                : tx("Double-click a row to see the full value. Install exiftool (libimage-exiftool-perl) to "
                                     "edit metadata.")) +
        "</small>"));
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
    meta_del_btn->setText(sel.size() > 1 ? txn("Remove %Ln Tags", sel.size()) : tx("Remove"));
}

QString PropertiesDialog::meta_key(QTreeWidgetItem *it) const
{
    return it->data(1, Qt::UserRole).toString();
}

void PropertiesDialog::meta_write(std::function<QStringList()> fn)
{
    // run a metadata write in the background, then reload the tag list
    run([fn]() { return QVariant(fn()); },
        [this](const QVariant &warnings) {
            if (!warnings.toStringList().isEmpty())
                inform(this, tx("Metadata"), warnings.toStringList().join('\n'));
            reload_meta();
        },
        tx("Writing metadata"));
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
        show_free(new TextDialog(this, it->text(1), val));
        return;
    }
    meta_ask(meta_key(it), val);
}

// the Edit / Add Tag window; saving writes the tag
void PropertiesDialog::meta_ask(const QString &key, const QString &value, const QHash<QString, QString> &existing)
{
    auto *d = new MetaTagDialog(this, key, value, path, existing);
    connect(d, &QDialog::accepted, this, [this, d]() {
        QString p = path;
        metadata::Change c = d->result_change();
        meta_write([p, c]() { return metadata::set_tags(p, {c}); });
    });
    show_free(d);
}

void PropertiesDialog::meta_add()
{
    QHash<QString, QString> existing;
    for (int i = 0; i < meta_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = meta_tree->topLevelItem(i);
        existing.insert(meta_key(it), it->data(2, Qt::UserRole).toString());
    }
    meta_ask("", "", existing);
}

void PropertiesDialog::meta_remove()
{
    auto sel = meta_selected();
    if (sel.isEmpty())
        return;
    QStringList keys;
    for (QTreeWidgetItem *it : sel)
        keys << meta_key(it);
    QString question = keys.size() == 1 ? tx("Remove %1 from this file?\n\nThis can't be undone.").arg(keys.first())
                                        : txn("Remove %Ln tag(s) from this file?\n\nThis can't be undone.", keys.size());
    auto *box = new QMessageBox(QMessageBox::Question, tx("Remove Metadata"), question, QMessageBox::Yes | QMessageBox::No, this);
    connect(box, &QMessageBox::buttonClicked, this, [this, box, keys](QAbstractButton *b) {
        if (b != box->button(QMessageBox::Yes))
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
    });
    show_free(box);
}

void PropertiesDialog::meta_clear()
{
    auto *box = new QMessageBox(QMessageBox::Warning, tx("Clear All Metadata"),
                                tx("Remove all metadata (EXIF, XMP, IPTC, GPS, comments, image-generation prompts…) "
                                        "from “%1”?\n\nThis can't be undone.")
                                    .arg(basename(path)),
                                QMessageBox::Cancel, this);
    QPushButton *clear = box->addButton(tx("Clear All"), QMessageBox::DestructiveRole);
    auto *keep = new QCheckBox(tx("Keep orientation and colour profile"));
    keep->setChecked(true);
    box->setCheckBox(keep);
    connect(box, &QMessageBox::buttonClicked, this, [this, clear, keep](QAbstractButton *b) {
        if (b != clear)
            return;
        bool k = keep->isChecked();
        QString p = path;
        meta_write([p, k]() { return metadata::clear_all(p, k); });
    });
    show_free(box);
}

void PropertiesDialog::maybe_load_meta(int idx)
{
    if (meta_loaded || tabs->widget(idx) != meta_tab_widget)
        return;
    meta_loaded = true;
    if (meta_editable && !metadata::tag_db_if_loaded())
        fileops::run_task(
            this, tx("Loading tags…"),
            []() {
                metadata::tag_db();
                return QVariant(true);
            },
            nullptr, true);
    meta_tree->addTopLevelItem(new QTreeWidgetItem(QStringList{"", tx("Loading…"), ""}));
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
        it->setData(1, Qt::UserRole, group.isEmpty() ? tag : group + ":" + tag);   // the tag written, kept apart from the shown text
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
    m.addAction(tx("Copy Value"), this, [val]() { QGuiApplication::clipboard()->setText(val); });
    m.addAction(tx("Copy Row"), this, [row]() { QGuiApplication::clipboard()->setText(row); });
    m.addAction(tx("View Value…"), this, [this, tag, val]() { show_free(new TextDialog(this, tag, val)); });
    if (meta_editable) {
        m.addSeparator();
        if (meta_row_editable(it))
            m.addAction(tx("Edit Value…"), this, [this, it]() { meta_edit(it); });
        auto sel = meta_selected();
        if (!sel.isEmpty())
            m.addAction(sel.size() > 1 ? txn("Remove %Ln Tags…", sel.size()) : tx("Remove Tag…"), this,
                        &PropertiesDialog::meta_remove);
        m.addAction(tx("Add Tag…"), this, &PropertiesDialog::meta_add);
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
        auto *btn = new QPushButton(tx("Compute"));
        row->addWidget(out, 1);
        row->addWidget(btn);
        form->addRow(name.toUpper() + ":", row);
        QString n = name;
        QCryptographicHash::Algorithm a = algo;
        connect(btn, &QPushButton::clicked, this, [this, n, a, out, btn]() {
            btn->setEnabled(false);
            out->setText(tx("Computing…"));
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
                [out](const QVariant &r) { out->setText(r.toString()); }, tx("Computing %1").arg(n.toUpper()));
        });
    }
    auto *verify = new QLineEdit;
    verify->setPlaceholderText(tx("Paste a checksum to compare…"));
    auto *result = new QLabel;
    connect(verify, &QLineEdit::textChanged, this, [outputs, result](const QString &text) {
        QString t = text.trimmed().toLower();
        bool match = false;
        for (QLineEdit *o : *outputs)
            match = match || o->text().toLower() == t;
        result->setText(t.isEmpty() ? QString() : (match ? tx("✔ Match") : tx("✘ No match (compute first)")));
    });
    form->addRow(tx("Verify:"), verify);
    form->addRow("", result);
    // or against a checksum file that lists it (SHA256SUMS, .md5, .sfv…)
    auto *hrow = new QHBoxLayout;
    hash_edit = new QLineEdit;
    hash_edit->setReadOnly(true);
    hash_edit->setPlaceholderText(tx("Verify against a checksum file…"));
    auto *browse = new QPushButton(tx("Browse…"));
    hrow->addWidget(hash_edit, 1);
    hrow->addWidget(browse);
    form->addRow(tx("Checksum file:"), hrow);
    hash_result = new QLabel;
    hash_result->setWordWrap(true);
    form->addRow("", hash_result);
    connect(browse, &QPushButton::clicked, this, [this]() {
        QString start = hash_edit->text().isEmpty() ? dirname(path) : dirname(hash_edit->text());
        choose_file(this, tx("Choose Checksum File"), start, hashcheck::file_filter(),
                    [this](const QString &f) { verify_against(f); });
    });
    return w;
}

void PropertiesDialog::verify_against(const QString &hash_path)
{
    hash_edit->setText(hash_path);
    hash_result->setToolTip(QString());
    auto said = [this](const QString &text, bool good) {
        hash_result->setText(text);
        hash_result->setStyleSheet(QString("color: %1").arg((good ? ok_color() : error_color()).name()));
    };
    hash_result->setStyleSheet(QString());
    hash_result->setText(tx("Checking…"));
    QPointer<QLabel> label(hash_result);
    QString file = path;
    // the checksum file is read on the job's thread too (it can be on a tape or floppy, which takes long to read)
    fileops::queue_job(
        this, tx("Verifying %1").arg(basename(path)), {file, hash_path},
        [hash_path, file](Task *t) -> QVariant {
            QList<hashcheck::Entry> entries = hashcheck::parse(hash_path);
            const hashcheck::Entry *found = hashcheck::find(entries, file);
            if (entries.isEmpty())
                return QVariantList{"not_checksums"};
            if (!found)
                return QVariantList{"not_listed"};
            hashcheck::Entry e = *found;
            hashcheck::Result r = hashcheck::verify(e, t);
            return QVariantList{r.status, r.actual, r.error, e.expected, hashcheck::algo_label(e.algo)};
        },
        [label, said, hash_path, file](const QVariant &res) {
            if (!label)
                return;
            QVariantList r = res.toList();
            QString status = r.value(0).toString(), algo = r.value(4).toString();
            if (!res.isValid())
                return said(tx("Stopped."), false);
            if (status == "not_checksums")
                return said(tx("✘ “%1” isn't a checksum file.").arg(basename(hash_path)), false);
            if (status == "not_listed")
                return said(tx("✘ “%1” isn't listed in it.").arg(basename(file)), false);
            if (status == "ok")
                return said(tx("✔ Matches (%1).").arg(algo), true);
            if (status == "failed") {
                label->setToolTip(tx("Expected: %1\nActual: %2").arg(r.value(3).toString(), r.value(1).toString()));
                return said(tx("✘ Doesn't match (%1).").arg(algo), false);
            }
            said("✘ " + (status == "missing" ? tx("Not found.") : r.value(2).toString()), false);
        });
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
                            owner, tx("Permissions"),
                            tx("You don't have permission to change the permissions of “%1”.").arg(basename(path)),
                            [p, m](Task *task) {
                                admin::session().call(task, "chmod", {{"path", p}, {"mode", int(m)}});
                                return QStringList();
                            });
                    } else {
                        warn(owner, tx("Permissions"), errno_text(e, path));   // owner: this may close below
                    }
                }
            }
        }
        QString new_name = name_edit->text().trimmed();
        if (!new_name.isEmpty() && new_name != basename(rstrip(path, '/'))) {
            auto res = dialogs::do_rename(path, new_name);
            if (!res.denied && res.error.isEmpty())
                undo::record("rename", TX_NOOP("Rename"), {qMakePair(path, join(dirname(path), new_name))});
            if (res.denied) {
                QString p = path, target = join(dirname(path), new_name);
                admin::retry_as_admin(owner, tx("Rename"),
                                      tx("You don't have permission to rename “%1”.").arg(basename(path)),
                                      [p, target](Task *task) {
                                          admin::session().call(task, "rename", {{"src", p}, {"dst", target}});
                                          return QStringList();
                                      });
            } else if (!res.error.isEmpty()) {
                warn(this, tx("Rename"), res.error);
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
    setWindowTitle(tx("Open With"));
    resize(420, 520);
    auto *lay = new QVBoxLayout(this);
    QString ct = content_type(paths.first());
    lay->addWidget(new QLabel(tx("Choose an application for “%1”\n<%2>").arg(basename(paths.first()), ct)));
    search = new QLineEdit;
    search->setPlaceholderText(tx("Search applications…"));
    connect(search, &QLineEdit::textChanged, this, &OpenWithDialog::filter);
    lay->addWidget(search);
    list = new QListWidget;
    connect(list, &QListWidget::itemDoubleClicked, this, [this]() { ok(); });
    lay->addWidget(list);
    auto [rec, others] = apps_for(paths.first());
    const QList<QPair<QString, QList<AppRef>>> groups = {{tx("Recommended"), rec}, {tx("Other applications"), others}};
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
    default_box = new QCheckBox(tx("Always use for this file type"));
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
        warn(parentWidget(), tx("Open With"), e.message());   // not this: it closes now
    }
    accept();
}

// ---------------------------------------------------------------- batch rename

BatchRenameDialog::BatchRenameDialog(QWidget *parent, const QStringList &paths) : QDialog(parent), paths(paths)
{
    setWindowTitle(tx("Rename %1 Items").arg(paths.size()));
    resize(700, 520);
    auto *lay = new QVBoxLayout(this);
    auto *modes = new QHBoxLayout;
    r_tmpl = new QRadioButton(tx("Rename using a template"));
    r_repl = new QRadioButton(tx("Find and replace text"));
    r_tmpl->setChecked(true);
    auto *grp = new QButtonGroup(this);
    grp->addButton(r_tmpl);
    grp->addButton(r_repl);
    modes->addWidget(r_tmpl);
    modes->addWidget(r_repl);
    lay->addLayout(modes);
    auto *form = new QFormLayout;
    tmpl = new QLineEdit("[Name] ###");
    tmpl->setToolTip(tx("[Name] = original name, # = number (### pads to 3 digits), [Date] = modified date"));
    start = new QSpinBox;
    start->setRange(0, 1000000);
    start->setValue(1);
    find = new QLineEdit;
    repl = new QLineEdit;
    regex = new QCheckBox(tx("Regular expression"));
    keep_ext = new QCheckBox(tx("Keep file extension"));
    keep_ext->setChecked(true);
    form->addRow(tx("Template:"), tmpl);
    form->addRow(tx("Start number:"), start);
    form->addRow(tx("Find:"), find);
    form->addRow(tx("Replace with:"), repl);
    form->addRow("", regex);
    form->addRow("", keep_ext);
    lay->addLayout(form);
    lay->addWidget(new QLabel(tx("<small>Template tokens: <b>[Name]</b> original name, <b>#</b> counter "
                              "(<b>###</b> = 001), <b>[Date]</b> modified date (YYYY-MM-DD)</small>")));
    table = new QTableWidget(0, 2);
    table->setHorizontalHeaderLabels({tx("Original"), tx("New name")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(table);
    status = new QLabel;
    lay->addWidget(status);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ok_btn = bb->button(QDialogButtonBox::Ok);
    ok_btn->setText(tx("Rename"));
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
    status->setText(problems ? txn("%Ln conflicting name(s)", problems) : QString());
    ok_btn->setEnabled(problems == 0);
}

void BatchRenameDialog::apply()
{
    // Two phases, so swaps and overlaps work: every item to a hidden temporary name, then each to its new name. Names
    // are only claimed, never replaced (put_new), so nothing that turns up meanwhile is overwritten. On a failure every
    // item is put back under its old name (lexists: a dangling symlink is an item too), and any that can't be are named.
    QStringList names = new_names();
    QList<QPair<QString, QString>> temps;   // (temporary, old)
    QStringList finals;                     // the new names given so far, in temps' order
    try {
        for (const QString &p : paths) {
            QString tmp = join(dirname(p), QString(".fe-rename-%1-%2").arg(::getpid()).arg(temps.size()));
            put_new(p, tmp);
            temps << qMakePair(tmp, p);
        }
        for (int i = 0; i < temps.size(); ++i) {
            QString target = join(dirname(temps[i].second), names[i]);
            put_new(temps[i].first, target);
            finals << target;
        }
    } catch (const OSError &e) {
        for (int i = finals.size() - 1; i >= 0; --i)   // first free the new names: one may be another item's old name
            if (lexists(finals[i]) && !lexists(temps[i].first))
                ::rename(enc(finals[i]).constData(), enc(temps[i].first).constData());
        QStringList stuck;
        for (const auto &[tmp, p] : temps) {
            if (!lexists(tmp))
                continue;
            try {
                put_new(tmp, p);
            } catch (const OSError &) {
                stuck << tx("%1 is now %2").arg(basename(p), basename(tmp));
            }
        }
        QString msg = e.message();
        if (!stuck.isEmpty())
            msg += "\n\n" + tx("These couldn't be put back under their old names:\n%1").arg(stuck.join('\n'));
        warn(this, tx("Rename"), msg);
        return;
    }
    QList<QPair<QString, QString>> renamed;
    for (int i = 0; i < temps.size(); ++i)
        if (basename(temps[i].second) != names[i])
            renamed << qMakePair(temps[i].second, finals[i]);
    undo::record("rename", txn("Rename %Ln Items", names.size()), renamed);
    accept();
}

// ---------------------------------------------------------------- free functions

namespace dialogs {

RenameResult do_rename(const QString &path, const QString &new_name)
{
    if (new_name.contains('/') || new_name == "." || new_name == "..")
        return {tx("Invalid name."), false};
    QString target = join(dirname(path), new_name);
    if (lexists(target) && realpath(target) != realpath(path))
        return {tx("“%1” already exists.").arg(new_name), false};
    try {
        util::rename(path, target);
    } catch (const OSError &e) {
        if (e.permission())
            return {QString(), true};   // callers offer to retry as administrator
        return {e.message(), false};
    }
    return {};
}

void ask_rename(QWidget *parent, const QString &path, std::function<void(const QString &)> renamed)
{
    auto *dlg = new QDialog(parent);
    bool is_dir = isdir(path);
    dlg->setWindowTitle(is_dir ? tx("Rename Folder") : tx("Rename File"));
    auto *lay = new QVBoxLayout(dlg);
    QString old = basename(path);
    auto *edit = new QLineEdit(old);
    edit->setMinimumWidth(380);
    lay->addWidget(edit);
    auto *err = new QLabel;
    err->setStyleSheet(QString("color: %1").arg(error_color().name()));
    lay->addWidget(err);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(tx("Rename"));
    lay->addWidget(bb);
    int stem_len = is_dir ? old.size() : split_ext(old).first.size();
    QTimer::singleShot(0, edit, [edit, stem_len]() { edit->setSelection(0, stem_len); });
    QObject::connect(edit, &QLineEdit::textChanged, dlg, [=](const QString &text) {
        QString t = text.trimmed();
        QString msg;
        if (t.isEmpty())
            msg = " ";
        else if (t.contains('/'))
            msg = tx("Names cannot contain “/”.");
        else if (t != old && lexists(join(dirname(path), t)))
            msg = tx("An item with that name already exists.");
        err->setText(msg.trimmed());
        bb->button(QDialogButtonBox::Ok)->setEnabled(msg.isEmpty());
    });
    QObject::connect(bb, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    QObject::connect(dlg, &QDialog::accepted, dlg, [edit, old, renamed]() {
        if (edit->text().trimmed() != old)
            renamed(edit->text().trimmed());
    });
    show_free(dlg);
}

void edit_bookmark(QWidget *parent, const QString &target, std::function<void()> saved)
{
    auto bms = read_bookmarks();
    int idx = -1;
    for (int i = 0; i < bms.size(); ++i)
        if (bms[i].first == target) {
            idx = i;
            break;
        }
    if (idx < 0)
        return;
    auto *d = new QDialog(parent);
    d->setWindowTitle(tx("Edit Bookmark"));
    d->setMinimumWidth(460);
    auto *form = new QFormLayout(d);
    auto *name = new QLineEdit(bms[idx].second);
    auto *path = new QLineEdit(bms[idx].first);
    auto *browse = new QPushButton(tx("Browse…"));
    QObject::connect(browse, &QPushButton::clicked, d, [d, path]() {
        QString start = path->text().trimmed();
        choose_folder(d, tx("Bookmark Location"), isdir(start) ? start : HOME(), [path](const QString &p) { path->setText(p); });
    });
    auto *row = new QHBoxLayout;
    row->addWidget(path, 1);
    row->addWidget(browse);
    form->addRow(tx("Name:"), name);
    form->addRow(tx("Location:"), row);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::rejected, d, &QDialog::reject);
    QObject::connect(bb, &QDialogButtonBox::accepted, d, [d, path]() {
        QString p = path->text().trimmed();
        if (p.startsWith('~'))
            p = expanduser(p);
        if (p.isEmpty()) {
            warn(d, tx("Edit Bookmark"), tx("Location can't be empty."));
            return;
        }
        if (!p.contains("://") && !isdir(p))
            ask_yes(d, tx("Edit Bookmark"), tx("“%1” is not an existing folder. Save anyway?").arg(p), [d]() { d->accept(); });
        else
            d->accept();
    });
    QObject::connect(d, &QDialog::accepted, d, [target, name, path, saved]() {
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
        auto now = read_bookmarks();   // as they are now: they may have changed while this was open
        for (auto &b : now)
            if (b.first == target) {
                b = qMakePair(p, label);
                write_bookmarks(now);
                if (saved)
                    saved();
                return;
            }
    });
    name->selectAll();
    show_free(d);
}

}  // namespace dialogs
