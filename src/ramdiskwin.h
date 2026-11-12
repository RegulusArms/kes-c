// New RAM Disk… and Edit RAM Disk… (ramdisk.h): its name, a size limit of its own (a tmpfs mounted on a folder you
// pick, which takes the admin password) or none (a folder in /dev/shm), how long it lasts, and for one that keeps its
// files, the folder on disk they're saved to and how often.
#pragma once

#include "ramdisk.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;

class RamDiskWindow : public QDialog {
    Q_OBJECT
public:
    // existing: Edit (its kind and folder stay as they are); null: New
    RamDiskWindow(QWidget *parent, const ramdisk::Disk *existing = nullptr);
    void accept() override;   // checks what's chosen, then starts the job (and closes)
    ramdisk::Disk chosen() const;
    QString problem_text() const;   // why it can't be made as chosen (empty: it can)

    QLineEdit *name, *location, *save_to;
    QCheckBox *limit;
    QSpinBox *size, *interval;
    QComboBox *unit;
    QList<QRadioButton *> lives;   // in ramdisk::Life's order
    QLabel *problem, *note;
    QPushButton *ok;

private:
    void update();
    bool editing = false;
    ramdisk::Disk before;
};
