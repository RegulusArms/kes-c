# Performance

[← Back to the README](../README.md)

**Short on time?** Read the [performance summary](performance-summary.md).

## At a glance

Every comparison below in one table; the sections after it have the details, notes and the limits of each comparison.
Kestrel, GNOME Files and Dolphin were measured in the same run, on the same machine (Ubuntu); Thunar, Krusader and
PCManFM, and every app's folder of 10,000 files, a day later on that machine. After the divider: Kestrel and Nemo,
measured earlier on Linux Mint 22 in a virtual machine, with Kestrel 0.2.1-alpha.3. Compare those two with each other,
not with the columns before the divider: it's another machine. "—" is a test that couldn't be run on that app (see its
section): Krusader and PCManFM make no thumbnails the benchmark can time, so the folder of 10,000 files compares how
fast they open a folder. Lower is better throughout.

| Test | Kestrel | GNOME Files | Dolphin | Thunar | Krusader | PCManFM | │ | Kestrel (Mint) | Nemo (Mint) |
| --- | --- | --- | --- | --- | --- | --- | :-: | --- | --- |
| Startup (launch to window shown) | 0.39 s | 0.40 s | 0.41 s | 0.86 s | 0.94 s | 0.42 s | │ | 0.75 s | 0.82 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | 1.22 s | 0.86 s | 6.66 s | — | — | │ | 0.88 s | — |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.75 s | 7.87 s | — | 20.17 s | — | — | │ | 4.55 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.73 s | 0.86 s | 1.22 s | 10.81 s | — | — | │ | 1.52 s | — |
| Open a folder of 10,000 files (launch to the full list shown) | 0.55 s | 1.32 s | 1.00 s | 1.69 s | 1.19 s | 0.48 s | │ | — | — |
| Copy 20,000 small files + 5 × 50 MB | 1.02 s | 2.25 s | 28.58 s | 8.49 s | 8.11 s | 2.89 s | │ | 1.71 s | 34.87 s |
| Move the same to another drive | 1.26 s | 4.41 s | 40.88 s | 8.52 s | 16.22 s | 7.13 s | │ | 1.62 s | 8.70 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.59 s | 1.85 s | 187.65 s | 3.75 s | 195.41 s | 13.18 s | │ | 3.23 s | — |
| Empty the trash (those 10,000 files) | 0.41 s | 8.54 s | 5.45 s | 181.62 s | 4.80 s | 23.75 s | │ | 1.01 s | 365.30 s |
| Peak memory (startup / 600-image folder open) | 143 / 153 MB | 194 / 302 MB | 182 / 194 MB | 97 / 101 MB | 155 / — MB | 97 / — MB | │ | 116 / 125 MB | 123 / — MB |
| Memory with 5 folders opened from other apps | 105 MB (51 MB as tabs) | 99 MB | 130 MB | 26 MB | 111 MB | 22 MB (as tabs) | │ | 58 MB (36 MB as tabs) | 51 MB |
| CPU time in 30 s, idle with a folder open | 10 ms | 5 ms | 0 ms | 15 ms | 0 ms | 10 ms | │ | 35 ms | 7510 ms |

## Performance: Kestrel vs GNOME Files

Kestrel Explorer is compared here with GNOME Files 50.2.2, the file manager it replaces.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS. The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, GTK's cairo renderer), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, so no file indexer ran. The benchmark is in [bench/](../bench) and is run with `bench/run.sh`.

### Compared with GNOME Files

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first 12 files' thumbnails are in the shared thumbnail cache.
- **Opening a folder of 10,000 files:** timed from launch until the window has stopped changing: the list drawn, with its count of items where the app shows one. The window is captured as often as it can be (every few ms), and the time is that of its last change, once nothing has changed for 2 s.
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. GNOME Files receives them through its D-Bus file-operations service, as when another app asks it to.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | GNOME Files |
|---|---|---|
| Startup (launch to window shown) | 0.39 s | 0.40 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | 1.22 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.75 s | 7.87 s |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.73 s | 0.86 s |
| Open a folder of 10,000 files (launch to the full list shown) | 0.55 s | 1.32 s |
| Copy 20,000 small files + 5 × 50 MB | 1.02 s | 2.25 s |
| Move the same to another drive | 1.26 s | 4.41 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.59 s | 1.85 s |
| Empty the trash (those 10,000 files) | 0.41 s | 8.54 s |
| Peak memory (startup / 600-image folder open) | 143 / 153 MB | 194 / 302 MB |
| Memory with 5 folders opened from other apps | 105 MB (51 MB as tabs) | 99 MB |
| CPU time used in 30 s with a folder open, idle | 10 ms (0.03% of a core) | 5 ms (0.02% of a core) |

Kestrel compared with GNOME Files:
- startup: about the same;
- first thumbnails in a 600-image folder: 3.1× faster;
- first video thumbnails: 4.5× faster;
- first PDF thumbnails: 1.2× faster;
- listing a folder of 10,000 files: 2.4× faster;
- copying: 2.2× faster;
- moving to another drive: 3.5× faster;
- moving to the trash: 1.2× faster;
- emptying the trash: about 21× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - GNOME Files makes thumbnails for the whole folder in the background. It finished all 600 images 10.46 s after launch.
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.87 s for these 600 images (table below).
- **File operations:** GNOME Files' D-Bus service returns straight away, so its times were measured by watching the files until the operation had finished, to within about 50 ms.
- **Memory:** GNOME Files makes thumbnails in separate sandboxed helper processes, whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** GNOME Files' search can't be timed from outside without its file indexer, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because GNOME Files has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.87 s |
| Build 150 folder mosaics | 6.20 s |
| Recursive search over 50,000 files | 0.10 s |
| Read EXIF / image-generation prompts for 400 images | 0.038 s |
| Peak memory (background jobs) | 40–45 MB |

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
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. Dolphin has no D-Bus file-operations service, so its own commands are used, with keys pressed in its window: Copy or Cut, then Paste in the destination (typed into its location bar), and Select All, then Delete (Move to Trash). Its Empty Trash runs KDE's empty-trash job, which `ktrash6 --empty` runs too; that was used for this step.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | Dolphin |
|---|---|---|
| Startup (launch to window shown) | 0.39 s | 0.41 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | 0.86 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.75 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.73 s | 1.22 s |
| Open a folder of 10,000 files (launch to the full list shown) | 0.55 s | 1.00 s |
| Copy 20,000 small files + 5 × 50 MB | 1.02 s | 28.58 s |
| Move the same to another drive | 1.26 s | 40.88 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.59 s | 187.65 s |
| Empty the trash (those 10,000 files) | 0.41 s | 5.45 s |
| Peak memory (startup / 600-image folder open) | 143 / 153 MB | 182 / 194 MB |
| Memory with 5 folders opened from other apps | 105 MB (51 MB as tabs) | 130 MB |
| CPU time used in 30 s with a folder open, idle | 10 ms (0.03% of a core) | 0 ms (0.00% of a core) |

Kestrel compared with Dolphin:
- startup: about the same;
- first thumbnails in a 600-image folder: 2.2× faster;
- first PDF thumbnails: 1.7× faster;
- listing a folder of 10,000 files: 1.8× faster;
- copying: about 28× faster;
- moving to another drive: about 33× faster;
- moving to the trash: about 118× faster;
- emptying the trash: about 13× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Dolphin made 500 thumbnails in all, the last 1.85 s after launch.
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.87 s for these 600 images (table below).
- **File operations:** Dolphin runs a paste or a move to the trash in the background, so its times were measured from the key press, by watching the files until the operation had finished, to within about 50 ms. Its window was given 2 s to list the folder first, which isn't counted.
- **Videos not timed:** its default settings leave video previews off, so Dolphin makes no thumbnails for them in a fresh home folder, and that row shows —.
- **Measured once:** Dolphin took over 2 minutes for the trash test, so it was run once instead of 3 times.
- **Memory:** Dolphin makes thumbnails in separate KIO worker processes, whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** Dolphin's search can't be timed from outside without its file indexer (Baloo), so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Dolphin's nearest equivalents (its folder previews and Information panel) can't be timed from outside, nor can its search.

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.87 s |
| Build 150 folder mosaics | 6.20 s |
| Recursive search over 50,000 files | 0.10 s |
| Read EXIF / image-generation prompts for 400 images | 0.038 s |
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
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. Thunar receives them through its D-Bus file-operations service, as when another app asks it to.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | Thunar |
|---|---|---|
| Startup (launch to window shown) | 0.39 s | 0.86 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | 6.66 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.75 s | 20.17 s |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.73 s | 10.81 s |
| Open a folder of 10,000 files (launch to the full list shown) | 0.55 s | 1.69 s |
| Copy 20,000 small files + 5 × 50 MB | 1.02 s | 8.49 s |
| Move the same to another drive | 1.26 s | 8.52 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.59 s | 3.75 s |
| Empty the trash (those 10,000 files) | 0.41 s | 181.62 s |
| Peak memory (startup / 600-image folder open) | 143 / 153 MB | 97 / 101 MB |
| Memory with 5 folders opened from other apps | 105 MB (51 MB as tabs) | 26 MB |
| CPU time used in 30 s with a folder open, idle | 10 ms (0.03% of a core) | 15 ms (0.05% of a core) |

Kestrel compared with Thunar:
- startup: 2.2× faster;
- first thumbnails in a 600-image folder: about 17× faster;
- first video thumbnails: about 12× faster;
- first PDF thumbnails: about 15× faster;
- listing a folder of 10,000 files: 3.1× faster;
- copying: 8.3× faster;
- moving to another drive: 6.8× faster;
- moving to the trash: 2.4× faster;
- emptying the trash: about 442× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Thunar made 12 thumbnails in all, the last 6.66 s after launch.
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.87 s for these 600 images (table below).
- **File operations:** Thunar's D-Bus service returns straight away, so its times were measured by watching the files until the operation had finished, to within about 50 ms.
- **Empty the trash:** Thunar asks first, even when asked through its D-Bus service; the question was answered at once (Return), and the time counted from that answer, as for Kestrel's.
- **Measured once:** Thunar took over 2 minutes for the trash test, so it was run once instead of 3 times.
- **Memory:** Thunar makes thumbnails in a separate thumbnail service (tumbler), whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** Thunar's search can't be timed from outside, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Thunar has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.87 s |
| Build 150 folder mosaics | 6.20 s |
| Recursive search over 50,000 files | 0.10 s |
| Read EXIF / image-generation prompts for 400 images | 0.038 s |
| Peak memory (background jobs) | 40–45 MB |

## Performance: Kestrel vs Krusader

Kestrel Explorer is compared here with Krusader 2.9.0, KDE's two-panel file manager.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS. The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, both being Qt apps), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, so no file indexer ran. The benchmark is in [bench/](../bench) and is run with `bench/run.sh`.

### Compared with Krusader

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first 12 files' thumbnails are in the shared thumbnail cache.
- **Opening a folder of 10,000 files:** timed from launch until the window has stopped changing: the list drawn, with its count of items where the app shows one. The window is captured as often as it can be (every few ms), and the time is that of its last change, once nothing has changed for 2 s.
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. Krusader has no D-Bus file-operations service, so its own commands are used, with keys pressed in its window: the source in one panel and the destination in the other, Copy or Cut, then Paste in the other panel (its F5 Copy and F6 Move keys don't reach it on a bare X server; Paste runs the same KIO copy), and Select All, then Delete (Move to Trash). Its Empty Trash runs KDE's empty-trash job, which `ktrash6 --empty` runs too; that was used for this step.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | Krusader |
|---|---|---|
| Startup (launch to window shown) | 0.39 s | 0.94 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | — |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.75 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.73 s | — |
| Open a folder of 10,000 files (launch to the full list shown) | 0.55 s | 1.19 s |
| Copy 20,000 small files + 5 × 50 MB | 1.02 s | 8.11 s |
| Move the same to another drive | 1.26 s | 16.22 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.59 s | 195.41 s |
| Empty the trash (those 10,000 files) | 0.41 s | 4.80 s |
| Peak memory (startup / 600-image folder open) | 143 / 153 MB | 155 / — MB |
| Memory with 5 folders opened from other apps | 105 MB (51 MB as tabs) | 111 MB |
| CPU time used in 30 s with a folder open, idle | 10 ms (0.03% of a core) | 0 ms (0.00% of a core) |

Kestrel compared with Krusader:
- startup: 2.4× faster;
- listing a folder of 10,000 files: 2.2× faster;
- copying: 7.9× faster;
- moving to another drive: about 13× faster;
- moving to the trash: about 123× faster;
- emptying the trash: about 12× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - Krusader wasn't timed on this (see below).
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.87 s for these 600 images (table below).
- **File operations:** Krusader runs a paste or a move to the trash in the background, so its times were measured from the key press, by watching the files until the operation had finished, to within about 50 ms. Its window was given 2 s to list the folder first, which isn't counted.
- **Thumbnails not timed:** Krusader shows no thumbnails with its default settings (previews are off), so the thumbnail tests are skipped for it and those rows show —, as does its peak memory with the 600-image folder open. How fast it opens a folder is compared by the folder of 10,000 files instead.
- **Measured once:** Krusader took over 2 minutes for the trash test, so it was run once instead of 3 times.
- **Memory:** Krusader shows no thumbnails, so its figures include none; Kestrel's include those it made.
- **Search:** Krusader's search can't be timed from outside, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because Krusader has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.87 s |
| Build 150 folder mosaics | 6.20 s |
| Recursive search over 50,000 files | 0.10 s |
| Read EXIF / image-generation prompts for 400 images | 0.038 s |
| Peak memory (background jobs) | 40–45 MB |

## Performance: Kestrel vs PCManFM

Kestrel Explorer is compared here with PCManFM 1.4.0, LXDE's file manager.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS. The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, GTK's cairo renderer), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, so no file indexer ran. The benchmark is in [bench/](../bench) and is run with `bench/run.sh`.

### Compared with PCManFM

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first 12 files' thumbnails are in the shared thumbnail cache.
- **Opening a folder of 10,000 files:** timed from launch until the window has stopped changing: the list drawn, with its count of items where the app shows one. The window is captured as often as it can be (every few ms), and the time is that of its last change, once nothing has changed for 2 s.
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. PCManFM has no D-Bus file-operations service, so its own commands are used, with keys pressed in its window: Copy or Cut, then Paste in the destination (typed into its location bar), and Select All, then Delete (Move to Trash). To empty the trash, its window was taken to the trash (`trash:///`), and everything there selected and deleted with Shift+Delete, which removes them through the same gvfs trash as its Empty Trash.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | PCManFM |
|---|---|---|
| Startup (launch to window shown) | 0.39 s | 0.42 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | — |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.75 s | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.73 s | — |
| Open a folder of 10,000 files (launch to the full list shown) | 0.55 s | 0.48 s |
| Copy 20,000 small files + 5 × 50 MB | 1.02 s | 2.89 s |
| Move the same to another drive | 1.26 s | 7.13 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.59 s | 13.18 s |
| Empty the trash (those 10,000 files) | 0.41 s | 23.75 s |
| Peak memory (startup / 600-image folder open) | 143 / 153 MB | 97 / — MB |
| Memory with 5 folders opened from other apps | 105 MB (51 MB as tabs) | 22 MB (as tabs) |
| CPU time used in 30 s with a folder open, idle | 10 ms (0.03% of a core) | 10 ms (0.03% of a core) |

Kestrel compared with PCManFM:
- startup: 1.1× faster;
- listing a folder of 10,000 files: 1.1× slower;
- copying: 2.8× faster;
- moving to another drive: 5.7× faster;
- moving to the trash: 8.3× faster;
- emptying the trash: about 58× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - PCManFM wasn't timed on this (see below).
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.87 s for these 600 images (table below).
- **File operations:** PCManFM runs a paste or a move to the trash in the background, so its times were measured from the key press, by watching the files until the operation had finished, to within about 50 ms. Its window was given 2 s to list the folder first, which isn't counted.
- **Thumbnails not timed:** PCManFM shows thumbnails but keeps them to itself, never saving them to the shared thumbnail cache the test watches, so the thumbnail tests are skipped for it and those rows show —, as does its peak memory with the 600-image folder open. How fast it opens a folder is compared by the folder of 10,000 files instead.
- **Several folders:** PCManFM opens folders from other apps as tabs in its window (its default), so its figure is for one window with 5 tabs, like Kestrel's "as tabs" one.
- **Memory:** PCManFM makes its thumbnails inside the app, as Kestrel does.
- **Search:** PCManFM's search can't be timed from outside, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because PCManFM has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.87 s |
| Build 150 folder mosaics | 6.20 s |
| Recursive search over 50,000 files | 0.10 s |
| Read EXIF / image-generation prompts for 400 images | 0.038 s |
| Peak memory (background jobs) | 40–45 MB |
