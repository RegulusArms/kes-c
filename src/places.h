// Starred and Recent: places that list files from anywhere, like the combined Trash.
//
// Starred items are Kestrel's own list (~/.config/kestrel-explorer/starred.json). Recent is the desktop's shared
// recently-used list (~/.local/share/recently-used.xbel), which GTK apps write too; files opened from Kestrel are
// added to it unless "Remember recent files" is off in GNOME's privacy settings.
//
// Pinned items (~/.config/kestrel-explorer/pinned.json, {folder: [names]}) aren't listed anywhere: "Pinned & Starred
// First" sorts them to the top of their folder.
//
// Folder sorts (~/.config/kestrel-explorer/sorts.json, {folder: [column, order]}): each folder keeps the sort it was
// last given; folders without one use the default (the "sort_col" / "sort_order" view settings).
#pragma once

#include <QObject>
#include <QPair>
#include <QStringList>

#include <optional>

namespace places {

extern const QString STARRED;   // "starred://"
extern const QString RECENT;    // "recent://"

bool is_virtual(const QString &place);
QString title(const QString &place);
QString icon_name(const QString &place);
QString empty_text(const QString &place);

class Signals : public QObject {
    Q_OBJECT
Q_SIGNALS:
    void starred_changed();   // the stars or the pins
    void sorts_changed();     // the folder sorts
};
Signals *signals_();

QStringList starred();
bool is_starred(const QString &path);
void set_starred(const QStringList &paths, bool on);
void reload_starred();   // re-read the lists, also the pins (another Kestrel changed them); emit starred_changed

QStringList pinned(const QString &dir);   // the names pinned in dir (also ones now gone)
bool is_pinned(const QString &path);
void set_pinned(const QStringList &paths, bool on);   // each in its own folder

struct FolderSort {
    int column = 0;
    int order = 0;   // Qt::SortOrder
};
std::optional<FolderSort> folder_sort(const QString &dir);   // dir's own sort, if it has one
void set_folder_sort(const QStringList &dirs, FolderSort sort);   // also announced to the other Kestrels
void reload_sorts();   // re-read them (another Kestrel changed them); emit sorts_changed

// (path, original path) for a place, as util::trashed_items() gives for the trash (here both the same)
QList<QPair<QString, QString>> items(const QString &place);

QStringList recent_files(int limit = 500);   // local files from the recently-used list, newest first
void add_recent(const QStringList &paths);   // files opened from Kestrel (as GNOME Files does)

}  // namespace places
