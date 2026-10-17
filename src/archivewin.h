// Open as Archive: a window of its own listing what's inside an archive (a tree of its folders and files, with sizes
// and times), to extract single items from it, or all of it, or test it. Reading and extracting run as jobs (the job
// queue, the status bar); an archive with encrypted names asks for its password first (found in the keyring if
// saved), and one with encrypted files asks when one of them is extracted.
#pragma once

#include "archive.h"

#include <QPointer>
#include <QWidget>

class MainWindow;
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
    QTreeWidget *tree;                              // (also read by the tests)
    QLabel *status;
    QLineEdit *filter;
    QPushButton *extract_here, *extract_to, *extract_all, *test;

private:
    ArchiveWindow(MainWindow *owner, const QString &path);
    void load();
    void show_entries(const QList<archive::Entry> &entries);
    void apply_filter();
    void selection_changed();
    void context_menu(const QPoint &pos);
    void choose_and_extract();
    bool needs_password(const QStringList &members) const;   // an encrypted one among them (or inside a folder)

    QPointer<MainWindow> owner;
    QString archive_path;
    QString password;   // once found (kept while the window is open)
    QList<archive::Entry> entries;
    bool is_loaded = false;
};
