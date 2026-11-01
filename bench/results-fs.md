## Performance: Kestrel vs GNOME Files

Kestrel Explorer is compared here with GNOME Files 50.2.2, the file manager it replaces.

**Test machine:** AMD Ryzen Threadripper 2950X 16-Core Processor (32 threads), Ubuntu 26.04.1 LTS. The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran 3 times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering (Qt's raster engine, GTK's cairo renderer), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start, so no file indexer ran. The benchmark is in [bench/](https://github.com/RegulusArms/kes-c/tree/main/bench) and is run with `bench/run.sh`.

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
- **Several folders opened from other apps:** the 5 folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window (0 processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in 30 s with a folder open and nothing happening. It's counted in 10 ms steps.

| Test | Kestrel | GNOME Files |
|---|---|---|
| Startup (launch to window shown) | — | — |
| Open a 600-image folder (launch to the first 12 thumbnails) | — | — |
| Open a folder of 40 videos (launch to the first 12 thumbnails) | — | — |
| Open a folder of 40 PDFs (launch to the first 12 thumbnails) | — | — |
| Open a folder of 10,000 files (launch to the full list shown) | — | — |
| Go to the folder of 10,000 files in an open window (Enter to the list shown) | — | — |
| Go to a folder of 100 files in an open window (Enter to the list shown) | — | — |
| Jump to the end of the 600-image folder (End to its last 6 thumbnails) | — | — |
| 5,000 files arriving in an open folder (until all shown) | — | — |
| Copy 20,000 small files + 5 × 50 MB | — | — |
| Move the same to another drive | — | — |
| Move 10,000 files to the trash (all selected in one folder) | — | — |
| Empty the trash (those 10,000 files) | — | — |
| Window answering while it copies (Back/Forward to the new title: longest / median) | — | — |
| CPU time to open the 600-image folder and jump to its end (app and helpers) | — | — |
| CPU time for 5,000 files arriving in an open folder (app and helpers) | — | — |
| Peak memory (startup / 600-image folder open) | — | — |
| Memory with 5 folders opened from other apps | — | — |
| CPU time used in 30 s with a folder open, idle | — | — |

**Copying with the system's help:** the same files copied within one Btrfs drive, one XFS drive (loop images on a RAM disk) and one SMB share (Samba on the same machine, mounted with the kernel's SMB client), source and copy both there, made by bench/fs.sh in a container. Kestrel is timed with Preferences → Files → Copying off (it reads and writes every byte itself, as before that setting) and on (a clone on Btrfs and XFS, the server's own copy on SMB).

| Copy 20,000 small files + 5 × 50 MB | Kestrel, its own copy (before) | Kestrel, the system's copy (now) | GNOME Files |
|---|---|---|---|
| Within one Btrfs drive (a clone) | 2.34 s | 1.80 s | 2.69 s |
| Within one XFS drive (a clone) | 1.61 s | 1.60 s | 3.45 s |
| Within one SMB share (the server copies it) | 62.69 s | 61.58 s | 108.34 s |
| …its 5 × 50 MB alone, within one Btrfs drive (a clone) | 0.34 s | 0.006 s | 0.22 s |
| …its 5 × 50 MB alone, within one XFS drive (a clone) | 0.086 s | 0.046 s | 0.26 s |
| …its 5 × 50 MB alone, within one SMB share (the server copies it) | 0.45 s | 0.24 s | 0.38 s |

Kestrel compared with GNOME Files:


**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - GNOME Files makes thumbnails for the whole folder in the background. It finished all None images — after launch.
  - Kestrel makes them only for what's on screen (None images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": — for these 600 images (table below).
- **File operations:** GNOME Files' D-Bus service returns straight away, so its times were measured by watching the files until the operation had finished, to within about 50 ms.
- **Jumping to the end:** GNOME Files makes thumbnails for the whole folder in the background, so some may be ready before End is pressed.
- **Memory:** GNOME Files makes thumbnails in separate sandboxed helper processes, whose memory isn't counted in its figures. Kestrel makes them inside the app.
- **Search:** GNOME Files' search can't be timed from outside without its file indexer, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because GNOME Files has no equivalent ("Generate Previews", folder mosaics, metadata panels) or can't be timed from outside (search).

| Test | Kestrel |
|---|---|
| Thumbnail all 600 images ("Generate Previews") | — |
| Build 150 folder mosaics | — |
| Recursive search over 50,000 files | — |
| Read EXIF / image-generation prompts for 400 images | — |
| Peak memory (background jobs) | — |
