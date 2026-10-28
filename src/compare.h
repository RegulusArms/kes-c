// Compare With… and Sync To…: two folders' differences, and making one folder match another.
//
// Compare lists what differs between two folders, file by file through their subfolders: only on one side, newer on
// one side, or the same name with other contents (by size and date, or byte by byte for a deep compare), and copies
// across or trashes from the list. Sync makes a destination match a source, one way (Pictures → a backup drive): a
// preview of exactly what will be copied, replaced and (optionally) deleted, then it runs, with rsync when it's
// installed (it resumes interrupted copies and only sends what changed) or Kestrel's own copy. Either way, nothing
// half-copied is ever left over a good file: rsync writes each file under a temporary name and renames it into place
// (an interrupted one waits in .kes-rsync-partial, for the next run), Kestrel's copy writes a part file. Deleting on
// the destination is off by default, and asks again. Sync pairs can be saved ("Pictures → Backup HDD") to run again
// in one click; a pair on a removable drive is found by its file system's UUID wherever the drive is mounted.
#pragma once

#include "fileops.h"

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>

class MainWindow;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QTreeWidget;

namespace compare {

enum Kind {
    SAME,
    ONLY_LEFT,
    ONLY_RIGHT,
    LEFT_NEWER,
    RIGHT_NEWER,
    DIFFERENT,   // the same name, other contents (or a file on one side and a folder on the other)
};

struct Item {
    QString rel;   // relative to both folders
    Kind kind = SAME;
    bool left_dir = false, right_dir = false;
    qint64 left_size = -1, right_size = -1;   // -1: not on that side
    qint64 left_mtime = 0, right_mtime = 0;
};

// Dates within this many seconds count as the same (FAT keeps them to 2 s; rsync's --modify-window does the same)
static const int SLACK = 1;
bool same_time(qint64 a, qint64 b);

// What differs between left and right (SAME items too, so a list can show them), in path order. A folder on one side
// only is one item (not each file in it). Files: the same size and date are the same; with deep, they're also read
// and compared byte by byte (and files of the same size with other dates that turn out identical are the same). On a
// thread: task (may be nullptr) for progress and stopping.
QList<Item> diff(const QString &left, const QString &right, bool deep = false, Task *task = nullptr);
bool same_bytes(const QString &a, const QString &b, Task *task = nullptr);   // throws OSError if one can't be read
QString kind_text(Kind k, const QString &left_name, const QString &right_name);   // "Only in Pictures", "Newer in Backup"

// ---- Sync

struct Step {
    enum Op { COPY, REPLACE, DELETE } op = COPY;
    QString rel;
    bool dir = false;          // a whole folder (copied or deleted with everything in it)
    qint64 bytes = 0;          // a file's size (a folder's: 0, counted when shown)
    bool dest_newer = false;   // REPLACE: the destination's copy is the newer one
};
// What makes dest match source, from diff(source, dest)
QList<Step> plan(const QList<Item> &items, bool deletes);
// Kestrel's own copy: the jobs for fileops::start_ops (replacing through part files, deleting what's to go)
QList<fileops::Job> jobs_for(const QString &source, const QString &dest, const QList<Step> &steps);
bool have_rsync();
QStringList rsync_argv(const QString &source, const QString &dest, bool deletes);
// Run rsync on a job's thread: its progress to task, its output to the job's log. Throws Error if it fails.
void run_rsync(Task *task, const QString &source, const QString &dest, bool deletes);

// Saved pairs: ~/.config/kestrel-explorer/sync-pairs.json
struct Pair {
    QString name, source, dest;
    bool deletes = false;
    bool rsync = true;
    QString source_uuid, source_rel, dest_uuid, dest_rel;   // the file system's UUID and the path inside it, if any
};
QString pairs_file();
QList<Pair> pairs();
void save_pairs(const QList<Pair> &pairs);   // written whole, atomically
Pair make_pair(const QString &name, const QString &source, const QString &dest, bool deletes, bool rsync);
// The file system's UUID path is on (from /dev/disk/by-uuid: no drive is asked), and *rel the path inside it; empty
// when it has none (a network share, a file system without a UUID)
QString fs_uuid(const QString &path, QString *rel = nullptr);
// Where a saved path is now: the path, if it's there on the same file system; else the same place on the file system
// with that UUID wherever it's mounted now; else the path as saved
QString resolve(const QString &path, const QString &uuid, const QString &rel);

}  // namespace compare

// Compare With…: the two folders, Compare (deep: by contents), the differences listed with what to show, and Open,
// Copy across and Move to Trash for the items selected
class CompareWindow : public QDialog {
    Q_OBJECT
public:
    CompareWindow(MainWindow *win, const QString &left, const QString &right);
    ~CompareWindow() override;
    void run();
    bool running() const { return task != nullptr; }
    QList<compare::Item> items;
    QTreeWidget *tree;
    QLineEdit *left_edit, *right_edit;
    QCheckBox *deep;
    QLabel *summary;
    QList<compare::Item> selected() const;
    void copy_across(bool to_right);
    void trash_side(bool right);
    void open_side(bool right);

Q_SIGNALS:
    void compared();   // the list is shown (also after a stop)

protected:
    void closeEvent(QCloseEvent *ev) override;

private:
    void fill();
    QPointer<MainWindow> win;
    QPointer<Task> task;
    QList<QCheckBox *> shows;   // which kinds are listed
    QProgressBar *bar;
    QPushButton *compare_btn, *stop_btn;
};

// Sync To…: source and destination, deleting (off), rsync or Kestrel's copy, Preview (the steps listed), Sync Now;
// the saved pairs, Save Pair…, Run (preview and run in one click) and Remove
class SyncWindow : public QDialog {
    Q_OBJECT
public:
    SyncWindow(MainWindow *win, const QString &source, const QString &dest);
    ~SyncWindow() override;
    void preview(bool then_run = false);
    void sync_now();
    void run_pair(int i);   // a saved pair: preview, and run (asking first when it deletes)
    void save_pair(const QString &name);
    bool busy() const { return task != nullptr; }
    QList<compare::Step> steps;
    QLineEdit *source_edit, *dest_edit;
    QCheckBox *deletes;
    QRadioButton *use_rsync, *use_kestrel;
    QTreeWidget *list;
    QComboBox *saved;
    QLabel *summary;
    QPushButton *preview_btn, *sync_btn;

Q_SIGNALS:
    void previewed();   // the steps are listed
    void synced();      // a sync has finished (also failed or stopped)

protected:
    void closeEvent(QCloseEvent *ev) override;

private:
    void fill_saved();
    void start(const QString &source, const QString &dest, const QList<compare::Step> &steps, bool del, bool rsync);
    QPointer<MainWindow> win;
    QPointer<Task> task;
    QString planned_source, planned_dest;   // what the steps are for
    bool planned_deletes = false;
    QProgressBar *bar;
};

namespace compare {
void open_compare(MainWindow *win, const QString &left);   // asks for the other folder first
void open_sync(MainWindow *win, const QString &source);    // the destination is chosen in the window
}
