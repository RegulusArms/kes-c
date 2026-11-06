// Making and changing links as a batch: what the Link window (linkwin.h) lists before anything is made, its checks,
// and the job that makes the links or changes them (failure-atomic, undoable as one step).
#pragma once

#include <QList>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>

class Task;

namespace links {

// kinds: "sym" (a symbolic link to the absolute path), "rel" (a symbolic link with a relative path), "hard" (another
// name of the same file, on its drive) and "desktop" (a .desktop shortcut)
QStringList kinds();
QString kind_label(const QString &kind);   // "Symbolic link"…, translated

// A link to make, or an existing symbolic link to change (existing set). Nothing is made until commit().
struct Link {
    int id = 0;            // the window's own, to find it again
    QString name, folder;  // where it goes
    QString points_to;     // what it points to: an absolute path (for a broken link, one that isn't there)
    QString kind = "sym";
    bool renamed = false;  // the user gave the name: kept when the kind or what it points to changes
    QString existing;      // a link being changed: its path as it was found
    QString old_text;      // and what it held then
    QString old_kind, old_points_to;   // and how it was read (kind, points_to) then
    QString path() const;
    bool is_edit() const { return !existing.isEmpty(); }
    QString text() const;   // what the symbolic link holds: points_to, or the way there from folder ("rel")
    // an edit that changes nothing (same place, same contents)
    bool unchanged() const;
};

// An existing symbolic link as an entry to change: kind "rel" when it holds a relative path. Raises OSError.
Link from_existing(const QString &path);
// The name a new link gets: the usual one (fileops::link_name), made free in its folder and among `taken` (paths of
// the other entries)
QString free_name(const QString &kind, const QString &points_to, const QString &folder, const QSet<QString> &taken);

struct Problem {
    QString text;
    bool blocks = true;   // false: a warning (a symbolic link to something that isn't there can still be made)
};
// What stands in the way of l (others: the whole list, l among them; a name two entries share is a problem)
QList<Problem> check(const Link &l, const QList<Link> &others);

// Undo items for undo::record("links", …), in the order done: (path, "") a link made · (old, new) a link renamed or
// moved · (path, "->" + text) a link's contents replaced (text: what it held before). undo.cpp undoes them backwards.
using UndoItems = QList<QPair<QString, QString>>;

// Make or change one link, on a job's thread. Raises OSError or Error (nothing changed then); undo gets its items.
// A new link is made under a part name and given its name only if that's still free (put_new); a hard link is
// claimed by link() itself. A changed link is a new one made beside it (a part, fsynced with its folder) and renamed
// over it: never deleted first. A rename keeps the link (rename without replacing); moved to another folder with new
// contents, the new one is made first and the old one removed after.
void commit(const Link &l, UndoItems *undo);
// The symbolic link at path made to hold text, failure-atomically (as commit). expected: what it must still hold
// (null: anything), otherwise nothing is changed and Error is raised.
void replace_symlink(const QString &path, const QString &text, const QString &expected = QString());
// A symbolic link moved to a new name that must be free: a rename, or across drives a copy made first and the old one
// removed after.
void move_symlink(const QString &from, const QString &to);

// The other names of a file on its drive (its hard links), searched from the top of that drive; stops once all of
// them are found. For a job (task: progress and cancel; may be null).
QStringList other_names(const QString &path, Task *task);

}  // namespace links
