# Performance

[← Back to the README](../README.md)

**Short on time?** Read the [performance summary](performance-summary.md).

## At a glance

Every comparison below in one table; the sections after it have the details, notes and the limits of each comparison.
Kestrel, GNOME Files, Dolphin, Thunar, Krusader and PCManFM were measured in the same run, on the same machine
(Ubuntu). After the first divider: Kestrel and Nemo, measured earlier on Linux Mint 22 in a virtual machine, with
Kestrel 0.2.1-alpha.3, before the tests in an open window existed. After the second: Kestrel and Dolphin on Kubuntu
26.04, in a container on the Ubuntu machine, with KDE's Qt integration loaded as on a Plasma desktop. Compare each pair
after a divider with each other, not with the other columns. "—" is a test that couldn't be run on that app (see its
section): Krusader and PCManFM make no thumbnails the benchmark can time, so the folder of 10,000 files compares how
fast they open a folder. Lower is better throughout.

| Test | Kestrel | GNOME Files | Dolphin | Thunar | Krusader | PCManFM | │ | Kestrel (Mint) | Nemo (Mint) | │ | Kestrel (Kubuntu) | Dolphin (Kubuntu) |
| --- | --- | --- | --- | --- | --- | --- | :-: | --- | --- | :-: | --- | --- |
| Startup (launch to window shown) | 0.37 s | 0.37 s | 0.40 s | 0.44 s | 0.93 s | 0.41 s | │ | 0.75 s | 0.82 s | │ | 0.60 s | 0.66 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | 1.17 s | 0.87 s | 5.58 s | — | — | │ | 0.88 s | — | │ | 0.68 s | 1.09 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 2.25 s | 7.71 s | — | 11.85 s | — | — | │ | 4.55 s | — | │ | 2.50 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.85 s | 0.80 s | 1.18 s | 5.88 s | — | — | │ | 1.52 s | — | │ | — | 1.49 s |
| Open a folder of 10,000 files (launch to the full list shown) | 0.51 s | 1.16 s | 0.96 s | 1.66 s | 1.18 s | 0.46 s | │ | — | — | │ | 0.75 s | 1.19 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.23 s | 0.72 s | 0.40 s | 2.04 s | 0.12 s | 0.21 s | │ | — | — | │ | 0.18 s | 0.43 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.042 s | 0.35 s | 0.048 s | 0.21 s | 0.046 s | 0.21 s | │ | — | — | │ | 0.14 s | 0.031 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.016 s | 8.62 s | 0.24 s | 0.29 s | — | — | │ | — | — | │ | 0.016 s | 0.22 s |
| 5,000 files arriving in an open folder (until all shown) | 0.23 s | 0.44 s | 0.87 s | 1.07 s | 0.53 s | 0.66 s | │ | — | — | │ | 0.25 s | 0.89 s |
| Copy 20,000 small files + 5 × 50 MB | 1.15 s | 2.20 s | 26.89 s | 6.80 s | 5.12 s | 2.58 s | │ | 1.71 s | 34.87 s | │ | 1.26 s | 29.74 s |
| Move the same to another drive | 1.35 s | 4.35 s | 42.41 s | 8.52 s | 14.30 s | 4.97 s | │ | 1.62 s | 8.70 s | │ | 1.56 s | 39.38 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.66 s | 1.86 s | 188.04 s | 2.54 s | 196.63 s | 11.81 s | │ | 3.23 s | — | │ | 2.17 s | 278.83 s |
| Empty the trash (those 10,000 files) | 0.43 s | 7.39 s | 4.86 s | 177.42 s | 4.40 s | 19.22 s | │ | 1.01 s | 365.30 s | │ | 0.48 s | 5.90 s |
| Longest wait for the window to answer while it copies (Back/Forward) | 0.035 s | 0.035 s | 0.036 s | 0.036 s | — | 0.035 s | │ | — | — | │ | 0.035 s | 0.035 s |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1070 ms | 20270 ms | 6310 ms | 8580 ms | — | — | │ | — | — | │ | 1330 ms | 6610 ms |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 440 ms | 1160 ms | 720 ms | 6410 ms | 170 ms | 970 ms | │ | — | — | │ | 430 ms | 1320 ms |
| Peak memory (startup / 600-image folder open) | 144 / 154 MB | 192 / 300 MB | 181 / 193 MB | 96 / 100 MB | 156 / — MB | 96 / — MB | │ | 116 / 125 MB | 123 / — MB | │ | 171 / 183 MB | 173 / 184 MB |
| Memory with 5 folders opened from other apps | 108 MB (36 MB as tabs) | 127 MB | 133 MB | 29 MB | 113 MB | 25 MB (as tabs) | │ | 58 MB (36 MB as tabs) | 51 MB | │ | 152 MB (106 MB as tabs) | 162 MB |
| CPU time used in 30 s with a folder open, idle | 5 ms | 0 ms | 5 ms | 10 ms | 0 ms | 0 ms | │ | 35 ms | 7510 ms | │ | 10 ms | 0 ms |

## Performance: Kestrel vs GNOME Files

Kestrel Explorer is compared here with GNOME Files 50.2.2, the file manager it replaces.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS. The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

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
| Startup (launch to window shown) | 0.37 s | 0.37 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | 1.17 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 2.25 s | 7.71 s |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.85 s | 0.80 s |
| Open a folder of 10,000 files (launch to the full list shown) | 0.51 s | 1.16 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.23 s | 0.72 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.042 s | 0.35 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.016 s | 8.62 s |
| 5,000 files arriving in an open folder (until all shown) | 0.23 s | 0.44 s |
| Copy 20,000 small files + 5 × 50 MB | 1.15 s | 2.20 s |
| Move the same to another drive | 1.35 s | 4.35 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.66 s | 1.86 s |
| Empty the trash (those 10,000 files) | 0.43 s | 7.39 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.035 s / 0.033 s (idle 0.033 s) | 0.035 s / 0.033 s (idle 0.034 s) |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1070 ms | 20270 ms |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 440 ms | 1160 ms |
| Peak memory (startup / 600-image folder open) | 144 / 154 MB | 192 / 300 MB |
| Memory with 5 folders opened from other apps | 108 MB (36 MB as tabs) | 127 MB |
| CPU time used in 30 s with a folder open, idle | 5 ms (0.02% of a core) | 0 ms (0.00% of a core) |

Kestrel compared with GNOME Files:
- startup: about the same;
- first thumbnails in a 600-image folder: 2.9× faster;
- first video thumbnails: 3.4× faster;
- first PDF thumbnails: 1.1× slower;
- listing a folder of 10,000 files: 2.3× faster;
- going to the folder of 10,000 files in an open window: 3.2× faster;
- going to a small folder in an open window: 8.4× faster;
- thumbnails at the end of the 600-image folder: about 526× faster;
- showing files as they arrive: 1.9× faster;
- copying: 1.9× faster;
- moving to another drive: 3.2× faster;
- moving to the trash: 1.1× faster;
- emptying the trash: about 17× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - GNOME Files makes thumbnails for the whole folder in the background. It finished all 600 images 10.25 s after launch.
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.71 s for these 600 images (table below).
- **File operations:** GNOME Files' D-Bus service returns straight away, so its times were measured by watching the files until the operation had finished, to within about 50 ms.
- **Jumping to the end:** Kestrel already had the last thumbnails when End was pressed (made when the folder opened, with the first screenful), so the time is only that of seeing them.
- **Jumping to the end:** GNOME Files makes thumbnails for the whole folder in the background, so some may be ready before End is pressed.
- **Memory:** GNOME Files makes thumbnails in separate sandboxed helper processes, whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** GNOME Files' search can't be timed from outside without its file indexer, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because GNOME Files has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.71 s |
| Build 150 folder mosaics | 6.16 s |
| Recursive search over 50,000 files | 0.11 s |
| Read EXIF / image-generation prompts for 400 images | 0.040 s |
| Peak memory (background jobs) | 41–47 MB |

## Performance: Kestrel vs Nemo

Kestrel Explorer is compared here with Nemo, the file manager it replaces.

These figures were measured with Kestrel 0.2.1-alpha.3 (2026-10-06) and haven't been re-run since; the GNOME Files
comparison above is newer.

**Test machine:** a VirtualBox virtual machine running Linux Mint 22 (Cinnamon). The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, GTK's cairo renderer), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, so no file indexer ran. The benchmark is in [bench/](../bench) and is run with `bench/run.sh`.

### Compared with Nemo

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first 12 files' thumbnails are in the shared thumbnail cache.
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. Nemo receives them through its D-Bus file-operations service, as when another app asks it to.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | Nemo |
|---|---|---|
| Startup (launch to window shown) | 0.75 s | 0.82 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.88 s | — |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 4.55 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 1.52 s | — |
| Copy 20,000 small files + 5 × 50 MB | 1.71 s | 34.87 s |
| Move the same to another drive | 1.62 s | 8.70 s |
| Move 10,000 files to the trash (all selected in one folder) | 3.23 s | — |
| Empty the trash (those 10,000 files) | 1.01 s | 365.30 s |
| Peak memory (startup / 600-image folder open) | 116 / 125 MB | 123 / — MB |
| Memory with 5 folders opened from other apps | 58 MB (36 MB as tabs) | 51 MB |
| CPU time used in 30 s with a folder open, idle | 35 ms (0.12% of a core) | 7510 ms (25.03% of a core) |

Kestrel compared with Nemo:
- startup: 1.1× faster;
- copying: about 20× faster;
- moving to another drive: 5.4× faster;
- emptying the trash: about 362× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Nemo wasn't timed on this (see below).
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 8.37 s for these 600 images (table below).
- **File operations:** Nemo's D-Bus service returns straight away, so its times were measured by watching the files until the operation had finished, to within about 50 ms.
- **Opening a folder not timed:** Nemo makes no thumbnails in this setup (a fresh home folder with its default settings), so the folder tests are skipped for it and those rows show —, as does its peak memory with the 600-image folder open.
- **Not timed:** Nemo's D-Bus file-operations service has no way to move files to the trash, so those rows show —. For "Empty the trash", the files were put in the trash with `gio trash` first.
- **CPU while idle:** Nemo kept about 25% of a core busy with nothing happening. That's unusual for a file manager at rest, so it probably comes from this headless setup (where it also made no thumbnails) rather than from everyday use.
- **Measured once:** Nemo took over 2 minutes for the trash test, so it was run once instead of 3 times. Its D-Bus service empties the trash one file at a time.
- **Memory:** Nemo makes thumbnails in separate helper processes, whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** Nemo's search can't be timed from outside, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Nemo has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 8.37 s |
| Build 150 folder mosaics | 11.12 s |
| Recursive search over 50,000 files | 0.17 s |
| Read EXIF / image-generation prompts for 400 images | 0.046 s |
| Peak memory (background jobs) | 27–31 MB |

## Performance: Kestrel vs Dolphin

Kestrel Explorer is compared here with Dolphin 25.12.3, KDE's file manager.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS. The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

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
| Startup (launch to window shown) | 0.37 s | 0.40 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | 0.87 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 2.25 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.85 s | 1.18 s |
| Open a folder of 10,000 files (launch to the full list shown) | 0.51 s | 0.96 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.23 s | 0.40 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.042 s | 0.048 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.016 s | 0.24 s |
| 5,000 files arriving in an open folder (until all shown) | 0.23 s | 0.87 s |
| Copy 20,000 small files + 5 × 50 MB | 1.15 s | 26.89 s |
| Move the same to another drive | 1.35 s | 42.41 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.66 s | 188.04 s |
| Empty the trash (those 10,000 files) | 0.43 s | 4.86 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.035 s / 0.033 s (idle 0.033 s) | 0.036 s / 0.033 s (idle 0.034 s) |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1070 ms | 6310 ms |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 440 ms | 720 ms |
| Peak memory (startup / 600-image folder open) | 144 / 154 MB | 181 / 193 MB |
| Memory with 5 folders opened from other apps | 108 MB (36 MB as tabs) | 133 MB |
| CPU time used in 30 s with a folder open, idle | 5 ms (0.02% of a core) | 5 ms (0.02% of a core) |

Kestrel compared with Dolphin:
- startup: 1.1× faster;
- first thumbnails in a 600-image folder: 2.2× faster;
- first PDF thumbnails: 1.4× faster;
- listing a folder of 10,000 files: 1.9× faster;
- going to the folder of 10,000 files in an open window: 1.8× faster;
- going to a small folder in an open window: 1.2× faster;
- thumbnails at the end of the 600-image folder: about 15× faster;
- showing files as they arrive: 3.8× faster;
- copying: about 23× faster;
- moving to another drive: about 31× faster;
- moving to the trash: about 113× faster;
- emptying the trash: about 11× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Dolphin made 500 thumbnails in all, the last 1.78 s after launch.
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.71 s for these 600 images (table below).
- **File operations:** Dolphin runs a paste or a move to the trash in the background, so its times were measured from the key press, by watching the files until the operation had finished, to within about 50 ms. Its window was given 2 s to list the folder first, which isn't counted.
- **Videos not timed:** its default settings leave video previews off, so Dolphin makes no thumbnails for them in a fresh home folder, and that row shows —.
- **Jumping to the end:** Kestrel already had the last thumbnails when End was pressed (made when the folder opened, with the first screenful), so the time is only that of seeing them.
- **Measured once:** Dolphin took over 2 minutes for the trash test, so it was run once instead of 3 times.
- **Memory:** Dolphin makes thumbnails in separate KIO worker processes, whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** Dolphin's search can't be timed from outside without its file indexer (Baloo), so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Dolphin's nearest equivalents (its folder previews and Information panel) can't be timed from outside, nor can its search.

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.71 s |
| Build 150 folder mosaics | 6.16 s |
| Recursive search over 50,000 files | 0.11 s |
| Read EXIF / image-generation prompts for 400 images | 0.040 s |
| Peak memory (background jobs) | 41–47 MB |

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
| Startup (launch to window shown) | 0.60 s | 0.66 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.68 s | 1.09 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 2.50 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | — | 1.49 s |
| Open a folder of 10,000 files (launch to the full list shown) | 0.75 s | 1.19 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.18 s | 0.43 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.14 s | 0.031 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.016 s | 0.22 s |
| 5,000 files arriving in an open folder (until all shown) | 0.25 s | 0.89 s |
| Copy 20,000 small files + 5 × 50 MB | 1.26 s | 29.74 s |
| Move the same to another drive | 1.56 s | 39.38 s |
| Move 10,000 files to the trash (all selected in one folder) | 2.17 s | 278.83 s |
| Empty the trash (those 10,000 files) | 0.48 s | 5.90 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.035 s / 0.033 s (idle 0.034 s) | 0.035 s / 0.033 s (idle 0.034 s) |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1330 ms | 6610 ms |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 430 ms | 1320 ms |
| Peak memory (startup / 600-image folder open) | 171 / 183 MB | 173 / 184 MB |
| Memory with 5 folders opened from other apps | 152 MB (106 MB as tabs) | 162 MB |
| CPU time used in 30 s with a folder open, idle | 10 ms (0.03% of a core) | 0 ms (0.00% of a core) |

Kestrel compared with Dolphin:
- startup: 1.1× faster;
- first thumbnails in a 600-image folder: 1.6× faster;
- first PDF thumbnails: —;
- listing a folder of 10,000 files: 1.6× faster;
- going to the folder of 10,000 files in an open window: 2.4× faster;
- going to a small folder in an open window: 4.6× slower;
- thumbnails at the end of the 600-image folder: about 14× faster;
- showing files as they arrive: 3.6× faster;
- copying: about 24× faster;
- moving to another drive: about 25× faster;
- moving to the trash: about 129× faster;
- emptying the trash: about 12× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Dolphin made 500 thumbnails in all, the last 1.85 s after launch.
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.75 s for these 600 images (table below).
- **File operations:** Dolphin runs a paste or a move to the trash in the background, so its times were measured from the key press, by watching the files until the operation had finished, to within about 50 ms. Its window was given 2 s to list the folder first, which isn't counted.
- **Videos not timed:** its default settings leave video previews off, so Dolphin makes no thumbnails for them in a fresh home folder, and that row shows —.
- **Jumping to the end:** Kestrel already had the last thumbnails when End was pressed (made when the folder opened, with the first screenful), so the time is only that of seeing them.
- **Measured once:** Dolphin took over 2 minutes for the trash test, so it was run once instead of 3 times.
- **Memory:** Dolphin makes thumbnails in separate KIO worker processes, whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** Dolphin's search can't be timed from outside without its file indexer (Baloo), so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Dolphin's nearest equivalents (its folder previews and Information panel) can't be timed from outside, nor can its search.

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.75 s |
| Build 150 folder mosaics | 6.24 s |
| Recursive search over 50,000 files | 0.10 s |
| Read EXIF / image-generation prompts for 400 images | 0.047 s |
| Peak memory (background jobs) | 40–45 MB |

## Performance: Kestrel vs Thunar

Kestrel Explorer is compared here with Thunar 4.20.7, Xfce's file manager.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS. The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

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
| Startup (launch to window shown) | 0.37 s | 0.44 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | 5.58 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 2.25 s | 11.85 s |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.85 s | 5.88 s |
| Open a folder of 10,000 files (launch to the full list shown) | 0.51 s | 1.66 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.23 s | 2.04 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.042 s | 0.21 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.016 s | 0.29 s |
| 5,000 files arriving in an open folder (until all shown) | 0.23 s | 1.07 s |
| Copy 20,000 small files + 5 × 50 MB | 1.15 s | 6.80 s |
| Move the same to another drive | 1.35 s | 8.52 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.66 s | 2.54 s |
| Empty the trash (those 10,000 files) | 0.43 s | 177.42 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.035 s / 0.033 s (idle 0.033 s) | 0.036 s / 0.032 s (idle 0.032 s) |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1070 ms | 8580 ms |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 440 ms | 6410 ms |
| Peak memory (startup / 600-image folder open) | 144 / 154 MB | 96 / 100 MB |
| Memory with 5 folders opened from other apps | 108 MB (36 MB as tabs) | 29 MB |
| CPU time used in 30 s with a folder open, idle | 5 ms (0.02% of a core) | 10 ms (0.03% of a core) |

Kestrel compared with Thunar:
- startup: 1.2× faster;
- first thumbnails in a 600-image folder: about 14× faster;
- first video thumbnails: 5.3× faster;
- first PDF thumbnails: 6.9× faster;
- listing a folder of 10,000 files: 3.2× faster;
- going to the folder of 10,000 files in an open window: 9.0× faster;
- going to a small folder in an open window: 5.1× faster;
- thumbnails at the end of the 600-image folder: about 18× faster;
- showing files as they arrive: 4.6× faster;
- copying: 5.9× faster;
- moving to another drive: 6.3× faster;
- moving to the trash: 1.5× faster;
- emptying the trash: about 416× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Thunar made 12 thumbnails in all, the last 5.58 s after launch.
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.71 s for these 600 images (table below).
- **File operations:** Thunar's D-Bus service returns straight away, so its times were measured by watching the files until the operation had finished, to within about 50 ms.
- **Empty the trash:** Thunar asks first, even when asked through its D-Bus service; the question was answered at once (Return), and the time counted from that answer, as for Kestrel's.
- **Jumping to the end:** Kestrel already had the last thumbnails when End was pressed (made when the folder opened, with the first screenful), so the time is only that of seeing them.
- **Measured once:** Thunar took over 2 minutes for the trash test, so it was run once instead of 3 times.
- **Memory:** Thunar makes thumbnails in a separate thumbnail service (tumbler), whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** Thunar's search can't be timed from outside, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Thunar has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.71 s |
| Build 150 folder mosaics | 6.16 s |
| Recursive search over 50,000 files | 0.11 s |
| Read EXIF / image-generation prompts for 400 images | 0.040 s |
| Peak memory (background jobs) | 41–47 MB |

## Performance: Kestrel vs Krusader

Kestrel Explorer is compared here with Krusader 2.9.0, KDE's two-panel file manager.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS. The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

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
| Startup (launch to window shown) | 0.37 s | 0.93 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | — |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 2.25 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.85 s | — |
| Open a folder of 10,000 files (launch to the full list shown) | 0.51 s | 1.18 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.23 s | 0.12 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.042 s | 0.046 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.016 s | — |
| 5,000 files arriving in an open folder (until all shown) | 0.23 s | 0.53 s |
| Copy 20,000 small files + 5 × 50 MB | 1.15 s | 5.12 s |
| Move the same to another drive | 1.35 s | 14.30 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.66 s | 196.63 s |
| Empty the trash (those 10,000 files) | 0.43 s | 4.40 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.035 s / 0.033 s (idle 0.033 s) | — |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1070 ms | — |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 440 ms | 170 ms |
| Peak memory (startup / 600-image folder open) | 144 / 154 MB | 156 / — MB |
| Memory with 5 folders opened from other apps | 108 MB (36 MB as tabs) | 113 MB |
| CPU time used in 30 s with a folder open, idle | 5 ms (0.02% of a core) | 0 ms (0.00% of a core) |

Kestrel compared with Krusader:
- startup: 2.5× faster;
- listing a folder of 10,000 files: 2.3× faster;
- going to the folder of 10,000 files in an open window: 1.9× slower;
- going to a small folder in an open window: 1.1× faster;
- showing files as they arrive: 2.3× faster;
- copying: 4.4× faster;
- moving to another drive: about 11× faster;
- moving to the trash: about 118× faster;
- emptying the trash: about 10× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Krusader wasn't timed on this (see below).
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.71 s for these 600 images (table below).
- **File operations:** Krusader runs a paste or a move to the trash in the background, so its times were measured from the key press, by watching the files until the operation had finished, to within about 50 ms. Its window was given 2 s to list the folder first, which isn't counted.
- **Thumbnails not timed:** Krusader shows no thumbnails with its default settings (previews are off), so the thumbnail tests are skipped for it and those rows show —, as does its peak memory with the 600-image folder open. How fast it opens a folder is compared by the folder of 10,000 files instead.
- **Window answering while it copies:** not run for Krusader, which pastes into its other panel rather than the folder it shows, so it can't be driven like the others; that row shows —.
- **Jumping to the end:** Kestrel already had the last thumbnails when End was pressed (made when the folder opened, with the first screenful), so the time is only that of seeing them.
- **Measured once:** Krusader took over 2 minutes for the trash test, so it was run once instead of 3 times.
- **Memory:** Krusader shows no thumbnails, so its figures include none; Kestrel's include those it made.
- **Search:** Krusader's search can't be timed from outside, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Krusader has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.71 s |
| Build 150 folder mosaics | 6.16 s |
| Recursive search over 50,000 files | 0.11 s |
| Read EXIF / image-generation prompts for 400 images | 0.040 s |
| Peak memory (background jobs) | 41–47 MB |

## Performance: Kestrel vs PCManFM

Kestrel Explorer is compared here with PCManFM 1.4.0, LXDE's file manager.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS. The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

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
| Startup (launch to window shown) | 0.37 s | 0.41 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | — |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 2.25 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.85 s | — |
| Open a folder of 10,000 files (launch to the full list shown) | 0.51 s | 0.46 s |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | 0.23 s | 0.21 s |
| Go to a folder of 100 files in an open window (Enter to the list shown) | 0.042 s | 0.21 s |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | 0.016 s | — |
| 5,000 files arriving in an open folder (until all shown) | 0.23 s | 0.66 s |
| Copy 20,000 small files + 5 × 50 MB | 1.15 s | 2.58 s |
| Move the same to another drive | 1.35 s | 4.97 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.66 s | 11.81 s |
| Empty the trash (those 10,000 files) | 0.43 s | 19.22 s |
| Window answering while it copies (Back/Forward to the new title: longest / median) | 0.035 s / 0.033 s (idle 0.033 s) | 0.035 s / 0.033 s (idle 0.033 s) |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | 1070 ms | — |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | 440 ms | 970 ms |
| Peak memory (startup / 600-image folder open) | 144 / 154 MB | 96 / — MB |
| Memory with 5 folders opened from other apps | 108 MB (36 MB as tabs) | 25 MB (as tabs) |
| CPU time used in 30 s with a folder open, idle | 5 ms (0.02% of a core) | 0 ms (0.00% of a core) |

Kestrel compared with PCManFM:
- startup: 1.1× faster;
- listing a folder of 10,000 files: 1.1× slower;
- going to the folder of 10,000 files in an open window: 1.1× slower;
- going to a small folder in an open window: 5.1× faster;
- showing files as they arrive: 2.9× faster;
- copying: 2.2× faster;
- moving to another drive: 3.7× faster;
- moving to the trash: 7.1× faster;
- emptying the trash: about 45× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - PCManFM wasn't timed on this (see below).
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.71 s for these 600 images (table below).
- **File operations:** PCManFM runs a paste or a move to the trash in the background, so its times were measured from the key press, by watching the files until the operation had finished, to within about 50 ms. Its window was given 2 s to list the folder first, which isn't counted.
- **Thumbnails not timed:** PCManFM shows thumbnails but keeps them to itself, never saving them to the shared thumbnail cache the test watches, so the thumbnail tests are skipped for it and those rows show —, as does its peak memory with the 600-image folder open. How fast it opens a folder is compared by the folder of 10,000 files instead.
- **Jumping to the end:** Kestrel already had the last thumbnails when End was pressed (made when the folder opened, with the first screenful), so the time is only that of seeing them.
- **Several folders:** PCManFM opens folders from other apps as tabs in its window (its default), so its figure is for one window with 5 tabs, like Kestrel's "as tabs" one.
- **Memory:** PCManFM makes its thumbnails inside the app, as Kestrel does.
- **Search:** PCManFM's search can't be timed from outside, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because PCManFM has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.71 s |
| Build 150 folder mosaics | 6.16 s |
| Recursive search over 50,000 files | 0.11 s |
| Read EXIF / image-generation prompts for 400 images | 0.040 s |
| Peak memory (background jobs) | 41–47 MB |
