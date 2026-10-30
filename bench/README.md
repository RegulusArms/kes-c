# Benchmark

← Back to [the README](../README.md) · [Development](../docs/development.md)

Compares Kestrel Explorer (this C++ version) with GNOME Files, Nemo (on Linux Mint), Dolphin (on Ubuntu, and on Kubuntu in a container: `bench/kubuntu.sh`), Thunar, Krusader and PCManFM, whichever are installed, on the same data and in the same conditions. The results are the sections of [docs/performance.md](../docs/performance.md). Each run writes them to `bench/results.md`: a "Kestrel vs …" section for each one installed, ready to paste into docs/performance.md in place of the old section (a run on Mint gives the Nemo section; the GNOME one stays as it is). docs/performance.md is never changed by the benchmark itself.

```bash
bench/run.sh                       # build, generate the data the first time, run everything, print the tables
                                   # and write them to bench/results.md
bench/run.sh --runs 5              # more runs per measurement (default 3; startup gets 2 extra)
bench/run.sh --only startup,copy   # re-run some measurements and keep the rest from the last run
bench/run.sh --apps thunar,pcmanfm # measure only these apps and keep the others' results from the last run
bench/run.sh --report             # rebuild the tables (and results.md) from the last results without running anything
```

A full run takes about 12 minutes (about 7 more with Nemo, whose Empty Trash is slow, and about 12 more with Dolphin, whose copies and Move to Trash are slow), plus about a minute and a half the first time to generate the data. The four tests in an open window (going to a folder, jumping to the end, files arriving, answering while copying) add a few minutes per app, more for one that copies slowly, since the last one lasts as long as its paste. Raw results are saved in `bench/results.json`, and the docs/performance.md sections in `bench/results.md`.

## On Kubuntu (Kestrel vs Dolphin)

```bash
bench/kubuntu.sh                       # build the image the first time, run Kestrel and Dolphin, write bench/results-kubuntu.md
bench/kubuntu.sh --only startup,copy   # takes run.sh's options; re-runs keep the other results from the last run
bench/kubuntu.sh --report              # rebuild bench/results-kubuntu.md from the last results
```

This runs the same benchmark in a Docker container of Kubuntu 26.04: Ubuntu 26.04's image with the parts of Kubuntu's desktop Dolphin uses (KIO and its thumbnailers for images, videos and PDFs, `ktrash6`, Plasma's Qt integration and Breeze), built from `bench/kubuntu/Dockerfile` as `kes-bench-kubuntu`. It needs only Docker on the machine. The desktop is set to KDE's (`XDG_CURRENT_DESKTOP=KDE`, `KDE_FULL_SESSION`), so both apps load Plasma's integration as on Kubuntu. The source is copied into the container without its build folders, and Kestrel is built there. The data and the throwaway homes are on a RAM disk in the container (`/tmp`), and the move between drives goes to its `/dev/shm`, so the data is made again each run (about a minute and a half). It measures Kestrel and Dolphin unless `--apps` names others. The results are copied out to `bench/results-kubuntu.json` and `bench/results-kubuntu.md`, whose section is "Kestrel vs Dolphin on Kubuntu", beside the Dolphin section of a run on Ubuntu. Each run leaves a stopped container named `kes-bench-kubuntu-<time>`.

## Requirements

- `xvfb` and `xdotool` (`sudo apt install xvfb xdotool`);
- `ffmpeg`, for the video test data;
- `dbus-run-session`;
- the build tools, since Kestrel and its benchmark harness are built Release;
- `x11-apps`, for `xwd` (the file-list test captures the app's window);
- `nautilus`, `nemo`, `dolphin`, `thunar`, `krusader` and/or `pcmanfm`: each one installed is measured and gets its own table. Without any, only Kestrel's own tables are made. Dolphin and Krusader also need `ktrash6` (part of KDE's KIO, installed with them) for Empty Trash; Thunar makes its thumbnails with `tumbler`, which is installed with it.

## How it's measured

### Same conditions for every run

- **Headless display:** every app runs on an Xvfb X server with software rendering: Qt's raster engine, and GTK with `GSK_RENDERER=cairo`. No windows appear on your screen.
- **Fresh home folder:** each run gets a new one, so the thumbnail cache is empty and settings are defaults.
- **Private session bus:** only the services every GNOME desktop runs can be started on it: settings (dconf) and the virtual file system (gvfs, which provides `trash:///`, the drive list and file metadata). Both are started before each measurement, because on a desktop they are already running when an app starts. Thunar's runs also allow the two Xfce services it needs, which an Xfce desktop runs: its settings store (xfconf) and its thumbnail service (tumbler). No file indexer, portal or online-accounts service runs in the background, and your own running apps never see the benchmark.
- **RAM disk:** the data is in `/tmp/kestrel-bench-data`, so disk speed doesn't count. Set `KESTREL_BENCH_DATA` to use another place. `KESTREL_BUILD_DIR` builds Kestrel and the harness into another folder name than `build` (for a machine that shares these folders with another).
- **Medians:** each measurement runs several times, and the tables show the median.
- **No crash reports:** the apps run with a core size limit of 1, which the kernel takes as "don't hand this crash to the crash reporter", so a crash (PCManFM sometimes crashes as it's closed) doesn't pop up Ubuntu's "internal error" window on your screen.

### Compared with the other file managers (measured from outside)

Dolphin has no D-Bus file-operations service, so its own commands are used, with keys pressed in its window (`xdotool`, as a user would): for a copy, `dolphin --select` opens the source's folder with it selected, then Ctrl+C, the destination typed into its location bar (Ctrl+L), and Ctrl+V; a move between drives is the same with Ctrl+X; the trash test is Ctrl+A, then Delete (Move to Trash) in the folder of 10,000 files. Its window gets 2 s to list the folder before the keys, which isn't timed; the clock starts at the key that starts the operation, and the files are watched as for the others. Its Empty Trash runs KDE's empty-trash job (KIO), which `ktrash6 --empty` runs too, and that's used for that step. Confirmations are off in the throwaway home (`kiorc`). Dolphin moves 10,000 files to the trash at about 100 a second, slowing as it goes, so that test gets 10 minutes. Its default settings leave video previews off, so the video folder isn't timed for it. Its thumbnails are made in KIO worker processes, which aren't counted in its memory.

Krusader has no such service either, and is driven the same way: it's opened with the destination in its left panel and a folder holding only the source in its right one (the active one), the cursor moved onto the source (End), then Ctrl+C (or Ctrl+X), Tab to the other panel, and Ctrl+V. Its own F5 Copy and F6 Move don't reach it on a bare X server (no window manager); Paste runs the same KIO copy. The trash test is Ctrl+A, then Delete in its right panel, and its Empty Trash is KIO's, as for Dolphin (`ktrash6 --empty`). Its first-run welcome and setup windows and the question before Delete are turned off in the throwaway home (`krusaderrc`).

PCManFM is driven like Dolphin, but it can't be opened with an item selected: it's opened on a folder holding only the source (hard links to the data's files), then Ctrl+A. Its Empty Trash is done in its window: the trash (`trash:///`) opened and given time to list all 10,000 (it doesn't redraw while it does, and keys pressed meanwhile are lost; not timed), a click in the list, Ctrl+A, then Shift+Delete, which removes them through the same gvfs trash. It sometimes misses Ctrl+A (then only the clicked file goes): if the trash stops emptying for 20 s, what's left is selected and deleted again, timed from that key. Its questions before trashing and deleting are off in the throwaway home (`libfm.conf`).

Thunar is measured like GNOME Files below, through its own D-Bus service (`org.xfce.FileManager`: `CopyInto`, `MoveInto`; `org.xfce.Trash`: `MoveToTrash`, `EmptyTrash`). Its Empty Trash asks first even when asked through D-Bus: the question is answered at once (Return), and the clock starts there, as for Kestrel.

Nemo is measured exactly like GNOME Files below, through its own D-Bus service (`org.Nemo.FileOperations`). Nemo is a fork of an older GNOME Files, and its service offers fewer operations: the benchmark asks the service which ones it has, and an operation it doesn't have shows "—" with a note under the table. Emptying the trash then starts from files put there with `gio trash`. Nemo's "confirm before emptying the trash" setting is turned off in the throwaway home, so Empty Trash doesn't wait for a click. Its service empties the trash one file at a time, about 50 files a second, so that test takes several minutes with 10,000 files: it gets 10 minutes, the other operations 2. A measurement that takes over 2 minutes is run only once (a median of several wouldn't tell more), and the docs/performance.md section says so. Nemo makes no thumbnails in this setup, PCManFM shows thumbnails but never saves them to the shared cache the test watches, and Krusader shows none with its default settings, so the thumbnail tests aren't run for them: each run would only wait out the 2-minute timeout. Those rows, and their peak memory with the 600-image folder open, show "—"; the file-list test (below) compares how fast they open a folder instead.

| Measurement | How |
|---|---|
| Startup | Launch the app on a small folder, and time it until its window is on screen (`xdotool`, checked every 10 ms). |
| Open a folder of 600 images, 40 videos or 40 PDFs | Launch the app on the folder, and time it until the first 12 files by name have thumbnails in the shared freedesktop thumbnail cache (`~/.cache/thumbnails`), which every app here writes to. The benchmark then waits until no new thumbnails appear for 2 s and records how many were made and when the last one appeared: Kestrel makes them only for what's on screen, GNOME Files for the whole folder. |
| Open a folder of 10,000 files | Launch the app on the folder, and time it until its window has stopped changing: the list drawn, with its count of items where the app shows one. The window is captured (`xwd`) as often as it can be, every few ms, and the time is that of its last change, once nothing has changed for 2 s (that wait isn't counted). Measured for every app, since it's the one way to compare how fast those that make no thumbnails here open a folder. GTK's blinking text cursor is turned off in the throwaway home, so it can't count as a change. |
| Go to a folder in an open window | A window opened on a small folder is taken, through its location bar (Ctrl+L, the path typed, Enter), to the folder of 10,000 files, then to one of 100 files. Each is timed from Enter until the window has stopped changing, as for the folder of 10,000 files above; typing the path isn't counted. Most folders are opened this way, not by starting the app. |
| Jump to the end of the 600-image folder | Once the folder's first 12 thumbnails are there, End is pressed in its window, timed until the last 6 files by name have thumbnails: an app that makes them only for what's on screen starts on those then (Kestrel makes the last screenful too when it opens a folder, so it usually has them already, and the report says so). Also the CPU time used from launch to the last thumbnail by every process of the run but the benchmark's own (the app, its thumbnailers and KIO workers, the session's services, with the processes each has waited for), so an app that makes thumbnails in several threads or processes can't look cheaper than it is. Skipped for the apps whose thumbnails aren't timed. |
| Files arriving in an open folder | 5,000 small files are created at once in the empty folder a window shows, as a download, an unpacked archive or a build would, timed until the window has stopped changing (all of them shown, with their count where the app shows one), with the CPU time used meanwhile, counted as above. |
| Window answering while it copies | In the app's window, on a folder holding only the copy test's source (hard links): Select All, Copy, the location bar to an empty folder, then Paste, as a user would. While the paste runs, Back and Forward (Alt+Left, Alt+Right) are pressed in turn, each timed until the window's title shows the folder it went to (`xdotool getwindowname`). The table shows the longest and the median wait, and the median before the paste, with nothing running: what pressing a key and reading the title cost by themselves. Not run for Krusader, which pastes into its other panel. An app whose title doesn't follow its folder shows "—". |
| Copy 20,000 small files + 5 × 50 MB | **Kestrel:** its own copy engine (`fileops.start_ops`, as used by paste and drag and drop), timed to completion. **GNOME Files:** its `org.gnome.Nautilus.FileOperations2.CopyURIs` D-Bus call, which other apps use to ask it to copy. That call returns at once, so the copy is timed by watching the destination, every 50 ms, until every file and byte has arrived. |
| Move the same to another drive | From the home folder (`/tmp`) to `/dev/shm`, a different filesystem, so the files are copied and then deleted, as with a USB stick. **Kestrel:** `start_ops` with a move. **GNOME Files:** `MoveURIs`, timed until everything has arrived and the source is gone. |
| Move 10,000 files to the trash, then empty it | All the files in one folder are selected and trashed. **Kestrel:** the window's own Move to Trash and Empty Trash actions, with the confirmation answered at once (the clock starts at the answer). **GNOME Files:** `TrashURIs`, then `EmptyTrash` without confirmation, each timed until the trash folders have filled or emptied. |
| Peak memory | The app process's peak resident memory (`VmHWM`), one second after startup and once the 600-image folder's thumbnails are done. GNOME Files makes thumbnails in separate sandboxed processes, which aren't counted. |
| Memory with 5 folders opened from other apps | The 5 folders are launched one after another, as from a browser's "Show in folder" or a terminal, and each launch waits for its window. Two seconds later, the private memory of every process the app runs is added up: what closing them would give back, without the libraries they share with each other and with other apps. That doesn't depend on which other apps happen to be running. That covers Kestrel's processes, including its tower, and GNOME Files' one process. Kestrel is measured twice: normally (a process per window), and with "open folders as tabs" on. |
| CPU time while idle | The app is launched on a small folder and left alone for 5 s. Then the CPU time all its processes use in the next 30 s is counted. The kernel counts it in 10 ms steps. This test runs twice, not three times. |

### Kestrel's own features (measured inside the app)

`kestrel_cpp.cpp` runs the same code the app runs for:
- "Generate Previews" on the 600 images;
- the mosaics for 150 folders;
- a recursive search for `*_7.jpg` among 50,000 files (500 matches);
- reading the EXIF and image-generation prompts for 400 images.

Each one prints its time and peak memory. GNOME Files has no equivalent of these. Its search can't be timed from outside without its file indexer.

## Test data

`make_data.py` makes this set once (about 600 MB). Each part is made again only if its version in `make_data.py` changes:

| Folder | Contents |
|---|---|
| `gallery/` | 600 JPEGs at 1600×1200 with camera EXIF (make, model, exposure, ISO, dates) |
| `albums/` | 150 folders of 4 images (hard links to gallery images, each with its own path) |
| `small/` | 20 text files and 5 small images: the folder used to time startup |
| `tree/` | 50,000 empty files in 500 folders, 500 of them named `*_7.jpg` |
| `copysrc/` | 20,000 files of 1–4 KB in 200 folders, plus 5 × 50 MB |
| `meta/` | 200 PNGs with Stable Diffusion "parameters" text |
| `flat/` | 10,000 files of 1–2 KB in one folder: the trash test |
| `videos/` | 40 MP4 videos, 5 s at 1280×720, made with `ffmpeg` (skipped if it isn't installed) |
| `pdfs/` | 40 PDFs of 3 pages |
| `windows/` | 5 folders of 100 small files: the several-windows test |

## Files

| File | Purpose |
|---|---|
| `run.sh` | Entry point |
| `kubuntu.sh`, `kubuntu/Dockerfile` | The run on Kubuntu, in a container |
| `bench.py` | Builds what's needed, runs every measurement in the sandbox, prints the tables, writes `results.md` |
| `make_data.py` | Generates the test data |
| `kestrel_cpp.cpp`, `CMakeLists.txt` | In-process measurements (built from `src/` into `bench/build/`) |
