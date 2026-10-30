# Bugs found outside Kestrel

← Back to [the README](../README.md)

Problems found while working on Kestrel that get in its way but aren't caused by it: bugs or unlucky behaviour in the
desktop, other programs, drivers or file systems. Each entry says what it does, how it was tracked down, how to work
around it, and what Kestrel does about it. Kestrel's own bugs are in the [to-do list](../TODO.md) and the
[changelog](../CHANGELOG.md).

## Desktop programs that ask every drive for its free space slow LTO tape writes down

**Affects:** writing to an LTO tape mounted with LTFS, from any program: Kestrel, `cp`, `dd` or another file manager.
**Caused by:** GNOME System Monitor and GNOME's low-disk-space check (`gsd-housekeeping`). Seen on Ubuntu with GNOME.
Other desktop tools that poll every mount's free space (system monitors, disk-usage widgets) will do the same.

### What happens

A copy onto the tape runs at the drive's full speed for a while, then drops to about a quarter of it for long
stretches, sometimes to nothing for several seconds, then recovers. The slow stretches don't follow the files being
written or the tape's position, and the source keeps up the whole time (Kestrel's write buffer stays full while the
drive is slow).

### Why

Asking a file system for its free space (the `statfs` call) is free on a disk, but on LTFS it turns into commands to
the tape drive. A drive that's streaming has to stop to answer them. It then slows down to a speed it thinks the
computer can keep up with (LTO drives match their speed to the host), and takes a long time to speed up again, or it
stops and repositions the tape. Either way the copy goes much slower, until nothing asks for a while.

Two GNOME programs ask about every mounted drive, the tape included, on a timer:

- **GNOME System Monitor**, while its **File Systems** tab is open (also when the window is in the background):
  every few seconds. This caused the long slow stretches.
- **GNOME's low-disk-space check** (`gsd-housekeeping`, part of GNOME's settings daemon, always running): once a
  minute. This caused shorter drops, of about 15 seconds.

Neither program knows that the question is expensive on a tape. Nothing is broken on the tape or the drive.

### How it was found

By elimination:

- **The drive and tape:** a raw `dd` straight to the tape device (LTFS unmounted) held full speed. The drive's logs
  showed no write retries, errors or alerts. A new tape and a cleaned drive behaved the same.
- **Kestrel's way of writing:** `dd` through LTFS from the same source held full speed with 512 KB and 16 MB writes,
  and with reading and writing overlapped through a pipe, so Kestrel's write pattern wasn't the cause. `strace` showed
  Kestrel's writer only writing, with each write waiting on LTFS during slow stretches. Dropping the page cache changed
  nothing.
- **Kestrel's own questions:** earlier versions asked the tape for its size and health while copying, which caused the
  same thing. That was fixed in 0.3.2-alpha.6, and the slow stretches were still there.
- **Other programs:** `perf` showed GNOME System Monitor busy during the slow stretches, and closing it brought full
  speed back. Recording every `statfs` call on the system then caught `gsd-housekeeping` asking about every mount once
  a minute, and one of those questions lined up with a 14-second drop.

### Workaround

- Keep GNOME System Monitor's File Systems tab closed while writing to a tape. Its Processes and Resources tabs don't
  ask about drives.
- Add the tape's mount point to the low-disk-space check's ignore list, for example:

  ```bash
  gsettings set org.gnome.settings-daemon.plugins.housekeeping ignore-paths "['/path/to/tape']"
  ```

  with the folder the tape is mounted on. This replaces the whole list, so check what's there first with `gsettings
  get` and keep those entries.

### What Kestrel does

Since 0.3.2-alpha.7, while a job writes to a tape, Kestrel adds the tape to the low-disk-space check's ignore list, and
takes it off again afterwards (unless it was there already). If a system monitor that polls drives is running, the
job's log names it, so you know to close it. Kestrel itself doesn't ask a tape anything while a job is using it.

### Still open

- Whether the ignore list stops `gsd-housekeeping` from asking about the drive at all hasn't been confirmed on a running
  system; it could still ask and then not warn.
- A few short drops (about 5 seconds) are left with no such question from any program. Their cause isn't known yet.
- Not reported to GNOME yet. The fix there would be to skip, or ask less often about, file systems where asking is
  expensive: tapes (LTFS), and also network and FUSE file systems.
