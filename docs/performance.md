# Performance

[← Back to the README](../README.md)

## Performance: Kestrel vs GNOME Files

Kestrel Explorer is compared here with GNOME Files 50.2.2, the file manager it replaces.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS. The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, GTK's cairo renderer), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, so no file indexer ran. The benchmark is in [bench/](../bench) and is run with `bench/run.sh`.

### Compared with GNOME Files

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first 12 files' thumbnails are in the shared thumbnail cache.
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. GNOME Files receives them through its D-Bus file-operations service, as when another app asks it to.
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (6 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | GNOME Files |
|---|---|---|
| Startup (launch to window shown) | 0.37 s | 0.40 s |
| Open a 600-image folder (launch to the first 12 thumbnails) | 0.40 s | 1.26 s |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | 1.79 s | 9.00 s |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | 0.73 s | 0.84 s |
| Copy 20,000 small files + 5 × 50 MB | 0.83 s | 2.24 s |
| Move the same to another drive | 1.04 s | 4.39 s |
| Move 10,000 files to the trash (all selected in one folder) | 1.68 s | 1.96 s |
| Empty the trash (those 10,000 files) | 0.33 s | 8.62 s |
| Peak memory (startup / 600-image folder open) | 137 / 149 MB | 194 / 303 MB |
| Memory with 5 folders opened from other apps | 85 MB (29 MB as tabs) | 95 MB |
| CPU time used in 30 s with a folder open, idle | 25 ms (0.08% of a core) | 5 ms (0.02% of a core) |

Kestrel compared with GNOME Files:
- startup: 1.1× faster;
- first thumbnails in a 600-image folder: 3.1× faster;
- first video thumbnails: 5.0× faster;
- first PDF thumbnails: 1.2× faster;
- copying: 2.7× faster;
- moving to another drive: 4.2× faster;
- moving to the trash: 1.2× faster;
- emptying the trash: about 26× faster.

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - GNOME Files makes thumbnails for the whole folder in the background. It finished all 600 images 11.06 s after launch.
  - Kestrel makes them only for what's on screen (40 images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": 4.84 s for these 600 images (table below).
- **File operations:** GNOME Files' D-Bus service returns straight away, so its times were measured by watching the files until the operation had finished, to within about 50 ms.
- **Memory:** GNOME Files makes thumbnails in separate sandboxed helper processes, whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** GNOME Files' search can't be timed from outside without its file indexer, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because GNOME Files has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | 4.84 s |
| Build 150 folder mosaics | 6.24 s |
| Recursive search over 50,000 files | 0.12 s |
| Read EXIF / image-generation prompts for 400 images | 0.042 s |
| Peak memory (background jobs) | 37–42 MB |

## Performance: Kestrel vs Nemo

Kestrel Explorer is compared here with Nemo, the file manager it replaces.

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
