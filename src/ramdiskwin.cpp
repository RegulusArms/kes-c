#include "ramdiskwin.h"

#include "util.h"
#include "windows.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <unistd.h>

using namespace util;
using namespace ramdisk;

static qint64 ram_mb()
{
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    return pages > 0 && page > 0 ? qint64(pages) * page / (1024 * 1024) : 0;
}

RamDiskWindow::RamDiskWindow(QWidget *parent, const Disk *existing) : QDialog(parent), editing(existing)
{
    if (existing)
        before = *existing;
    setWindowTitle(existing ? tx("Edit RAM Disk “%1”").arg(existing->name) : tx("New RAM Disk"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *lay = new QVBoxLayout(this);
    auto *intro = new QLabel(tx("A RAM disk keeps its files in memory: very fast, and gone when it's taken down, unless "
                                "it saves them to disk."));
    intro->setWordWrap(true);
    lay->addWidget(intro);

    auto *form = new QFormLayout;
    name = new QLineEdit(existing ? existing->name : tx("RAM Disk"));
    form->addRow(tx("Name:"), name);

    limit = new QCheckBox(tx("A size limit of its own (needs your password to mount it)"));
    form->addRow(QString(), limit);
    auto *size_row = new QHBoxLayout;
    size = new QSpinBox;
    size->setAccessibleName(tx("Size"));
    unit = new QComboBox;
    unit->addItem(tx("MB"), 1);
    unit->addItem(tx("GB"), 1024);
    qint64 mb = existing && existing->size_mb ? existing->size_mb : 1024;
    bool gb = mb % 1024 == 0;
    unit->setCurrentIndex(gb ? 1 : 0);
    size->setRange(1, int(std::max<qint64>(1, ram_mb())));
    size->setValue(int(gb ? mb / 1024 : mb));
    size_row->addWidget(size);
    size_row->addWidget(unit);
    size_row->addStretch(1);
    form->addRow(tx("Size:"), size_row);
    location = new QLineEdit(existing && existing->kind == MOUNT ? existing->path : join(HOME(), tx("RAM Disk")));
    auto *loc_row = new QHBoxLayout;
    loc_row->addWidget(location, 1);
    auto *pick_loc = new QPushButton(tx("Choose…"));
    loc_row->addWidget(pick_loc);
    form->addRow(tx("Mounted on:"), loc_row);
    lay->addLayout(form);

    lay->addWidget(new QLabel(tx("How long it lasts:")));
    auto *group = new QButtonGroup(this);
    for (Life l : {EXIT, SESSION, PERMANENT, PERSISTENT}) {
        auto *r = new QRadioButton;
        r->setProperty("life", int(l));
        group->addButton(r);
        lives << r;
        lay->addWidget(r);
    }
    lives[existing ? existing->life : SESSION]->setChecked(true);

    auto *save_form = new QFormLayout;
    save_form->setContentsMargins(24, 0, 0, 0);
    save_to = new QLineEdit(existing ? existing->save_to : QString());
    save_to->setPlaceholderText(tx("A folder on disk"));
    auto *save_row = new QHBoxLayout;
    save_row->addWidget(save_to, 1);
    auto *pick_save = new QPushButton(tx("Choose…"));
    save_row->addWidget(pick_save);
    save_form->addRow(tx("Saved to:"), save_row);
    interval = new QSpinBox;
    interval->setRange(0, 24 * 60);
    interval->setValue(existing ? existing->interval : 15);
    interval->setSpecialValueText(tx("Only at logout and shutdown"));
    interval->setSuffix(tx(" min"));
    interval->setAccessibleName(tx("Also saved every"));
    save_form->addRow(tx("Also saved every:"), interval);
    lay->addLayout(save_form);

    note = new QLabel;
    note->setWordWrap(true);
    lay->addWidget(note);
    problem = new QLabel;
    problem->setWordWrap(true);
    problem->setStyleSheet("color: palette(link)");
    lay->addWidget(problem);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    ok = buttons->addButton(existing ? tx("Save") : tx("Create"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &RamDiskWindow::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    if (existing) {   // its kind and (a mounted one's) folder stay
        limit->setChecked(existing->kind == MOUNT);
        limit->setEnabled(false);
        location->setEnabled(false);
        pick_loc->setEnabled(false);
    }
    connect(pick_loc, &QPushButton::clicked, this, [this]() {
        choose_folder(this, tx("Mount the RAM Disk On"), dirname(location->text()), [this](const QString &f) {
            location->setText(f);
        });
    });
    connect(pick_save, &QPushButton::clicked, this, [this]() {
        choose_folder(this, tx("Save the RAM Disk's Files To"), save_to->text().isEmpty() ? HOME() : save_to->text(),
                      [this](const QString &f) { save_to->setText(f); });
    });
    for (QWidget *w : QList<QWidget *>{name, location, save_to})
        connect(qobject_cast<QLineEdit *>(w), &QLineEdit::textChanged, this, &RamDiskWindow::update);
    connect(limit, &QCheckBox::toggled, this, &RamDiskWindow::update);
    connect(size, &QSpinBox::valueChanged, this, &RamDiskWindow::update);
    connect(unit, &QComboBox::currentIndexChanged, this, &RamDiskWindow::update);
    connect(group, &QButtonGroup::buttonToggled, this, &RamDiskWindow::update);
    update();
}

Disk RamDiskWindow::chosen() const
{
    Disk d = before;
    if (!editing)
        d.id = new_id(disks());
    d.name = name->text().trimmed();
    d.kind = limit->isChecked() ? MOUNT : FOLDER;
    d.path = d.kind == MOUNT ? normpath(expanduser(location->text().trimmed())) : folder_path(d.name);
    d.size_mb = d.kind == MOUNT ? qint64(size->value()) * unit->currentData().toInt() : 0;
    for (QRadioButton *r : lives)
        if (r->isChecked())
            d.life = Life(r->property("life").toInt());
    d.save_to = d.life == PERSISTENT ? normpath(expanduser(save_to->text().trimmed())) : QString();
    d.interval = interval->value();
    return d;
}

QString RamDiskWindow::problem_text() const
{
    Disk d = chosen();
    QString p = name_problem(d.name);
    if (!p.isEmpty())
        return p;
    const QList<Disk> list = disks();
    for (const Disk &o : list)
        if (o.id != d.id && (o.name == d.name || o.path == d.path))
            return tx("There's a RAM disk called that already.");
    if (d.kind == FOLDER && d.path.isEmpty())
        return tx("RAM disks without a size limit need a folder of Kestrel's own in /dev/shm, and it can't be made.");
    bool moved = !editing || d.path != before.path;
    if (moved && d.kind == FOLDER && lexists(d.path))
        return tx("There's something called that in %1 already.").arg(dirname(d.path));
    if (d.kind == MOUNT) {
        if (!d.path.startsWith('/') || d.path == "/")
            return tx("Choose the folder to mount it on.");
        if (d.path.contains('%') || d.path.contains('\\') || d.path.contains('"'))
            return tx("The folder's path can't contain %, \\ or \".");
        if (!editing && lexists(d.path) && (!isdir(d.path) || !is_empty(d.path) || is_tmpfs_mount(d.path)))
            return tx("Mount it on a new folder, or an empty one.");
    }
    if (d.life == PERSISTENT) {
        if (!d.save_to.startsWith('/'))
            return tx("Choose the folder on disk its files are saved to.");
        if (d.save_to == d.path || d.save_to.startsWith(d.path + '/') || d.path.startsWith(d.save_to + '/'))
            return tx("The folder it's saved to must be outside the RAM disk (and not around it).");
        if (d.save_to.startsWith(shm_root() + '/') || is_tmpfs_mount(d.save_to))
            return tx("Save it to a folder on a disk, not in memory.");
    }
    if (needs_user_units(d) && !have_user_systemd())
        return tx("This needs systemd's user services (systemctl --user), which aren't running.");
    return QString();
}

void RamDiskWindow::update()
{
    bool mount = limit->isChecked();
    size->setEnabled(mount);
    unit->setEnabled(mount);
    if (!editing)
        location->setEnabled(mount);
    Kind k = mount ? MOUNT : FOLDER;
    for (QRadioButton *r : lives)
        r->setText(life_text(Life(r->property("life").toInt()), k));
    bool keeps = lives[PERSISTENT]->isChecked();
    save_to->setEnabled(keeps);
    interval->setEnabled(keeps);
    QString n = mount ? tx("It uses memory only for what's in it, up to its size. Mounting it (and changing or "
                           "removing it) asks for your password once.")
                      : tx("No password needed. It has no size limit of its own: it shares the memory set aside for "
                           "/dev/shm (half the RAM).");
    if (keeps)
        n += " " + tx("Its files are loaded from the folder on disk when it comes up, and saved there when you log out "
                      "or shut down, and as often as set here. A power cut loses what changed since the last save.");
    note->setText(n);
    QString p = problem_text();
    problem->setText(p);
    ok->setEnabled(p.isEmpty());
}

void RamDiskWindow::accept()
{
    if (!problem_text().isEmpty())
        return;
    Disk d = chosen();
    if (editing)
        ramdisk::change(parentWidget(), before, d);
    else
        ramdisk::create(parentWidget(), d);
    QDialog::accept();
}
