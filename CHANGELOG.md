# Changelog

← Back to [the README](README.md) · [Bugs found elsewhere](docs/bugs-elsewhere.md) · [Install guide](docs/install.md) · [Test changelog](tests/CHANGELOG.md)

What changed in each version of Kestrel Explorer, newest first.

## 0.3.3-alpha.1

- **Files whose names aren't valid UTF-8 can be deleted, copied, moved and restored from the trash.** A name from an old archive or another
  system with a stray byte in it (Latin-1, DOS) was listed but then "No such file or directory" to everything done to
  it, so a folder holding one couldn't be deleted, even from the trash. Kestrel now keeps such names byte for byte,
  and a file shown with � in a folder is found too (unless two names in that folder read the same way).
- **A Delete Permanently in the trash that fails or is cancelled keeps the item's record**, so it can still be
  restored to where it came from; the record used to be deleted anyway, leaving the item's location "(unknown)".
  Empty Trash likewise deletes each item's record only after the item.
- **Fixed crashes:** Delete Permanently on a trashed item with no record (as such a failure left behind), and undoing
  New Folder with Selection when something else had been put in the new folder meanwhile.

## 0.3.3-alpha.0 (2026-10-31)

- **Kestrel in ten more languages:** Chinese (Simplified), French, German, Italian, Japanese, Korean, Portuguese
  (Brazil and Portugal), Russian and Spanish. It uses the desktop's language, and English where it has no
  translation. Every text is translated, including the counts each language words differently (Russian's three forms).
  The translations are new and not yet checked by native speakers, so some may be wrong or awkward:
  [docs/translations.md](docs/translations.md) says how to report one.
- **An "Incorrect translation" issue form** in the issue tracker: the language, what's wrong, and a table of the texts
  as Kestrel shows them, what they should say and where they appear.
- **Qt's own texts** (standard buttons, the colour and font pickers) follow the language too, with
  `qt6-translations-l10n`, now a recommended package (the .deb's Recommends and `install.sh --install-recommended`).

## 0.3.2-alpha.34 (2026-10-29)

- **Benchmark on Kubuntu:** `bench/kubuntu.sh` compares Kestrel with Dolphin on Kubuntu 26.04, in a Docker container,
  as the Nemo comparison was made on Linux Mint. Its report is a "Kestrel vs Dolphin on Kubuntu" section for the
  performance page.
- **Benchmark: four tests of the app in use.** Besides launching the app on a folder, the benchmark now times what
  happens in a window that's already open: going to a folder through the location bar, jumping to the end of the
  600-image folder (until its last thumbnails are there, with the CPU time every process used for them, helpers
  included), 5,000 files arriving in the folder it shows, and how long the window takes to answer Back and Forward
  while it's pasting 20,000 files.

## 0.3.2-alpha.33 (2026-10-29)

- **KDE Plasma (Kubuntu 26.04).** Kestrel now follows Plasma where it followed only GNOME and Cinnamon:
  - **Set as Wallpaper** sets Plasma's wallpaper; it did nothing there.
  - **System Settings → Recent Files → "Do not remember"** stops Kestrel adding to Recent and counting Frequent
    Folders, as GNOME's File History switch does. Preferences → Privacy says where to find both.
  - **Open Terminal** opens Konsole first, even with a GNOME terminal installed too.
  - **Dragging files into another app gives that app the focus** on Plasma on Wayland too (Kestrel asks KWin, with
    nothing to install), as it already did on GNOME and on X11.
  - Without KDE's Qt integration, the **icons** are Plasma's icon theme rather than GNOME's.
- **`kes-setup` on KDE Plasma:**
  - `--dock` (and the question `--default` asks) puts Kestrel in Plasma's taskbar in place of Dolphin, and `--undo`
    puts Dolphin back. A "default file manager" pin already opens Kestrel.
  - The file chooser falls back to KDE's own instead of GNOME's, which Kubuntu doesn't have (on Linux Mint, to Mint's).
  - `--undo` no longer makes GNOME Files the app for the trash (`trash:///`) when no app had it before, as on Kubuntu;
    it takes Kestrel out and leaves the desktop's own choice.
  - `--default` says when an open Dolphin window keeps "Show in folder" until it's closed.
  - `--default` and `--undo` no longer print "qtpaths: not found" on Kubuntu 26.04.

## 0.3.2-alpha.32 (2026-10-29)

- Version bump only; see the [changes to the tests](tests/CHANGELOG.md#032-alpha32).

## 0.3.2-alpha.31 (2026-10-29)

- **Hide places from the sidebar.** Right-click Recent, Starred, Trash or any other place in the sidebar →
  **Hide from Sidebar**, or untick it under Preferences → Sidebar → **Places shown**, where it can be ticked back.
  It applies to every window and every Kestrel.
- **A Privacy tab in Preferences.** Turn off, one by one, what Kestrel remembers as you use it: adding the files you
  open to Recent, counting the folders you open (Frequent Folders), the servers you connect to, and the image
  viewer's Move To / Copy To folders. It shows whether the desktop's File History switch is on, and has
  **Clear Kestrel's History…** and **Clear Recent Files…** (the desktop's list, which other apps share).
- **Finished jobs stay in the Jobs window until Clear Finished.** Only the newest 50 were kept, and a Kestrel that
  closed took its finished jobs out of the other Kestrels' Jobs window. Now every finished job stays listed until
  you clear them; one from a Kestrel that has closed says its log went with it.
- **Generate Previews, for just this folder.** The Previews submenu had Generate Previews Recursively and Regenerate
  Previews but nothing to make the missing previews of only the folder itself: **Generate Previews** does that.

## 0.3.2-alpha.30 (2026-10-29)

- Version bump only; see the [changes to the tests](tests/CHANGELOG.md#032-alpha30).

## 0.3.2-alpha.29 (2026-10-29)

- Version bump only; see the [changes to the tests](tests/CHANGELOG.md#032-alpha29).

## 0.3.2-alpha.28 (2026-10-29)

- **One Jobs window for all of Kestrel.** A folder opened from another app can start a separate Kestrel, and each
  had a Jobs window of its own, where the others' jobs showed without their steps or logs ("its log is in that
  window's Jobs window") and their finished jobs and thumbnail jobs didn't show at all. Now there's one: opened from
  any window, it closes the one another Kestrel had open, and shows every job wherever it runs, whole: its steps and
  log (running or finished), every Kestrel's finished jobs, and every Kestrel's thumbnail jobs, which Pause and Stop
  work on. Pause works on another Kestrel's job too, and Clear Finished clears every Kestrel's. Kestrels on this
  version and older ones don't share jobs (the tower's messages changed).

## 0.3.2-alpha.27 (2026-10-29)

- **"&" shows as "&" in tab and submenu names.** The Preferences tab "Images & Videos" read "Images Videos" with
  the V underlined (or "Images_Videos"), as did a folder's tab when its name has an "&" ("Tom & Jerry"), and, in a
  customised right-click menu, "Links & Shortcuts" and any submenu of your own with an "&" in its name.
- **Screenshots in the feature guide.** The Overview, grouping by day, the Jobs window, Preferences and its search,
  and customising a right-click menu are now shown in [Features](docs/features.md).
- **Numbers, sizes and dates in your system's format.** Digit grouping, the decimal mark and the order of a date follow
  your locale's settings (as GNOME Files does), not fixed English ones: on a German system a size reads "1,5 GB" and a
  date "28.10.2026". Dates written as 2026-10-28 (Properties, the info panel, Find Duplicates, Compare) now show in
  your locale's own short form, with the time in its 12- or 24-hour clock; job logs keep their exact timestamps.
- **Counts read as English.** Messages that said "1 file(s)", "2 item(s)" or "1 bytes" now say "1 file", "2 items"
  and "1 byte", and the few that built a plural by adding an "s" read correctly throughout.
- **BleachBit always loads Kestrel's cleaner.** `kes-setup --bleachbit` wrote the cleaner with whatever file
  permissions the session gave new files; where those let other users edit it (a umask of 000), BleachBit ignored the
  cleaner as a security measure and showed no Kestrel options. It's now always writable only by you, and
  `kes-setup --bleachbit` says that BleachBit's window will ask once whether to trust the new cleaner.
- **Ready for translations.** Every text Kestrel shows can be translated, with only English shipping for now: a
  translation is one file in `translations/`, loaded for the desktop's language (see
  [Development](docs/development.md#translations)). Building Kestrel now needs `qt6-tools-dev` and `qt6-l10n-tools`
  (`./install.sh` installs them).

## 0.3.2-alpha.26 (2026-10-27)

- **A package for each Ubuntu release.** Each release now gets a .deb built on it, with its own Qt and libraries,
  instead of one 24.04 build for all: Ubuntu 26.04 gets the build its tests ran on. The release suites get their
  own (`stable` and `dev` keep the 24.04 one, which installs everywhere), and the GitHub releases have both, named
  `…~ubuntu24.04` and `…~ubuntu26.04`: take the one for your release (`ubuntu24.04` on Linux Mint 22). A system
  upgraded to a newer Ubuntu release upgrades to that release's build.
- **Install instructions for each Ubuntu release.** The README, the install guide and the apt repository's page
  now give Ubuntu 26.04 and Ubuntu 24.04 / Linux Mint 22 their own commands, each naming its release's suite, so
  nobody ends up with another release's package. The previous commands fell back to `stable` when a release's suite
  wasn't there yet, which gave 26.04 the 24.04 package. They also explain how to switch from `stable` or `dev` (set
  up before 0.3.2-alpha.25), and which .deb to take when downloading one by hand.
- **Automated builds run on Node.js 24.** GitHub's actions for checking out the code and passing packages between
  jobs moved to their Node.js 24 versions, so the runs no longer warn that Node.js 20 is deprecated.
- **Drag-selecting up and to the left works again in the grid.** Pressing in empty space and dragging up and to the
  left selected nothing (since 0.3.2-alpha.17); the other directions, and the list view, worked.

## 0.3.2-alpha.25 (2026-10-27)

- **apt suites for each Ubuntu release.** The apt repository now has a suite for each Ubuntu release Kestrel
  supports, named after its codename: `noble` (Ubuntu 24.04, Linux Mint 22) and `resolute` (Ubuntu 26.04), each with a
  `-dev` one for development builds. When Kestrel stops supporting a release, its suites keep the last version that
  installs there, so people on it keep a working Kestrel and new installs still succeed, instead of updates stopping
  without a word and installs failing on missing dependencies. The install commands pick the suite from
  `/etc/os-release`, so Linux Mint and every Ubuntu flavour get the right one; they fall back to `stable` or `dev`
  on a release without its own. `stable` and `dev` keep working and get every build too, but if you added the
  repository before, run the install commands again to switch to your release's suite.
- **Tested on Ubuntu 26.04 too.** The automated tests now run on Ubuntu 26.04 as well as 24.04, and a package is only
  built when both pass, so a change that breaks only with 26.04's newer Qt or tools is caught before it reaches you.

## 0.3.2-alpha.24 (2026-10-27)

- **The image viewer picks up new images in the folder** without being reopened. Opened on an image of a folder, it
  adds images that appear there later (for example from a program generating them) to its list and filmstrip, in
  their place in the folder's order, without changing the image you're looking at. Files still being written wait
  until they're finished. Not on tapes and floppies.
- **Compare two images with a slider.** Select two images and right-click → **Compare in Viewer**: the viewer shows
  them over each other with a divider in the middle, B on its left and A on its right. Drag it fully left to see only
  A, fully right to see only B; zooming and panning move both, so the same spot stays under the divider. In the
  viewer, **D** switches any comparison (X) to the slider and back.

## 0.3.2-alpha.23 (2026-10-27)

- **Fixed: the file chooser asked a tape questions while Kestrel was writing to it.** The file chooser Kestrel
  provides to other apps didn't know about Kestrel's running jobs, so opening one asked the tape for its cartridge's
  health, and then for its size every 30 seconds, in the middle of a write; each question is a command to the drive
  that interrupts its writing. It now never asks a tape anything. A Kestrel window that has just opened also waits
  until it knows the other windows' jobs before asking a tape.
- **Faster builds:** the automated builds now install their packages from Ubuntu's main archive instead of the
  build machines' default Azure mirror, which was very slow and made every run take much longer. New test and dev
  builds (and their apt updates) arrive sooner after each change.

## 0.3.2-alpha.22 (2026-10-27)

- Version bump only; see the [changes to the tests](tests/CHANGELOG.md#032-alpha22).

## 0.3.2-alpha.21 (2026-10-27)

- **Calculate Folder Size** in a folder's right-click menu (also for several folders): opens Properties and counts the
  size afresh, also when a size was remembered from before.
- **Open Metadata** in a file's right-click menu: opens Properties straight on its Metadata tab. Shown for images, and
  with exiftool installed also for videos, audio, PDFs, e-books and office documents.
- **Kestrel's own cache folder gets no thumbnails** (`~/.cache/kestrel-explorer`: its folder previews, animated
  previews and temporary copies). Browsing it, or generating previews over it, made thumbnails of those previews
  and wrote them back into the caches; files and folders there now show plain icons.
- **Fixed: unlocking an encrypted drive failed** with "Expected G_MOUNT_OPERATION_HANDLED but got 2" as soon as the
  password window opened, and entering the password then did nothing. The drive now waits for the password and
  unlocks with it. The same applied to questions asked while mounting.

## 0.3.2-alpha.20 (2026-10-27)

- **Fixed: a drive's health could show billions of bad sectors** (e.g. "4294967299 bad sectors" on the Overview and
  sidebar) when its details listed only 3. Some drives put extra bits in the S.M.A.R.T. counters that udisks adds up;
  the health line now shows the real count.

## 0.3.2-alpha.19 (2026-10-26)

- Version bump only; see the [changes to the tests](tests/CHANGELOG.md#032-alpha19).

## 0.3.2-alpha.18 (2026-10-26)

- **Choose what the Overview shows** (Preferences → Overview): tick the sections to show and put them in any order;
  the card size (small, normal, large); whether the system drive is shown alone (as before), with its other mounts
  (`/boot`, `/boot/efi`), or not at all; the usage bar; and free space as a size or a percentage.
- **Two new Overview sections**, off until chosen: **Starred** (your starred files and folders; click a file to open
  it, right-click to unstar) and **Frequent Folders** (the 12 folders you open most in Kestrel, counted while the
  desktop's File History switch is on; right-click one to remove it). `kes-setup --bleachbit`'s History option now
  deletes that count too.
- **Drive health on the Overview.** Each drive's card shows its health from S.M.A.R.T., as udisks reports it (no root
  needed): "Healthy", or a warning such as "3 bad sectors", with its temperature and how long it has been powered on.
  A failing drive's card turns red, and a notification says so the first time Kestrel sees it. Click the health line
  (or right-click → Drive Health…) for what the drive reports, in plain words. NVMe drives are read too; drives in USB
  enclosures that don't pass it through say "Health not available"; a ZFS pool shows its worst disk. A drive's
  Properties shows the same line.
- Bookmarks on a tape or floppy no longer ask the drive anything to draw their Overview card.
- **Drag an image out of the image viewer** and drop a copy anywhere that takes files: the desktop, a folder, a
  browser's upload field, a chat or mail app (apps that only take pictures get its pixels). With the whole image in
  view just drag it; zoomed in, hold Alt (a plain drag pans). It's always a copy, never a move, even into a folder on
  the same drive; turned or edited and not saved, a copy as shown is dragged. **Ctrl+C** in the viewer now copies the
  file too, so Ctrl+V in a folder pastes it.
- **Rotate ▸** in an image's right-click menu (one or several): 90° either way, 180°, or flipped, saved in the files.
  JPEGs are turned without re-compressing them (jpegtran), keeping their metadata; one whose size isn't whole JPEG
  blocks asks whether to trim its edge or only set its orientation. PNG, BMP, TIFF and lossless WebP are saved again
  losslessly; HEIC, AVIF and lossy WebP ask first.
- **Convert Image…**: to JPEG, PNG, WebP, AVIF, HEIC or TIFF (what's installed can write), with a quality, a new
  size, the metadata kept or not, and a background for transparency; e.g. iPhone photos to JPEG.
- **Convert Video…**, **Extract Audio…** and **Convert Audio…** (with ffmpeg installed): presets and every option in
  plain terms, the exact command shown, the video's details and a rough size; a real percentage and time left in the
  status bar with ✕; several files as one job with one summary. Extract Audio keeps the sound as it is (instant,
  lossless) or converts it, from one track or each. Convert Audio keeps tags and cover art, can even out the volume
  across a batch, and converts a whole folder with its subfolders. Nothing half-made is ever left, and no file is
  replaced; the originals can go to the trash afterwards. `libheif-examples` (HEIC and AVIF output) is a new optional
  package.

## 0.3.2-alpha.17 (2026-10-26)

- **Find Duplicates…** (right-click a folder, several folders, or a drive in the sidebar or on the Overview): finds the
  files with the same contents, quickly (only files of the same size are read: their beginnings, then the whole file),
  as a job with progress and Stop. Options: the smallest size, subfolders, hidden files, skipping hard links. The
  groups show each file's thumbnail, folder, date and size, and the space that can be freed; keep the oldest, the
  newest or those in a folder, then **Move the Others to Trash** (undoable) or **Replace the Others with Hard Links…**
  (same drive, asking first). A group with no copy kept, or a file changed since the search, is never touched.
- **Compare With…** (right-click a folder): lists what differs between two folders, through their subfolders: only on
  one side, newer on one side, or other contents (by size and date, or byte by byte). Open, copy across (asking before
  replacing) or trash from the list.
- **Sync To…** (right-click a folder, or ☰ → Sync Folders…): makes a destination match a source, one way, after a
  preview of exactly what will be copied, replaced and (if chosen) deleted. With rsync when it's installed (it resumes
  an interrupted sync and only sends what changed), or Kestrel's own copy; a file being replaced is kept until its new
  copy is complete. Deleting on the destination is off by default and asks again. Save a pair ("Pictures → Backup
  HDD") to run it again in one click; a pair on a removable drive is found wherever the drive is mounted. `kes-setup
  --bleachbit`'s cleaner has a new option for the saved pairs, and `rsync` is among the optional packages.
- **Verify Signature.** Double-click a `.sig`, `.asc`, `.gpg` or `.sign` signature (or right-click → Verify
  Signature): gpg checks it against the file it signs, and Kestrel says plainly what that means: valid, signed by whom
  and when (and, if you haven't verified that key, that anyone can make a key with that name, with its fingerprint to
  compare); a bad signature; or the signer's key isn't in your keyring, with **Fetch Key…** (asking first). A signed
  checksum file (`SHA256SUMS` with `SHA256SUMS.gpg`) is checked first, then every file it lists.
- **Choose when thumbnails are made.** Under the Jobs window's thumbnail list (and in Preferences → Images & Videos),
  **Thumbnailing**: wait until the drive is free of other jobs (as before, the default); one per drive, even while
  it's in use (a folder's previews come during a long copy on that drive, one folder at a time); or paused (no
  thumbnailing until switched back; what's made still shows, and the folders' jobs carry on afterwards). It applies to
  every Kestrel window.
- **Drag files from one tab to another.** Hold them over a tab for a moment and it opens, so the drag can go on into
  its folder or a folder there; drop them onto the tab itself to put them in its folder. The same keys as any drop
  (none: move on the same drive, copy to another; Ctrl copies, Shift moves, Ctrl+Shift links, Alt asks), and the tab
  is highlighted while it would take them. The Overview, Starred, Recent and search results open but take no drop.
- **A selection box only as big as the item.** In the grid, the highlight of a selected or hovered file fits around
  its icon (a wide or tall picture's own shape) and its name, instead of filling the whole cell; and only that counts
  as the item. So a full folder still has empty space to right-click for its own menu (New Folder, Paste…), click to
  clear the selection, or drop files into. A selection rectangle picks the items whose icon or name it touches.
- **Dropping files onto the Trash moves them to the trash** (its tab or the sidebar's Trash), so they can be restored
  and undone. A drop onto the sidebar's Trash used to move them into the trash's folder without the record of where
  they came from.
- **The Jobs window's buttons are where they belong**: Pause, Stop, Start Now, Up, Down and Clear Finished right under
  the jobs list, the thumbnail jobs' Pause and Stop under theirs. They used to sit below both lists, as if they were
  for the thumbnails too.

## 0.3.2-alpha.16 (2026-10-26)

- **Wipe Metadata…** in the right-click menu of one or more images: takes out all of their metadata (camera details,
  GPS location, dates, comments, image-generation prompts) without touching the pixels, as a job in the Jobs window.
  By default the originals are kept and a copy without metadata is made beside each (`photo (no metadata).jpg`; Undo
  moves the copies to the trash). Preferences → Images & Videos can make it wipe the originals instead, with no backup
  (it asks first), and choose whether the orientation and colour profile are kept (on by default, so photos don't
  turn sideways). Needs exiftool; not for camera raw files. A failure or cancel leaves each image as it was.
- **Run jobs all at once.** A third way for the job queue to run jobs, besides by drive and one at a time: every job
  starts as soon as it's queued, also on a drive another job is using (paused jobs still wait). Not the default:
  choose it in the Jobs window's **Run jobs** or Preferences → Files. Jobs on the same drive then compete for it,
  which can be slower overall, and makes a tape or floppy seek back and forth. Kestrels of this version don't share
  the job queue with earlier ones still running.
- **A page listing every right-click menu** ([Right-click menus](docs/context-menus.md)): what each entry does and
  when it shows.

## 0.3.2-alpha.15 (2026-10-26)

- **Jobs no longer belong to the window that started them.** Closing a Kestrel window doesn't ask about its jobs
  or stop them: they go on, shown in another window's status bar. Closing the last window while jobs run asks
  whether to **exit and open the Jobs window** (the jobs go on, and Kestrel exits once they're done and the Jobs
  window is closed) or **stop all jobs and exit**. With the Jobs window open, the last window just closes. A copy or
  move finishes its work (undo, writing a tape's index, saying what failed) even when its window has gone. Quit
  (Ctrl+Q) closes every window the same way.
- **Making thumbnails and folder previews is a job in the Jobs window.** Each folder you look at gets one ("Thumbnails:
  Pictures/2024, 312 of 840"), with the window it's for, progress, Pause and Stop, in a part of the Jobs window of
  its own below the copies, moves and extractions. Leaving a folder no longer throws away its previews: its job waits
  behind the folder you're in now, carries on when nothing newer is waiting, and stops after 3 minutes (Preferences →
  Images & Videos). Going back to the folder puts it first again. A thumbnail job also waits while another job (a
  copy, an extraction…) uses its drive.
- **Remembered folder sizes in Properties.** A folder's size, once counted, shows at once the next time, with when
  it was counted ("Calculated 3 days ago (2026-10-21 14:05)") and a **Recalculate** button, which counts it again as
  a job in the Jobs window. It isn't counted again on its own. Kestrel's own copies, moves and deletes forget the
  sizes of the folders they change. The sizes are kept in Kestrel's cache, which the BleachBit cleaner empties.
- **New Folder with Selection…** in the right-click menu of one or more items: it asks for a name, makes the folder
  beside them (never over a name that's taken) and moves them into it. Undo puts them back and removes the folder.
- **Open as Archive: open and drag out.** Double-click a file in the archive window to open it with its app (it's
  extracted to a temporary copy first; copies are cleaned up after a day). Drag items out onto a Kestrel folder or
  the sidebar to extract them there, or onto another app or the desktop (extracted as they're dropped; up to 500 MB,
  and not encrypted items whose password hasn't been given yet).
- **Volume sizes to pick from in Compress….** When an archive can be split, the size is a list of standard sizes
  named by what they fit (a CD, a DVD, a Blu-ray, a FAT32 USB stick's largest file, an email), each the exact size
  that really fits, or **Custom…** with a size in KB, MB or GB. The size is passed to the program in bytes.
- **Fixed: the Jobs window and the archive window reopened as nothing just after being closed.** Opening one again
  right after closing it could raise the closed one instead of showing a new one.

## 0.3.2-alpha.14 (2026-10-25)

- **Fixed: a closed window's folder previews kept being made.** Thumbnails and folder previews are made by one queue
  for all of a Kestrel's windows, and closing a window took nothing out of it. With another window still open, the
  closed one's hundreds of folder previews went on being made, reading one folder after another, for minutes. Each
  window, tab, viewer and info panel now asks for its own, and closing it, or moving to another folder, cancels what
  it asked for. That also fixes the other way round: moving to another folder dropped every window's previews still
  waiting, not just its own.
- **Fixed: the scroll wheel over the image viewer's info panel went to the next image.** It now scrolls the panel,
  and does nothing when the panel has nowhere left to scroll. Only the wheel over the picture or the filmstrip changes
  images.
- **A job says what it is and where it came from.** A copy or move is titled with what it copies and where to
  ("Copying 2 items to Photos", "Copying “a.txt” to Backup (with dd)"), not just "Copying". Its log in the Jobs window
  starts with what started it (Paste, Paste Into Folder, a drop, Copy To…, Move To…, the image viewer), in which
  window and when, from where to where, and, if it had to wait, for how long and for which jobs.
- **Fixed: a job whose destination was moved while it waited gave one error for each file.** A copy or move queued
  behind another job now checks its destination folder when its turn comes. If the folder was moved or deleted in the
  meantime, the job fails once with a message saying so, and nothing is copied.
- **Copied files are forgotten once pasted.** After a paste of copied files (Paste or Paste Into Folder), the clipboard
  is cleared, so an old copy can't be pasted again by a slip of the mouse. To paste the same files somewhere else, copy
  them again. Cut and paste already worked this way. Paths copied as text are left alone.
- **Fixed: a drive or mount sometimes showed in the sidebar only later.** Mount changes coming in quick succession (a
  container starting, an automounter) put off the sidebar's update until they stopped. It now updates within a
  second, and checks again a little later (from the mount table, asking no drive anything) for a mount that settles
  after the first update, or whose label appears late.

## 0.3.2-alpha.13 (2026-10-25)

- **Fixed: Kestrel froze for a while when Open as Archive finished reading a big archive.** The window's list was
  built all at once on the UI thread, which held up every Kestrel window for 1–2 seconds per 100,000 files. It's now
  built while the archive is read, and opening the window doesn't pause anything.
- **Fixed: dragging hundreds of selected items into another folder froze the window for seconds.** As each item
  left the folder, the status line counted the whole selection again (slowly), so 800 folders held the window long
  enough for the desktop to offer to force quit. It's now counted once, and the move no longer holds the window.
- **Open as Archive reads big archives up to 10× faster.** Reading the files' dates took most of the time (about
  40 s for an archive of 200,000 files, now about 5 s), and listings are read in blocks instead of line by line.
- **The benchmark now compares Kestrel with Thunar, Krusader and PCManFM too** (see
  [Performance](docs/performance.md)), and every file manager is timed opening a folder of 10,000 files, until its
  whole list is shown. That's how Krusader and PCManFM are compared on opening a folder, as they make no thumbnails
  the benchmark can time; Kestrel lists those 10,000 files in 0.55 s, ahead of all but PCManFM (0.48 s).

## 0.3.2-alpha.12 (2026-10-25)

- **Tar archives can have a password.** In Compress…, tar and every tar.* format (tar.gz, tar.zst, tar.xz…) now offer
  **Encrypt with a password**: the archive is encrypted whole with gpg (AES-256), names and all, and saved as
  `name.tar.gz.gpg`. Kestrel opens, lists, tests and extracts these like its other encrypted archives (asking for the
  password, trying saved ones, asking again when it's wrong), and they open anywhere with `gpg -d`. The password goes
  to gpg privately, never on its command line. Needs `gnupg` (almost always installed; now among the optional
  packages).
- **A checksum file made while a tar archive is compressed.** Compress… has **Create a checksum file** for tar and
  every tar.* format (with a password too): the archive's bytes are hashed as they're written (SHA-256, SHA-512, MD5,
  SHA-1, BLAKE2b or CRC32; one file per algorithm or one with all of them), so `name.tar.zst.sha256` is there the
  moment the archive is, with no second read of it, which saves a pass over a big archive or a tape. A failed or
  cancelled job leaves no checksum file, and an older one for the archive it would have replaced is kept.

## 0.3.2-alpha.11 (2026-10-24)

- **The benchmark now compares Kestrel with Dolphin too** (KDE's file manager): see
  [Performance: Kestrel vs Dolphin](docs/performance.md#performance-kestrel-vs-dolphin). Dolphin has no service for
  other apps to ask for file operations, so its own Copy, Paste and Move to Trash are used, with keys pressed in its
  window. The GNOME Files figures are from a new run too.

## 0.3.2-alpha.10 (2026-10-24)

- **Fixed: an image put back with Ctrl+Z could show "Cannot display this image"** in the viewer, if it was deleted (or
  moved) while the viewer was still reading it: the read, failing once the file had gone, was kept for it. Reads that
  were under way when an image was deleted, moved or saved over are now dropped.

## 0.3.2-alpha.9 (2026-10-23)

- **Fixed: Open as Archive left out part of a big archive's contents.** Only the last 1–2 MB of the program's listing
  was read, so in an archive of more than about 10,000 files (7z, rar) or 20,000 (zpaq, zip) the first ones were
  missing without a word, and a big 7z or zip could show as empty. The whole listing is now read, as it's printed.
  The quick password test on big encrypted 7z and rar archives had the same problem and could take a wrong password.
- **Fixed: opening, testing or extracting a big .tar archive (.tar.gz, .tar.xz…) could hang** once tar had printed
  more than its output could hold while Kestrel was still feeding it the archive. Its output is now read as it comes.

## 0.3.2-alpha.8 (2026-10-23)

- **Write to a tape with Kestrel's copy or with dd.** Copying or moving files onto a tape asks which to use: Kestrel's
  copy (a buffer in memory keeps the tape streaming when the source slows down) or dd (each file written straight from
  the source in 512 KB blocks). A dd job is named "Copying with dd" in the Jobs window, which shows dd's command and its
  output live; cancelling or pausing the job stops or pauses dd, and a file it couldn't finish is deleted, as with
  Kestrel's copy. "Don't ask again" keeps the choice; Preferences → Files → Tapes → Write files with changes it.
- **Fixed: the image viewer stayed open after answering its question on closing** (Save, Always Save, Don't Save, or
  Discard for unsaved edits) on Ubuntu 24.04's Qt 6.4. It closes once the answer has been handled.

## 0.3.2-alpha.7 (2026-10-23)

- **Fixed: copies onto a tape slowed to about a quarter of the drive's speed for long stretches.** The cause wasn't
  Kestrel: programs that ask every drive for its free space knock a tape out of streaming, because on LTFS each
  question is commands to the drive. While a job writes to a tape, Kestrel now takes it off GNOME's low-disk-space
  check (which asks once a minute), and puts it back afterwards. A system monitor that's open (GNOME System Monitor's
  File Systems tab asks every few seconds) is named in the job's log, so you know to close it.
- **The image viewer opens images faster.** The next and previous images are decoded ahead of time at the screen's
  size, and the full image only when you zoom in past that. Until an image is ready, its preview from the thumbnail
  cache shows (for RAW photos, made from the camera's embedded preview), instead of an empty window.
- **A filmstrip** of thumbnails along the viewer's bottom (T); click one to jump to it.
- **An info panel** (I): the file's details and the camera's settings, a GPS position with a link to a map,
  image-generation prompts with Copy buttons, all of its metadata, and a histogram. The info line is now Tab.
- **Better zoom:** double-click switches between fitting the window and 100% at the pointer (F still goes full
  screen), a touchpad pinch zooms, the zoom level shows on screen as it changes, and K keeps the zoom from one image to
  the next.
- **Compare two images** (X): the current one and the next, side by side, or one at a time (V, then B switches
  between them). Zooming or panning one does the same to the other.
- **Rotations are saved without losing quality.** R, L and H used to be for viewing only. Now, leaving a turned image
  asks whether to save it (with Always Save; Ctrl+S saves at once; Preferences → Images & Videos: ask, always or
  never). Only a JPEG's or TIFF's orientation tag changes (exiftool adds one to a JPEG without it), and a PNG or BMP is
  written again turned, keeping its generation prompts.
- **Crop and resize** (Shift+C, Shift+S), shown in the viewer at once without touching the file: crop and resize again
  as often as you like, and Esc undoes them. Ctrl+S saves the result as a new file beside the original or over it, as
  you choose (leaving the image or closing the viewer asks the same, with Discard); a JPEG keeps its camera details, a
  PNG its prompts. **Straighten** (Shift+O) turns a JPEG's pixels the way its orientation tag says
  (jpegtran, from libjpeg-turbo-progs, now among the optional packages), for programs that ignore the tag.
- **Quick actions in the viewer:** star (*), pin (P), show in its folder (G), Open With (O), move or copy to a folder
  (M, C) or to one of the last nine folders in one key (Ctrl+1–9 moves, Alt+1–9 copies), and undo (Ctrl+Z), which puts
  a trashed or moved image back in the viewer too. The viewer's Move to Trash can now be undone in Kestrel's windows as
  well.
- **The viewer's right-click menu can be laid out in Preferences → Menus**, like the others: hide entries, reorder
  them, move them into or out of its View and Rotate and Edit submenus, and add separators and submenus of your own.
- **The viewer remembers** its size and position, full screen, and whether the filmstrip and info panel were shown.
- **Several selected images open in the folder's order**, not the order you clicked them in.
- The info panel and Properties show a photo's GPS position as coordinates instead of "yes".

## 0.3.2-alpha.6 (2026-10-22)

- **Fixed: copies onto a tape could crawl at about a quarter of the drive's speed.** Every open Kestrel window asked
  the tape for its size and its cartridge's health about every 8 seconds, also while it was writing; LTFS answers
  those with commands to the drive (on an HP drive, a rejected one followed by eleven drive-dump attempts), which
  knocked it out of streaming. Now a tape is asked nothing while a job of any Kestrel window uses it, and otherwise its
  size every 30 seconds and its health every 10 minutes.
- **The tape buffer only takes what the system can spare.** The 16 GB limit is used where it's safe; otherwise the
  buffer takes half the memory available at most (and still leaves the share kept free), checked when a copy starts
  and every second while it runs. Preferences → Files → Tapes shows the size it would get now, and says when the limit
  is more than the system can spare. The limit is now in GB as Kestrel shows sizes (16 GB, not 17.2).
- **A tape copy's log shows its speed every 5 seconds**: the time, how fast the tape took the data (or that it's
  waiting for the buffer), the source's speed, the buffer and how far the copy is, in the Jobs window as it runs and
  in the saved log. A drive that slows down for a while shows when and for how long, to set beside the drive's own
  logs.
- **Drives are no longer polled.** Every window asked every drive in the sidebar for its size every few seconds, to show
  a total that never changes. Now Kestrel hears from the kernel when a drive is mounted or unmounted (nothing checked
  on a timer), reads a disk's size from the system (as `lsblk` does, without asking the drive) and anything else's
  once, and asks for the free space in the status bar when you open a folder or a job ends. An idle Kestrel also
  uses less CPU.

## 0.3.2-alpha.5 (2026-10-21)

- **The tape buffer takes at most 16 GB by default** (it took as much as the memory allowed, less 10% of it):
  Preferences → Files → Tapes → Largest buffer changes it; 0 lets it use as much as the memory allows, as before.

## 0.3.2-alpha.4 (2026-10-21)

- Version bump only; see the [changes to the tests](tests/CHANGELOG.md#032-alpha4).

## 0.3.2-alpha.3 (2026-10-21)

- **A Mounts section in the sidebar** lists the mounts GNOME Files shows and Kestrel's sidebar left out: ZFS datasets,
  FUSE mounts (sshfs, rclone, gocryptfs…) and other mounts in your home folder, and network shares (SMB, SFTP, FTP…)
  connected through the desktop, which until now only showed on the Overview. Hover over one to see what it is;
  right-click to unmount it. Recently used servers that aren't connected show greyed: click one to connect, or
  right-click → Remove from Recent Servers. It moves and collapses like the other sections, and Preferences → Sidebar
  lists it too.
- **Other apps' Save windows give free names, as browsers do with downloads.** When the folder already has a file of
  the suggested name, the name becomes the next free one (photo.jpg → photo (1).jpg, then (2)…), in every folder you
  go to, and the original comes back where it's free. A name you type or a file you pick stays yours (with the
  "Replace?" question as before). Saving several files at once gives each a free name instead of asking to replace.
- **Save locations for other apps' Save windows:** Preferences → Windows → "A Save window starts in" can open them on
  an Overview of their own, which lists first the folders you keep for saving (Downloads, a project folder…), separate
  from Starred and bookmarks. Add Folder… on that page or in Preferences; right-click one to move or remove it. The
  default stays as before (the folder the app suggests), an app saving over an existing file always opens in its
  folder, and Open windows aren't affected. BleachBit's cleaner (`kes-setup --bleachbit`) gets a Save locations option.

- **Copies onto an LTFS tape write each new file straight to its name**, without the hidden part file renamed into
  place afterwards. On a tape that part file protected nothing (LTFS never gives back what a failed write used,
  whatever the file is called), and the tape can't claim a name safely the way other drives do. A file already there
  is never replaced, and a cancel or failed write deletes the half-written file. Replacing a file on a tape still keeps
  the old one until the new one is whole, and floppies are unchanged. Also with the admin session.

- **Collapse a date group from anywhere in it.** Grouped by Period or by Day, a long group no longer needs scrolling
  back to its header: each open group ends with a "Collapse Today (1,240)" line, Ctrl+Left on any of its items
  collapses it (Ctrl+Right expands it), and an item's right-click menu has Collapse This Group. The view stays on the
  group's header. Collapse All Groups and Expand All Groups are in the Sort menu and on empty space's right-click menu.

- **Search filters:** the search bar's Filters button finds by type (images, videos, audio, documents, archives,
  folders), size and date (today, this week, this month, this year, or between two days; modified or created), with
  or without a name, here or in subfolders. They're kept for the session in every tab and window.
- **Searching inside files works everywhere now.** File contents used only the desktop's search index, which covers only
  the folders it indexes. Now Automatic uses the index where it covers the folder and otherwise reads the files with
  ripgrep (or grep), so external drives, network shares and skipped folders are searched too, as are folders the
  index hasn't got to. Reading the files offers regular expressions and Match case, skips binary files, shows the
  matching line and its number, and stops with ✕. Opening a result starts the text editor at that line where the
  editor allows it (gedit, VS Code, Kate…). `kes-setup --install-optional` now offers ripgrep.

- **Searching shows its progress, and "Nothing found" in the window.** A thin progress bar runs under the search bar
  while a search is going ("Searching…" in the view until something turns up). A search that finds nothing says
  "Nothing found" in the view, with the reason when there is one (a regular expression that doesn't parse), instead of
  only in the status bar; filtering the folder by name no longer says "Folder is empty".
- **Fixed:** the view's own messages ("Folder is empty", search messages) could be hidden behind the list view.

## 0.3.2-alpha.2 (2026-10-19)

- **Faster again with many files** (slower since 0.3.1-alpha.11, when tapes and floppies came in, which made Kestrel
  ask for every file whether it was on a tape):
  - Deleting, emptying the trash or acting on thousands of items at once no longer freezes the window before the job
    starts: about 0.7 s for every 10,000 items on an SSD, now a few hundredths of a second.
  - Copying and moving many small files is a third faster (20,000 files: 1.54 s → 1.04 s), and emptying a trash of
    10,000 files more than twice as fast (1.08 s → 0.44 s). With no tape or floppy mounted, nothing is looked up.

## 0.3.2-alpha.1 (2026-10-19)

- The benchmark (`bench/`) works again: its Empty Trash step waited for a modal question, which Kestrel no longer
  shows since its windows became free. [Performance](docs/performance.md) has new figures against GNOME Files.

## 0.3.2-alpha.0 (2026-10-18)

- Version bump only.

## 0.3.1-alpha.16 (2026-10-18)

- **Fixed: double-clicking a checksum file on a tape still froze Kestrel** while the drive sought to it (most
  noticeably right after writing to the tape, when the drive has to wind back from the index). Adding the file to
  Recent asked GLib for its type, which reads the start of the file, on the window's thread. On a tape or floppy the
  type now comes from the file's name, for Recent, Open With in the right-click menu (which froze the same way) and
  opening a file with its default app.

## 0.3.1-alpha.15 (2026-10-18)

- **Window diagnostics** (developer tools: Preferences → Show developer tools, then ☰ → Tower… → Windows): each of
  this Kestrel's windows as Qt sees it (its size and position, the minimum and maximum it asks the desktop for, its
  state and screen, the border's margins, children wanting more room than it has) and its recent size, position and
  state changes, with Copy and Save to Home Folder. For a window that misbehaves (one that can't be resized, as
  reported): open a new one with Ctrl+N and compare the two.

## 0.3.1-alpha.14 (2026-10-18)

- **Fixed:** opening a checksum file on a tape still froze Kestrel while the drive sought to it (alpha.11 moved the
  reading itself to the background, but not everything around it). On a real LTFS mount, anything asked of the tape
  waits while the drive seeks, and the window still asked it things: the status bar's size of the selection, the info
  panel, sorting the opened items into files and folders, Recent, and the Verify Checksums window looking up each
  file's size. They now use what the file view already knows, or ask in the background.

## 0.3.1-alpha.13 (2026-10-18)

- **A new Preferences window with every setting.** ☰ → Preferences now has tabs (General, Views, Sidebar, Images &
  Videos, Files, Archives, Windows, Menus) and shows every setting Kestrel keeps, also the ones only reachable where
  they're used until now: the view, icon sizes, hidden files, the panels, the default sort and grouping, the search
  bar's options, the sidebar's sections, checksum and job queue defaults, each archive format's options and Extract
  To…'s choices, and the file chooser's own view. Each says where else it's changed. Changes apply at once to every
  open window (and the other Kestrels), there's no OK to press, and a change made in a window shows there as it
  happens. A search box filters every tab; Reset This Tab and Reset All go back to the defaults. "Open the image
  viewer full screen", which no window could set before, is there too.
- **Customise the right-click menus.** Preferences → Menus: for a folder, a file, an archive, several items and the
  empty space, hide entries, reorder them, move them into or out of submenus, and add separators and submenus of your
  own. Entries still appear only when they apply, wherever you put them; Reset to Default puts a menu back.
- **No more shoe-shining on tapes.** Copies onto an LTFS tape go through a buffer in memory: a thread reads ahead
  from the source as fast as it can, writing to the tape starts once the buffer is full, and the drive then keeps
  streaming instead of stopping and rewinding every time the source slows down. The buffer uses the memory available
  less 10% of the total kept free (Preferences → Files → Tapes), gives memory back when other programs need it, and
  1 TB tarballs stream through it. Tell Kestrel your drive's generation there, and a source fast enough for the drive
  to slow down and match is written at its pace; a slower one is written in full-speed bursts. The status bar shows
  the buffer and both speeds; the job's log says how it went.
- Copying onto a tape checks the file fits first (LTFS never gives back space a failed write used), doesn't force
  LTFS to write its index mid-copy, and nothing in the windows reads files from the tape while it writes (Properties
  named a file's type by reading it). LTFS's timed index writes, which stop the drive every 5 minutes by default, are
  pointed out on the tape's Overview card and in the copy's log, with the mount option that avoids them.
- Kestrels of this version don't share settings changes with running Kestrels of earlier versions (the message
  between them changed); close the old windows after updating.

## 0.3.1-alpha.12 (2026-10-17)

- **One job queue for every Kestrel window:** copies, moves, extractions, compressions, checksum jobs and the like
  no longer all start at once and fight over the same drive. Each waits for its turn on the drives it uses, across
  every Kestrel window and process: jobs on different drives still run side by side, but two jobs on the same drive run
  one after the other, in order. A rename within one drive doesn't wait. Prefer one job at a time? Choose it in the
  Jobs window.
- **The Jobs window:** click the job progress in the status bar (or ☰ → Jobs…, Ctrl+Shift+J) to see every job of
  every Kestrel window: running, waiting and finished, with progress and the time left. Select one to see its steps
  (each item of a copy, done / in progress / waiting) and its log, including the live output of the archive tools, with
  passwords masked; copy it or save it. Pause, resume, stop, start a waiting job now, or move it up or down the queue;
  pause or resume everything at once, or start new jobs paused to line up work first. Delete stops the selected job.
- **Pausing jobs:** a paused copy stops within a few megabytes, its half-copied file kept aside until it goes on; a
  paused extraction or compression pauses the program doing it. (Administrator jobs can't be paused.)
- **The Tower window** for the curious (Preferences → Show developer tools, then ☰ → Tower…, or `kes --tower-monitor`):
  what the background helper that keeps Kestrel windows in sync is doing, live: its status, the Kestrels checked in,
  every message it passes on, the job queue and the drives in use, and the shared undo list, with a Restart Tower
  button.
- **Extract several archives at once:** select more than one archive and right-click → **Extract All Here** or
  **Extract All To…**. Each is extracted as Extract Here / Extract To… would on its own, one after another as one job
  with one ✕, and one summary at the end says what was extracted and what wasn't, instead of a message per archive. A
  split set selected whole counts as one archive (it now also gets Extract Here / Extract To…, which only a single
  part got before). The first password you type is tried on every encrypted archive after it; you're asked again only
  for one it doesn't open.
- **Remember archive passwords:** a **Remember this password** box in the password window (off by default) saves it
  in your desktop's login keyring (GNOME Keyring, KWallet), as "Kestrel Explorer: archive password for …" in Passwords
  and Keys. The next time, Kestrel finds it by itself, even if the archive was renamed or moved, and also tries your
  other saved passwords (a quick test each) before asking. **Preferences → Forget Saved Archive Passwords…** deletes
  them. Without a keyring nothing changes. Kestrel now needs libsecret (`libsecret-1-0`, on every desktop already).
- **Open as Archive:** right-click an archive to see what's inside it in a window of its own (folders, files, sizes,
  times, which ones are encrypted, a filter) and extract just the files or folders you pick, each put right in the
  folder you choose (a taken name gets a number; nothing is replaced). Works for 7z, zip, rar, zpaq, tar and what
  7-Zip opens (iso, deb…).
- **Test Archive:** right-click one or several archives → **Test Archive(s)** checks every file in them against its
  checksum without extracting anything, and says which files are damaged.
- **Preview builds are jobs:** each Generate Previews / Generate Folder Previews is now a job like a copy: it waits
  for its turn on the drive, shows in the Jobs window, and can be paused or stopped. Several can be started at once
  (before, a second one was refused until the first finished).
- **Regenerate previews:** right-click a folder → **Previews** → **Regenerate Previews**, **Regenerate Folder
  Previews**, and their **Recursively** forms make previews again even where they were already made, for ones that
  look wrong or out of date. The generate options moved into the same **Previews** submenu.
- **Fixed:** with Group by Period or Group by Day on, files dragged onto a folder in the same window did nothing
  (they should move into it). The grouped view's drag carried no files.
- Extract To… no longer insists on a password for an encrypted archive: leave it empty to use a saved one or be asked.
- Kestrels of this version don't share jobs, stars or undo with running Kestrels of earlier versions (the messages
  between them changed); close the old windows after updating.

## 0.3.1-alpha.11 (2026-10-16)

**Tapes (LTFS) and floppy disks:**
- **No more freezing.** Kestrel froze while a tape or a USB floppy drive was busy, even when the copy was running in a
  terminal and no Kestrel window was on the drive: it asked every drive for its size every few seconds, and a busy
  tape answers only when it's free. Drive sizes are now read in the background everywhere (sidebar, Overview, status
  bar, Properties), and opening a folder or a checksum file on a slow drive no longer waits for it either.
- **Copying to an LTFS tape works.** It failed with "no permission", also as administrator: a tape can't do the
  special rename and hard links Kestrel used to put a finished copy under its name safely. Kestrel now falls back to
  checking the name is free and renaming on such drives (also for renames, archives and the admin session).
- **Tapes and floppies are recognised** and shown with their own icons; a tape appears on the Overview (it didn't
  before) with its volume name and details from LTFS.
- **No previews from them unless you ask:** thumbnails, folder previews, image details, GIF/WebM playback and dates
  taken would make a tape wind or a floppy read for ages, so they're off there. Right-click the drive → **Show
  Previews on This Drive** to allow them (they're then read in the background).
- **One job at a time per tape or floppy:** a second copy waits ("Waiting for the tape…") instead of competing.
- **Tape-friendly copies:** files are read off a tape in the order they lie on it, so the drive streams instead of
  winding back and forth, and "Positioning the tape" shows while it winds. After a copy to a tape, its index is
  written (or right-click it → **Write Tape Index Now**); copying more than 1,000 small files to a tape asks first.
- Unmounting a drive no longer freezes the window (for a tape it writes the index first), and it asks first while a
  job is still using the drive.

## 0.3.1-alpha.10 (2026-10-16)

Bug fixes:
- **The view no longer jumps after dropping files into a subfolder.** Moving items into a folder in the same view
  scrolled the list to where the moved items had been (often far from where you were). It now stays put.
- **No more folder picture sliding across the view when going back.** Going back with the mouse's back button while
  the mouse moved a little started an accidental drag of the folder you had just left, and its picture slid back into
  place. Only the left mouse button starts a drag now.
- **No more gaps in the grid.** After zooming, resizing or changes in a folder of more than 400 items, the grid could
  leave one empty space at the end of a row; and at some window widths every row ended with an empty space. Both are
  fixed: the grid is always packed, with as many columns as fit.
- **The file chooser's Save button no longer stays greyed out.** After passing through the Overview, Starred or
  Recent while saving a download, Save could stay disabled in the next folder until the chooser was reopened. It now
  follows the folder shown.

## 0.3.1-alpha.9 (2026-10-15)

- **No builds for documentation-only changes:** a push or pull request that only changes Markdown files (the README,
  the docs, the changelogs, the to-do list) no longer runs the build, the tests or the package jobs, and doesn't
  publish a new development build. Any other change runs everything as before. (The build can still be started by
  hand from GitHub's Actions tab.)
- The README links to the new [to-do list](TODO.md) of planned additions, fixes and changes.

## 0.3.1-alpha.8 (2026-10-13)

- **Copied folders fill in as they go:** copying a folder to a new place (photos from a camera's SD card to a hard
  drive, for example) now makes the folder under its real name straight away, and each file appears in it as soon as
  it has been copied. Before, the whole folder was built under a hidden `.kes-….part` name and only appeared when
  everything was done. Each file is still written under a hidden name and renamed when complete, so no half-written
  file ever shows. Cancelling now keeps the files copied so far instead of removing them, and Ctrl+Z puts that
  partial copy in the trash. Moving a folder to another drive works the same way (the originals are deleted once
  everything is copied). Replacing an existing folder still keeps the old one intact until the new one is complete.

## 0.3.1-alpha.7 (2026-10-13)

- **The admin session's purpose is spelled out:** the [Admin session](docs/admin.md) page now says it's for everyday
  file operations that happen to need admin rights, not a replacement for the terminal, and explains why batch rename,
  extracting archives into system folders, editing metadata, running programs as root and changing owners are left
  out on purpose rather than "not yet". No change to what it does.
- **Documentation brought up to date:** the README and Features list the per-folder sort and grouping by date; the
  build instructions include `libxcb1-dev`, which building needs; Dependencies says what `python3` and
  `qt6-gtk-platformtheme` are used for since the dark-style fix; Archives describes Keep damaged files; the Install
  guide's BleachBit section counts its five options; Development and the test README have the current source files,
  test counts and run time; and the review Roadmap is marked complete.
- The apt package now suggests every optional package Kestrel can use (`rar`, the multi-core `.bz2` and `.lz` tools,
  `lz4` and `zfsutils-linux` as well as exiftool, ffmpeg and BleachBit), as `kes-setup --install-optional` and the
  Dependencies page list them.

## 0.3.1-alpha.6 (2026-10-13)

- **Each folder keeps its own sort:** changing the sort (a column header or the Sort menu) now changes it for that
  folder only, and the folder keeps it. Before, it changed the sort of every folder. Folders you haven't sorted use the
  default, which is the sort you last chose before this version; **Sort → Set as Default** makes the current sort the
  default. Other tabs and windows showing the folder follow the change.
- **Sort → Apply to Subfolders:** gives every folder below the current one (all levels) the same sort, replacing any
  they had. It runs in the status bar and can be cancelled, which leaves every folder as it was.
- **Sort → Group by Period / Group by Day:** shows the folder's items under date headers. By period: Today,
  Yesterday, Earlier this week, Last week, Earlier this month, Last month, Earlier this year, then one per year. By day:
  one header per date ("Tuesday, 13 October 2026"). Each header shows how many items it has; click it (or press Enter
  on it) to collapse or expand the group. The groups stay in date order, newest first, whatever you sort by; inside
  each group the items follow the chosen sort (name, size, type or date). Sorting by Modified, oldest first, puts the
  oldest group first.
- **Sort → Date to Group By:** Date Modified, Date Created, or Date Taken (Metadata): when a photo or video was taken,
  from its EXIF (JPEG, PNG, WebP, TIFF, DNG), a PNG's Creation Time, or an MP4 / MOV / 3GP video's header. Those dates
  are read in the background, so photos move to their groups a moment after the folder opens; files without one go by
  their date created (and that by the date modified where the drive keeps no creation dates).
- Grouping works in the grid and the list, in search results, Starred, Recent and Trash, and is kept per folder like
  the sort (Set as Default and Apply to Subfolders include it).
- The right-click Sort By menu now shows the chosen sort, like the toolbar's Sort menu.
- BleachBit's Kestrel cleaner (`kes-setup --bleachbit`) can now delete the folders' sorts too, under "Folder covers,
  colours and sorts". Run `kes-setup --bleachbit` again to update it.
- Kestrels of this version don't share stars, tasks, undo or hand-offs with running Kestrels of earlier versions (the
  messages between them changed); close the old windows after updating.

## 0.3.1-alpha.5 (2026-10-12)

- **Follows the desktop's dark style:** with Dark chosen in GNOME's Settings (or any desktop's dark colour scheme),
  Kestrel now opens with a dark background and light text, and switches when the style changes. Before, it stayed
  light unless the GTK theme itself was a dark one (such as Yaru-dark): with Adwaita or Yaru plus the dark style, or
  on Ubuntu 26.04 without the qt6-gtk-platformtheme package, it was white. It uses the theme's own dark colours,
  or Adwaita's dark ones when the theme has none.
- **More ways to pick a folder preview's pictures:** Preferences → Folder preview picks now also offers Last by
  name, Oldest first, and Beginning, middle and end (pictures evenly spaced through the folder by name, so a preview
  shows the whole set rather than its first few). Folders of galleries pick their subfolders the same way.
- **Every window moves freely:** Properties, Preferences, Open With, Rename, Batch Rename, Compress and Extract,
  Create Checksum File, Network Sharing, Edit Bookmark, password prompts, the file-conflict question during a copy,
  confirmations and error messages are now windows of their own. Before, most were modal: they blocked Kestrel, and on
  GNOME dragging one dragged the whole Kestrel window along. Now none blocks Kestrel or stays on top of it, so you can
  move one aside and leave it open until you need it, and several can be open at once (several Properties windows, for
  example). They still close with the Kestrel window they belong to. Choosing a folder or file (Move To, Copy To,
  Create Links In, Browse… buttons) now opens Kestrel's own chooser window instead of the toolkit's. There is no way any unintended consequences could come of this.

## 0.3.1-alpha.4 (2026-10-12)

- **Keep damaged files when extracting:** Extract To… has a new "Keep damaged files" option for rar, 7z and zip
  archives. Files that fail their checksum are kept as far as they could be extracted, so the extraction no longer
  fails, and a list of them is shown afterwards. Before, a single damaged file failed the whole extraction, and unrar
  deleted the file. When an extraction fails because of damaged files, the error message now points to this option.
- **Generate Folder Previews** and **Generate Folder Previews Recursively** (right-click a folder, or empty space):
  they build only folder previews, for the folder and the folders directly in it or for every folder below it. Unlike
  Generate Previews Recursively, they don't make a thumbnail for every file.
- **Preview builds scan quickly:** the Generate Previews entries now list the whole folder tree at full speed while
  the previews are being made, so the progress bar shows the real total early. Before, the scan could only move as
  fast as the previews were made, so "scanning…" lasted almost the whole build.
  The folder-preview builds only look at folders while scanning, like `tree -d`: each folder's files are read when
  its preview is made, as when you open it.
- **Documentation:** the README is now a short overview. The details moved to docs/ (features, install, archives,
  admin sessions, dependencies, development, performance), and the version history to this CHANGELOG.

## 0.3.1-alpha.3 (2026-10-12)

- **Tab right-click menu:** Duplicate Tab (keeps the folder and back/forward history, opens next to the original),
  Open in New Window (moves the tab), and Close Tab.
- **Folders without images** are drawn in the folder colour, like the rest, instead of the theme's plain icon.
- **Special folders** (Downloads, Documents, Music, Pictures, Videos, Templates, Public, Desktop, Home, Trash) without
  images show their symbol on the coloured folder. The symbol is tinted so it's readable on any colour, white and
  black included.
- **Overview bookmarks that aren't available** are now coloured folders too: faded with a drive symbol for an
  unplugged drive, faded with a question mark for a deleted folder, and a network symbol for network locations.

## 0.3.1-alpha.2 (2026-10-09 to 10-11)

- **Pinned items:** Right-click → Pin marks a file or folder in its own folder with a red pin, without adding it to
  Starred. **Sort → Pinned & Starred First** puts them at the top. Pins are synced between Kestrels and saved in
  `pinned.json`, which BleachBit's cleaner can now remove.
- **Better drag and drop visuals:**
  - On Wayland, Kestrel draws the dragged items' picture itself, because GNOME doesn't show Qt's.
  - Items dropped into a folder are shown shrinking into it, and a cancelled drag shows them going back.
  - Dropping onto empty space or a file in the same view is refused, so items no longer jump out of the grid.
- **Separate project from here on.** The Python/PyQt6 Kestrel Explorer and this one were the same up to this version
  and are developed independently since. Where they already differed at the split:
  - Image decoding is Qt only. The Python version fell back to Pillow for images Qt couldn't read; here a file Qt
    can't decode (even with `kimageformat6-plugins`) gets no thumbnail.
  - The EXIF summary and the image-generation prompts in the info panel come from a built-in reader instead of
    Pillow: EXIF from JPEG, PNG (`eXIf`), WebP and TIFF-based RAW files, and text chunks from PNG. HEIC/AVIF EXIF
    appears only in the Metadata tab (through exiftool).
  - The admin helper is a compiled program (`kes-admin-helper`) instead of a Python script, so the password prompt
    names that file rather than `/usr/bin/python3`.

## 0.3.1-alpha.1 (2026-10-09)

- **Copies don't replace files another program creates mid-job.** A copy, move or extract to a name that was free
  won't replace a file another program saves there while the job runs. That file is kept and Kestrel tells you. Only
  **Replace** replaces.
- Test for leftover `.kes-….part` files, and CI switched to a container image mirror (Docker Hub rate limits).

## 0.3.1-alpha.0 (2026-10-09)

- Test suite updates.

## 0.3.0-alpha.10 (2026-10-09)

- The sync protocol between Kestrels (the "tower") is versioned. A Kestrel speaking a different protocol is ignored
  rather than misread.
- Documentation and benchmark write-up updates. Tests added for undo between Kestrels, opening folders as tabs,
  menus/shortcuts and parsing.

## 0.3.0-alpha.9 (2026-10-09)

- Right-click on a selection mixing trashed and live files gives the normal menu. Only an all-trash selection offers
  Restore / Delete Permanently, so a live file is never offered for permanent deletion.
- A copy that replaced a file says it can't be undone, so Ctrl+Z no longer undoes the action before it.
- After a tar/unzip extraction, symlinks pointing outside the folder are removed even in folders the archive left
  read-only, and every removed link is reported.

## 0.3.0-alpha.8 (2026-10-09)

- **Delete on mixed selections:** in search, Recent and Starred views, trashed items are deleted permanently (after
  asking) and the rest go to the trash.
- **Admin helper ownership:** a move keeps each item's owner, and anything new gets the owner of the folder it's made
  in (yours in your home, root's in /etc).
- **Escaping links from GNU tar / UnZip** (ones leading outside the destination) are removed after extraction,
  including after a failure or cancel. Your own existing links are left alone.
- **Batch rename** works in two phases, so name swaps and overlaps work. On failure, everything goes back to its old
  name.

## 0.3.0-alpha.7 (2026-10-09)

- **Accent colour:** follows GNOME 47+'s accent colour setting, using libadwaita's exact colours, ahead of the theme's
  selection colour.
- Colours update everywhere when the theme or accent changes while Kestrel is open, including stylesheets that
  previously kept old colours.

## 0.3.0-alpha.6 (2026-10-09)

- **Failure-safe replacing:** anything copied or moved over an existing file, link or folder is built under a hidden
  `.kes-….part` name and swapped in only when complete. A full disk, a cancel, a crash or a logout never leaves you
  with half a file or the old one gone. Folders are swapped atomically (RENAME_EXCHANGE, with a fallback) and the old
  one is deleted afterwards.

## 0.3.0-alpha.5 (2026-10-09)

- Version bump only.

## 0.3.0-alpha.4 (2026-10-08)

- **Create Checksum File…** (right-click files or folders): CRC32, MD5, SHA1, SHA256, SHA512, BLAKE2b. Writes one
  file per algorithm (`md5sum` style, `.sfv` for CRC32) or a single BSD-tag `CHECKSUM` file.
- **Safer writes:**
  - Files are written beside the destination and renamed over it, so a symlink is never written through and hard
    links keep their old contents.
  - `.desktop` launchers escape every value, so a file name can't inject keys or commands.

## 0.3.0-alpha.3 (2026-10-08)

- **Single-file archives** (`.gz`, `.xz`, `.zst`…) extract to a new file that's renamed into place. They no longer
  follow a symlink at the target or truncate a hard-linked file, and a failed run leaves the old file intact.
- **Admin helper move fix:** moving something onto itself (same path, a hard link to it, or a case variant on a
  case-insensitive drive) no longer deletes the source.

## 0.3.0-alpha.2 (2026-10-08)

- Version bump only.

## 0.3.0-alpha.1 (2026-10-08)

- **Checksum verification:**
  - Double-clicking `.sfv`, `.md5`, `.sha256`, `SHA256SUMS`, `CHECKSUM` and similar files opens **Verify
    Checksums**, which checks every listed file with progress, Stop and Verify Again.
  - Supported: md5sum style, BSD tags (also PGP-signed), SFV, and hash-only files. Algorithms: CRC32 through SHA512
    and BLAKE2b.
  - Properties → Checksums: **Browse…** checks a file against a checksum file.

## 0.3.0-alpha.0 (2026-10-06)

- `kes-setup --install-optional` installs the optional packages (exiftool, ffmpeg, BleachBit, rar…).
- Screenshots added to the README.

## 0.2.1-alpha.3 (2026-10-05 to 10-06): security and robustness

- **Admin helper hardened:**
  - Paths are walked folder by folder without following symlinks. Only root-owned system links like `/lib` are
    followed.
  - Every operation works on open folders (`*at` calls), and recursive delete/copy go by file descriptor, so a
    swapped symlink can't redirect root.
  - One policy table covers every operation.
  - Protected paths: `/`, top-level folders and home folders.
  - It won't hard-link files you don't own, drops setuid from copies, and never deletes a mount point.
- **Normal file operations** never follow symlinks inside folders they work through, and a delete stops at other
  drives mounted inside.
- **Archive passwords** go to 7-Zip/rar/unrar on standard input and to zip/unzip in an environment variable. zpaq
  still needs the command line, and the dialog warns you. Malicious archive links (`../`, absolute paths) are
  contained.
- **Process handling:** timeouts are guaranteed, child processes are killed when Kestrel is done with them, and
  Kestrel doesn't depend on ignoring SIGPIPE.
- **Tower messages** are validated: protocol version, known types, field types, absolute paths and size.
- **Code structure:** `app.cpp` split into `actions.cpp`, `opening.cpp` and `incoming.cpp`; global state documented.
- **EXIF/PNG metadata** uses a built-in reader, and exiftool's JSON is parsed safely.
- **Accessibility:** every control has a screen-reader name.
- **Performance counters:** `KESTREL_STATS=1 kes` prints timings on quit.
- **CI:** runs the tests, sanitizers (ASAN/UBSAN), and package installs on clean Ubuntu 24.04 and 26.04.
- **App icon** added: installed in the icon theme, with a copy built into `kes`.
- **BleachBit:**
  - **Shred with BleachBit…** and **Empty Trash with BleachBit…** in the menus.
  - `kes-setup --bleachbit` adds a cleaner for Kestrel's cache, history, starred items and folder covers/colours.
- Grid layout adjustments, and the folder under a drag is highlighted.

## 0.2.1-alpha.2 (2026-10-05)

- Ubuntu 24.04 tested and supported (apt too).
- README: the mission statement and architecture diagrams.
- CI test jobs.

## 0.2.1-alpha1 / 0.2.1-alpha.1 (2026-10-05)

- Version naming fixed to `alpha.N`, so apt sorts versions correctly.

## 0.2.1-alpha (2026-10-05)

- **Drop focus:** dragging files from Kestrel into another app gives that app the focus. On GNOME Wayland this goes
  through a small bundled Shell extension.
- **File chooser windows** keep their own view settings (zoom, view, sort, panels) separate from the main windows,
  with a smaller default size.

## 0.2.0-alpha (2026-10-04 to 10-05)

- **Linux Mint 22 (Cinnamon) support:**
  - Reads GTK theme colours on Qt 6.4.
  - Uses Cinnamon's own settings for the wallpaper, history and icons.
  - `--default` remembers Nemo and puts it back.
- **apt repository** with stable and dev channels, signed packages, and .debs for Ubuntu 24.04 and 26.04.
- **`kes-setup`:** the "make Kestrel the file manager" part as its own command (`--default`, `--dock`, `--undo`).
- **Phones and cameras** (iPhone, Android, PTP) on the Overview and in the sidebar:
  - Thumbnails come from the phone's own previews.
  - Hints for connecting (Trust, File transfer, PTP).
  - iPhone videos are copied once so they play smoothly.
- Benchmark against Nemo.

## 0.1.3-alpha (2026-10-03 to 10-04)

- **File chooser:** other apps' Open/Save dialogs (browsers, Flatpak, Snap, GTK 4/Qt portal apps) open as a Kestrel
  window.
- **Sidebar:** shows connected phones, entries can be dragged to reorder, sections moved and collapsed.
- **Theme support:** light/dark, accent, fonts and icons, including live switching.
- **Archives:** double-clicking opens Kestrel's Extract dialog. Packages and disk images still open with the system's
  app.
- `install.sh --default` puts `~/.local/bin` on PATH.
- Fixes for the path bar (popups stealing focus while typing) and for GNOME's busy cursor spinning at launch.
- Benchmark against GNOME Files, and README rewrite.

## 0.1.2-alpha (2026-10-03): first release

- **Core:**
  - Tabs, back/forward history, path bar, sidebar with places/bookmarks/drives.
  - Grid and list views, hidden files, search (wildcards, subfolders).
  - Background file operations with progress and Cancel.
- **Galleries:**
  - Folder icons with 1–4 image mosaics (video stills with ▶).
  - Folder covers, per-folder colours, a per-folder previews toggle, recursive preview generation.
  - Zoom from 48 to 320 px.
  - GIF/WebM playback in the view.
  - Built-in image viewer, EXIF and AI-prompt info panel.
- **Overview page:** drives with usage bars, mount/unlock/eject, ZFS pools, Connect to Server, bookmark cards.
- **Terminal jobs in the window:**
  - Archives (7z, zip, rar, zpaq, tar formats) with every option.
  - Admin session (Retry as Administrator).
  - Permissions, links, metadata editing with exiftool, batch rename.
- **Undo (Ctrl+Z)** for moves, renames, trash and new items.
- **Starred and Recent.**
- **Search inside files**, using the desktop's search index.
- **Thumbnails for other file types** from the system's thumbnailers.
- **Integrations:** Open in VS Code and other editors, Nautilus scripts, Samba sharing.
- **Default file manager:** `install.sh --default` makes Kestrel open folders and the Trash, and answer browsers'
  "Show in folder".
- **Several Kestrels in sync** through the tower, with optional shared undo and opening folders as tabs in an open
  window.
- Anaconda's folders are moved to the end of PATH at startup, so Kestrel uses the system's programs.
- First test suite.
