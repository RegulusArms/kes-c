# Test changelog

What changed in the test suite (`tests/`) and in CI's test jobs (`.github/workflows/deb.yml`) for each version,
newest first, and why. The versions and their grouping follow the main [CHANGELOG](../CHANGELOG.md): a test is listed
under the version whose change it covers. What each test checks today is in [README.md](README.md).

## 0.3.2-alpha.15

Added:
- `jobs`: jobs belong to Kestrel, not to a window. Closing the window that started a job doesn't ask or stop it (it
  moves to another window's status bar); closing the last one asks (exit and open the Jobs window, or stop all jobs),
  and Exit and Open the Jobs Window opens it as a window of its own while the job goes on; a job the user sees keeps
  Kestrel running and a quiet task doesn't; with every window closed (the Jobs window too), Kestrel's event loop runs
  until the job ends and then quits.
- `theme`: thumbnailing as jobs: one per pane and folder (its title, total and window), listed in the Jobs window's
  own lower part; Pause holds it; leaving the folder lowers it (another window's newer ones come first), it carries on
  after, and going back raises it; it waits while another job uses its drive; a lowered job stops after the time set
  in Preferences, keeping what it made; Stop isn't undone by the view drawing the folder again.
- `fileops`: remembered folder sizes in Properties (counted once and remembered, "Calculated just now"; shown at once
  later with how old they are, also when the folder changed since; Recalculate is a queued job, a stopped one stores
  nothing, a finished one replaces the size; a copy drops the entries of the folders it changes; a different folder
  under the same name isn't given the old size), and New Folder with Selection (a free name window, the items moved
  in, Undo, a taken name not replaced, a move failing part-way keeping everything).
- `archives`: Compress…'s volume sizes (offered by what they fit, a DVD's exact size passed in bytes, Custom… with a
  KB size making volumes of exactly that size, the choice remembered); opening a file from the archive window (a
  read-only temporary copy opened with its app, through a test application); dragging items out (onto a Kestrel
  folder: extracted straight there, no temporary copy; for another app: extracted when the drop asks; encrypted ones
  whose password isn't known offered to Kestrel only); day-old temporary copies deleted; and the unzip path (taken
  only without 7-Zip, never before in the tests): listing, single items with [ ] * ? in their names, testing.
- CI (`deb.yml`): unzip installed in the test and sanitizer jobs, for the unzip checks.

Changed:
- `theme`: "a pane moving to another folder cancels its own folder previews" (0.3.2-alpha.14) is now "leaving a folder
  lowers its thumbnail job": leaving no longer cancels them (by design: they wait behind newer ones).
- `installer`: the BleachBit dry run also lists the folder sizes cache and the archive window's temporary copies.
- `ui`: the context menus listed in ui_actions.txt include New Folder with Selection….
- `prefs`: the Jobs window's split between its two lists (`jobs_window_lists`) is kept state.

## 0.3.2-alpha.14

Added:
- `theme`: two windows of one Kestrel asking for folder previews (300 folders for one, 3 for the other). One window
  moving to another folder drops its own requests and not the other's; then a window closing with its previews still
  queued gets nothing more made, while the other window's still come. Guards against the queue belonging to no one
  (previews went on being made for a closed window, and moving to another folder dropped every window's).
- `viewer`: the wheel over the info panel, at its top where it can't scroll, is kept by the panel and doesn't change
  the image, while the wheel over the picture still goes to the next one. Guards against the scroll bar passing an
  unused wheel turn on to the viewer.
- `jobs`: Paste Into Folder queued behind another job on the same drive, its destination moved while it waits. The
  title names how much it copies and where to; the copied files are gone from the clipboard once pasted; it fails
  with one message saying the folder was moved while it waited, and copies nothing; its log starts with what started
  it, in which window and when, from where to where, and how long it waited for which job.
- `sidebar`: a mount that's in the mount table only after the sidebar's refresh, with no further event, still shows;
  and a drive mounted while mount changes keep coming every 100 ms shows within a second. Guards against the
  sidebar's update being put off by a stream of changes, or missing a mount's later steps.

## 0.3.2-alpha.13

Added:
- `archives`: Open as Archive on a big tar (60,000 files), the freeze fix: the longest pause of the UI thread while
  it's read and shown stays under 0.5 s (it was about 4 s in a CI-like container, as the list was built on the UI
  thread); the items are folders first, by name, at every level; a file's time is read right (the new date parsing);
  the status counts every file and folder; and a click on a column's header still sorts (the tree no longer keeps
  itself sorted).
- `ui`: 1,500 selected folders dropped onto a sibling folder (a move on one drive), the freeze fix: they all move, and
  the window is never held for more than 2.5 s (about 0.4 s now; 5.7 s before in a CI-like container, as each folder
  leaving the view deselected it and the status line counted the whole selection again).

## 0.3.2-alpha.12

Added:
- `archives`: tar archives with a password (through gpg), the new feature: a tar and a tar.gz made with a password are
  gpg-encrypted (gpg itself opens them with that password only, and no file name is readable in them); the command
  shown pipes through gpg without the password; `.tar.gz.gpg` is recognised as an encrypted tar (a `.txt.gpg` isn't an
  archive); the quick password test, listing (only with the password), extracting and testing them; a wrong password
  is reported as one and extracts nothing; one failing part-way (a file-size limit) keeps the archive of that name and
  leaves no part; Compress… offers the password and names the archive `.tar.gz.gpg`; Open as Archive and Extract Here
  ask for its password (again when wrong). Skipped where gpg isn't installed.
- `fileops`: the "password never on a command line" check also covers a tar.gz through gpg.
- `archives`: checksum files made while a tar archive is compressed, the new feature: a tar.gz's `.sha256` and `.sfv`
  and a plain tar's `-CHECKSUM` hold the finished archive's hashes (compared with sha256sum's, md5sum's and
  sha512sum's, and checked with Verify Checksums' own code); the command shown names them; a tar.gz through gpg gets
  the encrypted file's hash; a full disk part-way (a file-size limit, with a 3 MB file and with a 64 MB one, where the
  programs are still being fed) says "File too large" and keeps the old archive and checksum file and no part; ✕
  part-way leaves no archive, checksum file or part; Compress… offers it for tar formats and not for others.

Changed:
- `archives`: the Secret Service's name is taken (for the "no keyring" checks) before the gpg checks, not after. Their
  Open as Archive and Extract Here look for a saved password, which had the test's bus start a gnome-keyring on CI;
  the 6 keyring checks after them then failed.
- `archives`: "big encrypted 7z" now says which part failed (making the archive, and how long it took; the right
  password; the wrong one; an error), since it failed once in a full run in a CI-like container and couldn't be
  reproduced in 11 runs after.
- `fileops`: that check's watcher now counts the password in any command line, not only ones naming the archive: a
  tool making the archive names only its hidden part file, so a password leaked there went unseen.
- `installer`: `kes-setup --install-optional` now also installs `gnupg` (tar archives with a password).

## 0.3.2-alpha.11

No test changes.

## 0.3.2-alpha.10

Added:
- `viewer`: an image deleted and put back with Ctrl+Z is shown, even when a read of it started before the delete ends
  afterwards with "can't read" (sent to the viewer by the test, so it doesn't depend on timing). On CI (0.3.2-alpha.9)
  that race left "Cannot display this image" and failed the four crop and resize checks after it.

## 0.3.2-alpha.9

Added:
- `archives`: big archives (25,000 files with long names, a listing of several MB) in 7z, zip, tar.gz, rar and zpaq
  are listed in full, the first folder too: the listing used to keep only its last 1–2 MB (7z and zip then listed
  nothing, rar and zpaq only the last files), and a big tar.gz hung (tar's output pipe filled while it was being fed).
  The quick password test refuses a wrong password on a big encrypted 7z (it found no file to test with in the cut
  listing, and took any password). The earlier archives were far too small to reach the limit.

## 0.3.2-alpha.8

Added:
- `slow`: writing onto a tape with dd (the new choice): the question (a free window) marks only the jobs onto the tape,
  Cancel starts nothing, "Don't ask again" saves the setting and then nothing is asked; dd writes the file whole, with
  its command and output in the job's log and no buffer of Kestrel's; a dd write that fails part-way (a file-size limit,
  which dd inherits) leaves no half-written file and says dd failed.

Changed:
- `sidebar`: the ZFS examples use the pool name "pool".
- `viewer`: closing with an unsaved rotation, then Don't Save, failed on Qt 6.4 (CI): the viewer closed itself from
  inside the question's own click, which Qt 6.4 ignores (now it closes once the click is handled). The check now says
  which part failed: whether it asked, closed, and the file's orientation tag.
- `slow`: the check that GNOME's low-disk-space check skips a tape crashed the test where GNOME's housekeeping settings
  aren't installed (GSettings aborts on a key without its schema): skipped there, and the system-monitor check that
  follows no longer reads those settings.
- `sidebar`: the disk-size check only picks a disk whose device node exists; in a container the host's disk is in the
  mount table without one, so lsblk failed and the check with it.

## 0.3.2-alpha.7

Added:
- `slow`: while a job writes to a tape, GNOME's low-disk-space check skips it, and afterwards its list is as it was (a
  tape the user put there stays); a system monitor that's running is named in the job's log, once. These programs
  asking the tape for its free space were what slowed real copies to a quarter of the drive's speed. The test keeps
  GNOME's settings in memory (GSETTINGS_BACKEND=memory) and uses a stand-in /proc.
- `viewer`, a new test for the image viewer's new features and for saving its changes. Rotations saved through the
  orientation tag (every orientation, turn and flip shows as the viewer showed it, and only the tag's two bytes
  change), exiftool adding a tag, a PNG written again turned with its text; a rotation or crop that fails part-way (a
  file-size limit) keeping the original and leaving no part file; Straighten; crop and resize as a new file or over
  the original, keeping the camera details and generation prompts; GPS positions. In the viewer: decoding at screen
  size and at full size when zoomed in, double-click and pinch zoom, Keep Zoom, the filmstrip, the info panel (map
  link, histogram), compare with linked zoom, star, move and copy to (recent) folders, Delete and Ctrl+Z, the question
  about saving a rotation (also on closing), the remembered size and panels, and the folder's order for a selection.
  Crops and resizes are previewed first: shown at once (turned as shown, on the screen-size copy) with nothing
  written, Esc undoing them, and saved by Ctrl+S or when leaving the image (asked, with Discard).
  The viewer's right-click menu in Preferences → Menus: every entry has an id in the default layout's order, a saved
  layout and Reset apply to it, and the editor offers it.
  CI's test jobs install exiftool and jpegtran so those checks run there.
- `installer`: BleachBit's cleaner also clears the image viewer's recent Copy To / Move To folders (a list of paths
  Kestrel now keeps).

Changed:
- `installer`: `kes-setup --install-optional` also installs libjpeg-turbo-progs (the viewer's Straighten).
- `prefs`: the viewer's window size and recent folders are kept state, not settings.

## 0.3.2-alpha.6

Added:
- `slow`: a copy onto a tape logs a line every 5 seconds (the time, the tape's speed or that it waits for the buffer,
  the source's, the buffer, how far it is), both while the buffer fills and while the tape writes. It also guards
  against the copy hanging when that line asks the buffer for its figures (the buffer called back while holding its
  own lock).
- `prefs`: Preferences shows the tape buffer's size now, and says when the system can't spare the largest size.
- `sidebar`: no drive is asked anything over 7 idle seconds; a drive mounted shows and one unmounted goes as soon as the
  mount table changes (MountWatch); a disk's size from /sys equals lsblk's; the free space is asked again when a job
  ends.
- `slow`: a tape isn't asked its size or health while a job uses it (each question is a command to the drive, which
  slowed real copies to a quarter of the drive's speed), and is asked again once the job has ended.

Changed:
- `slow`: the tape buffer's size is what's safe: half the memory available at most (counting what it holds), never
  leaving less than the share kept free, the 16 GB limit (now 10^9-byte GB, as Kestrel shows sizes) only where that's
  safe; checked on a mid-sized, a big and a squeezed machine.

## 0.3.2-alpha.5

Changed:
- `slow`: the tape buffer's size: at most 16 GB by default (20 GB available of 32 now gives 16 GB, not 16.8), and with
  the limit set to 0, the memory available less what's kept free, as before.

## 0.3.2-alpha.4

Changed:
- `groups`: "collapsing a long group from its footer brings its header into view" checks that the header is in view
  from top to bottom, not that its whole rectangle fits: on Qt 6.4 (Ubuntu 24.04, CI) a header spanning the list's
  columns is wider than the view, so the check failed there although the header was in view.

## 0.3.2-alpha.3

Added:
- `sidebar`: the Mounts section. Which mounts are shown (GIO's rule, from the mount table), when a recent server
  counts as connected, and, with mounts added through the new `KESTREL_TEST_MOUNTS` hook and two recent servers: the
  section's entries in order, a mount Devices lists not repeated, a mount's name and tooltip, a greyed server, moving
  an entry (saved), collapsing, and Remove from Recent Servers (also told to other Kestrels).
- `chooser`: free names (download numbering from (1), the suggested name made free where the window opens and in
  each folder visited, the original back where it's free, a name the user typed left alone, an app replacing a file
  keeping its name and the Replace question, several files each made free without asking) and save locations (only on a
  Save window's Overview, starting there when set to and not when replacing a file or opening, added, moved and removed
  from the page and saved, a click going there).

- `slow`: writing onto a tape, watched with inotify (the new `DirWatch` in common.h): a new file is written under its
  name with no part file; replacing one there, and a new file onto a floppy, still go through a part file; a taken name
  is kept; a write failing part-way (RLIMIT_FSIZE) and a cancel while writing leave no half-written file and no part.
- `admin_helper`: the same onto a tape through the admin helper (it recognises a tape from the mount table, or a
  `KESTREL_TEST_SLOW` folder when not root): written under its name, a replacement through a part file, a taken name
  kept, a failed write leaving nothing.

- `groups`: collapsing a date group from anywhere in it: the footer at each open group's end (named, counted, not
  selectable; a click collapses the group and makes its header current, also for a 150-item group whose header was out
  of view), Ctrl+Left / Ctrl+Right on an item, Collapse This Group on an item's menu, and Collapse / Expand All Groups
  on empty space's menu and the Sort menu.

- `search` (a new test): the search bar's filters (types by name, size, dates, a new tab keeping them) and searching
  inside files: with ripgrep and with grep (matching lines, case, regular expressions and a bad one, subfolders,
  binary and hidden files), ✕ ending the program, which folders the index covers and when Automatic uses it or reads
  the files (also after an index that finds nothing), nothing read on a tape, and opening a result at its line.

- `search`: a progress bar while a search runs ("Searching…" in the view), "Nothing found" in the view with no window
  and no progress bar left (also filtering the folder by name), nothing said over results, and the progress bar gone
  after ✕; the view's message is checked as seen: above the list view.

Changed:
- `sidebar`: the saved section order now includes Mounts.
- `search`: a regular expression that doesn't parse is reported in the view ("Nothing found" and why), not the status
  bar.
- `installer`: --install-optional's expected packages include ripgrep.
- `groups`: the list of what a grouped view shows leaves out the new footers (checked on their own).
- `slow`: "a copy onto a tape writes nothing while the buffer fills" now looks for data written (a new file on a tape
  is made under its name, empty, when it's opened).
- `chooser`: "saving over a file asks first" now has the user type the taken name: a suggested name that's taken is
  made free instead of asking.
- `installer`: the BleachBit cleaner's new Save locations option (its action, the dry run and the real clean removing
  `chooser_places` and keeping the other settings).
- `prefs`: `sidebar_mounts_order` (the Mounts section's order) is kept state, not a setting to choose.

## 0.3.2-alpha.2

Added:
- `jobs`: the drives of a job's items, asked all at once (2,000 items on one drive and one on a tape), come back as
  each drive once, in order. The job queue now asks this way, looking each mount up once instead of each item, so a
  job of thousands of items doesn't hold up the window.

## 0.3.2-alpha.1

No test changes.

## 0.3.2-alpha.0

No test changes.

## 0.3.1-alpha.16

Added:
- `slow`: the type GLib would read from a file's contents (Open With, opening, Recent) is taken from its name on a
  tape (a PDF without an extension is still "unknown" there, while elsewhere it's read as a PDF), and opening a file
  on a tape adds it to Recent with its type by name. The alpha.14 check couldn't see this read: its busy tape only
  made stat wait. Covers the fix for the freeze still seen when double-clicking a checksum file on a tape.

## 0.3.1-alpha.15

Added:
- `jobs`: the Tower's Windows tab shows each window's size, the minimum it asks for, its border and screen, its
  recent resizes (a resize made by the test) and a child wanting more room than the window has, and saves it all to
  a file in the home folder. Covers the window diagnostics.

## 0.3.1-alpha.14

Added:
- `slow`: while the tape is busy seeking (`util::set_test_busy`: every stat under it waits 1.5 s, as LTFS's do),
  selecting files there and opening a checksum file there don't hold the window, the info panel fills in later and
  Verify Checksums opens. The alpha.11 check only made the checksum file's read wait (a FIFO), so it missed the window
  asking the tape for other things meanwhile (reported on alpha.12: it still froze). Covers that fix.

## 0.3.1-alpha.13

Added:
- **`prefs`, a new test for the Preferences window and the right-click menus' layouts.** Every setting the code reads
  is in the list Preferences is built from (or is kept state, like window sizes): it scans `src/` for the keys read,
  so a new setting can't be left out. The window: a free window with its tabs and a control for every setting. A
  change applies at once to every open window (hidden files, list view, icon size, Pinned & Starred First, the default
  sort, the sidebar, folder preview settings) and is told to the other Kestrels with its key; a change made in a
  window (Ctrl+H, the zoom) shows in the open window; the search box filters rows and greys out tabs; a homepage that
  isn't a folder is refused; the file chooser's view follows the main windows' until set; a format's level and Extract
  To…'s choices set there are what those windows start with; Reset This Tab and Reset All. The menus: every entry has
  an id and the default layouts list them all in the menu's order (for a folder, a file, an image, an archive,
  several items and the empty space); a saved layout hides, reorders, takes an entry out of its submenu and puts two
  in a submenu of the user's own, with no stray separators; an entry that doesn't apply doesn't appear, and appears
  where it was put when it does; a layout is for its kind only; an entry added in a later version appears in its
  default place; the editor hides, moves up, into and out of a submenu, and its changes are told to the other
  Kestrels. Covers the new Preferences and customised right-click menus.

- `slow`: the tape write buffer. Its size follows the memory figures (a stand-in /proc/meminfo,
  `KESTREL_TEST_MEMINFO`): available less 10% of the total, the share kept free, the largest size, at least 64 MB. The
  buffer gives every file whole and in order (also ones larger than it), drops skipped files, and leaves files not
  planned or changed since to be read directly. With a slow source (`KESTREL_TEST_TAPEBUF_DELAY_MS`): writing starts
  only once it's full and, below the drive's lowest streaming speed, waits for a full buffer again after running dry;
  within the speed-matching range it carries on without refilling. A copy onto the tape folder fills first, writes
  nothing meanwhile, then every file whole and in order, and its log gives the buffer's figures; pausing pauses the
  reading (/proc/self/io); ✕ while filling leaves nothing, not even a part file; a file that doesn't fit isn't started
  (`KESTREL_TEST_TAPE_FREE`). LTFS's index sync is read from a stand-in ltfs command line (`KESTREL_TEST_PROC`). Properties
  doesn't read a tape file to name its type. Covers preventing shoe-shining on tapes.

Changed:
- `atc_sync`: "Saved Preferences are reported" is now "a change made in Preferences is reported, with the setting
  that changed": Preferences has no OK any more (changes apply as they're made), and the message names the keys.

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

- **`archives`, a new test for extracting through the window and remembered passwords.** Which archives a selection
  holds (a split set once, no other files) and the menu it gets (Extract All for several, Extract for a split set
  selected whole, nothing with other files mixed in); the quick password test for 7z (also with encrypted names), zip,
  rar and zpaq; Extract All asking once, again only for an archive the typed password doesn't open and after a wrong
  one, extracting each into its own folder and ending with one summary (a damaged archive among them leaves no
  folder); no keyring seen and no Remember box while the test holds the Secret Service's name (so the bus can't
  start gnome-keyring by itself, as it would on a clean Ubuntu); a cancelled password window skipping just that archive; ✕ while it waits for a password closing the window
  and stopping the job with a summary; the password window a job's thread asks with being a free window; Extract
  Here on one archive as before. With a real gnome-keyring-daemon on the
  test's private bus (skipped where it isn't installed): fingerprints follow the contents, nothing is saved without
  Remember, a ticked one is saved, found again after a rename and move, other saved passwords are tried, and
  Preferences forgets them. Covers Extract All and remembered archive passwords.
  Also, for Open as Archive and Test Archive: 7z, zip, tar.gz, rar and zpaq archives are listed (paths, sizes) and a
  single file or a folder extracted from each into a folder (a taken name gets a number, nothing replaced, nothing else
  extracted, no part folder left; rar and zpaq skipped where not installed); extracting single items that fails
  part-way (a file-size limit standing for a full disk) leaves nothing; a folder taken out on its own leaves out a
  symlink that would then lead out of the destination; Test tells sound 7z, zip and tar.gz archives from damaged
  ones, names the damaged file, and tells a wrong password apart; the menu offers Open as Archive / Test Archive(s)
  (not Open as Archive for a single .gz); Test Archives asks for an encrypted one's password and tells all results
  in one message; the archive window asks for the password of an archive with encrypted names, shows its tree in a
  free window (raised, not doubled, when opened again) and extracts the selected files without asking again; with
  encrypted files but visible names it opens without asking and asks once when one is extracted.
  The keyring checks wait for the test's own gnome-keyring-daemon to own the Secret Service's name (asked with
  NameHasOwner) before using it: asking libsecret earlier could make the bus start a second, locked keyring that took
  the name first (seen on a clean Ubuntu 24.04).
- CI's test and sanitizer jobs install `gnome-keyring` and `p7zip-full` (so these checks run there) and `libsecret-1-dev`.

- `ui`: files dragged from a view grouped by period or by day onto a folder in it move into that folder, in the grid
  and the list. The drag is what the view's own model gives for the selected items, with the actions it offers (as
  the view's startDrag asks): the earlier drop checks used files from outside the view, so they missed that a grouped
  view's drag carried no files and offered no move.

Changed:
- `theme`: the preview-build checks run the builder directly (it's no longer a thread of its own but runs in a job).
  Added: each Generate is a job of its own (two at once are both queued and shown in the status bar); Generate keeps
  previews already made while Regenerate Previews makes them again (a cached preview replaced by a grey stand-in
  with the same tags must be gone), for the folder's own files and its and its subfolders' previews but nothing
  deeper; Regenerate Folder Previews (here and recursively) and Regenerate Previews Recursively; ✕ stops a preview job
  also after its folders are all found (the workers stop, not just the walk); the Previews submenu's entries. Covers
  preview builds as jobs and the Regenerate options.
- `ui_actions.txt`: Generate Previews… moved into a Previews submenu (folder and empty space), with the four
  Regenerate entries.
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
