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

- [ ] **Startup is about 50 ms behind GNOME Files in the Ubuntu benchmark.** The 2026-11-05 benchmark shows startup at
  0.50 s against GNOME Files' 0.44 s, where the one before showed 0.37 s for both. Investigated (2026-11-05):
  - Not a regression in Kestrel. 0.3.2-alpha.34 (the version behind the 0.37 s) and 0.3.3-alpha.13, both timed with
    the same bench.py in the benchmark's own image: 0.485 / 0.496 s against 0.499 / 0.493 s (two rounds of 11 launches
    each). Launched straight on an Xvfb on the host: 0.226 s against 0.223 s (25 launches each).
  - The jump comes from the benchmark itself: it now runs in an Ubuntu 26.04 container instead of on the host, and
    every app got slower there (GNOME Files 0.37 → 0.44 s, Dolphin 0.40 → 0.64, Thunar 0.44 → 0.59, Krusader 0.93 →
    1.27, PCManFM 0.41 → 0.51). The new run is the one to compare against from now on.
  - What's left is the gap to GNOME Files, which only shows under the benchmark's conditions: in the same image on a
    full session bus (portals, accessibility bus) both started in 0.46–0.47 s.
  - About half the gap is Qt's GTK integration (the benchmark's desktop is set to GNOME, so Qt loads it): with
    `QT_QPA_PLATFORMTHEME=gnome`, Kestrel starts in 0.46–0.47 s instead of 0.49 s. A trace shows the main thread
    reading Yaru's gtk.css and gdk-pixbuf's loaders, then waiting until Pango has set up fontconfig in a background
    thread before it can draw any text. GNOME Files does the same font work, so the wait itself isn't the gap.
  - Next: time Kestrel's own steps from main() to the first window shown under the benchmark's conditions (the private
    bus with only dconf and gvfs, gvfs already running, `GTK_A11Y=none`/`NO_AT_BRIDGE=1`), to find the rest of the
    gap. Also check whether anything the GTK integration loads can wait until after the window is shown.

- [ ] **A drag into a Brave page's "drop files here" box sometimes fails on the first try.** Dragging a file from
  Kestrel onto a page's upload box often didn't work the first time; the second or third try did. Dropping onto an
  empty Brave window (Brave opens the file) always worked. GNOME on Wayland, Brave running as a native Wayland app.
  - Seen 2026-11-06. While it was being looked at, it stopped happening, in the window that had shown it too, with
    nothing changed in Kestrel. Not reproduced since.
  - A Wayland log of three working drags (`WAYLAND_DEBUG=client`): Kestrel offers `text/uri-list` (copy and move),
    Brave asks for the data, the drop completes. Kestrel does the same in every drag, so nothing on its side explains
    one try failing and the next working.
  - Suspect (unconfirmed): while the pointer is over the page, Brave keeps telling GNOME which action it accepts as
    the page answers, and the log shows it flipping between copy and none. A drop made while the last answer was none
    is cancelled by GNOME, which would fit a quick first drop failing and a slower retry working.
  - Not known yet: what a failure looks like (the picture flying back to Kestrel, nothing at all, or the box not
    lighting up).
  - Next time it happens: keep the failing window open, start `WAYLAND_DEBUG=client build/kes ~ 2> ~/kes-drag.log`,
    and drag from that window until one fails. The log shows whether Brave refused the drop (it never accepted, or
    its last action was none) or Kestrel did something wrong. If Brave refused it, compare with a drag from GNOME
    Files into the same page.

- [ ] **A merge into the parent folder lost a file (seen once, not reproduced yet).** On ZFS (2026-11-12): a folder
  `1` held a folder `1` with pictures in it (`1/1/img.jpg`). Cut `1/1` with the mouse, went up to the folder above,
  pasted; the conflict question came up for `1`, answered Merge with "Apply this action to all conflicts". The job
  finished and the status bar said the merge can't be undone, but img.jpg wasn't in `1` afterwards.
  - Not reproduced: the same steps through the real paste path (cut, paste, the conflict dialog's Merge with "all"),
    on ext4 and on ZFS 2.4.1 with block cloning (plain and aes-256-gcm encrypted datasets), with a few files, 3 MB
    files, and 500 files of mixed sizes, with and without files of the same names already in `1`: every file arrived
    whole and `1/1` was deleted. Reading the code (plan_transfer → merge_move → copy_at into the existing folder,
    then remove(src)) finds no path that drops a file.
  - Job logs weren't being saved (Preferences → Jobs), so there's no record of that run.
  - Still to find out: whether the file was really gone or the folder view just didn't show it (`ls`, or F5); how
    many files and how big; whether anything else was cut or open at the time (another tab or window on `1`); the
    dataset's properties. Next time: turn on saving job logs, and check the folder with `ls` before anything else.

## New features

### Jobs

### Archives

- [ ] **Open as Archive, part 1: a mini Kestrel window, cached in RAM.** Open as Archive (src/archivewin.cpp) is
  now a plain member list (a QTreeWidget); opening a member extracts it to a folder on disk
  (`ArchiveWindow::temp_root()`, `APP_CACHE()/archive-files`, swept after a day). Make it a small Kestrel window
  instead, and keep everything it extracts in RAM, never on disk:
  - **Browsing like a pane:** the archive's folders browsed like real ones (icon and list views, zoom, sorting,
    back/forward/up, a path bar inside the archive), reusing Pane's views where possible rather than a second copy of
    them. Extract Here / To… / All and Test stay.
  - **Thumbnails and previews:** members' thumbnails and the image viewer / Quick Look (Space), made from copies
    extracted into the RAM cache. Their thumbnails stay in that cache too, never in `~/.cache/thumbnails` or the
    folder-mosaic cache (the archive's names and pictures don't leak to disk).
  - **The RAM cache:** a tmpfs Kestrel can use without root (a folder under `$XDG_RUNTIME_DIR` or `/dev/shm`, mode
    0700; mounting a tmpfs of its own needs root), with a size limit (a setting in src/prefs.cpp, e.g. a share of RAM;
    `$XDG_RUNTIME_DIR` is usually capped at 10% of RAM by logind). When it's full it works first in, first out: the
    oldest extracted files are removed to make room for what's being browsed now. Never evict a file that's still in
    use: one open in another app, being dragged, on the clipboard, or being copied out (pin those until they're done;
    if everything is pinned, say the cache is full rather than evicting). A member bigger than the whole cache is
    extracted on request for that one use (or refused with a message), not cached. The cache is removed when the
    window closes and swept at start-up after a crash. Encrypted archives: the password from the keyring as now
    (src/keyring.cpp); their extracted members also only in RAM.
  - **Getting files out:** drag from the archive window into a Kestrel window, the desktop or another app, and Copy
    (Ctrl+C) then Paste in a Kestrel window or the desktop; both extract into the RAM cache and hand over those paths,
    and a paste/drop into Kestrel runs as a normal copy job (part names, renamed into place). Cut / Move / Delete
    aren't offered (read-only until part 2). The extraction on demand for drags (`MembersMime`) moves to the cache.
  - **The basic window stays as an option:** a setting in Preferences (src/prefs.cpp; e.g. "Open as Archive opens:
    the archive browser / the simple list") makes Open as Archive open today's plain member list instead of the new
    window. The new window is the default. The simple list keeps working as now, including its disk cache
    (`archive-files`) for opening members, dragging out and Extract.
  - The disk cache (`archive-files`) is then only for the simple list: keep it in kes-setup's BleachBit cleaner and
    tests/test_installer.sh's expectations.
  - docs/context-menus.md (the archive window's menu), the Open as Archive part of the docs, and tests (FIFO eviction,
    pinned files kept, a cancelled extraction leaves no half file under a member's name).
- [ ] **Open as Archive, part 2: changing an archive.** After part 1: for formats whose tools can add (and, for some,
  remove) members, e.g. zip, 7z, tar (uncompressed), let the archive window add files (drop or paste into it, Add
  Files…) and delete members, and make folders inside it. Read-only formats (rar without a rar licence, compressed
  tars, isos…) keep it read-only, with the reason shown. Failure-atomic: the tools write the archive in place, so
  the change is made to a copy under a part name beside it and swapped in when the tool succeeds (a failure or
  cancel leaves the original as it was); never edit an archive on a tape in place. Which formats and tools (7z, zip,
  bsdtar…) support what to be worked out when part 1 is done.

### Drives, the sidebar and the Overview

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

- [ ] **Scripts that choose when they show, and run as jobs.** This is Kestrel's form of plugins/extensions: users
  connect Kestrel to outside tools and programs on their own terms, without changing Kestrel. So the header lines and
  variables below are a public interface: documented as such, kept working across versions (new lines and variables
  only added, never renamed or changed in meaning), with `KES_SCRIPT_API` (a number, starting at 1) telling a script
  which version it's talking to. Scripts ▸ (src/sharing.cpp, `add_scripts_menu`) lists every executable in
  `~/.local/share/nautilus/scripts` for every selection and runs it blind. Build on it:
  - **Folders:** also read `~/.local/share/kestrel-explorer/scripts` (`XDG_DATA_HOME`; not the cache, which BleachBit
    and users clear), merged with Nautilus's in the one menu (subfolders → submenus, as now).
  - **A header saying when a script shows:** comment lines Kestrel reads, e.g. `# kestrel-on: file files` (kinds:
    `file`, `files`, `folder`, `folders`, `mixed`, `background` for the folder's own menu) and `# kestrel-types:
    image/* application/pdf` (MIME filter, matched against every selected item). A script with no header shows for
    every selection, as now, so Nautilus scripts keep working. Kestrel never edits a script.
  - **What the script gets:** the selected items as full paths in its arguments (today names in the current folder are
    passed bare); `KES_KIND`, `KES_COUNT`, `KES_CURRENT_DIR`; `KES_SELECTION_FILE`, a NUL-separated list in a temp file
    (names with newlines, selections too big for argv), removed when the script ends; the `NAUTILUS_SCRIPT_*`
    variables as now.
  - **Runs as a job:** a row in the job queue (src/jobs.cpp) with Cancel (SIGTERM to its process group), its
    stdout/stderr in the job's log (saved by joblogs.cpp when that's on), failed on a non-zero exit. A header can ask
    for `# kestrel-run: silent` (as today) or `terminal` (the user's terminal, for scripts that ask questions).
  - **New Script…** at the end of Scripts ▸ (beside Open Scripts Folder): asks for a name (ask_text), writes a starter
    script in Kestrel's folder with the header lines (commented, with every kind and an example type) and a block
    reading the arguments and variables, made executable, then opens it in the text editor. Written as a part and
    claimed with RENAME_NOREPLACE (never replaces a script of that name).
  - **One way only: a script never controls Kestrel.** A Kestrel interaction starts a process (or several); the only
    thing that comes back is its output, as the job's log in the Jobs window (verbose logging). No commands, no
    selecting or opening files, no messages read from its output: whatever it prints is shown as text, never acted on.
    The job ends when the script itself exits (a program it started and left running doesn't keep the job open, even
    holding its output).
  - docs/context-menus.md rows (both Scripts ▸ rows: the filtering, New Script…), a docs page on extending Kestrel
    with scripts (header lines, variables, run modes, the compatibility promise, example scripts), and tests.
    - Example, **Extract frames** (`kestrel-on: file files`, `kestrel-types: video/*`): makes a folder named after
      each selected video without its extension (`holiday.mp4` → `holiday/`) and lets ffmpeg extract every frame into
      it, its output in the job's log. Shows the header, the arguments and logging. Scripts are the user's own: Kestrel
      starts them, but they (and the examples) don't follow Kestrel's rules or design (no part names, no atomic
      writes); a tool does what it does.
    - The tests: the filter per kind and type, the arguments and variables a stub script receives, Cancel ending the
      job (the script held on a FIFO), New Script… not overwriting a name.
  - The example script, for reference:
    ```bash
    #!/bin/bash
    # kestrel-on: file files
    # kestrel-types: video/*
    # Extract every frame of each selected video into a folder named after it (holiday.mp4 → holiday/).

    for video in "$@"; do
        out="${video%.*}"
        mkdir -p -- "$out"
        ffmpeg -hide_banner -i "$video" "$out/frame_%06d.png"
    done
    ```

### Images, video and audio

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

## Beta release blocker

*Everything in this section is to be done before the first beta.*

### Drives, the sidebar and the Overview

- [ ] **Format drives and partitions (Format…), with partition editing handed off.** *Preliminary plan, not fully
  thought out yet: to be gone through again before any of it is built.*
  - **The tool:** udisks2 over D-Bus (`Block.Format()`, `PartitionTable.CreatePartition()` /
    `CreatePartitionAndFormat()`, `Partition.Delete()`), the engine GNOME Disks and GNOME Files' Format… use; it
    runs libblockdev, sfdisk and the `mkfs.*` tools. `udisksctl` has no format command, so it's the library (as for
    drive health). Not the admin helper: udisks does its own polkit check, and partitioning is outside what the
    admin session is for (docs/admin.md).
  - **In Kestrel:** right-click a drive or partition (Overview card, sidebar) → **Format…**. Whole drive: a new
    partition table (GPT by default, MBR for older devices), one partition, a file system. One partition: reformat it
    in place. Options udisks offers: file system, label, LUKS encryption (passphrase in the D-Bus options, never on a
    command line), erase with zeros.
  - **Handed off:** resizing, moving or making several partitions → **Edit Partitions…** opens GNOME Disks
    (`gnome-disks --block-device …`) or GParted if installed, hidden otherwise. No partition editor of our own.
  - **Missing tools:** without udisks the entries are hidden. File systems come from `Manager.CanFormat(type)`; one
    whose `mkfs` is missing is greyed out with its package (exfatprogs, dosfstools, ntfs-3g, btrfs-progs…), which go
    in docs/dependencies.md, kes-setup's OPTIONAL_PACKAGES and the .deb's Suggests.
  - **Safety (formatting can't be failure-atomic, so the next best thing):** refuse the system's own drives (`/`,
    `/home`, `/boot`, active swap, LUKS/LVM/ZFS members; reuse diskimage.cpp's `bases()` / `mounted_writable()`;
    udisks' `HintSystem` as a second signal); the confirmation names the drive (model, size, partitions, labels, used
    space), and a whole drive needs its name or size typed (`ask_box`); a "Back Up Drive to Image first…" button;
    remember the udisks object path, serial and size and re-check them before starting (the drive may have been
    swapped while the window was open); unmount through udisks first, never forced.
  - **A job in the queue:** progress from the udisks `Job`'s `Progress`; Cancel only where udisks can cancel (an
    erase), disabled during `mkfs` and the partition-table write, with the reason in the log. Failure decided by the
    D-Bus error, its message shown.
  - **Also:** menus::tag ids and defaults, rows in docs/context-menus.md, `tx()`, a test_ui check for the new window,
    a row in docs/design-philosophy.md's tools table.
  - **Tests:** the refusal checks, the options sent for each choice, the `CanFormat` → greyed-out mapping; a real
    format of a loop device backed by a sparse file, checked with `blkid`, skipped where udisksd or polkit isn't
    available (likely CI's containers).
  - Open questions: whether this belongs in Kestrel at all versus only a hand-off to GNOME Disks; KDE and Mint (no
    GNOME Disks: KDE Partition Manager, GParted); how polkit's prompt looks for udisks' format action on each
    desktop; what to do with drives udisks marks as system ones but the user wants formatted.

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


### Tests and CI

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


### Security

- [ ] **Every cache inside Kestrel's cache folder.** Everything Kestrel caches or keeps temporarily on disk goes under
  `APP_CACHE()` (`~/.cache/kestrel-explorer`, or the private RAM disk's in private mode), so it's in one place that
  users, BleachBit (kes-setup's cleaner) and Clear Cache can find and empty. The one exception is thumbnails while the
  shared thumbnail cache is in use (the default, not "separate caching"): those go to `~/.cache/thumbnails` like
  every other app's.
  - Audit src/ for anything written outside it: `/tmp`, `QDir::tempPath()`, `QTemporaryFile`/`QTemporaryDir`,
    `mkstemp`/`mkdtemp`, `$XDG_RUNTIME_DIR`, other `~/.cache` folders, and caches written by tools Kestrel runs
    (ffmpeg, exiftool, thumbnailers, 7z…, via their environment or output paths). Known so far:
    - Thumbnailer output in `/tmp/gnome-desktop-thumbnailer-*.png` (src/thumbs.cpp `run_thumbnailer`): kept there
      because Ubuntu's AppArmor profiles only let thumbnailers such as evince/papers write to that name. Find a way
      that works with those profiles (e.g. a pipe or the thumbnailer's stdout where it supports it), or keep it as a
      documented exception, removed at once after reading as now.
    - Back Up Drive to Image's FIFO folder in `$XDG_RUNTIME_DIR` / the temp dir (src/diskimage.cpp): a pipe, not a
      cache; decide whether it moves or is listed as an exception.
  - Things that aren't caches (settings, histories, bookmarks, sockets, runtime state) stay where they belong
    (`~/.config`, `$XDG_RUNTIME_DIR`); the rule is about caches and temporary copies.
  - Anything that does have to stay outside gets a comment saying why, and is listed in docs (with BleachBit's
    cleaner covering it if it can outlive Kestrel). A test (like test_prefs's checks on src/) that fails on a new
    temp/cache path in src/ outside `APP_CACHE()` and the allowed exceptions.
  - The RAM cache planned for Open as Archive part 1 (New features → Archives) is RAM, not disk: it follows that
    item's own rules.

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

## Done

- [x] **Kestrel as the file chooser for native GTK apps too (opt-in), and where it can't be** (0.3.4-alpha.1).
  `kes-setup --native-gtk` / `install.sh --native-gtk`: GTK_USE_PORTAL=1 and GDK_DEBUG=portals in
  ~/.config/environment.d/60-kestrel-explorer-portal.conf (marked; install-state native_gtk=yes; --undo removes it),
  next login; the limit (apps building GtkFileChooserDialog themselves: gedit's Save / Save As) in docs/install.md and
  the --default and --native-gtk messages.

- [x] **Portrait RAW photos show the right way up in the viewer and thumbnails** (0.3.4-alpha.1). The embedded
  preview (quality 0) is turned by the file's Orientation when its shape doesn't match the photo's
  (thumbs::raw_preview; metadata::raw_orientation reads TIFF-based formats incl. ORF/RW2, RAF's preview EXIF, CR3's
  CMT1), never scaled into the photo's shape; no tag: shown unturned, unsqueezed. Old RAW thumbnails (Kestrel's,
  without X-Kestrel-Raw-Turned) and mosaics (v3) are made again. Still to do: confirm with portrait samples of each
  format from raw.pixls.us in a container with the RAW plugin.

- [x] **Drop files and folders onto any sidebar entry** (0.3.4-alpha.1). Highlight and "Move to …" hint under the
  entry, spring-loading (500 ms), each entry's target worked out once per drag, Starred stars, a phone, share or
  server that isn't connected is connected first (a RAM disk brought back), Recent, the Overview, a gone bookmark and
  a read-only drive (the mount table's "ro") refuse with the reason; the keys held at the drop go with it.

- [x] **The file chooser uses the same caches and follows the same preferences as the rest of Kestrel**
  (0.3.4-alpha.1). Chooser windows (and the Link window's two sides) are applied every setting change, the
  `--file-chooser` process hears other Kestrels' changes through the tower, and only the `chooser/…` view keys
  (view, icon sizes, hidden files, sidebar, info panel) and its size are its own; the sort default, pinned-first,
  folder previews and search options are the main windows'.

## Struck items

*Struck-through items (~~like this~~, marked "Struck for now") are low-priority ideas, kept for reference. They may
or may not be returned to a normal to-do item.*

*This list is the road map. More ideas are kept in a separate private list: provisional ones, thought experiments and
other ideas that would take up too much room here, most of which won't be built. An idea is usually worked out fully
there before it's added to this list.*

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
