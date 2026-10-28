// Drive health from S.M.A.R.T., as udisks reports it (org.freedesktop.UDisks2 on the system bus): ATA/SATA drives
// through Drive.Ata, NVMe through NVMe.Controller (udisks 2.10 and later). Both are read without root; smartctl would
// need it, so it isn't used. USB enclosures often don't pass S.M.A.R.T. through: their drives are "not available".
//
// Shown on the Overview's drive cards and in a drive's Properties; Drive Health… (click the health line, or the card's
// menu) lists what udisks reports, in plain words. A failing drive is told once, as a desktop notification.
//
// Tests: KESTREL_TEST_HEALTH names a JSON file used instead of udisks: its objects as GetManagedObjects gives them
// ({path: {interface: {property: value}}}; a block's Device as a string), and a drive's "SmartGetAttributes" (the
// ATA list of [id, name, flags, value, worst, threshold, pretty, pretty_unit], or the NVMe map).
#pragma once

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QWidget;

namespace health {

enum class State { Unknown, Good, Warning, Failing };   // Unknown: udisks knows the drive but has no health for it

struct Info {
    State state = State::Unknown;
    QString summary;   // "Healthy", "3 bad sectors", "Failing", "Health not available"…
    QString drive;     // udisks' object path of the drive (for its details)
    QString id;        // the drive's udisks Id (model and serial): stays the same across ports and boots
    QString model;
    bool nvme = false;
    int temp_c = -1;           // -1: unknown
    qint64 power_on_s = -1;    // -1: unknown
    int drives = 1;            // a pool's: how many drives this is the worst of
};

Info from_ata(const QVariantMap &ata);     // the Drive.Ata properties
Info from_nvme(const QVariantMap &nvme);   // the NVMe.Controller properties
Info worst(const QList<Info> &infos);      // a pool's (ZFS): its worst drive's, the hottest temperature

// The health of the drive each device is on (/dev/sda2, /dev/mapper/…, /dev/nvme0n1p1, a /dev/disk/by-id link):
// only devices udisks knows. Empty when udisks can't be reached. Blocks on D-Bus (up to a few seconds): a worker's job.
QHash<QString, Info> read(const QStringList &devices);
// read()'s matching, given udisks' objects (converted to Qt types)
QHash<QString, Info> match(const QVariantMap &objects, const QStringList &devices);

QString line(const Info &info);    // one line for a card: "Healthy  ·  38 °C  ·  on for 2.1 years"
QString duration(qint64 secs);     // "15 hours", "300 days", "2.1 years"

// What udisks reports for a drive, in plain words
struct Row {
    QString what, value, note;   // note: "OK", "Past its limit", "Check this", or empty
    bool bad = false;
    QString tip;                 // udisks' name for it, and the normalised values
};
QList<Row> ata_rows(const QVariantList &attrs);   // SmartGetAttributes' list, each [id, name, flags, value, worst, threshold, pretty, pretty_unit]
QList<Row> nvme_rows(const QVariantMap &attrs);
QList<Row> details(const Info &info, QString *error);   // asks udisks (blocks: a worker's job)

// Drive Health window (free), its details read on a worker
void open_window(QWidget *parent, const QString &name, const Info &info);

// A desktop notification for a failing drive, the first time Kestrel sees it (remembered by its id, "health_warned")
void warn_failing(const QString &name, const Info &info);
QStringList warned_now();   // the drive ids notified by this Kestrel (tests)

}  // namespace health

Q_DECLARE_METATYPE(health::Info)
