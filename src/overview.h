// Overview page: drives with usage (and health), phones and cameras, network locations, connect-to-server, bookmarks,
// and, when chosen, Starred and Frequent Folders. Preferences → Overview picks the sections, their order, the card size
// and what drive cards show.
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
#include <QSet>
#include <QVariantMap>

#include "health.h"

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

// Mounted filesystems with usage (worker thread: statfs can block). system_mounts: also /boot, /efi and the like;
// with_health: each local drive's health (udisks, health.h) as "health".
QVariantList scan_filesystems(bool system_mounts = false, bool with_health = false);

// The sections, in their default order: id, title. overview_sections keeps them in the order chosen, a hidden one as
// "-id"; Starred and Frequent Folders start hidden.
extern const QList<QPair<QString, QString>> SECTIONS;
QList<QPair<QString, bool>> section_order(const QStringList &saved);   // (id, shown), each section once
QStringList save_order(const QList<QPair<QString, bool>> &order);      // as overview_sections keeps it

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
    bool has_health = false;
    health::Info health;
};

struct DriveLists {
    QList<DriveInfo> local, phones, network;
};

namespace overview {

// Phones and cameras: gvfs's afc (Apple devices), gphoto2 (cameras, and an iPhone's photos) and mtp (Android).
bool is_phone_scheme(const QString &scheme);
QString phone_kind(const QString &scheme);                       // what the mount holds, e.g. "Photos and videos"
QString phone_hint(const QString &scheme, const QString &name);   // what to do on the device when it won't mount
extern const QString NO_PHONES_HINT;

// Where the Overview lists a GIO mount. known_root: its path is one of the scanned filesystems.
enum class MountGroup { Skip, Local, Phone, Network };
MountGroup mount_group(const QString &scheme, const QString &path, bool shadowed, bool known_root);

// Connected phones (mounted, with their FUSE path) and ones that can be mounted (uri = where to mount them).
QList<DriveInfo> phone_infos(GVolumeMonitor *monitor);
// Network shares mounted through GVFS (smb, sftp, ftp…). known_roots: paths that are kernel mounts (listed as local).
QList<DriveInfo> network_infos(GVolumeMonitor *monitor, const QSet<QString> &known_roots);
// A recent server's address is one of these shares, or inside one
bool connected(const QString &server, const QList<DriveInfo> &shares);

}  // namespace overview

// What drive cards show (Preferences → Overview); size: 0 small, 1 normal, 2 large (bookmark cards too)
struct CardLook {
    int size = 1;
    bool bar = true;
    bool percent = false;
};

class DriveCard : public Card {
    Q_OBJECT
public:
    explicit DriveCard(const DriveInfo &info, const CardLook &look = CardLook(), QWidget *parent = nullptr);
    DriveInfo info;
    QLabel *health_label = nullptr;   // its health line (a link to Drive Health…), when it has one
};

// A folder's card (a bookmark, a save location, a starred or frequent folder), or a starred file's
class BookmarkCard : public Card {
    Q_OBJECT
public:
    BookmarkCard(const QString &target, const QString &label, ThumbnailManager *thumbs, int size = 1,
                 QWidget *parent = nullptr);
    void update_pic();
    QString target;

private:
    ThumbnailManager *thumbs;
    QLabel *pic;
    bool local;
    int px;   // the picture's size
};

// Shows drives, network locations, bookmarks, starred items and frequent folders, as chosen.
class OverviewPage : public QScrollArea {
    Q_OBJECT
public:
    explicit OverviewPage(ThumbnailManager *thumbs, QWidget *parent = nullptr);
    ~OverviewPage() override;
    void refresh();   // re-scan filesystems in the background, then rebuild
    // A Save window's Overview (chooser.h) starts with its save locations: folders the user keeps for saving from other
    // apps ("chooser_places"; like Starred, but only the chooser's). Managed here and in Preferences → Windows.
    void set_save_places(bool on);
    void add_save_place(const QString &path);
    void remove_save_place(const QString &path);
    void move_save_place(const QString &path, int by);   // -1: earlier, 1: later
    QStringList save_place_cards() const;                // the save locations shown, in order
    QStringList section_titles() const;                  // the sections shown, in order
    QList<DriveCard *> drive_cards() const;
    QStringList card_targets(const QString &section) const;   // "starred", "frequent", "bookmarks": in order
    BookmarkCard *card_for(const QString &section, const QString &target) const;   // "starred" or "frequent"

Q_SIGNALS:
    void navigate(const QString &target);
    void open_in_tab(const QString &target);
    void sidebar_changed();
    void add_bookmark(const QString &target);
    void properties(const QString &target);
    void find_duplicates(const QString &root);
    void open_file(const QString &path);   // a starred file clicked

protected:
    void showEvent(QShowEvent *ev) override;
    void hideEvent(QHideEvent *ev) override;

private:
    void clear_layout(QLayout *lay);
    FlowLayout *section(const QString &title);
    void rebuild();
    void drives_section(const QList<DriveInfo> &local);
    void phones_section(const QList<DriveInfo> &phones);
    void network_section(const QList<DriveInfo> &network);
    void bookmarks_section();
    void starred_section();
    void frequent_section();
    BookmarkCard *folder_card(const QString &target, const QString &label, const QString &section);
    void starred_menu(const QString &target, const QPoint &pos);
    void frequent_menu(const QString &target, const QPoint &pos);
    DriveLists drive_infos();
    DriveCard *drive_card(DriveInfo info);
    QHBoxLayout *connect_row();
    void do_mount(const overview::VolumeRef &volume);
    void do_unmount(const overview::MountRef &mount);
    void drive_menu(const DriveInfo &info, const QPoint &pos);
    void bookmark_menu(const QString &target, const QPoint &pos);
    void save_place_menu(const QString &target, const QPoint &pos);
    void save_places_section();
    void thumb_ready(const QString &path);
    static void monitor_changed(void *, void *, void *self);

    ThumbnailManager *thumbs;
    QWidget *body;
    QVBoxLayout *lay;
    QHash<QString, BookmarkCard *> bookmark_cards;
    QHash<QString, BookmarkCard *> save_cards;
    QHash<QString, BookmarkCard *> other_cards;   // starred and frequent cards, by section + "\n" + path
    QHash<QString, QStringList> shown_targets;   // section -> its cards' targets, in order
    QStringList titles;                          // the sections shown
    QList<DriveCard *> shown_drives;
    CardLook look;
    QStringList save_order;   // the save locations shown
    bool save_places = false;
    bool fs_running = false;
    bool scan_again = false;
    bool have_fs = false;
    QHash<QString, QString> shown_slow;   // tape / floppy root -> what its card showed (redrawn when it changes)
    QVariantList fs;
    QTimer refresh_timer, usage_timer;
    GVolumeMonitor *monitor = nullptr;
};
