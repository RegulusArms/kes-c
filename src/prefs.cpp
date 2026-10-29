#include "prefs.h"

#include "atc.h"
#include "util.h"

#include <QJsonArray>

using namespace util;

namespace prefs {

void (*apply_hook)(const QStringList &keys) = nullptr;

static QList<Pref> build()
{
    QList<Pref> l;
    auto add = [&](const QString &tab, const QString &group, const QString &key, const QString &label, Kind kind,
                   const QVariant &def, const QString &where = QString(), const QString &tip = QString()) -> Pref & {
        Pref p;
        p.key = key, p.tab = tab, p.group = group, p.label = label, p.kind = kind, p.def = def, p.where = where;
        p.tip = tip;
        l << p;
        return l.last();
    };
    auto range = [](Pref &p, int min, int max, const QString &suffix = QString()) {
        p.min = min, p.max = max, p.suffix = suffix;
    };
    const QList<QPair<QString, QVariant>> views = {{TX_NOOP("Grid"), "grid"}, {TX_NOOP("List"), "list"}};

    // -- General
    add(TX_NOOP("General"), "", "homepage", TX_NOOP("Homepage"), CUSTOM, "overview", TX_NOOP("the Home button (Alt+Home)"),
        TX_NOOP("Shown when Kestrel starts and by the Home button. Network locations are connected automatically."));
    add(TX_NOOP("General"), "", "single_click", TX_NOOP("Single-click to open items"), BOOL, false);
    add(TX_NOOP("General"), TX_NOOP("Opening files"), "image_opener", TX_NOOP("Open images with"), CUSTOM, "system");
    add(TX_NOOP("General"), TX_NOOP("Opening files"), "video_opener", TX_NOOP("Open videos with"), CUSTOM, "system");
    add(TX_NOOP("General"), TX_NOOP("Opening files"), "viewer_fullscreen", TX_NOOP("Open the image viewer full screen"), BOOL, false,
        TX_NOOP("the image viewer's F (it opens as it was last left)"));
    add(TX_NOOP("General"), "", "shared_undo", TX_NOOP("Share undo between all Kestrel windows"), BOOL, false, QString(),
        TX_NOOP("On: Ctrl+Z in any Kestrel window undoes the newest action from any of them. Off: each Kestrel undoes only "
        "what was done in it."));
    add(TX_NOOP("General"), "", "developer_tools", TX_NOOP("Show developer tools (☰ → Tower…)"), BOOL, false, QString(),
        TX_NOOP("The Tower window: what the tower that keeps Kestrel windows in sync is doing, live"));

    // -- Views (the main windows'; a file chooser's own are under Windows)
    add(TX_NOOP("Views"), TX_NOOP("View"), "view_mode", TX_NOOP("View"), CHOICE, "grid", TX_NOOP("☰ → View → Grid View / List View (Ctrl+1, Ctrl+2), "
        "the toolbar's view button")).choices = views;
    range(add(TX_NOOP("Views"), TX_NOOP("View"), "grid_size", TX_NOOP("Icon size in the grid"), INT, 160, TX_NOOP("the zoom slider, Ctrl++ / Ctrl+-")),
          48, 320, TX_NOOP(" px"));
    range(add(TX_NOOP("Views"), TX_NOOP("View"), "list_size", TX_NOOP("Icon size in the list"), INT, 28, TX_NOOP("the zoom slider, Ctrl++ / Ctrl+-")),
          16, 128, TX_NOOP(" px"));
    add(TX_NOOP("Views"), TX_NOOP("View"), "show_hidden", TX_NOOP("Show hidden files"), BOOL, false, TX_NOOP("☰ → View → Show Hidden Files (Ctrl+H)"));
    add(TX_NOOP("Views"), TX_NOOP("View"), "sidebar", TX_NOOP("Show the sidebar"), BOOL, true, TX_NOOP("☰ → View → Sidebar (F9)"));
    add(TX_NOOP("Views"), TX_NOOP("View"), "info_panel", TX_NOOP("Show the info panel"), BOOL, false, TX_NOOP("☰ → View → Info Panel (F3)"));
    add(TX_NOOP("Views"), TX_NOOP("Default sort (folders without one of their own)"), "sort_col", TX_NOOP("Sort by"), CHOICE, 0,
        TX_NOOP("the column headers, Sort By → Set as Default"))
        .choices = {{TX_NOOP("Name"), 0}, {TX_NOOP("Size"), 1}, {TX_NOOP("Type"), 2}, {TX_NOOP("Modified"), 3}};
    add(TX_NOOP("Views"), TX_NOOP("Default sort (folders without one of their own)"), "sort_order", TX_NOOP("Order"), CHOICE, 0,
        TX_NOOP("Sort By → Set as Default"))
        .choices = {{TX_NOOP("Ascending"), 0}, {TX_NOOP("Descending"), 1}};
    add(TX_NOOP("Views"), TX_NOOP("Default sort (folders without one of their own)"), "sort_group", TX_NOOP("Group"), CHOICE, 0,
        TX_NOOP("Sort By → Group By, Set as Default"))
        .choices = {{TX_NOOP("None"), 0}, {TX_NOOP("By period"), 1}, {TX_NOOP("By day"), 2}};
    add(TX_NOOP("Views"), TX_NOOP("Default sort (folders without one of their own)"), "sort_group_by", TX_NOOP("Group by the date"), CHOICE, 0,
        TX_NOOP("Sort By → Date, Set as Default"))
        .choices = {{TX_NOOP("Modified"), 0}, {TX_NOOP("Created"), 1}, {TX_NOOP("Taken (photos and videos)"), 2}};
    add(TX_NOOP("Views"), TX_NOOP("Default sort (folders without one of their own)"), "pinned_first", TX_NOOP("Pinned & Starred First"), BOOL,
        false, TX_NOOP("Sort By → Pinned && Starred First"));
    add(TX_NOOP("Views"), TX_NOOP("Folder previews"), "folder_previews", TX_NOOP("Show folder previews"), BOOL, true,
        TX_NOOP("the toolbar's preview switch, ☰ → View → Toggle Folder Previews (Ctrl+Shift+P)"));
    add(TX_NOOP("Views"), TX_NOOP("Folder previews"), "list_folder_previews", TX_NOOP("Previews for folders in list view"), BOOL, false);
    range(add(TX_NOOP("Views"), TX_NOOP("Folder previews"), "folder_count", TX_NOOP("Images in folder previews"), INT, 4), 1, 4);
    add(TX_NOOP("Views"), TX_NOOP("Folder previews"), "folder_order", TX_NOOP("Folder preview picks"), CHOICE, "name")
        .choices = {{TX_NOOP("First by name"), "name"},
                    {TX_NOOP("Last by name"), "last"},
                    {TX_NOOP("Newest first"), "newest"},
                    {TX_NOOP("Oldest first"), "oldest"},
                    {TX_NOOP("Beginning, middle and end"), "spread"}};
    add(TX_NOOP("Views"), TX_NOOP("Folder previews"), "folder_color", TX_NOOP("Folder colour"), CUSTOM, "accent",
        TX_NOOP("a folder's Folder Colour menu (for that folder)"));
    add(TX_NOOP("Views"), TX_NOOP("Search"), "search_recursive", TX_NOOP("Search in subfolders"), BOOL, false, TX_NOOP("the search bar (Ctrl+F)"));
    add(TX_NOOP("Views"), TX_NOOP("Search"), "search_contents", TX_NOOP("Search inside files"), BOOL, false, TX_NOOP("the search bar (Ctrl+F)"));

    // -- Sidebar
    add(TX_NOOP("Sidebar"), "", "sidebar_sections", TX_NOOP("Sections"), CUSTOM, QStringList(),
        TX_NOOP("drag a section's heading in the sidebar; click it to collapse it"));

    // -- Overview
    add(TX_NOOP("Overview"), "", "overview_sections", TX_NOOP("Sections"), CUSTOM, QStringList(), "",
        TX_NOOP("Ticked sections show on the Overview, in this order"));
    add(TX_NOOP("Overview"), "", "overview_card_size", TX_NOOP("Card size"), CHOICE, "normal")
        .choices = {{TX_NOOP("Small"), "small"}, {TX_NOOP("Normal"), "normal"}, {TX_NOOP("Large"), "large"}};
    add(TX_NOOP("Overview"), TX_NOOP("Drives"), "overview_system", TX_NOOP("System drives"), CHOICE, "root")
        .choices = {{TX_NOOP("The system drive only"), "root"},
                    {TX_NOOP("The system drive and its other mounts (/boot, /boot/efi…)"), "all"},
                    {TX_NOOP("None"), "none"}};
    add(TX_NOOP("Overview"), TX_NOOP("Drives"), "overview_usage_bar", TX_NOOP("Show the usage bar"), BOOL, true);
    add(TX_NOOP("Overview"), TX_NOOP("Drives"), "overview_free_as", TX_NOOP("Free space as"), CHOICE, "size")
        .choices = {{TX_NOOP("A size (120 GB free of 1 TB)"), "size"}, {TX_NOOP("A percentage (12% free of 1 TB)"), "percent"}};
    add(TX_NOOP("Overview"), TX_NOOP("Drives"), "overview_health", TX_NOOP("Show each drive's health (S.M.A.R.T.)"), BOOL, true, "",
        TX_NOOP("From udisks: healthy, bad sectors or failing, the temperature and the time powered on. A failing drive is "
        "also told once as a notification. Drives in USB enclosures often don't report it."));

    // -- Images & Videos
    range(add(TX_NOOP("Images & Videos"), "", "slideshow_secs", TX_NOOP("Slideshow interval"), INT, 4, TX_NOOP("the image viewer")), 1, 120, TX_NOOP(" s"));
    add(TX_NOOP("Images & Videos"), TX_NOOP("Image viewer"), "viewer_filmstrip", TX_NOOP("Show the filmstrip"), BOOL, true, TX_NOOP("T in the image viewer"));
    add(TX_NOOP("Images & Videos"), TX_NOOP("Image viewer"), "viewer_info_panel", TX_NOOP("Show the info panel"), BOOL, false, TX_NOOP("I in the image viewer"));
    add(TX_NOOP("Images & Videos"), TX_NOOP("Image viewer"), "viewer_keep_zoom", TX_NOOP("Keep the zoom from image to image"), BOOL, false,
        TX_NOOP("K in the image viewer"));
    add(TX_NOOP("Images & Videos"), TX_NOOP("Image viewer"), "viewer_save_rotation", TX_NOOP("Rotations (R, L, H)"), CHOICE, "ask",
        TX_NOOP("the image viewer's Always Save"),
        TX_NOOP("How an image turned in the viewer is saved when you leave it: only its orientation tag changes (or a "
        "lossless format is written again), so nothing is lost. Ctrl+S saves it at once."))
        .choices = {{TX_NOOP("Ask whether to save them"), "ask"}, {TX_NOOP("Always save them"), "always"},
                    {TX_NOOP("Never save them (only for viewing)"), "never"}};
    const QString wiping = TX_NOOP("Wipe Metadata (an image's right-click menu)");
    add(TX_NOOP("Images & Videos"), wiping, "metadata_wipe", TX_NOOP("Wiping an image's metadata"), CHOICE, "copy", QString(),
        TX_NOOP("Wipe Metadata… takes out the camera details, GPS location, dates, comments and image-generation prompts, "
        "without touching the pixels."))
        .choices = {{TX_NOOP("Keep the original and make a copy without metadata"), "copy"},
                    {TX_NOOP("Wipe the original (no backup)"), "original"}};
    add(TX_NOOP("Images & Videos"), wiping, "metadata_wipe_keep_basic", TX_NOOP("Keep the orientation and colour profile"), BOOL, true,
        QString(), TX_NOOP("Without them, a photo taken turned shows sideways, and colours can look different."));
    add(TX_NOOP("Images & Videos"), "", "play_gifs", TX_NOOP("Play animated GIFs in the file view"), BOOL, false);
    add(TX_NOOP("Images & Videos"), "", "play_webm", TX_NOOP("Play WebM videos in the file view (silent looping previews)"), BOOL, false);
    range(add(TX_NOOP("Images & Videos"), "", "thumb_max_mb", TX_NOOP("Don't make previews of files larger than"), INT, 200), 1, 10000,
          TX_NOOP(" MB"));
    range(add(TX_NOOP("Images & Videos"), "", "thumb_job_minutes", TX_NOOP("Stop making a folder's previews this long after leaving it"),
              INT, 3, QString(),
              TX_NOOP("Its thumbnail job in the Jobs window waits behind the folder you're in, and stops after this long. "
              "What it made is kept.")),
          1, 240, TX_NOOP(" min"));
    add(TX_NOOP("Images & Videos"), "", "thumb_mode", TX_NOOP("Making thumbnails and folder previews"), CHOICE, "wait",
        TX_NOOP("the Jobs window (Ctrl+Shift+J), under its thumbnail jobs"),
        TX_NOOP("Each folder you look at has a thumbnail job in the Jobs window. Tapes and floppies only get previews where "
        "allowed (a drive's menu → Show Previews on This Drive)."))
        .choices = {{TX_NOOP("One per drive, even while the drive is in use by other jobs"), "drive"},
                    {TX_NOOP("Wait until the drive is free of other jobs"), "wait"},
                    {TX_NOOP("Paused: none made until this is changed"), "paused"}};
    add(TX_NOOP("Images & Videos"), "", "slow_previews", TX_NOOP("Previews on tapes and floppies"), CUSTOM, QStringList(),
        TX_NOOP("a drive's menu → Show Previews on This Drive"));

    // -- Files
    add(TX_NOOP("Files"), TX_NOOP("Checksums"), "checksum_algorithms", TX_NOOP("Algorithms"), CUSTOM, "sha256",
        TX_NOOP("Create Checksum File…, Compress… (tar formats)"));
    add(TX_NOOP("Files"), TX_NOOP("Checksums"), "checksum_one_file", TX_NOOP("Checksum files"), CHOICE, false,
        TX_NOOP("Create Checksum File…, Compress… (tar formats)"))
        .choices = {{TX_NOOP("One file for each algorithm"), false}, {TX_NOOP("One file with every algorithm"), true}};
    add(TX_NOOP("Files"), TX_NOOP("Jobs"), "jobs_mode", TX_NOOP("Run jobs"), CHOICE, "drive", TX_NOOP("the Jobs window (Ctrl+Shift+J)"))
        .choices = {{TX_NOOP("By drive (jobs on different drives side by side)"), "drive"}, {TX_NOOP("One at a time"), "one"},
                    {TX_NOOP("All at once (every job starts as it's queued, even on a drive in use)"), "all"}};
    add(TX_NOOP("Files"), TX_NOOP("Jobs"), "jobs_start_paused", TX_NOOP("Start new jobs paused"), BOOL, false, TX_NOOP("the Jobs window (Ctrl+Shift+J)"));

    const QString dupes = TX_NOOP("Find Duplicates");
    const QString in_dupes = TX_NOOP("the Find Duplicates window (a folder's menu → Find Duplicates…)");
    range(add(TX_NOOP("Files"), dupes, "dupes_min_kb", TX_NOOP("Only files of at least"), INT, 1, in_dupes), 0, 100000000, TX_NOOP(" KB"));
    add(TX_NOOP("Files"), dupes, "dupes_recursive", TX_NOOP("Look in subfolders"), BOOL, true, in_dupes);
    add(TX_NOOP("Files"), dupes, "dupes_hidden", TX_NOOP("Include hidden files"), BOOL, false, in_dupes);
    add(TX_NOOP("Files"), dupes, "dupes_skip_hardlinks", TX_NOOP("Skip hard links (they're the same file, not copies)"), BOOL, true, in_dupes);

    const QString tapes = TX_NOOP("Tapes (copying onto an LTFS tape)");
    add(TX_NOOP("Files"), tapes, "tape_writer", TX_NOOP("Write files with"), CHOICE, "ask", TX_NOOP("asked when a copy onto a tape starts"),
        TX_NOOP("Kestrel's copy reads ahead into a buffer in memory, so the tape keeps streaming when the source slows down. "
        "dd writes each file straight from the source, in 512 KB blocks; the Jobs window shows its output."))
        .choices = {{TX_NOOP("Ask each time"), "ask"}, {TX_NOOP("Kestrel's copy (a buffer in memory keeps the tape streaming)"), "kestrel"},
                    {TX_NOOP("dd (each file straight from the source)"), "dd"}};
    range(add(TX_NOOP("Files"), tapes, "tape_buffer_keep", TX_NOOP("Memory kept free for other programs"), INT, 10, QString(),
              TX_NOOP("Copies onto a tape go through a buffer in memory, so the drive keeps streaming instead of stopping and "
              "rewinding whenever the source slows down. It uses the memory available, less this share of the total.")),
          0, 90, " %");
    range(add(TX_NOOP("Files"), tapes, "tape_buffer_gb", TX_NOOP("Largest buffer (0: as much as is safe)"), INT, 16), 0, 4096,
          TX_NOOP(" GB"));
    add(TX_NOOP("Files"), tapes, "tape_buffer_now", TX_NOOP("The buffer now"), CUSTOM, QVariant());
    add(TX_NOOP("Files"), tapes, "tape_min_speed", TX_NOOP("The drive's lowest streaming speed"), CHOICE, 1000000, QString(),
        TX_NOOP("When the source reads at least this fast, the tape writes at the source's pace (the drive slows down to match "
        "it); slower, it waits for the buffer to fill and writes in full-speed runs. The speeds are approximate: check "
        "your drive's specifications."))
        .choices = {{TX_NOOP("Not sure: always refill the buffer before writing again"), 1000000},
                    {TX_NOOP("LTO-4 (about 40 MB/s)"), 40},
                    {TX_NOOP("LTO-5 (about 47 MB/s)"), 47},
                    {TX_NOOP("LTO-6 (about 54 MB/s)"), 54},
                    {TX_NOOP("LTO-7 (about 80 MB/s)"), 80},
                    {TX_NOOP("LTO-8 or LTO-9 (about 112 MB/s)"), 112}};

    // -- Archives
    add(TX_NOOP("Archives"), TX_NOOP("Compressing"), "archive/format", TX_NOOP("Format"), CUSTOM, QString(),
        TX_NOOP("Compress… (the last one used), Compress to “name.ext”"));
    add(TX_NOOP("Archives"), TX_NOOP("Compressing"), "archive_formats", TX_NOOP("Each format's options"), CUSTOM, QVariant(), TX_NOOP("Compress…"),
        TX_NOOP("What Compress… starts with for each format; it remembers them as you use it."));
    add(TX_NOOP("Archives"), TX_NOOP("Extract To…"), "archive/x_subfolder", TX_NOOP("Into a new folder named after the archive"), BOOL, true,
        TX_NOOP("Extract To…"));
    add(TX_NOOP("Archives"), TX_NOOP("Extract To…"), "archive/x_overwrite", TX_NOOP("Existing files"), CHOICE, "rename", TX_NOOP("Extract To…"))
        .choices = {{TX_NOOP("Keep both (rename)"), "rename"}, {TX_NOOP("Replace"), "overwrite"}, {TX_NOOP("Skip"), "skip"}};
    add(TX_NOOP("Archives"), TX_NOOP("Extract To…"), "archive/x_trash", TX_NOOP("Move the archive to the trash afterwards"), BOOL, false,
        TX_NOOP("Extract To…"));
    add(TX_NOOP("Archives"), TX_NOOP("Extract To…"), "archive/x_open", TX_NOOP("Open the extracted folder"), BOOL, false, TX_NOOP("Extract To…"));
    add(TX_NOOP("Archives"), TX_NOOP("Passwords"), "archive_passwords", TX_NOOP("Saved archive passwords"), CUSTOM, QVariant(),
        TX_NOOP("Passwords and Keys (Seahorse)"));

    // -- Windows
    add(TX_NOOP("Windows"), "", "open_in_tabs", TX_NOOP("Open folders from other apps as tabs in an open Kestrel window"), BOOL, false,
        QString(),
        TX_NOOP("On: a folder opened from another app (or with “Show in folder”) becomes a tab in the Kestrel window you used "
        "last. Off: it opens in a new window."));
    const QString chooser = TX_NOOP("The file chooser (Open and Save windows of other apps)");
    const QString in_chooser = TX_NOOP("the same places in a file chooser window");
    add(TX_NOOP("Windows"), chooser, "chooser/view_mode", TX_NOOP("View"), CHOICE, QVariant(), in_chooser).choices = views;
    range(add(TX_NOOP("Windows"), chooser, "chooser/grid_size", TX_NOOP("Icon size in the grid"), INT, 96, in_chooser), 48, 320, TX_NOOP(" px"));
    range(add(TX_NOOP("Windows"), chooser, "chooser/list_size", TX_NOOP("Icon size in the list"), INT, 24, in_chooser), 16, 128, TX_NOOP(" px"));
    add(TX_NOOP("Windows"), chooser, "chooser/show_hidden", TX_NOOP("Show hidden files"), BOOL, QVariant(), in_chooser);
    add(TX_NOOP("Windows"), chooser, "chooser/sidebar", TX_NOOP("Show the sidebar"), BOOL, QVariant(), in_chooser);
    add(TX_NOOP("Windows"), chooser, "chooser/info_panel", TX_NOOP("Show the info panel"), BOOL, QVariant(), in_chooser);
    add(TX_NOOP("Windows"), chooser, "chooser_start", TX_NOOP("A Save window starts in"), CHOICE, "folder", QString(),
        TX_NOOP("Where a Save window of another app opens. An app saving over an existing file always opens in its folder; "
        "Open windows start in the folder the app suggests either way."))
        .choices = {{TX_NOOP("The folder the app suggests, else where the last one saved"), "folder"},
                    {TX_NOOP("Its own Overview, with the save locations"), "overview"}};
    add(TX_NOOP("Windows"), chooser, "chooser_places", TX_NOOP("Save locations"), CUSTOM, QStringList(),
        TX_NOOP("a Save window's Overview (Add Folder…; right-click one to move or remove it)"),
        TX_NOOP("Folders shown first on a Save window's Overview, for saving from other apps. Only for Save windows: adding "
        "one doesn't star or bookmark it."));
    return l;
}

const QList<Pref> &all()
{
    static const QList<Pref> list = build();
    return list;
}

const Pref *find(const QString &key)
{
    for (const Pref &p : all())
        if (p.key == key)
            return &p;
    return nullptr;
}

QStringList tabs()
{
    QStringList out;
    for (const Pref &p : all())
        if (!out.contains(p.tab))
            out << p.tab;
    return out;
}

QVariant get(const QString &key)
{
    QSettings &s = settings();
    if (s.contains(key))
        return s.value(key);
    const Pref *p = find(key);
    if (key.startsWith("chooser/")) {   // until a chooser has its own: the main windows' (sizes: the chooser's smaller)
        QString main = key.mid(8);
        if (main != "grid_size" && main != "list_size")
            return get(main);
    }
    return p ? p->def : QVariant();
}

static void changed(const QStringList &keys)
{
    settings().sync();   // the other Kestrels read the file as soon as they hear
    if (apply_hook)
        apply_hook(keys);
    notify(keys);
    atc::announce("settings", {{"keys", QJsonArray::fromStringList(keys)}});
}

void set(const QString &key, const QVariant &value) { set_many({{key, value}}); }

void set_many(const QList<QPair<QString, QVariant>> &values)
{
    QStringList keys;
    for (const auto &[k, v] : values) {
        if (v.isValid())
            settings().setValue(k, v);
        else
            settings().remove(k);
        keys << k;
    }
    if (!keys.isEmpty())
        changed(keys);
}

void reset(const QStringList &keys)
{
    for (const QString &k : keys)
        settings().remove(k);
    if (!keys.isEmpty())
        changed(keys);
}

Notifier *Notifier::instance()
{
    static Notifier *n = new Notifier;
    return n;
}

void notify(const QStringList &keys) { Q_EMIT Notifier::instance()->changed(keys); }

}  // namespace prefs
