// Undo for file operations (Ctrl+Z), shared by this Kestrel's windows (or every Kestrel's, with Preferences → Share
// undo between all Kestrel windows).
//
// Recorded: moves (drag and drop, cut/paste, Move To), renames (single and batch), Move to Trash, and items created by
// copy, paste, duplicate, New Folder / New File and links. Undoing a creation moves the new items to the trash, like
// GNOME Files, so nothing is lost by undoing. Not undoable: permanent deletes, merges into an existing folder, copies
// that replaced something, archive jobs and admin-session operations. A replace or merge that leaves nothing else to
// undo records a "none" entry: Ctrl+Z then only says it can't be undone, instead of undoing the action before it.
#pragma once

#include <QObject>
#include <QPair>
#include <QStringList>

class MainWindow;

namespace undo {

class Signals : public QObject {
    Q_OBJECT
Q_SIGNALS:
    void changed();
};
Signals *signals_();

// kind "move": pairs (src, dst) · "rename": (old, new) · "trash": (original, -) · "create": (path, -) ·
// "none": no items (what was done can't be undone)
void record(const QString &kind, const QString &label, const QList<QPair<QString, QString>> &items);
void record_paths(const QString &kind, const QString &label, const QStringList &paths);   // "trash" / "create"
QString label();   // what Ctrl+Z would undo, e.g. "Move", or empty
void undo(MainWindow *win);

}  // namespace undo
