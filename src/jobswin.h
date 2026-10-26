// The Jobs window: every job of every Kestrel (the queue, jobs.h, and the shared task list), what each is doing and
// going to do, its log or its program's live output, and the queue's controls. Opened by clicking the status bar's
// job panel, or ☰ → Jobs.
#pragma once

#include <QDialog>
#include <QHash>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTreeWidget;
class QTreeWidgetItem;

class JobsWindow : public QDialog {
    Q_OBJECT
public:
    explicit JobsWindow(QWidget *parent = nullptr);
    // shows this Kestrel's Jobs window (made if there's none). A window of its own, not its opener's: jobs go on
    // when every Kestrel window has closed, and it's where they're watched then
    static JobsWindow *open(QWidget *parent);
    static bool is_open();
    void refresh();
    QTreeWidget *list;   // one row per job: its key in Qt::UserRole ("flight/id", or "done:<n>" for a finished one)
    QPushButton *pause_btn, *stop_btn, *now_btn, *up_btn, *down_btn;   // right under it: for its jobs
    // Below them (a quarter of the height), this Kestrel's thumbnail jobs (ThumbJob): browsing doesn't flood the list
    // of copies, moves and extractions. Their id in Qt::UserRole. Under it, their own Pause and Stop, and how
    // thumbnailing runs (thumb_mode, every Kestrel's).
    QTreeWidget *thumb_list;
    QPushButton *thumb_pause, *thumb_stop;
    QComboBox *thumb_mode;
    QSplitter *lists;
    void refresh_thumbs();

protected:
    void keyPressEvent(QKeyEvent *ev) override;
    void closeEvent(QCloseEvent *ev) override;

private:
    struct Row {
        QString key, flight, id, title, state, text;
        double fraction = -1;
        qint64 elapsed = 0;
        bool admin = false, local = false, finished = false;
        int finished_index = -1;
    };
    QList<Row> rows() const;
    const Row *selected_row() const;
    void show_details();
    void act(const QString &action);
    void save_log();
    QTimer timer;
    QComboBox *mode;
    QCheckBox *start_paused;
    QLabel *head;
    QListWidget *steps;
    QPlainTextEdit *log;
    QSplitter *split;
    QList<Row> shown;
    QString log_key;   // whose log the right side shows
};
