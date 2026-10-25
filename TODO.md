# To do

Things to add, fix or change. Newest at the bottom; tick an item off (`[x]`) when it's done, with the version it went
into.

## Bugs

- [ ] **Resizing a window by its edge can send it flying off the screen.** In one long-running Kestrel window,
  pressing and dragging any edge (the resize cursor showing) sent the window off into empty space instead of resizing
  it; Alt+F7 brought it back. Reopening Kestrel fixed it, and it hasn't been reproduced since.
  - That window had done two very large copies, a checksum of a 1 TB zpaq archive, and drags into Brave. It still did
    it after the jobs ended; its console showed nothing relevant.
  - Ruled out: Kestrel's own code (it never moves or resizes windows; on GNOME Wayland Qt's Adwaita border plugin
    asks GNOME to resize on the press), its minimum size (stays small, also in deep folders and with long status
    text), the drop-focus extension, the drag picture, single drags out of the window (to the desktop, Brave, a
    folder), Tiling Assistant, and Qt or GNOME in general (a bare Qt window, Kestrel with
    `QT_WAYLAND_DECORATION=bradient` or `QT_QPA_PLATFORM=xcb`, and fresh Kestrel windows all resize fine).
  - Seen again (2026-10-18), in a window that had been open a long time: Ctrl+N's new window of the same Kestrel
    resizes fine, the old one doesn't. So it's that window's state, not the process's. A suspect: the window having
    been snapped to a screen edge or corner (Tiling Assistant, Super+arrows), which marks edges as tiled; Qt's border
    changes its resize zones and margins for tiled edges. To try on a stuck window: un-snap it (Super+Down, or drag it
    off by the title bar), then maximize and restore it, and see which makes resizing work again.
    Tried on the stuck window: it had been snapped to a screen before, but un-snapping it and maximizing/restoring it
    didn't help; the new window kept resizing fine. Snapping a fresh window (to a side, a quarter, on the portrait
    monitor, then un-snapping) doesn't make it happen either: not snapping.
  - Diagnostic added in 0.3.1-alpha.15: ☰ → Tower… → Windows (with developer tools on) shows each window's size, the
    minimum and maximum it asks for, Qt's idea of its state and screen, the border's margins, children wanting more room
    than it has, and its recent resizes and moves. Next time a window is stuck: Save to Home Folder from that window's
    Tower (and try resizing it first, so its attempts are in the list), compared with a fresh Ctrl+N window.
  - So something during a long session breaks that window's state. Still to find out: does the window follow the
    mouse until the next click, or jump away at once?

- [ ] **A closed window's folder previews keep being made.** Seen: a window showing a folder of a few hundred
  subfolders (folder previews on) was closed (its size and layout were saved then, so the close went through, past
  the "Operations Running" question), yet new mosaics were still written to ~/.cache/kestrel-explorer/folders minutes
  later, its process reading one subfolder after another.
  - Found: the thumbnail and folder-preview queue belongs to the process (`g_thumbs`, src/thumbs.cpp: `pool`,
    `dir_pool`), not to a window, and closing a window cancels nothing (`MainWindow::closeEvent` stops searches and
    closes viewers; `Pane::~Pane` only stops a search). So while the process lives on, it works through everything the
    closed window asked for (hundreds of mosaics: each lists its folder, picks 4 images spread across it and reads them).
  - Why the process lived on wasn't seen (it had gone before it could be looked at). Not an exit waiting for the
    queue: `g_thumbs` is never deleted, so no `~QThreadPool` waits for it, and nothing runs on Qt's global pool (which
    `~QApplication` would wait for). Not the hidden "Operations Running" question either (the layout was saved).
    Most likely another window of the same process (Ctrl+N windows share it) left open, minimised or on another
    workspace; the Jobs and Tower windows close with their main window. Next time: `xdotool search --pid <pid>`
    lists its windows, and `gdb -p <pid> -batch -ex 'thread apply all bt'` shows what it's doing.
  - The other way round, too: a pane moving to another folder calls `cancel_pending()`, which empties the whole
    process's queue, other windows' and tabs' requests included (they're asked again only when those views repaint).
  - To change: requests with an owner (the pane that asked): a pane leaving a folder, closing, or its window closing
    cancels its own (queued tasks skipped when no one wants them any more, `QThreadPool::tryTake` where it can), and
    no one else's. Test: two windows of one process, one closed while its previews are queued: nothing more is made
    for it, and the other window's previews still come.

- [ ] **Tape copies: short dips left, and fewer questions to the drive.** Copies onto an LTFS tape used to drop to
  about a quarter of the drive's speed for long stretches; the cause was programs asking the tape for its free space
  (see Done). Still open:
  - A few short dips (about 5 seconds) with no such question from any program; cause unknown.
  - Kestrel could interrupt the drive less still: before every file on a tape, `copy_file_at` (src/fileops.cpp) checks
    the free space (fstatvfs: on LTFS probably a command to the drive); once per job and counting down would do.
  - While LTFS holds the drive's SCSI generic node, its logs can be read through the drive's /dev/bsg node instead
    (sg_logs gets EBUSY on the sg one).

- [ ] **The scroll wheel over the image viewer's info panel goes to the next image.** With the Info Panel (I) open,
  turning the wheel over it changes the image instead of scrolling the panel.
  - Likely cause: `ViewerInfo` (src/viewer.cpp) is a QScrollArea with no wheel handling of its own. Its scroll bar
    ignores a wheel event it can't use (the panel's contents fit, or it's already at that end), so the event goes up
    to `ImageViewer::wheelEvent`, which turns it into `navigate()`. Check whether it also happens mid-scroll.
  - To change: the panel keeps every wheel event over it (scrolling when it can, otherwise nothing), so only the
    wheel over the picture or the filmstrip changes images.

- [ ] **A job queued behind another fails when its destination is moved while it waits, and its log looks like part
  of the other job.** Seen: a copy of two files onto a tape ("Copying with dd") ran; a move on another drive ran;
  then a "Copying" job of the *same two source files* into an unrelated folder failed with
  `[Errno 2] No such file or directory: '<the folder>'` for each file. Investigated (2026-10-25): the jobs didn't mix.
  The third was a job of its own, made while the tape copy ran, most likely **Paste Into Folder** on that folder's
  right-click menu: the clipboard still held the files copied for the tape copy, and Paste Into Folder sits right
  above Move To… (a misclick there is easy, and there's no confirmation). It read from the same drive as the tape copy,
  so the queue held it ("Waiting for its turn"). Meanwhile the folder was moved into another folder (its parent's
  time matches), so when its turn came the destination wasn't there. Kestrel does what it was told. What's wrong:
  - **Nothing says what a job is or where it came from.** Both were titled "Copying", and the logs have no header.
    The log should start with what started it (Paste Into Folder / drop / Copy To… / Move To…, which window),
    when, from where to where, and how long it waited and for which job ("Waited 2 min for: Copying with dd onto
    the tape"). The title should name the destination ("Copying 2 items to <folder>").
  - **A destination that's gone when the job starts gives one error per file.** Check it when the turn comes and fail
    once, clearly: "The folder it was copying into was moved or deleted while this job waited." Option: hold the
    destination open (`O_PATH` fd) when the job is queued and copy with `openat`, so a job follows its folder when
    it's moved (it ends up where the user put the folder), and fails clearly only when the folder is deleted.
    Choose: follow the folder, or stop?
  - **Paste Into Folder is easy to hit by mistake, and the paste may be stale.** Ideas: move it below Move To… /
    Copy To… (or only in the empty space's menu), show what it would paste ("Paste 2 Items Into Folder"), and/or
    forget a copy from the clipboard once it has been pasted somewhere.
  - Test: `jobs` / `fileops` (a job queued behind another, its destination moved meanwhile: it follows it or fails
    once with the clear message; the log header names the source, destination and the wait).

- [ ] **A drive or mount sometimes doesn't show up in the sidebar right after it's mounted.** It comes later (on
  some other refresh). Not because Kestrel polls and misses it. Nothing is polled, and it should stay that way. The
  sidebar already listens to what the OS says:
  - The kernel's mount table: `drives::MountWatch` (src/drives.cpp) uses GIO's `GUnixMountMonitor`, which waits on
    `/proc/self/mountinfo`, and the kernel wakes it on every mount and unmount. `Sidebar` (src/widgets.cpp) restarts
    `phone_timer` on that, then `refresh()` 300 ms later reads `drives::mounts()` afresh (`mounts_stale`).
  - GIO's `GVolumeMonitor` (volume/mount added/removed/changed) for phones and network shares (gvfs, which gets them
    from udisks2 over D-Bus, as GNOME Files does).
  - So the event comes, and the gap must be in what happens after it. To find out first: **which kind of mount** was
    late (a USB stick or disk under /media, a network share, a FUSE mount in the home folder, a tape, a phone), and
    whether it was missing entirely or just named wrong.
  - Leads: (1) the refresh runs 300 ms after the *first* event, and a mount can come in steps (udisks2 mounts, then
    the udev symlinks such as /dev/disk/by-label appear, gvfs adds its mount object later still). If the last step
    arrives after the timer has fired and nothing else fires, the sidebar shows an in-between state.
    (2) `drives::mounts()` keeps its list for a second unless `mounts_stale` is set, and that flag is shared by every
    caller (thumbnails, jobs, `drive_ids` on other threads). Check that one caller using up the flag can't leave the
    sidebar with an older copy. (3) `g_unix_mount_monitor_get()` belongs to the GLib main context of the thread that
    first calls it. If `MountWatch::instance()` is ever first made on a worker thread, its signal never comes.
  - If the kernel's table really isn't enough: udisks2's D-Bus objects (`org.freedesktop.UDisks2`, `InterfacesAdded`
    and the `MountPoints` property changing on a `Filesystem`) are the service that already knows each drive. They
    send signals and don't need polling, but they only cover drives udisks2 manages.
  - Test: `MountWatch::mounts_changed()` followed by a mount that only appears in `KESTREL_TEST_MOUNTS` a moment
    later: the sidebar lists it without any other event.

## New features

### Jobs

- [ ] **Thumbnailing is a job in the Jobs window.** Making thumbnails and folder previews (mosaics) shows up in the
  Jobs window like any other job, instead of running unseen in `g_thumbs` (src/thumbs.cpp: `pool`, `dir_pool`).
  - One job per pane and folder that asked (e.g. "Thumbnails: Pictures/2024, 312 of 840"), with the window and
    folder it's for, progress, and Pause / Stop like other jobs. Stopping it leaves the thumbnails made so far.
  - Ties in with the bug "A closed window's folder previews keep being made": requests get an owner (the pane), so
    a pane closing or its window closing ends its own jobs, and `cancel_pending()` no longer empties every other
    window's and tab's requests.
  - Decided: leaving a folder doesn't end its job, it lowers its priority: the folder being shown comes first, and the
    jobs of folders left behind run only when nothing newer is waiting (going back to a folder raises its job again).
    A lowered job times out after a few minutes (Preferences, default 3 minutes) and stops, keeping what it made, so
    while browsing deeper the old low-priority jobs fall away on their own. The timer counts from when it was lowered.
  - Decided: they follow the job queue's per-drive rules like any other job (a drive's one-job-at-a-time, physical
    disks, tapes and floppies; previews there still need permission as today).
  - Decided: the Jobs window's list is split in two: normal jobs on top (75% of the height), thumbnailing below (25%),
    so browsing doesn't flood the list of copies, moves and extractions.

- [ ] **Jobs run in the background, apart from the Kestrel windows.** Jobs (copies, moves, extractions, checksums,
  preview builds…) run in a background process of their own, like the tower, instead of inside the window that
  started them. The Jobs window is a view onto that process: it can be opened at any time and closed without
  stopping anything.
  - All Kestrel windows can be closed while jobs keep running.
  - Closing the last Kestrel window while jobs are running asks something like "Exit and open the Jobs window, or
    stop all jobs?"
    - **Exit and open the Jobs window:** the Jobs window opens, the Kestrel windows close, and the jobs go on.
    - **Stop all jobs:** they're stopped (each one leaving things as a cancel does today) and Kestrel exits.
  - If the Jobs window is already open, the last Kestrel window just closes, without asking: the jobs carry on and
    can still be watched there.
  - Today closing a window that runs jobs asks whether to stop them or keep going, and they live in that window's
    process. With this, a job no longer depends on the window that started it.
  - Messages a job needs to show when it ends or needs an answer (a conflict, a password, a summary) would then come
    from the Jobs window, or a window opened for them, since the window that started it may be gone.

### Archives

- [ ] **Better archive window QoL.** Follow-ups to Open as Archive (`src/archivewin.cpp`):
  - Open a file from inside the archive (double-click): extract it to a temporary copy and open that with its app.
    The temporary copies need a cache folder, cleaned up afterwards and added to the BleachBit cleaner.
  - Drag items out of the window into a Kestrel folder, the desktop or another app, extracted as they're dropped.
  - A test for the unzip path (used only when 7-Zip isn't installed), which the test environment never takes because
    7-Zip is always there: listing, extracting single items (names with `[ ] * ?` escaped) and testing.

- [ ] **Split sizes to pick from when compressing.** In Compress…, when the chosen program can split an archive into
  volumes (7z, rar, zip with 7-Zip; the **Split into volumes** box, shown only then), the size becomes a dropdown of
  standard sizes plus **Custom…** with a box for any size. Today it's a number box and an MB / GB unit
  (`src/archive_ui.cpp`).
  - Standard sizes, named by what they fit: 25 MB (email), 100 MB, 650 MB (CD), 700 MB (CD), 1 GB, 2 GB, 4 GB (just under
    4 GiB: a FAT32 USB stick's largest file, 4095 MB), 4.7 GB (DVD), 8.5 GB (dual-layer DVD), 25 GB (Blu-ray),
    50 GB (dual-layer Blu-ray). Exact byte sizes for the discs (a "4.7 GB" DVD holds 4,700,372,992 bytes, not
    4.7 GiB), so a volume really fits; to settle when it's built.
  - **Custom…** shows the number box with a unit (KB, MB, GB) again; the last custom size is remembered, and the
    choice (standard or custom) is remembered per format as now.
  - The Command preview shows the exact size passed to the program (`-v` for 7z and rar).
  - The size is passed around in whole megabytes today (`volume_mb`): exact disc sizes and a KB unit need it in bytes
    (or KB) all the way to the command.

### Drives, the sidebar and the Overview

- [ ] **Friendly names for drives and removable media.** Right-click a drive (Overview card or sidebar entry) →
  **Rename in Kestrel…** to give it a name of your own, such as "Camera SD card" or "Backup HDD". Kestrel only shows
  that name; the drive's own label is never changed.
  - Recognised by a stable id, not by where it's mounted: the file system's UUID (GIO's `g_volume_get_uuid()` /
    `g_mount_get_uuid()`, or the volume's UUID identifier), so the name follows the drive to any port or mount point,
    also before it's mounted. Phones and cameras have no UUID: use the most stable id GIO offers for them (the
    device's serial, else its gvfs address), and say in the dialog when the name may not stick.
  - Shown wherever the drive's name is: Overview cards, the sidebar's Devices, the path bar and tab title at the
    drive's top folder, the window title, Properties. The real label stays visible where it matters (a tooltip, or
    Properties: "Name in Kestrel: Camera SD card · Label: EOS_DIGITAL").
  - Saved in Kestrel's config folder (a list of id → name, written whole like the others), synced to other open
    Kestrels through the tower (a new message, so bump PROTOCOL), and added to BleachBit's cleaner. **Reset Name**
    puts the drive's own name back.
  - Two drives with the same UUID (a cloned disk): the name shows on both; say so in the dialog rather than guessing.

- [ ] **Overview page improvements, chosen in Preferences.** Let the user pick what the Overview shows, in
  Preferences → Overview. Today it always shows the same four sections: Drives, Phones & Cameras, Network, Bookmarks.
  - Show or hide each section, and put them in any order (as the sidebar's sections can be).
  - New sections to choose from (ideas to settle when this is picked up): Starred, Recent files, Pinned folders,
    frequently used folders, and the drives' friendly names (see the item above).
  - Per section: card size, and for Drives whether to show system and hidden drives, the usage bar, free space as
    a number or a percentage, and unmounted drives.
  - Details still to decide with the user: which new sections and options are actually wanted.

- [ ] **Drive health on the Overview.** Each drive card (and Properties of a drive) shows its health from S.M.A.R.T.:
  "Healthy", or a warning such as "3 bad sectors" or "failing", plus its temperature and power-on time.
  - Read through udisks (`org.freedesktop.UDisks2.Drive.Ata`: SmartFailing, SmartTemperature, SmartPowerOnSeconds,
    SmartNumBadSectors), which works without root for ATA/SATA drives; NVMe through udisks' NVMe interface where the
    installed udisks has it. `smartctl` needs root, so it isn't used. USB enclosures often don't pass S.M.A.R.T.
    through: show "health not available" rather than nothing.
  - A failing drive gets a red warning on its card and a notification the first time Kestrel sees it.
  - Details on click: the attributes udisks reports, in plain words (reallocated sectors, pending sectors, CRC errors).


- [ ] **LTFS and slow drives: what's left.** The slow-drives work (0.3.1-alpha.11) covered recognising tapes and
  floppies, never waiting on them in the window, no previews unless allowed, one job at a time, tape order, index sync,
  the Overview card and safer unmounting. Still to do:
  - Check on the real tape that LTFS's virtual extended attributes have the names Kestrel reads (`user.ltfs.startblock`,
    `partition`, `volumeName`, `mediaLoads`, `mediaStorageAlert`, `sync`; plain `ltfs.*` also tried): with the tape
    mounted, `getfattr -d -m -` on the tape's mount point and on a file. Fix the names where they differ.
  - Check with the real USB floppy drive that it's recognised (a removable `sd` disk of at most 4 MB) and that browsing
    it no longer freezes.
  - Mount and format tapes (`ltfs`, `mkltfs`) from Kestrel (needs permissions on the drive device; within the admin
    session's scope), and a catalogue of tapes that aren't loaded (their index saved when last mounted, to search
    what's on which tape).

### Views and search

- [ ] **Drag files and folders from one tab to another.** Dragging items over a tab in the tab bar switches to that
  tab (after a short hover, as browsers and GNOME Files do), so the drag can go on into its folder, a subfolder there,
  or empty space (its folder). Dropping straight onto the tab itself drops into that tab's folder.
  - The same rules as any other drop: with no key held, a move on the same drive and a copy to another drive; Ctrl
    copies, Shift moves, Ctrl+Shift links, Alt asks. The folder or tab under the pointer is highlighted.
  - Today the tab bar takes no drops at all (src/app.cpp: `tabs`, its `tabBar()` has only the tab menu). It also hides
    itself with a single tab, so this is for windows with two or more.
  - Not onto a tab showing the Overview, Starred, Recent or search results (no one folder to drop into): it still
    switches, and the drop goes where the pointer then is. Onto the Trash: Move to Trash.
  - Test: `ui`, as the drops onto folders are tested there (the drag events where a real drag sends them).

### File tools

- [ ] **Find duplicate files.** Right-click a folder (or several, or a drive) → **Find Duplicates…**: lists groups of
  identical files and lets the user choose which copies to keep and move the rest to the trash.
  - Fast: group by size first, then hash only same-size files (Kestrel already hashes for checksums), with byte-by-byte
    comparison or a strong hash to be sure. Runs on a thread with progress and ✕. Optionally `jdupes`/`rmlint` when
    installed, but not needed.
  - Options: minimum size, include hidden files, skip hard links (already the same file), follow into subfolders.
  - Results: each group with thumbnails, path, date and size; "keep the oldest / newest / in this folder" helpers;
    Move to Trash (undoable) or replace duplicates with hard links (same drive only, asking first). Space to be freed
    shown at the top.
  - Later: similar images (not identical bytes), e.g. the same photo at another size.

- [ ] **Compare and sync folders.** Right-click a folder → **Compare With…** and **Sync To…**.
  - **Compare:** two folders side by side, the differences listed: only on the left, only on the right, newer on one
    side, same name but different contents (by size and date, or by contents for a deep compare). Open, copy across
    or trash from the list.
  - **Sync:** make a destination match a source (one way, e.g. Pictures → backup drive): a preview of exactly what will
    be copied, replaced and (optionally) deleted, then run it. With `rsync` when installed (resumes interrupted
    copies, only sends changes), else Kestrel's own copy engine. Deleting on the destination is off by default and
    asks again.
  - Saved sync pairs ("Pictures → Backup HDD") to run again in one click, recognising the drive by its id (see the
    friendly drive names item).
  - Every replacement still goes through part files (nothing half-copied over a good file).

- [ ] **Verify signatures.** Double-click a `.sig` / `.asc` / `.gpg` signature (or `SHA256SUMS.gpg`, `*.asc` next to a
  download) → **Verify Signature**: checks it with `gpg --verify` against the file it signs (the same name without
  the extension, or the one inside a clearsigned file) and says plainly: valid, signed by *name / key ID*, on *date*;
  or bad signature; or the signer's key isn't in your keyring (with its key ID and fingerprint, and a button to fetch
  it from a keyserver, asking first).
  - Ties in with the checksum files Kestrel already verifies: a signed `SHA256SUMS` (+ `.gpg`/`.asc`) checks the
    signature first, then every file it lists.
  - Trust in plain terms: a valid signature from a key you haven't marked as trusted is still "valid, but this key
    isn't verified by you", not a scary wall of gpg output.
  - Optional, later: **Encrypt / Decrypt** with gpg (to a recipient or with a password).

- [ ] **Remember folder sizes for Properties.** Kestrel's cache gets a section for folder sizes and counts it has
  already worked out (size, files, folders, and when it was measured). When Properties opens on a folder that's in it,
  the remembered figures show at once instead of "Calculating…", with a **Recalculate** button beside them;
  Recalculate runs the count again and stores the new result.
  - It says how old the figures are: "Calculated 3 days ago (2026-10-21 14:05)", in plain words (minutes, hours,
    days, months ago) with the exact date and time after. A freshly counted size says "Calculated just now".
  - Today Properties always counts from scratch (`fileops::dir_stats` on a worker, src/dialogs.cpp `PropertiesDialog`,
    shown by `show_dir_size`), which takes a long time on big trees and is slow on tapes and floppies.
  - Stored after every finished count (not a cancelled or failed one), keyed by the folder's path (plus the device
    and inode, so a different folder later under the same name isn't shown the old figures). For a selection of several
    items, each folder's own entry is used where there is one and only the rest is counted.
  - Whether to also recalculate on its own when the entry looks stale (the folder's modification time is newer than
    the entry; that only catches changes directly in it, not deeper down), or only when asked. Kestrel's own copies,
    moves and deletes into a folder could drop the entries of it and its parents.
  - Written with `util::write_atomic()` under ~/.cache/kestrel-explorer; old entries trimmed (count or age). Added to
    the BleachBit cleaner (kes-setup `bleachbit_cleaner`, tests/test_installer.sh) and to Clear Cache if there is one.
  - Test: `fileops` or `ui` (a remembered entry shows without counting; Recalculate replaces it; a cancelled count
    stores nothing).

- [ ] **New folder from the selection.** With two or more files and/or folders selected, right-click →
  **New Folder with Selection…**: a window asks for the new folder's name (prefilled with a free "New Folder" name,
  selected for typing over); on OK the folder is made in the folder the items were selected in and every selected item
  is moved into it.
  - A menu entry for the several-items menu (`menus::SEVERAL`): an id in `build_menu` (`menus::tag`), a place in
    `menus::defaults()` near Move To… (`src/menus.cpp`; `prefs` test checks they match). Maybe also for one item.
  - The name window is free (`ask_text()`, no modal). Re-read the selection's state in the callback: items moved or
    deleted meanwhile are left out (said in the summary), and the name re-checked when OK is pressed.
  - Claim the name without replacing anything (`mkdirat`, which fails on a taken name); a taken name asks again with
    the name kept. Then the moves are an ordinary queued move job (same drive: renames), so Undo puts the items back
    and removes the folder if Kestrel made it and it's empty. Each item moved whole; a failure or cancel leaves the
    rest where they were and the folder with what got moved (said in the job's log, not deleted).
  - Admin windows go through the helper; tapes and floppies work like any move on that drive.
  - Test: `fileops` / `ui` (the items end up inside, the folder in the right place, a taken name isn't replaced, Undo
    restores, a move failing part-way keeps everything).

### Images, video and audio

- [ ] **Image viewer: what's left of the quality-of-life ideas** (the rest is done, 0.3.2-alpha.7). Not chosen then:
  - **Rate** images (stars 1–5, as XMP Rating), next to star / pin.
  - **Video and animation:** GIF/WebP already animate; play short videos in place (or hand them to the default app),
    with play/pause and a seek bar.
  - **Background:** choose black, grey, the theme's colour, or a checkerboard for transparency.

- [ ] **Drag an image out of the viewer.** With an image open in Kestrel's image viewer, drag it out and drop a copy
  anywhere that takes files: the desktop, another Kestrel or GNOME Files window, a browser's upload field, a chat or
  mail app.
  - What's dragged: the file itself (`text/uri-list`, the `file://` URL, as the file views drag it), offered as a
    **copy** (never a move: the original stays), plus the image data (`image/png` or the original's type) for apps that
    only take pasted pixels. The drag shows the image's thumbnail under the pointer (Kestrel's own drag picture on
    Wayland, as the file views do), and the app it's dropped into gets the focus (the drop focus extension), as for
    drags from the file views.
  - Telling a drag-out from panning: left-drag pans the image today. Options: start a drag-out when the image fits the
    window (nothing to pan) and pan when zoomed in; or drag-out with a modifier (Alt or Ctrl) or the middle button; or
    from a small handle / the filmstrip thumbnail (see the viewer improvements item). To decide; maybe the first plus a
    modifier for zoomed images.
  - A rotated or flipped view that hasn't been saved: drag the file as it is on disk (and say so), or the image as
    shown (a temporary copy). (Saving a rotation is in the viewer now: R / L / H, then Save or Ctrl+S.)
  - Related: Ctrl+C already copies the image's pixels; it could also put the file on the clipboard, so Ctrl+V in a
    folder pastes a copy of the file.

- [ ] **Rotate images losslessly.** Right-click one image or a selection → **Rotate** → **90° clockwise**, **180°**,
  **90° counter-clockwise (270°)**, and **Flip horizontally / vertically**, saved to the files. Today rotating only
  exists in the image viewer (R / L / H, saved through the orientation tag: `imgedit::save_rotation`, reusable here).
  - JPEG: lossless (`jpegtran -rotate 90 -perfect -copy all`, or `exiftran`; package `libjpeg-turbo-progs` /
    `exiftran`): the picture data is turned without re-compressing, so no quality is lost. Where a JPEG's size isn't a
    multiple of its block size, `-perfect` refuses; then offer either trimming the odd edge pixels (`-trim`) or, as an
    option, just setting the EXIF orientation flag instead.
  - **Or only change the orientation flag** (EXIF Orientation, through exiftool or by rewriting the tag): instant,
    changes nothing else, and every modern viewer honours it. Possibly the default for JPEGs that already have one.
  - Other formats: PNG, WebP (lossless), TIFF are re-saved losslessly (Qt or the format's tool); lossy WebP, HEIC and
    AVIF re-encoding loses a little quality, so say so (or only set the orientation where the format has one).
  - Keeps the metadata (EXIF, dates) and resets the orientation flag to "normal" after turning the pixels, so the photo
    isn't turned twice. Written under a hidden part name and renamed over the original when done (fsync first, as it
    replaces data); a batch in the status bar with ✕. Thumbnails and folder mosaics refresh.
  - The viewer's R / L could then offer to save the rotation too.

- [ ] **Convert images.** Right-click one or more images → **Convert Image…**: to JPEG, PNG, WebP, AVIF, HEIC or TIFF
  (only formats something installed can write), e.g. iPhone HEIC photos to JPEG.
  - Options in plain terms: quality slider for lossy formats, resize (keep, a longest side, a percentage), keep or strip
    the metadata (EXIF, location), and what to do with transparency when the target has none (a background colour).
  - Decoding: Qt's image plugins first (Kestrel already reads HEIC, AVIF, JPEG XL and camera RAW through
    `kimageformat6-plugins`); ffmpeg where Qt can't. Encoding: Qt's writers where they exist, ffmpeg (or the
    format's own tool) for the rest. Offer only the targets that can actually be written on this system.
  - Keep the photo's orientation right (apply the EXIF rotation, or carry it over), and its colour profile where the
    target supports one.
  - Output beside the originals (or a chosen folder), never replacing a file; part names; progress and ✕ in the status
    bar for a batch; optionally move the originals to the trash afterwards.

- [ ] **Convert videos with ffmpeg.** Right-click one or more video files → **Convert Video…** (shown only when `ffmpeg`
  is installed, like the other tool-dependent entries) opens a settings window with the options in plain terms, the
  way Compress… presents an archive tool's options.
  - **Presets first:** e.g. "MP4 (H.264), plays everywhere", "Smaller file (H.265)", "WebM for the web", "Audio only
    (MP3 / M4A / Opus)", "GIF", "Same format, just smaller". Choosing one fills in the options below; changing any
    option makes it "Custom".
  - **Options, in an easy form:** container (MP4, MKV, WebM, MOV, GIF, audio-only), video codec (H.264, H.265, VP9,
    AV1, copy as is), quality as a slider from "smaller file" to "better quality" (CRF underneath) or a target size,
    resolution (keep, 4K, 1080p, 720p, 480p, or a width), frame rate (keep, 60, 30, 24), audio (keep, re-encode with a
    bitrate, remove), speed vs. size (ffmpeg's preset), trim (start and end time), rotate/flip, and **Extra options**
    passed straight to ffmpeg for anything else.
  - Only what the installed ffmpeg can do is offered (ask it for its encoders: `ffmpeg -encoders`); hardware encoding
    (VAAPI / NVENC) when available, off by default.
  - Shows the source's details (`ffprobe`: length, resolution, codecs, size) and a rough estimate of the new size
    where it can.
  - **Command preview** at the bottom showing exactly what will run, as the Compress dialog does; the last settings are
    remembered (per preset).
  - Output: beside the original as `name.converted.mp4` (or a chosen folder and name), never replacing the original
    unless asked; written under a hidden part name and renamed when finished, so a cancel or failure leaves no
    half-made video. Optionally move the original to the trash afterwards.
  - Runs in the status bar with a real percentage (ffmpeg's `-progress` output against the length from ffprobe), the
    time left, and ✕ to cancel. Several files are converted one after another as one job, with one summary at the end.

- [ ] **Extract audio from videos.** Right-click one or more videos → **Extract Audio…** (shown when `ffmpeg` is
  installed).
  - **Keep the original audio** (default when it fits a common container): the audio stream copied out unchanged
    (`-vn -c:a copy`), instant and lossless, into the matching file type (AAC → `.m4a`, Opus → `.opus`/`.ogg`, MP3 →
    `.mp3`, anything else → `.mka`).
  - **Or convert it:** MP3, M4A (AAC), Opus, FLAC or WAV, with a quality choice in plain terms (e.g. "Good (192 kbps)").
  - A video with several audio tracks: choose which (language/title from `ffprobe`), or all of them.
  - Same handling as Convert Video: written under a hidden part name, beside the video by default, never replacing a
    file, progress and ✕ in the status bar, several files as one job.

- [ ] **Convert audio files.** Right-click one or more audio files → **Convert Audio…** (with `ffmpeg`): FLAC, WAV,
  ALAC, MP3, M4A (AAC), Opus or OGG, e.g. a FLAC album to MP3 or Opus for a phone or music player.
  - Quality in plain terms (bitrate or VBR level), sample rate and channels (keep, or 44.1/48 kHz, stereo/mono).
  - Tags and cover art kept (title, artist, album, track number; embedded cover where the target supports it).
  - Optional **loudness normalisation** (ffmpeg's `loudnorm`, two passes) so a batch plays at the same volume.
  - A whole folder at once, keeping its subfolders in the output folder.
  - Same handling as the other conversions: part names, never replacing, progress and ✕, one summary for a batch.

### Desktops

- [ ] **Kubuntu 26.04 (KDE Plasma) support.** The .deb already installs on 26.04 (CI's `package` job), and Kubuntu
  uses the same archive, so this is desktop integration: everywhere Kestrel assumes GNOME or Cinnamon. Kubuntu 26.04
  is Wayland-only. Test in a Kubuntu VM, the way Mint was done.
  - **Probably fine already, to confirm:** Breeze colours and fonts come through KDE's Qt integration
    (plasma-integration), dark mode through the portal's colour scheme (`desktop_prefers_dark`), cut/paste with Dolphin
    (`application/x-kde-cutselection`), trash (`g_file_trash`), the shared thumbnail cache, `recently-used.xbel`
    (KDE Frameworks write it too), archive passwords in KWallet (Secret Service).
  - **File chooser portal (kes-setup):** `PORTAL_DESKTOP` becomes `kde` (right), but the FileChooser line falls back
    to `kestrel;gnome;gtk;`. On KDE the fallback should be `kestrel;kde;`. Check `restart_portal` and `--undo` with
    xdg-desktop-portal-kde.
  - **Default file manager:** remember and restore Dolphin (`org.kde.dolphin.desktop`), not Nautilus. Today that's
    `prev:-org.gnome.Nautilus.desktop` and the `pgrep nautilus`/`nemo` handling. A running Dolphin takes
    `org.freedesktop.FileManager1` ("Show in folder"), so `--default` has to deal with it the way it quits Nautilus.
  - **`--dock`:** Plasma's taskbar pins (launchers in `plasma-org.kde.plasma.desktop-appletsrc`). Change them through
    plasmashell's `evaluateScript` over D-Bus, not by editing the file. This is the same open item as Cinnamon's panel.
  - **Drag focus on Wayland:** the GNOME Shell extension does nothing under KWin. The equivalent is a small KWin
    script loaded through `org.kde.KWin /Scripting` that activates the window under the pointer (a new path in
    src/focus.cpp). It may not need a log-out.
  - **gsettings-only desktop settings** (`desktop_schema()` only maps Cinnamon):
    - wallpaper: `set_wallpaper()` → `plasma-apply-wallpaperimage`;
    - File History privacy switch: KDE keeps it in its activity manager settings, not `org.gnome.desktop.privacy`;
    - icon theme: `setup_icon_theme()` reads gsettings, which KDE's GTK-config module usually fills in. Reading
      `kdeglobals` `[Icons] Theme=` is safer.
  - **Drives, network shares and phones without gvfs:** src/overview.cpp uses GIO's volume monitor, and the udisks2
    and MTP monitors come from gvfs, which Kubuntu may not install. Without it, unmounted USB drives, `smb://`/`sftp://`
    and phones wouldn't appear or mount. Either add gvfs-backends/gvfs-daemons to Recommends (and install.sh's
    RECOMMENDED) or add a udisks2 path. Check this first; it's the item most likely to change the plan.
  - **Terminal:** `konsole` is in `open_terminal()` but comes after ptyxis and gnome-terminal. Try the desktop's own
    terminal first.
  - **Search index (optional):** `index_covers()` only knows Tracker/localsearch. KDE uses Baloo (folders in
    `~/.config/baloofilerc`). Without it, Kestrel falls back to its own search, which is fine.
  - **Tests:** a `XDG_CURRENT_DESKTOP=KDE` case in `installer` (portal fallback, Dolphin restore, dock script), plus
    unit checks for the wallpaper and icon-theme choice. No new CI job: the 26.04 package check covers installing.
  - **Docs:** a Kubuntu section in docs/install.md, like the Mint one.
  - Order: walk through the app in the VM (drives first), then kes-setup (portal, Dolphin, FileManager1), then the
    wallpaper/privacy/icon mapping, then KWin drag focus, then `--dock`.

## Done

- [x] **Dragging 800+ selected folders into another folder froze the window for several seconds** (fixed in 0.3.2-alpha.13; not grouped, same drive, the desktop offered to force quit). Cause: each folder leaving the view deselects it, and every selection change ran `MainWindow::update_status`, which counted the whole selection through `Pane::selected_paths()`, whose duplicate check was a list search over every row × column index (quadratic per call, so cubic in all). Now `selected_paths` uses a set, and the status line is updated once per burst (30 ms single-shot); the file chooser's update stays immediate (its OK button follows it). Measured in a CI-like container with 1,500 folders: the window held 5.7 s before, about 0.4 s now. Not changed: date grouping's full regroup per removed row (`DateGroupProxy::after`), still quadratic for big grouped folders. Test: `ui`.
- [x] **Kestrel froze when Open as Archive finished reading a big archive** (fixed in 0.3.2-alpha.13). Cause: `show_entries` built the QTreeWidget items (icons, dates, sorting) on the UI thread, about 1.9 s per 100,000 files. Now `ArchiveWindow::Listing::build` makes them in the reading job, sorted there (QCollator sort keys), icons looked up in `EntryItem::data` only for drawn rows; the tree isn't kept sorted (the header's clicks sort it) so adding them doesn't re-sort. The listing itself was slow too: `QDateTime::fromString` (~0.2 ms each) replaced by a hand parser in `archive.cpp` `parse_time`, and listings no longer run under `stdbuf -o0`. Test: `archives`.
- [x] **Password-encrypted tar archives (through gpg)** (0.3.2-alpha.12). Compress… offers a password for tar and every tar.*: the stream goes on through `gpg --symmetric` (AES-256, `-z 0`) to `name.tar.zst.gpg`, the password on gpg's fd 3 (`proc::Options::fd3`), `--no-symkey-cache`. `archive::kind` / `gpg_encrypted` recognise `.tar…gpg`; `tar_pipe` reads through `gpg -d` for listing, testing and extracting; probe calls them encrypted, `password_opens` waits for gpg's first byte or "Bad session key", which is a WrongPassword. gnupg in Suggests, OPTIONAL_PACKAGES and docs/dependencies.md; the password is greyed out with an install hint without gpg. Test: `archives`, `fileops`.
- [x] **Open as Archive left out part of a big archive's contents** (0.3.2-alpha.9). `run_reporting()` (src/archive.cpp) kept only the last 1–2 MB of a tool's output, and `archive::list()` and the quick password test read listings through it. Now they pass an `on_line` callback that gets every line as it's printed, and parse as they go (`SmallestEncrypted` for the password test); the returned text keeps the cap, for messages. 7z and rar failures are judged by whether the listing started, not by text near its top. Found along the way: `tar_pipe()` read tar's output only after feeding it the whole archive, so a listing (or `-tf` test, or many errors) over a pipe's worth hung; it's read on a thread now. Test: `archives`.
- [x] **Copies onto an LTO tape slowed to about a quarter of the drive's speed for long stretches** (0.3.2-alpha.7). Found by elimination: a raw dd to the drive, dd through LTFS with 512 KB and with 16 MB writes, and with reading and writing overlapped all held full speed; Kestrel's writer only writes (strace), each write waiting on LTFS; the page cache wasn't it. The cause: other programs asking every mount for its free space. GNOME System Monitor (its File Systems tab, every few seconds) caused the long slow stretches; GNOME's low-disk-space check (gsd-housekeeping, once a minute) caused shorter drops. On LTFS each such question is commands to the drive, which then leaves streaming (as Kestrel's own polling did before 0.3.2-alpha.6). Fixed in Kestrel: while a job writes to a tape, the tape's mount point is on GNOME's check's ignore list (`drives::quiet_tape`, taken off after if Kestrel put it there), and a system monitor that's running is named in the job's log (`drives::free_space_pollers`). Left: "Tape copies: short dips left" above. Test: `slow`.
- [x] **Image viewer: quality-of-life improvements** (0.3.2-alpha.7). `src/viewer.cpp`: `ViewerCanvas` (one image: zoom, pan, rotation, crop selection, on-screen zoom level; double-click fit ↔ 100% at the pointer, pinch), `ImageViewer` with a second canvas for Compare (X; V side by side ↔ A/B, B switches; zoom linked relative to fitting), `Filmstrip` (T; thumbnails from the thumbnail manager, not on slow drives unless allowed) and `ViewerInfo` (I; details, GPS map link to OpenStreetMap, generation prompts, all metadata, histogram). Decoding at screen size ahead (next, previous, +2) and full size when zoomed past it; the thumbnail cache's preview meanwhile. Quick actions: * star, P pin, G show in folder, O Open With, M / C move or copy to a folder, Ctrl+1–9 / Alt+1–9 to the last nine (`viewer_recent_folders`, in the BleachBit cleaner's History), Delete records undo and Ctrl+Z puts the image back in the list (`undo::undo` gained an after-callback). `src/imgedit.cpp`: rotations saved losslessly (the EXIF orientation tag patched in a part file; exiftool adds one to a JPEG without; PNG/BMP re-written; others refused), asked when leaving the image (`viewer_save_rotation`: ask / always / never; Ctrl+S now), Straighten (jpegtran, now optional: libjpeg-turbo-progs), crop and resize (previewed at once as `steps`, `imgedit::preview` on the screen-size copy; saved with Ctrl+S or on leaving the image: new file or replace, asked; a JPEG's APP1 segments copied with the tag set to 1, a PNG's text kept). Remembered: geometry (`viewer_geometry`), full screen, filmstrip, info panel, keep zoom. A selection opens in the view's order. Its right-click menu is a kind in Preferences → Menus (`menus::VIEWER`, `ImageViewer::build_menu`; View and Rotate and Edit are containers). Changed: I is now the info panel (Tab the info line); double-click no longer goes full screen. Not done: see "Image viewer: what's left". Test: `viewer`.
- [x] **Better search: filters, and grep** (0.3.2-alpha.3). Filters: `SearchFilter` (src/search.h; type by name, size, date modified or created) in a row under the search bar (`Pane::build_filter_row`), kept for the session, applied by `SearchThread` (also with no name, and without subfolders). Inside files: `ContentSearch` (Automatic / Search index / Read the files, Regular expression, Match case); `SearchThread::run_read` streams ripgrep (else grep, `--null`) with a few lines per file into a Match column, stopped with ✕; Automatic uses localsearch where its settings cover the folder (`index_covers`) for plain words, and reads the files when it finds nothing. Opening a result at its line: gedit and forks, Geany, VS Code and forks, Kate, Sublime, Zed (GNOME Text Editor has no line option). Decided: chips weren't used (a row of drop-downs); File contents now follows Include subfolders (it always searched subfolders); nothing is read on tapes or floppies. ripgrep is optional (kes-setup, Suggests). Test: `search`.
- [x] **Collapse a date group from anywhere in it** (0.3.2-alpha.3). A footer row in `DateGroupProxy` (src/groups.cpp: a row kind of its own after an open group's last item, "Collapse Today (1,240)", drawn quieter like a header, not selectable, spanning the list's columns and a whole line in the grid); `Pane::collapse_group` keeps the view on the header (the current row). Ctrl+Left / Ctrl+Right on the current item's group (Ctrl+Shift+Minus / Plus would clash with zoom on many layouts); Collapse This Group on an item's menu (`collapse_group`), Collapse / Expand All Groups in the Sort menu and on empty space's (`collapse_groups`, `expand_groups`). Not done: the sticky header (the footer was chosen). Test: `groups`.
- [x] **Write straight to the file on a tape, without a part file** (0.3.2-alpha.3; tapes only, by the user's choice: floppies keep the part file, which works as intended on FAT). `copy_file_at` (src/fileops.cpp) and the admin helper's `Job::copy` (it recognises a tape from the mount table, `on_tape`) open a new name on a tape with O_CREAT|O_EXCL and write it there; a cancel or failure deletes it; a taken name is a conflict, kept. Decided: replacing a file on a tape keeps the part file and rename (the old one is never lost first). Test: `slow`, `admin_helper` (inotify: `DirWatch`).
- [x] **File chooser: its own Overview for downloads, and free names when changing folders** (0.3.2-alpha.3). Free names: `ChooserBar::folder_changed` (src/chooser.cpp) makes the suggested name free in each folder shown, also the one it opens in, with `util::unique_path(..., "download")` (from (1)); the original comes back where it's free; a name typed or a file picked is the user's; an app replacing a file (current_file) keeps its name; SaveFiles gives each file a free name (the portal allows it) instead of asking to replace; not on tapes or floppies (not looked up there). Save locations: `chooser_places`, shown first on a Save window's Overview (`OverviewPage::save_places_section`), managed there (Add Folder…, Move Earlier / Later, Remove) and in Preferences → Windows; `chooser_start` (default: as before) can open Save windows there. Decided: Open windows start as before; an app replacing a file opens in its folder; Add Folder… starts where the last Save window saved. BleachBit cleaner: a Save locations option. Test: `chooser`, `installer`.
- [x] **A sidebar section for mounts** (0.3.2-alpha.3). A **Mounts** section (`Sidebar::user_mount_list`, src/widgets.cpp) lists what file managers show that Devices doesn't: GIO's rule (`drives::user_mount`: in the home folder, /media or /run/media/<user>, not a system file system, device or path, not inside a hidden folder), from the mount table only (GIO also tries to enter /media mounts, which can ask the drive, and hides bind mounts; GNOME Files' udisks2 monitor lists them, as a folder in the home folder mounted elsewhere). Plus GVFS network shares (`overview::network_infos`, shared with the Overview) with Unmount, and recent servers that aren't connected, greyed, with Remove from Recent Servers. An icon and a tooltip by kind (ZFS dataset, network share, encrypted folder, FUSE, disk). Not done: friendly names (their own item); the Overview still shows one card per ZFS pool (datasets in the home folder aren't cards of their own) and leaves out FUSE mounts that aren't network ones (gocryptfs…); comparing with GNOME Files in the real session (unmounted partitions it offers to mount) is still to do by hand. Test: `sidebar` (`KESTREL_TEST_MOUNTS`).
- [x] **Double-clicking a checksum file on a tape still froze Kestrel while the drive sought** (0.3.1-alpha.16). Caught with a trace of the window's thread: adding the file to Recent asked GIO for its content type (`util::content_type`, `g_file_query_info`), which reads the file's start, right after a write to the tape when the drive winds back from the index. On slow drives the type is now by name (Recent, Open With, `open_default` starts the type's app itself). Test: `slow`.
- [x] **Prevent shoe-shining on tapes as much as possible: a RAM write buffer, and more** (0.3.1-alpha.13). `src/tapebuf.cpp` (the buffer: memory available less a share kept free, re-checked; a reader thread in the copy's order; fill first, then stream at the source's pace within the drive's speed-matching range or refill-and-burst below it; posix_fadvise; status and log figures), used by copies onto a tape (`Ops::start_tape_buffer`, src/fileops.cpp); room checked before each file; no fsync on tapes; Properties no longer reads a tape file to name its type; LTFS's timed index sync read from its command line (Overview card, the copy's log). Preferences → Files → Tapes. Not done: mounting tapes from Kestrel with `sync_type=unmount` (in "LTFS and slow drives: what's left"); not yet measured on the real drive. Test: `slow`.
- [x] **A better Preferences window that shows every setting** (0.3.1-alpha.13). `src/prefs.cpp` (every setting: tab, label, default, where else it's changed; set() applies it to every window and tells the other Kestrels, "settings" with its keys, PROTOCOL 4), `src/prefswin.cpp` (tabs General, Views, Sidebar, Images & Videos, Files, Archives, Windows, Menus; live changes, search, Reset This Tab / Reset All). Not done: the Overview's contents ("Overview page improvements"); conflict and trash choices (Kestrel keeps none). Test: `prefs`.
- [x] **Customise the right-click menu** (0.3.1-alpha.13). `src/menus.cpp`: ids on every entry (build_menu), default layouts for a folder, a file, an archive, several items and the empty space, `arrange()` from the saved layout (hidden, reordered, submenus, new entries in their default places), and the editor in Preferences → Menus. The Trash's own menu isn't customisable. Test: `prefs`.
- [x] **Bug: drag and drop onto a folder did nothing when grouped by period or by day** (0.3.1-alpha.12). The grouping model (`DateGroupProxy`, src/groups.cpp) didn't pass the drag's data and actions on to the folder model, so a drag from a grouped view carried no files and offered no move. Test: `ui`.
- [x] **Open as Archive and Test Archive** (0.3.1-alpha.12). `src/archivewin.cpp` (a window listing an archive's contents, extracting the selected items: `archive::list`, `archive::extract_members`) and Test Archive(s) (`archive::test`, `archive_ui::test_archives`). Follow-ups: "Better archive window QoL" under New features. Test: `archives`.
- [x] **Extract several archives at once** (0.3.1-alpha.12). Extract All Here / Extract All To… for two or more selected archives (a split set selected whole is one, and now gets Extract Here / Extract To…): one queued job with each archive as a step, the first typed password tried on the rest (asked again only when it doesn't open one), one summary at the end (`src/archive_ui.cpp` extract_all; the quick password test is `archive::password_opens`). Extract Here / Extract To… on one archive run through the same job. Test: `archives`.
- [x] **Remember archive passwords in the keyring** (0.3.1-alpha.12). `src/keyring.cpp` (libsecret, schema org.kestrel_explorer.ArchivePassword in the login keyring, labelled "Kestrel Explorer: archive password for …"; fingerprint: size + SHA-256 of the first 64 KB). Opt-in Remember box in the password window and Extract To…; order: the password typed last, the one saved for the archive, the other saved ones, then ask. Preferences → Forget Saved Archive Passwords…. New dependency libsecret-1 (build: libsecret-1-dev; .deb: libsecret-1-0). Test: `archives` (with a real gnome-keyring-daemon).
- [x] **A Jobs window, and one job queue for every Kestrel** (0.3.1-alpha.12). `src/jobs.cpp` (the queue: kept by the tower across Kestrels, each Kestrel's own without one; by drive or one at a time; drives by physical disk), `src/jobswin.cpp` (every job, steps, logs and live tool output, Pause/Resume/Stop/Start Now/Up/Down, Pause All, start paused). Not done: another Kestrel's job log isn't sent through the tower (the window says it's in that Kestrel's Jobs window); only copy/move/delete jobs list steps; administrator jobs can't be paused (the helper runs as root). Test: `jobs`.
- [x] **A Tower window: see what the tower is doing** (0.3.1-alpha.12). `src/towerwin.cpp`, ☰ → Tower… with Preferences → Show developer tools, or `kes --tower-monitor`: status (pid, uptime, protocol, landing, refused messages) with Restart Tower, the Kestrels checked in, the live message log with a filter and pause, the queue and drives in use, the shared undo list. Test: `jobs`.
- [x] **LTFS tape support: no freezing, and tape-friendly behaviour** (0.3.1-alpha.11). `src/drives.cpp`: tapes (LTFS mounts) and floppies recognised from the mount table; sizes read in the background (sidebar, Overview, status bar, Properties); no previews/details/animations/dates read unless allowed per drive; one job at a time per drive; tape-order reads with "Positioning the tape"; index written after a copy and on demand; small-files warning; the Overview card; background unmount with a warning while a job runs. What's left: see "LTFS and slow drives: what's left". Test: `slow`.
- [x] **Kestrel freezes while another program copies from the tape** (0.3.1-alpha.11). Cause as suspected: the sidebar's 4-second mount check asked every mount for its size on the UI thread. Mounts now come from `/proc/self/mountinfo`, sizes from a worker. Tests: `slow`, and `ui` (no `QStorageInfo` outside the Overview's background scan).
- [x] **Copying to an LTFS tape fails with "no permission", also as administrator** (0.3.1-alpha.11). Fallback to check-then-rename when `link()` fails for lack of support (Kestrel's copy, `util::put_new`, the admin helper). Not yet confirmed on the tape itself (see what's left). Tests: `fileops`, `admin_helper` (`KESTREL_TEST_NO_LINK`).
- [x] **Opening a checksum file on a tape freezes Kestrel until the tape gets there** (0.3.1-alpha.11; finished in 0.3.1-alpha.14: it still froze, as the window asked the busy tape for other things meanwhile: the selection's size, the info panel, the opened items' kinds, Recent, the Verify window's file sizes). The checksum file is read on a worker (also Properties → Browse…). Test: `slow` (a FIFO).
- [x] **USB floppy drives freeze Kestrel while they read** (0.3.1-alpha.11). Fixed with the same slow-drives mode as tapes; a floppy is a removable disk of at most 4 MB (or `/dev/fd*`); navigating into one no longer stats it on the UI thread. Not yet tried with the real drive (see what's left). Test: `slow`.
- [x] **The file view jumps after a drag into a subfolder** (fixed in 0.3.1-alpha.10). Cause confirmed: the current item leaving the folder made Qt scroll to its neighbour (seen in the list view). `FileView` no longer scrolls when the current item is removed. Test: `ui`.
- [x] **A folder's picture slides across the view when going back up** (fixed in 0.3.1-alpha.10). Cause: Qt starts a drag on a move with any button held; a move with the mouse's back button still down dragged the folder just left (selected again above), and the drag going nowhere slid back. Only the left button drags now, and a drag that outlives its folder doesn't fly back. Test: `ui`.
- [x] **A rare single gap in the grid** (fixed in 0.3.1-alpha.10). Two causes found: a batched layout redone started each 400-item batch on a new row (now one pass: ~20 ms for 50,000 items), and cells adding up to exactly the width pushed the last column onto the next row (one pixel of slack). Test: `ui`.
- [x] **The file chooser's Save button stays greyed out after changing folders** (fixed in 0.3.1-alpha.10). Cause as found: the button was only re-checked on name and selection changes; now on every folder and tab change too. Test: `chooser`.
