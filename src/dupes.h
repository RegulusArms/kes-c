// Find Duplicates…: the files with the same contents in the folders (or drives) chosen, in groups, and a window to
// choose which copies to keep and move the others to the trash, or replace them with hard links to the one kept.
//
// Fast: files are grouped by size first (a file of a size no other has can't have a copy), then only those of the same
// size are read: their first 64 KB, then, where those match too, the whole file (SHA-256, so files in a group are
// identical). It runs as a job (it reads the drives, so it waits for its turn on them), with progress and ✕.
#pragma once

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class MainWindow;
class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;
class Task;

namespace dupes {

struct Options {
    qint64 min_size = 1;          // bytes; smaller files are left out (empty files are never duplicates)
    bool hidden = false;          // hidden files and folders (a name starting with a dot)
    bool skip_hardlinks = true;   // hard links to one file are that file, not copies: listed once
    bool recursive = true;        // into subfolders (never through a symlinked folder)
};

struct File {
    QString path;
    qint64 size = 0, mtime = 0;   // mtime: seconds since the epoch, as found (checked again before anything is done)
    quint64 dev = 0, ino = 0;
};

// Files with identical contents, the oldest first (then by path)
struct Group {
    qint64 size = 0;
    QList<File> files;
};

// The groups in roots, biggest waste first. Each root's own drive only (a drive mounted in a folder below it isn't
// gone into). On a thread: task (may be nullptr) gets the progress and is checked between files and reads (a stop
// throws Cancelled). Files that couldn't be read are left out and named in *unreadable.
QList<Group> find(const QStringList &roots, const Options &opts, Task *task = nullptr, QStringList *unreadable = nullptr);
// The bytes freed by keeping one file of each group (hard links to one file free nothing more)
qint64 freed(const QList<Group> &groups);
qint64 freed(const Group &g, const QList<bool> &keep);   // by removing the files not kept

// Which files to keep (in group order): the oldest, the newest, or those in `folder` (or below it; none there: all)
QList<bool> keep_oldest(const Group &g);
QList<bool> keep_newest(const Group &g);
QList<bool> keep_in(const Group &g, const QString &folder);

// Has the file changed since it was found (size or date), or gone?
bool changed(const File &f);

// Replace each of `others` by a hard link to `keep`, failure-atomic: the link is made under a part name beside the file
// and renamed over it, so the file is either still itself or the link, never missing. Skipped (and said in the
// result): a file on another drive, one that changed since it was found, one that's the same file already. Returns
// one line per file not replaced; *replaced gets those that were. On a thread.
QStringList link_over(const File &keep, const QList<File> &others, QStringList *replaced = nullptr);

}  // namespace dupes

// The Find Duplicates window: the folders, the options, Search / Stop; the groups found with their files (a thumbnail,
// the folder, the date and the size), a Keep box for each file, helpers to keep the oldest, the newest or those in a
// folder, the space that would be freed, and Move the Others to Trash (undoable) / Replace the Others with Hard Links…
class DupesWindow : public QDialog {
    Q_OBJECT
public:
    DupesWindow(MainWindow *win, const QStringList &roots);
    ~DupesWindow() override;
    void search();
    bool searching() const { return task != nullptr; }
    QList<dupes::Group> groups;
    QList<QList<bool>> keep;   // per group, per file
    void set_keep(const std::function<QList<bool>(const dupes::Group &)> &rule);
    void trash_others();
    void link_others();
    QTreeWidget *tree;
    QLabel *summary;
    QSpinBox *min_kb;
    QCheckBox *hidden, *skip_links, *recursive;
    QPushButton *search_btn, *stop_btn, *trash_btn, *link_btn;

Q_SIGNALS:
    void finished_search();   // the results are shown (also after a stop: none)

protected:
    void closeEvent(QCloseEvent *ev) override;

private:
    void show_results();
    void update_summary();
    void icon_ready(const QString &path);
    void drop_files(const QSet<QString> &gone);   // they were trashed or replaced: out of the list
    QPointer<MainWindow> win;
    QStringList roots;
    QPointer<Task> task;
    bool filling = false;
    QProgressBar *bar;
    QLabel *status;
};

namespace dupes {
void open_window(MainWindow *win, const QStringList &roots);
}
