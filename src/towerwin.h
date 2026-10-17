// The Tower window (☰ → Tower…, with Preferences → Show developer tools; or `kes --tower-monitor`): what the tower
// (`kes --atc`, atc.h) is doing, live. The tower has no window of its own; this one asks it over D-Bus like any client,
// read-only: its status, the Kestrels checked in, every message it passes on, the job queue and its lanes, and the
// shared undo list.
#pragma once

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;

class TowerWindow : public QDialog {
    Q_OBJECT
public:
    explicit TowerWindow(QWidget *parent = nullptr);
    static TowerWindow *open(QWidget *parent);
    void refresh();
    QLabel *status;              // (also read by the tests)
    QTreeWidget *flights, *messages, *queue, *undo;

private:
    void heard(const QString &flight, const QString &message);
    QTimer timer;
    QPushButton *restart;
    QComboBox *filter;
    QCheckBox *paused;
    qint64 tower_pid = 0;
    QHash<QString, QString> names;   // bus name -> "pid 1234"
};
