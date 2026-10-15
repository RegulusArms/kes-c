# To do

Things to add, fix or change. Newest at the bottom; tick an item off (`[x]`) when it's done, with the version it went
into.

## Open

- [ ] **Extract several archives at once.** Selecting more than one archive and right-clicking offers **Extract All
  Here** and **Extract All To…** (today Extract only appears for a single archive).
  - Each archive is extracted the way Extract Here / Extract To… does it on its own (its own folder, the same conflict
    handling), one after another, in the status bar with one ✕ to stop them all.
  - Passwords: the first password typed is tried on every encrypted archive that follows. Only when it doesn't open
    one is the password asked again for that archive, and the new password is then the one tried on the rest.
  - At the end, one summary of what was extracted and what failed (a wrong password cancelled, a missing tool, a
    damaged archive), not a message per archive.
  - A split set (`x.part1.rar` … `x.part3.rar`) selected whole counts as one archive, not three.

- [ ] **Remember archive passwords in the keyring.** Save archive passwords in the desktop's keyring (the Secret
  Service: GNOME Keyring on Ubuntu and Mint, KWallet on KDE) through `libsecret`, so a password used before doesn't
  have to be typed again.
  - Stored in the login keyring as Kestrel's own entries (a Kestrel schema, e.g. "Kestrel Explorer: archive password
    for photos.7z" in Passwords and Keys), not a separate keyring, which would need its own password and unlocking.
  - Opt-in: a **Remember this password** box in the password prompt, off by default. Any app running as the user can
    read the login keyring, so nothing is saved without asking.
  - Finding one: first the password saved for this archive (recognised by its contents, e.g. size plus a hash of its
    start, so renaming or moving it still works), then the other saved passwords (each a quick test with the archive
    tool, not an extraction), and only if none opens it, ask. Works with Extract All (the item above).
  - Forgetting: a Preferences button to forget all saved archive passwords (they can also be deleted in Seahorse).
  - No keyring running, or none that speaks the Secret Service: everything works as today, just without saving.
  - New dependency: `libsecret-1-dev` to build, `libsecret-1-0` in the .deb's Depends (install.sh's build deps,
    docs/dependencies.md).

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

- [ ] **A sidebar section (dropdown) for mounts.** A collapsible **Mounts** section in the sidebar, like Places,
  Bookmarks and Devices (moved and collapsed the same way), listing the mounts GNOME Files shows that Kestrel's
  sidebar leaves out.
  - Found on this machine (2026-10-14): GNOME Files lists `~/jd-download` and `~/secure` (ZFS datasets
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

## Done

(nothing yet)
