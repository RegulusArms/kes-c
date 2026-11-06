# Benchmark

← Back to [the README](../README.md) · [Development](../docs/development.md)

Compares Kestrel Explorer (this C++ version) with GNOME Files, Nemo, Dolphin, Thunar, Krusader and PCManFM on the same data and in the same conditions. Every run is in a clean Docker container that holds nothing of this machine's but the project's source (see [Clean, sterile containers](#clean-sterile-containers)): the benchmark never runs on the machine itself. There are four runs, each with its own container and results:

| Run | In a container of | Measures | Writes | In docs/performance.md |
|---|---|---|---|---|
| `bench/run.sh` | Ubuntu 26.04 | Kestrel, GNOME Files, Dolphin, Thunar, Krusader, PCManFM | `bench/results.md` | the "Kestrel vs …" sections of each (GNOME Files, Dolphin, Thunar, Krusader, PCManFM) |
| `bench/mint.sh` | Linux Mint 22.3 | Kestrel, Nemo | `bench/results-mint.md` | "Kestrel vs Nemo" |
| `bench/kubuntu.sh` | Kubuntu 26.04 | Kestrel, Dolphin with Plasma's integration | `bench/results-kubuntu.md` | "Kestrel vs Dolphin on Kubuntu" |
| `bench/fs.sh` | Ubuntu 26.04, privileged | copies on Btrfs, XFS and SMB | `bench/results-fs.md` | "Copying with the system's help" |

Each `.md` has sections ready to paste into [docs/performance.md](../docs/performance.md) in place of the old ones, and its `.json` the raw results; docs/performance.md (and its "At a glance" table) is never changed by the benchmark itself. Run them one at a time, on an otherwise idle machine: run side by side, they'd slow each other down. Each takes the same options:

```bash
bench/run.sh                       # build the image afresh, build Kestrel and make the data in it, run everything,
                                   # print the tables and write them to bench/results.md
bench/run.sh --runs 5              # more runs per measurement (default 3; startup gets 2 extra)
bench/run.sh --only startup,copy   # re-run some measurements and keep the rest from the last run
bench/run.sh --apps thunar,pcmanfm # measure only these apps and keep the others' results from the last run
bench/run.sh --report             # rebuild the tables (and results.md) from the last results without running anything
```

`bench/run.sh` takes about 30–40 minutes with all six apps (12 for the tests every app gets, about 12 more for Dolphin, whose copies and Move to Trash are slow, and a few minutes per app for the four tests in an open window, more for one that copies slowly, since the last one lasts as long as its paste). Each run also builds its image (about 5 minutes), builds Kestrel and makes the data (about a minute and a half), since nothing is kept from one run to the next. The `.json` of the last run goes back in with the source, so `--only` and `--apps` can keep its other results.

## On Ubuntu (Kestrel vs GNOME Files, Dolphin, Thunar, Krusader and PCManFM)

`bench/run.sh` builds its image from `bench/ubuntu/Dockerfile` as `kes-bench-ubuntu`: Ubuntu 26.04's image with what a standard Ubuntu desktop (`ubuntu-desktop`) gives the apps (GNOME Files; its thumbnailers: Papers for PDFs, GStreamer's for videos, glycin's for images; the Yaru theme and icons), the other file managers with what each one recommends, as `apt install` gives them (KIO and its thumbnailers, `ktrash6`, `tumbler`…), and the packages Kestrel recommends that change what it shows (Qt's image formats, its GTK integration). The desktop is set to Ubuntu's GNOME (`XDG_CURRENT_DESKTOP=ubuntu:GNOME`), as in an Ubuntu session.

## On Kubuntu (Kestrel vs Dolphin)

```bash
bench/kubuntu.sh                       # build the image afresh, run Kestrel and Dolphin, write bench/results-kubuntu.md
bench/kubuntu.sh --only startup,copy   # takes run.sh's options; re-runs keep the other results from the last run
bench/kubuntu.sh --report              # rebuild bench/results-kubuntu.md from the last results
```

This runs the same benchmark in a Docker container of Kubuntu 26.04: Ubuntu 26.04's image with the parts of Kubuntu's desktop Dolphin uses (KIO and its thumbnailers for images, videos and PDFs, `ktrash6`, Plasma's Qt integration and Breeze), built from `bench/kubuntu/Dockerfile` as `kes-bench-kubuntu`. The desktop is set to KDE's (`XDG_CURRENT_DESKTOP=KDE`, `KDE_FULL_SESSION`), so both apps load Plasma's integration as on Kubuntu. It measures Kestrel and Dolphin unless `--apps` names others. Its section is "Kestrel vs Dolphin on Kubuntu", beside the Dolphin section of `bench/run.sh`.

## On Linux Mint (Kestrel vs Nemo)

```bash
bench/mint.sh                       # build the image afresh, run Kestrel and Nemo on Mint 22.3, write bench/results-mint.md
bench/mint.sh --release 23          # on Linux Mint 23 (Ubuntu 26.04's base) instead
bench/mint.sh --only startup,copy   # takes run.sh's options; re-runs keep the other results from the last run
bench/mint.sh --report              # rebuild bench/results-mint.md from the last results
```

This runs the same benchmark in a Docker container of Linux Mint, so Nemo can be measured without a Mint machine or virtual machine. The image is built from `bench/mint/Dockerfile` as `kes-bench-mint`, on Linux Mint's own image (`linuxmintd/mint22.3-amd64`, or the `--release` given: Ubuntu's base with Mint's repository, so Nemo, Cinnamon's libraries and the XApp packages are Mint's builds). On it go the parts of Mint's Cinnamon desktop that the two apps use: Nemo, the thumbnailers Mint installs (`ffmpegthumbnailer`, Xreader's for PDFs, HEIF's), Qt's GTK integration and the Mint-Y theme and icons. The desktop is set to Cinnamon's (`XDG_CURRENT_DESKTOP=X-Cinnamon`), so Kestrel loads Qt's GTK integration as on Mint. Mint's theme and icons are its gsettings defaults, which Cinnamon's settings daemon hands to every app; with no such daemon on the bare X server, the image writes them to `/etc/gtk-3.0/settings.ini` instead (Mint-Y-Aqua, Mint-Y-Sand). Mint 22.3's Qt is 6.4, as on a real Mint 22.3. It measures Kestrel and Nemo unless `--apps` names others. Mint's image keeps Ubuntu's `/etc/os-release`, so the script tells the report which Mint it was (`KESTREL_BENCH_DISTRO`). Its section is "Kestrel vs Nemo", to paste in place of the one from the Mint virtual machine.

## Copying on Btrfs, XFS and SMB

```bash
bench/fs.sh                 # build the image afresh, run the three copy tests, write bench/results-fs.md
bench/fs.sh --runs 5        # takes run.sh's options (--only, --apps, --report…)
```

Kestrel lets the system copy files (Preferences → Files → Copying): a clone where the drive can share data between files, the server's own copy on a network share. These copies are measured in a privileged Docker container, built from `bench/fs/Dockerfile` as `kes-bench-fs` (Ubuntu 26.04 with GNOME Files, the Btrfs and XFS tools, Samba and the kernel SMB client's mount helper). In its RAM disk it makes a 4 GB Btrfs and a 4 GB XFS file system on loop images, and a Samba share mounted with the kernel's SMB client (SMB 3.1.1, to the same machine); all of it goes with the container. It then runs the benchmark with `KESTREL_BENCH_BTRFS`, `KESTREL_BENCH_XFS` and `KESTREL_BENCH_SMB` naming them, which turns on three tests (`copy_btrfs`, `copy_xfs`, `copy_smb`): the copy test's 20,000 small files and 250 MB copied within the one folder, source and copy both there, then its 5 × 50 MB alone (the sources are put there once, not timed). Kestrel is timed with the setting off (its own read/write loop, as before the setting) and on; GNOME Files through its D-Bus service, as in the copy test. An app driven through its window (Dolphin, PCManFM, Krusader) isn't timed there. The three tests are skipped in the other runs, which don't set those variables. The results go to `bench/results-fs.json` and `bench/results-fs.md`, whose table is the "Copying with the system's help" part of docs/performance.md.

## Clean, sterile containers

Every run is made the same way (`bench/container.sh`, which the four scripts share), so that what's measured is the apps on a known system, and nothing of this machine:

- **Clean:** any container left from an earlier run of that kind (`kes-bench-ubuntu-*`, `kes-bench-mint-*`, `kes-bench-kubuntu-*`, `kes-bench-fs-*`) is destroyed first, the image is built again from a freshly pulled base image with nothing cached (`docker build --pull --no-cache`), so no state or old package carries over, and the run's container is destroyed once it's done, failed or interrupted. A container Docker refuses to remove (the snap's AppArmor profile can) is named; `sudo snap restart docker` frees it.
- **Sterile:** nothing of the machine is mounted in the container. The source goes in as a tar on its standard input: only the files git has (tracked, and new ones not yet added, which are named as they're sent), never files git ignores (local notes and settings, build folders, logs) and not `.git` (its history, names and emails), with no owner names, extended attributes or ACLs. The one other file sent is the last results' `.json` (measurements only), so `--only` and `--apps` can keep the others. The container gets no variables but the ones its script sets, and no network (`--network none`; loopback still works, for the SMB share), so nothing outside reaches or slows it. Kestrel is built inside, Release, and the data made there. The container may make the sandboxes desktop apps put their helpers in, as an Ubuntu session allows: GNOME's thumbnailers and glycin's image loaders run under `bwrap`, which makes user namespaces and mounts its own `/proc`. So Docker's seccomp profile and its masking of `/proc` are off (`--security-opt seccomp=unconfined,systempaths=unconfined`), and AppArmor confines it with the benchmark's own profile, `bench/apparmor/kes-bench`, instead of Docker's (which denies mounts). That profile allows everything inside the container; what it's for is that every program there stays in it, so this machine's own AppArmor profiles for programs of the same name (Papers, Evince, Nautilus, glycin, bwrap…) never attach to the container's copies. Unconfined, they would (and Papers' then can't even start there), so the results would depend on this machine's AppArmor setup. The scripts load it with `sudo` into `/etc/apparmor.d/kes-bench` the first time, and again whenever `bench/apparmor/kes-bench` changes (`sudo apparmor_parser -R /etc/apparmor.d/kes-bench`, then deleting the file, takes it away); on a machine without AppArmor it isn't needed. Without these, GNOME Files makes no thumbnails in a container. They change what the container may do inside itself, not what it can see.
- **The benchmark itself** (`bench.py`) refuses to run outside such a container (it checks `KESTREL_BENCH_CONTAINER`, which only they set).
- **Requirements:** only Docker on the machine; every other tool is in the images.

## How it's measured

### Same conditions for every run

- **Headless display:** every app runs on an Xvfb X server with software rendering: Qt's raster engine, and GTK with `GSK_RENDERER=cairo`. No windows appear on your screen.
- **Fresh home folder:** each run gets a new one, so the thumbnail cache is empty and settings are defaults.
- **Private session bus:** only the services every GNOME desktop runs can be started on it: settings (dconf) and the virtual file system (gvfs, which provides `trash:///`, the drive list and file metadata). Both are started before each measurement, because on a desktop they are already running when an app starts. Thunar's runs also allow the two Xfce services it needs, which an Xfce desktop runs: its settings store (xfconf) and its thumbnail service (tumbler). No file indexer, portal or online-accounts service runs in the background, and your own running apps never see the benchmark.
- **RAM disk:** the data is in `/tmp/kestrel-bench-data`, on the container's RAM disk (`/tmp`), so disk speed doesn't count; the move between drives goes to its `/dev/shm`, another one.
- **Medians:** each measurement runs several times, and the tables show the median.
- **An app that quits before its window:** Krusader now and then exits at once when started, before showing any window. In a test that doesn't time the launch (going to a folder, files arriving, the window answering while it copies, and the copies driven through its window), the app is then started again, up to 3 times, and its exit code and what it printed go in the run's log. A test that times the launch (startup, opening a folder) isn't retried.
- **No crash reports:** the apps run with a core size limit of 1, which the kernel takes as "don't hand this crash to the crash reporter", so a crash (PCManFM sometimes crashes as it's closed) doesn't hand anything to a crash reporter.

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
| `run.sh`, `ubuntu/Dockerfile` | The run on Ubuntu, in a container |
| `container.sh` | What the four runs share: the clean container, the source sent in, the results fetched |
| `kubuntu.sh`, `kubuntu/Dockerfile` | The run on Kubuntu, in a container |
| `mint.sh`, `mint/Dockerfile` | The run on Linux Mint, in a container |
| `fs.sh`, `fs/Dockerfile` | The copies on Btrfs, XFS and SMB, in a privileged container |
| `bench.py` | Builds what's needed, runs every measurement in the sandbox, prints the tables, writes `results.md` |
| `make_data.py` | Generates the test data |
| `kestrel_cpp.cpp`, `CMakeLists.txt` | In-process measurements (built from `src/` into `bench/build/`) |
