# To do

← Back to [the README](README.md) · [Changelog](CHANGELOG.md) · [Bugs found elsewhere](docs/bugs-elsewhere.md)

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

## New features

### Jobs

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

- [ ] **Quick Look for every kind of file.** Space opens the image viewer for images, but Properties for anything
  else (`MainWindow::quick_view`, src/opening.cpp). Give other files a preview window too, opened and closed with
  Space, with arrow keys to step through the folder's files:
  - Text and code: with line numbers (maybe syntax colours); large files read only in part.
  - PDFs: the first pages, through poppler (a new optional dependency: the .deb's Suggests, kes-setup's
    OPTIONAL_PACKAGES and docs/dependencies.md).
  - Audio and video: play, pause and seek (Qt Multimedia, or ffmpeg already used for thumbnails). This also covers
    the struck viewer idea of playing short videos in place.
  - Archives: their contents, reusing Open as Archive's listing (src/archivewin.cpp).
  - Fonts: a sample of the font.
  - Anything else: what Properties' General tab shows, as now.
  - A free window (show_free); never reads files on a tape or floppy without the drive's previews allowed, as with
    thumbnails.

### File tools

- [ ] ~~**Find similar images** (the part of Find Duplicates left for later): the same photo at another size or
  quality, not identical bytes, e.g. by a perceptual hash of each image's thumbnail, grouped in the Find Duplicates
  window.~~ *Struck for now.*

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

- [ ] **Check the file-safety guarantees, path by path.** Go through every write path (copy, move within and between
  drives, duplicate, paste, extract, Extract All, compress, decompressing a single file, convert, Wipe Metadata,
  rotate / image edits, checksum files, links, the admin helper's operations, tapes and floppies) and verify each of
  these, with a test that fails when it's broken:
  1. **No clobber:** a file that appears at the destination after a copy or extraction starts is never silently
     overwritten (another program saving there meanwhile).
  2. **Replace only when chosen:** explicitly choosing Replace is the only reason an existing destination gets
     replaced (not Merge, Skip, Keep Both, a retry, an admin retry or a default).
  3. **Cancellation:** cancelling never leaves a destination looking like a successfully completed file (no
     half-written file under a real name; the tape exception deletes its file).
  4. **Partial failures:** a failed copy or extraction (disk full, RLIMIT_FSIZE, a read error, the tool failing)
     leaves a predictable, recoverable state: what finished is whole, the rest absent, the job's log says which.
  5. **Folder merging:** an existing folder's contents survive errors and cancellation during a merge.
  6. **Undo:** Undo never acts on a file another application has replaced since the original operation (a different
     inode, size or time than what was recorded): it leaves that file alone and says so.
  - Note what's found that doesn't hold, fix it, and add the checks to tests/CHANGELOG.md.

- [ ] **Security review of the admin helper (kes-admin-helper).** It runs as root (through pkexec) and does what
  Kestrel's JSON requests on its stdin ask (src/admin_helper.cpp, its header comment lists the ops and the safety
  rules), so it's one of the most sensitive parts. Check, and cover with tests that fail when it's broken:
  1. **Validation before privileged execution:** every request's fields (op, id, paths, mode, text, merge) are
     checked for type and range before anything is done; a malformed, unknown or oversized request is refused, never
     half-run.
  2. **Paths and file names passed safely:** no shell anywhere; names with newlines, leading `-`, `..`, empty
     components, invalid UTF-8 or very long paths can't escape the walk or be misread (JSON encoding both ways).
  3. **Symlinks revalidated at execution:** the walk-from-`/` without following symlinks holds at the moment of each
     operation, not only when the request arrives (swapping a folder for a symlink mid-copy, mid-delete or between
     two requests); the "root-controlled link" rule can't be met by a user's link.
  4. **Only operations the GUI is authorized to request:** the op list and POLICY are the whole surface; ops that
     act for the user (hardlink "only the user's own files") know which user that is (pkexec's `PKEXEC_UID`), and
     nothing lets a request read or change files outside what the policy allows (e.g. `write`/`chmod` on
     `/etc/shadow`, a protected folder or a home folder itself).
  5. **Temporary files and intermediate destinations:** the helper's part files and folders built aside are created
     with safe owner and permissions (not readable or writable by others while being written, no window where a
     setuid bit or a user's ownership is live on a root-owned copy), and can't be pre-created or replaced by the
     user.
  6. **Cancellation and partial failures:** behave as in Kestrel's own jobs (see Check the file-safety guarantees):
     a cancel or failure removes only the part, leaves nothing half-made under a real name, and the reply says what
     was done.
  - Also: the helper exits when stdin closes, and an admin session can't be driven by another process.
  - Note the findings, fix them, and add the checks to tests/CHANGELOG.md.

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

## Done

- [x] **A folder whose own name isn't valid UTF-8 shows empty, and admin jobs lose such names.** Since
  0.3.3-alpha.1 Kestrel's own file code keeps such names byte for byte, but two places still go through text:
  - The folder view is Qt's QFileSystemModel, which decodes names lossily (U+FFFD) and opens folders by its own
    encoding of them: going into a folder whose name has a stray byte lists nothing. Files with such names inside an
    ordinary folder work (`enc()` finds them). Fixing it means a model that lists through `dec()`.
  - The admin session (src/admin.cpp → kes-admin-helper) sends paths as JSON text, where a stray byte's lone
    surrogate doesn't survive: Retry as Administrator on such a name fails. Send the bytes (e.g. base64) instead.
  Done in 0.3.3-alpha.12: the folder view's model is Kestrel's own (src/fsmodel.cpp, FSModel): one top-level row,
  the folder, with its items as children, listed by bytes on a thread in batches, changes through one shared inotify
  instance (only the names that changed are looked at again), sorted as QFileSystemModel did (folders first, natural
  order, mirrored when descending). The admin session sends each path as `<key>_b64` beside the text (and a
  symlink's target); the helper uses the bytes and refuses a NUL in them. A drag out of a folder view carries the
  bytes in its URLs and a drop onto one reads them byte for byte. Left: other drop targets (the sidebar, the window)
  still use QUrl::toLocalFile(), and the location bar's completer is still Qt's QFileSystemModel: both give such a
  name with U+FFFD, which enc() finds when it's the only one in its folder that reads that way.

- [x] **Verifying checksums stops when the Kestrel window that started it is closed.** It should be like any other
  job: it carries on in the Jobs window after its window has gone, and its result is still there to see.
  - Why: the Verify window (VerifyDialog, src/hashcheck.cpp) runs the check as one job per file, starting the next
    when one finishes, and it's that window that keeps the list and moves on. It belongs to the Kestrel window it was
    opened from, so closing that window deletes it, and its destructor (`stop()`) cancels the job that's running and
    nothing starts the rest.
  - Fix: one job for the whole checksum file (every entry, in order), not tied to any window (queue_job with
    outlives_parent, as other jobs are), with each file's result in its log and its list of failures. The Verify
    window then only shows that job's progress and results while it's open; closing it or its Kestrel window leaves
    the job running.
  Done in 0.3.3-alpha.12: hashcheck::verify_job (one queued job, outlives_parent, steps per file, failures listed at
  the end of its log and "done with errors"); VerifyDialog only shows its progress and results, and no longer stops it
  when it's closed or hidden.

- [x] **Check that every job runs on its own, whatever happens to the Kestrel window that started it.** Closing
  that window (or the tool window it came from) mustn't stop a job, lose its result, or crash when it finishes. The
  job goes on in the Jobs window, its log and failures are kept there, and it keeps Kestrel running until it ends, as
  copies and moves already do. Found with Verify Checksums (above).
  - Go through every `fileops::queue_job` / `run_job` / `run_task` / `start_ops` call: what it's parented to, whether
    its `on_done` needs that window (and `outlives_parent`), and whether a window drives a series of jobs itself
    (starting the next as one ends), as Verify Checksums does.
  - Windows that cancel their job when closed or deleted, so they're deliberate choices to keep or change: Find
    Duplicates (`DupesWindow`), Disk Usage (`DiskUsageWindow`) and Compare With… (`CompareWindow`) stop their scans,
    whose results show only in that window; Sync To… (`SyncWindow`) stops only its preview, and a sync goes on. A scan
    that's only useful in its window may be fine to stop, but closing the Kestrel window behind it shouldn't stop it
    while the tool window stays open.
  - Add a check for each kind of job in the tests (close the window that started it part-way, the job still ends
    and its result is in the Jobs window).
  Done in 0.3.3-alpha.12. Changed: run_job and run_task got outlives_parent too; Move to Trash, Restore, Apply Sort to
  Subfolders, Undo, Delete Thumbnails, making thumbnails (MainWindow's thumbnail job), Write Tape Index, Start Admin
  Session and the admin retry now finish what they do after the job (undo entries, settings, caches, messages) also
  when their window has gone. Find Duplicates, Disk Usage, Compare With… and Sync To… have no parent now (they stay
  open when their Kestrel window closes; closing the tool window itself still stops its scan, by choice; a sync
  starts also without its Kestrel window). Mount Image's Mounter has no parent (it drove a series of tasks and was
  deleted with its window part-way, leaving a loop device set up), its tasks deliver to it with outlives_parent and
  its questions have no parent. Left as they are: on_done callbacks that only tell their window (a status-bar
  message, selecting what was made, a result shown in a window that has gone): the job's log in the Jobs window has
  the result; Extract / Test (say_one/say_all) show their summary only while the window is open. Tests (`jobs`):
  Verify, the four tool windows, Trash, Apply Sort to Subfolders; not tested: Mount Image (needs udisks and a loop
  device), the admin retry (pkexec), Delete Thumbnails, Write Tape Index.

- [x] **Keep extended attributes and ACLs when copying (opt-in).** A copy now keeps only the file's times and permissions
  (`futimens` / `fchmod` in src/fileops.cpp), so it silently drops its extended attributes: the address a browser
  saved a download from (`user.xdg.origin.url`), tags other apps keep (`user.xdg.tags`, KDE's), ACLs
  (`system.posix_acl_*`), security labels, and attributes backup tools rely on.
  - Copy them onto the part file before the rename (`flistxattr` / `fgetxattr` / `fsetxattr`), for folders too, and
    in the admin helper's copy.
  - Off by default: a setting the user can turn on (src/prefs.cpp, so Preferences → Files). While it's on, where the
    destination can't hold them (FAT, exFAT, some FUSE mounts, a tape), the copy goes on and its log says which
    were left out, once per job rather than per file.
  - Skip what the user can't set (`security.*`, `trusted.*` without root) quietly.
  - A test that copies a file with a `user.` attribute (and an ACL where the test's file system allows) and checks
    it arrived.
  Done in 0.3.3-alpha.9 (src/xattrs.h, used by src/fileops.cpp and src/admin_helper.cpp; "copy_xattrs"): set after the
  times and mode (in the helper after the owner, which a chown would clear security.capability for); the helper tells
  Kestrel what was left out in its answer. Not tested: a real FAT destination (a hook stands in for one).

- [x] **Verify after copy (opt-in).** An option, off by default, to read each copied file back once it's written and
  compare its checksum with the source's, as TeraCopy does; mismatches are listed in the job's log and its summary.
  - Set per job in the Jobs window (and a default in Preferences → Files, off); most useful for tapes, where
    checking really matters.
  - Hash the source while copying (no second read of it), then read the copy back; on a tape, after the whole job,
    in the order the files lie on it, so the drive doesn't wind back and forth.
  - Maybe: write a `SHA256SUMS` (or the chosen algorithms, as Create Checksum File… does) beside the copy, so it can
    be checked again later with Verify Checksums.
  - Decide what a mismatch does: report only, or offer to copy that file again.
  Done in 0.3.3-alpha.9 (src/fileops.cpp, Ops; "copy_verify", Task::verify, the Jobs window's Verify copies): SHA-256,
  the copy's pages written out and dropped from the cache before it's read back (not on a tape: an fsync there writes
  LTFS's index). A mismatch is reported only (the log, the job's failures; a move keeps its original), not copied
  again. Not yet: the SHA256SUMS beside the copy; changing it for another Kestrel's job (would need a tower message);
  verifying copies made as administrator; a test on a tape.

- [x] **Encrypt / Decrypt with gpg** (left for later from Verify Signature): to a recipient's key or with a password.
  Done in 0.3.3-alpha.9 (src/pgp.cpp): right-click → Encrypt… (files, one or several) with a password or to keys from
  the keyring (each marked verified or not, your own added), as .gpg or .asc; Decrypt (and double-clicking an
  encrypted .gpg, .pgp or .asc) asks for the password (the keyring's first), or lets gpg's agent use your key, or says
  it's someone else's. Not yet: encrypting folders (Compress… with a password does that), signing while encrypting,
  and Back Up Drive to Image to a recipient's key.

- [x] **Sync To… remembers past syncs and fills itself in.** Once a folder has been synced, Sync To… on that folder
  again (from either end: the source or the destination) opens with the other folder and the choices used last time
  (delete what's not in the source, rsync or Kestrel's copy) already filled in, so only Preview → Sync Now is left.
  - Today a pair is only kept if you press Save Pair… and give it a name, and the window doesn't look at the saved
    pairs when it opens: it starts with just the folder you right-clicked.
  - Remember each sync that ran (or the last few per folder) automatically, without naming it; the saved pairs
    stay as they are. Several past partners for one folder → fill in the latest, the others a click away.
  - Opened from the destination end: fill it in the right way round (that folder as To:, the source as From:), not
    swapped, so a sync can't be run backwards by accident.
  - A remembered folder that's gone (deleted, drive not mounted) isn't filled in silently: say so.
  - It's a new list of paths kept in the config folder: add it to kes-setup's BleachBit cleaner, and to prefs.cpp's
    STATE list if it's kept that way.
  Done in 0.3.3-alpha.9 (src/compare.cpp): sync-history.json, the last five per folder, kept when a sync starts;
  Last synced lists the folder's others. Not in Preferences' STATE list: it's a file of its own, not a setting.

- [x] **Back up a whole drive to an image file (dd), optionally encrypted with gpg.** Right-click a drive (sidebar
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
  Done in 0.3.3-alpha.6 (src/diskimage.cpp): the admin helper's new readdev op (block devices only, into a FIFO of
  the user's) streams the device; zstd and gpg run as the user. Not yet: a checksum beside the image, Restore Image
  to Drive…, encrypting to a recipient's key. Only the device itself is offered, or its disk (not what a LUKS/LVM
  device sits on).

- [x] **Open and mount disk image files (.img, maybe .iso and others), including encrypted ones.** Double-click
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
  Done in 0.3.3-alpha.6 (src/diskimage.cpp) with udisksctl (loop-setup -r, mount, unlock with the password on
  stdin, autoclear set through gdbus). Decided: .img, .iso, .raw, each also .zst/.xz/.gz and .gpg; compressed or
  gpg-encrypted ones are expanded into ~/.cache/kestrel-explorer/images (asked first; streaming can't work, a loop
  device reads anywhere), mounted read-only, deleted once unmounted (MountWatch → diskimage::tidy, which also locks
  LUKS and detaches the loop). Double-click mounts an .img/.raw; an .iso still opens as an archive (Mount Image in its
  menu).

- [x] **A window for making, viewing and editing links.** One "Link…" window instead of (or ahead of) the many
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
  Done in 0.3.3-alpha.6: Links & Shortcuts → Link… and Edit Link… (src/linkwin.cpp, src/links.cpp), and Edit… beside
  Properties' link target. The two browsers are chooser-mode Kestrel windows embedded (MainWindow::embed). The
  links-to-be show in a ghosted strip under the right-hand browser (renamable in place), not mixed into the folder's
  own items. Kept one-click: Create (Relative) Symbolic Link Here, Create Hard Link Here, Send Link to Desktop;
  replaced by the window: Create Symbolic Link In…, Create Desktop Shortcut (.desktop) Here, Send Shortcut (.desktop)
  to Desktop. Creating as administrator is offered for new links only (not edits); admin-made ones aren't undoable.


- [x] **Faster copies: let the kernel clone or copy the file.** Kestrel's copy reads and writes every byte itself
  (the read/write loop in `copy_file_at`, src/fileops.cpp), never using the kernel's own copy calls.
  - Clone first (`ioctl(FICLONE)`): on Btrfs, XFS, bcachefs and ZFS 2.2 or later a copy on the same drive is then
    instant and takes no extra space until one side changes.
  - Else `copy_file_range()`: SMB3 and NFS 4.2 servers copy the file themselves (server-side copy), so the data
    doesn't cross the network twice; elsewhere it's an in-kernel copy, with no trip through Kestrel's buffers.
  - Fall back to the current loop when neither works (EXDEV, EOPNOTSUPP, EINVAL…), and keep it for tapes and their
    buffer (src/tapebuf.cpp).
  - Still failure-atomic: clone or copy into the part file, then rename it into place as now. Progress, pause and
    cancel have to keep working: `copy_file_range` in chunks; a clone is one call (no progress needed).
  - GNOME Files (GLib) already does both: add a same-drive copy on Btrfs/XFS and a copy on an SMB share to the
    benchmark (bench/), before and after, in docs/performance.md.
  - A test for each path (clone, copy_file_range, the fallback) that fails part-way, as the other write paths have.
  Done in 0.3.3-alpha.4, as a setting (Preferences → Files → Copying, `copy_kernel`, on by default): clone, then copy_file_range in 4 MB pieces, then the loop; files under 64 KB (not on Btrfs) and tapes use the loop; a clone or copy_file_range found unsupported between two drives isn't tried again in that job. Benchmark: bench/fs.sh (Btrfs, XFS, SMB in a privileged container), results in docs/performance.md. The admin helper's copy (retry as administrator) still uses its own loop.

- [x] **Every job as verbose as possible in the Jobs window, without slowing the job down.** Each kind of job should
  report what it's doing (the current item, counts and sizes so far, its steps, a log of what was done, skipped or
  failed) as fully as it can, through the existing progress/log reporting, throttled so the extra detail costs the
  job no measurable speed (check bulk copies and tape writes against the benchmark). Start with the jobs that
  report little or nothing now (e.g. folder size counts: only the folder's name).
  Done in 0.3.3-alpha.4: the quiet jobs log each item (trash, restore, undo, size counts, checksums, duplicates, compare, previews, signature, archive listing, admin), copies/checksums/duplicates show their speed, a quiet log gets the progress every 15 s, and every log ends with how it ended and how long it took. Progress texts are built only when due: the benchmark's copy went from 1.14 s to 1.02 s (median of 5). Tape writes not measured (no tape here); their only change is the same throttled progress text.

- [x] **Save every job's log to Kestrel's cache folder (opt-in).** A setting, off by default, that writes each
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
  Done in 0.3.3-alpha.4 (src/joblogs.cpp): a file per job in job-logs/<day>/, written as a part renamed into place; kept 90 days and 100 MB (oldest days first, never today's). BleachBit: Cache skips job-logs (nwholeregex), a Job logs option empties it.

- [x] **Disk usage map: what's using this space?** A window showing what fills a folder or a drive as a treemap or
  a sunburst (decide which, or offer both), like Baobab or Filelight: each folder a block or ring sized by what it
  holds, its biggest files and subfolders inside it; click one to go into it, back to go up, hover for its size and
  share.
  - Opened from a drive's Overview card ("What's using this space?", beside its usage bar), the sidebar's drives,
    and right-click a folder → Disk Usage….
  - Counting is a job (the queue, its turn on the drive, progress, Pause / Stop) that stays on the drive, as Find
    Duplicates does. It reuses the folder sizes Properties remembers where they're still current, shows them at
    once with when they were counted, and saves what it counts for Properties in turn; Recalculate counts again.
  - From the map: open or show an item in its folder, Move to Trash (Undo as usual), Properties.
  - Not on tapes and floppies unless asked (counting walks the whole drive).
  - Add the menu entries to docs/context-menus.md.
  Done in 0.3.3-alpha.4 as a treemap (src/diskusage.cpp), with a list of the folder shown beside it: a folder's small files are one block, folders under 128 KB keep only their totals. The remembered sizes show at once while it counts again (no count is skipped: the map needs every level).

- [x] **Kestrel freezes when a drive is busy with another program.** When another program loads a drive heavily
  and Kestrel does something on that drive, its window stops responding until GNOME's "not responding" (force quit)
  dialog appears. Kestrel's own jobs on that drive don't cause it.
  Fixed in 0.3.3-alpha.3: with every stat on an ordinary folder made to wait (the tests' busy drive), the window waited in `Pane::set_path` (an isfile and isdir of the folder opened), `Pane::refresh` (an isdir of every item, for the folder previews) and `build_menu` / Send To (isdir/isfile/islink of the selection). The menu and refresh now use what the view knows (`selected_known`, KindRole), `set_path` waits at most 300 ms for its checks (on a thread; what they find later is dealt with then), and search results and the trash/Starred/Recent lists are stat'ed on their threads. The Info Panel (which stat'ed, listed and read the metadata of what it shows on the UI thread, except on tapes) now reads it on its worker for every drive. Anything inside Qt (QFileSystemModel's own stats of a new path) isn't covered: if it happens again, `eu-stack -p` on the frozen kes.
  - Likely: something on the GUI thread waits on the drive (a stat, listing, free-space check, thumbnail or metadata
    read, file watcher), which is fast normally but blocks for seconds when the drive's queue is full.
  - To find out: which action triggers it (opening the folder, refreshing, selecting a file, the sidebar or Overview
    updating), and where the GUI thread is stuck (`gdb -p` / `eu-stack -p` on the frozen kes).

- [x] **Double-clicking an encrypted archive whose password is in the keyring opens the Extract dialog.**
  Right-click → Extract Here on the same archive (a RAR in the case seen) just extracts it, finding the password in
  the keyring without asking, as expected; double-clicking it opens the Extract… dialog (with its Password field)
  instead.
  Fixed in 0.3.3-alpha.3: double-click (`archive_ui::open_archive`) extracts an encrypted archive with a password saved for it as Extract Here does; any other archive still gets the dialog.
  - Why: activating any archive calls `archive_ui::extract_dialog` (src/opening.cpp), which probes the archive and
    always shows `ExtractDialog`; Extract Here calls `extract_here` → `extract_all` with `here_options()`, whose
    password search tries the keyring before asking (src/archive_ui.cpp).
  - To decide: should double-click do what Extract Here does (for every archive, or only an encrypted one whose
    password is already saved), or keep the dialog but skip it when the keyring has the password? Update
    docs/context-menus.md / the docs if what double-click does changes.

- [x] **Folder size jobs disappear from the Jobs window.** A "Counting the size of …" job shows in the Jobs window
  while it runs, then vanishes instead of staying under the finished jobs. Rule: no job that has appeared in the Jobs
  window should ever disappear from it (until Clear Finished).
  Fixed in 0.3.3-alpha.3: `queue_job` records a quiet job (`TaskBoard::record`), so it's kept among the finished ones without showing in the status bars; every quiet queued job had the same problem (the fix covers them all). A job whose window has no task panel is added to the board too.
  - Likely: the first count when Properties opens is queued quiet (`queue_job(…, quiet = !fresh)` in
    `PropertiesDialog::start_size`, src/dialogs.cpp), and a quiet job is never `TaskBoard::add`ed (src/fileops.cpp
    `queue_job`), so nothing records it in `TaskBoard::finished` when it ends; it was only visible through the queue.
  - Check every other quiet `queue_job` / `run_job` caller for the same thing, and that a job whose window
    (Properties) is closed while it runs still ends up in the finished list.

- [x] **Two right-click menu texts aren't translated.** In the menu layouts (src/menus.cpp `defaults()`), View
  Image's note "for images" (around line 156) and "Rotate 180°" in the Rotate submenu (around line 212) are plain
  literals, not `tx()`, so Preferences → Menus shows them in English in every language. Wrap them in `tx()`, run
  translations/update.sh, and find out why the `ui` / `i18n` tests didn't catch them (a check that does, in
  tests/CHANGELOG.md).
  Fixed in 0.3.3-alpha.3. The `i18n` test missed them because it read only a tree's first 50 top-level rows (Rotate 180° is inside a submenu) and only the first menu kind in Preferences → Menus; it now reads every row of every kind.

- [x] **Deleted items stay in the search results.** Search, then delete (or trash) some or all of the results: they
  go, but their rows stay in the results as if they were still there. They should disappear from the results as soon
  as they're deleted.
  Fixed in 0.3.3-alpha.3: SearchModel drops a row whose file has gone (kept aside, so it comes back with Undo or a restore) and follows renames and moves Kestrel makes (`undo::Signals::moved`). Checked on a thread when a results folder changes (a watcher on up to 1000 of them), a job ends, or Kestrel comes back to the front.
  - The results are their own list (SearchModel, src/widgets.h), not the folder view, so they don't follow changes
    on disk: rows are only ever added, or all cleared for a new search. It needs a way to drop rows (and, for a
    rename or a move, update them), called when a job deletes, trashes, renames or moves a result, and ideally also
    when another program deletes one (a watcher on the results' folders, or a check when the window comes back).
  - Undo (bringing a trashed result back) should put its row back, and the result count shown should stay right.

- [x] **Files whose names aren't valid UTF-8 can't be deleted (or copied, moved…): "No such file or directory".**
  Fixed in 0.3.3-alpha.1: `enc()`/`dec()` keep stray bytes as lone surrogates (U+DC80–U+DCFF), and `enc()` finds a name Qt decoded with U+FFFD when one entry of its folder matches.
  Seen deleting a trashed folder from an old DOS-era archive holding a file named `…:\xb3SUBJECT` (a Latin-1/CP437
  byte): Delete Permanently stops at that file with `[Errno 2] No such file or directory`, and everything above it
  stays (its folders aren't empty).
  - Why: names go through QString, `enc()`/`dec()` (src/util.cpp) being `QFile::encodeName`/`decodeName`. Under a
    UTF-8 locale the stray byte becomes U+FFFD and encodes back as different bytes, so `names_in` → `stat_at` in
    `Ops::remove_at` (src/fileops.cpp) asks for a name that isn't there.
  - Likely everywhere a name read from a folder is used again: copy, move, delete, rename, listing, thumbnails,
    the admin helper. Needs names that keep their bytes (e.g. listing as QByteArray and carrying the raw name to the
    *at() calls, or a lossless decode such as Python's surrogateescape), plus a test with such a name in a tree.

- [x] **A failed Delete Permanently in the trash still deletes the item's .trashinfo.** `delete_now`'s done callback
  Fixed in 0.3.3-alpha.1, Empty Trash too (each record goes after its item).
  (src/actions.cpp) unlinks the `.trashinfo` of every item it was given, whether or not that item was actually
  deleted. After a failure (the bug above) the item stays in `files/` with no record: the Trash shows its location
  as "(unknown)" and Restore can't put it back. Only remove the record of an item that's gone (`!lexists(p)`).

- [x] **Deleting a trashed item that has no .trashinfo crashes Kestrel.** Delete Permanently on such an item (e.g.
  Fixed in 0.3.3-alpha.1: util.h declares the C library's versions, which also fixed undo.cpp's `::rmdir` (a crash undoing New Folder with Selection with something else in the folder), and animate.cpp's and dupes.cpp's `::unlink`.
  left by the bug above) aborts: `terminate called after throwing an instance of 'OSError'` … `No such file or
  directory: '…/Trash/info/<name>.trashinfo'`.
  - Why: the same callback calls `::unlink(enc(info_path).constData())`, meant as libc's, but src/actions.cpp
    doesn't include `<unistd.h>` and has `using namespace util`, so `::unlink` resolves to `util::unlink(const
    QString &)`, which throws on ENOENT; the throw is in a main-thread callback with no catch.
  - Fix: include `<unistd.h>` (or call util::unlink in a try / check first), and look for other `::` calls in files
    with `using namespace util` that may resolve to the throwing util versions the same way. Test: delete a trashed
    item whose `.trashinfo` is missing.
