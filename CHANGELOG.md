# Changelog

What changed in each version of Kestrel Explorer, newest first. Versions with a GitHub release: 0.2.1-alpha1,
0.2.1-alpha.2, 0.2.1-alpha.3, 0.3.0-alpha.0, 0.3.0-alpha.2 and 0.3.1-alpha.2.

## 0.3.1-alpha.5

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
