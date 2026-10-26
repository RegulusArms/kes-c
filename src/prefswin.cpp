#include "prefswin.h"

#include "archive.h"
#include "drives.h"
#include "fileops.h"
#include "hashcheck.h"
#include "jobs.h"
#include "keyring.h"
#include "menus.h"
#include "overview.h"
#include "prefs.h"
#include "tapebuf.h"
#include "thumbs.h"
#include "util.h"
#include "widgets.h"
#include "windows.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

using namespace util;

static QLabel *note(const QString &text)
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    l->setFont(f);
    l->setStyleSheet("color: palette(placeholder-text)");
    return l;
}

// system default / built-in viewer / any installed app that handles `ct`; item data is the setting's value
static QComboBox *opener_combo(const QString &ct, bool builtin)
{
    auto *cb = new QComboBox;
    AppRef def = default_app_for_type(ct);
    cb->addItem(def ? QString("System default (%1)").arg(app_name(def)) : QString("System default"), "system");
    if (builtin)
        cb->addItem(QGuiApplication::windowIcon(), "Kestrel image viewer", "builtin");
    for (const AppRef &a : apps_for_type(ct))
        cb->addItem(app_icon(a), app_name(a), app_id(a));
    return cb;
}

static void select_data(QComboBox *cb, const QVariant &v)
{
    int i = cb->findData(v);
    if (i < 0 && v.typeId() != QMetaType::QString)   // stored as text in the settings file ("1", "true")
        for (int j = 0; j < cb->count(); ++j)
            if (cb->itemData(j).toString() == v.toString())
                i = j;
    cb->setCurrentIndex(std::max(i, 0));
}

static const QMap<QString, QString> SECTION_NAMES = {{"places", "Places"}, {"bookmarks", "Bookmarks"}, {"devices", "Devices"}, {"mounts", "Mounts"}};

PreferencesDialog::PreferencesDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Preferences");
    setObjectName("preferences");
    auto *lay = new QVBoxLayout(this);
    search = new QLineEdit;
    search->setObjectName("search");
    search->setPlaceholderText("Search all settings");
    search->setClearButtonEnabled(true);
    lay->addWidget(search);
    tabs = new QTabWidget;
    lay->addWidget(tabs, 1);
    tab_names = prefs::tabs();
    for (const QString &tab : tab_names) {
        auto *page = new QWidget;
        auto *pl = new QVBoxLayout(page);
        QString group = "\x01";
        for (const prefs::Pref &p : prefs::all()) {
            if (p.tab != tab)
                continue;
            if (p.group != group) {
                group = p.group;
                if (!group.isEmpty()) {
                    auto *h = new QLabel("<b>" + group.toHtmlEscaped() + "</b>");
                    h->setContentsMargins(0, 10, 0, 2);
                    pl->addWidget(h);
                    headings[tab + "\n" + group] = h;
                }
            }
            pl->addWidget(make_row(p));
        }
        pl->addStretch(1);
        auto *scroll = new QScrollArea;
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        tabs->addTab(scroll, tab);
    }
    menu_editor = new menus::Editor;
    tabs->addTab(menu_editor, "Menus");
    tab_names << "Menus";

    auto *buttons = new QHBoxLayout;
    auto *reset_tab_btn = new QPushButton("Reset This Tab…");
    reset_tab_btn->setObjectName("reset_tab");
    auto *reset_all_btn = new QPushButton("Reset All…");
    reset_all_btn->setObjectName("reset_all");
    auto *close_btn = new QPushButton("Close");
    buttons->addWidget(reset_tab_btn);
    buttons->addWidget(reset_all_btn);
    buttons->addStretch(1);
    buttons->addWidget(close_btn);
    lay->addLayout(buttons);
    connect(close_btn, &QPushButton::clicked, this, &QDialog::close);
    connect(reset_tab_btn, &QPushButton::clicked, this, [this]() {
        int i = tabs->currentIndex();
        ask_yes(this, "Reset This Tab",
                QString("Put the settings on the %1 tab back to their defaults?").arg(tabs->tabText(i).section(" (", 0, 0)),
                [this, i]() { reset_tab(i); });
    });
    connect(reset_all_btn, &QPushButton::clicked, this, [this]() {
        ask_yes(this, "Reset All", "Put every setting, and the right-click menus, back to their defaults?",
                [this]() { reset_all(); });
    });
    connect(search, &QLineEdit::textChanged, this, &PreferencesDialog::apply_filter);
    connect(prefs::Notifier::instance(), &prefs::Notifier::changed, this,
            [this](const QStringList &keys) { refresh(keys); });
    refresh();
    resize(720, 640);
}

QWidget *PreferencesDialog::make_row(const prefs::Pref &p)
{
    auto *row = new QWidget;
    row->setObjectName("row:" + p.key);
    auto *grid = new QGridLayout(row);
    grid->setContentsMargins(0, 2, 0, 2);
    QWidget *ed = nullptr;
    if (p.kind == prefs::BOOL) {
        auto *b = new QCheckBox(p.label);
        connect(b, &QCheckBox::toggled, this, [this, key = p.key](bool on) { save(key, on); });
        grid->addWidget(b, 0, 0, 1, 2);
        ed = b;
    } else {
        auto *label = new QLabel(p.label + ":");
        label->setMinimumWidth(200);
        grid->addWidget(label, 0, 0, Qt::AlignTop);
        if (p.kind == prefs::INT) {
            auto *sp = new QSpinBox;
            sp->setRange(p.min, p.max);
            sp->setSuffix(p.suffix);
            connect(sp, &QSpinBox::valueChanged, this, [this, key = p.key](int v) { save(key, v); });
            ed = sp;
        } else if (p.kind == prefs::CHOICE) {
            auto *cb = new QComboBox;
            for (const auto &[text, value] : p.choices)
                cb->addItem(text, value);
            connect(cb, &QComboBox::activated, this, [this, cb, key = p.key](int i) { save(key, cb->itemData(i)); });
            ed = cb;
        } else {
            ed = make_custom(p);
        }
        grid->addWidget(ed, 0, 1);
        grid->setColumnStretch(1, 1);
    }
    if (ed->objectName().isEmpty())
        ed->setObjectName(p.key);
    if (!p.tip.isEmpty())
        ed->setToolTip(p.tip);
    if (!p.where.isEmpty())
        grid->addWidget(note("Also: " + p.where), 1, p.kind == prefs::BOOL ? 0 : 1, 1, p.kind == prefs::BOOL ? 2 : 1);
    editors[p.key] = ed;
    rows[p.key] = row;
    return row;
}

QWidget *PreferencesDialog::make_custom(const prefs::Pref &p)
{
    QString key = p.key;
    if (key == "homepage") {
        auto *box = new QWidget;
        auto *v = new QVBoxLayout(box);
        v->setContentsMargins(0, 0, 0, 0);
        auto *mode = new QComboBox;
        mode->setObjectName("homepage_mode");
        mode->addItems({"Overview (drives, network & bookmarks)", "Home folder", "Custom location…"});
        auto *h = new QHBoxLayout;
        auto *edit = new QLineEdit;
        edit->setObjectName("homepage_path");
        edit->setPlaceholderText("/path/to/folder  or  smb://server/share, sftp://user@host/path");
        auto *browse = new QPushButton("Browse…");
        h->addWidget(edit, 1);
        h->addWidget(browse);
        auto *err = new QLabel;
        err->setObjectName("homepage_error");
        err->setStyleSheet(QString("color: %1").arg(error_color().name()));
        err->hide();
        v->addWidget(mode);
        v->addLayout(h);
        v->addWidget(err);
        auto commit = [this, edit, err]() {
            QString c = edit->text().trimmed();
            if (c.startsWith('~'))
                c = expanduser(c);
            QString problem = c.isEmpty() ? QString("Enter a folder or network location.")
                              : c.startsWith('/') && !isdir(c) ? QString("“%1” is not a folder.").arg(c)
                                                               : QString();
            err->setText(problem);
            err->setVisible(!problem.isEmpty());
            if (problem.isEmpty())
                save("homepage", c);
        };
        connect(mode, &QComboBox::activated, this, [this, edit, browse, err, commit](int i) {
            edit->setEnabled(i == 2);
            browse->setEnabled(i == 2);
            err->hide();
            if (i < 2)
                save("homepage", i == 0 ? "overview" : "home");
            else if (!edit->text().trimmed().isEmpty())
                commit();
            else
                edit->setFocus();
        });
        connect(edit, &QLineEdit::editingFinished, this, commit);
        connect(browse, &QPushButton::clicked, this, [this, edit, commit]() {
            QString start = edit->text().startsWith('/') ? edit->text() : HOME();
            choose_folder(this, "Choose Homepage Folder", start, [edit, commit](const QString &d) {
                edit->setText(d);
                commit();
            });
        });
        loaders[key] = [mode, edit, browse, err]() {
            QString hp = prefs::get("homepage").toString();
            int i = hp == "overview" ? 0 : hp == "home" ? 1 : 2;
            mode->setCurrentIndex(i);
            if (i == 2)
                edit->setText(hp);
            edit->setEnabled(i == 2);
            browse->setEnabled(i == 2);
            err->hide();
        };
        return box;
    }
    if (key == "image_opener" || key == "video_opener") {
        auto *cb = opener_combo(key == "image_opener" ? "image/jpeg" : "video/mp4", key == "image_opener");
        connect(cb, &QComboBox::activated, this, [this, cb, key](int i) { save(key, cb->itemData(i)); });
        loaders[key] = [cb, key]() {
            QString cur = prefs::get(key).toString();
            if (cb->findData(cur) < 0)
                if (AppRef a = app_by_id(cur))
                    cb->addItem(app_icon(a), app_name(a), app_id(a));
            select_data(cb, cur);
        };
        return cb;
    }
    if (key == "folder_color") {
        auto *box = new QWidget;
        auto *h = new QHBoxLayout(box);
        h->setContentsMargins(0, 0, 0, 0);
        auto *btn = new QPushButton;
        btn->setObjectName("folder_color_button");
        auto *accent = new QCheckBox("Use the desktop's accent colour");
        accent->setObjectName("folder_color_accent");
        h->addWidget(btn);
        h->addWidget(accent, 1);
        auto paint = [btn, accent]() {
            QString s = prefs::get("folder_color").toString();
            QColor c = thumbs::follows_accent(s) ? accent_color() : QColor(s);
            btn->setStyleSheet(QString("background:%1; min-width:60px; min-height:20px").arg(c.name()));
            btn->setEnabled(!accent->isChecked());
        };
        connect(accent, &QCheckBox::toggled, this, [this](bool on) {
            save("folder_color", on ? QString("accent") : accent_color().name());
        });
        connect(btn, &QPushButton::clicked, this, [this]() {
            QString s = prefs::get("folder_color").toString();
            auto *d = new QColorDialog(thumbs::follows_accent(s) ? accent_color() : QColor(s), this);
            d->setWindowTitle("Folder colour");
            d->setOption(QColorDialog::DontUseNativeDialog);   // the desktop's own one would be modal
            connect(d, &QColorDialog::colorSelected, this, [this](const QColor &c) { save("folder_color", c.name()); });
            show_free(d);
        });
        loaders[key] = [accent, paint]() {
            accent->setChecked(thumbs::follows_accent(prefs::get("folder_color").toString()));
            paint();
        };
        return box;
    }
    if (key == "sidebar_sections") {
        auto *box = new QWidget;
        auto *h = new QHBoxLayout(box);
        h->setContentsMargins(0, 0, 0, 0);
        auto *list = new QListWidget;
        list->setObjectName("sidebar_sections_list");
        list->setMaximumHeight(110);
        auto *bv = new QVBoxLayout;
        auto *up = new QPushButton("Up"), *down = new QPushButton("Down");
        bv->addWidget(up);
        bv->addWidget(down);
        bv->addStretch(1);
        h->addWidget(list, 1);
        h->addLayout(bv);
        // each section, in order; ticked: open, unticked: collapsed
        auto store = [this, list]() {
            QStringList order, collapsed;
            for (int i = 0; i < list->count(); ++i) {
                QString id = list->item(i)->data(Qt::UserRole).toString();
                order << id;
                if (list->item(i)->checkState() != Qt::Checked)
                    collapsed << id;
            }
            if (!loading)
                prefs::set_many({{"sidebar_sections", order}, {"sidebar_collapsed", collapsed}});
        };
        connect(list, &QListWidget::itemChanged, this, store);
        auto move = [list, store](int by) {
            int r = list->currentRow();
            if (r < 0 || r + by < 0 || r + by >= list->count())
                return;
            QListWidgetItem *it = list->takeItem(r);
            list->insertItem(r + by, it);
            list->setCurrentRow(r + by);
            store();
        };
        connect(up, &QPushButton::clicked, this, [move]() { move(-1); });
        connect(down, &QPushButton::clicked, this, [move]() { move(1); });
        loaders[key] = [list]() {
            list->clear();
            QStringList collapsed = settings().value("sidebar_collapsed").toStringList();
            for (const QString &id : sidebar::section_order(settings().value("sidebar_sections").toStringList())) {
                auto *it = new QListWidgetItem(SECTION_NAMES.value(id, id) + "  (ticked: open; unticked: collapsed)");
                it->setData(Qt::UserRole, id);
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
                it->setCheckState(collapsed.contains(id) ? Qt::Unchecked : Qt::Checked);
                list->addItem(it);
            }
        };
        return box;
    }
    if (key == "overview_sections") {
        auto *box = new QWidget;
        auto *h = new QHBoxLayout(box);
        h->setContentsMargins(0, 0, 0, 0);
        auto *list = new QListWidget;
        list->setObjectName("overview_sections_list");
        list->setMaximumHeight(150);
        auto *bv = new QVBoxLayout;
        auto *up = new QPushButton("Up"), *down = new QPushButton("Down");
        bv->addWidget(up);
        bv->addWidget(down);
        bv->addStretch(1);
        h->addWidget(list, 1);
        h->addLayout(bv);
        // each section, in order; ticked: shown
        auto store = [this, list]() {
            QList<QPair<QString, bool>> order;
            for (int i = 0; i < list->count(); ++i)
                order << qMakePair(list->item(i)->data(Qt::UserRole).toString(),
                                   list->item(i)->checkState() == Qt::Checked);
            if (!loading)
                save("overview_sections", overview::save_order(order));
        };
        connect(list, &QListWidget::itemChanged, this, store);
        auto move = [list, store](int by) {
            int r = list->currentRow();
            if (r < 0 || r + by < 0 || r + by >= list->count())
                return;
            QListWidgetItem *it = list->takeItem(r);
            list->insertItem(r + by, it);
            list->setCurrentRow(r + by);
            store();
        };
        connect(up, &QPushButton::clicked, this, [move]() { move(-1); });
        connect(down, &QPushButton::clicked, this, [move]() { move(1); });
        loaders[key] = [list]() {
            list->clear();
            for (const auto &[id, shown] : overview::section_order(prefs::get("overview_sections").toStringList())) {
                QString title;
                for (const auto &[sid, stitle] : overview::SECTIONS)
                    if (sid == id)
                        title = stitle;
                auto *it = new QListWidgetItem(title);
                it->setData(Qt::UserRole, id);
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
                it->setCheckState(shown ? Qt::Checked : Qt::Unchecked);
                list->addItem(it);
            }
        };
        return box;
    }
    if (key == "slow_previews") {
        auto *box = new QWidget;
        auto *h = new QHBoxLayout(box);
        h->setContentsMargins(0, 0, 0, 0);
        auto *list = new QListWidget;
        list->setObjectName("slow_previews_list");
        list->setMaximumHeight(80);
        auto *remove = new QPushButton("Remove");
        h->addWidget(list, 1);
        h->addWidget(remove, 0, Qt::AlignTop);
        connect(remove, &QPushButton::clicked, this, [this, list]() {
            QStringList l = prefs::get("slow_previews").toStringList();
            for (QListWidgetItem *it : list->selectedItems())
                l.removeAll(it->text());
            save("slow_previews", l);
        });
        loaders[key] = [list, remove]() {
            list->clear();
            list->addItems(prefs::get("slow_previews").toStringList());
            remove->setEnabled(list->count() > 0);
            list->setToolTip(list->count() ? QString() : QString("None: no tape or floppy shows previews"));
        };
        return box;
    }
    if (key == "chooser_places") {   // a Save window's save locations (OverviewPage::save_places_section)
        auto *box = new QWidget;
        auto *h = new QHBoxLayout(box);
        h->setContentsMargins(0, 0, 0, 0);
        auto *list = new QListWidget;
        list->setObjectName("chooser_places_list");
        list->setMaximumHeight(110);
        auto *bv = new QVBoxLayout;
        auto *add = new QPushButton("Add…"), *remove = new QPushButton("Remove");
        auto *up = new QPushButton("Up"), *down = new QPushButton("Down");
        for (QPushButton *b : {add, remove, up, down})
            bv->addWidget(b);
        bv->addStretch(1);
        h->addWidget(list, 1);
        h->addLayout(bv);
        connect(add, &QPushButton::clicked, this, [this]() {
            QPointer<PreferencesDialog> self(this);
            choose_folder(this, "Add a Save Location", HOME(), [self](const QString &p) {
                QStringList l = prefs::get("chooser_places").toStringList();
                if (self && !l.contains(p))
                    self->save("chooser_places", l << p);
            });
        });
        connect(remove, &QPushButton::clicked, this, [this, list]() {
            QStringList l = prefs::get("chooser_places").toStringList();
            for (QListWidgetItem *it : list->selectedItems())
                l.removeAll(it->text());
            save("chooser_places", l);
        });
        auto move = [this, list](int by) {
            QStringList l = prefs::get("chooser_places").toStringList();
            int r = list->currentRow();
            if (r < 0 || r + by < 0 || r + by >= l.size())
                return;
            l.move(r, r + by);
            save("chooser_places", l);
            list->setCurrentRow(r + by);
        };
        connect(up, &QPushButton::clicked, this, [move]() { move(-1); });
        connect(down, &QPushButton::clicked, this, [move]() { move(1); });
        loaders[key] = [list, remove, up, down]() {
            int r = list->currentRow();
            list->clear();
            list->addItems(prefs::get("chooser_places").toStringList());
            list->setCurrentRow(std::min(r, list->count() - 1));
            for (QPushButton *b : {remove, up, down})
                b->setEnabled(list->count() > 0);
            list->setToolTip(list->count() ? QString() : QString("None yet: add the folders you save to most"));
        };
        return box;
    }
    if (key == "checksum_algorithms") {
        auto *box = new QWidget;
        auto *g = new QGridLayout(box);
        g->setContentsMargins(0, 0, 0, 0);
        QHash<QString, QCheckBox *> boxes;
        int i = 0;
        for (const QString &a : hashcheck::create_algorithms()) {
            auto *b = new QCheckBox(a == "crc32" ? QString("CRC32 (SFV)") : hashcheck::algo_label(a));
            b->setObjectName("algo_" + a);
            boxes[a] = b;
            g->addWidget(b, i / 3, i % 3);
            ++i;
        }
        auto store = [this, boxes]() {
            QStringList on;
            for (const QString &a : hashcheck::create_algorithms())
                if (boxes[a]->isChecked())
                    on << a;
            if (on.isEmpty()) {   // at least one
                load("checksum_algorithms");
                return;
            }
            save("checksum_algorithms", on.join(','));
        };
        for (QCheckBox *b : boxes)
            connect(b, &QCheckBox::toggled, this, store);
        loaders[key] = [boxes]() {
            QStringList on = prefs::get("checksum_algorithms").toString().split(',', Qt::SkipEmptyParts);
            for (auto it = boxes.begin(); it != boxes.end(); ++it)
                it.value()->setChecked(on.contains(it.key()));
        };
        return box;
    }
    if (key == "tape_buffer_now") {   // what the buffer would get now, from the memory figures
        auto *l = new QLabel;
        l->setObjectName("tape_buffer_now_label");
        l->setWordWrap(true);
        loaders[key] = [l]() {
            tapebuf::Memory m = tapebuf::memory();
            qint64 most = prefs::get("tape_buffer_gb").toLongLong() * 1000000000, b = tapebuf::budget(m);
            QString text = QString("Up to %1 (memory %2, %3 available now)")
                               .arg(human_size(b), human_size(m.total), human_size(m.available));
            if (most > 0 && b < most)   // the largest size isn't safe here: what's used instead, and why
                text += "\n" + human_size(most) + " is more than this system can spare now: the buffer takes half the "
                        "memory available at most.";
            l->setText(text);
        };
        return l;
    }
    if (key == "archive/format") {
        auto *cb = new QComboBox;
        for (const archive::Format &f : archive::formats())
            if (!f.available.isEmpty())
                cb->addItem(f.label, f.id);
        connect(cb, &QComboBox::activated, this, [this, cb](int i) { save("archive/format", cb->itemData(i)); });
        loaders[key] = [cb]() {
            QString cur = prefs::get("archive/format").toString();
            if (cur.isEmpty())
                cur = archive::tool("7z").isEmpty() ? "tar.gz" : "7z";
            select_data(cb, cur);
        };
        return cb;
    }
    if (key == "archive_formats") {
        // one format at a time: its program and the options Compress… starts with (archive/<format>/<option>)
        auto *box = new QWidget;
        auto *g = new QGridLayout(box);
        g->setContentsMargins(0, 0, 0, 0);
        auto *fmt = new QComboBox;
        fmt->setObjectName("archive_formats_format");
        QList<archive::Format> fmts;
        for (const archive::Format &f : archive::formats())
            if (!f.available.isEmpty()) {
                fmts << f;
                fmt->addItem(f.label, f.id);
            }
        auto *tool = new QComboBox;
        tool->setObjectName("archive_formats_tool");
        auto *level = new QSpinBox;
        level->setObjectName("archive_formats_level");
        auto *method = new QComboBox;
        auto *threads = new QSpinBox;
        threads->setRange(0, archive::cpu_count() * 2);
        threads->setSpecialValueText(QString("Auto (%1 cores)").arg(archive::cpu_count()));
        auto *solid = new QCheckBox("Solid archive");
        auto *enc_names = new QCheckBox("Encrypt file names when encrypting");
        auto *recovery = new QSpinBox;
        recovery->setRange(0, 10);
        recovery->setSuffix(" %");
        recovery->setSpecialValueText("None");
        auto *split = new QCheckBox("Split into volumes of");
        auto *vol = new QSpinBox;
        vol->setRange(1, 1000000);
        auto *unit = new QComboBox;
        unit->addItems({"MB", "GB"});
        auto *extra = new QLineEdit;
        extra->setPlaceholderText("Extra options for the program");
        auto *split_row = new QHBoxLayout;
        split_row->addWidget(split);
        split_row->addWidget(vol);
        split_row->addWidget(unit);
        split_row->addStretch(1);
        auto *level_lbl = new QLabel("Level:"), *method_lbl = new QLabel("Method:"), *threads_lbl = new QLabel("CPU threads:"),
             *recovery_lbl = new QLabel("Recovery record:");
        int r = 0;
        g->addWidget(fmt, r++, 0, 1, 2);
        g->addWidget(new QLabel("Program:"), r, 0);
        g->addWidget(tool, r++, 1);
        g->addWidget(level_lbl, r, 0);
        g->addWidget(level, r++, 1);
        g->addWidget(method_lbl, r, 0);
        g->addWidget(method, r++, 1);
        g->addWidget(threads_lbl, r, 0);
        g->addWidget(threads, r++, 1);
        g->addWidget(recovery_lbl, r, 0);
        g->addWidget(recovery, r++, 1);
        g->addWidget(solid, r++, 0, 1, 2);
        g->addWidget(enc_names, r++, 0, 1, 2);
        g->addLayout(split_row, r++, 0, 1, 2);
        g->addWidget(extra, r++, 0, 1, 2);
        auto current = [fmt, fmts]() -> archive::Format {
            for (const archive::Format &f : fmts)
                if (f.id == fmt->currentData().toString())
                    return f;
            return fmts.value(0);
        };
        auto k = [current](const QString &n) { return QString("archive/%1/%2").arg(current().id, n); };
        auto show = [=]() {   // the format's (and program's) options, as Compress… would start with them
            QSignalBlocker b1(tool), b2(level), b3(method), b4(threads), b5(solid), b6(enc_names), b7(recovery), b8(split),
                b9(vol), b10(unit), b11(extra);
            archive::Format f = current();
            QSettings &s = settings();
            QString t = s.value(k("tool")).toString();
            if (tool->property("format").toString() != f.id) {
                tool->clear();
                for (const QString &x : f.available)
                    tool->addItem(x, x);
                tool->setProperty("format", f.id);
            }
            if (tool->findData(t) >= 0)
                tool->setCurrentIndex(tool->findData(t));
            t = tool->currentData().toString();
            auto lv = archive::tool_levels(f, t);
            level->setVisible(bool(lv));
            level_lbl->setVisible(bool(lv));
            if (lv) {
                level->setRange(lv->min, lv->max);
                level->setValue(s.value(k("level_" + t), lv->def).toInt());
            }
            QStringList methods = t == "7z" ? f.methods : QStringList();
            method->clear();
            method->addItems(methods);
            method->setVisible(!methods.isEmpty());
            method_lbl->setVisible(!methods.isEmpty());
            if (!methods.isEmpty())
                method->setCurrentText(s.value(k("method"), methods.first()).toString());
            bool th = archive::tool_threads(f, t);
            threads->setVisible(th);
            threads_lbl->setVisible(th);
            threads->setValue(s.value(k("threads"), 0).toInt());
            solid->setVisible(f.solid);
            solid->setChecked(s.value(k("solid"), true).toBool());
            enc_names->setVisible(f.encrypt_names);
            enc_names->setChecked(s.value(k("encrypt_names"), true).toBool());
            recovery->setVisible(f.recovery);
            recovery_lbl->setVisible(f.recovery);
            recovery->setValue(s.value(k("recovery"), 0).toInt());
            bool vols = f.volumes && !(f.id == "zip" && t == "zip");
            split->setVisible(vols);
            vol->setVisible(vols);
            unit->setVisible(vols);
            split->setChecked(s.value(k("split"), false).toBool());
            vol->setValue(s.value(k("volume"), 4000).toInt());
            unit->setCurrentText(s.value(k("volume_unit"), "MB").toString());
            extra->setText(s.value(k("extra"), "").toString());
        };
        auto put = [this, k](const QString &n, const QVariant &v) { save(k(n), v); };
        connect(fmt, &QComboBox::activated, this, show);
        connect(tool, &QComboBox::activated, this, [=](int i) {
            put("tool", tool->itemData(i));
            show();
        });
        connect(level, &QSpinBox::valueChanged, this, [=](int v) { put("level_" + tool->currentData().toString(), v); });
        connect(method, &QComboBox::activated, this, [=]() { put("method", method->currentText()); });
        connect(threads, &QSpinBox::valueChanged, this, [=](int v) { put("threads", v); });
        connect(solid, &QCheckBox::toggled, this, [=](bool on) { put("solid", on); });
        connect(enc_names, &QCheckBox::toggled, this, [=](bool on) { put("encrypt_names", on); });
        connect(recovery, &QSpinBox::valueChanged, this, [=](int v) { put("recovery", v); });
        connect(split, &QCheckBox::toggled, this, [=](bool on) { put("split", on); });
        connect(vol, &QSpinBox::valueChanged, this, [=](int v) { put("volume", v); });
        connect(unit, &QComboBox::activated, this, [=]() { put("volume_unit", unit->currentText()); });
        connect(extra, &QLineEdit::editingFinished, this, [=]() { put("extra", extra->text()); });
        loaders[key] = show;
        return box;
    }
    if (key == "archive_passwords") {
        auto *forget = new QPushButton("Forget Saved Archive Passwords…");
        forget->setObjectName("forget_passwords");
        forget->setToolTip("Delete every archive password Kestrel saved in your login keyring");
        connect(forget, &QPushButton::clicked, this, [this]() {
            QPointer<QWidget> owner(parentWidget() ? parentWidget() : this);   // stays open after Preferences closes
            ask_yes(this, "Forget Saved Archive Passwords",
                    "Delete every archive password Kestrel saved in your login keyring? Encrypted archives will ask for "
                    "theirs again.",
                    [owner]() {
                        if (!owner)
                            return;
                        fileops::run_task(
                            owner, "Forgetting saved archive passwords", []() { return QVariant(keyring::forget_all()); },
                            [owner](const QVariant &r) {
                                if (!owner || !r.isValid())
                                    return;
                                int n = r.toInt();
                                inform(owner, "Forget Saved Archive Passwords",
                                       n < 0    ? QString("No keyring answered, so there was nothing to forget.")
                                       : n == 0 ? QString("There were no saved archive passwords.")
                                                : QString("Forgot %1 saved archive password%2.")
                                                      .arg(QString::number(n), n > 1 ? "s" : ""));
                            });
                    });
        });
        return forget;
    }
    return new QLabel("?");
}

void PreferencesDialog::save(const QString &key, const QVariant &value)
{
    if (loading)
        return;
    if (key == "jobs_mode") {   // the job queue tells every Kestrel itself
        jobs::set_mode(value.toString());
        prefs::notify({key});
    } else if (key == "jobs_start_paused") {
        jobs::Queue::instance()->set_start_paused(value.toBool());
        prefs::notify({key});
    } else {
        prefs::set(key, value);
    }
}

void PreferencesDialog::load(const QString &key)
{
    QWidget *ed = editors.value(key);
    const prefs::Pref *p = prefs::find(key);
    if (!ed || !p)
        return;
    bool was = loading;
    loading = true;
    QVariant v = prefs::get(key);
    if (auto *b = qobject_cast<QCheckBox *>(ed); b && p->kind == prefs::BOOL)
        b->setChecked(v.toBool());
    else if (auto *sp = qobject_cast<QSpinBox *>(ed); sp && p->kind == prefs::INT)
        sp->setValue(v.toInt());
    else if (auto *cb = qobject_cast<QComboBox *>(ed); cb && p->kind == prefs::CHOICE)
        select_data(cb, v);
    else if (loaders.contains(key))
        loaders[key]();
    if (key.startsWith("tape_buffer_") && loaders.contains("tape_buffer_now") && key != "tape_buffer_now")
        loaders["tape_buffer_now"]();   // follows the two settings
    if (key == "play_webm" && !which("ffmpeg")) {
        ed->setEnabled(false);
        ed->setToolTip("Needs ffmpeg:  sudo apt install ffmpeg");
    }
    loading = was;
}

void PreferencesDialog::refresh(const QStringList &keys)
{
    for (const prefs::Pref &p : prefs::all())
        if (keys.isEmpty() || keys.contains(p.key) || (p.key == "sidebar_sections" && keys.contains("sidebar_collapsed")) ||
            (p.key == "archive_formats" && std::any_of(keys.begin(), keys.end(), [](const QString &k) {
                 return k.startsWith("archive/") && k.count('/') == 2;
             })))
            load(p.key);
    if (keys.isEmpty() || std::any_of(keys.begin(), keys.end(), [](const QString &k) { return k.startsWith("menus/"); }))
        menu_editor->reload();
}

void PreferencesDialog::changeEvent(QEvent *ev)
{
    // coming back to it: show what was changed meanwhile (the Compress window, another Kestrel…)
    if (ev->type() == QEvent::ActivationChange && isActiveWindow())
        refresh();
    QDialog::changeEvent(ev);
}

void PreferencesDialog::apply_filter()
{
    QString t = search->text().trimmed();
    for (int i = 0; i < tab_names.size(); ++i) {
        const QString &tab = tab_names[i];
        int shown = 0;
        if (tab == "Menus") {
            QString words = "menus right-click context menu entries layout submenu reorder " +
                            QStringList{"folder", "file", "archive", "several", "empty space", "image viewer"}.join(' ');
            shown = t.isEmpty() || words.contains(t, Qt::CaseInsensitive);
        } else {
            QHash<QString, int> in_group;
            for (const prefs::Pref &p : prefs::all()) {
                if (p.tab != tab)
                    continue;
                QString hay = QStringList{p.label, p.where, p.tip, p.group, p.tab}.join('\n');
                if (p.kind == prefs::CHOICE)
                    for (const auto &c : p.choices)
                        hay += "\n" + c.first;
                bool match = t.isEmpty() || hay.contains(t, Qt::CaseInsensitive);
                rows[p.key]->setVisible(match);
                if (match) {
                    ++shown;
                    in_group[p.group]++;
                }
            }
            for (auto it = headings.begin(); it != headings.end(); ++it)
                if (it.key().startsWith(tab + "\n"))
                    it.value()->setVisible(in_group.value(it.key().section('\n', 1)) > 0);
        }
        tabs->setTabEnabled(i, shown > 0);
        tabs->setTabText(i, t.isEmpty() || shown == 0 ? tab : QString("%1 (%2)").arg(tab).arg(shown));
    }
    if (!tabs->isTabEnabled(tabs->currentIndex()))
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->isTabEnabled(i)) {
                tabs->setCurrentIndex(i);
                break;
            }
}

QStringList PreferencesDialog::keys_of(const QString &tab) const
{
    QStringList out;
    for (const prefs::Pref &p : prefs::all()) {
        if (p.tab != tab)
            continue;
        if (p.key == "tape_buffer_now")
            continue;
        if (p.key == "sidebar_sections")
            out << "sidebar_sections" << "sidebar_collapsed";
        else if (p.key == "archive_formats") {
            for (const QString &k : settings().allKeys())
                if (k.startsWith("archive/") && k.count('/') == 2)   // archive/<format>/<option>
                    out << k;
        } else if (p.key != "archive_passwords" && p.key != "jobs_mode" && p.key != "jobs_start_paused") {
            out << p.key;
        }
    }
    return out;
}

void PreferencesDialog::reset_tab(int index)
{
    QString tab = tab_names.value(index);
    if (tab == "Menus") {
        menus::reset_all();
        return;
    }
    if (tab == "Files") {   // the job queue's own: told by the queue
        jobs::set_mode("drive");
        jobs::Queue::instance()->set_start_paused(false);
    }
    prefs::reset(keys_of(tab));
    refresh();
}

void PreferencesDialog::reset_all()
{
    for (int i = 0; i < tab_names.size(); ++i)
        reset_tab(i);
}
