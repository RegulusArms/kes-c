// The Overview's choices (Preferences → Overview: its sections and their order, the card size, the system drives, the
// usage display), its Starred and Frequent Folders sections, and each drive's health from udisks (health.h), with
// udisks played by a JSON file (KESTREL_TEST_HEALTH) and the drives added to the mount table (KESTREL_TEST_MOUNTS).
#include "common.h"

#include "dialogs.h"
#include "health.h"
#include "overview.h"
#include "prefswin.h"

#include <QJsonDocument>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>

#include <gio/gio.h>
#include <csignal>
#include <sys/resource.h>

using namespace test;

static const QString DRIVES = "/org/freedesktop/UDisks2/drives/";
static const QString BLOCKS = "/org/freedesktop/UDisks2/block_devices/";

// udisks as GetManagedObjects gives it: a failing ATA disk (and an unlocked LUKS volume on it), a healthy NVMe drive,
// a USB enclosure that passes no health through
static QVariantMap udisks_objects()
{
    auto block = [](const QString &dev, const QString &drive, const QString &backing = "/") {
        return QVariantMap{{"org.freedesktop.UDisks2.Block",
                            QVariantMap{{"Device", dev}, {"Drive", drive}, {"CryptoBackingDevice", backing}}}};
    };
    QVariantMap o;
    o[BLOCKS + "sdz1"] = block("/dev/kestrel-test-sdz1", DRIVES + "Old_Disk");
    o[BLOCKS + "dm_0"] = block("/dev/kestrel-test-dm0", "/", BLOCKS + "sdz1");
    o[BLOCKS + "nvme"] = block("/dev/kestrel-test-nvme0n1p1", DRIVES + "Fast_SSD");
    o[BLOCKS + "sdy1"] = block("/dev/kestrel-test-sdy1", DRIVES + "USB_Box");
    o[DRIVES + "Old_Disk"] = QVariantMap{
        {"org.freedesktop.UDisks2.Drive", QVariantMap{{"Id", "Old-Disk-1234"}, {"Vendor", "ATA"}, {"Model", "Old Disk"}}},
        {"org.freedesktop.UDisks2.Drive.Ata",
         QVariantMap{{"SmartSupported", true}, {"SmartEnabled", true}, {"SmartUpdated", 1}, {"SmartFailing", true},
                     {"SmartTemperature", 313.15}, {"SmartPowerOnSeconds", 69120000}, {"SmartNumBadSectors", 8}}},
        {"SmartGetAttributes",
         QVariantList{QVariantList{5, "reallocated-sector-count", 0, 90, 90, 36, 8, 3},
                      QVariantList{199, "udma-crc-error-count", 0, 200, 200, 0, 0, 1},
                      QVariantList{9, "power-on-hours", 0, 70, 70, 0, 69120000000LL, 2},
                      QVariantList{194, "temperature-celsius-2", 0, 40, 50, 0, 313150, 4}}}};
    o[DRIVES + "Fast_SSD"] = QVariantMap{
        {"org.freedesktop.UDisks2.Drive", QVariantMap{{"Id", "Fast-SSD-1"}, {"Model", "Fast SSD"}}},
        {"org.freedesktop.UDisks2.NVMe.Controller",
         QVariantMap{{"SmartUpdated", 1}, {"SmartCriticalWarning", QStringList()}, {"SmartTemperature", 308},
                     {"SmartPowerOnHours", 100}}},
        {"SmartGetAttributes", QVariantMap{{"percent_used", 3}, {"avail_spare", 100}, {"spare_thresh", 10}}}};
    o[DRIVES + "USB_Box"] = QVariantMap{{"org.freedesktop.UDisks2.Drive", QVariantMap{{"Id", "USB-Box"}}}};
    return o;
}

static DriveCard *card_at(OverviewPage *page, const QString &root)
{
    for (DriveCard *c : page->drive_cards())
        if (c->info.root == root)
            return c;
    return nullptr;
}

static BookmarkCard *folder_card(OverviewPage *page, const QString &target) { return page->card_for("starred", target); }

static QJsonObject frequent_file()
{
    return QJsonDocument::fromJson(read_file(join(CONFIG_DIR(), "frequent.json"))).object();
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    setup_app();
    QString home = HOME();

    // -- the sections' order, as overview_sections keeps it
    using overview::section_order;
    auto ids = [](const QList<QPair<QString, bool>> &order) {
        QStringList out;
        for (const auto &[id, shown] : order)
            out << (shown ? id : "-" + id);
        return out;
    };
    check(ids(section_order({})) == QStringList({"drives", "phones", "network", "bookmarks", "-starred", "-frequent"}),
          "by default the Overview shows Drives, Phones & Cameras, Network and Bookmarks; Starred and Frequent Folders "
          "are there to choose");
    check(ids(section_order({"-drives", "starred", "bogus", "starred"})) ==
              QStringList({"-drives", "starred", "phones", "network", "bookmarks", "-frequent"}),
          "a saved order keeps its sections' places and shown state, drops unknown ids and repeats, and adds the "
          "sections it doesn't name in their default place and state");
    check(overview::save_order(section_order({"network", "-phones"})) ==
              QStringList({"network", "-phones", "drives", "bookmarks", "-starred", "-frequent"}),
          "an order is saved with each section once, a hidden one as -id");

    // -- health from udisks' properties
    using health::State;
    health::Info i = health::from_ata({{"SmartSupported", false}});
    check(i.state == State::Unknown && i.summary == "Health not available", "a drive without S.M.A.R.T.: not available");
    i = health::from_ata({{"SmartSupported", true}, {"SmartEnabled", true}, {"SmartUpdated", 0}});
    check(i.state == State::Unknown && i.summary == "Health not read yet", "S.M.A.R.T. not read by udisks yet: said so");
    i = health::from_ata({{"SmartSupported", true}, {"SmartEnabled", true}, {"SmartUpdated", 5},
                          {"SmartNumBadSectors", 3}, {"SmartTemperature", 303.15}});
    check(i.state == State::Warning && i.summary == "3 bad sectors" && i.temp_c == 30,
          "bad sectors are a warning that counts them, with the temperature in °C");
    i = health::from_ata({{"SmartSupported", true}, {"SmartEnabled", true}, {"SmartUpdated", 5}, {"SmartFailing", true},
                          {"SmartNumBadSectors", 3}});
    check(i.state == State::Failing, "a drive udisks calls failing is failing, whatever else it says");
    i = health::from_ata({{"SmartSupported", true}, {"SmartEnabled", true}, {"SmartUpdated", 5},
                          {"SmartNumBadSectors", Q_INT64_C(4294967299)}});
    check(i.state == State::Warning && i.summary == "3 bad sectors",
          "a bad-sector total with vendor bits above the low 32 counts only the low 32 (3, not 4294967299)");
    i = health::from_ata({{"SmartSupported", true}, {"SmartEnabled", true}, {"SmartUpdated", 5},
                          {"SmartNumBadSectors", 0}, {"SmartPowerOnSeconds", 7200}});
    check(i.state == State::Good && i.summary == "Healthy" && i.power_on_s == 7200, "otherwise healthy");
    check(health::from_nvme({{"SmartUpdated", 1}, {"SmartCriticalWarning", QStringList{"spare"}}}).state == State::Warning &&
              health::from_nvme({{"SmartUpdated", 1}, {"SmartCriticalWarning", QStringList{"readonly"}}}).state ==
                  State::Failing &&
              health::from_nvme({{"SmartUpdated", 1}, {"SmartPowerOnHours", 100}}).power_on_s == 360000,
          "NVMe: little spare space is a warning, a read-only drive is failing, power-on hours count");
    health::Info good, warm;
    good.state = State::Good, good.temp_c = 30;
    warm.state = State::Warning, warm.temp_c = 45, warm.summary = "1 bad sector";
    health::Info pool = health::worst({good, warm});
    check(pool.state == State::Warning && pool.temp_c == 45 && pool.drives == 2 &&
              health::line(pool).contains("worst of 2 drives"),
          "a pool shows its worst drive, the hottest temperature, and that it's the worst of its drives");
    QHash<QString, health::Info> found =
        health::match(udisks_objects(), {"/dev/kestrel-test-dm0", "/dev/kestrel-test-nvme0n1p1", "/dev/kestrel-nowhere"});
    check(found.value("/dev/kestrel-test-dm0").id == "Old-Disk-1234" &&
              found.value("/dev/kestrel-test-nvme0n1p1").nvme && !found.contains("/dev/kestrel-nowhere"),
          "an unlocked encrypted volume is matched to the drive it's on, and a device udisks doesn't know gets nothing");
    QString old_line = health::line(found.value("/dev/kestrel-test-dm0"));
    check(old_line.contains("Failing: back up its files now") && old_line.contains("40 °C") &&
              old_line.contains("2.2 years"),
          "a card's health line: failing, what to do, the temperature and the time powered on (" + old_line + ")");
    QList<health::Row> rows = health::ata_rows(udisks_objects()[DRIVES + "Old_Disk"].toMap()["SmartGetAttributes"].toList());
    auto row = [&](const QString &what) {
        for (const health::Row &r : rows)
            if (r.what.startsWith(what))
                return r;
        return health::Row();
    };
    check(row("Bad sectors moved").value == "8 sectors" && row("Bad sectors moved").bad &&
              !row("Cable or connection").bad && row("Temperature").value == "40 °C" &&
              row("Time powered on").value == "2.2 years",
          "the details in plain words: reallocated sectors (any is a warning), cable errors, temperature, time on");
    rows = health::ata_rows({QVariantList{1, "raw-read-error-rate", 0, 30, 30, 36, 12, 1}});
    check(rows.value(0).bad && rows.value(0).note == "Past its limit", "a value at or below its limit is past its limit");
    check(health::nvme_rows({{"percent_used", 3}}).value(0).value == "3%", "NVMe details: its wear, as a percentage");

    // -- the page: drives added to the mount table, udisks played by a file
    QString disk = home_path("disk"), ssd = home_path("ssd"), usb = home_path("usb");
    for (const QString &d : {disk, ssd, usb})
        makedirs(d, true);
    write_text(home_path("udisks.json"), QJsonDocument::fromVariant(QVariantMap{{"objects", udisks_objects()}}).toJson());
    qputenv("KESTREL_TEST_HEALTH", home_path("udisks.json").toUtf8());
    QString mounts = disk + "|ext4|/dev/kestrel-test-sdz1;" + ssd + "|ext4|/dev/kestrel-test-nvme0n1p1;" + usb +
                     "|exfat|/dev/kestrel-test-sdy1";
    bool boot = isdir("/boot");
    if (boot)
        mounts += ";/boot|ext4|/dev/kestrel-test-boot";
    qputenv("KESTREL_TEST_MOUNTS", mounts.toUtf8());
    drives::MountWatch::instance()->mounts_changed();

    MainWindow *w = open_window({home});
    w->pane()->set_path(OVERVIEW);
    OverviewPage *page = w->pane()->overview_page();
    page->refresh();
    check(wait_for([&]() { return card_at(page, disk) && card_at(page, ssd) && card_at(page, usb); }),
          "the drives added to the mount table have cards");
    check(page->section_titles() == QStringList({"Drives", "Phones & Cameras", "Network", "Bookmarks"}),
          "the page shows the default sections, in order (" + page->section_titles().join(", ") + ")");

    // -- health on the cards
    DriveCard *old = card_at(page, disk);
    check(old && old->info.has_health && old->info.health.state == State::Failing && old->health_label &&
              old->health_label->text().contains("Failing") && old->styleSheet().contains("#c01c28"),
          "a failing drive's card says so in red, with a red border");
    DriveCard *fast = card_at(page, ssd);
    check(fast && fast->health_label && fast->health_label->text().contains("Healthy") &&
              fast->health_label->text().contains("35 °C"),
          "a healthy NVMe drive's card: healthy, with its temperature");
    DriveCard *box = card_at(page, usb);
    check(box && box->health_label && box->health_label->text().contains("Health not available"),
          "a drive in a USB enclosure without S.M.A.R.T.: health not available, rather than nothing");
    check(health::warned_now() == QStringList{"Old-Disk-1234"} &&
              settings().value("health_warned").toStringList().contains("Old-Disk-1234"),
          "the failing drive is told as a notification, and remembered");
    page->refresh();
    spin(500);
    check(health::warned_now() == QStringList{"Old-Disk-1234"}, "...only the first time Kestrel sees it");

    old = card_at(page, disk);   // (the refresh made new cards)
    if (old && old->health_label)
        Q_EMIT old->health_label->linkActivated("health");
    QDialog *hw = nullptr;
    QTreeWidget *tree = nullptr;
    bool details = wait_for([&]() {
        hw = nullptr;
        for (QWidget *t : QApplication::topLevelWidgets())
            if (t->objectName() == "drive_health" && t->isVisible())
                hw = qobject_cast<QDialog *>(t);
        tree = hw ? hw->findChild<QTreeWidget *>("health_rows") : nullptr;
        return tree && tree->topLevelItemCount() == 4;
    });
    QTreeWidgetItem *first = tree ? tree->topLevelItem(0) : nullptr;
    QString shown_rows;
    for (int r = 0; tree && r < tree->topLevelItemCount(); ++r)
        shown_rows += " | " + tree->topLevelItem(r)->text(0) + ": " + tree->topLevelItem(r)->text(1) + " " +
                      tree->topLevelItem(r)->text(2);
    check(details && is_free_window(hw) && first && first->text(1) == "8 sectors" && first->text(2) == "Check this",
          "clicking the health line opens Drive Health as a free window, with what the drive reports in plain words" +
              (details && is_free_window(hw) ? QString() : QString(" (window %1, free %2;%3)").arg(hw ? "shown" : "none", is_free_window(hw) ? "yes" : "no", shown_rows)));
    if (hw)
        hw->close();

    // ...and in the drive's Properties
    w->properties({disk});
    QLabel *prop_health = nullptr;
    check(wait_for([&]() {
              auto *pd = shown_window<PropertiesDialog>();
              prop_health = pd ? pd->findChild<QLabel *>("health") : nullptr;
              return prop_health && prop_health->text().contains("Failing");
          }),
          "a drive's Properties shows its health");
    if (auto *pd = shown_window<PropertiesDialog>())
        pd->close();

    prefs::set("overview_health", false);
    check(wait_for([&]() { return card_at(page, disk) && !card_at(page, disk)->health_label; }),
          "with drive health turned off, the cards have no health line");
    prefs::set("overview_health", true);

    // -- what the drive cards show
    prefs::set("overview_usage_bar", false);
    check(card_at(page, ssd) && !card_at(page, ssd)->findChild<QProgressBar *>("usage"), "the usage bar can be hidden");
    prefs::set("overview_usage_bar", true);
    check(card_at(page, ssd) && card_at(page, ssd)->findChild<QProgressBar *>("usage"), "...and shown again");
    prefs::set("overview_free_as", "percent");
    bool percent = false;
    for (QLabel *l : card_at(page, ssd)->findChildren<QLabel *>())
        percent = percent || (l->text().contains("% free of") && !l->text().contains("used"));
    check(percent, "free space can be shown as a percentage");
    prefs::reset({"overview_free_as"});
    if (boot) {
        check(!card_at(page, "/boot"), "the system's other mounts (/boot) aren't shown by default");
        prefs::set("overview_system", "all");
        bool shown = wait_for([&]() { return card_at(page, "/boot") && card_at(page, "/boot")->info.kind == "system"; });
        prefs::set("overview_system", "none");
        bool gone = wait_for([&]() {
            auto cards = page->drive_cards();
            return !cards.isEmpty() &&
                   std::none_of(cards.begin(), cards.end(), [](DriveCard *c) { return c->info.kind == "system"; });
        });
        check(shown && gone, "the system drives: with their other mounts, or none at all");
        prefs::reset({"overview_system"});
    } else {
        skip("the system drives' choice (no /boot folder here to mount a test drive on)");
    }

    // -- Starred
    QString starred_dir = home_path("Projects"), starred_file = home_path("notes.txt");
    makedirs(starred_dir, true);
    write_text(starred_file, "notes");
    places::set_starred({starred_dir, starred_file}, true);
    prefs::set("overview_sections", QStringList({"starred", "-drives", "bookmarks", "frequent"}));
    check(page->section_titles() == QStringList({"Starred", "Bookmarks", "Frequent Folders", "Phones & Cameras", "Network"}),
          "the sections chosen show in the order chosen, and a hidden one doesn't (" + page->section_titles().join(", ") + ")");
    check(page->card_targets("starred") == QStringList({starred_dir, starred_file}),
          "Starred shows the starred folder and file");
    QString opened;
    QObject::connect(page, &OverviewPage::open_file, [&](const QString &p) { opened = p; });
    if (BookmarkCard *c = folder_card(page, starred_file))
        Q_EMIT c->clicked();
    check(opened == starred_file, "clicking a starred file opens it in its app");
    if (BookmarkCard *c = folder_card(page, starred_dir))
        Q_EMIT c->clicked();
    check(w->pane()->dir() == starred_dir, "clicking a starred folder opens it");
    w->pane()->set_path(OVERVIEW);
    places::set_starred({starred_file}, false);
    check(page->card_targets("starred") == QStringList({starred_dir}), "an item unstarred leaves the section");

    // -- the card size
    prefs::set("overview_card_size", "small");
    bool small_cards = folder_card(page, starred_dir) && folder_card(page, starred_dir)->width() == 128;
    prefs::set("overview_card_size", "large");
    bool large_cards = folder_card(page, starred_dir) && folder_card(page, starred_dir)->width() == 216;
    prefs::set("overview_sections", QStringList({"drives"}));
    bool large_drive = card_at(page, ssd) && card_at(page, ssd)->width() == 420;
    check(small_cards && large_cards && large_drive, "cards can be small or large");
    prefs::reset({"overview_card_size"});

    // -- Frequent Folders: counted as folders are opened, written a moment later
    QString a = home_path("often"), b = home_path("sometimes"), c = home_path("other-kestrel");
    for (const QString &d : {a, b, c})
        makedirs(d, true);
    for (int n = 0; n < 3; ++n) {
        w->pane()->set_path(a);
        w->pane()->set_path(home);
    }
    w->pane()->set_path(b);
    w->pane()->set_path(OVERVIEW);
    places::save_frequent();
    QJsonObject f = frequent_file();
    check(f.value(a).toArray().at(0).toInt() == 3 && f.value(b).toArray().at(0).toInt() == 1,
          "each folder opened is counted, and written to frequent.json");
    // another Kestrel's visits, written meanwhile, stay
    f.insert(c, QJsonArray{10, 1});
    write_text(join(CONFIG_DIR(), "frequent.json"), QJsonDocument(f).toJson());
    w->pane()->set_path(b);
    w->pane()->set_path(OVERVIEW);
    places::save_frequent();
    f = frequent_file();
    check(f.value(c).toArray().at(0).toInt() == 10 && f.value(b).toArray().at(0).toInt() == 2,
          "a save adds this Kestrel's visits to what's in the file, keeping another Kestrel's");
    prefs::set("overview_sections", QStringList({"frequent"}));
    check(page->card_targets("frequent").mid(0, 3) == QStringList({c, home, a}),
          "Frequent Folders shows the most opened first (" + page->card_targets("frequent").join(", ") + ")");
    rmtree(c);
    page->refresh();
    check(wait_for([&]() { return !page->card_targets("frequent").contains(c); }), "a folder that's gone isn't shown");
    places::forget_frequent({a});
    page->refresh();
    check(wait_for([&]() { return !page->card_targets("frequent").contains(a); }) && !frequent_file().contains(a),
          "Remove from Frequent Folders forgets it");

    // a save that fails part-way (a file size limit) leaves the file as it was, and no part file
    QByteArray before = read_file(join(CONFIG_DIR(), "frequent.json"));
    w->pane()->set_path(b);
    w->pane()->set_path(OVERVIEW);
    struct rlimit old_limit;
    getrlimit(RLIMIT_FSIZE, &old_limit);
    std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit tiny = {16, old_limit.rlim_max};
    setrlimit(RLIMIT_FSIZE, &tiny);
    places::save_frequent();
    setrlimit(RLIMIT_FSIZE, &old_limit);
    QStringList parts = QDir(CONFIG_DIR()).entryList({"*.part"}, QDir::Files | QDir::Hidden);
    check(read_file(join(CONFIG_DIR(), "frequent.json")) == before && parts.isEmpty(),
          "a save that fails part-way keeps the old count whole, with no part file left");

    // the desktop's File History switch off: nothing counted
    if (has_schema_key("org.gnome.desktop.privacy", "remember-recent-files")) {
        GSettings *privacy = g_settings_new("org.gnome.desktop.privacy");
        g_settings_set_boolean(privacy, "remember-recent-files", false);
        QString hidden_dir = home_path("private");
        makedirs(hidden_dir, true);
        w->pane()->set_path(hidden_dir);
        w->pane()->set_path(OVERVIEW);
        places::save_frequent();
        bool not_counted = !frequent_file().contains(hidden_dir);
        g_settings_set_boolean(privacy, "remember-recent-files", true);
        g_object_unref(privacy);
        check(not_counted, "with the desktop's File History off, folders opened aren't counted");
    } else {
        skip("File History off (no GNOME privacy settings here)");
    }

    // -- Preferences → Overview: tick and order the sections
    prefs::reset({"overview_sections"});
    w->preferences();
    auto *pd = shown_window<PreferencesDialog>();
    auto *list = pd ? pd->findChild<QListWidget *>("overview_sections_list") : nullptr;
    QListWidgetItem *network = nullptr;
    for (int r = 0; list && r < list->count(); ++r)
        if (list->item(r)->data(Qt::UserRole).toString() == "network")
            network = list->item(r);
    if (network)
        network->setCheckState(Qt::Unchecked);
    check(network && prefs::get("overview_sections").toStringList().contains("-network") &&
              !page->section_titles().contains("Network"),
          "unticking a section in Preferences hides it on the Overview");
    if (list) {
        list->setCurrentRow(3);   // Bookmarks
        for (QPushButton *btn : list->parentWidget()->findChildren<QPushButton *>())
            if (btn->text() == "Up")
                btn->click();
    }
    check(page->section_titles() == QStringList({"Drives", "Phones & Cameras", "Bookmarks"}),
          "...and Up moves one earlier (" + page->section_titles().join(", ") + ")");
    qunsetenv("KESTREL_TEST_MOUNTS");
    finish();
}
