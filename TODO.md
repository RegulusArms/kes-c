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

- [ ] **Tape copies: short dips left, and fewer questions to the drive.** Copies onto an LTFS tape used to drop to
  about a quarter of the drive's speed for long stretches; the cause was programs asking the tape for its free space
  (see Done). Still open:
  - A few short dips (about 5 seconds) with no such question from any program; cause unknown.
  - Kestrel could interrupt the drive less still: before every file on a tape, `copy_file_at` (src/fileops.cpp) checks
    the free space (fstatvfs: on LTFS probably a command to the drive); once per job and counting down would do.
  - While LTFS holds the drive's SCSI generic node, its logs can be read through the drive's /dev/bsg node instead
    (sg_logs gets EBUSY on the sg one).

- [ ] **Kestrel freezes when a drive is busy with another program.** When another program loads a drive heavily
  and Kestrel does something on that drive, its window stops responding until GNOME's "not responding" (force quit)
  dialog appears. Kestrel's own jobs on that drive don't cause it.
  - Likely: something on the GUI thread waits on the drive (a stat, listing, free-space check, thumbnail or metadata
    read, file watcher), which is fast normally but blocks for seconds when the drive's queue is full.
  - To find out: which action triggers it (opening the folder, refreshing, selecting a file, the sidebar or Overview
    updating), and where the GUI thread is stuck (`gdb -p` / `eu-stack -p` on the frozen kes).

- [ ] **Double-clicking an encrypted archive whose password is in the keyring opens the Extract dialog.**
  Right-click → Extract Here on the same archive (a RAR in the case seen) just extracts it, finding the password in
  the keyring without asking, as expected; double-clicking it opens the Extract… dialog (with its Password field)
  instead.
  - Why: activating any archive calls `archive_ui::extract_dialog` (src/opening.cpp), which probes the archive and
    always shows `ExtractDialog`; Extract Here calls `extract_here` → `extract_all` with `here_options()`, whose
    password search tries the keyring before asking (src/archive_ui.cpp).
  - To decide: should double-click do what Extract Here does (for every archive, or only an encrypted one whose
    password is already saved), or keep the dialog but skip it when the keyring has the password? Update
    docs/context-menus.md / the docs if what double-click does changes.

- [ ] **Folder size jobs disappear from the Jobs window.** A "Counting the size of …" job shows in the Jobs window
  while it runs, then vanishes instead of staying under the finished jobs. Rule: no job that has appeared in the Jobs
  window should ever disappear from it (until Clear Finished).
  - Likely: the first count when Properties opens is queued quiet (`queue_job(…, quiet = !fresh)` in
    `PropertiesDialog::start_size`, src/dialogs.cpp), and a quiet job is never `TaskBoard::add`ed (src/fileops.cpp
    `queue_job`), so nothing records it in `TaskBoard::finished` when it ends; it was only visible through the queue.
  - Check every other quiet `queue_job` / `run_job` caller for the same thing, and that a job whose window
    (Properties) is closed while it runs still ends up in the finished list.

## New features

### Jobs

- [ ] **Every job as verbose as possible in the Jobs window, without slowing the job down.** Each kind of job should
  report what it's doing (the current item, counts and sizes so far, its steps, a log of what was done, skipped or
  failed) as fully as it can, through the existing progress/log reporting, throttled so the extra detail costs the
  job no measurable speed (check bulk copies and tape writes against the benchmark). Start with the jobs that
  report little or nothing now (e.g. folder size counts: only the folder's name).

- [ ] **Save every job's log to Kestrel's cache folder (opt-in).** A setting, off by default, that writes each
  finished job's log (title, outcome, times, steps and the full log the Jobs window shows) to a file in Kestrel's
  cache folder, so logs outlive Clear Finished and quitting Kestrel. Off unless the user turns it on.
  - The setting goes in src/prefs.cpp (and so Preferences); written with util `write_atomic()` or a part renamed
    into place, never half a file under a real name.
  - Decide: one file per job or one per day, and whether old logs are pruned (by age or total size).
  - A BleachBit option for it in kes-setup's `bleachbit_cleaner` (a "Job logs" option of its own, separate from the
    cache one, so clearing the cache can keep the logs or the other way round), with the dry-run expectations in
    tests/test_installer.sh. The existing Cache option deletes all of `$XDG_CACHE_HOME/kestrel-explorer`
    (walk.all), so it would take the logs too: either keep them in a subfolder the Cache option skips (it would then
    list its subfolders instead), or say in its description that it includes the job logs.

### Archives

### Drives, the sidebar and the Overview

- [ ] ~~**Friendly names for drives and removable media.** Right-click a drive (Overview card or sidebar entry) →
  **Rename in Kestrel…** to give it a name of your own, such as "Camera SD card" or "Backup HDD". Kestrel only shows
  that name; the drive's own label is never changed.~~ *Struck for now.*
  - ~~Recognised by a stable id, not by where it's mounted: the file system's UUID (GIO's `g_volume_get_uuid()` /
    `g_mount_get_uuid()`, or the volume's UUID identifier), so the name follows the drive to any port or mount point,
    also before it's mounted. Phones and cameras have no UUID: use the most stable id GIO offers for them (the
    device's serial, else its gvfs address), and say in the dialog when the name may not stick.~~
  - ~~Shown wherever the drive's name is: Overview cards, the sidebar's Devices, the path bar and tab title at the
    drive's top folder, the window title, Properties. The real label stays visible where it matters (a tooltip, or
    Properties: "Name in Kestrel: Camera SD card · Label: EOS_DIGITAL").~~
  - ~~Saved in Kestrel's config folder (a list of id → name, written whole like the others), synced to other open
    Kestrels through the tower (a new message, so bump PROTOCOL), and added to BleachBit's cleaner. **Reset Name**
    puts the drive's own name back.~~
  - ~~Two drives with the same UUID (a cloned disk): the name shows on both; say so in the dialog rather than
    guessing.~~

- [ ] **Back up a whole drive to an image file (dd), optionally encrypted with gpg.** Right-click a drive (sidebar
  Devices, Overview card) → Back Up Drive to Image…: pick where to save it, then a job copies the raw device
  (whole disk or one partition) to an image file, with progress, speed and a log in the Jobs window.
  - Reading the device needs root: through the admin session / kes-admin-helper (a new operation there, kept to
    reading block devices), not by running Kestrel as root.
  - Options: compress (zstd) and/or encrypt with a password through `gpg --symmetric`, the same way Compress…'s
    password-encrypted tar archives do it (the password on gpg's fd 3, `--no-symkey-cache`), so the image never
    exists unencrypted on disk. Later: to a recipient's key (see Encrypt / Decrypt with gpg).
  - Failure-atomic: written under a part name and renamed into place when whole; a cancel or failure deletes the
    part. Refuse to save the image onto the drive being read; check free space first (the device size, or warn when
    compressed and it can't be known).
  - Better done while the drive's file systems are unmounted or read-only; ask when one is mounted read-write.
  - Maybe later: a checksum of the image saved beside it, and Restore Image to Drive… (destructive: a strong
    confirmation naming the target drive).
  - Add the menu entries to docs/context-menus.md.

- [ ] **Open and mount disk image files (.img, maybe .iso and others), including encrypted ones.** Double-click
  or right-click → Mount Image on a `.img`: it's set up as a loop device and its partitions mounted, showing in the
  sidebar and Overview like a plugged-in drive, with Unmount (which also detaches the loop device).
  - Without root where possible: udisks2 (`udisksctl loop-setup` / the D-Bus LoopSetup, then mount each
    partition), as GNOME Files' Disk Image Mounter does; read-only by default, read-write as an option.
  - Note: drives.cpp's drive detection leaves out loop devices (around `src/drives.cpp:261`); a mounted image's
    loop device has to show anyway (but not snap's squashfs loops).
  - Encrypted images: LUKS inside the image (udisks2 Unlock, asking for the password; maybe offer to keep it in the
    keyring like archive passwords), and the gpg-encrypted images from Back Up Drive to Image (`.img.gpg`, maybe
    `.img.zst.gpg`): these have to be decrypted first, to a file in the cache (as big as the drive: check free space,
    ask first, delete it on unmount) or streamed where the format allows; decide which.
  - Also decide: compressed images (`.img.zst`, `.img.xz`), and which other formats (.iso currently opens as an
    archive through 7-Zip; keep that as Open as Archive and add Mount).
  - Add the menu entries to docs/context-menus.md.

### Views and search

### File tools

- [ ] ~~**Find similar images** (the part of Find Duplicates left for later): the same photo at another size or
  quality, not identical bytes, e.g. by a perceptual hash of each image's thumbnail, grouped in the Find Duplicates
  window.~~ *Struck for now.*

- [ ] **Encrypt / Decrypt with gpg** (left for later from Verify Signature): to a recipient's key or with a password.

- [ ] **A window for making, viewing and editing links.** One "Link…" window instead of (or ahead of) the many
  Links & Shortcuts ▸ entries. Layout (the user's design):
  - Two browsers side by side, each like the file chooser (navigable: path bar, sidebar places, folders):
    - Left: the source, what the links will point to. Pick one item, or select several.
    - Right: where the links will be made. Navigate to the folder; the left side's picks show there as previews of
      the links-to-be (ghosted, among the folder's real items), each one renamable in place.
    - (Naming in the UI: say "Points to" / "Link goes in", not "source" / "target": in symlink terms the *target* is
      what the link points to, i.e. the left side, which would confuse.)
  - Options for the kind: symbolic, relative symbolic, hard link (files on the same drive only), maybe `.desktop`
    shortcut.
  - Below both browsers: a list of all the uncommitted links (pending ones: new links and edits), each showing its
    name, folder, what it points to and the kind; problems flagged (name taken, broken target, hard link across
    drives). Clicking one sets the left side to what it points to and the right side to its folder, selected, to
    change it; a Remove button drops it. More links can be added to the list from other folders before committing.
  - OK shows a summary of everything that will be made or changed (new links, changed targets, renames), with a
    final OK; only then are the links made or edited, as one job (with a log in the Jobs window), the whole batch
    undoable.
  - Right-click a symbolic link → Edit Link…: the same window opened with that link already in the pending list
    (left side at what it points to, or showing it's broken; right side at its folder), to change what it points to,
    switch absolute/relative, or rename it. Selecting several links → edit them all at once. Properties' "Link
    target:" row (src/dialogs.cpp) could have an Edit button that opens it.
  - Changing a link's target is failure-atomic: a new link made under a part name and renamed over the old one
    (never delete-then-create), as the rest of Kestrel's writes; undoable like a rename. A new link claims its name
    without replacing anything (made under a part name, RENAME_NOREPLACE). Everything is checked again at the final
    OK (a name taken or a target gone meanwhile is shown, not overwritten).
  - The window is a free window (show_free), and its two browsers reuse the file chooser's widgets rather than
    copying them.
  - Hard links: show the link count and, maybe, the other names of the same file on that drive (a search; slow on
    big drives, so a job).
  - Decide which of the current menu entries stay (the quick ones like Create Symbolic Link Here and Send Link to
    Desktop are one-click; maybe keep those and replace Create Symbolic Link In… and the shortcut ones with the
    window), keeping menus::defaults() and test_prefs in step, and update docs/context-menus.md.

### Images, video and audio

- [ ] ~~**Image viewer: what's left of the quality-of-life ideas** (the rest is done, 0.3.2-alpha.7). Not chosen
  then:~~ *Struck for now.*
  - ~~**Rate** images (stars 1–5, as XMP Rating), next to star / pin.~~
  - ~~**Video and animation:** GIF/WebP already animate; play short videos in place (or hand them to the default
    app), with play/pause and a seek bar.~~
  - ~~**Background:** choose black, grey, the theme's colour, or a checkerboard for transparency.~~

### Desktops

- [ ] **Kubuntu 26.04 (KDE Plasma): what only a real Plasma session shows.** The rest is done (0.3.2-alpha.33, under
  Done). Still to try in a Kubuntu VM: archive passwords saved in KWallet (Secret Service), network shares (`smb://`,
  `sftp://`) and phones (GIO's volume monitor; the .deb's Recommends bring gvfs-backends), drop focus on Wayland
  (dragging into Firefox should give it the focus: the KWin script), `kes-setup --dock` with Kubuntu's own taskbar
  pins, Set as Wallpaper, and Recent Files' "Do not remember".
  - **Search index (optional):** `index_covers()` only knows Tracker/localsearch. KDE uses Baloo (folders in
    `~/.config/baloofilerc`, `baloosearch`). Without it, Kestrel uses its own search, which is fine.

### Translations

### Packaging

- [ ] **Show Kestrel in the software centres (AppStream).** Once a user has added Kestrel's apt repo, Kestrel should
  appear as a proper app (name, icon, summary, screenshots, Install button, updates) in GNOME Software, KDE Discover
  (Kubuntu), Ubuntu's App Center (search with the "Debian packages" filter) and Mint's Software Manager. Today
  Discover and GNOME Software don't show it at all (no AppStream data), and Mint's Software Manager lists it as a bare
  package. Users who haven't added the repo still won't see it; that would need Debian/universe (below).
  - **data/kestrel-explorer.metainfo.xml**, installed to `/usr/share/metainfo/` (CMakeLists.txt, also by
    install.sh): id `kestrel-explorer`, name, summary, description, license, `<launchable>` →
    `kestrel-explorer.desktop`, screenshot URLs (hosted on gh-pages), `<releases>`. Kept valid with
    `appstreamcli validate` (a check in the tests), and in tests/check_package.sh (the .deb ships it).
  - **DEP-11 metadata in the apt repo:** reprepro doesn't make it. CI's apt job generates
    `dists/<suite>/main/dep11/Components-amd64.yml.gz` and the icon tarballs for every suite (appstream-generator or
    a small script) and lists them in the signed `Release` file. apt only downloads them where a software centre asks,
    so nothing changes for other users.
  - Check on Kubuntu (the kes-kubuntu image or a VM: Discover), Ubuntu (App Center, GNOME Software) and Mint 22
    (Software Manager: how much of the AppStream data it uses).
  - **Not chosen for now:** a strictly confined snap (no hidden files at the top of `~`, no files outside `~` and
    the removable-media mounts, no admin helper, no host tools, no portal or FileManager1 role, settings under
    `~/snap/`): a crippled Kestrel. A classic-confinement snap needs the Snap Store's approval, which file managers
    may not get. Debian (→ Ubuntu universe, so every user sees it with no setup) needs Debian policy packaging, an
    ITP and a sponsor, and Debian's release pace: worth it after a stable 1.0, not during the alpha.

### Tests and CI — beta release blocker

*Everything in this section is to be done before the first beta.*

- [ ] **A tool check: which external tools work, in its own CI job.** Kestrel wraps the standard command-line tools
  (7-Zip, unrar/rar, zpaq, tar's compressors, gpg, rsync, ffmpeg/ffprobe, exiftool, jpegtran, heif-enc, ripgrep,
  BleachBit…), and the test suite skips a tool's checks when it isn't installed, so CI only covers the tools it
  happens to have (unrar, rar, zpaq, ripgrep, pigz, the parallel compressors and heif-enc aren't installed; zstd, xz
  and bzip2 only come with GitHub's runner image). A separate job installs every tool Kestrel can use, runs each of
  them through Kestrel's own code, and reports what works and what doesn't.
  - Its own job (or workflow), run on its own: nothing waits for it, and a failure there never stops the build, the
    package, the release or the apt update.
  - A report of each tool: installed or not, its version, and works / broken / missing, with the error for a broken
    one (in the run's summary page, and kept as an artifact).
  - Both 7-Zips: `7zip` (what the docs recommend) and the older `p7zip-full` (what the test jobs install now).
  - On ubuntu:24.04 and 26.04 containers (pulled through mirror.gcr.io), so a newer tool that behaves differently
    shows up before users find it.
  - Also on a schedule (e.g. weekly), not only on pushes: tools change when the distro updates them, not when
    Kestrel does.

## Done
