// Starred and Recent: places that list files from anywhere, like the combined Trash.
//
// Starred items are Kestrel's own list (~/.config/kestrel-explorer/starred.json). Recent is the desktop's shared
// recently-used list (~/.local/share/recently-used.xbel), which GTK apps write too; files opened from Kestrel are
// added to it unless "Remember recent files" is off in GNOME's privacy settings.
#pragma once

#include <QObject>
#include <QPair>
#include <QStringList>

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
    void starred_changed();
};
Signals *signals_();

QStringList starred();
bool is_starred(const QString &path);
void set_starred(const QStringList &paths, bool on);
void reload_starred();   // re-read the list (another Kestrel changed it) and emit starred_changed

// (path, original path) for a place, as util::trashed_items() gives for the trash (here both the same)
QList<QPair<QString, QString>> items(const QString &place);

QStringList recent_files(int limit = 500);   // local files from the recently-used list, newest first
void add_recent(const QStringList &paths);   // files opened from Kestrel (as GNOME Files does)

}  // namespace places
