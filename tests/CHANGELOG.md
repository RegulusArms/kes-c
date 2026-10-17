# Test changelog

What changed in the test suite (`tests/`) and in CI's test jobs (`.github/workflows/deb.yml`) for each version,
newest first, and why. The versions and their grouping follow the main [CHANGELOG](../CHANGELOG.md): a test is listed
under the version whose change it covers. What each test checks today is in [README.md](README.md).

## 0.3.1-alpha.12

Added:
- **`jobs`, a new test for the job queue, the Jobs window and the Tower window.** The scheduler's rules (by drive: a job
  waits for a running one on its drives, keeps its order within a lane, isn't overtaken; a paused waiting job doesn't
  hold others up; Start Now; one at a time). Drive ids: paths on one drive share one, a tape is its own, a partition
  counts as its whole disk. In one Kestrel: a job waits for another on its drive (saying so) while one on another
  drive runs; one at a time; cancelling a waiting job, Start Now, moving a job up, Start New Jobs Paused; pausing a copy
  (its part file stops growing, then it finishes) and a program a job runs (stopped and continued, then ended on
  cancel); a copy's steps and its log kept after it ends. The Jobs window opens from the job panel, lists running,
  waiting and finished jobs, and Delete cancels the selected one without moving the main window's selection to the
  trash. Through the tower: another Kestrel's job holds a lane our job waits for, a Kestrel that leaves frees its lanes,
  another Kestrel pauses and resumes our job, everyone hears the queue; the tower's Status (pid, protocol, refused
  messages) and UndoList; the Tower window shows the status and the messages live. Covers the unified job queue and the
  two windows.

Changed:
- `slow`: "a copy to a tape that another job is using waits" now holds the tape with a queued job (the per-Kestrel
  lanes it used are replaced by the job queue).
- `ui_actions.txt`: ☰ → Jobs… (Ctrl+Shift+J) and Tower….

## 0.3.1-alpha.11

Added:
- **`slow`, a new test for tapes and floppies**: folders marked as a tape and a floppy (`KESTREL_TEST_SLOW`, read by
  `src/drives.cpp`) with a statfs that waits like a busy tape's (`KESTREL_TEST_STATFS_DELAY_MS`). Checks: they're
  recognised and listed (also that a snap's small loop device isn't taken for a floppy); asking their size doesn't
  hold the window (sidebar, status bar, Properties, Overview) and the size comes later; a checksum file is read in the
  background (a FIFO stands in for a seeking tape); no thumbnail, date taken, GIF playback or image details read
  from a tape until previews are allowed, then they are; copies read a tape in tape order (from LTFS's start-block
  attributes); a job waits while another holds the tape; the index is written after a copy; more than 1,000 small
  files ask first; the Overview shows the tape by its volume name. Covers the LTFS and floppy fixes.
- `fileops`: on a file system without RENAME_NOREPLACE or hard links (`KESTREL_TEST_NO_LINK`, acting as LTFS) a copy
  to a new name works, a file appearing under the name meanwhile is kept, and `put_new` refuses a taken name. Guards
  against the "no permission" failure copying to a tape.
- `admin_helper`: the same for the helper's copy, write and copyfile.
- `ui`: no `QStorageInfo` outside the Overview's background scan (it asks drives for their size, which waits on a busy
  tape or floppy). Guards against the window freezing on slow drives again.

## 0.3.1-alpha.10

Added:
- **Dropping into a subfolder keeps the view where it was** (`ui`): a long folder scrolled to the top, its last files
  (the view's current item among them) dropped onto a subfolder at the top; after the move the scroll position is
  unchanged, in the grid, the list and the grouped grid. Guards against the list jumping to where the moved items were.
- **The mouse's back button doesn't drag** (`ui`): double-click into a folder, click a file there, go back with the
  back button and move the mouse while it's down: no drag picture appears in the folder above. Guards against the
  folder just left being dragged and sliding back into place.
- **The grid is packed** (`ui`): a folder of 850 items, re-laid out by zooming and at window widths one pixel apart:
  every item in its own cell in order, with as many columns as fit. Guards against the gap a batched layout left at
  the 400-item batch boundary, and the empty last column when the cells added up to exactly the width.
- **Save follows the place shown** (`chooser`): a Save chooser's button is off on the Overview and Starred and on
  again in a folder with nothing selected, and saves there. Guards against it staying greyed out after the Overview.

## 0.3.1-alpha.9

Changed:
- CI (`deb.yml`): a push or pull request that only changes Markdown files (`*.md`: the docs, changelogs, README, TODO)
  no longer starts the workflow, so the tests, sanitizers and package jobs don't run for it, and no package is built
  or published. A change to anything else, in the same push or later, runs everything as before.

## 0.3.1-alpha.8

Added:
- **Folder copies fill in as they go** (`fileops`): a folder copied to a new name has that name from the start, with no
  hidden part folder beside it, and a finished file shows up in it while the copy still runs; cancelled part-way, the
  files finished so far stay whole (no part file left), the folder gets the original's permissions, and Ctrl+Z puts
  the partial copy in the trash; a write that fails part-way (RLIMIT_FSIZE) keeps the files finished before it and
  reports the error. These cover copies that used to show nothing until the whole folder was done.

Changed:
- `fileops`: "a folder made under the new name while a folder copy runs" relied on the hidden part folder the copy
  used to build beside its destination; there's none now (the new folder is claimed by making it). It now checks that
  a folder already under the new name when the copy starts is kept and reported.
- `fileops`: before the new write-failure check, the previous check's error message is waited for, so it can't turn
  up among a later check's messages (it made "a file another program saves under the new name…" fail now and then).

## 0.3.1-alpha.7

No test changes.

## 0.3.1-alpha.6

Added:
- **Folder sorts** (`pinned`): sorting a folder sorts only that folder, leaving the other folders and the default as
  they were; it's saved in `sorts.json`, kept when coming back, followed by other tabs showing the folder and by other
  Kestrels (both ways). Apply to Subfolders reaches every folder below and none outside it; Set as Default changes
  only folders without a sort of their own. These cover the new per-folder sort, which replaced one sort for all
  folders.
- **Grouping by date** (`groups`, a new test): which period a date falls in (each period, weeks starting on the
  locale's first day, a week across two months, January, and that going back in time never returns to a newer group);
  a key and title per day for Group by Day; the date taken read from a photo's EXIF, a video's movie header and a PNG's
  Creation Time (files built in the test), and none from a file without one. A folder grouped by period shows its
  items under date headers, each group keeping the chosen sort (name both ways, size) and the oldest group first
  when sorted by Modified ascending; by day, a header per date; by Date Created, when the files were made (skipped
  where the file system keeps no creation times); by Date Taken, photos and videos under when they were taken once
  read in the background, the rest under their date created; Select All leaving headers out; collapsing by click and Enter, and selecting an
  item of a collapsed group opening it; in the grid a header taking a whole line, in the list spanning the columns;
  new, removed and changed files moving between groups (the changed one staying selected); a drop on a header going
  into the folder; grouping (mode and date) saved per folder and with Set as Default; Starred grouped too. These
  cover the new Group by Period and Group by Day views and the Date to Group By choice.
- **Old protocol** (`atc_sync`): a flight that names no protocol (one from before the protocol was named, speaking 1) is
  ignored now that the protocol is 2 (the new "sorts" message). The tower used to accept such a flight as speaking the
  current protocol.

Changed:
- `FakeFlight::check_in()` in `common.h` names the current protocol: without one, the tower now takes it for an old
  flight and ignores it.
- `ui_actions.txt`: the empty-space Sort By menu shows which sort is chosen (its entries are toggles now, like the
  toolbar's Sort menu) and has Group by Period, Group by Day, the Date to Group By submenu, Set as Default and Apply to
  Subfolders.
- `pinned`: a folder's entry in `sorts.json` has two more values, its grouping and the date it groups by.
- `installer`: the BleachBit cleaner's expected actions, dry-run listing and clean include `sorts.json` (under Folder
  covers, colours and sorts).

## 0.3.1-alpha.5

Added:
- **Following the desktop's dark style** (`theme`): a light theme with the dark style wanted gives a dark background
  and light text, with no theme colours at all (no python3 or GTK) too, and a dark theme keeps its own colours. Kestrel
  stayed white under GNOME's dark style unless the GTK theme itself was dark; these pin down the fix.
- **Folder preview picks** (`theme`): first and last by name, newest and oldest first, and "beginning, middle and end"
  (evenly spaced, with a chosen cover first), checked against files whose ages aren't in name order, so an order mixed
  up with another fails. Also: an unknown setting falls back to first by name, and a folder of galleries picks its
  subfolders the same way. These came with the three new choices in Preferences.
- **Free windows** (`ui`): Properties opens as a free window (not modal, a normal window, not kept above Kestrel), several
  at once, and closing one deletes it and what it opened. Opening them gives no widget in Kestrel's or Properties'
  window a native window of its own: that side effect made both windows glitch on Wayland, and this check names the
  widgets if it comes back. Messages, questions (answered, and closed without an answer), text input, the folder
  picker (Kestrel's own chooser) and a copy's name-conflict question are all checked to open free.
- **No modal windows in the source** (`ui`): a scan of `src/` fails on `exec()`, `open()`, the static
  `QMessageBox`/`QInputDialog`/`QFileDialog`/`QColorDialog` functions or a modality, naming the line. Every window was
  made free in this version; the scan keeps a modal one from slipping back in.
- `shown_window<T>()` and `is_free_window()` in `common.h`, for finding and checking windows now that none is modal.

Changed:
- `fileops` and `chooser` found message boxes and the Extract dialog through `QApplication::activeModalWidget()`, which
  sees nothing once windows aren't modal; they look among the visible windows instead. Checks that read a message right
  after an operation now wait for it: a modal box used to hold the operation until the test had seen it.
- "Closing a Properties window deletes it" waits for the deletion and carries out pending deletions while it waits.
  Qt 6.4 (CI) doesn't carry them out in the nested event loop the tests wait in, so it failed there while passing on
  newer Qt. The message now says which part failed.

## 0.3.1-alpha.4

Added:
- **Folder-preview builds** (`theme`): Generate Folder Previews covers the folder and the folders directly in it, the
  recursive one every folder below, neither making a thumbnail for every file; Generate Previews Recursively still
  makes them all. And the scan finishes before most previews are made, which was the point of the faster scan.
- **Folders-only walk** (`fileops`): it reaches every folder a full walk does and lists no files. The folder-preview
  builds rely on it.
- **Keep damaged files** (`fileops`): damaged 7z, zip and rar archives extract with the option, listing the damaged
  file, and fail without it, saying how to keep them (rar skipped where rar or unrar isn't installed).

## 0.3.1-alpha.3

Added:
- **Tab menu** (`ui`): Duplicate Tab (folder and back history, placed after the tab), Open in New Window (moves the
  tab) and Close Tab, for the new tab right-click menu.
- **Folders without images** (`theme`) are drawn in the folder colour; **special folders** show their symbol, which
  stands out on any folder colour, white and black included (skipped when the icon theme lacks the symbols); a
  **bookmark that isn't there** is a faded folder, a network one a plain one.

Changed:
- "No folder previews on a phone" (`devices`) now also checks what's drawn instead: a plain folder in the folder colour,
  not nothing, since folders without previews are coloured like the rest now.

## 0.3.1-alpha.2

Added:
- **`pinned`**, a new test for pinned items: pins mark an item without starring it, are saved in `pinned.json` and
  reported to (and picked up from) other Kestrels; Pinned & Starred First sorts them first in either direction, keeps
  each group sorted, puts newly pinned or starred items first at once, survives leaving and coming back, and turning it
  off restores the order. `pinned.json` also went into the BleachBit cleaner's dry-run expectations (`installer`).

## 0.3.1-alpha.1

Added:
- **Files another program creates mid-job** (`fileops`, `admin_helper`): a file or folder saved under the new name while
  a copy, move or single-file decompress runs is kept and reported, and a name that's taken by the time a job starts is
  never replaced unless Replace was chosen. These cover the fix that made only Replace replace.

Changed:
- The race checks step in when the copy's hidden `.kes-….part` file appears, instead of at the first progress report:
  reports come at most every 80 ms, and on a fast disk the copy could finish before the first one, so the check
  sometimes tested nothing.
- "A replace finishes" asks for Replace explicitly, since a copy over an existing name no longer replaces on its own.
- CI pulls its Ubuntu containers through `mirror.gcr.io`: Docker Hub's rate limit on the runners' shared addresses was
  failing the package job.

## 0.3.1-alpha.0

Changed:
- The admin helper's ownership check now expects new items to get the folder's **group**, not its owner: run as the
  user, it can't give away ownership. The owner-as-root half moved to `tests/check_package.sh`, which runs as root.
- The symlink-swap delete check reports how many swaps it managed, so a run where the race never happened is visible.
- The escaping-link sweep is also checked in folders an archive leaves unlistable (mode 111), not only read-only and
  closed ones.

## 0.3.0-alpha.10

Removed:
- The cross-version checks against the Python version (launching its `kes` in `atc_tabs`, matching its undo format in
  `atc_undo`, the shared check texts), ahead of the two projects going separate ways.

Changed (checks made able to fail):
- The cancel test cancels from the copying thread at the first report on a file, with long sparse files, so the cancel
  always lands mid-copy. It used to cancel at once, and on a fast disk the copy could finish first, making
  "stops part-way" untested. It also checks no hidden part folder is left.
- `atc_tabs`: a launched Kestrel that crashes no longer counts as one that kept its own window, and a second hand-off is
  checked to add a second tab. A hand-off with a selection is routed to the Kestrel that has a window.
- `theme`: the dark test palette's highlight changed from `#3584e4` to `#2a7fd4`. The old one was the default accent
  itself, so "follows the theme's colour" passed even when the default was used.
- `sidebar`: Trash moved within Places is also checked not to appear in another section.
- `chooser`: "with multiple, every selected file is chosen" checks the exact files, not just that two came back.
- `parsing`: the KESTREL_STATS report check describes this project's format only.
- `tests/check_package.sh`: the admin helper's ownership rules as root (what it makes in root's folder is root's, in the
  user's folder the user's).

## 0.3.0-alpha.9

Added (`fileops`):
- Ctrl+Z after a copy that replaced a file says it can't be undone and leaves the action before it alone.
- Right-clicking a selection mixing trashed and live files gives the normal menu; only an all-trash one gives Restore.
- Escaping links are removed in folders an archive leaves read-only (555) or closed (000) too, the folders' modes are
  put back, and each removed link is reported.

## 0.3.0-alpha.8

Added:
- **Ownership** (`admin_helper`): copies, new folders (with the parents made for them), files and links get the
  folder's owner, and a copy given another group loses its set-group-ID bit.
- **Undo after Replace** (`fileops`): a copy that replaced a file isn't offered for undo, and undoing a batch leaves
  that copy where it is (undoing it would trash the only copy left).
- **Batch rename** (`fileops`): one failing part-way puts every item back, a dangling symlink too, and replaces nothing.
- **Conflict dialog** (`fileops`): it says a merge replaces files with the same names and can't be undone.
- **Delete on mixed selections** (`fileops`): Move to Trash trashes the live items and asks only about the trashed one.
- **Escaping links** (`fileops`): which link targets lead out of the folder, and extracting into an existing folder keeps
  the user's own links while dropping the archive's escaping one.

## 0.3.0-alpha.7

Added (`theme`):
- GNOME's accent colour names give libadwaita's colours, a chosen accent comes before the theme's and folders follow it,
  and with none chosen folders take the theme's (skipped before GNOME 47). For the new accent colour support.

## 0.3.0-alpha.6

Added (failure-safe replacing):
- `fileops`: replacing a folder and cancelling part-way, or failing part-way (a write error through RLIMIT_FSIZE), keeps
  the old folder; copying or moving a folder onto another replaces it whole; compressing over an archive that fails
  keeps the old one; Kestrel's own files are written in one step (keeping permissions and symlinks); a new file never
  replaces one that's there; moving between drives copies whole before deleting.
- `admin_helper`: a write failing part-way (a full disk) keeps the old file or folder, a cancel or an ended admin session
  stops a large copy at once, and copying or moving a folder onto another replaces it whole.

These cover the rule that a failure, cancel or crash never leaves half a file or loses the old one.

## 0.3.0-alpha.5

- No test changes (version bump only).

## 0.3.0-alpha.4

Added (`fileops`):
- **Create Checksum File**: one file per algorithm that verifies as OK, or one BSD-tag file (making it again leaves out
  its own older copy), and an unreadable file is left out and named.
- **Safer writes**: replacing a file and cancelling or failing part-way keeps the original; replacing a symlink replaces
  the link, not its target, and another hard link keeps its contents.
- **Desktop shortcuts**: names and paths with quotes, `$`, backquotes, backslashes, `%` and line breaks stay whole and
  add no keys, `desktop-file-validate` accepts them, other control characters are refused, and the program's shortcut
  runs only that program. These came with the escaping fix that stopped file names injecting keys or commands.

## 0.3.0-alpha.3

Added:
- **Single-file archives** (`fileops`): decompressing doesn't write through a dangling symlink of that name or truncate
  the file a symlink points to, and a failed run keeps the old file and no temporary one.
- **Admin helper moves** (`admin_helper`): moving or copying something onto itself or into itself, or a file onto a
  hard link to it, leaves it as it was. This was the bug where such a move deleted the source.

## 0.3.0-alpha.2

- No test changes (version bump only).

## 0.3.0-alpha.1

Added (`fileops`):
- **Checksum verification**: checksum files are recognised by name; GNU, BSD-tag, SFV and lone-hash files are read; a
  verify finds matching, changed and missing files for every algorithm; double-clicking a checksum file opens Verify
  Checksums; Properties' Checksums tab checks a file against a chosen checksum file.

## 0.3.0-alpha.0

Added (`installer`):
- `kes-setup --install-optional` installs the missing optional packages apt has, names the ones it hasn't, and installs
  nothing when they're all there.

## 0.2.1-alpha.3: security and robustness

Added:
- **`admin_helper`**, a new test for the part that runs as root. It runs the helper as the user and sends it requests as
  Kestrel does: every operation works; relative paths and protected folders are refused; every operation through a
  symlinked folder is refused; hard links to other users' files are refused; a folder swapped for a symlink during a
  delete can't redirect it. This came with the helper's hardening.
- **Processes** (`fileops`): a program's timeout holds without pipes and after it closes its output, a tool nothing
  refers to any more is stopped, and a program exiting without reading its input doesn't kill Kestrel.
- **Archive passwords** (`fileops`): password-protected rar and zip archives are made and opened, and a watcher on every
  process's command line never sees the password.
- **Crafted archives** (`fileops`, fixtures in `tests/fixtures/`): tar.gz, zip, 7z and rar archives with `../` and
  absolute paths or links can't write or leave a link outside the folder.
- **Symlinks in normal file operations** (`fileops`): a merge doesn't write through a symlink in the destination,
  copying onto a symlink replaces the link, and a symlink-swapped folder can't take a delete outside the tree.
- **Tower messages** (`atc_sync`): malformed, unknown and oversized messages are dropped, field types and paths are
  checked, only valid undo entries are kept, and a flight speaking another protocol version isn't passed on.
- **`ui`**, a new test: every menu entry and shortcut compared with `tests/ui_actions.txt` (so one lost when code moves
  is named), every control has a screen-reader name, and a file dropped onto a folder goes into it with the folder
  highlighted.
- **`parsing`**, a new test: `shlex_split` and the ordered JSON reader for exiftool's output.
- **Performance counters** (`fileops`, `atc_sync`): KESTREL_STATS counts folder listings, copy speed, archive jobs, tasks
  and the tower's delay.
- **App icon** (`installer`): installed in every size and used by the menu entry, and removed again.
- **BleachBit**: `kes-setup --bleachbit` writes a valid cleaner that follows BleachBit's schema and names only Kestrel's
  files, a dry run lists exactly those and changes nothing, a real clean keeps everything else, and `--undo` removes it
  (`installer`); Shred and Empty Trash with BleachBit hand it the right files, and ✕ stops it (`fileops`).

CI:
- A **sanitizers** job runs the suite with AddressSanitizer and UndefinedBehaviorSanitizer.
- A **package** job installs the .deb on clean Ubuntu 24.04 and 26.04 containers (`tests/check_package.sh`): it runs, is
  complete, keeps the admin helper's root-only rules, and removes cleanly. Only a package that passes is published.

## 0.2.1-alpha.2

CI:
- A **test** job runs `tests/run.sh` on Ubuntu 24.04 before the .deb is built, so a failing test means no package,
  release or apt update. It installs what the installer test needs (xdg-utils, desktop-file-utils, GNOME Shell's
  settings).

## 0.2.1-alpha1 / 0.2.1-alpha.1

- No test changes (version naming only).

## 0.2.1-alpha

Added:
- **File chooser settings** (`chooser`): a chooser opens smaller with smaller icons, and its zoom and hidden-files
  settings are its own, for the chooser's separate view settings.
- **Drop focus extension** (`installer`): `--default` installs and enables it keeping the user's other extensions (only
  enabling the package's copy after the .deb), once, and `--uninstall` / `--undo` take it out again.

## 0.2.0-alpha

Added:
- **`devices`**, a new test for phones and cameras: which locations are on a device, which videos are copied before
  playing, where the Overview lists mounts, the connection hints (Trust, File transfer, PTP), and thumbnails from a
  phone without folder previews (which would download every file) or hanging on an unreadable photo.
- **Linux Mint** (`theme`, `chooser`): Cinnamon's own settings are used for the wallpaper, history and icons; on Qt
  before 6.5 the GTK theme's colours become the palette; an X11 app's window id is read from the portal's handle.
- **`kes-setup`** (`installer`): on its own, as after installing the .deb, `--default` points the D-Bus services at that
  `kes` without sudo and leaves the menu entry and PATH alone, and `--undo` puts everything back.

Changed (`installer`):
- The setup also makes a stand-in for Nemo the folder handler, and `--uninstall` is checked to put it back.
- The dock checks are skipped where GNOME Shell's settings aren't installed (Linux Mint and other desktops).
- A stand-in `kes` on PATH, since GIO ignores a menu entry whose program it can't find.

## 0.1.3-alpha

Added:
- **`chooser`**, a new test for Kestrel as the system's file chooser: filters, Save and Open, replacing a file (asking
  first), multiple files, folders, several files into one folder, Cancel, closing the window, and the app closing it.
- **`sidebar`**, a new test: rearranging entries and sections (saved, and kept within their section), collapsing, and
  sharing the order with other windows and Kestrels.
- **`theme`**, a new test: light and dark palettes, colours derived from them, and following a change (callbacks run
  once, stylesheets, the sidebar and the Overview take the new colours, the folder colour follows the accent).
- **Archives** (`fileops`): a .tar.gz opens Kestrel's Extract dialog; .deb, .iso and .apk open with their own apps.
- **Installer** (`installer`): the file chooser's portal definition and service are installed (keeping the user's own
  portals.conf, and putting it back), and `~/.local/bin` goes on new terminals' PATH once and comes off again.

## 0.1.2-alpha: first release

Added, the first test suite (`tests/run.sh`, each test a program with its own HOME and session bus):
- **`fileops`**: copy, move, merge, replace, delete, cancel, trash, undo, unique names and links.
- **`atc_sync`**: changes reported to and picked up from other Kestrels through the tower, and a crashed tower replaced.
- **`atc_tasks`**: running tasks shown and cancelled across windows and Kestrels.
- **`atc_undo`**: undo per Kestrel (the default) and shared between them, with and without a tower.
- **`atc_tabs`**: folders from launched Kestrels opening as tabs when that's on.
- **`installer`**: `install.sh --default`, `--dock` and `--uninstall` in a throwaway HOME, run twice to show nothing
  doubles.
