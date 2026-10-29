#include "health.h"

#include "fileops.h"
#include "util.h"
#include "windows.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QMutex>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <gio/gio.h>

#include <algorithm>
#include <cmath>

using namespace util;

namespace health {

static const char *UDISKS = "org.freedesktop.UDisks2";
static const char *BLOCK = "org.freedesktop.UDisks2.Block";
static const char *DRIVE = "org.freedesktop.UDisks2.Drive";
static const char *ATA = "org.freedesktop.UDisks2.Drive.Ata";
static const char *NVME = "org.freedesktop.UDisks2.NVMe.Controller";

static int kelvin_to_c(double k) { return k > 0 ? int(std::lround(k - 273.15)) : -1; }

Info from_ata(const QVariantMap &a)
{
    Info i;
    i.temp_c = kelvin_to_c(a.value("SmartTemperature").toDouble());
    qint64 on = a.value("SmartPowerOnSeconds").toLongLong();
    i.power_on_s = on > 0 ? on : -1;
    if (!a.value("SmartSupported").toBool()) {
        i.summary = tx("Health not available");
        return i;
    }
    if (!a.value("SmartEnabled").toBool()) {
        i.summary = tx("S.M.A.R.T. is switched off");
        return i;
    }
    if (a.value("SmartUpdated").toLongLong() <= 0) {   // udisks hasn't read it yet (it does every 10 minutes)
        i.summary = tx("Health not read yet");
        return i;
    }
    qint64 bad = a.value("SmartNumBadSectors", -1).toLongLong();
    // libatasmart's total (reallocated + pending) can carry vendor bits from above a raw value's low 32 bits (a Seagate
    // with 3 reallocated sectors reports 4294967299); the attributes in the details window show the real count
    if (bad > 0xFFFFFFFFLL)
        bad &= 0xFFFFFFFFLL;
    int failing = a.value("SmartNumAttributesFailing").toInt();
    QString test = a.value("SmartSelftestStatus").toString();
    if (a.value("SmartFailing").toBool()) {
        i.state = State::Failing;
        i.summary = tx("Failing");
    } else if (bad > 0) {
        i.state = State::Warning;
        i.summary = txn("%Ln bad sector(s)", bad);
    } else if (failing > 0) {
        i.state = State::Warning;
        i.summary = txn("%Ln value(s) past its limit", failing);   // (English: "1 value past its limit", "2 values past their limit")
    } else if (test.startsWith("error") || test == "fatal") {
        i.state = State::Warning;
        i.summary = tx("Its last self-test failed");
    } else {
        i.state = State::Good;
        i.summary = "Healthy";
    }
    return i;
}

Info from_nvme(const QVariantMap &n)
{
    Info i;
    i.nvme = true;
    i.temp_c = kelvin_to_c(n.value("SmartTemperature").toDouble());
    qint64 hours = n.value("SmartPowerOnHours").toLongLong();
    i.power_on_s = hours > 0 ? hours * 3600 : -1;
    if (n.value("SmartUpdated").toLongLong() <= 0) {
        i.summary = tx("Health not read yet");
        return i;
    }
    // the controller's critical warnings: the first three mean data is at risk
    QStringList warnings = n.value("SmartCriticalWarning").toStringList();
    struct Word {
        const char *key, *text;
        bool failing;   // data at risk
    };
    static const Word words[] = {
        {"readonly", TX_NOOP("Failing: it has made itself read-only"), true},
        {"degraded", TX_NOOP("Failing: its reliability is degraded"), true},
        {"volatile_mem", TX_NOOP("Failing: its backup memory has failed"), true},
        {"pmr_readonly", TX_NOOP("Failing: its persistent memory is read-only"), true},
        {"spare", TX_NOOP("Its spare space is running out"), false},
        {"temperature", TX_NOOP("Too hot or too cold"), false},
    };
    for (const Word &w : words)
        if (warnings.contains(w.key)) {
            i.state = w.failing ? State::Failing : State::Warning;
            i.summary = tx(w.text);
            return i;
        }
    if (!warnings.isEmpty()) {
        i.state = State::Warning;
        i.summary = tx("Warning: %1").arg(warnings.join(", "));
        return i;
    }
    i.state = State::Good;
    i.summary = tx("Healthy");
    return i;
}

Info worst(const QList<Info> &infos)
{
    if (infos.isEmpty())
        return Info();
    auto rank = [](State s) { return s == State::Failing ? 3 : s == State::Warning ? 2 : s == State::Unknown ? 1 : 0; };
    Info w = infos.first();
    int hottest = -1;
    for (const Info &i : infos) {
        if (rank(i.state) > rank(w.state))
            w = i;
        hottest = std::max(hottest, i.temp_c);
    }
    w.temp_c = hottest;
    w.power_on_s = -1;   // (each drive's own: in its details)
    w.drives = int(infos.size());
    return w;
}

// ---------------------------------------------------------------- udisks

// GVariant -> Qt: numbers, strings, byte strings (as text, without their trailing NUL), arrays, dictionaries
static QVariant qv(GVariant *v)
{
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_VARIANT)) {
        GVariant *in = g_variant_get_variant(v);
        QVariant r = qv(in);
        g_variant_unref(in);
        return r;
    }
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING))
        return QString::fromUtf8(g_variant_get_bytestring(v));
    switch (g_variant_classify(v)) {
    case G_VARIANT_CLASS_BOOLEAN: return bool(g_variant_get_boolean(v));
    case G_VARIANT_CLASS_BYTE: return int(g_variant_get_byte(v));
    case G_VARIANT_CLASS_INT16: return int(g_variant_get_int16(v));
    case G_VARIANT_CLASS_UINT16: return int(g_variant_get_uint16(v));
    case G_VARIANT_CLASS_INT32: return int(g_variant_get_int32(v));
    case G_VARIANT_CLASS_UINT32: return qint64(g_variant_get_uint32(v));
    case G_VARIANT_CLASS_INT64: return qint64(g_variant_get_int64(v));
    case G_VARIANT_CLASS_UINT64: return qint64(g_variant_get_uint64(v));
    case G_VARIANT_CLASS_DOUBLE: return g_variant_get_double(v);
    case G_VARIANT_CLASS_STRING:
    case G_VARIANT_CLASS_OBJECT_PATH:
    case G_VARIANT_CLASS_SIGNATURE: return QString::fromUtf8(g_variant_get_string(v, nullptr));
    case G_VARIANT_CLASS_ARRAY:
        if (g_variant_type_is_dict_entry(g_variant_type_element(g_variant_get_type(v)))) {
            QVariantMap m;
            GVariantIter it;
            g_variant_iter_init(&it, v);
            while (GVariant *e = g_variant_iter_next_value(&it)) {
                GVariant *k = g_variant_get_child_value(e, 0), *val = g_variant_get_child_value(e, 1);
                m.insert(qv(k).toString(), qv(val));
                g_variant_unref(k);
                g_variant_unref(val);
                g_variant_unref(e);
            }
            return m;
        }
        [[fallthrough]];
    case G_VARIANT_CLASS_TUPLE: {
        QVariantList l;
        GVariantIter it;
        g_variant_iter_init(&it, v);
        while (GVariant *e = g_variant_iter_next_value(&it)) {
            l << qv(e);
            g_variant_unref(e);
        }
        return l;
    }
    default: return QVariant();
    }
}

static QVariantMap test_data()
{
    QString f = qEnvironmentVariable("KESTREL_TEST_HEALTH");
    return f.isEmpty() ? QVariantMap() : QJsonDocument::fromJson(read_file(f)).toVariant().toMap();
}

static GDBusConnection *system_bus()
{
    return g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);   // (cached by GIO; nullptr without a system bus)
}

// udisks' method on an object; nullptr (and *error) when it fails
static GVariant *call(const QString &path, const char *iface, const char *method, GVariant *args, QString *error)
{
    GDBusConnection *bus = system_bus();
    if (!bus) {
        if (error)
            *error = "The system bus can't be reached.";
        if (args)
            g_variant_unref(g_variant_ref_sink(args));
        return nullptr;
    }
    GError *err = nullptr;
    GVariant *r = g_dbus_connection_call_sync(bus, UDISKS, path.toUtf8().constData(), iface, method, args, nullptr,
                                              G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &err);
    g_object_unref(bus);
    if (!r && error)
        *error = err ? QString::fromUtf8(err->message) : QString("udisks didn't answer.");
    if (err)
        g_error_free(err);
    return r;
}

static QString block_device(const QVariantMap &block) { return block.value("Device").toString(); }

// The drive a device is on: its block's Drive, else through what it's made from (an unlocked LUKS volume's backing
// device; an LVM or dm device's /sys slaves, when they are all on one drive)
static QString drive_of(const QString &dev, const QHash<QString, QString> &block_by_dev,
                        const QHash<QString, QVariantMap> &blocks, int depth = 0)
{
    if (depth > 4)
        return QString();
    QString real = realpath(dev);
    QString path = block_by_dev.value(dev, block_by_dev.value(real));
    if (!path.isEmpty()) {
        const QVariantMap &b = blocks[path];
        QString drive = b.value("Drive").toString();
        if (!drive.isEmpty() && drive != "/")
            return drive;
        QString backing = b.value("CryptoBackingDevice").toString();
        if (!backing.isEmpty() && backing != "/" && blocks.contains(backing))
            return drive_of(block_device(blocks[backing]), block_by_dev, blocks, depth + 1);
    }
    QString slaves = "/sys/class/block/" + basename(real) + "/slaves";
    QStringList found;
    for (const QString &s : QDir(slaves).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString d = drive_of("/dev/" + s, block_by_dev, blocks, depth + 1);
        if (d.isEmpty())
            return QString();
        if (!found.contains(d))
            found << d;
    }
    return found.size() == 1 ? found.first() : QString();
}

QHash<QString, Info> match(const QVariantMap &objects, const QStringList &devices)
{
    QHash<QString, QVariantMap> blocks;
    QHash<QString, QString> block_by_dev;   // a device, or one of its links, -> its block's object
    for (auto it = objects.begin(); it != objects.end(); ++it) {
        QVariantMap ifaces = it.value().toMap();
        if (!ifaces.contains(BLOCK))
            continue;
        QVariantMap b = ifaces.value(BLOCK).toMap();
        blocks.insert(it.key(), b);
        block_by_dev.insert(block_device(b), it.key());
        for (const QVariant &l : b.value("Symlinks").toList())
            block_by_dev.insert(l.toString(), it.key());
    }
    QHash<QString, Info> out;
    for (const QString &dev : devices) {
        QString drive = drive_of(dev, block_by_dev, blocks);
        if (drive.isEmpty() || !objects.contains(drive))
            continue;
        QVariantMap ifaces = objects.value(drive).toMap();
        Info i;
        if (ifaces.contains(ATA))
            i = from_ata(ifaces.value(ATA).toMap());
        else if (ifaces.contains(NVME))
            i = from_nvme(ifaces.value(NVME).toMap());
        else
            i.summary = tx("Health not available");   // (a USB enclosure, a card reader…)
        QVariantMap d = ifaces.value(DRIVE).toMap();
        i.drive = drive;
        i.id = d.value("Id").toString();
        i.model = QStringList{d.value("Vendor").toString().trimmed(), d.value("Model").toString().trimmed()}
                      .join(' ').trimmed();
        out.insert(dev, i);
    }
    return out;
}

static QVariantMap managed_objects()
{
    QVariantMap test = test_data();
    if (!test.isEmpty())
        return test.value("objects").toMap();
    GVariant *r = call("/org/freedesktop/UDisks2", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
                       nullptr);
    if (!r)
        return QVariantMap();
    GVariant *objs = g_variant_get_child_value(r, 0);
    QVariantMap m = qv(objs).toMap();
    g_variant_unref(objs);
    g_variant_unref(r);
    return m;
}

QHash<QString, Info> read(const QStringList &devices)
{
    QStringList devs;
    for (const QString &d : devices)
        if (d.startsWith("/dev/"))
            devs << d;
    return devs.isEmpty() ? QHash<QString, Info>() : match(managed_objects(), devs);
}

// ---------------------------------------------------------------- words

QString duration(qint64 secs)
{
    qint64 hours = secs / 3600;
    if (hours < 48)
        return txn("%Ln hour(s)", hours);
    qint64 days = hours / 24;
    if (days < 730)
        return txn("%Ln day(s)", days);
    return tx("%1 years").arg(i18n::decimal(days / 365.25, 1));
}

QString line(const Info &i)
{
    QStringList parts;
    parts << (i.state == State::Failing ? tx("⚠ %1: back up its files now").arg(i.summary)
              : i.state == State::Warning ? "⚠ " + i.summary
                                          : i.summary);
    if (i.drives > 1)
        parts.last() = tx("%1 (worst of %2 drives)").arg(parts.last(), QString::number(i.drives));
    if (i.temp_c >= 0)
        parts << tx("%1 °C").arg(i.temp_c);
    if (i.power_on_s > 0)
        parts << tx("on for %1").arg(duration(i.power_on_s));
    return parts.join("  ·  ");
}

namespace {
struct AtaWord {
    QString what;
    bool count_bad;   // any count above 0 is a warning
    QString tip;
};
}  // namespace

static const QHash<QString, AtaWord> &ata_words()
{
    static const QHash<QString, AtaWord> w = {
        {"reallocated-sector-count", {TX_NOOP("Bad sectors moved to spares (reallocated)"), true,
                                      TX_NOOP("Sectors that failed and were replaced by spare ones. A few can be stable; a "
                                              "growing number means the disk is wearing out.")}},
        {"current-pending-sector", {TX_NOOP("Sectors waiting to be checked (pending)"), true,
                                    TX_NOOP("Sectors that couldn't be read and will be checked again when written.")}},
        {"offline-uncorrectable", {TX_NOOP("Sectors that couldn't be read (uncorrectable)"), true,
                                   TX_NOOP("Found by the disk's own scan: the data in them is lost.")}},
        {"reported-uncorrect", {TX_NOOP("Errors it couldn't correct"), true, ""}},
        {"reallocated-event-count", {TX_NOOP("Times sectors were moved to spares"), true, ""}},
        {"udma-crc-error-count", {TX_NOOP("Cable or connection errors (CRC)"), true,
                                  TX_NOOP("Usually a loose or faulty cable or port, not the disk itself.")}},
        {"spin-retry-count", {TX_NOOP("Spin-up retries"), true,  TX_NOOP("The motor needed more than one try to reach speed.")}},
        {"power-on-hours", {TX_NOOP("Time powered on"), false, ""}},
        {"power-cycle-count", {TX_NOOP("Times switched on"), false, ""}},
        {"start-stop-count", {TX_NOOP("Spin-ups"), false, ""}},
        {"load-cycle-count", {TX_NOOP("Head load cycles"), false, ""}},
        {"temperature-celsius-2", {TX_NOOP("Temperature"), false, ""}},
        {"temperature-celsius", {TX_NOOP("Temperature"), false, ""}},
        {"airflow-temperature-celsius", {TX_NOOP("Air temperature"), false, ""}},
        {"wear-leveling-count", {TX_NOOP("Wear (100 is new)"), false,  TX_NOOP("A normalised value: it counts down as the cells wear.")}},
        {"media-wearout-indicator", {TX_NOOP("Wear (100 is new)"), false,  TX_NOOP("A normalised value: it counts down as the cells wear.")}},
        {"total-lbas-written", {TX_NOOP("Total written"), false, ""}},
        {"total-lbas-read", {TX_NOOP("Total read"), false, ""}},
        {"raw-read-error-rate", {TX_NOOP("Read error rate"), false,  TX_NOOP("A normalised value; its raw number means little on most drives.")}},
        {"seek-error-rate", {TX_NOOP("Seek error rate"), false,  TX_NOOP("A normalised value; its raw number means little on most drives.")}},
    };
    return w;
}

QList<Row> ata_rows(const QVariantList &attrs)
{
    QList<Row> rows;
    for (const QVariant &v : attrs) {
        QVariantList a = v.toList();
        if (a.size() < 8)
            continue;
        int id = a[0].toInt(), value = a[3].toInt(), worst_v = a[4].toInt(), threshold = a[5].toInt(), unit = a[7].toInt();
        QString name = a[1].toString();
        qint64 pretty = a[6].toLongLong();
        AtaWord w = ata_words().value(name, {name.isEmpty() ? tx("Attribute %1").arg(id) : name, false, ""});
        Row r;
        r.what = tx(w.what);
        switch (unit) {
        case 2: r.value = duration(pretty / 1000); break;                                      // milliseconds
        case 3: r.value = txn("%Ln sector(s)", pretty); break;
        case 4: r.value = tx("%1 °C").arg(std::lround(pretty / 1000.0 - 273.15)); break;   // millikelvin
        default: r.value = i18n::number(pretty);
        }
        if (name.startsWith("total-lbas"))
            r.value = human_size(pretty * 512);
        bool past = threshold > 0 && value <= threshold;
        bool counted = w.count_bad && pretty > 0;
        r.bad = past || counted;
        r.note = past ? tx("Past its limit") : counted ? tx("Check this") : tx("OK");
        r.tip = tx("%1 (attribute %2): normalised %3, worst %4, limit %5%6")
                    .arg(name, QString::number(id), QString::number(value), QString::number(worst_v),
                         QString::number(threshold), w.tip.isEmpty() ? QString() : "\n" + tx(w.tip));
        rows << r;
    }
    return rows;
}

QList<Row> nvme_rows(const QVariantMap &a)
{
    QList<Row> rows;
    auto add = [&rows](const QString &what, const QString &value, bool bad, const QString &key, const QString &tip = {}) {
        Row r;
        r.what = what;
        r.value = value;
        r.bad = bad;
        r.note = bad ? tx("Check this") : tx("OK");
        r.tip = key + (tip.isEmpty() ? QString() : "\n" + tip);
        rows << r;
    };
    if (a.contains("percent_used")) {
        int used = a.value("percent_used").toInt();
        add(tx("Wear (of its rated life)"), QString("%1%").arg(used), used >= 100, "percent_used",
            tx("How much of the writes it's rated for have been used; it can go past 100%."));
    }
    if (a.contains("avail_spare")) {
        int spare = a.value("avail_spare").toInt(), thresh = a.value("spare_thresh").toInt();
        add(tx("Spare space left"), tx("%1% (warns below %2%)").arg(spare).arg(thresh), spare < thresh, "avail_spare");
    }
    if (a.contains("media_errors"))
        add(tx("Data errors (media and integrity)"), QString::number(a.value("media_errors").toLongLong()),
            a.value("media_errors").toLongLong() > 0, "media_errors");
    if (a.contains("unsafe_shutdowns"))
        add(tx("Unsafe shutdowns (power lost)"), QString::number(a.value("unsafe_shutdowns").toLongLong()), false,
            "unsafe_shutdowns");
    if (a.contains("power_cycles"))
        add(tx("Times switched on"), QString::number(a.value("power_cycles").toLongLong()), false, "power_cycles");
    if (a.contains("total_data_written"))
        add(tx("Total written"), human_size(a.value("total_data_written").toLongLong()), false, "total_data_written");
    if (a.contains("total_data_read"))
        add(tx("Total read"), human_size(a.value("total_data_read").toLongLong()), false, "total_data_read");
    if (a.contains("ctrl_busy_time"))
        add(tx("Time busy"), duration(a.value("ctrl_busy_time").toLongLong() * 60), false, "ctrl_busy_time");
    if (a.contains("num_err_log_entries"))
        add(tx("Error log entries"), QString::number(a.value("num_err_log_entries").toLongLong()), false,
            "num_err_log_entries", tx("Errors the controller logged; most are harmless (a command the system tried)."));
    if (a.contains("warning_temp_time"))
        add(tx("Time too hot (warning)"), duration(a.value("warning_temp_time").toLongLong() * 60), false, "warning_temp_time");
    if (a.contains("critical_temp_time"))
        add(tx("Time too hot (critical)"), duration(a.value("critical_temp_time").toLongLong() * 60),
            a.value("critical_temp_time").toLongLong() > 0, "critical_temp_time");
    return rows;
}

QList<Row> details(const Info &info, QString *error)
{
    QVariantMap test = test_data();
    if (!test.isEmpty()) {
        QVariant attrs = test.value("objects").toMap().value(info.drive).toMap().value("SmartGetAttributes");
        return info.nvme ? nvme_rows(attrs.toMap()) : ata_rows(attrs.toList());
    }
    GVariant *r = call(info.drive, info.nvme ? NVME : ATA, "SmartGetAttributes",
                       g_variant_new("(@a{sv})", g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)), error);
    if (!r)
        return {};
    GVariant *attrs = g_variant_get_child_value(r, 0);
    QVariant a = qv(attrs);
    g_variant_unref(attrs);
    g_variant_unref(r);
    return info.nvme ? nvme_rows(a.toMap()) : ata_rows(a.toList());
}

// ---------------------------------------------------------------- window and notification

static QColor state_color(State s) { return s == State::Failing ? QColor("#c01c28") : QColor("#c64600"); }

void open_window(QWidget *parent, const QString &name, const Info &info)
{
    auto *d = new QDialog(parent);
    d->setObjectName("drive_health");
    d->setWindowTitle(tx("Drive Health — %1").arg(name));
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->resize(620, 460);
    auto *lay = new QVBoxLayout(d);
    auto *head = new QLabel(info.state == State::Failing ? tx("%1: back up its files now").arg(info.summary) : info.summary);
    QFont f = head->font();
    f.setPointSizeF(f.pointSizeF() * 1.3);
    f.setBold(true);
    head->setFont(f);
    head->setWordWrap(true);
    if (info.state == State::Failing || info.state == State::Warning)
        head->setStyleSheet("color: " + state_color(info.state).name());
    lay->addWidget(head);
    QStringList facts;
    if (!info.model.isEmpty())
        facts << info.model;
    if (info.temp_c >= 0)
        facts << tx("%1 °C").arg(info.temp_c);
    if (info.power_on_s > 0)
        facts << tx("powered on for %1").arg(duration(info.power_on_s));
    auto *sub = new QLabel(facts.join("  ·  "));
    sub->setTextFormat(Qt::PlainText);
    sub->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(sub);
    auto *about = new QLabel(tx("What the drive reports about itself (S.M.A.R.T.), through udisks. Hover a row for its "
                             "own name and values."));
    about->setWordWrap(true);
    about->setForegroundRole(QPalette::PlaceholderText);
    lay->addWidget(about);
    auto *tree = new QTreeWidget;
    tree->setObjectName("health_rows");
    tree->setHeaderLabels({tx("What"), tx("Value"), ""});
    tree->setRootIsDecorated(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tree->header()->setStretchLastSection(false);
    tree->addTopLevelItem(new QTreeWidgetItem(QStringList{tx("Reading…")}));
    lay->addWidget(tree, 1);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(bb, &QDialogButtonBox::rejected, d, &QDialog::close);
    lay->addWidget(bb);
    show_free(d);
    if (info.drive.isEmpty()) {
        tree->clear();
        tree->addTopLevelItem(new QTreeWidgetItem(QStringList{info.summary}));
        return;
    }
    QPointer<QTreeWidget> tp(tree);
    fileops::run_task(
        d, "",
        [info]() {
            QString error;
            QList<Row> rows = details(info, &error);
            QVariantList out;
            for (const Row &r : rows)
                out << QVariant(QStringList{r.what, r.value, r.note, r.tip, r.bad ? "1" : ""});
            return QVariant(QVariantMap{{"rows", out}, {"error", error}});
        },
        [tp, info](const QVariant &res) {
            if (!tp)
                return;
            tp->clear();
            QVariantMap m = res.toMap();
            QVariantList rows = m.value("rows").toList();
            if (rows.isEmpty())
                tp->addTopLevelItem(new QTreeWidgetItem(QStringList{
                    m.value("error").toString().isEmpty() ? QString("udisks reports nothing more for this drive.")
                                                          : "Couldn't read the details: " + m.value("error").toString()}));
            for (const QVariant &v : rows) {
                QStringList r = v.toStringList();
                auto *it = new QTreeWidgetItem(QStringList{r.value(0), r.value(1), r.value(2)});
                for (int c = 0; c < 3; ++c)
                    it->setToolTip(c, r.value(3));
                if (!r.value(4).isEmpty())
                    for (int c = 0; c < 3; ++c)
                        it->setForeground(c, state_color(State::Warning));
                tp->addTopLevelItem(it);
            }
        },
        true);
}

static QMutex warned_lock;
static QStringList warned_list;

QStringList warned_now()
{
    QMutexLocker l(&warned_lock);
    return warned_list;
}

void warn_failing(const QString &name, const Info &info)
{
    if (info.state != State::Failing)
        return;
    QString id = !info.id.isEmpty() ? info.id : info.drive;
    QStringList warned = settings().value("health_warned").toStringList();
    if (id.isEmpty() || warned.contains(id))
        return;
    settings().setValue("health_warned", warned << id);
    {
        QMutexLocker l(&warned_lock);
        warned_list << id;
    }
    if (!qEnvironmentVariable("KESTREL_TEST_HEALTH").isEmpty())
        return;   // (tests: noted, nothing shown on the desktop)
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus)
        return;
    QString body = info.model.isEmpty() ? tx("%1: %2. Back up its files now.").arg(name, info.summary)
                                        : tx("%1 (%2): %3. Back up its files now.").arg(name, info.model, info.summary);
    GVariantBuilder actions, hints;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(2));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(APP_ID));
    g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications", "Notify",
                           g_variant_new("(susssasa{sv}i)", APP_NAME, 0u, "drive-harddisk", "A drive may be failing",
                                         body.toUtf8().constData(), &actions, &hints, -1),
                           nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    g_object_unref(bus);
}

}  // namespace health
