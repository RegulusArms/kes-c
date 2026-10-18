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
    const QList<QPair<QString, QVariant>> views = {{"Grid", "grid"}, {"List", "list"}};

    // -- General
    add("General", "", "homepage", "Homepage", CUSTOM, "overview", "the Home button (Alt+Home)",
        "Shown when Kestrel starts and by the Home button. Network locations are connected automatically.");
    add("General", "", "single_click", "Single-click to open items", BOOL, false);
    add("General", "Opening files", "image_opener", "Open images with", CUSTOM, "system");
    add("General", "Opening files", "video_opener", "Open videos with", CUSTOM, "system");
    add("General", "Opening files", "viewer_fullscreen", "Open the image viewer full screen", BOOL, false);
    add("General", "", "shared_undo", "Share undo between all Kestrel windows", BOOL, false, QString(),
        "On: Ctrl+Z in any Kestrel window undoes the newest action from any of them. Off: each Kestrel undoes only "
        "what was done in it.");
    add("General", "", "developer_tools", "Show developer tools (☰ → Tower…)", BOOL, false, QString(),
        "The Tower window: what the tower that keeps Kestrel windows in sync is doing, live");

    // -- Views (the main windows'; a file chooser's own are under Windows)
    add("Views", "View", "view_mode", "View", CHOICE, "grid", "☰ → View → Grid View / List View (Ctrl+1, Ctrl+2), "
        "the toolbar's view button").choices = views;
    range(add("Views", "View", "grid_size", "Icon size in the grid", INT, 160, "the zoom slider, Ctrl++ / Ctrl+-"),
          48, 320, " px");
    range(add("Views", "View", "list_size", "Icon size in the list", INT, 28, "the zoom slider, Ctrl++ / Ctrl+-"),
          16, 128, " px");
    add("Views", "View", "show_hidden", "Show hidden files", BOOL, false, "☰ → View → Show Hidden Files (Ctrl+H)");
    add("Views", "View", "sidebar", "Show the sidebar", BOOL, true, "☰ → View → Sidebar (F9)");
    add("Views", "View", "info_panel", "Show the info panel", BOOL, false, "☰ → View → Info Panel (F3)");
    add("Views", "Default sort (folders without one of their own)", "sort_col", "Sort by", CHOICE, 0,
        "the column headers, Sort By → Set as Default")
        .choices = {{"Name", 0}, {"Size", 1}, {"Type", 2}, {"Modified", 3}};
    add("Views", "Default sort (folders without one of their own)", "sort_order", "Order", CHOICE, 0,
        "Sort By → Set as Default")
        .choices = {{"Ascending", 0}, {"Descending", 1}};
    add("Views", "Default sort (folders without one of their own)", "sort_group", "Group", CHOICE, 0,
        "Sort By → Group By, Set as Default")
        .choices = {{"None", 0}, {"By period", 1}, {"By day", 2}};
    add("Views", "Default sort (folders without one of their own)", "sort_group_by", "Group by the date", CHOICE, 0,
        "Sort By → Date, Set as Default")
        .choices = {{"Modified", 0}, {"Created", 1}, {"Taken (photos and videos)", 2}};
    add("Views", "Default sort (folders without one of their own)", "pinned_first", "Pinned & Starred First", BOOL,
        false, "Sort By → Pinned && Starred First");
    add("Views", "Folder previews", "folder_previews", "Show folder previews", BOOL, true,
        "the toolbar's preview switch, ☰ → View → Toggle Folder Previews (Ctrl+Shift+P)");
    add("Views", "Folder previews", "list_folder_previews", "Previews for folders in list view", BOOL, false);
    range(add("Views", "Folder previews", "folder_count", "Images in folder previews", INT, 4), 1, 4);
    add("Views", "Folder previews", "folder_order", "Folder preview picks", CHOICE, "name")
        .choices = {{"First by name", "name"},
                    {"Last by name", "last"},
                    {"Newest first", "newest"},
                    {"Oldest first", "oldest"},
                    {"Beginning, middle and end", "spread"}};
    add("Views", "Folder previews", "folder_color", "Folder colour", CUSTOM, "accent",
        "a folder's Folder Colour menu (for that folder)");
    add("Views", "Search", "search_recursive", "Search in subfolders", BOOL, false, "the search bar (Ctrl+F)");
    add("Views", "Search", "search_contents", "Search inside files", BOOL, false, "the search bar (Ctrl+F)");

    // -- Sidebar
    add("Sidebar", "", "sidebar_sections", "Sections", CUSTOM, QStringList(),
        "drag a section's heading in the sidebar; click it to collapse it");

    // -- Images & Videos
    range(add("Images & Videos", "", "slideshow_secs", "Slideshow interval", INT, 4, "the image viewer"), 1, 120, " s");
    add("Images & Videos", "", "play_gifs", "Play animated GIFs in the file view", BOOL, false);
    add("Images & Videos", "", "play_webm", "Play WebM videos in the file view (silent looping previews)", BOOL, false);
    range(add("Images & Videos", "", "thumb_max_mb", "Don't make previews of files larger than", INT, 200), 1, 10000,
          " MB");
    add("Images & Videos", "", "slow_previews", "Previews on tapes and floppies", CUSTOM, QStringList(),
        "a drive's menu → Show Previews on This Drive");

    // -- Files
    add("Files", "Checksums", "checksum_algorithms", "Algorithms", CUSTOM, "sha256", "Create Checksum File…");
    add("Files", "Checksums", "checksum_one_file", "Checksum files", CHOICE, false, "Create Checksum File…")
        .choices = {{"One file for each algorithm", false}, {"One file with every algorithm", true}};
    add("Files", "Jobs", "jobs_mode", "Run jobs", CHOICE, "drive", "the Jobs window (Ctrl+Shift+J)")
        .choices = {{"By drive (jobs on different drives side by side)", "drive"}, {"One at a time", "one"}};
    add("Files", "Jobs", "jobs_start_paused", "Start new jobs paused", BOOL, false, "the Jobs window (Ctrl+Shift+J)");

    // -- Archives
    add("Archives", "Compressing", "archive/format", "Format", CUSTOM, QString(),
        "Compress… (the last one used), Compress to “name.ext”");
    add("Archives", "Compressing", "archive_formats", "Each format's options", CUSTOM, QVariant(), "Compress…",
        "What Compress… starts with for each format; it remembers them as you use it.");
    add("Archives", "Extract To…", "archive/x_subfolder", "Into a new folder named after the archive", BOOL, true,
        "Extract To…");
    add("Archives", "Extract To…", "archive/x_overwrite", "Existing files", CHOICE, "rename", "Extract To…")
        .choices = {{"Keep both (rename)", "rename"}, {"Replace", "overwrite"}, {"Skip", "skip"}};
    add("Archives", "Extract To…", "archive/x_trash", "Move the archive to the trash afterwards", BOOL, false,
        "Extract To…");
    add("Archives", "Extract To…", "archive/x_open", "Open the extracted folder", BOOL, false, "Extract To…");
    add("Archives", "Passwords", "archive_passwords", "Saved archive passwords", CUSTOM, QVariant(),
        "Passwords and Keys (Seahorse)");

    // -- Windows
    add("Windows", "", "open_in_tabs", "Open folders from other apps as tabs in an open Kestrel window", BOOL, false,
        QString(),
        "On: a folder opened from another app (or with “Show in folder”) becomes a tab in the Kestrel window you used "
        "last. Off: it opens in a new window.");
    const QString chooser = "The file chooser (Open and Save windows of other apps)";
    const QString in_chooser = "the same places in a file chooser window";
    add("Windows", chooser, "chooser/view_mode", "View", CHOICE, QVariant(), in_chooser).choices = views;
    range(add("Windows", chooser, "chooser/grid_size", "Icon size in the grid", INT, 96, in_chooser), 48, 320, " px");
    range(add("Windows", chooser, "chooser/list_size", "Icon size in the list", INT, 24, in_chooser), 16, 128, " px");
    add("Windows", chooser, "chooser/show_hidden", "Show hidden files", BOOL, QVariant(), in_chooser);
    add("Windows", chooser, "chooser/sidebar", "Show the sidebar", BOOL, QVariant(), in_chooser);
    add("Windows", chooser, "chooser/info_panel", "Show the info panel", BOOL, QVariant(), in_chooser);
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
