# Performance

← Back to [the README](../README.md) · [Changelog](../CHANGELOG.md) · [Compared with other file managers](comparison.md) · [Development](development.md) · [Performance summary](performance-summary.md) · [Benchmark](../bench/README.md)

**Short on time?** Read the [performance summary](performance-summary.md).

## At a glance

Every comparison below in one table; the sections after it have the details, notes and the limits of each comparison.
Kestrel, GNOME Files, Dolphin, Thunar, Krusader and PCManFM were measured in the same run, in a container of Ubuntu
26.04. After the first divider: Kestrel and Nemo in a container of Linux Mint 22.3. After the second: Kestrel and
Dolphin in a container of Kubuntu 26.04, with KDE's Qt integration loaded as on a Plasma desktop. All on the same
machine; compare each pair after a divider with each other, not with the other columns. "—" is a test that couldn't be
run on that app (see its section): Krusader, PCManFM and Nemo make no thumbnails the benchmark can time, so the folder
of 10,000 files compares how fast they open a folder. Lower is better throughout; the best figure of each group (the
six apps on Ubuntu, each pair after a divider) is in bold and underlined.

| Test | Kestrel | GNOME Files | Dolphin | Thunar | Krusader | PCManFM | │ | Kestrel (Mint) | Nemo (Mint) | │ | Kestrel (Kubuntu) | Dolphin (Kubuntu) |
| --- | --- | --- | --- | --- | --- | --- | :-: | --- | --- | :-: | --- | --- |
| Startup (launch to window shown) | 0.50 s | <ins>**0.44 s**</ins> | 0.64 s | 0.59 s | 1.27 s | 0.51 s | │ | 0.37 s | <ins>**0.28 s**</ins> | │ | <ins>**0.63 s**</ins> | 0.69 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | <ins>**0.52 s**</ins> | 1.20 s | 0.99 s | 1.35 s | — | — | │ | 0.49 s | — | │ | <ins>**0.66 s**</ins> | 1.14 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | <ins>**1.38 s**</ins> | 1.76 s | — | 2.10 s | — | — | │ | 1.34 s | — | │ | 1.53 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | <ins>**0.73 s**</ins> | 0.84 s | 1.33 s | 1.40 s | — | — | │ | 0.68 s | — | │ | — | 1.48 s |
| Open a folder of 10,000 files (launch to the full list shown) | <ins>**0.55 s**</ins> | 1.37 s | 1.17 s | 1.76 s | 1.51 s | <ins>**0.55 s**</ins> | │ | <ins>**0.49 s**</ins> | 3.11 s | │ | <ins>**0.69 s**</ins> | 1.19 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | <ins>**0.089 s**</ins> | 0.83 s | 0.30 s | 2.02 s | 0.16 s | 0.22 s | │ | <ins>**0.16 s**</ins> | 2.50 s | │ | <ins>**0.16 s**</ins> | 0.44 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | <ins>**0.053 s**</ins> | 0.36 s | 0.065 s | 0.21 s | 0.064 s | 0.22 s | │ | <ins>**0.13 s**</ins> | 1.51 s | │ | 0.16 s | <ins>**0.036 s**</ins> |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | <ins>**0.088 s**</ins> | 7.99 s | 0.30 s | 0.29 s | — | — | │ | 0.089 s | — | │ | <ins>**0.088 s**</ins> | 0.23 s |
| 5,000 files arriving in an open folder (until all shown) | <ins>**0.20 s**</ins> | 0.43 s | 0.88 s | 1.16 s | 0.58 s | 0.57 s | │ | <ins>**0.22 s**</ins> | 1.01 s | │ | <ins>**0.20 s**</ins> | 0.92 s |
| Copy 20,000 small files + 5 × 50 MB | <ins>**1.16 s**</ins> | 2.57 s | 29.87 s | 7.66 s | 6.86 s | 3.57 s | │ | <ins>**1.15 s**</ins> | 15.68 s | │ | <ins>**1.14 s**</ins> | 29.77 s |
| Move the same to another drive | <ins>**1.47 s**</ins> | 5.07 s | 42.59 s | 9.30 s | 17.21 s | 6.26 s | │ | <ins>**1.46 s**</ins> | 4.90 s | │ | <ins>**1.47 s**</ins> | 41.58 s |
| Move 10,000 files to the trash (all selected in one folder) | <ins>**2.19 s**</ins> | 2.39 s | 271.24 s | 3.26 s | 283.31 s | 14.05 s | │ | 1.89 s | — | │ | <ins>**2.14 s**</ins> | 274.10 s |
| Empty the trash (those 10,000 files) | <ins>**0.48 s**</ins> | 8.82 s | 6.98 s | 172.64 s | 6.88 s | 23.96 s | │ | <ins>**0.48 s**</ins> | 183.50 s | │ | <ins>**0.48 s**</ins> | 6.44 s |
| Longest wait for the window to answer while it copies (Back/Forward) | 0.037 s | 0.041 s | <ins>**0.036 s**</ins> | 0.037 s | — | 0.037 s | │ | 0.037 s | — | │ | <ins>**0.036 s**</ins> | <ins>**0.036 s**</ins> |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | <ins>**1390 ms**</ins> | 18450 ms | 6310 ms | 2000 ms | — | — | │ | 1150 ms | — | │ | <ins>**1430 ms**</ins> | 6740 ms |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | <ins>**180 ms**</ins> | 1550 ms | 1350 ms | 1470 ms | 190 ms | 970 ms | │ | <ins>**180 ms**</ins> | 1890 ms | │ | <ins>**160 ms**</ins> | 1360 ms |
| Peak memory (startup / 600-image folder open) | 129 / 142 MB | 156 / 269 MB | 169 / 183 MB | 96 / 98 MB | 151 / — MB | <ins>**94 / — MB**</ins> | │ | 118 / 127 MB | <ins>**102 / — MB**</ins> | │ | <ins>**179 / 189 MB**</ins> | 184 / 194 MB |
| Memory with 5 folders opened from other apps | 80 MB (71 MB as tabs) | 129 MB | 111 MB | 51 MB | 100 MB | <ins>**45 MB (as tabs)**</ins> | │ | <ins>**64 MB (54 MB as tabs)**</ins> | <ins>**64 MB**</ins> | │ | <ins>**152 MB (109 MB as tabs)**</ins> | 163 MB |
| CPU time used in 30 s with a folder open, idle | 5 ms | <ins>**0 ms**</ins> | <ins>**0 ms**</ins> | 5 ms | <ins>**0 ms**</ins> | 5 ms | │ | 5 ms | <ins>**0 ms**</ins> | │ | <ins>**0 ms**</ins> | <ins>**0 ms**</ins> |

## Performance: Kestrel vs GNOME Files

Kestrel Explorer is compared here with GNOME Files 50.2.2, the file manager it replaces.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS, in a Docker container (Ubuntu's image with the parts of Ubuntu's desktop the apps use). The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, GTK's cairo renderer), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, so no file indexer ran. The benchmark is in [bench/](../bench) and is run with `bench/run.sh`.

### Compared with GNOME Files

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first 12 files' thumbnails are in the shared thumbnail cache.
- **Opening a folder of 10,000 files:** timed from launch until the window has stopped changing: the list drawn, with its count of items where the app shows one. The window is captured as often as it can be (every few ms), and the time is that of its last change, once nothing has changed for 2 s.
- **Going to a folder in an open window:** the folder's path typed into the location bar of a window that's already open, timed from Enter until the window has stopped changing, as above. Most folders are opened this way rather than by starting the app.
- **Jumping to the end of the 600-image folder:** once the first 12 thumbnails are there, End is pressed, timed until the last 6 files have thumbnails. With it, the CPU time used from launch to the last thumbnail, by the app and every helper it ran (thumbnailers, KIO workers, services), so an app that spreads the work over threads or processes can't look cheaper than it is.
- **Files arriving in an open folder:** 5,000 small files created at once in the folder a window shows, as a download, an unpacked archive or a build would, timed until the window has stopped changing, with the CPU time used meanwhile.
- **Window answering while it copies:** the 20,000 files + 250 MB copied and pasted in the app's window, and while the paste runs, Back and Forward pressed in turn, each timed until the window's title shows the folder it went to. The longest and the median wait, and in brackets the median with nothing running (what pressing a key and reading the title cost by themselves).
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. GNOME Files receives them through its D-Bus file-operations service, as when another app asks it to.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | GNOME Files |
|---|---|---|
| Startup (launch to window shown) | 0.50 s | 0.44 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.52 s | 1.20 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.38 s | 1.76 s |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.73 s | 0.84 s |
| Open a folder of 10,000 files (launch to the full list shown) | 0.55 s | 1.37 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.089 s | 0.83 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.053 s | 0.36 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.088 s | 7.99 s |
| 5,000 files arriving in an open folder (until all shown) | 0.20 s | 0.43 s |
| Copy 20,000 small files + 5 × 50 MB | 1.16 s | 2.57 s |
| Move the same to another drive | 1.47 s | 5.07 s |
| Move 10,000 files to the trash (all selected in one folder) | 2.19 s | 2.39 s |
| Empty the trash (those 10,000 files) | 0.48 s | 8.82 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.037 s / 0.036 s (idle 0.035 s) | 0.041 s / 0.036 s (idle 0.035 s) |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1390 ms | 18450 ms |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 180 ms | 1550 ms |
| Peak memory (startup / 600-image folder open) | 129 / 142 MB | 156 / 269 MB |
| Memory with 5 folders opened from other apps | 80 MB (71 MB as tabs) | 129 MB |
| CPU time used in 30 s with a folder open, idle | 5 ms (0.02% of a core) | 0 ms (0.00% of a core) |

Kestrel compared with GNOME Files:
- startup: 1.1× slower;
- first thumbnails in a 600-image folder: 2.3× faster;
- first video thumbnails: 1.3× faster;
- first PDF thumbnails: 1.2× faster;
- listing a folder of 10,000 files: 2.5× faster;
- going to the folder of 10,000 files in an open window: 9.3× faster;
- going to a small folder in an open window: 6.8× faster;
- thumbnails at the end of the 600-image folder: about 91× faster;
- showing files as they arrive: 2.1× faster;
- copying: 2.2× faster;
- moving to another drive: 3.4× faster;
- moving to the trash: 1.1× faster;
- emptying the trash: about 18× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - GNOME Files makes thumbnails for the whole folder in the background. It finished all 600 images 9.42 s after launch.
  - Kestrel makes them only for what's on screen (20 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.86 s for these 600 images (table below).
- **File operations:** GNOME Files' D-Bus service returns straight away, so its times were measured by watching the files until the operation had finished, to within about 50 ms.
- **Jumping to the end:** GNOME Files makes thumbnails for the whole folder in the background, so some may be ready before End is pressed.
- **Memory:** GNOME Files makes thumbnails in separate sandboxed helper processes, whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** GNOME Files' search can't be timed from outside without its file indexer, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because GNOME Files has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.86 s |
| Build 150 folder mosaics | 6.28 s |
| Recursive search over 50,000 files | 0.12 s |
| Read EXIF / image-generation prompts for 400 images | 0.046 s |
| Peak memory (background jobs) | 41–46 MB |

## Copying on Btrfs, XFS and SMB

Kestrel lets the system copy files (Preferences → Files → Copying, on by default): a clone where the drive can share
data between files, the server's own copy on a network share, and otherwise the kernel's in-kernel copy, for files of
64 KB and more (and every file on Btrfs). GNOME Files (GLib) does the same. Measured with `bench/fs.sh` in a privileged
container on the same machine (Ubuntu 26.04's image, GNOME Files 50): a 4 GB Btrfs and a 4 GB XFS file system on loop
images in a RAM disk, and a Samba share on the same machine mounted with the kernel's SMB client (SMB 3.1.1). The copy
test's files were copied within one folder there, source and copy both on it; then its 5 × 50 MB alone. Kestrel is
timed with the setting off (it reads and writes every byte itself, as before the setting) and on. Medians of 3 runs.

| Copy 20,000 small files + 5 × 50 MB | Kestrel, its own copy (before) | Kestrel, the system's copy (now) | GNOME Files |
|---|---|---|---|
| Within one Btrfs drive (a clone) | 2.45 s | 1.95 s | 3.05 s |
| Within one XFS drive (a clone) | 1.75 s | 1.72 s | 3.75 s |
| Within one SMB share (the server copies it) | 65.17 s | 64.52 s | 120.22 s |
| …its 5 × 50 MB alone, within one Btrfs drive (a clone) | 0.32 s | 0.006 s | 0.22 s |
| …its 5 × 50 MB alone, within one XFS drive (a clone) | 0.086 s | 0.046 s | 0.32 s |
| …its 5 × 50 MB alone, within one SMB share (the server copies it) | 0.44 s | 0.22 s | 0.38 s |

**What it shows:**
- **Big files:** a clone on Btrfs is instant (the 250 MB in 6 ms, taking no extra space) and halves the time on XFS;
  on the SMB share the server's own copy halves it too, and that's with the server on the same machine: over a real
  network the data no longer crosses it twice, so the gain is larger.
- **Small files:** the 20,000 files of 1–4 KB are under 64 KB, so Kestrel reads and writes them itself either way
  (except on Btrfs, which clones even small files faster: most of its 0.5 s gain). Cloning a 4–32 KB file on XFS
  is about 30% slower than one read and one write, and on SMB a small file costs the same either way; measured file
  by file (4 KB to 50 MB) before choosing 64 KB. On the SMB share each small file is a few trips to the server, which
  is most of the minute; the difference between the two Kestrel columns there is within the spread of its runs.
- **Limits:** loop images in a RAM disk and a share on the same machine, so no disk or network speed counts.

## Performance: Kestrel vs Nemo

Kestrel Explorer is compared here with Nemo 6.6.3, the file manager it replaces.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Linux Mint 22.3, in a Docker container (Linux Mint's image: Ubuntu 24.04 with Mint's own packages, and the parts of its Cinnamon desktop the apps use). The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, GTK's cairo renderer), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, so no file indexer ran. The desktop was set to Cinnamon's, so Kestrel used Qt's GTK integration and both apps Mint's Mint-Y theme and icons, as on Linux Mint. The benchmark is in [bench/](../bench) and is run with `bench/mint.sh`.

### Compared with Nemo

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first 12 files' thumbnails are in the shared thumbnail cache.
- **Opening a folder of 10,000 files:** timed from launch until the window has stopped changing: the list drawn, with its count of items where the app shows one. The window is captured as often as it can be (every few ms), and the time is that of its last change, once nothing has changed for 2 s.
- **Going to a folder in an open window:** the folder's path typed into the location bar of a window that's already open, timed from Enter until the window has stopped changing, as above. Most folders are opened this way rather than by starting the app.
- **Jumping to the end of the 600-image folder:** once the first 12 thumbnails are there, End is pressed, timed until the last 6 files have thumbnails. With it, the CPU time used from launch to the last thumbnail, by the app and every helper it ran (thumbnailers, KIO workers, services), so an app that spreads the work over threads or processes can't look cheaper than it is.
- **Files arriving in an open folder:** 5,000 small files created at once in the folder a window shows, as a download, an unpacked archive or a build would, timed until the window has stopped changing, with the CPU time used meanwhile.
- **Window answering while it copies:** the 20,000 files + 250 MB copied and pasted in the app's window, and while the paste runs, Back and Forward pressed in turn, each timed until the window's title shows the folder it went to. The longest and the median wait, and in brackets the median with nothing running (what pressing a key and reading the title cost by themselves).
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. Nemo receives them through its D-Bus file-operations service, as when another app asks it to.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | Nemo |
|---|---|---|
| Startup (launch to window shown) | 0.37 s | 0.28 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.49 s | — |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.34 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.68 s | — |
| Open a folder of 10,000 files (launch to the full list shown) | 0.49 s | 3.11 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.16 s | 2.50 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.13 s | 1.51 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.089 s | — |
| 5,000 files arriving in an open folder (until all shown) | 0.22 s | 1.01 s |
| Copy 20,000 small files + 5 × 50 MB | 1.15 s | 15.68 s |
| Move the same to another drive | 1.46 s | 4.90 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.89 s | — |
| Empty the trash (those 10,000 files) | 0.48 s | 183.50 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.037 s / 0.036 s (idle 0.035 s) | — |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1150 ms | — |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 180 ms | 1890 ms |
| Peak memory (startup / 600-image folder open) | 118 / 127 MB | 102 / — MB |
| Memory with 5 folders opened from other apps | 64 MB (54 MB as tabs) | 64 MB |
| CPU time used in 30 s with a folder open, idle | 5 ms (0.02% of a core) | 0 ms (0.00% of a core) |

Kestrel compared with Nemo:
- startup: 1.3× slower;
- listing a folder of 10,000 files: 6.3× faster;
- going to the folder of 10,000 files in an open window: about 16× faster;
- going to a small folder in an open window: about 12× faster;
- showing files as they arrive: 4.6× faster;
- copying: about 14× faster;
- moving to another drive: 3.4× faster;
- emptying the trash: about 385× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Nemo wasn't timed on this (see below).
  - Kestrel makes them only for what's on screen (21 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.87 s for these 600 images (table below).
- **File operations:** Nemo's D-Bus service returns straight away, so its times were measured by watching the files until the operation had finished, to within about 50 ms.
- **Thumbnails not timed:** Nemo makes no thumbnails in this setup (a fresh home folder with its default settings), so the thumbnail tests are skipped for it and those rows show —, as does its peak memory with the 600-image folder open. How fast it opens a folder is compared by the folder of 10,000 files instead.
- **Not timed:** Nemo's D-Bus file-operations service has no way to move files to the trash, so those rows show —. For "Empty the trash", the files were put in the trash with `gio trash` first.
- **Measured once:** Nemo took over 2 minutes for the trash test, so it was run once instead of 3 times.
- **Memory:** Nemo makes thumbnails in separate helper processes, whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** Nemo's search can't be timed from outside, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Nemo has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.87 s |
| Build 150 folder mosaics | 6.25 s |
| Recursive search over 50,000 files | 0.12 s |
| Read EXIF / image-generation prompts for 400 images | 0.038 s |
| Peak memory (background jobs) | 29–33 MB |

## Performance: Kestrel vs Dolphin

Kestrel Explorer is compared here with Dolphin 25.12.3, KDE's file manager.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS, in a Docker container (Ubuntu's image with the parts of Ubuntu's desktop the apps use). The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, both being Qt apps), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, so no file indexer ran. The benchmark is in [bench/](../bench) and is run with `bench/run.sh`.

### Compared with Dolphin

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first 12 files' thumbnails are in the shared thumbnail cache.
- **Opening a folder of 10,000 files:** timed from launch until the window has stopped changing: the list drawn, with its count of items where the app shows one. The window is captured as often as it can be (every few ms), and the time is that of its last change, once nothing has changed for 2 s.
- **Going to a folder in an open window:** the folder's path typed into the location bar of a window that's already open, timed from Enter until the window has stopped changing, as above. Most folders are opened this way rather than by starting the app.
- **Jumping to the end of the 600-image folder:** once the first 12 thumbnails are there, End is pressed, timed until the last 6 files have thumbnails. With it, the CPU time used from launch to the last thumbnail, by the app and every helper it ran (thumbnailers, KIO workers, services), so an app that spreads the work over threads or processes can't look cheaper than it is.
- **Files arriving in an open folder:** 5,000 small files created at once in the folder a window shows, as a download, an unpacked archive or a build would, timed until the window has stopped changing, with the CPU time used meanwhile.
- **Window answering while it copies:** the 20,000 files + 250 MB copied and pasted in the app's window, and while the paste runs, Back and Forward pressed in turn, each timed until the window's title shows the folder it went to. The longest and the median wait, and in brackets the median with nothing running (what pressing a key and reading the title cost by themselves).
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. Dolphin has no D-Bus file-operations service, so its own commands are used, with keys pressed in its window: Copy or Cut, then Paste in the destination (typed into its location bar), and Select All, then Delete (Move to Trash). Its Empty Trash runs KDE's empty-trash job, which `ktrash6 --empty` runs too; that was used for this step.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | Dolphin |
|---|---|---|
| Startup (launch to window shown) | 0.50 s | 0.64 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.52 s | 0.99 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.38 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.73 s | 1.33 s |
| Open a folder of 10,000 files (launch to the full list shown) | 0.55 s | 1.17 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.089 s | 0.30 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.053 s | 0.065 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.088 s | 0.30 s |
| 5,000 files arriving in an open folder (until all shown) | 0.20 s | 0.88 s |
| Copy 20,000 small files + 5 × 50 MB | 1.16 s | 29.87 s |
| Move the same to another drive | 1.47 s | 42.59 s |
| Move 10,000 files to the trash (all selected in one folder) | 2.19 s | 271.24 s |
| Empty the trash (those 10,000 files) | 0.48 s | 6.98 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.037 s / 0.036 s (idle 0.035 s) | 0.036 s / 0.035 s (idle 0.035 s) |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1390 ms | 6310 ms |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 180 ms | 1350 ms |
| Peak memory (startup / 600-image folder open) | 129 / 142 MB | 169 / 183 MB |
| Memory with 5 folders opened from other apps | 80 MB (71 MB as tabs) | 111 MB |
| CPU time used in 30 s with a folder open, idle | 5 ms (0.02% of a core) | 0 ms (0.00% of a core) |

Kestrel compared with Dolphin:
- startup: 1.3× faster;
- first thumbnails in a 600-image folder: 1.9× faster;
- first PDF thumbnails: 1.8× faster;
- listing a folder of 10,000 files: 2.1× faster;
- going to the folder of 10,000 files in an open window: 3.3× faster;
- going to a small folder in an open window: 1.2× faster;
- thumbnails at the end of the 600-image folder: 3.4× faster;
- showing files as they arrive: 4.4× faster;
- copying: about 26× faster;
- moving to another drive: about 29× faster;
- moving to the trash: about 124× faster;
- emptying the trash: about 15× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Dolphin made 500 thumbnails in all, the last 1.74 s after launch.
  - Kestrel makes them only for what's on screen (20 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.86 s for these 600 images (table below).
- **File operations:** Dolphin runs a paste or a move to the trash in the background, so its times were measured from the key press, by watching the files until the operation had finished, to within about 50 ms. Its window was given 2 s to list the folder first, which isn't counted.
- **Videos not timed:** its default settings leave video previews off, so Dolphin makes no thumbnails for them in a fresh home folder, and that row shows —.
- **Measured once:** Dolphin took over 2 minutes for the trash test, so it was run once instead of 3 times.
- **Memory:** Dolphin makes thumbnails in separate KIO worker processes, whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** Dolphin's search can't be timed from outside without its file indexer (Baloo), so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Dolphin's nearest equivalents (its folder previews and Information panel) can't be timed from outside, nor can its search.

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.86 s |
| Build 150 folder mosaics | 6.28 s |
| Recursive search over 50,000 files | 0.12 s |
| Read EXIF / image-generation prompts for 400 images | 0.046 s |
| Peak memory (background jobs) | 41–46 MB |

## Performance: Kestrel vs Dolphin on Kubuntu

Kestrel Explorer is compared here with Dolphin 25.12.3, KDE's file manager, which it replaces on Kubuntu.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Kubuntu 26.04.1 LTS, in a Docker container (Ubuntu's image with the Kubuntu packages the apps use). The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, both being Qt apps), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, so no file indexer ran. The desktop was set to KDE's, so both apps used Plasma's Qt integration and the Breeze style, as on a Plasma desktop. The benchmark is in [bench/](../bench) and is run with `bench/kubuntu.sh`.

### Compared with Dolphin

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first 12 files' thumbnails are in the shared thumbnail cache.
- **Opening a folder of 10,000 files:** timed from launch until the window has stopped changing: the list drawn, with its count of items where the app shows one. The window is captured as often as it can be (every few ms), and the time is that of its last change, once nothing has changed for 2 s.
- **Going to a folder in an open window:** the folder's path typed into the location bar of a window that's already open, timed from Enter until the window has stopped changing, as above. Most folders are opened this way rather than by starting the app.
- **Jumping to the end of the 600-image folder:** once the first 12 thumbnails are there, End is pressed, timed until the last 6 files have thumbnails. With it, the CPU time used from launch to the last thumbnail, by the app and every helper it ran (thumbnailers, KIO workers, services), so an app that spreads the work over threads or processes can't look cheaper than it is.
- **Files arriving in an open folder:** 5,000 small files created at once in the folder a window shows, as a download, an unpacked archive or a build would, timed until the window has stopped changing, with the CPU time used meanwhile.
- **Window answering while it copies:** the 20,000 files + 250 MB copied and pasted in the app's window, and while the paste runs, Back and Forward pressed in turn, each timed until the window's title shows the folder it went to. The longest and the median wait, and in brackets the median with nothing running (what pressing a key and reading the title cost by themselves).
- **PDFs:** Kestrel made no PDF thumbnails here, so its row shows —. It makes them with the desktop's thumbnailer programs (the `.thumbnailer` files that GNOME's PDF viewers install), and Kubuntu has none for PDFs: KDE makes them with its own plugins, which Dolphin uses.
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. Dolphin has no D-Bus file-operations service, so its own commands are used, with keys pressed in its window: Copy or Cut, then Paste in the destination (typed into its location bar), and Select All, then Delete (Move to Trash). Its Empty Trash runs KDE's empty-trash job, which `ktrash6 --empty` runs too; that was used for this step.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | Dolphin |
|---|---|---|
| Startup (launch to window shown) | 0.63 s | 0.69 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.66 s | 1.14 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.53 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | — | 1.48 s |
| Open a folder of 10,000 files (launch to the full list shown) | 0.69 s | 1.19 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.16 s | 0.44 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.16 s | 0.036 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.088 s | 0.23 s |
| 5,000 files arriving in an open folder (until all shown) | 0.20 s | 0.92 s |
| Copy 20,000 small files + 5 × 50 MB | 1.14 s | 29.77 s |
| Move the same to another drive | 1.47 s | 41.58 s |
| Move 10,000 files to the trash (all selected in one folder) | 2.14 s | 274.10 s |
| Empty the trash (those 10,000 files) | 0.48 s | 6.44 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.036 s / 0.035 s (idle 0.035 s) | 0.036 s / 0.035 s (idle 0.035 s) |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1430 ms | 6740 ms |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 160 ms | 1360 ms |
| Peak memory (startup / 600-image folder open) | 179 / 189 MB | 184 / 194 MB |
| Memory with 5 folders opened from other apps | 152 MB (109 MB as tabs) | 163 MB |
| CPU time used in 30 s with a folder open, idle | 0 ms (0.00% of a core) | 0 ms (0.00% of a core) |

Kestrel compared with Dolphin:
- startup: 1.1× faster;
- first thumbnails in a 600-image folder: 1.7× faster;
- first PDF thumbnails: —;
- listing a folder of 10,000 files: 1.7× faster;
- going to the folder of 10,000 files in an open window: 2.8× faster;
- going to a small folder in an open window: 4.4× slower;
- thumbnails at the end of the 600-image folder: 2.6× faster;
- showing files as they arrive: 4.6× faster;
- copying: about 26× faster;
- moving to another drive: about 28× faster;
- moving to the trash: about 128× faster;
- emptying the trash: about 13× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Dolphin made 500 thumbnails in all, the last 1.85 s after launch.
  - Kestrel makes them only for what's on screen (20 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.85 s for these 600 images (table below).
- **File operations:** Dolphin runs a paste or a move to the trash in the background, so its times were measured from the key press, by watching the files until the operation had finished, to within about 50 ms. Its window was given 2 s to list the folder first, which isn't counted.
- **Videos not timed:** its default settings leave video previews off, so Dolphin makes no thumbnails for them in a fresh home folder, and that row shows —.
- **Measured once:** Dolphin took over 2 minutes for the trash test, so it was run once instead of 3 times.
- **Memory:** Dolphin makes thumbnails in separate KIO worker processes, whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** Dolphin's search can't be timed from outside without its file indexer (Baloo), so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Dolphin's nearest equivalents (its folder previews and Information panel) can't be timed from outside, nor can its search.

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.85 s |
| Build 150 folder mosaics | 6.33 s |
| Recursive search over 50,000 files | 0.12 s |
| Read EXIF / image-generation prompts for 400 images | 0.046 s |
| Peak memory (background jobs) | 42–47 MB |

## Performance: Kestrel vs Thunar

Kestrel Explorer is compared here with Thunar 4.20.7, Xfce's file manager.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS, in a Docker container (Ubuntu's image with the parts of Ubuntu's desktop the apps use). The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, GTK's cairo renderer), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, and Thunar's own settings store and thumbnail service (xfconf, tumbler), so no file indexer ran. The benchmark is in [bench/](../bench) and is run with `bench/run.sh`.

### Compared with Thunar

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first 12 files' thumbnails are in the shared thumbnail cache.
- **Opening a folder of 10,000 files:** timed from launch until the window has stopped changing: the list drawn, with its count of items where the app shows one. The window is captured as often as it can be (every few ms), and the time is that of its last change, once nothing has changed for 2 s.
- **Going to a folder in an open window:** the folder's path typed into the location bar of a window that's already open, timed from Enter until the window has stopped changing, as above. Most folders are opened this way rather than by starting the app.
- **Jumping to the end of the 600-image folder:** once the first 12 thumbnails are there, End is pressed, timed until the last 6 files have thumbnails. With it, the CPU time used from launch to the last thumbnail, by the app and every helper it ran (thumbnailers, KIO workers, services), so an app that spreads the work over threads or processes can't look cheaper than it is.
- **Files arriving in an open folder:** 5,000 small files created at once in the folder a window shows, as a download, an unpacked archive or a build would, timed until the window has stopped changing, with the CPU time used meanwhile.
- **Window answering while it copies:** the 20,000 files + 250 MB copied and pasted in the app's window, and while the paste runs, Back and Forward pressed in turn, each timed until the window's title shows the folder it went to. The longest and the median wait, and in brackets the median with nothing running (what pressing a key and reading the title cost by themselves).
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. Thunar receives them through its D-Bus file-operations service, as when another app asks it to.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | Thunar |
|---|---|---|
| Startup (launch to window shown) | 0.50 s | 0.59 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.52 s | 1.35 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.38 s | 2.10 s |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.73 s | 1.40 s |
| Open a folder of 10,000 files (launch to the full list shown) | 0.55 s | 1.76 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.089 s | 2.02 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.053 s | 0.21 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.088 s | 0.29 s |
| 5,000 files arriving in an open folder (until all shown) | 0.20 s | 1.16 s |
| Copy 20,000 small files + 5 × 50 MB | 1.16 s | 7.66 s |
| Move the same to another drive | 1.47 s | 9.30 s |
| Move 10,000 files to the trash (all selected in one folder) | 2.19 s | 3.26 s |
| Empty the trash (those 10,000 files) | 0.48 s | 172.64 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.037 s / 0.036 s (idle 0.035 s) | 0.037 s / 0.035 s (idle 0.035 s) |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1390 ms | 2000 ms |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 180 ms | 1470 ms |
| Peak memory (startup / 600-image folder open) | 129 / 142 MB | 96 / 98 MB |
| Memory with 5 folders opened from other apps | 80 MB (71 MB as tabs) | 51 MB |
| CPU time used in 30 s with a folder open, idle | 5 ms (0.02% of a core) | 5 ms (0.02% of a core) |

Kestrel compared with Thunar:
- startup: 1.2× faster;
- first thumbnails in a 600-image folder: 2.6× faster;
- first video thumbnails: 1.5× faster;
- first PDF thumbnails: 1.9× faster;
- listing a folder of 10,000 files: 3.2× faster;
- going to the folder of 10,000 files in an open window: about 23× faster;
- going to a small folder in an open window: 4.0× faster;
- thumbnails at the end of the 600-image folder: 3.3× faster;
- showing files as they arrive: 5.8× faster;
- copying: 6.6× faster;
- moving to another drive: 6.3× faster;
- moving to the trash: 1.5× faster;
- emptying the trash: about 362× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Thunar made 15 thumbnails in all, the last 1.40 s after launch.
  - Kestrel makes them only for what's on screen (20 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.86 s for these 600 images (table below).
- **File operations:** Thunar's D-Bus service returns straight away, so its times were measured by watching the files until the operation had finished, to within about 50 ms.
- **Empty the trash:** Thunar asks first, even when asked through its D-Bus service; the question was answered at once (Return), and the time counted from that answer, as for Kestrel's.
- **Measured once:** Thunar took over 2 minutes for the trash test, so it was run once instead of 3 times.
- **Memory:** Thunar makes thumbnails in a separate thumbnail service (tumbler), whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** Thunar's search can't be timed from outside, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Thunar has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.86 s |
| Build 150 folder mosaics | 6.28 s |
| Recursive search over 50,000 files | 0.12 s |
| Read EXIF / image-generation prompts for 400 images | 0.046 s |
| Peak memory (background jobs) | 41–46 MB |

## Performance: Kestrel vs Krusader

Kestrel Explorer is compared here with Krusader 2.9.0, KDE's two-panel file manager.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS, in a Docker container (Ubuntu's image with the parts of Ubuntu's desktop the apps use). The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, both being Qt apps), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, so no file indexer ran. The benchmark is in [bench/](../bench) and is run with `bench/run.sh`.

### Compared with Krusader

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first 12 files' thumbnails are in the shared thumbnail cache.
- **Opening a folder of 10,000 files:** timed from launch until the window has stopped changing: the list drawn, with its count of items where the app shows one. The window is captured as often as it can be (every few ms), and the time is that of its last change, once nothing has changed for 2 s.
- **Going to a folder in an open window:** the folder's path typed into the location bar of a window that's already open, timed from Enter until the window has stopped changing, as above. Most folders are opened this way rather than by starting the app.
- **Jumping to the end of the 600-image folder:** once the first 12 thumbnails are there, End is pressed, timed until the last 6 files have thumbnails. With it, the CPU time used from launch to the last thumbnail, by the app and every helper it ran (thumbnailers, KIO workers, services), so an app that spreads the work over threads or processes can't look cheaper than it is.
- **Files arriving in an open folder:** 5,000 small files created at once in the folder a window shows, as a download, an unpacked archive or a build would, timed until the window has stopped changing, with the CPU time used meanwhile.
- **Window answering while it copies:** the 20,000 files + 250 MB copied and pasted in the app's window, and while the paste runs, Back and Forward pressed in turn, each timed until the window's title shows the folder it went to. The longest and the median wait, and in brackets the median with nothing running (what pressing a key and reading the title cost by themselves).
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. Krusader has no D-Bus file-operations service, so its own commands are used, with keys pressed in its window: the source in one panel and the destination in the other, Copy or Cut, then Paste in the other panel (its F5 Copy and F6 Move keys don't reach it on a bare X server; Paste runs the same KIO copy), and Select All, then Delete (Move to Trash). Its Empty Trash runs KDE's empty-trash job, which `ktrash6 --empty` runs too; that was used for this step.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | Krusader |
|---|---|---|
| Startup (launch to window shown) | 0.50 s | 1.27 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.52 s | — |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.38 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.73 s | — |
| Open a folder of 10,000 files (launch to the full list shown) | 0.55 s | 1.51 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.089 s | 0.16 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.053 s | 0.064 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.088 s | — |
| 5,000 files arriving in an open folder (until all shown) | 0.20 s | 0.58 s |
| Copy 20,000 small files + 5 × 50 MB | 1.16 s | 6.86 s |
| Move the same to another drive | 1.47 s | 17.21 s |
| Move 10,000 files to the trash (all selected in one folder) | 2.19 s | 283.31 s |
| Empty the trash (those 10,000 files) | 0.48 s | 6.88 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.037 s / 0.036 s (idle 0.035 s) | — |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1390 ms | — |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 180 ms | 190 ms |
| Peak memory (startup / 600-image folder open) | 129 / 142 MB | 151 / — MB |
| Memory with 5 folders opened from other apps | 80 MB (71 MB as tabs) | 100 MB |
| CPU time used in 30 s with a folder open, idle | 5 ms (0.02% of a core) | 0 ms (0.00% of a core) |

Kestrel compared with Krusader:
- startup: 2.5× faster;
- listing a folder of 10,000 files: 2.7× faster;
- going to the folder of 10,000 files in an open window: 1.8× faster;
- going to a small folder in an open window: 1.2× faster;
- showing files as they arrive: 2.9× faster;
- copying: 5.9× faster;
- moving to another drive: about 12× faster;
- moving to the trash: about 129× faster;
- emptying the trash: about 14× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Krusader wasn't timed on this (see below).
  - Kestrel makes them only for what's on screen (20 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.86 s for these 600 images (table below).
- **File operations:** Krusader runs a paste or a move to the trash in the background, so its times were measured from the key press, by watching the files until the operation had finished, to within about 50 ms. Its window was given 2 s to list the folder first, which isn't counted.
- **Thumbnails not timed:** Krusader shows no thumbnails with its default settings (previews are off), so the thumbnail tests are skipped for it and those rows show —, as does its peak memory with the 600-image folder open. How fast it opens a folder is compared by the folder of 10,000 files instead.
- **Window answering while it copies:** not run for Krusader, which pastes into its other panel rather than the folder it shows, so it can't be driven like the others; that row shows —.
- **Measured once:** Krusader took over 2 minutes for the trash test, so it was run once instead of 3 times.
- **Memory:** Krusader shows no thumbnails, so its figures include none; Kestrel's include those it made.
- **Search:** Krusader's search can't be timed from outside, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Krusader has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.86 s |
| Build 150 folder mosaics | 6.28 s |
| Recursive search over 50,000 files | 0.12 s |
| Read EXIF / image-generation prompts for 400 images | 0.046 s |
| Peak memory (background jobs) | 41–46 MB |

## Performance: Kestrel vs PCManFM

Kestrel Explorer is compared here with PCManFM 1.4.0, LXDE's file manager.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS, in a Docker container (Ubuntu's image with the parts of Ubuntu's desktop the apps use). The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, GTK's cairo renderer), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, so no file indexer ran. The benchmark is in [bench/](../bench) and is run with `bench/run.sh`.

### Compared with PCManFM

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first 12 files' thumbnails are in the shared thumbnail cache.
- **Opening a folder of 10,000 files:** timed from launch until the window has stopped changing: the list drawn, with its count of items where the app shows one. The window is captured as often as it can be (every few ms), and the time is that of its last change, once nothing has changed for 2 s.
- **Going to a folder in an open window:** the folder's path typed into the location bar of a window that's already open, timed from Enter until the window has stopped changing, as above. Most folders are opened this way rather than by starting the app.
- **Jumping to the end of the 600-image folder:** once the first 12 thumbnails are there, End is pressed, timed until the last 6 files have thumbnails. With it, the CPU time used from launch to the last thumbnail, by the app and every helper it ran (thumbnailers, KIO workers, services), so an app that spreads the work over threads or processes can't look cheaper than it is.
- **Files arriving in an open folder:** 5,000 small files created at once in the folder a window shows, as a download, an unpacked archive or a build would, timed until the window has stopped changing, with the CPU time used meanwhile.
- **Window answering while it copies:** the 20,000 files + 250 MB copied and pasted in the app's window, and while the paste runs, Back and Forward pressed in turn, each timed until the window's title shows the folder it went to. The longest and the median wait, and in brackets the median with nothing running (what pressing a key and reading the title cost by themselves).
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. PCManFM has no D-Bus file-operations service, so its own commands are used, with keys pressed in its window: Copy or Cut, then Paste in the destination (typed into its location bar), and Select All, then Delete (Move to Trash). To empty the trash, its window was taken to the trash (`trash:///`), and everything there selected and deleted with Shift+Delete, which removes them through the same gvfs trash as its Empty Trash.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | PCManFM |
|---|---|---|
| Startup (launch to window shown) | 0.50 s | 0.51 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.52 s | — |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.38 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.73 s | — |
| Open a folder of 10,000 files (launch to the full list shown) | 0.55 s | 0.55 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.089 s | 0.22 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.053 s | 0.22 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.088 s | — |
| 5,000 files arriving in an open folder (until all shown) | 0.20 s | 0.57 s |
| Copy 20,000 small files + 5 × 50 MB | 1.16 s | 3.57 s |
| Move the same to another drive | 1.47 s | 6.26 s |
| Move 10,000 files to the trash (all selected in one folder) | 2.19 s | 14.05 s |
| Empty the trash (those 10,000 files) | 0.48 s | 23.96 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.037 s / 0.036 s (idle 0.035 s) | 0.037 s / 0.036 s (idle 0.035 s) |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1390 ms | — |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 180 ms | 970 ms |
| Peak memory (startup / 600-image folder open) | 129 / 142 MB | 94 / — MB |
| Memory with 5 folders opened from other apps | 80 MB (71 MB as tabs) | 45 MB (as tabs) |
| CPU time used in 30 s with a folder open, idle | 5 ms (0.02% of a core) | 5 ms (0.02% of a core) |

Kestrel compared with PCManFM:
- startup: about the same;
- listing a folder of 10,000 files: about the same;
- going to the folder of 10,000 files in an open window: 2.5× faster;
- going to a small folder in an open window: 4.1× faster;
- showing files as they arrive: 2.8× faster;
- copying: 3.1× faster;
- moving to another drive: 4.3× faster;
- moving to the trash: 6.4× faster;
- emptying the trash: about 50× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - PCManFM wasn't timed on this (see below).
  - Kestrel makes them only for what's on screen (20 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.86 s for these 600 images (table below).
- **File operations:** PCManFM runs a paste or a move to the trash in the background, so its times were measured from the key press, by watching the files until the operation had finished, to within about 50 ms. Its window was given 2 s to list the folder first, which isn't counted.
- **Thumbnails not timed:** PCManFM shows thumbnails but keeps them to itself, never saving them to the shared thumbnail cache the test watches, so the thumbnail tests are skipped for it and those rows show —, as does its peak memory with the 600-image folder open. How fast it opens a folder is compared by the folder of 10,000 files instead.
- **Several folders:** PCManFM opens folders from other apps as tabs in its window (its default), so its figure is for one window with 5 tabs, like Kestrel's "as tabs" one.
- **Memory:** PCManFM makes its thumbnails inside the app, as Kestrel does.
- **Search:** PCManFM's search can't be timed from outside, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because PCManFM has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.86 s |
| Build 150 folder mosaics | 6.28 s |
| Recursive search over 50,000 files | 0.12 s |
| Read EXIF / image-generation prompts for 400 images | 0.046 s |
| Peak memory (background jobs) | 41–46 MB |

