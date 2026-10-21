# Changelog

What changed in each version of Kestrel Explorer, newest first.

## 0.3.2-alpha.3

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
