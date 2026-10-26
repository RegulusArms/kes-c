// Starred and Recent: places that list files from anywhere, like the combined Trash.
//
// Starred items are Kestrel's own list (~/.config/kestrel-explorer/starred.json). Recent is the desktop's shared
// recently-used list (~/.local/share/recently-used.xbel), which GTK apps write too; files opened from Kestrel are
// added to it unless "Remember recent files" is off in GNOME's privacy settings.
//
// Pinned items (~/.config/kestrel-explorer/pinned.json, {folder: [names]}) aren't listed anywhere: "Pinned & Starred
// First" sorts them to the top of their folder.
//
// Frequent folders (~/.config/kestrel-explorer/frequent.json, {folder: [visits, last visit]}): the folders opened in
// Kestrel's windows (not in file choosers), counted while the desktop's File History switch is on, as Recent is. The
// Overview can show the most used. Counted in memory and written a few seconds later, merged with the file as it is
// then (so another Kestrel's visits stay); the 200 most used are kept.
//
// Folder sorts (~/.config/kestrel-explorer/sorts.json, {folder: [column, order, group, group_by]}): each folder keeps
// the sort it was last given, and its grouping (groups.h: none, by period or by day, of the date modified, created or
// taken); folders without one use the default (the "sort_col", "sort_order", "sort_group" and "sort_group_by" view
// settings).
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
    int group = 0;      // groups::Mode
    int group_by = 0;   // groups::DateBy
};
std::optional<FolderSort> folder_sort(const QString &dir);   // dir's own sort, if it has one
void set_folder_sort(const QStringList &dirs, FolderSort sort);   // also announced to the other Kestrels
void reload_sorts();   // re-read them (another Kestrel changed them); emit sorts_changed

// (path, original path) for a place, as util::trashed_items() gives for the trash (here both the same)
QList<QPair<QString, QString>> items(const QString &place);

bool remembers_history();   // the desktop's File History privacy switch (on when the desktop has none)
void visit_folder(const QString &dir);
QStringList frequent_folders(int limit);           // the most visited first (then the latest)
void forget_frequent(const QStringList &dirs);     // Remove from Frequent Folders
void save_frequent();                              // write the visits counted so far (also done on its own)

QStringList recent_files(int limit = 500);   // local files from the recently-used list, newest first
void add_recent(const QStringList &paths);   // files opened from Kestrel (as GNOME Files does)

}  // namespace places
