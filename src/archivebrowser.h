// Open as Archive, the archive browser (the default; Preferences → Archives → "Open as Archive opens" chooses it or the
// simple list, archivewin.h): a small Kestrel window inside an archive. Its folders are browsed like real ones, in the
// panes' own views (the grid with GridDelegate, the list with ListDelegate, set up as Pane sets them up): icons or a
// list, zoom, sorting, back / forward / up, and a path bar of the archive's folders. Extract Here, Extract To…,
// Extract All… and Test are as in the simple list.
//
// Everything it extracts goes into the RAM cache (archivecache.h), never to disk: a file opened with its app, pictures
// shown in the image viewer (Quick Look, Space), the copies its thumbnails are made from, and what's dragged or copied
// out. Thumbnails are kept in memory only (never in ~/.cache/thumbnails or the folder-preview cache). Dragging items
// out, or Copy (Ctrl+C), extracts them into the cache and hands over those copies: pasted or dropped in Kestrel, they're
// copied as any files are (a copy job: part names, renamed into place). Read-only: nothing is cut, moved or deleted.
// Items too big for the whole cache, or encrypted ones whose password isn't known yet, are offered to Kestrel only
// (MEMBERS_MIME: extracted straight into the folder they're dropped on, asking for the password).
#pragma once

#include "archivewin.h"

#include <QHash>
#include <QPixmap>
#include <QSet>

class ArchiveModel;
class QAbstractItemView;
class GridDelegate;
class ImageViewer;
class QHBoxLayout;
class QListView;
class QSlider;
class QStackedWidget;
class QToolButton;
class QTreeView;

class ArchiveBrowser : public ArchiveView {
    Q_OBJECT
public:
    // the browser for `path`, raised if it's open already
    static ArchiveBrowser *open(MainWindow *owner, const QString &path);
    ~ArchiveBrowser() override;

    QStringList selected_members() const override;
    void select_members(const QStringList &members);
    QString folder() const { return cur; }   // the folder shown: "" (the archive's top) or "a/b/"
    QStringList shown_members() const;       // its items, in the order shown
    // Shows a folder of the archive (record: on the back history); select: an item of it to select
    void go_to(const QString &folder, bool record = true, const QString &select = QString());
    void go_back();
    void go_forward();
    void go_up();
    void activate(const QString &member);   // a folder opened, a file opened with its app
    void set_view_mode(const QString &mode);   // "grid" or "list"
    bool is_grid() const;
    void zoom(int step, int absolute = -1);
    int zoom_value() const;
    void sort_by(int column, Qt::SortOrder order);   // ArchiveModel's columns

    // Members' copies in the RAM cache: those not there are extracted (as a job titled `title`), then then(paths) in
    // the same order, each pinned (ArchiveCache::pin) until the caller unpins it. A full cache, a member bigger than
    // it, or a failure is told and then isn't called.
    void fetch(const QStringList &members, const QString &title, std::function<void(const QStringList &)> then);
    // The same, here and now (for a drop that asks for the files): the window keeps drawing meanwhile. Empty when
    // they can't be had (the reason in *why).
    QStringList fetch_now(const QStringList &members, QString *why = nullptr);
    void open_member(const QString &member);   // its copy, opened with its app
    void quick_look();                         // the selected pictures in the image viewer
    void copy_selected();                      // Ctrl+C: their copies on the clipboard, as copied files
    QMimeData *drag_data(const QStringList &members);
    void hold_for_drag(const QStringList &copies) { drag_pins += copies; }   // fetched for the drag: let go when it ends
    void drag_ended();                         // the drag's copies unpinned (and counted as just handed over)
    QPixmap thumbnail(const QString &member) const { return thumbs.value(member); }
    bool thumbnails_busy() const { return thumb_job; }
    QPointer<ImageViewer> viewer;              // the last one Quick Look opened

    QStackedWidget *stack;
    QListView *grid;
    QTreeView *list;
    ArchiveModel *model;
    QToolButton *back_btn, *fwd_btn, *up_btn, *grid_btn, *list_btn, *sort_btn;
    QLineEdit *filter;
    QSlider *zoom_slider;
    QPushButton *extract_here, *extract_to, *extract_all, *test;
    QWidget *crumbs;

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;
    void closeEvent(QCloseEvent *ev) override;   // its copies in the RAM cache go (those in use once free)

private:
    ArchiveBrowser(MainWindow *owner, const QString &path);
    void loaded_listing();
    void show_folder(const QString &select);
    void update_crumbs();
    void update_buttons();
    void update_grid_size();
    void context_menu(QAbstractItemView *v, const QPoint &pos);
    void queue_thumbs();
    void stop_thumbs();
    void let_go();
    void thumbs_made(const QString &folder, const QStringList &members, const QList<QImage> &images);

    struct Tree;
    std::shared_ptr<Tree> tree;   // the listing, by folder
    QString cur;
    QStringList back_stack, fwd_stack;
    GridDelegate *delegate;
    int grid_size = 128, list_size = 24;
    QHash<QString, QPixmap> thumbs;   // by member: made from copies in the RAM cache, kept in memory only
    QStringList thumb_order;          // oldest first (at most MAX_THUMBS kept)
    QSet<QString> thumb_failed;
    QPointer<QObject> thumb_job;      // the thumbnail job running (a Task)
    QStringList drag_pins;            // the copies the drag under way handed over
    bool closed = false;
    static constexpr int MAX_THUMBS = 1500;
};
