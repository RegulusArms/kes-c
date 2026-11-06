# Performance summary

← Back to [the README](../README.md) · [Performance](performance.md)

[Full results and methods →](performance.md)

Kestrel was benchmarked against GNOME Files, Dolphin, Thunar, Krusader and PCManFM on one machine with the same test
data, against Dolphin again on Kubuntu, and against Nemo separately on Linux Mint.

- **Thumbnails:** the first thumbnails of a 600-image folder appear in 0.52 s: 1.9× faster than Dolphin, 2.3× faster
  than GNOME Files and 2.6× faster than Thunar. Videos and PDFs are 1.2–1.3× faster than GNOME Files. It also does the
  work cheaply: opening that folder and jumping to its end takes about 1.4 s of CPU, against 2–18 s for the others.
- **In an open window:** going to a folder of 10,000 files takes 0.09 s, the fastest of the six: 2–3× faster than
  Krusader, PCManFM and Dolphin, 9× faster than GNOME Files and 23× faster than Thunar. A small folder takes 0.05 s,
  a little quicker than Dolphin and Krusader and 4–7× faster than the rest. 5,000 files arriving in the folder it
  shows are all on screen in 0.20 s, also the fastest. Like the others, its window keeps answering at once while it
  copies.
- **Copying and moving:** fastest in every test. 20,000 small files plus 250 MB copy in about 1.2 s, under half the
  time GNOME Files takes and 3–26× faster than PCManFM, Krusader, Thunar and Dolphin. A move to another drive takes
  1.5 s, against 5–43 s for the others.
- **Trash:** moving 10,000 files to the trash takes 2.2 s, against 2.4 s for GNOME Files and over 4 minutes for Dolphin
  and Krusader. Emptying the trash takes 0.5 s, 14–380× faster than the rest (Thunar and Nemo take 3 minutes).
- **Startup and big folders:** startup takes 0.50 s, between GNOME Files (0.44 s) and Dolphin (0.64 s), and ties with
  PCManFM. A folder of 10,000 files opens in 0.55 s, tied with PCManFM and 2–3× faster than the rest.
- **Resources:** it uses less memory than GNOME Files and Dolphin, but more than the lightweight Thunar and PCManFM
  (about 140 MB against under 100 MB). Idle CPU use is negligible.
- **On Kubuntu**, against Dolphin, the picture is the same, with two exceptions: Kestrel makes no PDF thumbnails there
  yet, and it takes 0.16 s to go to a small folder, against Dolphin's 0.04 s.
- **On Linux Mint**, against Nemo: Nemo starts a little faster (0.28 s against 0.37 s); Kestrel opens a folder of
  10,000 files 6× faster, goes to a folder in an open window 12–16× faster and copies 14× faster.

**In short:** Kestrel starts about as fast as the rest and is the fastest at thumbnails, copying, moving and the trash,
often by a wide margin, while using less CPU for its thumbnails. It uses mid-range memory.
