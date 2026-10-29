# Performance summary

[Full results and methods →](performance.md)

Kestrel was benchmarked against GNOME Files, Dolphin, Thunar, Krusader and PCManFM on one machine with the same test
data, against Dolphin again on Kubuntu, and against Nemo separately on Linux Mint.

- **Thumbnails:** the first thumbnails of a 600-image folder appear in 0.40 s: 2× faster than Dolphin, 3× faster than
  GNOME Files and 14× faster than Thunar. Videos are 3.4× faster than GNOME Files; PDFs about the same. It also does
  the work cheaply: opening that folder and jumping to its end takes about 1 s of CPU, against 6–20 s for the others.
- **In an open window:** going to a folder of 10,000 files takes 0.23 s, 2–9× faster than GNOME Files, Dolphin and
  Thunar (Krusader and PCManFM are a little quicker); a small folder takes 0.04 s, tied with Dolphin and Krusader and
  5–8× faster than the rest. 5,000 files arriving in the folder it shows are all on screen in 0.23 s, the fastest of
  the six. Like the others, its window keeps answering at once while it copies.
- **Copying and moving:** fastest in every test. 20,000 small files plus 250 MB copy in about 1.2 s, half the time
  GNOME Files takes and 4–23× faster than Krusader, Thunar and Dolphin. A move to another drive takes 1.4 s, against
  4.4–42 s for the others.
- **Trash:** moving 10,000 files to the trash takes 1.7 s, against 1.9 s for GNOME Files and over 3 minutes for Dolphin
  and Krusader. Emptying the trash takes 0.4 s, 10–400× faster than the rest (Thunar takes 3 minutes; Nemo 6).
- **Startup and big folders:** startup ties with GNOME Files, Dolphin and PCManFM, at about 0.4 s. A folder of 10,000
  files opens in 0.51 s, 2–3× faster than most; only PCManFM is a little quicker (0.46 s).
- **Resources:** it uses less memory than GNOME Files and Dolphin, but more than the lightweight Thunar and PCManFM
  (about 145 MB against 100 MB). Idle CPU use is negligible.
- **On Kubuntu**, against Dolphin, the picture is the same, with two exceptions: Kestrel makes no PDF thumbnails there
  yet, and it takes 0.14 s to go to a small folder, against Dolphin's 0.03 s.

**In short:** Kestrel starts as fast as the rest and is the fastest at thumbnails, copying, moving and the trash,
often by a wide margin, while using less CPU for its thumbnails. It uses mid-range memory.
