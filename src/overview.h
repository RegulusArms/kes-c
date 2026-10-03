// Overview page: drives with usage, network locations, connect-to-server, bookmarks.
//
// Modelled on the old "Other Locations" page in GNOME Files. Volume discovery uses GIO's GVolumeMonitor (same
// source as Nautilus, so unmounted and encrypted drives can be mounted/unlocked) merged with QStorageInfo for
// everything else that is mounted (ZFS datasets, fstab mounts, network filesystems).
#pragma once

#include <QFrame>
#include <QLayout>
#include <QLabel>
#include <QScrollArea>
#include <QTimer>
#include <QVariantMap>

#include <functional>
#include <memory>

typedef struct _GMount GMount;
typedef struct _GVolume GVolume;
typedef struct _GVolumeMonitor GVolumeMonitor;

class ThumbnailManager;
class QVBoxLayout;

extern const QString OVERVIEW;         // "overview://"
extern const QString OVERVIEW_TITLE;   // "Overview"

bool is_uri(const QString &target);

namespace overview {

using MountRef = std::shared_ptr<GMount>;
using VolumeRef = std::shared_ptr<GVolume>;

QVariantList scan_filesystems();   // mounted filesystems with usage (worker thread: statfs can block)

// Mount a network location (smb://, sftp://, ...) and call on_done(local_path, error).
void mount_uri(QWidget *parent, const QString &uri, std::function<void(const QString &, const QString &)> on_done);
void mount_volume(QWidget *parent, const VolumeRef &volume,
                  std::function<void(const QString &, const QString &)> on_done);
void unmount(QWidget *parent, const MountRef &mount, std::function<void(const QString &)> on_done);

}  // namespace overview

class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget *parent = nullptr, int spacing = 12);
    ~FlowLayout() override;
    void addItem(QLayoutItem *item) override;
    int count() const override;
    QLayoutItem *itemAt(int i) const override;
    QLayoutItem *takeAt(int i) override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int w) const override;
    void setGeometry(const QRect &rect) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;

private:
    int do_layout(const QRect &rect, bool test) const;
    QList<QLayoutItem *> items;
};

class Card : public QFrame {
    Q_OBJECT
public:
    explicit Card(QWidget *parent = nullptr);

Q_SIGNALS:
    void clicked();
    void middle_clicked();
    void menu_requested(const QPoint &pos);

protected:
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void contextMenuEvent(QContextMenuEvent *ev) override;
};

struct DriveInfo {
    QString name, root, fs, device, uri, status, kind;
    qint64 total = 0;
    qint64 free = -1;   // -1 = unknown
    bool mounted = true;
    QIcon icon;
    overview::MountRef mount;
    overview::VolumeRef volume;
    QString action;
    QStringList action_icon;
    std::function<void()> on_action;
};

class DriveCard : public Card {
    Q_OBJECT
public:
    explicit DriveCard(const DriveInfo &info, QWidget *parent = nullptr);
    DriveInfo info;
};

class BookmarkCard : public Card {
    Q_OBJECT
public:
    BookmarkCard(const QString &target, const QString &label, ThumbnailManager *thumbs, QWidget *parent = nullptr);
    void update_pic();
    QString target;

private:
    ThumbnailManager *thumbs;
    QLabel *pic;
    bool local;
};

// Shows drives, network locations and bookmarks.
class OverviewPage : public QScrollArea {
    Q_OBJECT
public:
    explicit OverviewPage(ThumbnailManager *thumbs, QWidget *parent = nullptr);
    ~OverviewPage() override;
    void refresh();   // re-scan filesystems in the background, then rebuild

Q_SIGNALS:
    void navigate(const QString &target);
    void open_in_tab(const QString &target);
    void sidebar_changed();
    void add_bookmark(const QString &target);
    void properties(const QString &target);

protected:
    void showEvent(QShowEvent *ev) override;
    void hideEvent(QHideEvent *ev) override;

private:
    void clear_layout(QLayout *lay);
    FlowLayout *section(const QString &title);
    void rebuild();
    QPair<QList<DriveInfo>, QList<DriveInfo>> drive_infos();
    DriveCard *drive_card(DriveInfo info);
    QHBoxLayout *connect_row();
    void do_mount(const overview::VolumeRef &volume);
    void do_unmount(const overview::MountRef &mount);
    void drive_menu(const DriveInfo &info, const QPoint &pos);
    void bookmark_menu(const QString &target, const QPoint &pos);
    void thumb_ready(const QString &path);
    static void monitor_changed(void *, void *, void *self);

    ThumbnailManager *thumbs;
    QWidget *body;
    QVBoxLayout *lay;
    QHash<QString, BookmarkCard *> bookmark_cards;
    bool fs_running = false;
    bool have_fs = false;
    QVariantList fs;
    QTimer refresh_timer, usage_timer;
    GVolumeMonitor *monitor = nullptr;
};
