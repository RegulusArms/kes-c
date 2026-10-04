#include "overview.h"

#include "dialogs.h"
#include "fileops.h"
#include "places.h"
#include "proc.h"
#include "thumbs.h"
#include "util.h"

#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStorageInfo>
#include <QToolButton>
#include <QVBoxLayout>

#include <gio/gio.h>

#include <sys/sysmacros.h>

using namespace util;

const QString OVERVIEW = "overview://";
const QString OVERVIEW_TITLE = "Overview";

static const QSet<QString> LOCAL_FS = {"ext2", "ext3", "ext4", "xfs", "btrfs", "zfs", "vfat", "exfat", "ntfs",
                                       "ntfs3", "fuseblk", "f2fs", "iso9660", "udf", "hfsplus", "apfs", "bcachefs",
                                       "jfs", "reiserfs", "msdos"};
static const QSet<QString> NET_FS = {"cifs", "smb3", "smbfs", "nfs", "nfs4", "fuse.sshfs", "sshfs", "davfs",
                                     "fuse.rclone", "9p", "fuse.davfs2", "afs", "ceph", "glusterfs"};
static const QStringList HIDDEN_ROOTS = {"/boot", "/efi", "/snap", "/var/snap", "/var/lib/docker",
                                         "/var/lib/containers", "/run/", "/sys", "/proc", "/dev", "/tmp"};
// partitions that belong to a pool/array/LVM: GIO offers to "mount" them, but they can't be
static const QSet<QString> NON_MOUNTABLE_FS = {"zfs_member", "linux_raid_member", "LVM2_member", "swap",
                                               "ddf_raid_member", "isw_raid_member", "bcache", "ceph", "drbd",
                                               "btrfs_member"};

bool is_uri(const QString &target)
{
    static const QRegularExpression re("^[a-zA-Z][a-zA-Z0-9+.-]*://");
    // a network location to mount (smb://, sftp://, …), not a local path, the Overview, Starred or Recent
    return re.match(target).hasMatch() && !target.startsWith("file://") && target != OVERVIEW &&
           !places::is_virtual(target);
}

static QString gstr(char *s)
{
    QString r = s ? QString::fromUtf8(s) : QString();
    g_free(s);
    return r;
}

namespace overview {

static MountRef mount_ref(GMount *m)
{
    return m ? MountRef(m, [](GMount *x) { g_object_unref(x); }) : MountRef();
}

static VolumeRef volume_ref(GVolume *v)
{
    return v ? VolumeRef(v, [](GVolume *x) { g_object_unref(x); }) : VolumeRef();
}

QVariantList scan_filesystems()
{
    QHash<QString, QPair<qint64, qint64>> zfs_pools;
    if (which("zfs")) {
        auto r = proc::run({"zfs", "list", "-Hp", "-o", "name,used,avail", "-d", "0"}, 10000);
        for (const QString &line : QString::fromUtf8(r.out).split('\n', Qt::SkipEmptyParts)) {
            QStringList f = line.split('\t');
            if (f.size() == 3)
                zfs_pools[f[0]] = {f[1].toLongLong() + f[2].toLongLong(), f[2].toLongLong()};
        }
    }
    QMap<QString, QVariantMap> by_dev;
    for (const QStorageInfo &v : QStorageInfo::mountedVolumes()) {
        if (!v.isValid() || !v.isReady())
            continue;
        QString root = v.rootPath();
        QString fs = QString::fromUtf8(v.fileSystemType());
        QString dev = QString::fromUtf8(v.device());
        bool hidden = false;
        for (const QString &h : HIDDEN_ROOTS)
            hidden = hidden || root.startsWith(h);
        if (root != "/" && hidden && !root.startsWith("/run/media/"))
            continue;
        if (dev.startsWith("/dev/loop"))
            continue;
        bool net = NET_FS.contains(fs);
        if (!net && !LOCAL_FS.contains(fs))
            continue;
        qint64 total = v.bytesTotal(), free = v.bytesAvailable();
        QString key = dev;
        QString name = v.displayName() != root ? v.displayName() : basename(root);
        if (name.isEmpty())
            name = root;
        if (fs == "zfs") {
            QString pool = dev.split('/').first();
            key = "zfs:" + pool;
            if (zfs_pools.contains(pool)) {
                total = zfs_pools[pool].first;
                free = zfs_pools[pool].second;
            }
            name = pool;
        }
        QVariantMap entry{{"name", name}, {"root", root}, {"fs", fs}, {"device", dev}, {"total", total},
                          {"free", free}, {"kind", net ? "network" : (root == "/" ? "system" : "local")}};
        // one card per device/pool: keep the shallowest mount (skips bind mounts, subvolumes, nested datasets)
        auto prev = by_dev.find(key);
        if (prev == by_dev.end() || root.size() < prev->value("root").toString().size()) {
            if (prev != by_dev.end() && fs == "zfs")
                entry["name"] = prev->value("name");
            by_dev[key] = entry;
        }
    }
    QList<QVariantMap> out = by_dev.values();
    std::stable_sort(out.begin(), out.end(), [](const QVariantMap &a, const QVariantMap &b) {
        auto k = [](const QVariantMap &e) {
            return std::make_tuple(e["kind"].toString() != "system", e["kind"].toString() == "network",
                                   e["root"].toString());
        };
        return k(a) < k(b);
    });
    QVariantList res;
    for (QVariantMap &e : out) {
        if (e["root"].toString() == "/")
            e["name"] = "Computer";
        res << e;
    }
    return res;
}

// filesystem type from the udev database (no root, no subprocess)
static QString udev_fstype(const QString &dev)
{
    struct stat st;
    if (!stat_(dev, st))
        return QString();
    bool ok = false;
    QByteArray data = read_file(QString("/run/udev/data/b%1:%2").arg(major(st.st_rdev)).arg(minor(st.st_rdev)), &ok);
    for (const QByteArray &line : data.split('\n'))
        if (line.startsWith("E:ID_FS_TYPE="))
            return QString::fromUtf8(line.mid(13)).trimmed();
    return QString();
}

static qint64 sysfs_size(const QString &dev)
{
    bool ok = false;
    QByteArray data = read_file(QString("/sys/class/block/%1/size").arg(basename(realpath(dev))), &ok);
    qint64 n = data.trimmed().toLongLong(&ok);
    return ok ? n * 512 : 0;
}

// ---------------------------------------------------------------- GIO mount helpers

// A GMountOperation that asks for passwords/questions with Qt dialogs.
static void ask_password(GMountOperation *op, const char *message, const char *default_user,
                         const char *default_domain, GAskPasswordFlags flags, gpointer data)
{
    QWidget *parent = static_cast<QPointer<QWidget> *>(data)->data();
    QDialog dlg(parent);
    dlg.setWindowTitle("Authentication Required");
    auto *form = new QFormLayout(&dlg);
    auto *msg = new QLabel(QString::fromUtf8(message));
    msg->setWordWrap(true);
    form->addRow(msg);
    QCheckBox *anon = nullptr;
    QLineEdit *user = nullptr, *domain = nullptr, *pw = nullptr;
    QComboBox *save = nullptr;
    if (flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED) {
        anon = new QCheckBox("Connect anonymously");
        form->addRow(anon);
    }
    if (flags & G_ASK_PASSWORD_NEED_USERNAME) {
        user = new QLineEdit(default_user && *default_user ? QString::fromUtf8(default_user)
                                                           : QString::fromLocal8Bit(qgetenv("USER")));
        form->addRow("Username:", user);
    }
    if (flags & G_ASK_PASSWORD_NEED_DOMAIN) {
        domain = new QLineEdit(default_domain && *default_domain ? QString::fromUtf8(default_domain) : "WORKGROUP");
        form->addRow("Domain:", domain);
    }
    if (flags & G_ASK_PASSWORD_NEED_PASSWORD) {
        pw = new QLineEdit;
        pw->setEchoMode(QLineEdit::Password);
        form->addRow("Password:", pw);
    }
    if (flags & G_ASK_PASSWORD_SAVING_SUPPORTED) {
        save = new QComboBox;
        save->addItems({"Forget password immediately", "Remember until you log out", "Remember forever"});
        save->setCurrentIndex(1);
        form->addRow("", save);
    }
    if (anon)
        QObject::connect(anon, &QCheckBox::toggled, &dlg, [=](bool on) {
            for (QWidget *w : std::initializer_list<QWidget *>{user, domain, pw, save})
                if (w)
                    w->setEnabled(!on);
        });
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText("Connect");
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);
    if (pw)
        pw->setFocus();
    else if (user)
        user->setFocus();
    if (!dlg.exec()) {
        g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
        return;
    }
    if (anon && anon->isChecked()) {
        g_mount_operation_set_anonymous(op, TRUE);
    } else {
        if (user)
            g_mount_operation_set_username(op, user->text().toUtf8().constData());
        if (domain)
            g_mount_operation_set_domain(op, domain->text().toUtf8().constData());
        if (pw)
            g_mount_operation_set_password(op, pw->text().toUtf8().constData());
        if (save) {
            const GPasswordSave modes[] = {G_PASSWORD_SAVE_NEVER, G_PASSWORD_SAVE_FOR_SESSION,
                                           G_PASSWORD_SAVE_PERMANENTLY};
            g_mount_operation_set_password_save(op, modes[save->currentIndex()]);
        }
    }
    g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
}

static void ask_question(GMountOperation *op, const char *message, const char **choices, gpointer data)
{
    QWidget *parent = static_cast<QPointer<QWidget> *>(data)->data();
    QMessageBox box(QMessageBox::Question, "Question", QString::fromUtf8(message), QMessageBox::NoButton, parent);
    QList<QAbstractButton *> buttons;
    for (const char **c = choices; c && *c; ++c)
        buttons << box.addButton(QString::fromUtf8(*c), QMessageBox::AcceptRole);
    box.exec();
    int i = buttons.indexOf(box.clickedButton());
    if (i >= 0) {
        g_mount_operation_set_choice(op, i);
        g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
    } else {
        g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
    }
}

static GMountOperation *make_mount_operation(QWidget *parent)
{
    GMountOperation *op = g_mount_operation_new();
    auto *ptr = new QPointer<QWidget>(parent);
    g_object_set_data_full(G_OBJECT(op), "kes-parent", ptr, [](gpointer p) { delete static_cast<QPointer<QWidget> *>(p); });
    g_signal_connect(op, "ask-password", G_CALLBACK(ask_password), ptr);
    g_signal_connect(op, "ask-question", G_CALLBACK(ask_question), ptr);
    return op;
}

struct PathCallback {
    GMountOperation *op;
    std::function<void(const QString &, const QString &)> done;
};

struct ErrCallback {
    GMountOperation *op;
    bool eject;
    std::function<void(const QString &)> done;
};

static QString take_error(GError *e)
{
    QString m = QString::fromUtf8(e->message);
    g_error_free(e);
    return m;
}

void mount_uri(QWidget *parent, const QString &uri, std::function<void(const QString &, const QString &)> on_done)
{
    GFile *f = g_file_new_for_uri(uri.toUtf8().constData());
    GMount *existing = g_file_find_enclosing_mount(f, nullptr, nullptr);
    if (existing) {
        g_object_unref(existing);
        QString path = gstr(g_file_get_path(f));
        g_object_unref(f);
        on_done(path, path.isEmpty() ? "Mounted, but no local (FUSE) path is available" : QString());
        return;
    }
    auto *cb = new PathCallback{make_mount_operation(parent), std::move(on_done)};
    g_file_mount_enclosing_volume(
        f, G_MOUNT_MOUNT_NONE, cb->op, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
            auto *cb = static_cast<PathCallback *>(data);
            GFile *file = G_FILE(src);
            GError *err = nullptr;
            bool failed = false;
            QString message;
            if (!g_file_mount_enclosing_volume_finish(file, res, &err)) {
                if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
                    failed = true;
                    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
                        message = QString::fromUtf8(err->message);
                }
                g_error_free(err);
            }
            if (failed) {
                cb->done(QString(), message);
            } else {
                QString path = gstr(g_file_get_path(file));
                cb->done(path, path.isEmpty() ? "Mounted, but no local (FUSE) path is available" : QString());
            }
            g_object_unref(cb->op);
            g_object_unref(file);
            delete cb;
        },
        cb);
}

void mount_volume(QWidget *parent, const VolumeRef &volume, std::function<void(const QString &, const QString &)> on_done)
{
    auto *cb = new PathCallback{make_mount_operation(parent), std::move(on_done)};
    g_object_ref(volume.get());
    g_volume_mount(
        volume.get(), G_MOUNT_MOUNT_NONE, cb->op, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
            auto *cb = static_cast<PathCallback *>(data);
            GVolume *v = G_VOLUME(src);
            GError *err = nullptr;
            if (!g_volume_mount_finish(v, res, &err)) {
                QString msg = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED) ? QString()
                                                                                          : QString::fromUtf8(err->message);
                g_error_free(err);
                cb->done(QString(), msg);
            } else {
                GMount *m = g_volume_get_mount(v);
                QString path;
                if (m) {
                    GFile *root = g_mount_get_root(m);
                    path = gstr(g_file_get_path(root));
                    g_object_unref(root);
                    g_object_unref(m);
                }
                cb->done(path, QString());
            }
            g_object_unref(cb->op);
            g_object_unref(v);
            delete cb;
        },
        cb);
}

void unmount(QWidget *parent, const MountRef &mount, std::function<void(const QString &)> on_done)
{
    bool eject = g_mount_can_eject(mount.get());
    auto *cb = new ErrCallback{make_mount_operation(parent), eject, std::move(on_done)};
    g_object_ref(mount.get());
    auto finished = [](GObject *src, GAsyncResult *res, gpointer data) {
        auto *cb = static_cast<ErrCallback *>(data);
        GMount *m = G_MOUNT(src);
        GError *err = nullptr;
        bool ok = cb->eject ? g_mount_eject_with_operation_finish(m, res, &err)
                            : g_mount_unmount_with_operation_finish(m, res, &err);
        cb->done(ok ? QString() : take_error(err));
        g_object_unref(cb->op);
        g_object_unref(m);
        delete cb;
    };
    if (eject)
        g_mount_eject_with_operation(mount.get(), G_MOUNT_UNMOUNT_NONE, cb->op, nullptr, finished, cb);
    else
        g_mount_unmount_with_operation(mount.get(), G_MOUNT_UNMOUNT_NONE, cb->op, nullptr, finished, cb);
}

}  // namespace overview

// ---------------------------------------------------------------- widgets

FlowLayout::FlowLayout(QWidget *parent, int spacing) : QLayout(parent)
{
    setSpacing(spacing);
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem *it = takeAt(0))
        delete it;
}

void FlowLayout::addItem(QLayoutItem *item) { items << item; }
int FlowLayout::count() const { return items.size(); }
QLayoutItem *FlowLayout::itemAt(int i) const { return i >= 0 && i < items.size() ? items[i] : nullptr; }
QLayoutItem *FlowLayout::takeAt(int i) { return i >= 0 && i < items.size() ? items.takeAt(i) : nullptr; }
Qt::Orientations FlowLayout::expandingDirections() const { return {}; }
bool FlowLayout::hasHeightForWidth() const { return true; }
int FlowLayout::heightForWidth(int w) const { return do_layout(QRect(0, 0, w, 0), true); }

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    do_layout(rect, false);
}

QSize FlowLayout::sizeHint() const { return minimumSize(); }

QSize FlowLayout::minimumSize() const
{
    QSize s;
    for (QLayoutItem *it : items)
        s = s.expandedTo(it->minimumSize());
    return s;
}

int FlowLayout::do_layout(const QRect &rect, bool test) const
{
    int x = rect.x(), y = rect.y(), line_h = 0;
    int sp = spacing();
    for (QLayoutItem *it : items) {
        QSize hint = it->sizeHint();
        if (x + hint.width() > rect.right() + 1 && line_h > 0) {
            x = rect.x();
            y += line_h + sp;
            line_h = 0;
        }
        if (!test)
            it->setGeometry(QRect(QPoint(x, y), hint));
        x += hint.width() + sp;
        line_h = std::max(line_h, hint.height());
    }
    return y + line_h - rect.y();
}

Card::Card(QWidget *parent) : QFrame(parent)
{
    setObjectName("card");
    setCursor(Qt::PointingHandCursor);
    setStyleSheet("QFrame#card { background: palette(base); border: 1px solid palette(midlight); border-radius: 10px; }"
                  "QFrame#card:hover { border: 1px solid palette(highlight); }");
}

void Card::mouseReleaseEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton && rect().contains(ev->position().toPoint()))
        Q_EMIT clicked();
    else if (ev->button() == Qt::MiddleButton)
        Q_EMIT middle_clicked();
}

void Card::contextMenuEvent(QContextMenuEvent *ev) { Q_EMIT menu_requested(ev->globalPos()); }

static QLabel *small(QLabel *label, bool dim = true)
{
    QFont f = label->font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    label->setFont(f);
    if (dim)
        label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

DriveCard::DriveCard(const DriveInfo &info_, QWidget *parent) : Card(parent), info(info_)
{
    setFixedSize(340, 96);
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    auto *icon = new QLabel;
    icon->setPixmap(info.icon.pixmap(48, 48));
    icon->setFixedSize(52, 52);
    lay->addWidget(icon, 0, Qt::AlignVCenter);
    auto *col = new QVBoxLayout;
    col->setSpacing(3);
    auto *top = new QHBoxLayout;
    auto *name = new QLabel(info.name);
    QFont f = name->font();
    f.setBold(true);
    name->setFont(f);
    top->addWidget(name, 1);
    if (!info.action.isEmpty()) {
        auto *b = new QToolButton;
        b->setAutoRaise(true);
        b->setIcon(theme_icon(info.action_icon));
        b->setToolTip(info.action);
        auto action = info.on_action;
        connect(b, &QToolButton::clicked, this, [action]() { action(); });
        top->addWidget(b);
    }
    col->addLayout(top);
    QStringList sub;
    QString where = !info.root.isEmpty() ? info.root : info.device;
    if (!where.isEmpty())
        sub << where;
    if (!info.fs.isEmpty())
        sub << info.fs;
    col->addWidget(small(new QLabel(sub.join("  ·  "))));
    qint64 total = info.total;
    if (info.mounted && total) {
        qint64 free = info.free < 0 ? 0 : info.free;
        double pct = (total - free) * 100.0 / total;
        auto *bar = new QProgressBar;
        bar->setRange(0, 1000);
        bar->setValue(int(pct * 10));
        bar->setTextVisible(false);
        bar->setFixedHeight(8);
        QString color = pct >= 90 ? "#c01c28" : (pct >= 75 ? "#e5a50a" : "palette(highlight)");
        bar->setStyleSheet(QString("QProgressBar { border: none; border-radius: 4px; background: palette(midlight); }"
                                   "QProgressBar::chunk { border-radius: 4px; background: %1; }")
                               .arg(color));
        col->addWidget(bar);
        col->addWidget(small(new QLabel(QString("%1 free of %2  (%3% used)")
                                            .arg(human_size(free), human_size(total))
                                            .arg(qRound(pct)))));
    } else {
        QString state = !info.status.isEmpty() ? info.status
                                                : (!info.mounted ? QString("Not mounted — click to mount") : QString());
        if (total)
            state = QString("%1  ·  %2").arg(human_size(total), state);
        col->addWidget(small(new QLabel(state)));
    }
    col->addStretch(1);
    lay->addLayout(col, 1);
}

BookmarkCard::BookmarkCard(const QString &target, const QString &label, ThumbnailManager *thumbs, QWidget *parent)
    : Card(parent), target(target), thumbs(thumbs)
{
    setFixedSize(168, 196);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 8);
    lay->setSpacing(4);
    pic = new QLabel;
    pic->setFixedSize(148, 128);
    pic->setAlignment(Qt::AlignCenter);
    lay->addWidget(pic);
    auto *name = new QLabel(label);
    QFont f = name->font();
    f.setBold(true);
    name->setFont(f);
    name->setAlignment(Qt::AlignHCenter);
    name->setText(name->fontMetrics().elidedText(label, Qt::ElideMiddle, 148));
    lay->addWidget(name);
    auto *where = small(new QLabel);
    where->setAlignment(Qt::AlignHCenter);
    where->setText(where->fontMetrics().elidedText(target, Qt::ElideLeft, 148));
    where->setToolTip(target);
    lay->addWidget(where);
    local = !is_uri(target);
    update_pic();
}

void BookmarkCard::update_pic()
{
    QString icon = "folder-remote";
    if (local) {
        if (isdir(target)) {
            QFileInfo fi(target);
            QPixmap pm = thumbs->folder_pixmap(target, fi.lastModified().toSecsSinceEpoch(), 128);
            if (!pm.isNull()) {
                pic->setPixmap(thumbs->scaled(pm, 128));
                return;
            }
            icon = special_dir_icons().value(target, "folder");
        } else {
            bool remote = target.startsWith("/run/user") || target.startsWith("/media") || target.startsWith("/mnt");
            icon = remote ? "folder-remote" : "dialog-question";
        }
    }
    pic->setPixmap(theme_icon({icon, "folder"}).pixmap(96, 96));
}

// ---------------------------------------------------------------- page

OverviewPage::OverviewPage(ThumbnailManager *thumbs, QWidget *parent) : QScrollArea(parent), thumbs(thumbs)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    body = new QWidget;
    body->setAutoFillBackground(true);
    setWidget(body);
    lay = new QVBoxLayout(body);
    lay->setContentsMargins(24, 18, 24, 24);
    lay->setSpacing(10);
    refresh_timer.setSingleShot(true);
    refresh_timer.setInterval(300);
    connect(&refresh_timer, &QTimer::timeout, this, &OverviewPage::refresh);
    usage_timer.setInterval(15000);
    connect(&usage_timer, &QTimer::timeout, this, &OverviewPage::refresh);
    connect(thumbs, &ThumbnailManager::updated, this, &OverviewPage::thumb_ready);
    monitor = g_volume_monitor_get();
    for (const char *sig : {"volume-added", "volume-removed", "volume-changed", "mount-added", "mount-removed",
                            "mount-changed"})
        g_signal_connect(monitor, sig, G_CALLBACK(&OverviewPage::monitor_changed), this);
}

OverviewPage::~OverviewPage()
{
    if (monitor) {
        g_signal_handlers_disconnect_by_data(monitor, this);
        g_object_unref(monitor);
    }
}

void OverviewPage::monitor_changed(void *, void *, void *self)
{
    auto *page = static_cast<OverviewPage *>(self);
    if (page->isVisible())
        page->refresh_timer.start();
}

void OverviewPage::showEvent(QShowEvent *ev)
{
    QScrollArea::showEvent(ev);
    refresh();
    usage_timer.start();
}

void OverviewPage::hideEvent(QHideEvent *ev)
{
    QScrollArea::hideEvent(ev);
    usage_timer.stop();
}

void OverviewPage::refresh()
{
    if (fs_running)
        return;
    if (!have_fs)
        rebuild();   // show bookmarks / GIO volumes immediately
    fs_running = true;
    Task *t = fileops::run_task(
        this, "", []() { return QVariant(overview::scan_filesystems()); },
        [this](const QVariant &res) {
            fs_running = false;
            fs = res.toList();
            have_fs = true;
            rebuild();
        },
        true);
    connect(t, &Task::error, this, [this]() { fs_running = false; });
}

void OverviewPage::clear_layout(QLayout *l)
{
    while (QLayoutItem *it = l->takeAt(0)) {
        if (it->widget())
            it->widget()->deleteLater();
        else if (it->layout())
            clear_layout(it->layout());
        delete it;
    }
}

FlowLayout *OverviewPage::section(const QString &title)
{
    auto *lab = new QLabel(title);
    QFont f = lab->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 1.15);
    lab->setFont(f);
    lay->addSpacing(6);
    lay->addWidget(lab);
    auto *box = new QWidget;
    auto *flow = new FlowLayout(box);
    lay->addWidget(box);
    return flow;
}

void OverviewPage::rebuild()
{
    int scroll = verticalScrollBar()->value();
    clear_layout(lay);
    bookmark_cards.clear();
    auto [local, network] = drive_infos();
    FlowLayout *flow = section("Drives");
    for (const DriveInfo &info : local)
        flow->addWidget(drive_card(info));
    if (local.isEmpty())
        flow->addWidget(small(new QLabel("Scanning…")));
    FlowLayout *nflow = section("Network");
    for (const DriveInfo &info : network)
        nflow->addWidget(drive_card(info));
    if (network.isEmpty())
        nflow->addWidget(small(new QLabel("No network locations connected.")));
    lay->addLayout(connect_row());
    QStringList recents = settings().value("recent_servers").toStringList();
    if (!recents.isEmpty()) {
        auto *row = new QHBoxLayout;
        row->addWidget(small(new QLabel("Recent:")));
        for (const QString &uri : recents.mid(0, 6)) {
            auto *b = new QPushButton(uri);
            b->setFlat(true);
            b->setCursor(Qt::PointingHandCursor);
            connect(b, &QPushButton::clicked, this, [this, uri]() { Q_EMIT navigate(uri); });
            row->addWidget(b);
        }
        row->addStretch(1);
        lay->addLayout(row);
    }
    FlowLayout *bflow = section("Bookmarks");
    auto bms = read_bookmarks();
    for (const auto &[target, label] : bms) {
        auto *card = new BookmarkCard(target, label, thumbs);
        QString t = target;
        connect(card, &Card::clicked, this, [this, t]() { Q_EMIT navigate(t); });
        connect(card, &Card::middle_clicked, this, [this, t]() { Q_EMIT open_in_tab(t); });
        connect(card, &Card::menu_requested, this, [this, t](const QPoint &pos) { bookmark_menu(t, pos); });
        bflow->addWidget(card);
        bookmark_cards.insert(target, card);
    }
    if (bms.isEmpty())
        lay->addWidget(small(new QLabel("No bookmarks yet — press Ctrl+D in a folder, or right-click a folder and "
                                        "choose Links & Shortcuts → Add to Bookmarks.")));
    lay->addStretch(1);
    QTimer::singleShot(0, this, [this, scroll]() { verticalScrollBar()->setValue(scroll); });
}

QPair<QList<DriveInfo>, QList<DriveInfo>> OverviewPage::drive_infos()
{
    QHash<QString, QVariantMap> roots;
    for (const QVariant &v : fs) {
        QVariantMap e = v.toMap();
        roots.insert(e["root"].toString(), e);
    }
    QList<DriveInfo> local, network;
    QHash<QString, overview::MountRef> gio_by_root;
    GList *mounts = g_volume_monitor_get_mounts(monitor);
    for (GList *l = mounts; l; l = l->next) {
        overview::MountRef m = overview::mount_ref(G_MOUNT(l->data));
        GFile *root = g_mount_get_root(m.get());
        QString path = gstr(g_file_get_path(root));
        QString scheme = gstr(g_file_get_uri_scheme(root));
        if (!path.isEmpty() && roots.contains(path)) {
            gio_by_root.insert(path, m);
        } else if (scheme != "file" || path.contains("/gvfs/")) {
            // gvfs network mount (smb, sftp, ...)
            DriveInfo info;
            info.name = gstr(g_mount_get_name(m.get()));
            info.root = path;
            info.uri = gstr(g_file_get_uri(root));
            info.fs = scheme;
            GIcon *gi = g_mount_get_icon(m.get());
            info.icon = gicon_to_qicon(gi);
            g_object_unref(gi);
            info.mount = m;
            info.status = "Connected";
            info.kind = "network";
            network << info;
        }
        g_object_unref(root);
    }
    g_list_free(mounts);
    for (const QVariant &v : fs) {
        QVariantMap e = v.toMap();
        QString root = e["root"].toString(), kind = e["kind"].toString();
        QString icon = kind == "system"    ? "drive-harddisk-system"
                       : kind == "network" ? "folder-remote"
                       : (root.startsWith("/media/") || root.startsWith("/run/media/")) ? "drive-removable-media"
                                                                                        : "drive-harddisk";
        DriveInfo info;
        info.name = e["name"].toString();
        info.root = root;
        info.fs = e["fs"].toString();
        info.device = e["device"].toString();
        info.total = e["total"].toLongLong();
        info.free = e["free"].toLongLong();
        info.kind = kind;
        info.icon = theme_icon({icon, "drive-harddisk"});
        info.mounted = true;
        overview::MountRef m = gio_by_root.value(root);
        if (m) {
            QString n = gstr(g_mount_get_name(m.get()));
            if (!n.isEmpty())
                info.name = n;
            GIcon *gi = g_mount_get_icon(m.get());
            QIcon qi = gicon_to_qicon(gi);
            g_object_unref(gi);
            if (!qi.isNull())
                info.icon = qi;
            if (g_mount_can_unmount(m.get()))
                info.mount = m;
        }
        (kind == "network" ? network : local) << info;
    }
    // unmounted volumes (GIO)
    GList *volumes = g_volume_monitor_get_volumes(monitor);
    for (GList *l = volumes; l; l = l->next) {
        overview::VolumeRef v = overview::volume_ref(G_VOLUME(l->data));
        GMount *vm = g_volume_get_mount(v.get());
        if (vm) {
            g_object_unref(vm);
            continue;
        }
        if (!g_volume_can_mount(v.get()))
            continue;
        QString dev = gstr(g_volume_get_identifier(v.get(), G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
        QString fstype = dev.isEmpty() ? QString() : overview::udev_fstype(dev);
        if (NON_MOUNTABLE_FS.contains(fstype))
            continue;
        DriveInfo info;
        info.name = gstr(g_volume_get_name(v.get()));
        info.device = dev;
        info.fs = fstype == "crypto_LUKS" ? QString("encrypted") : fstype;
        info.total = dev.isEmpty() ? 0 : overview::sysfs_size(dev);
        info.mounted = false;
        GIcon *gi = g_volume_get_icon(v.get());
        info.icon = gicon_to_qicon(gi);
        g_object_unref(gi);
        info.volume = v;
        info.kind = "unmounted";
        local << info;
    }
    g_list_free(volumes);
    return {local, network};
}

DriveCard *OverviewPage::drive_card(DriveInfo info)
{
    if (info.mount) {
        overview::MountRef m = info.mount;
        info.action = g_mount_can_eject(m.get()) ? "Eject" : "Unmount";
        info.action_icon = {"media-eject-symbolic", "media-eject"};
        info.on_action = [this, m]() { do_unmount(m); };
    }
    auto *card = new DriveCard(info);
    if (info.mounted) {
        QString target = !info.root.isEmpty() ? info.root : info.uri;
        connect(card, &Card::clicked, this, [this, target]() {
            if (!target.isEmpty())
                Q_EMIT navigate(target);
        });
        connect(card, &Card::middle_clicked, this, [this, target]() {
            if (!target.isEmpty())
                Q_EMIT open_in_tab(target);
        });
        connect(card, &Card::menu_requested, this, [this, info](const QPoint &pos) { drive_menu(info, pos); });
    } else {
        overview::VolumeRef v = info.volume;
        connect(card, &Card::clicked, this, [this, v]() { do_mount(v); });
    }
    return card;
}

QHBoxLayout *OverviewPage::connect_row()
{
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel("Connect to Server:"));
    auto *edit = new QLineEdit;
    edit->setPlaceholderText("smb://server/share   sftp://user@host/path   nfs://server/export   ftp://…");
    auto *go = new QPushButton(theme_icon({"network-server", "network-wired"}), "Connect");
    auto do_connect = [this, edit]() {
        QString uri = edit->text().trimmed();
        if (uri.isEmpty())
            return;
        if (!is_uri(uri)) {
            QString s = uri;
            while (s.startsWith('/') || s.startsWith('\\'))
                s.remove(0, 1);
            uri = "smb://" + s.replace('\\', '/');
        }
        Q_EMIT navigate(uri);
    };
    connect(edit, &QLineEdit::returnPressed, this, do_connect);
    connect(go, &QPushButton::clicked, this, do_connect);
    row->addWidget(edit, 1);
    row->addWidget(go);
    return row;
}

void OverviewPage::do_mount(const overview::VolumeRef &volume)
{
    QPointer<OverviewPage> self(this);
    overview::mount_volume(this, volume, [self](const QString &path, const QString &err) {
        if (!self)
            return;
        if (!err.isEmpty())
            QMessageBox::warning(self, "Mount", err);
        Q_EMIT self->sidebar_changed();
        self->refresh();
        if (!path.isEmpty())
            Q_EMIT self->navigate(path);
    });
}

void OverviewPage::do_unmount(const overview::MountRef &mount)
{
    QPointer<OverviewPage> self(this);
    overview::unmount(this, mount, [self](const QString &err) {
        if (!self)
            return;
        if (!err.isEmpty())
            QMessageBox::warning(self, "Unmount", err);
        Q_EMIT self->sidebar_changed();
        self->refresh();
    });
}

void OverviewPage::drive_menu(const DriveInfo &info, const QPoint &pos)
{
    QString target = info.root;
    QMenu m(this);
    if (!target.isEmpty()) {
        m.addAction("Open", this, [this, target]() { Q_EMIT navigate(target); });
        m.addAction("Open in New Tab", this, [this, target]() { Q_EMIT open_in_tab(target); });
        m.addAction("Open in Terminal", this, [target]() { open_terminal(target); });
        m.addAction("Add to Bookmarks", this, [this, target]() {
            Q_EMIT add_bookmark(target);
            refresh();
        });
        m.addAction("Properties", this, [this, target]() { Q_EMIT properties(target); });
    }
    if (info.mount) {
        overview::MountRef mt = info.mount;
        m.addSeparator();
        m.addAction(info.action, this, [this, mt]() { do_unmount(mt); });
    }
    m.exec(pos);
}

void OverviewPage::bookmark_menu(const QString &target, const QPoint &pos)
{
    QMenu m(this);
    m.addAction("Open", this, [this, target]() { Q_EMIT navigate(target); });
    m.addAction("Open in New Tab", this, [this, target]() { Q_EMIT open_in_tab(target); });
    m.addSeparator();
    m.addAction("Edit Bookmark…", this, [this, target]() {
        if (dialogs::edit_bookmark(this, target)) {
            Q_EMIT sidebar_changed();
            rebuild();
        }
    });
    m.addAction("Remove Bookmark", this, [this, target]() {
        auto bms = read_bookmarks();
        QList<QPair<QString, QString>> keep;
        for (const auto &b : bms)
            if (b.first != target)
                keep << b;
        write_bookmarks(keep);
        Q_EMIT sidebar_changed();
        rebuild();
    });
    m.exec(pos);
}

void OverviewPage::thumb_ready(const QString &path)
{
    if (BookmarkCard *card = bookmark_cards.value(path))
        card->update_pic();
}
