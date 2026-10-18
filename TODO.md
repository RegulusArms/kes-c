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
  - So something during a long session breaks that window's state. Still to find out: does the window follow the
    mouse until the next click, or jump away at once?

## New features

### Jobs

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

- [ ] **A sidebar section (dropdown) for mounts.** A collapsible **Mounts** section in the sidebar, like Places,
  Bookmarks and Devices (moved and collapsed the same way), listing the mounts GNOME Files shows that Kestrel's
  sidebar leaves out.
  - Found on this machine (2026-10-14): GNOME Files lists `~/dataset-a` and `~/dataset-b` (ZFS datasets
    `tank/secure/…` mounted inside the home folder); Kestrel's sidebar doesn't. Its Devices list (`Sidebar::mount_list`
    in `src/widgets.cpp`) only takes a mount whose device is a `/dev/…` disk, or one under `/media`, `/run/media` or
    `/mnt`, so anything else is dropped: ZFS datasets, bind mounts, and FUSE mounts (rclone, sshfs, gocryptfs,
    Cryptomator…) in the home folder or elsewhere.
  - Use GIO's own rule for what to show (`g_unix_mount_guess_should_display()`, or the volume monitor's mounts, as
    GNOME Files does), so Kestrel lists the same mounts, including those in the home folder.
  - Network shares (SMB, SFTP, NFS, FTP: GVFS mounts) aren't in the sidebar either, only on the Overview: list them in
    this section too, with Unmount on the entry or its menu. Recently used servers that aren't mounted could show
    greyed, a click reconnecting them.
  - Each entry: its name (and the friendly name, see that item), an icon by kind (ZFS dataset, network share, FUSE,
    disk), and opening on click. Check the Overview's Drives section for the same gap.
  - Also compare with GNOME Files in the user's real session (unmounted partitions it offers to mount), which a
    shell without the desktop's udisks volume monitor can't show.

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
    mounted, `getfattr -d -m - /media/tape` and on a file. Fix the names where they differ.
  - Check with the real USB floppy drive that it's recognised (a removable `sd` disk of at most 4 MB) and that browsing
    it no longer freezes.
  - Mount and format tapes (`ltfs`, `mkltfs`) from Kestrel (needs permissions on the drive device; within the admin
    session's scope), and a catalogue of tapes that aren't loaded (their index saved when last mounted, to search
    what's on which tape).

### Preferences and customising

- [ ] **A better Preferences window that shows every setting.** Every setting Kestrel keeps, also the ones set from
  elsewhere in the windows (menus, toolbar, dialogs), in one place, in tabs.
  - Today Preferences is one long form without tabs, and many settings are only reachable where they're used: grid or
    list view, zoom, hidden files, folder previews, the default sort and grouping, Pinned & Starred First, sidebar
    sections, search options (subfolders, file contents), checksum file choices, archive options per format, Extract
    To… choices. Preferences should show and change all of them, as well as where they are now.
  - Tabs, for example: General (homepage, single click, opening apps) · Views (view, zoom, hidden files, previews,
    folder colours, default sort and grouping) · Sidebar & Overview (sections, order, the Overview item above) ·
    Images & Videos (viewer, slideshow, GIF/WebM, preview picks, thumbnail size limit) · Files (undo sharing, conflict
    and trash choices, checksum defaults) · Archives (per-format defaults, Extract To… choices) · Windows (tabs from
    other apps, the file chooser's own view settings).
  - A search box that filters every tab's settings, and "Reset this tab" / "Reset all" to the defaults.
  - Settings changed in Preferences apply at once to every open window (and other Kestrels, as saving Preferences does
    now); settings changed in a window show up in an open Preferences window.

- [ ] **Customise the right-click menu.** To be done after "A better Preferences window that shows every setting"
  (above): it would be a tab of that window.
  - Users set up how the right-click menu looks, separately for a folder, a file and an archive (and perhaps for
    several selected items and for the empty background).
  - Add or remove any of the available entries, and reorder them.
  - Change the layout: separators, which entries sit at the top level and which go into submenus (such as Links &
    Shortcuts), and creating submenus of their own.
  - Entries that only apply sometimes (Extract only for archives, admin entries, tools that aren't installed) keep
    appearing only when they apply, wherever the user put them.
  - "Reset to default" for each menu. New entries added in later versions show up in their default place.

### Views and search

- [ ] **Collapse a date group from anywhere in it.** With Group by Period / Group by Day, a large group means scrolling
  all the way back up to its header to collapse it. Make it collapsible from the bottom too, and without scrolling:
  - **A footer at the end of each group:** a slim row ("▴ Collapse Today (1,240)") after the group's last item that
    collapses it, then keeps the view on that group's header so the user doesn't lose their place. A new row kind in
    `DateGroupProxy` (`src/groups.cpp`), drawn like the header, not selectable, spanning the list's columns and taking
    a whole line in the grid, as headers do.
  - **A sticky header:** the header of the group at the top of the view stays pinned there while scrolling through it
    (as phones' photo apps and many file managers do), still clickable to collapse. Collapsing from it scrolls so the
    collapsed header is at the top. Could replace the footer, or come with it (to decide; maybe a preference).
  - **From anywhere:** right-click an item → **Collapse This Group**, plus **Collapse All Groups** / **Expand All
    Groups** (Sort menu and right-click on empty space); a key for it, e.g. Ctrl+Left / Ctrl+Right or
    Ctrl+Shift+Minus / Plus, on the current item's group.
  - Keep: a collapsed group still counts its items, and selecting an item in it opens it (as now).

- [ ] **Better search: filters, and grep.** The search bar gains filters, and a way to search inside files that
  doesn't depend on the desktop's index.
  - **Filters** (what `find` does): type (images, videos, audio, documents, archives, folders), size (over / under),
    modified/created date (today, this week, a range), combined with the name; usable with "search subfolders".
    Shown as chips or a small filter row, and remembered for the session.
  - **grep** (search file contents directly): today *File contents* uses the desktop's search index (`localsearch`),
    which only covers indexed folders (not external drives, network shares or folders it skips) and doesn't do regular
    expressions. Add a direct content search with `grep -r` (or `ripgrep` when installed, much faster): plain text or
    a regular expression, case-sensitive or not, skipping binary files, on a thread with ✕. Results show the matching
    line and line number, and open the file (in a text editor at that line where possible).
  - Use the index when it covers the folder (instant), grep otherwise, or let the user pick.

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

### Images, video and audio

- [ ] **Image viewer: quality-of-life improvements.** The viewer (`src/viewer.cpp`) covers the basics today: arrows,
  wheel and mouse back/forward to move between images, Ctrl+wheel zoom at the pointer, drag to pan, fit (0) and
  actual size (1), fullscreen, slideshow, the info overlay (I), trash, copy, and a cache of the nearby images.
  Rotate and flip only change the display. Ideas, to choose from when this is picked up:
  - **Saving rotation:** R / L / H can save the change to the file, losslessly (see the rotate item above), with an
    "always save" choice so it doesn't ask each time.
  - **A filmstrip** of thumbnails along the bottom (toggle), to jump to any image; the current one highlighted.
  - **Same order as the folder:** the images in the order the folder is sorted (and grouped) in Kestrel, and only the
    selection when several were selected.
  - **Zoom:** the zoom level shown briefly when it changes; double-click toggles fit / 100% at the pointer;
    pinch-to-zoom on touchpads; keep the zoom and position when moving to the next image (an option, for comparing).
  - **Faster:** preload the next and previous images in the background at screen size (larger cache for big folders),
    full-resolution decode only when zoomed in; show the embedded/thumbnail preview first for RAW and huge files.
  - **Info:** a side panel option for the full EXIF (camera, lens, exposure, GPS with a map link, the image-generation
    prompt) instead of only the overlay; a histogram.
  - **Quick actions:** star / pin (the badges Kestrel shows elsewhere), rate, set as wallpaper, open the containing
    folder with the file selected, Open With, copy to / move to a recent folder with one key (for sorting a card of
    photos), and undo for trash from inside the viewer.
  - **Light edits:** crop (saved under a new name or over the original, asking), resize, and auto-straighten the
    orientation flag; anything bigger stays with a real editor (Open With).
  - **Video and animation:** GIF/WebP already animate; play short videos in place (or hand them to the default app),
    with play/pause and a seek bar.
  - **Compare:** two images side by side (or A/B toggle) with linked zoom, for picking the better shot.
  - **Background:** choose black, grey, the theme's colour, or a checkerboard for transparency.
  - **Remembered:** window size and position, fullscreen, filmstrip and info panel state.

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
    shown (a temporary copy) — to decide with the rotation-saving item.
  - Related: Ctrl+C already copies the image's pixels; it could also put the file on the clipboard, so Ctrl+V in a
    folder pastes a copy of the file.

- [ ] **Rotate images losslessly.** Right-click one image or a selection → **Rotate** → **90° clockwise**, **180°**,
  **90° counter-clockwise (270°)**, and **Flip horizontally / vertically**, saved to the files. Today rotating only
  exists in the image viewer (R / L), and only for display.
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

### The file chooser

- [ ] **File chooser: its own Overview for downloads, and free names when changing folders.** For the Save dialogs other
  apps open in Kestrel (a browser's downloads, "Save image as"…).
  - **Its own start page:** a Save chooser can open on an Overview of its own, with quick-access save locations the user
    sets up for the chooser only (like Starred, but separate: adding one doesn't star the folder in Kestrel), e.g.
    Downloads, Pictures/Saved, a project folder. Managed from the page itself (add the current folder, remove, reorder)
    and from the chooser's preferences.
  - **Or keep today's behaviour:** a setting in the chooser's preferences chooses where a Save chooser starts: that
    Overview, or as now (the folder the app suggests, else where the last chooser saved; `chooser_folder` in
    `src/app.cpp` / `src/chooser.cpp`). Open choosers keep starting as now unless this should apply to them too
    (to decide).
  - **A free name in each folder:** when the user goes to another folder while saving, Kestrel checks the name in the
    name box against that folder and, if a file of that name is already there, changes it to the next free one
    (`photo.jpg` → `photo (1).jpg`, then `(2)`…), as browsers name repeated downloads. Going back to a folder where the
    original name is free puts the original name back. Only names Kestrel changed itself are changed back: once the
    user edits the name, it's theirs (still checked, with today's "Replace?" question when saving over a file).
    Several files saved at once: each name made free the same way.
  - Numbering: `util::unique_path(..., "number")` starts at ` (2)`; downloads want ` (1)` first, so it needs a style
    for that (and it already strips an existing ` (n)` before numbering).

## Done

- [x] **Bug: drag and drop onto a folder did nothing when grouped by period or by day** (0.3.1-alpha.12). The grouping model (`DateGroupProxy`, src/groups.cpp) didn't pass the drag's data and actions on to the folder model, so a drag from a grouped view carried no files and offered no move. Test: `ui`.
- [x] **Open as Archive and Test Archive** (0.3.1-alpha.12). `src/archivewin.cpp` (a window listing an archive's contents, extracting the selected items: `archive::list`, `archive::extract_members`) and Test Archive(s) (`archive::test`, `archive_ui::test_archives`). Follow-ups: "Better archive window QoL" under New features. Test: `archives`.
- [x] **Extract several archives at once** (0.3.1-alpha.12). Extract All Here / Extract All To… for two or more selected archives (a split set selected whole is one, and now gets Extract Here / Extract To…): one queued job with each archive as a step, the first typed password tried on the rest (asked again only when it doesn't open one), one summary at the end (`src/archive_ui.cpp` extract_all; the quick password test is `archive::password_opens`). Extract Here / Extract To… on one archive run through the same job. Test: `archives`.
- [x] **Remember archive passwords in the keyring** (0.3.1-alpha.12). `src/keyring.cpp` (libsecret, schema org.kestrel_explorer.ArchivePassword in the login keyring, labelled "Kestrel Explorer: archive password for …"; fingerprint: size + SHA-256 of the first 64 KB). Opt-in Remember box in the password window and Extract To…; order: the password typed last, the one saved for the archive, the other saved ones, then ask. Preferences → Forget Saved Archive Passwords…. New dependency libsecret-1 (build: libsecret-1-dev; .deb: libsecret-1-0). Test: `archives` (with a real gnome-keyring-daemon).
- [x] **A Jobs window, and one job queue for every Kestrel** (0.3.1-alpha.12). `src/jobs.cpp` (the queue: kept by the tower across Kestrels, each Kestrel's own without one; by drive or one at a time; drives by physical disk), `src/jobswin.cpp` (every job, steps, logs and live tool output, Pause/Resume/Stop/Start Now/Up/Down, Pause All, start paused). Not done: another Kestrel's job log isn't sent through the tower (the window says it's in that Kestrel's Jobs window); only copy/move/delete jobs list steps; administrator jobs can't be paused (the helper runs as root). Test: `jobs`.
- [x] **A Tower window: see what the tower is doing** (0.3.1-alpha.12). `src/towerwin.cpp`, ☰ → Tower… with Preferences → Show developer tools, or `kes --tower-monitor`: status (pid, uptime, protocol, landing, refused messages) with Restart Tower, the Kestrels checked in, the live message log with a filter and pause, the queue and drives in use, the shared undo list. Test: `jobs`.
- [x] **LTFS tape support: no freezing, and tape-friendly behaviour** (0.3.1-alpha.11). `src/drives.cpp`: tapes (LTFS mounts) and floppies recognised from the mount table; sizes read in the background (sidebar, Overview, status bar, Properties); no previews/details/animations/dates read unless allowed per drive; one job at a time per drive; tape-order reads with "Positioning the tape"; index written after a copy and on demand; small-files warning; the Overview card; background unmount with a warning while a job runs. What's left: see "LTFS and slow drives: what's left". Test: `slow`.
- [x] **Kestrel freezes while another program copies from the tape** (0.3.1-alpha.11). Cause as suspected: the sidebar's 4-second mount check asked every mount for its size on the UI thread. Mounts now come from `/proc/self/mountinfo`, sizes from a worker. Tests: `slow`, and `ui` (no `QStorageInfo` outside the Overview's background scan).
- [x] **Copying to an LTFS tape fails with "no permission", also as administrator** (0.3.1-alpha.11). Fallback to check-then-rename when `link()` fails for lack of support (Kestrel's copy, `util::put_new`, the admin helper). Not yet confirmed on the tape itself (see what's left). Tests: `fileops`, `admin_helper` (`KESTREL_TEST_NO_LINK`).
- [x] **Opening a checksum file on a tape freezes Kestrel until the tape gets there** (0.3.1-alpha.11). The checksum file is read on a worker (also Properties → Browse…). Test: `slow` (a FIFO).
- [x] **USB floppy drives freeze Kestrel while they read** (0.3.1-alpha.11). Fixed with the same slow-drives mode as tapes; a floppy is a removable disk of at most 4 MB (or `/dev/fd*`); navigating into one no longer stats it on the UI thread. Not yet tried with the real drive (see what's left). Test: `slow`.
- [x] **The file view jumps after a drag into a subfolder** (fixed in 0.3.1-alpha.10). Cause confirmed: the current item leaving the folder made Qt scroll to its neighbour (seen in the list view). `FileView` no longer scrolls when the current item is removed. Test: `ui`.
- [x] **A folder's picture slides across the view when going back up** (fixed in 0.3.1-alpha.10). Cause: Qt starts a drag on a move with any button held; a move with the mouse's back button still down dragged the folder just left (selected again above), and the drag going nowhere slid back. Only the left button drags now, and a drag that outlives its folder doesn't fly back. Test: `ui`.
- [x] **A rare single gap in the grid** (fixed in 0.3.1-alpha.10). Two causes found: a batched layout redone started each 400-item batch on a new row (now one pass: ~20 ms for 50,000 items), and cells adding up to exactly the width pushed the last column onto the next row (one pixel of slack). Test: `ui`.
- [x] **The file chooser's Save button stays greyed out after changing folders** (fixed in 0.3.1-alpha.10). Cause as found: the button was only re-checked on name and selection changes; now on every folder and tab change too. Test: `chooser`.
