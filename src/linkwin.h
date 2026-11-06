// The Link window: making, viewing and changing links (Links & Shortcuts → Link… and Edit Link…, and the Edit… button
// beside a link's target in Properties).
//
// Two browsers side by side, the file chooser's (MainWindow::embed): on the left what the links point to ("Points to"),
// on the right the folder they go in ("Link goes in"). The left side's picks show under the right side as the
// links-to-be in that folder, ghosted, each renamable in place. Add to List keeps them in the list below, which holds
// every link to make or change, from any folders, with its problems; clicking one shows it on both sides to change it.
// OK shows everything that will be made or changed and, once that's confirmed, checks it all again and does it as one
// job (links::commit; its log in the Jobs window), undone as one step.
#pragma once

#include "links.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

class MainWindow;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTreeWidget;

class LinkWindow : public QWidget {
    Q_OBJECT
public:
    // picks: what to link to (selected on the left, in their folder); folder: where the links go (the right side; empty:
    // win's folder); edit: existing symbolic links to change, in the list from the start
    static LinkWindow *open(MainWindow *win, const QStringList &picks, const QString &folder,
                            const QStringList &edit = {});

    // for tests
    QPointer<MainWindow> left, right;
    QComboBox *kind;
    QListWidget *ghosts;
    QTreeWidget *list;
    QPushButton *add_btn, *change_btn, *remove_btn, *ok_btn;
    QList<links::Link> pending;
    void add_picks();         // Add to List
    void change_selected();   // Change Link: the left side's pick and the right side's folder for the one selected
    void remove_selected();
    void commit();            // OK: checks, the summary, then the job
    // the summary's final OK (commit() asks first): checked again, then made as one job
    void run(const QList<links::Link> &batch);
    QStringList picks() const;   // what's selected on the left
    QString dest() const;        // the folder shown on the right (empty: not a real folder)

protected:
    void closeEvent(QCloseEvent *ev) override;

private:
    LinkWindow(MainWindow *win, const QString &left_dir, const QString &right_dir);
    QPointer<MainWindow> win;
    QLabel *ghost_label, *pick_info;
    QPushButton *find_btn;
    QHash<QString, QString> preview_names;   // a pick's preview renamed before it's added: by what it points to
    int next_id = 1;
    bool filling = false;   // the widgets are being filled: not the user's changes
    bool running = false;   // the job is making them
    bool close_confirmed = false;   // closing with links in the list: asked
    void add(links::Link l);
    QSet<QString> taken(int except = 0) const;   // the paths of the list's links
    links::Link *find(int id);
    QList<int> selected_ids() const;
    void refresh();          // the list, the ghosts and the buttons
    void refresh_ghosts();
    void update_buttons();
    void update_pick_info();
    void list_selected();    // a link clicked in the list: shown on both sides
    void kind_changed();
    void ghost_renamed(QListWidgetItem *it);
    void find_names();       // the pick's other names (hard links): a job
    void show_in(MainWindow *side, const QString &folder, const QStringList &select);
};
