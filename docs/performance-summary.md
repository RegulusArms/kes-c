# Performance summary

[Full results and methods →](performance.md)

Kestrel was benchmarked against GNOME Files, Dolphin, Thunar, Krusader and PCManFM on one machine with the same test
data, and against Nemo separately on Linux Mint.

- **Thumbnails:** fastest in every test. It shows a 600-image folder's first thumbnails in 0.40 s: 2× faster than
  Dolphin, 3× faster than GNOME Files and 16× faster than Thunar. Videos are 4.5× faster than GNOME Files.
- **Copying and moving:** fastest in every test. 20,000 small files plus 250 MB copy in about 1 s, half the time GNOME
  Files takes and 8–28× faster than Thunar, Krusader and Dolphin. A move to another drive takes 1.3 s, against
  4.4–41 s for the others.
- **Trash:** moving 10,000 files to the trash takes 1.6 s, against 1.9 s for GNOME Files and over 3 minutes for Dolphin
  and Krusader. Emptying the trash takes 0.4 s, 10–400× faster than the rest (Thunar takes 3 minutes; Nemo 6).
- **Startup and big folders:** startup ties with GNOME Files, Dolphin and PCManFM, at about 0.4 s. A folder of 10,000
  files opens in 0.55 s, 2–3× faster than most; only PCManFM is a little quicker (0.48 s).
- **Resources:** it uses less memory than GNOME Files and Dolphin, but more than the lightweight Thunar and PCManFM
  (about 145 MB against 100 MB). Idle CPU use is negligible.

**In short:** Kestrel starts as fast as the rest and is the fastest at thumbnails, copying, moving and the trash,
often by a wide margin. It uses mid-range memory.
