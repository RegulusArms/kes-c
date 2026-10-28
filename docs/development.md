# Development

[← Back to the README](../README.md)

## Tests

```bash
tests/run.sh                  # every test
tests/run.sh fileops atc_undo # only some
```

There are 792 checks in 21 tests: file operations (copy, move, merge, delete, cancel, trash, links, undo), archives (extracting several at once, looking inside one, testing, and remembered passwords with a real keyring), Preferences and the right-click menus' layouts, the tower that keeps several Kestrels in sync (shared changes, the shared task list, shared undo, opening folders as tabs), phones and cameras, rearranging the sidebar and its Mounts section, searching (filters, and inside files with the index, ripgrep or grep), pinned items and folder sorts, grouping by date, tapes and floppy disks (slow drives), the job queue and its windows, following the desktop theme, the file chooser, the admin helper that runs as root, every menu entry and shortcut, screen-reader names and dropping onto folders, parsing modelled on Python's, the image viewer (lossless rotations, crop and resize, compare, the filmstrip and info panel), and `install.sh` and `kes-setup`, including a BleachBit dry run of Kestrel's cleaner. Each test runs with a throwaway home folder on a private D-Bus bus, so your files, settings, dock and open windows are never touched. GitHub Actions runs them on Ubuntu 24.04 and 26.04 on every push that changes more than Markdown files, also with AddressSanitizer and UndefinedBehaviorSanitizer, builds a .deb on each, installs each on a clean system of its release, and, once they're published, installs them again from each release's apt suite. Details: [tests/README.md](../tests/README.md).

## Architecture

### Inside one Kestrel

```
                     Kestrel Explorer (kes, C++ / Qt 6)
                                  │
   ┌──────────────┬───────────────┼──────────────┬──────────────────┬─────────────────┐
   │              │               │              │                  │                 │
 File UI      File jobs          GIO        External tools    D-Bus services      Radio
 panes,       (worker threads)   mounts,    7z, tar, zpaq,    Show in folder,     its link to
 sidebar,     copy · move ·      Open With, exiftool,         file chooser        the tower
 viewer,      delete · trash ·   trash      ffmpeg, system                        (see below)
 search,      undo · extract                thumbnailers
 thumbnails   progress, cancel
 (threads)    and errors shown
              in this window
   │              │               │              │
   └──────────────┴───────┬───────┴──────────────┘
                          │
                  Linux filesystem ◄── kes-admin-helper (root, via pkexec):
                          │              "Retry as Administrator"
             ┌────────────┴────────────┐
           local              GVFS mounts (/run/user/…/gvfs)
                              SMB · SFTP · phones · cameras
```

Each Kestrel runs its own file jobs and shows their errors itself. Network shares, phones and cameras are mounted through GIO/GVFS and then read like any other folder.

### Several Kestrels and the tower

Kestrel isn't single-instance: a folder opened from another app may start a Kestrel process of its own, and a crash takes down only that one. The tower (`kes --atc`, no window) keeps them in sync. It talks to Kestrel processes, not windows: the windows of one process share its radio and update each other directly.

```
 ┌────────────────────────────┐                          ┌────────────────────────────┐
 │ KESTREL A                  │                          │ KESTREL B                  │
 │                            │                          │                            │
 │  Window 1      Window 2    │                          │  Window 3                  │
 │      └────┬───────┘        │                          │      │                     │
 │         radio              │                          │    radio                   │
 └───────────┬────────────────┘                          └──────┬─────────────────────┘
             │  D-Bus (session bus)                             │  D-Bus (session bus)
             │  check in · report · request                     │  check in · report · request
             ▼                                                  ▼
       ┌───────────────────────────────────────────────────────────────┐
       │ TOWER  (kes --atc)                                            │
       │                                                               │
       │ • sends each report to every other Kestrel                    │
       │ • job progress: keeps each Kestrel's latest, so a new window  │
       │   can show jobs running elsewhere                             │
       │ • ✕ Cancel: passed to the Kestrel that owns the job           │
       │ • shared undo list (optional, off by default)                 │
       │ • shared changes: Preferences, stars, pins, folder colours,   │
       │   covers, folder sorts, bookmarks, the sidebar's layout,      │
       │   cleared caches                                              │
       │ • hands folders to an open window ("open folders as tabs")    │
       │ • the job queue: lanes on drives for every Kestrel's jobs     │
       │                                                               │
       │ Checks every message (protocol version, known types, the      │
       │ types of known fields, absolute paths, size); so does each    │
       │ Kestrel.                                                      │
       │                                                               │
       │ Never touches files, and runs no jobs. Started by the first   │
       │ Kestrel, gone shortly after the last one leaves.              │
       └───────────────────────────────────────────────────────────────┘
```

Examples:

```
 Star a file in Window 1    ─► A reports "starred"  ─► tower ─► B: Window 3 shows the star
 Copy running in A          ─► progress reports     ─► tower ─► B shows it in the status bar
 ✕ on that copy in B        ─► "cancel" for A       ─► tower ─► A stops its copy
 Ctrl+Z in B (shared undo)  ─► asks the tower for the newest action, which may be A's
 Folder opened by an app    ─► handed to the Kestrel whose window was used last (tabs on)
 Window 1 ↔ Window 2        ─► no tower: they're in the same process
```

Messages are JSON; a Kestrel speaking another protocol version is ignored. If the tower goes down, the next Kestrel to notice starts a new one and every Kestrel checks in again.

## Windows

Every window Kestrel opens can be moved anywhere and left open until it's needed: Properties, Preferences, Open With,
Compress and Extract, a question, an error message, the folder picker. None is modal, so none blocks the window it came
from or (on GNOME, which attaches modal dialogs to their parent) drags it along. None is kept above another window, and
several can be open at once. A window goes when the Kestrel window it belongs to closes.

To keep it that way, a new window:

- is made on the heap and shown with `show_free()` (`src/windows.h`), never with `exec()`, `open()`, `setModal()` or a
  window modality;
- uses `warn()` / `inform()` for a message, `ask()` / `ask_yes()` / `ask_box()` for a question, `ask_text()` for a line
  of text, and `choose_folder()` / `choose_file()` for a picker (Kestrel's own chooser window), never the static functions
  of `QMessageBox`, `QInputDialog`, `QFileDialog` or `QColorDialog`, which are all modal;
- does what follows the answer in a callback (on `accepted`, or the helper's), since nothing waits for it. Re-read
  anything that may have changed while it was open, and parent a message to a window that stays open (not to a
  dialog that closes right after).

tests/test_ui.cpp checks the source for modal calls, so one that slips in fails the tests.

## Layout

| File | Purpose |
|---|---|
| `src/main.cpp` | The `kes` program's entry point (calls `kes_main` in `app.cpp`) |
| `src/app.cpp` | Main window: tabs, toolbar, menus and shortcuts, context menus, applying settings, starting up |
| `src/actions.cpp` | The main window's file actions: clipboard, drops, new files and folders, rename, trash and delete, restore, links |
| `src/opening.cpp` | Opening files and folders: what double-click and Enter do (folders, archives, images, videos, other files, as Preferences says), Quick View, the image viewer |
| `src/incoming.cpp` | Requests from outside this Kestrel: changes other Kestrels report through the tower, folders handed over to open as tabs, and other apps' "Show in folder" (FileManager1) |
| `src/pane.cpp` | The browser pane in each tab: grid and list views, the overview page, the combined trash, Starred and Recent, search, history, selection, each folder's sort and grouping |
| `src/drives.cpp` | Slow drives (LTFS tapes, floppy disks): recognising them from the mount table, their sizes read in the background, tape attributes and index sync, one job at a time per drive |
| `src/tapebuf.cpp` | The RAM write buffer for copies onto a tape (no shoe-shining): read ahead by a thread, sized from the memory available |
| `src/jobs.cpp` | The job queue: one for every Kestrel (kept by the tower, else each Kestrel's own), by drive, one at a time or all at once |
| `src/jobswin.cpp` | The Jobs window: every job, its steps and log, the queue's controls |
| `src/windiag.cpp` | Window diagnostics: each window's size, limits, state and recent changes (the Tower window's Windows tab) |
| `src/towerwin.cpp` | The Tower window (developer tools, `kes --tower-monitor`): what the tower is doing, live |
| `src/groups.cpp` | Grouping by date: the model that puts date headers between the items (by period or by day), and the dates it groups by (created, and taken from metadata on worker threads) |
| `src/widgets.cpp` | File-system model, grid delegate, path bar, sidebar, info panel, search |
| `src/thumbs.cpp` | Background thumbnail and folder-mosaic generation and caching |
| `src/viewer.cpp` | Image viewer: decoding ahead at screen size, zoom, the filmstrip, the info panel, compare, quick actions, dragging an image out and Ctrl+C |
| `src/imgedit.cpp` | Saving the viewer's changes to image files: rotations through the orientation tag, Straighten, crop and resize; turning files from the menu (Rotate ▸: jpegtran, lossless formats); keeping metadata in an image made from another |
| `src/media.cpp` | Converting media: ffprobe's details, ffmpeg's options built from the settings (videos, audio), running ffmpeg with progress into a part file; converting images |
| `src/convert.cpp` | The menu's Rotate ▸, Convert Image…, Convert Video…, Extract Audio… and Convert Audio…: their windows and jobs |
| `src/fileops.cpp` | Background tasks and the status-bar task panel; copy, move and delete (retrying denied ones as administrator); conflict handling; links |
| `src/places.cpp` | Starred and Recent: places that list files from anywhere; pinned items; each folder's sort and grouping (`sorts.json`) |
| `src/undo.cpp` | Undo for file operations, shared by a Kestrel's windows (or every Kestrel's) |
| `src/sharing.cpp` | What GNOME Files offers through extensions: code editors, Nautilus scripts, Send To, Samba sharing |
| `src/animate.cpp` | Animated GIFs and WebM clips playing in the file view |
| `src/fm1.cpp` | The `org.freedesktop.FileManager1` service ("Show in folder" from browsers and other apps) |
| `src/chooser.cpp` | The system's file chooser: the portal backend (`kes --file-chooser`) and the Save/Open bar of chooser windows |
| `src/focus.cpp` | Giving the focus to the app files are dropped into: on X11 directly, on GNOME on Wayland through the drop focus extension (`data/gnome-shell/`) |
| `src/admin.cpp` | The admin session: starts the root helper once, sends it operations, the 🛡 status-bar indicator, and "Retry as Administrator" |
| `src/admin_helper.cpp` | The small root helper (Qt Core only, for JSON) that performs admin-session operations |
| `src/windows.cpp` | Opening windows: every dialog, message, question and file or folder picker is a free window (see [Windows](#windows)) |
| `src/dialogs.cpp` | Properties (including the metadata editor and Add Tag picker), Open With, rename, batch rename, Edit Bookmark |
| `src/prefs.cpp` | Every setting: its tab, label, default and where else it's changed; setting one applies it everywhere and tells the other Kestrels |
| `src/prefswin.cpp` | The Preferences window, built from that list: tabs, search, resets |
| `src/menus.cpp` | The right-click menus as the user lays them out: entry ids, the default layouts, rearranging a built menu, and the editor (Preferences → Menus) |
| `src/hashcheck.cpp` | Checksum files: Verify Checksums, Create Checksum File…, and checking one file in Properties |
| `src/signature.cpp` | Verify Signature: checking `.sig` / `.asc` / `.gpg` signatures with gpg, said plainly; fetching a missing key |
| `src/dupes.cpp` | Find Duplicates…: grouping identical files (by size, then their beginnings, then SHA-256), the window, trashing or hard-linking the copies not kept |
| `src/compare.cpp` | Compare With… and Sync To…: two folders' differences, the sync plan (rsync or Kestrel's copy), saved sync pairs |
| `src/metadata.cpp` | EXIF and PNG-text reading, the date a photo or video was taken, the prompts image generators save in files, exiftool reading/editing, and the tag catalog for the Add Tag picker (the tag descriptions and "Accepts" types were written with the help of an AI tool and may not be completely accurate) |
| `src/overview.cpp` | Overview page: drives, phones and cameras, network locations, bookmarks, starred items, frequent folders; its sections chosen in Preferences → Overview |
| `src/health.cpp` | Drive health (S.M.A.R.T.) from udisks over D-Bus: the cards' health line, the Drive Health window, the failing-drive notification |
| `src/util.cpp` | Shared helpers: paths and file-system calls, file types, icons, desktop integration (default apps, wallpaper, terminal), trash on every drive, GTK bookmarks |
| `src/proc.cpp` | Running command-line tools: pipes, process groups, timeouts |
| `src/archive.cpp` | Archive engine: tool detection, the commands for every format, progress, cancel, password handling |
| `src/archive_ui.cpp` | Compress and Extract dialogs, password prompts, and the archive job flows: Extract, Extract All and its password search, Test Archive(s) |
| `src/archivewin.cpp` | Open as Archive: the window listing what's inside an archive, extracting single items from it, or all of it, or testing it |
| `src/keyring.cpp` | Remembered archive passwords in the desktop's keyring (the Secret Service, through libsecret), found by the archive's fingerprint |
| `src/uwp.cpp` | Integration with the UWP wallpaper manager (over D-Bus and the `uwp` command) |
| `src/stats.cpp` | The `KESTREL_STATS=1` performance counters (see [Performance counters](#performance-counters)) |
| `src/atc.cpp` | The tower (`kes --atc`) and each window's link to it, which keep several running Kestrels in sync |
| `tests/` | The test suite (see [Tests](#tests)) |
| `bench/` | The benchmark against GNOME Files, Nemo and Dolphin (see Performance: [vs GNOME Files](performance.md#performance-kestrel-vs-gnome-files), [vs Nemo](performance.md#performance-kestrel-vs-nemo), [vs Dolphin](performance.md#performance-kestrel-vs-dolphin)) |
| `CMakeLists.txt` | Build configuration, and the .deb (CPack) |
| `kes-setup` | Makes Kestrel the default file manager for the current user, and puts things back (run by `install.sh`; also in the .deb) |
| `data/` | The app icon (`icons/hicolor`, built into `kes` through `kestrel.qrc`), the .deb's app menu entry and file chooser portal files, and the GNOME Shell drop focus extension |
| `.github/workflows/deb.yml` | Builds the .debs and publishes releases and the apt repository |
| `.github/apt/` | The apt repository's suites (`distributions`, reprepro's) and its web page (`index.html`), which CI copies to the gh-pages branch with each build |

## The apt repository and Ubuntu releases

The repository (the gh-pages branch, served at https://regulusarms.github.io/kes-c/) has a suite for each supported Ubuntu release, named after its codename: `noble` (24.04) and `resolute` (26.04) get every release, `noble-dev` and `resolute-dev` every development build. `stable` and `dev` get 24.04's builds too, for the sources files written before the release suites (`APT_GENERAL` in `deb.yml`: the oldest supported release, whose build installs on all of them). The releases built for are the `deb` job's matrix; the suites are in `.github/apt/distributions`. The install instructions (README, docs/install.md and the repository's page, `.github/apt/index.html`) give each release its own commands, naming its suite, so nobody is handed another release's package; `grep UBUNTU_CODENAME /etc/os-release` tells users which they have (Linux Mint and the Ubuntu flavours set it to their Ubuntu base). CI copies the page to gh-pages only with a release, so it never names a suite a release hasn't published yet; the README and install guide reach `main`, the branch GitHub shows, with the same merge. Suites follow the Ubuntu base, not the desktop: the package's dependencies come from the base, and the desktop is handled when Kestrel runs, so dropping a desktop is a code and docs change, not an apt one.

Each release gets a .deb built on it (in a clean container of that release), using its Qt and libraries; its Depends ask for that release's C library, C++ library and Qt or newer (`CMakeLists.txt`), so it won't install on an older release. Its version ends in the release's number (`0.3.2~alpha.25~ubuntu24.04`, `…~ubuntu26.04`), which keeps the two apart in the pool and lets a system upgraded to the next Ubuntu release upgrade to that release's build; a development build (`0.3.2~alpha.25+dev118.g1a2b3c4~ubuntu24.04`) still sorts after its version's release. The GitHub releases carry every release's .deb. gh-pages keeps no history and `reprepro includedeb` replaces a suite's previous version, so once a build that doesn't install on a release reached that release's suite, the last one that does would be gone: its users would stop getting updates without being told (apt keeps the installed version when the new one's dependencies can't be met), and new installs would fail. So, to stop supporting a release (24.04 will be first: a newer Qt, or a library it doesn't have):

1. Before the first version that won't install there, take it out of the `deb` and `package` jobs' matrices. Its two suites keep the last builds that do and get no more; leave them in `distributions`. If it's `APT_GENERAL`, put the next release there.
2. In `deb.yml`'s `apt-check`, move its entry to the frozen ones (`frozen: noble` and `frozen: noble-dev` on its container), so each update of the repository still checks that they install on that release.
3. Say so in the CHANGELOG and on the repository's page (`.github/apt/index.html`: its suites table).
4. `stable` and `dev` move on, so their users on that release stop getting updates. The last version for that release should tell them in Kestrel to switch to the release's suite (Kestrel reads its sources file; a package doesn't rewrite people's apt sources).
5. Optionally, a maintenance branch for data-safety fixes, published only to that release's suite.

The test jobs run on GitHub's `ubuntu-24.04` runners. When GitHub retires that image, they move to `ubuntu:24.04` containers (pulled through mirror.gcr.io, like the package checks) for as long as 24.04 is supported.

The repository's signing key expires on 2029-10-04, about when 24.04's standard support ends. Extend it before then whatever is still supported (`gpg --quick-set-expire`), put the re-exported public key in gh-pages' `kestrel.asc`, and update the `APT_GPG_PRIVATE_KEY` secret. A user's copy of the key (`/etc/apt/keyrings/kestrel.asc`) isn't updated by apt, so they need to download it again.

## Performance counters

Run Kestrel with `KESTREL_STATS=1` (for example `KESTREL_STATS=1 kes ~/Pictures`) and it prints a summary to the terminal when you quit it normally: how long folders took to list, thumbnails and folder previews taken from the disk cache versus made, the longest thumbnail queue, copy speed, how long archive jobs took, how long messages from other Kestrels took through the tower, and the most tasks at once. Off (the default), it costs nothing measurable.
