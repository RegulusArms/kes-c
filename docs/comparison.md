# Kestrel compared with other file managers

← Back to [the README](../README.md)

Kestrel is made to sit beside the file manager your desktop comes with: GNOME Files on Ubuntu, Nemo on Linux Mint,
Dolphin on Kubuntu. This page says what it adds over each, what it doesn't have (yet), and how it lives alongside
them. The numbers come from the benchmark ([Performance](performance.md)).

- [Installs alongside, changes nothing](#installs-alongside-changes-nothing)
- [Compared with GNOME Files (Ubuntu)](#compared-with-gnome-files-ubuntu)
- [Compared with Nemo (Linux Mint)](#compared-with-nemo-linux-mint)
- [Compared with Dolphin (Kubuntu)](#compared-with-dolphin-kubuntu)
- [Not in Kestrel (yet)](#not-in-kestrel-yet)

## Installs alongside, changes nothing

Kestrel is an app of its own. It installs next to GNOME Files, Nemo or Dolphin instead of replacing them, and doesn't
remove, reconfigure or disrupt them: they keep working as before, and you can use either one at any time.

- **What's shared:** with GNOME Files and Nemo, the bookmarks, the thumbnails, the clipboard, the trash and Recent
  files, so either one carries on where the other left off. With Dolphin, the trash, the thumbnails, cut and paste
  (cutting in one and pasting in the other moves the files), and the files opened in KDE apps in Kestrel's Recent;
  Dolphin's Places are its own.
- **Making Kestrel the default is your choice, and undone in one step.** `./install.sh --default` (or
  `kes-setup --default` with the apt package) makes Kestrel open folders and "Show in folder" requests and show other
  apps' Open and Save dialogs; with `--dock` it also takes the other file manager's place in the dock or taskbar.
  Kestrel remembers what it changed, and `./install.sh --uninstall` (or `kes-setup --undo`) hands everything back,
  leaving your other settings alone. See the [install guide](install.md) for what each option changes.

## Compared with GNOME Files (Ubuntu)

- Undo for moves, renames, Move to Trash and new files.
- One Trash for every drive, and an Overview page of drives with usage bars.
- Folder icons that preview the images inside, plus per-folder colours.
- A built-in image viewer, and GIF/WebM playback in the file view.
- Background copies with progress and Cancel in the status bar, and one Jobs window that queues them by drive.
- Phones and cameras (iPhone, Android, PTP cameras) on the Overview page and in the sidebar, with thumbnails from the
  phone's own previews.
- A sidebar you can rearrange and collapse.
- A sort for each folder, and grouping by date (Today, Last week… or one group per day), by the date modified, created
  or taken (from photos' and videos' metadata), with each group still sorted by name, size or type.
- Follows the desktop's theme and accent colour, also when you change them while it's open.
- Other apps' Open and Save dialogs, such as a browser's "Save image as", can open in Kestrel.
- Several windows that stay in sync.
- Faster in most measurements (see [Performance: Kestrel vs GNOME Files](performance.md#performance-kestrel-vs-gnome-files)).

## Compared with Nemo (Linux Mint)

- Archives created and extracted with every option in the window, instead of handing off to a separate archive app.
- Admin rights for just the operation that needs them, instead of opening a whole Nemo window as root.
- Folder icons that preview the images inside.
- A built-in image viewer, and GIF/WebM playback in the file view.
- Metadata viewing and editing (EXIF, XMP, and the prompts image generators such as Stable Diffusion save in the file)
  and checksums in Properties.
- Phones: thumbnails from the phone's own previews, and iPhone videos copied once so they play smoothly.
- Faster file operations in the benchmark: copying about 20× faster, moving to another drive 5.4× faster, and emptying
  the trash several hundred times faster (see [Performance: Kestrel vs Nemo](performance.md#performance-kestrel-vs-nemo)).

## Compared with Dolphin (Kubuntu)

Dolphin already has much of what Kestrel adds over GNOME Files (undo, folder previews, grouping by date, one trash for
every drive). Kestrel adds:

- Archives created and extracted with every option in the window, with a preview of the exact command, instead of
  handing off to Ark.
- A built-in image viewer (compare, crop and resize, lossless rotation) instead of a separate app.
- Editing any metadata tag (EXIF, XMP, the prompts image generators save), not only ratings, tags and comments; and
  Wipe Metadata.
- Checksum files: double-click one (`SHA256SUMS`, `.md5`, `.sfv`…) to check every file it lists, and Create Checksum
  File…; Verify Signature for `.sig` / `.asc` files.
- Converting images, videos and audio, Find Duplicates, and Compare With… / Sync To… for folders, in the right-click
  menu.
- One Jobs window that queues copies by drive, so two jobs never fight over one disk, with pause, reorder and each
  job's log.
- Admin rights for just the operation that failed, offered when it fails.
- Faster file operations on Kubuntu in the benchmark: copying about 24× faster, moving to another drive about 25×
  faster, moving 10,000 files to the trash about 130× faster, and emptying it about 12× faster (see
  [Performance: Kestrel vs Dolphin on Kubuntu](performance.md#performance-kestrel-vs-dolphin-on-kubuntu)).

Kestrel follows Plasma's look and settings there (see the [install guide](install.md#kubuntu-2604-kde-plasma)).

## Not in Kestrel (yet)

- **Split view** (two folders side by side in one window), which Nemo and Dolphin have.
- **A terminal panel** inside the window, and **tags**, which Dolphin has.
- **On Kubuntu:** PDF thumbnails (Kestrel uses the thumbnailer programs GNOME's PDF viewers install, which Kubuntu
  doesn't have), and KDE's search index (Baloo): Kestrel uses its own content search there. Going to a small folder in
  an open window is slower than Dolphin (0.14 s against 0.03 s).

## Where Kestrel came from

Kestrel began as a C++ port of the Python/PyQt6 [Kestrel Explorer](https://github.com/RegulusArms/kestrel-explorer).
The two were the same up to 0.3.1-alpha.2; since then each is developed on its own. They still use the same settings
folder, bookmarks and caches, but aren't kept compatible any more.
