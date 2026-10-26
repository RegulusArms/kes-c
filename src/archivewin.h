// Open as Archive: a window of its own listing what's inside an archive (a tree of its folders and files, with sizes
// and times), to extract single items from it, or all of it, or test it. Reading and extracting run as jobs (the job
// queue, the status bar); an archive with encrypted names asks for its password first (found in the keyring if
// saved), and one with encrypted files asks when one of them is extracted.
//
// A file double-clicked is extracted to a temporary copy (~/.cache/kestrel-explorer/archive-files, copies not used for
// a day deleted) and opened with its app. Items dragged out carry MEMBERS_MIME, which Kestrel's folders and sidebar
// take (extracted straight into the folder, as a job), and, for other apps, a file list filled only when the drop asks
// for it: they're extracted to the temporary folder then (not over BIG_DRAG bytes, nor encrypted ones without their
// password, which only Kestrel takes).
#pragma once

#include "archive.h"

#include <QPointer>
#include <QWidget>

class MainWindow;
class QMimeData;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class ArchiveWindow : public QWidget {
    Q_OBJECT
public:
    // the window for `path`, raised if it's open already
    static ArchiveWindow *open(MainWindow *owner, const QString &path);
    QString path() const { return archive_path; }
    bool loaded() const { return is_loaded; }
    QStringList selected_members() const;           // the chosen items' paths in the archive (a folder's end in "/")
    void extract_selected(const QString &dest);     // as a job; to the archive's own folder with Extract Here
    void open_member(const QString &member);         // a temporary copy, opened with its app
    QMimeData *drag_data(const QStringList &members) const;   // what dragging these out carries

    static constexpr const char *MEMBERS_MIME = archive::MEMBERS_MIME;   // {"archive", "members"}
    static constexpr qint64 BIG_DRAG = 500LL * 1000 * 1000;
    static QString temp_root();   // ~/.cache/kestrel-explorer/archive-files
    // A drop carrying MEMBERS_MIME onto folder dest: extracted there as a job (by the archive's window when it's
    // open in this Kestrel, which knows its password). False if data isn't one.
    static bool drop_members(MainWindow *win, const QMimeData *data, const QString &dest);
    // Extract members of archive into dest as a job; after(made) on success. win: the archive's window (its password)
    static void extract_job(MainWindow *owner, QPointer<ArchiveWindow> win, const QString &archive,
                            const QStringList &members, const QString &dest, const QString &title,
                            std::function<void(const QStringList &)> after = nullptr);
    QTreeWidget *tree;                              // (also read by the tests)
    QLabel *status;
    QLineEdit *filter;
    QPushButton *extract_here, *extract_to, *extract_all, *test;

private:
    ArchiveWindow(MainWindow *owner, const QString &path);
    void load();
    struct Listing;
    void show_listing(Listing &listing);
    void apply_filter();
    void selection_changed();
    void context_menu(const QPoint &pos);
    void choose_and_extract();
    bool needs_password(const QStringList &members) const;   // an encrypted one among them (or inside a folder)
    qint64 size_of(const QStringList &members) const;         // their files' sizes, folders' contents included

    QPointer<MainWindow> owner;
    QString archive_path;
    QString password;   // once found (kept while the window is open)
    QList<archive::Entry> entries;
    bool is_loaded = false;
};
