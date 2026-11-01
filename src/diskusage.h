// Disk Usage…: what fills a folder or a drive, as a treemap. Each folder is a block sized by what it holds, with its
// biggest subfolders and files inside it; click a folder to go into it, Back (or a folder named in the path above the
// map) to go up, hover for a size and its share. A list beside the map has the folder shown, biggest first.
//
// Counting is a job (it reads the whole folder, so it waits for its turn on the drive), with progress, Pause and Stop.
// It stays on the folder's own drive (a drive mounted below it isn't gone into). The folder sizes Properties remembers
// (fileops::dirsizes) are shown at once, with when they were counted, while it counts; what it counts is remembered
// for Properties in turn. On a tape or a floppy it counts only when asked: it reads every folder there.
#pragma once

#include <QDialog>
#include <QIcon>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

class MainWindow;
class QLabel;
class QMenu;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class Task;

namespace diskusage {

// A folder, a file, or the smaller files of a folder taken together. Sizes as Properties counts them
// (fileops::dir_stats): each name's own size (st_size), every file and link, hard links under each name.
struct Node {
    QString name;               // the root's: its whole path
    qint64 size = 0;
    qint64 files = 0, dirs = 0;   // a folder's: everything below it; a file's: 1; the smaller files': how many
    quint64 ino = 0;
    bool dir = false;
    bool rest = false;          // the smaller files of its folder, not kept one by one
    bool remembered = false;    // from Properties' remembered sizes, not counted now (its contents may be missing)
    bool complete = true;       // counted the way Properties counts (no drive mounted below it): can be remembered
    bool pruned = false;        // a small folder: its contents counted but not kept
    Node *parent = nullptr;
    std::vector<std::unique_ptr<Node>> kids;   // biggest first
    QString path() const;
    bool can_enter() const { return dir && !kids.empty(); }
};
using Tree = std::shared_ptr<Node>;

const int FILES_KEPT = 20;               // each folder's biggest files kept one by one, at most…
const qint64 SMALL_FOLDER = 128 * 1024;  // …and a folder under this keeps only its totals (a block, not gone into)

// Count root (a folder), on a thread: task (may be nullptr) gets the progress, expected (bytes, 0: unknown) makes
// it a percentage, and is checked between folders (a stop throws Cancelled). Folders that couldn't be read are
// named in *unreadable (they count as empty, as in Properties).
Tree count(const QString &root, Task *task = nullptr, qint64 expected = 0, QStringList *unreadable = nullptr);
// What Properties remembers below root (and root's own), as a tree, at most `depth` levels down; nullptr when it
// remembers nothing there. *oldest: when its oldest figure was counted. UI thread.
Tree remembered(const QString &root, qint64 *oldest = nullptr, int depth = 4);
// The counted folders to remember for Properties: root, its subfolders, then the biggest, at most `max` in all
// (only folders counted as Properties counts them: complete)
QList<QPair<QString, const Node *>> worth_remembering(const Node &root, int max = 1000);
// Take n out of its tree (it went to the trash): the folders above it get smaller
void remove(Node *n);
// Find the node at path in the tree (nullptr: not there)
Node *find(Node *root, const QString &path);

// The squarified treemap: rects for sizes (biggest first, all > 0) filling area, as square as can be
QList<QRectF> squarify(const QList<double> &sizes, const QRectF &area);

// Counting needs asking first here: a tape or a floppy
bool count_asked_first(const QString &path);
QIcon icon();   // Disk Usage's, in menus and on a drive's Overview card


void open_window(MainWindow *win, const QString &folder);

}  // namespace diskusage

// The map itself: draws `shown` (a folder of the tree) and what's inside it, a few levels deep
class UsageMap : public QWidget {
    Q_OBJECT
public:
    explicit UsageMap(QWidget *parent = nullptr);
    void set_shown(diskusage::Node *n);   // also after the tree changed
    diskusage::Node *shown = nullptr;
    diskusage::Node *selected = nullptr;
    diskusage::Node *node_at(const QPointF &pos) const;   // the innermost block there
    QRectF rect_of(const diskusage::Node *n) const;        // empty when it isn't drawn
    QString describe(const diskusage::Node *n) const;      // "Photos — 12.3 GB, 45% of Pictures"

Q_SIGNALS:
    void enter(diskusage::Node *n);     // a folder clicked
    void picked(diskusage::Node *n);    // a file clicked (selected)
    void activated(diskusage::Node *n); // a file double-clicked
    void menu(diskusage::Node *n, const QPoint &global);
    void hovered(diskusage::Node *n);   // nullptr: off the map
    void back();                        // the mouse's back button

protected:
    void paintEvent(QPaintEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void mouseDoubleClickEvent(QMouseEvent *ev) override;
    void contextMenuEvent(QContextMenuEvent *ev) override;
    void leaveEvent(QEvent *ev) override;
    bool event(QEvent *ev) override;   // tooltips

private:
    struct Block {
        diskusage::Node *node;
        QRectF rect;
        int depth;
        int hue;
    };
    void relayout();
    void lay(diskusage::Node *n, const QRectF &area, int depth, int hue);
    QList<Block> blocks;   // outer ones first
    diskusage::Node *hover = nullptr;
};

// The Disk Usage window: the path shown (each folder above a link), Back, Recalculate, Pause, Stop, the progress,
// when the sizes were counted, the map and the list of the folder shown.
class DiskUsageWindow : public QDialog {
    Q_OBJECT
public:
    DiskUsageWindow(MainWindow *win, const QString &root);
    ~DiskUsageWindow() override;
    void recount();                        // Recalculate (and the first count)
    bool counting() const { return task != nullptr; }
    void go(diskusage::Node *n);           // show this folder
    void up();                             // Back
    void trash(diskusage::Node *n);        // Move to Trash (Undo as usual)
    QMenu *item_menu(diskusage::Node *n);  // its right-click menu
    diskusage::Tree tree;
    UsageMap *map;
    QTreeWidget *list;
    QLabel *where, *age, *status, *info;
    QPushButton *back_btn, *recount_btn, *pause_btn, *stop_btn;

Q_SIGNALS:
    void finished_count();   // the count ended (also stopped)

protected:
    void closeEvent(QCloseEvent *ev) override;

private:
    void show_tree(const diskusage::Tree &t);
    void refresh();          // the path, the list and the map, for the folder shown
    void sync_pause();
    QPointer<MainWindow> win;
    QString root;
    QPointer<Task> task;
    QProgressBar *bar;
    bool from_memory = false;   // tree is Properties' remembered sizes
};
