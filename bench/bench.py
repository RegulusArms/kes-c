#!/usr/bin/python3
"""Benchmark Kestrel Explorer (C++) against GNOME Files, Nemo (on Linux Mint), Dolphin, Thunar, Krusader and PCManFM,
whichever are installed. See bench/README.md; the results go in docs/performance.md.

  bench/run.sh                      build, generate the data (first time), run everything, print the tables
  bench/run.sh --runs 5             more runs per measurement (medians are reported)
  bench/run.sh --only startup,copy  re-run only some measurements (the rest are kept from the last run)
  bench/run.sh --apps thunar        measure only these apps (the others' results are kept from the last run)
  bench/run.sh --report             rebuild the tables from the last results without running anything
The tables are printed and written to bench/results.md, as Performance sections ready to paste into docs/performance.md.

Every run happens on a headless X server (Xvfb) with a fresh home folder and a private D-Bus session bus on which
only the desktop's settings (dconf) and virtual file system (gvfs) services can start, so caches are empty and no
indexer or other background service runs.
"""
import argparse
import hashlib
import json
import os
import re
import resource
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.realpath(os.path.dirname(HERE))
DATA = os.environ.get("KESTREL_BENCH_DATA", "/tmp/kestrel-bench-data")
CMAKE = "/usr/bin/cmake" if os.access("/usr/bin/cmake", os.X_OK) else "cmake"
BUILD = os.environ.get("KESTREL_BUILD_DIR", "build")  # build folder name (another machine sharing these folders)
# set by a wrapper that runs the benchmark in a container of another desktop's distribution ("Kubuntu": bench/kubuntu.sh),
# whose report gets sections of its own ("Kestrel vs Dolphin on Kubuntu")
PLATFORM = os.environ.get("KESTREL_BENCH_PLATFORM", "")
# The file managers Kestrel is compared with; the ones installed are measured (GNOME Files on Ubuntu, Nemo on Linux
# Mint, Dolphin, Thunar, Krusader and PCManFM where they're installed). File operations go through each one's D-Bus
# file-operations service, as other apps use it: "service" starts it, "ops" is (bus name, object path, {operation:
# (interface, method)}), and "settings" are set in the throwaway home first (gsettings). Dolphin, Krusader and PCManFM
# have no such service ("ops": None): their operations are keys pressed in their window (UiOps; "ui" names how).
# "files" are config files written into the throwaway home first. "dbus" names the session services the app needs
# besides the desktop's settings and gvfs (Thunar's settings store and thumbnail service), allowed on its bus.
# "thumbnails": False skips the FOLDER_TESTS: Nemo makes no thumbnails in the sandbox, PCManFM doesn't save its own to
# the shared cache, and Krusader shows none by default, so each run would only wait out the 2-minute timeout; the
# file-list test (LIST_TEST) times how they open a folder instead. "skip" names folder tests skipped for that app
# alone, with the reason (Dolphin's default settings leave video previews off). "no_responsive" says why the
# responsiveness test (RESPONSIVE) can't be run on that app.
NO_BLINK = {f".config/gtk-{v}/settings.ini": "[Settings]\ngtk-cursor-blink=false\n" for v in ("3.0", "4.0")}   # the
# file-list test watches the window's pixels: a blinking text cursor would keep changing them
OTHERS = {
    "nautilus": {"label": "GNOME Files", "cmd": ["nautilus"], "service": ["nautilus", "--gapplication-service"],
                 "ops": ("org.gnome.Nautilus", "/org/gnome/Nautilus/FileOperations2",
                         {op: ("org.gnome.Nautilus.FileOperations2", m) for op, m in
                          (("copy", "CopyURIs"), ("move", "MoveURIs"), ("trash", "TrashURIs"), ("empty", "EmptyTrash"))}),
                 "settings": [], "files": dict(NO_BLINK), "skip": {},
                 "heading": "## Performance: Kestrel vs GNOME Files", "intro": "the file manager it replaces", "gtk": True,
                 "helpers": "separate sandboxed helper processes", "search": " without its file indexer",
                 "whole_folder": True,   # thumbnails the whole folder in the background
                 "thumbnails": True},
    "nemo": {"label": "Nemo", "cmd": ["nemo"], "service": ["nemo", "--no-default-window"],
             "ops": ("org.Nemo", "/org/Nemo",
                     {op: ("org.Nemo.FileOperations", m) for op, m in
                      (("copy", "CopyURIs"), ("move", "MoveURIs"), ("trash", "TrashURIs"), ("empty", "EmptyTrash"))}),
             "settings": [("org.nemo.preferences", "confirm-trash", "false")],   # Empty Trash would ask first
             "files": dict(NO_BLINK), "skip": {},
             "heading": "## Performance: Kestrel vs Nemo", "intro": "the file manager it replaces", "gtk": True,
             "helpers": "separate helper processes", "search": "", "whole_folder": False,
             "thumbnails": False, "no_thumbs_why": "makes no thumbnails in this setup (a fresh home folder with its default "
                                                   "settings)"},
    "dolphin": {"label": "Dolphin", "cmd": ["dolphin"], "service": None, "ops": None, "ui": "dolphin", "settings": [],
                "files": {".config/kiorc": "[Confirmations]\nConfirmTrash=false\nConfirmEmptyTrash=false\n",
                          ".config/dolphinrc": "[General]\nRememberOpenedTabs=false\n"},
                "skip": {"videos": "its default settings leave video previews off"},
                "heading": "## Performance: Kestrel vs Dolphin", "intro": "KDE's file manager", "gtk": False,
                # its nearest equivalents of Kestrel's own features, which can't be timed from outside
                "features": "Dolphin's nearest equivalents (its folder previews and Information panel) can't be timed "
                            "from outside, nor can its search",
                "helpers": "separate KIO worker processes", "search": " without its file indexer (Baloo)",
                "whole_folder": False, "thumbnails": True},
    "thunar": {"label": "Thunar", "cmd": ["thunar"], "service": ["thunar", "--daemon"],
               "ops": ("org.xfce.FileManager", "/org/xfce/FileManager",
                       {"copy": ("org.xfce.FileManager", "CopyInto"), "move": ("org.xfce.FileManager", "MoveInto"),
                        "trash": ("org.xfce.Trash", "MoveToTrash"), "empty": ("org.xfce.Trash", "EmptyTrash")}),
               "confirm": {"empty"},   # Empty Trash asks first, even when asked through D-Bus
               "dbus": ["org.xfce.Xfconf.", "org.xfce.Tumbler."], "settings": [], "files": dict(NO_BLINK), "skip": {},
               "heading": "## Performance: Kestrel vs Thunar", "intro": "Xfce's file manager", "gtk": True,
               "dbus_text": ", and Thunar's own settings store and thumbnail service (xfconf, tumbler)",
               "helpers": "a separate thumbnail service (tumbler)", "search": "", "whole_folder": False,
               "thumbnails": True},
    "krusader": {"label": "Krusader", "cmd": ["krusader"], "service": None, "ops": None, "ui": "krusader",
                 "settings": [],
                 # no first-run welcome and setup windows; Delete (to the trash) without asking
                 "files": {".config/krusaderrc": "First Time=false\n\n[Advanced]\nConfirm Delete=false\n"},
                 "skip": {}, "heading": "## Performance: Kestrel vs Krusader", "intro": "KDE's two-panel file manager",
                 "gtk": False, "helpers": "separate KIO worker processes", "search": "", "whole_folder": False,
                 "memory": "Krusader shows no thumbnails, so its figures include none; Kestrel's include those it made.",
                 # the responsiveness test pastes in one folder and goes Back and Forward: Krusader pastes into the other
                 # panel, and its window title doesn't follow the folder
                 "no_responsive": "pastes into its other panel rather than the folder it shows, so it can't be driven "
                                  "like the others",
                 "thumbnails": False, "no_thumbs_why": "shows no thumbnails with its default settings (previews are off)"},
    "pcmanfm": {"label": "PCManFM", "cmd": ["pcmanfm"], "service": None, "ops": None, "ui": "pcmanfm", "settings": [],
                "files": {".config/libfm/libfm.conf": "[config]\nconfirm_del=0\nconfirm_trash=0\n", **NO_BLINK},
                "skip": {}, "heading": "## Performance: Kestrel vs PCManFM", "intro": "LXDE's file manager", "gtk": True,
                "helpers": None, "memory": "PCManFM makes its thumbnails inside the app, as Kestrel does.",
                "tabs": True,   # folders opened from other apps become tabs in its window (its default)
                "search": "", "whole_folder": False,
                "thumbnails": False, "no_thumbs_why": "shows thumbnails but keeps them to itself, never saving them to the "
                                                      "shared thumbnail cache the test watches"},
}
APPS = ("kestrel",) + tuple(OTHERS)
# measured the same way for every app
COMMON = ("startup", "open_gallery", "videos", "pdfs", "open_list", "navigate", "scroll_end", "live", "responsive", "copy",
          "move_xdev", "trash", "windows", "idle")
# Kestrel only: its "open folders as tabs" setting, and features the other file managers have no equivalent of
KESTREL_ONLY = ("windows_tabs", "bulk_thumbs", "mosaics", "search", "metadata")
FOLDER_TESTS = ("open_gallery", "videos", "pdfs")   # "open a folder": timed by the thumbnails it makes
SCROLL_TEST = "scroll_end"   # the 600-image folder opened, then End: timed until its last LAST files have thumbnails
THUMB_TESTS = FOLDER_TESTS + (SCROLL_TEST,)   # skipped for an app whose thumbnails can't be timed
LAST = 6            # ...the last LAST by name: few enough to be on screen at the end in every app's window
NAV_TEST = "navigate"   # a running window taken to another folder (location bar): timed until its window stops changing
LIVE_TEST = "live"      # LIVE_FILES files created at once in the folder a window shows: timed until it stops changing
LIVE_FILES = 5000
RESPONSIVE = "responsive"   # Back and Forward pressed while a paste runs in the window: how long until it answers
PROBE_TIMEOUT = 30  # ...a key it hasn't answered in this long counts as this long
LIST_TEST = "open_list"   # "open a folder of 10,000 files": timed until its window has stopped changing (the list shown)
LIST_QUIET = 2.0          # ...for this long
FIRST = 12          # "open a folder": time until the first FIRST files (by name) have thumbnails
WINDOWS = 5         # "several windows": folders opened from outside, one after another
IDLE_SECONDS = 30   # "idle": CPU used in this long with a folder open and nothing happening
EMPTY_TIMEOUT = 600 # emptying the trash through a D-Bus service: Nemo's deletes about 50 files a second (3+ min)
UI_SETTLE = 2.0     # Dolphin, Krusader, PCManFM: time for a window to list a folder, before keys are pressed in it
UI_TRASH_TIMEOUT = 600   # Dolphin moves 10,000 files to the trash at about 100 a second, slowing as it goes
SLOW = 120          # a measurement that takes longer than this is run once: its median wouldn't tell more
THUMB_FLAVORS = ("normal", "large", "x-large", "xx-large")


# ---------------------------------------------------------------- measurements (run inside the sandbox)

def home():
    return os.path.expanduser("~")


def app_command(app, folder=None):
    if app in OTHERS:
        cmd = list(OTHERS[app]["cmd"])
    else:
        cmd = [os.path.join(ROOT, BUILD, "kes")]
    return cmd + ([folder] if folder else [])


def no_crash_report():
    """In an app's process before it starts: a crash isn't handed to the desktop's crash reporter (apport), which would
    pop up on the user's screen (PCManFM sometimes crashes as it's closed). The kernel skips a piped core_pattern for
    a process whose core size limit is 1, its mark for the crash handler's own processes."""
    resource.setrlimit(resource.RLIMIT_CORE, (1, 1))


def launch(app, folder):
    return subprocess.Popen(app_command(app, folder), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=True, preexec_fn=no_crash_report)


def stop(p):
    try:
        os.killpg(p.pid, signal.SIGTERM)
        p.wait(5)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        p.wait()


def app_pids(app):
    """Every process of this app in this run (Kestrel: its windows and its tower; the others: their process)."""
    marker = f"HOME={home()}".encode() + b"\0"
    exe = app_command(app)[-1]
    out = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == os.getpid():
            continue
        try:
            with open(f"/proc/{pid}/environ", "rb") as f:
                if marker not in f.read():
                    continue
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                args = f.read().split(b"\0")
        except OSError:
            continue
        if exe.encode() in args[:2] or (app in OTHERS and os.path.basename(args[0]) == exe.encode()):
            out.append(int(pid))
    return out


def peak_mb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            return next(int(ln.split()[1]) for ln in f if ln.startswith("VmHWM")) / 1024
    except (OSError, StopIteration):
        return None


def private_mb(pids):
    """Private memory: what the processes use beyond the libraries they share with each other and with other apps,
    so what closing them would give back. Unlike proportional memory (PSS), it doesn't depend on which other apps
    happen to be running."""
    total = 0
    for pid in pids:
        try:
            with open(f"/proc/{pid}/smaps_rollup") as f:
                total += sum(int(ln.split()[1]) for ln in f if ln.startswith(("Private_Clean:", "Private_Dirty:")))
        except OSError:
            pass
    return total / 1024


def cpu_ticks(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return int(fields[11]) + int(fields[12])   # utime + stime
    except (OSError, IndexError):
        return None


def window_ids(pid):
    r = subprocess.run(["xdotool", "search", "--onlyvisible", "--pid", str(pid)], capture_output=True, text=True)
    return r.stdout.split()


def windows_of(pid):
    return len(window_ids(pid))


def wait_for(cond, timeout, what):
    t0 = time.monotonic()
    while not cond():
        if time.monotonic() - t0 > timeout:
            raise RuntimeError(what)
        time.sleep(0.01)


def thumb_files():
    base = os.path.join(home(), ".cache", "thumbnails")
    out = set()
    for flavor in THUMB_FLAVORS:
        try:
            out.update(n for n in os.listdir(os.path.join(base, flavor)) if n.endswith(".png"))
        except OSError:
            pass
    return out


def measure_startup(app):
    t0 = time.monotonic()
    p = launch(app, f"{DATA}/small")
    try:
        wait_for(lambda: windows_of(p.pid) or p.poll() is not None, 60, "no window")
        s = time.monotonic() - t0
        time.sleep(1.0)
        return {"s": s, "rss_mb": peak_mb(p.pid)}
    finally:
        stop(p)


def measure_folder(app, folder):
    """Launch on a folder, and time it until the first FIRST files have thumbnails in the shared cache. Then wait
    until no more thumbnails appear, noting how many were made, when the last one appeared, and peak memory."""
    names = sorted(os.listdir(folder))[:FIRST]
    wanted = {hashlib.md5(("file://" + urllib.parse.quote(f"{folder}/{n}")).encode()).hexdigest() + ".png"
              for n in names}
    t0 = time.monotonic()
    p = launch(app, folder)
    try:
        try:
            wait_for(lambda: wanted <= thumb_files() or p.poll() is not None, 120, "thumbnails never appeared")
        except RuntimeError:   # this app made none of them: noted in the table instead of stopping the run
            return {"s": None, "rss_mb": peak_mb(p.pid), "n": len(thumb_files()), "all_s": None, "no_thumbs": True}
        s = time.monotonic() - t0
        count, last = len(thumb_files()), time.monotonic()
        while time.monotonic() - last < 2.0 and time.monotonic() - t0 < 180:
            time.sleep(0.05)
            n = len(thumb_files())
            if n != count:
                count, last = n, time.monotonic()
        return {"s": s, "rss_mb": peak_mb(p.pid), "n": count, "all_s": last - t0}
    finally:
        stop(p)


def window_image(win):
    return hashlib.md5(subprocess.run(["xwd", "-silent", "-id", win], capture_output=True).stdout).digest()


def settle(win, t0, what, timeout=120, quiet=LIST_QUIET):
    """When the window last changed, once it hasn't for `quiet` s (checked as fast as it can be captured)."""
    last, changed = None, time.monotonic()
    while time.monotonic() - changed < quiet:
        if time.monotonic() - t0 > timeout:
            raise RuntimeError(f"{what}: the window never stopped changing")
        now = time.monotonic()
        img = window_image(win)
        if img != last:
            last, changed = img, now
    return changed


def measure_list(app, folder):
    """Launch on a folder of 10,000 files, and time it until its window has stopped changing: the list drawn, with
    its count of items where the app shows one. The window is captured (xwd) as fast as it goes, a few ms each; the
    time is that of the last change, once nothing has changed for LIST_QUIET s (so not counting that wait). For apps
    whose thumbnails can't be timed, and measured the same way for every app."""
    t0 = time.monotonic()
    p = launch(app, folder)
    try:
        wait_for(lambda: window_ids(p.pid) or p.poll() is not None, 60, "no window")
        changed = settle(window_ids(p.pid)[0], t0, app)
        return {"s": changed - t0, "rss_mb": peak_mb(p.pid)}
    finally:
        stop(p)


def press(win, *keys, focus=True):
    """Keys pressed in a window (XTEST), as a user would."""
    if focus:
        subprocess.run(["xdotool", "windowfocus", "--sync", win], capture_output=True)
    subprocess.run(["xdotool", "key", "--clearmodifiers", *keys], check=True, capture_output=True)


def type_location(win, folder):
    """A folder typed into the window's location bar (Ctrl+L), without the Return that goes there."""
    press(win, "ctrl+l")
    subprocess.run(["xdotool", "type", "--delay", "2", "--", folder], check=True, capture_output=True)


def window_title(win):
    return subprocess.run(["xdotool", "getwindowname", win], capture_output=True, text=True).stdout.strip()


def run_cpu():
    """CPU time (ticks) of every process in this run but the benchmark's own: the app, the helpers it runs (thumbnailers,
    KIO workers) and the session's services, with the processes each has waited for (a thumbnailer that has
    finished). Only an app's own processes would leave out helpers that make thumbnails for it."""
    marker = f"HOME={home()}".encode() + b"\0"
    total = 0
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == os.getpid():
            continue
        try:
            with open(f"/proc/{pid}/environ", "rb") as f:
                if marker not in f.read():
                    continue
            with open(f"/proc/{pid}/stat") as f:
                fields = f.read().rsplit(")", 1)[1].split()
            total += sum(int(v) for v in fields[11:15])   # utime, stime, cutime, cstime
        except (OSError, IndexError, ValueError):
            pass
    return total


def ticks_ms(ticks):
    return ticks * 1000 / os.sysconf("SC_CLK_TCK")


def measure_navigate(app):
    """A window already open (on the small folder) taken to the folder of 10,000 files, then to a small one (100 files),
    through its location bar, as a user goes from folder to folder. Each is timed from the Return key until the window
    has stopped changing, as for the folder of 10,000 files (settle); typing the path isn't counted."""
    p = launch(app, f"{DATA}/small")
    try:
        wait_for(lambda: window_ids(p.pid) or p.poll() is not None, 60, "no window")
        win = window_ids(p.pid)[0]
        settle(win, time.monotonic(), f"{app} on the small folder", 60, quiet=1.0)
        r = {"rss_mb": None}
        for key, folder in (("s", f"{DATA}/flat"), ("small_s", f"{DATA}/windows/folder0")):
            type_location(win, folder)
            time.sleep(0.5)   # the location bar's completion done
            t0 = time.monotonic()
            press(win, "Return", focus=False)
            try:
                r[key] = settle(win, t0, f"{app} going to {folder}") - t0
            except RuntimeError:   # a text cursor still blinking in its location bar: noted instead of a time
                r[key], r["kept_changing"] = None, True
        r["rss_mb"] = peak_mb(p.pid)
        return r
    finally:
        stop(p)


def measure_scroll(app):
    """Open the 600-image folder and, once its first FIRST thumbnails are there, press End: timed from that key until
    the last LAST files have thumbnails (an app that makes them only for what's on screen starts on those then). Also
    the CPU time of the whole run, launch to the last thumbnail, the app and its helpers (run_cpu), so an app that
    makes thumbnails with several threads or processes can't look cheaper than it is."""
    folder = f"{DATA}/gallery"
    names = sorted(os.listdir(folder))
    key = lambda n: hashlib.md5(("file://" + urllib.parse.quote(f"{folder}/{n}")).encode()).hexdigest() + ".png"  # noqa: E731
    first, last = {key(n) for n in names[:FIRST]}, {key(n) for n in names[-LAST:]}
    before = run_cpu()
    p = launch(app, folder)
    try:
        try:
            wait_for(lambda: first <= thumb_files() or p.poll() is not None, 120, "thumbnails never appeared")
        except RuntimeError:
            return {"s": None, "no_thumbs": True}
        win = window_ids(p.pid)[0]
        subprocess.run(["xdotool", "windowfocus", "--sync", win], capture_output=True)   # before the clock starts
        t0 = time.monotonic()
        press(win, "End", focus=False)
        try:
            wait_for(lambda: last <= thumb_files() or p.poll() is not None, 120, "no thumbnails at the end")
        except RuntimeError:
            return {"s": None, "no_thumbs": True}
        s = time.monotonic() - t0
        count, quiet = len(thumb_files()), time.monotonic()
        while time.monotonic() - quiet < 2.0 and time.monotonic() - t0 < 180:   # until no more appear
            time.sleep(0.05)
            n = len(thumb_files())
            if n != count:
                count, quiet = n, time.monotonic()
        return {"s": s, "cpu_ms": ticks_ms(run_cpu() - before), "n": count, "rss_mb": peak_mb(p.pid)}
    finally:
        stop(p)


def measure_live(app):
    """LIVE_FILES files created at once in the (empty) folder a window shows, as a download, an unpacked archive or a
    build would: timed from the first one until the window has stopped changing (settle: all shown, with their count
    where the app shows one), and the CPU time the run used meanwhile (run_cpu)."""
    folder = os.path.join(home(), "incoming")
    os.makedirs(folder)
    p = launch(app, folder)
    try:
        wait_for(lambda: window_ids(p.pid) or p.poll() is not None, 60, "no window")
        win = window_ids(p.pid)[0]
        settle(win, time.monotonic(), f"{app} on an empty folder", 60, quiet=1.0)
        before = run_cpu()
        t0 = time.monotonic()
        data = b"x" * 1024
        for i in range(LIVE_FILES):
            with open(os.path.join(folder, f"file{i:05d}.txt"), "wb") as f:
                f.write(data)
        made = time.monotonic() - t0
        try:
            changed = settle(win, t0, f"{app} with files arriving")
        except RuntimeError:
            return {"s": None, "kept_changing": True}
        return {"s": changed - t0, "cpu_ms": ticks_ms(run_cpu() - before), "made_s": made, "rss_mb": peak_mb(p.pid)}
    finally:
        stop(p)


def measure_responsive(app):
    """Whether the window keeps answering while it copies. In the app's window: Select All and Copy in a folder holding
    the 20,000 files + 250 MB (hard links), into another folder through the location bar, then Paste, as a user
    would. While the paste runs, Back and Forward (Alt+Left, Alt+Right) are pressed in turn, each timed until the
    window's title shows the folder it went to. The longest and the median wait, against the median before the paste
    (what a key costs with nothing running: xdotool, the title read)."""
    src = stage(f"{DATA}/copysrc", os.path.join(home(), "from"))
    want = tree_size(src)
    to = os.path.join(home(), "to")
    os.makedirs(to)
    dst = os.path.join(to, os.path.basename(src))
    p = launch(app, os.path.dirname(src))
    try:
        wait_for(lambda: window_ids(p.pid) or p.poll() is not None, 60, "no window")
        win = window_ids(p.pid)[0]
        time.sleep(UI_SETTLE)
        title_from = window_title(win)
        press(win, "ctrl+a")
        press(win, "ctrl+c")
        type_location(win, to)
        press(win, "Return")
        time.sleep(UI_SETTLE)
        title_to = window_title(win)
        if title_from == title_to:   # its title doesn't follow the folder: nothing to watch
            return {"s": None, "same_title": True}
        here = [True]   # in "to" (Forward last), or in "from" (Back last)

        def probe():
            key, want_title = ("alt+Left", title_from) if here[0] else ("alt+Right", title_to)
            # back to the folder window first (an X round trip, not the app): a progress window that opened for the
            # paste may have taken the focus (PCManFM's does), and the key would go to it
            subprocess.run(["xdotool", "windowfocus", "--sync", win], capture_output=True)
            t0 = time.monotonic()
            press(win, key, focus=False)
            while window_title(win) != want_title:
                if time.monotonic() - t0 > PROBE_TIMEOUT:
                    break
                time.sleep(0.002)
            here[0] = not here[0]
            return time.monotonic() - t0

        idle = [probe() for _ in range(6)]
        done = []

        def watch():   # tree_size takes a while on 20,000 files: off the probing loop
            while not done:
                if os.path.isdir(dst) and count_files(dst) == want[0] and tree_size(dst) == want:
                    done.append(time.monotonic())
                    return
                time.sleep(0.1)
        t0 = time.monotonic()
        press(win, "ctrl+v", focus=False)
        watcher = threading.Thread(target=watch, daemon=True)
        watcher.start()
        waits = []
        while not done:
            if time.monotonic() - t0 > 120:
                done.append(None)
                raise Stuck(f"{app}: the paste didn't finish within 120 s")
            waits.append(probe())
            time.sleep(0.05)
        return {"s": max(waits) if waits else None, "median_s": statistics.median(waits) if waits else None,
                "idle_s": statistics.median(idle), "probes": len(waits), "copy_s": done[0] - t0,
                "rss_mb": peak_mb(p.pid)}
    finally:
        stop(p)
        shutil.rmtree(os.path.join(home(), "from"), ignore_errors=True)
        shutil.rmtree(to, ignore_errors=True)


def tree_size(path):
    files = size = 0
    for root, _dirs, names in os.walk(path):
        files += len(names)
        for n in names:
            size += os.lstat(os.path.join(root, n)).st_size
    return files, size


def count_files(path):
    return sum(len(names) for _root, _dirs, names in os.walk(path))


def listing(path):
    try:
        return os.listdir(path)
    except OSError:
        return []


class Stuck(RuntimeError):
    """A file operation another file manager accepted but didn't finish (for example waiting for a confirmation)."""


class FileOps:
    """Another file manager's file-operations D-Bus service, as other apps use it. Its calls return at once, so each
    operation is timed by watching the files until it has finished (checked every 50 ms). The service's methods and
    their arguments are read from the service itself (Nemo's, a fork of an older GNOME Files, has fewer)."""

    def __init__(self, app):
        from gi.repository import Gio, GLib
        self.Gio, self.GLib, self.app = Gio, GLib, app
        conf = OTHERS[app]
        for schema, key, value in conf["settings"]:
            subprocess.run(["gsettings", "set", schema, key, value], capture_output=True)
        self.bus_name, self.path, ops = conf["ops"]
        self.p = subprocess.Popen(conf["service"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  start_new_session=True, preexec_fn=no_crash_report)
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        t0 = time.monotonic()
        while True:
            try:
                xml = self.bus.call_sync(self.bus_name, self.path, "org.freedesktop.DBus.Introspectable",
                                         "Introspect", None, None, Gio.DBusCallFlags.NONE, 1000, None).unpack()[0]
                node = Gio.DBusNodeInfo.new_for_xml(xml)
                # operation -> (interface, method, [(argument name, signature)]), for those the service has
                self.methods = {}
                for op, (iface_name, method) in ops.items():
                    iface = node.lookup_interface(iface_name)
                    m = iface.lookup_method(method) if iface is not None else None
                    if m is not None:
                        self.methods[op] = (iface_name, method, [(a.name, a.signature) for a in m.in_args])
                if self.methods:
                    return
            except GLib.Error:
                pass
            if time.monotonic() - t0 > 30:
                stop(self.p)
                raise RuntimeError(f"{app}: no file-operations service")
            time.sleep(0.05)

    def has(self, op):
        return op in self.methods

    def call(self, op, uris=(), dest=""):
        """The method's arguments in its own order: as = the files, s = the destination folder (Thunar's display,
        startup id and working folder: empty, the defaults), a{sv} = no platform data, b = no confirmation.
        Returns when the operation started: the call, or for an operation that asks first ("confirm": Thunar's Empty
        Trash, whose call only returns once it's answered) the Return key that answers its window, as for Kestrel."""
        iface, method, args = self.methods[op]
        values = []
        for name, sig in args:
            if sig == "s" and any(w in name.lower() for w in ("display", "startup", "working")):
                values.append("")
            else:
                values.append({"as": list(uris), "s": dest, "a{sv}": {}, "b": False}[sig])
        sig = "".join(t for _n, t in args)
        params = self.GLib.Variant(f"({sig})", tuple(values)) if args else None
        if op not in OTHERS[self.app].get("confirm", ()):
            t0 = time.monotonic()
            self.bus.call_sync(self.bus_name, self.path, iface, method, params, None, self.Gio.DBusCallFlags.NONE, -1,
                               None)
            return t0
        self.bus.call(self.bus_name, self.path, iface, method, params, None, self.Gio.DBusCallFlags.NONE, -1, None,
                      None)
        self.bus.flush_sync(None)
        wait_for(lambda: window_ids(self.p.pid), 30, f"{self.app}: no question before {op}")
        subprocess.run(["xdotool", "windowfocus", "--sync", window_ids(self.p.pid)[0]], capture_output=True)
        t0 = time.monotonic()
        subprocess.run(["xdotool", "key", "Return"], check=True, capture_output=True)
        return t0

    def wait(self, done, timeout=120):
        t0 = time.monotonic()
        while not done():
            if time.monotonic() - t0 > timeout:
                raise Stuck(f"{self.app}: the operation didn't finish within {timeout} s")
            self.sample()
            time.sleep(0.05)
        self.sample()
        return time.monotonic() - t0

    def sample(self):
        # read while it runs: a service with no window may quit as soon as the operation is done (Nemo does)
        m = peak_mb(self.p.pid)
        if m is not None:
            self.peak_seen = max(self.peak_seen or 0, m)

    peak_seen = None

    def peak(self):
        self.sample()
        return self.peak_seen

    def close(self):
        stop(self.p)


def stage(src, parent):
    """A folder holding only `src` (hard links to its files, or copies on another drive), for an app whose window
    can't be opened with one item selected: Select All in it then selects just that."""
    os.makedirs(parent)
    dst = os.path.join(parent, os.path.basename(src))

    def link(a, b):
        try:
            os.link(a, b)
        except OSError:
            shutil.copy2(a, b)
    shutil.copytree(src, dst, symlinks=True, copy_function=link)
    return dst


class UiOps:
    """Dolphin, Krusader or PCManFM, which have no file-operations service: a window of the app, and keys pressed in it
    with xdotool (XTEST), as a user would. Like a service's calls, a paste returns at once (it runs as a job), so it's
    timed by watching the files."""

    def __init__(self, app, args):
        self.app = app
        self.p = subprocess.Popen(app_command(app) + args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  start_new_session=True, preexec_fn=no_crash_report)
        wait_for(lambda: window_ids(self.p.pid) or self.p.poll() is not None, 60, f"{app}: no window")
        self.win = window_ids(self.p.pid)[0]
        time.sleep(UI_SETTLE)

    def keys(self, *keys):
        subprocess.run(["xdotool", "windowfocus", "--sync", self.win], capture_output=True)
        subprocess.run(["xdotool", "key", "--clearmodifiers", *keys], check=True, capture_output=True)

    def go(self, folder):
        """Into another folder, through the location bar (Ctrl+L)."""
        self.keys("ctrl+l")
        subprocess.run(["xdotool", "type", "--delay", "2", "--", folder], check=True, capture_output=True)
        self.keys("Return")
        time.sleep(UI_SETTLE)

    wait = FileOps.wait
    sample = FileOps.sample
    peak = FileOps.peak
    peak_seen = None

    def close(self):
        stop(self.p)


def ui_transfer(app, src, dst_parent, move):
    """Copy (or Cut) `src`, then Paste it into dst_parent, in the app's window. Returns the window, and the time of the
    key that starts the operation.
    Dolphin: opened with src selected (--select), the destination typed into its location bar.
    PCManFM: no --select, so opened on a folder holding only src (Select All), then the same.
    Krusader: two panels, the destination on the left and a folder holding only src on the right (the active one, the
    cursor moved onto it with End), then Tab to the other panel. Its own F5 Copy / F6 Move don't reach it on a bare X
    server; Copy and Paste between its panels run the same KIO copy."""
    style = OTHERS[app]["ui"]
    cut = "ctrl+x" if move else "ctrl+c"
    if style == "dolphin":
        n = UiOps(app, ["--select", src])
        n.keys(cut)
        n.go(dst_parent)
    elif style == "pcmanfm":
        n = UiOps(app, [os.path.dirname(src)])
        n.keys("ctrl+a")
        n.keys(cut)
        n.go(dst_parent)
    else:   # krusader
        n = UiOps(app, ["--left", dst_parent, "--right", os.path.dirname(src)])
        n.keys("End")
        n.keys(cut)
        n.keys("Tab")
    t0 = time.monotonic()
    n.keys("ctrl+v")
    return n, t0


def measure_copy_other(app):
    src = f"{DATA}/copysrc"
    want = tree_size(src)
    dst_parent = os.path.join(home(), "copydst")
    os.makedirs(dst_parent)
    dst = os.path.join(dst_parent, "copysrc")
    n = None
    try:
        if OTHERS[app]["ops"] is None:   # Copy, then Paste in the destination
            if OTHERS[app]["ui"] != "dolphin":
                src = stage(src, os.path.join(home(), "copy"))
            n, t0 = ui_transfer(app, src, dst_parent, move=False)
        else:
            n = FileOps(app)
            if not n.has("copy"):
                return {"s": None}
            t0 = n.call("copy", [f"file://{src}"], f"file://{dst_parent}")
        n.wait(lambda: os.path.isdir(dst) and count_files(dst) == want[0] and tree_size(dst) == want)
        return {"s": time.monotonic() - t0, "rss_mb": n.peak()}
    finally:
        if n:
            n.close()
        shutil.rmtree(dst_parent, ignore_errors=True)
        shutil.rmtree(os.path.join(home(), "copy"), ignore_errors=True)


def measure_move_xdev(app):
    """Move 20,000 files + 250 MB from the home folder (/tmp) to another drive (/dev/shm): copy, then delete."""
    src = os.path.join(home(), "move", "movesrc")   # alone in its folder: PCManFM and Krusader select it with Select All
    shutil.copytree(f"{DATA}/copysrc", src, symlinks=True)
    want = tree_size(src)
    dst_parent = tempfile.mkdtemp(prefix="kestrel-bench-move.", dir="/dev/shm")
    dst = os.path.join(dst_parent, "movesrc")
    try:
        if app not in OTHERS:
            r = harness(app, "move_xdev", dst)
        else:
            n = None
            try:
                if OTHERS[app]["ops"] is None:   # Cut, then Paste on the other drive
                    n, t0 = ui_transfer(app, src, dst_parent, move=True)
                else:
                    n = FileOps(app)
                    if not n.has("move"):   # Nemo's service can only copy
                        return {"s": None}
                    t0 = n.call("move", [f"file://{src}"], f"file://{dst_parent}")
                n.wait(lambda: not os.path.exists(src) and os.path.isdir(dst) and count_files(dst) == want[0]
                       and tree_size(dst) == want)
                r = {"s": time.monotonic() - t0, "rss_mb": n.peak()}
            finally:
                if n:
                    n.close()
        if tree_size(dst) != want or os.path.exists(src):
            raise RuntimeError(f"{app}: the move didn't finish")
        return r
    finally:
        shutil.rmtree(dst_parent, ignore_errors=True)
        shutil.rmtree(os.path.dirname(src), ignore_errors=True)


def measure_trash(app):
    """Move 10,000 files (all selected in one folder) to the trash, then empty the trash."""
    victim = os.path.join(home(), "victim")
    shutil.copytree(f"{DATA}/flat", victim)
    total = len(os.listdir(victim))
    trash = os.path.join(home(), ".local", "share", "Trash")
    if app not in OTHERS:
        r = harness(app, "trash")
        if r.get("n") != total or r.get("empty_left"):
            raise RuntimeError(f"{app}: trash {r}")
        return r
    if OTHERS[app]["ops"] is None:
        return measure_trash_ui(app, victim, total, trash)
    n = FileOps(app)
    try:
        uris = [f"file://{victim}/{name}" for name in sorted(os.listdir(victim))]
        s = None
        if n.has("trash"):
            t0 = n.call("trash", uris)
            n.wait(lambda: not listing(victim) and len(listing(f"{trash}/files")) == total
                   and len(listing(f"{trash}/info")) == total)
            s = time.monotonic() - t0
        else:   # Nemo's service can't move to the trash: put them there with gio (not timed), to time emptying it
            subprocess.run(["gio", "trash", "--"] + [f"{victim}/{name}" for name in sorted(os.listdir(victim))],
                           check=True)
        if not n.has("empty"):
            return {"s": s, "rss_mb": n.peak()}
        t0 = n.call("empty")
        try:
            n.wait(lambda: not listing(f"{trash}/files") and not listing(f"{trash}/info"), EMPTY_TIMEOUT)
        except Stuck as e:
            return {"s": s, "rss_mb": n.peak(), "stuck_empty": str(e)}
        return {"s": s, "empty_s": time.monotonic() - t0, "rss_mb": n.peak()}
    finally:
        n.close()


def measure_trash_ui(app, victim, total, trash):
    """Select All and Delete (Move to Trash) in the app's window (Krusader: in its right panel, the active one); then
    Empty Trash. Dolphin's and Krusader's Empty Trash run KIO's empty-trash job; ktrash6 --empty runs that same job, so
    it's used for that step. PCManFM's: in its window, the trash (trash:///) opened, Select All, then Shift+Delete,
    which deletes them through gvfs's trash, as its Empty Trash does."""
    style = OTHERS[app]["ui"]
    n = UiOps(app, ["--right", victim] if style == "krusader" else [victim])
    try:
        n.keys("ctrl+a")
        t0 = time.monotonic()
        n.keys("Delete")
        n.wait(lambda: not listing(victim) and len(listing(f"{trash}/files")) == total
               and len(listing(f"{trash}/info")) == total, UI_TRASH_TIMEOUT)
        s = time.monotonic() - t0
        empty = None
        if style == "pcmanfm":
            n.go("trash:///")
            # all 10,000 listed before Select All: it lists the trash for several seconds without redrawing its window
            # (keys pressed meanwhile are lost), so a long quiet, not timed
            settle(n.win, time.monotonic(), "pcmanfm's trash", 300, quiet=30)
            # it sometimes misses a key (Select All: then only the clicked file goes): if the trash stops emptying
            # for 20 s, what's left is selected and deleted again, timed from then
            for _attempt in range(3):
                subprocess.run(["xdotool", "mousemove", "--window", n.win, "200", "110", "click", "1"], check=True,
                               capture_output=True)   # into the list (the keys went to its location bar)
                time.sleep(0.5)
                n.keys("ctrl+a")
                time.sleep(0.5)
                t0 = time.monotonic()
                n.keys("shift+Delete")
                left, moved = len(listing(f"{trash}/files")), time.monotonic()
                while left and time.monotonic() - moved < 20:
                    n.sample()
                    time.sleep(0.05)
                    now = len(listing(f"{trash}/files"))
                    if now != left:
                        left, moved = now, time.monotonic()
                if not left:
                    break
        else:
            ktrash = shutil.which("ktrash6") or shutil.which("ktrash5")
            if not ktrash:
                return {"s": s, "rss_mb": n.peak()}
            t0 = time.monotonic()
            empty = subprocess.Popen([ktrash, "--empty"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            n.wait(lambda: not listing(f"{trash}/files") and not listing(f"{trash}/info"), EMPTY_TIMEOUT)
        except Stuck as e:
            return {"s": s, "rss_mb": n.peak(), "stuck_empty": str(e)}
        finally:
            if empty:
                empty.wait()
        return {"s": s, "empty_s": time.monotonic() - t0, "rss_mb": n.peak()}
    finally:
        n.close()


def measure_windows(app, tabs=False):
    """Open WINDOWS folders from outside, one after another (as from a browser's "Show in folder" or a terminal),
    then measure the memory of everything the app is running for them."""
    if tabs:
        os.makedirs(os.path.join(home(), ".config", "kestrel-explorer"), exist_ok=True)
        with open(os.path.join(home(), ".config", "kestrel-explorer", "kestrel-explorer.conf"), "w") as f:
            f.write("[General]\nopen_in_tabs=true\n")
    folders = [f"{DATA}/windows/folder{k}" for k in range(WINDOWS)]
    procs = []
    try:
        for i, folder in enumerate(folders):
            p = launch(app, folder)
            procs.append(p)
            # each launch either shows its own window, or hands the folder over and exits
            wait_for(lambda: windows_of(p.pid) or p.poll() is not None, 60, "no window")
            if i == 0:
                time.sleep(1.0)   # the first one also starts the tower (Kestrel) or the other app's service
        if app in OTHERS and OTHERS[app].get("tabs"):   # PCManFM: one window, the later folders become its tabs
            wait_for(lambda: all(p.poll() is not None for p in procs[1:]), 30, "not all folders handed over")
        elif app in OTHERS:   # GNOME Files and Nemo: one process, the later launches hand their folder to it
            wait_for(lambda: sum(windows_of(pid) for pid in app_pids(app)) >= WINDOWS, 30, "not all windows opened")
        elif not tabs:
            wait_for(lambda: all(windows_of(p.pid) for p in procs), 30, "not all windows opened")
        time.sleep(2.0)
        pids = app_pids(app)
        return {"s": 0, "mem_mb": private_mb(pids), "procs": len(pids)}
    finally:
        for p in procs:
            if p.poll() is None:
                stop(p)


def measure_idle(app):
    """CPU used in IDLE_SECONDS with a folder open and nothing happening (after 5 s to settle)."""
    p = launch(app, f"{DATA}/small")
    try:
        wait_for(lambda: windows_of(p.pid) or p.poll() is not None, 60, "no window")
        time.sleep(5.0)
        before = {pid: cpu_ticks(pid) for pid in app_pids(app)}
        time.sleep(IDLE_SECONDS)
        used = 0
        for pid in app_pids(app):
            now = cpu_ticks(pid)
            if now is not None:
                used += now - (before.get(pid) or 0)
        return {"s": 0, "cpu_ms": used * 1000 / os.sysconf("SC_CLK_TCK")}
    finally:
        stop(p)


def harness(app, test, arg=None):
    """A measurement inside the app (kestrel_cpp)."""
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    cmd = [os.path.join(HERE, BUILD, "kestrel_cpp"), test, DATA]
    out = subprocess.run(cmd + ([arg] if arg else []), env=env, capture_output=True, text=True, timeout=900)
    line = next((ln for ln in out.stdout.splitlines() if ln.startswith("{")), None)
    if line is None:
        raise RuntimeError(f"{app} {test} failed:\n{out.stderr[-2000:]}")
    return json.loads(line)


def start_desktop_services():
    """On a desktop, gvfs is already running when an app starts. Start it here (the daemon, the volume monitors and
    the metadata store), so no app pays for starting it."""
    from gi.repository import Gio
    Gio.File.new_for_uri("trash:///").query_info("standard::*", Gio.FileQueryInfoFlags.NONE, None)
    Gio.VolumeMonitor.get().get_mounts()
    subprocess.run(["gio", "info", "-a", "metadata::*", home()], capture_output=True)
    time.sleep(0.5)


def measure(app, test):
    for name, text in (OTHERS[app]["files"] if app in OTHERS else {}).items():
        path = os.path.join(home(), name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
    start_desktop_services()
    if test == "startup":
        return measure_startup(app)
    if test in FOLDER_TESTS:
        return measure_folder(app, f"{DATA}/{'gallery' if test == 'open_gallery' else test}")
    if test == LIST_TEST:
        return measure_list(app, f"{DATA}/flat")
    if test == NAV_TEST:
        return measure_navigate(app)
    if test == SCROLL_TEST:
        return measure_scroll(app)
    if test == LIVE_TEST:
        return measure_live(app)
    try:
        if test == RESPONSIVE:
            return measure_responsive(app)
        if test == "copy" and app in OTHERS:
            return measure_copy_other(app)
        if test == "move_xdev":
            return measure_move_xdev(app)
        if test == "trash":
            return measure_trash(app)
    except Stuck as e:   # noted in the table instead of stopping the run
        return {"s": None, "stuck": str(e)}
    if test in ("windows", "windows_tabs"):
        return measure_windows(app, tabs=test == "windows_tabs")
    if test == "idle":
        return measure_idle(app)
    return harness(app, test)


# ---------------------------------------------------------------- the sandbox

class Sandbox:
    """A headless X server, plus a fresh home folder and private session bus for each run."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="kestrel-bench.")
        self.confs = {}
        self.display = next(f":{n}" for n in range(90, 200) if not os.path.exists(f"/tmp/.X11-unix/X{n}"))
        self.xvfb = subprocess.Popen(["Xvfb", self.display, "-screen", "0", "1920x1080x24", "-nolisten", "tcp"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        sock = f"/tmp/.X11-unix/X{self.display[1:]}"
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.05)

    def conf(self, app):
        """The session bus's configuration for this app: settings (dconf) and the virtual file system (gvfs: trash:///,
        volumes, metadata) work as on a desktop, and the services the app itself needs ("dbus": Thunar's settings
        store and thumbnail service); nothing else can be started (no file indexer, portals, online accounts…)."""
        if app in self.confs:
            return self.confs[app]
        services = os.path.join(self.dir, f"services-{app}")
        os.makedirs(services)
        system = "/usr/share/dbus-1/services"
        wanted = ("org.gtk.vfs.",) + tuple(OTHERS[app].get("dbus", []) if app in OTHERS else ())
        for name in os.listdir(system):
            if name == "ca.desrt.dconf.service" or name.startswith(wanted):
                shutil.copy(os.path.join(system, name), services)
        with open("/usr/share/dbus-1/session.conf") as f:
            conf = re.sub(r"<standard_session_servicedirs\s*/>", f"<servicedir>{services}</servicedir>", f.read())
        self.confs[app] = os.path.join(self.dir, f"session-{app}.conf")
        with open(self.confs[app], "w") as f:
            f.write(conf)
        return self.confs[app]

    def run(self, app, test):
        home_dir = tempfile.mkdtemp(prefix="home.", dir=self.dir)
        env = {k: v for k, v in os.environ.items()
               if k not in ("XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "DBUS_SESSION_BUS_ADDRESS",
                            "WAYLAND_DISPLAY", "XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID", "CONDA_DEFAULT_ENV")}
        env.update(HOME=home_dir, DISPLAY=self.display, XDG_SESSION_TYPE="x11", QT_QPA_PLATFORM="xcb",
                   GDK_BACKEND="x11", GSK_RENDERER="cairo", GTK_A11Y="none", NO_AT_BRIDGE="1",
                   KESTREL_BENCH_DATA=DATA)
        try:
            out = subprocess.run(["dbus-run-session", f"--config-file={self.conf(app)}", "--", "/usr/bin/python3",
                                  os.path.abspath(__file__), "--measure", app, test], env=env, capture_output=True,
                                 text=True, timeout=1200)
        finally:
            shutil.rmtree(home_dir, ignore_errors=True)
        line = next((ln for ln in out.stdout.splitlines() if ln.startswith("{")), None)
        if line is None:
            raise RuntimeError(f"{app} {test}: {out.stdout[-1500:]}{out.stderr[-1500:]}")
        return json.loads(line)

    def close(self):
        self.xvfb.terminate()
        self.xvfb.wait()
        shutil.rmtree(self.dir, ignore_errors=True)


# ---------------------------------------------------------------- results

def secs(v):
    if v is None:
        return "—"
    return f"{v:.3f} s" if v < 0.1 else f"{v:.2f} s"


def ratio(a, b):
    """How many times faster b is than a, in words."""
    if not a or not b:
        return "—"
    r = a / b
    if r >= 1.05:
        return f"{r:.1f}× faster" if r < 10 else f"about {round(r)}× faster"
    if r <= 0.95:
        return f"{1 / r:.1f}× slower"
    return "about the same"


def mb(v):
    return "—" if v is None else f"{round(v)}"


# (label, test, key) for the rows timed in every app
TIMED = [("Startup (launch to window shown)", "startup", "s"),
         (f"Open a 600-image folder (launch to the first {FIRST} thumbnails)", "open_gallery", "s"),
         (f"Open a folder of 40 videos (launch to the first {FIRST} thumbnails)", "videos", "s"),
         (f"Open a folder of 40 PDFs (launch to the first {FIRST} thumbnails)", "pdfs", "s"),
         ("Open a folder of 10,000 files (launch to the full list shown)", LIST_TEST, "s"),
         ("Go to the folder of 10,000 files in an open window (Enter to the list shown)", NAV_TEST, "s"),
         ("Go to a folder of 100 files in an open window (Enter to the list shown)", NAV_TEST, "small_s"),
         (f"Jump to the end of the 600-image folder (End to its last {LAST} thumbnails)", SCROLL_TEST, "s"),
         (f"{LIVE_FILES:,} files arriving in an open folder (until all shown)", LIVE_TEST, "s"),
         ("Copy 20,000 small files + 5 × 50 MB", "copy", "s"),
         ("Move the same to another drive", "move_xdev", "s"),
         ("Move 10,000 files to the trash (all selected in one folder)", "trash", "s"),
         ("Empty the trash (those 10,000 files)", "trash", "empty_s")]


def tables(res, other):
    """The tables comparing Kestrel with `other` (a key of OTHERS), the summary for the docs/performance.md text, and notes on what
    `other` couldn't be timed on."""
    apps = ("kestrel", other)
    if not OTHERS[other]["thumbnails"]:   # also drops results saved before its folder tests were skipped
        res = {k: v for k, v in res.items() if not (k[0] == other and k[1] in THUMB_TESTS)}
    res = {k: v for k, v in res.items() if not (k[0] == other and k[1] in OTHERS[other]["skip"])}

    def med(app, test, key="s"):
        vals = [r[key] for r in res.get((app, test), []) if r.get(key) is not None]
        return statistics.median(vals) if vals else None

    t1 = [f"| Test | Kestrel | {OTHERS[other]['label']} |", "|---|---|---|"]
    for label, test, key in TIMED:
        t1.append(f"| {label} | {' | '.join(secs(med(a, test, key)) for a in apps)} |")

    def mem(app):
        a, b = med(app, "startup", "rss_mb"), med(app, "open_gallery", "rss_mb")
        return "—" if a is None and b is None else f"{mb(a)} / {mb(b)} MB"

    def windows(app):
        sep, tabs = med(app, "windows", "mem_mb"), med(app, "windows_tabs", "mem_mb")
        if sep is None:
            return "—"
        if app in OTHERS and OTHERS[app].get("tabs"):
            return f"{mb(sep)} MB (as tabs)"
        return f"{mb(sep)} MB" + (f" ({mb(tabs)} MB as tabs)" if tabs is not None else "")

    def idle(app):
        v = med(app, "idle", "cpu_ms")
        return "—" if v is None else f"{round(v)} ms ({v / (IDLE_SECONDS * 10):.2f}% of a core)"

    def cpu(app, test):
        v = med(app, test, "cpu_ms")
        return "—" if v is None else f"{round(v)} ms"

    def answer(app):
        worst, typical, idle_ = (med(app, RESPONSIVE, k) for k in ("s", "median_s", "idle_s"))
        return "—" if worst is None else f"{secs(worst)} / {secs(typical)} (idle {secs(idle_)})"

    t1.append(f"| Window answering while it copies (Back/Forward to the new title: longest / median) | "
              f"{' | '.join(answer(a) for a in apps)} |")
    t1.append(f"| CPU time to open the 600-image folder and jump to its end (app and helpers) | "
              f"{' | '.join(cpu(a, SCROLL_TEST) for a in apps)} |")
    t1.append(f"| CPU time for {LIVE_FILES:,} files arriving in an open folder (app and helpers) | "
              f"{' | '.join(cpu(a, LIVE_TEST) for a in apps)} |")
    t1.append(f"| Peak memory (startup / 600-image folder open) | {' | '.join(mem(a) for a in apps)} |")
    t1.append(f"| Memory with {WINDOWS} folders opened from other apps | {' | '.join(windows(a) for a in apps)} |")
    t1.append(f"| CPU time used in {IDLE_SECONDS} s with a folder open, idle | {' | '.join(idle(a) for a in apps)} |")

    def row2(label, test):
        return f"| {label} | {secs(med('kestrel', test))} |"

    def bg(app):
        vals = [med(app, t, "rss_mb") for t in ("bulk_thumbs", "mosaics", "search", "metadata", "copy")]
        vals = [v for v in vals if v is not None]
        return f"{round(min(vals))}–{round(max(vals))} MB" if vals else "—"

    t2 = ["| Test | Kestrel |", "|---|---|",
          row2("Thumbnail all 600 images (\"Generate Previews\")", "bulk_thumbs"),
          row2("Build 150 folder mosaics", "mosaics"),
          row2("Recursive search over 50,000 files", "search"),
          row2("Read EXIF / image-generation prompts for 400 images", "metadata"),
          f"| Peak memory (background jobs) | {bg('kestrel')} |"]

    vs = []
    for label, test, key in TIMED:
        short = {("startup", "s"): "startup", ("open_gallery", "s"): "first thumbnails in a 600-image folder",
                 ("videos", "s"): "first video thumbnails", ("pdfs", "s"): "first PDF thumbnails",
                 (LIST_TEST, "s"): "listing a folder of 10,000 files",
                 (NAV_TEST, "s"): "going to the folder of 10,000 files in an open window",
                 (NAV_TEST, "small_s"): "going to a small folder in an open window",
                 (SCROLL_TEST, "s"): "thumbnails at the end of the 600-image folder",
                 (LIVE_TEST, "s"): "showing files as they arrive", ("copy", "s"): "copying",
                 ("move_xdev", "s"): "moving to another drive", ("trash", "s"): "moving to the trash",
                 ("trash", "empty_s"): "emptying the trash"}[(test, key)]
        if med(other, test, key) is not None:
            vs.append(f"- {short}: {ratio(med(other, test, key), med('kestrel', test, key))};")
    if vs:
        vs[-1] = vs[-1][:-1] + "."
    gallery_n, kes_n = med(other, "open_gallery", "n"), med("kestrel", "open_gallery", "n")
    ops = {"copy": "copy files", "move_xdev": "move files to another drive", "trash": "move files to the trash"}
    runs_of = lambda test: res.get((other, test), [])   # noqa: E731
    stuck = [phrase for test, phrase in ops.items() if runs_of(test) and all(r.get("stuck") for r in runs_of(test))]
    if runs_of("trash") and all(r.get("stuck_empty") for r in runs_of("trash")):
        stuck.append("empty the trash")
    untimed = [phrase for test, phrase in ops.items()
               if runs_of(test) and med(other, test) is None and phrase not in stuck]
    kinds = {"open_gallery": "images", "videos": "videos", "pdfs": "PDFs"}
    no_thumbs = [kinds[t] for t in kinds if res.get((other, t)) and all(r.get("no_thumbs") for r in res[(other, t)])]
    # measured once (over SLOW): say so, since the others are medians of several runs
    most = max((len(v) for (_a, t), v in res.items() if t not in ("startup", "idle")), default=0)
    once = [{"copy": "copying", "move_xdev": "moving to another drive", "trash": "the trash test"}[t]
            for t in ("copy", "move_xdev", "trash") if len(runs_of(t)) == 1 < most]
    summary = {"vs": "\n".join(vs), "no_thumbs": no_thumbs, "once": once,
               "files_n": round(gallery_n) if gallery_n else None,
               "files_all": secs(med(other, "open_gallery", "all_s")),
               "kestrel_n": round(kes_n) if kes_n else None,
               "kestrel_all": secs(med("kestrel", "bulk_thumbs")),
               "procs": round(med("kestrel", "windows", "procs") or 0),
               "untimed": untimed, "stuck": stuck,
               # the tests that couldn't give a time for this app, and why
               "kept_changing": [what for test, what in ((NAV_TEST, "going to a folder in an open window"),
                                                         (LIVE_TEST, "files arriving in an open folder"))
                                 if any(r.get("kept_changing") for r in runs_of(test))],
               "same_title": any(r.get("same_title") for r in runs_of(RESPONSIVE)),
               # the last thumbnails already there when End was pressed (Kestrel makes the first and last screenful of
               # a folder when it opens it)
               "ready_at_end": [OTHERS[a]["label"] if a in OTHERS else "Kestrel" for a in apps
                                if med(a, SCROLL_TEST) is not None and med(a, SCROLL_TEST) < 0.05]}
    # idle CPU far above Kestrel's: worth a word of caution (Nemo kept a fifth of a core busy in the Mint VM)
    busy, calm = med(other, "idle", "cpu_ms"), med("kestrel", "idle", "cpu_ms")
    summary["busy_idle"] = busy / (IDLE_SECONDS * 10) if busy and busy > 1000 and busy > 10 * (calm or 0) else None
    return t1, t2, summary


def machine(other):
    cpu = "unknown CPU"
    with open("/proc/cpuinfo") as f:
        for ln in f:
            if ln.startswith("model name"):
                cpu = ln.split(":", 1)[1].strip()
                break
    version = ""
    try:   # its --version (Krusader prints log lines first; PCManFM has none: its package's version)
        out = subprocess.run(OTHERS[other]["cmd"] + ["--version"], capture_output=True, text=True).stdout
        number = next((m.group(1) for m in map(re.compile(r"\b(\d+\.\d+(?:\.\d+)*)").search, out.splitlines()) if m),
                      None)
        if number is None:
            out = subprocess.run(["dpkg-query", "-W", "-f=${Version}", OTHERS[other]["cmd"][0]], capture_output=True,
                                 text=True).stdout
            number = re.match(r"(?:\d+:)?(\d+(?:\.\d+)*)", out)
            number = number.group(1) if number else None
        if number:
            version = f"{OTHERS[other]['label']} {number}"
    except OSError:   # a report made on another machine
        pass
    distro = ""
    try:
        with open("/etc/os-release") as f:
            distro = next((ln.split("=", 1)[1].strip().strip('"') for ln in f if ln.startswith("PRETTY_NAME=")), "")
    except OSError:
        pass
    if PLATFORM:   # the container is Ubuntu's image with the desktop's packages added
        distro = (f"{distro.replace('Ubuntu', PLATFORM, 1)}, in a Docker container (Ubuntu's image with the {PLATFORM} "
                  f"packages the apps use)")
    return cpu, os.cpu_count(), version or OTHERS[other]["label"], distro


def section(t1, t2, summary, runs, other):
    cpu, threads, version, distro = machine(other)
    label = OTHERS[other]["label"]
    heading = OTHERS[other]["heading"] + (f" on {PLATFORM}" if PLATFORM else "")
    intro = OTHERS[other]["intro"] + (f", which it replaces on {PLATFORM}" if PLATFORM else "")
    run_with = f"`bench/{PLATFORM.lower()}.sh`" if PLATFORM else "`bench/run.sh`"
    kde = (" The desktop was set to KDE's, so both apps used Plasma's Qt integration and the Breeze style, as on a "
           "Plasma desktop." if os.environ.get("XDG_CURRENT_DESKTOP") == "KDE" else "")
    bench_link = "https://github.com/RegulusArms/kes-c/tree/main/bench"
    owns = label + ("'" if label.endswith("s") else "'s")   # GNOME Files', Nemo's
    ui = OTHERS[other]["ops"] is None   # Dolphin, Krusader, PCManFM: no file-operations service, keys in its window
    if not OTHERS[other]["thumbnails"]:
        thumbs = f"{label} wasn't timed on this (see below)."
    elif "images" in summary["no_thumbs"]:
        thumbs = f"{label} made no thumbnails for these images in this setup."
    elif OTHERS[other]["whole_folder"]:
        thumbs = (f"{label} makes thumbnails for the whole folder in the background. It finished all "
                  f"{summary['files_n']} images {summary['files_all']} after launch.")
    else:
        thumbs = f"{label} made {summary['files_n']} thumbnails in all, the last {summary['files_all']} after launch."
    untimed = ""
    if not OTHERS[other]["thumbnails"]:
        untimed += (f"\n- **Thumbnails not timed:** {label} {OTHERS[other]['no_thumbs_why']}, so the thumbnail tests "
                    f"are skipped for it and those rows show —, as does its peak memory with the 600-image folder open. "
                    f"How fast it opens a folder is compared by the folder of 10,000 files instead.")
    kinds = {"open_gallery": "images", "videos": "videos", "pdfs": "PDFs"}
    for test, why in OTHERS[other]["skip"].items():
        untimed += (f"\n- **{kinds[test].capitalize()} not timed:** {why}, so {label} makes no thumbnails for them in a fresh "
                    f"home folder, and that row shows —.")
    if summary["no_thumbs"]:
        untimed += (f"\n- **No thumbnails:** {label} made none for the {' or '.join(summary['no_thumbs'])} within 2 minutes "
                    f"in this setup (a fresh home folder with its default settings), so those rows show —.")
    if summary["untimed"]:
        untimed += (f"\n- **Not timed:** {owns} D-Bus file-operations service has no way to "
                   f"{' or '.join(summary['untimed'])}, so those rows show —.")
        if "move files to the trash" in summary["untimed"]:
            untimed += " For \"Empty the trash\", the files were put in the trash with `gio trash` first."
    if "empty" in OTHERS[other].get("confirm", ()):
        untimed += (f"\n- **Empty the trash:** {label} asks first, even when asked through its D-Bus service; the "
                    f"question was answered at once (Return), and the time counted from that answer, as for Kestrel's.")
    if OTHERS[other].get("no_responsive"):
        untimed += (f"\n- **Window answering while it copies:** not run for {label}, which "
                    f"{OTHERS[other]['no_responsive']}; that row shows —.")
    if summary["same_title"]:
        untimed += (f"\n- **Window answering while it copies:** {owns} window title doesn't change with its folder, so "
                    f"there was nothing to watch, and that row shows —.")
    if summary["kept_changing"]:
        untimed += (f"\n- **Kept changing:** for {' and '.join(summary['kept_changing'])}, {owns} window didn't stop "
                    f"changing within 2 minutes (a text cursor blinking in it), so that row shows —.")
    if summary["ready_at_end"]:
        untimed += (f"\n- **Jumping to the end:** {' and '.join(summary['ready_at_end'])} already had the last "
                    f"thumbnails when End was pressed (made when the folder opened, with the first screenful), so the "
                    f"time is only that of seeing them.")
    if OTHERS[other]["whole_folder"] and OTHERS[other]["thumbnails"]:
        untimed += (f"\n- **Jumping to the end:** {label} makes thumbnails for the whole folder in the background, so "
                    f"some may be ready before End is pressed.")
    if OTHERS[other].get("tabs"):
        untimed += (f"\n- **Several folders:** {label} opens folders from other apps as tabs in its window (its "
                    f"default), so its figure is for one window with {WINDOWS} tabs, like Kestrel's \"as tabs\" one.")
    if summary["busy_idle"]:
        headless = " (where it also made no thumbnails)" if not OTHERS[other]["thumbnails"] or summary["no_thumbs"] else ""
        untimed += (f"\n- **CPU while idle:** {label} kept about {summary['busy_idle']:.0f}% of a core busy with nothing "
                    f"happening. That's unusual for a file manager at rest, so it probably comes from this headless "
                    f"setup{headless} rather than from everyday use.")
    if summary["once"]:
        untimed += (f"\n- **Measured once:** {label} took over {SLOW // 60} minutes for {' and '.join(summary['once'])}, "
                    f"so it was run once instead of {runs} times.")
    if summary["stuck"]:
        untimed += (f"\n- **Didn't finish:** asked {'in its window' if ui else 'through its D-Bus service'} to {' or '.join(summary['stuck'])}, "
                    f"{label} didn't finish within the time limit (2 minutes; {EMPTY_TIMEOUT // 60} for emptying the trash"
                    f"{f', and for moving to it' if ui else ''}), so those rows show —.")
    if ui:
        ktrash = ("Its Empty Trash runs KDE's empty-trash job, which `ktrash6 --empty` runs too; that was used for this "
                  "step.")
        ops_how = f"{label} has no D-Bus file-operations service, so its own commands are used, with keys pressed in its window: " + {
            "dolphin": "Copy or Cut, then Paste in the destination (typed into its location bar), and Select All, then "
                       "Delete (Move to Trash). " + ktrash,
            "krusader": "the source in one panel and the destination in the other, Copy or Cut, then Paste in the other "
                        "panel (its F5 Copy and F6 Move keys don't reach it on a bare X server; Paste runs the same KIO "
                        "copy), and Select All, then Delete (Move to Trash). " + ktrash,
            "pcmanfm": "Copy or Cut, then Paste in the destination (typed into its location bar), and Select All, then "
                       "Delete (Move to Trash). To empty the trash, its window was taken to the trash (`trash:///`), and "
                       "everything there selected and deleted with Shift+Delete, which removes them through the same "
                       "gvfs trash as its Empty Trash.",
        }[OTHERS[other]["ui"]]
        ops_limit = (f"{label} runs a paste or a move to the trash in the background, so its times were measured from "
                     f"the key press, by watching the files until the operation had finished, to within about 50 ms. "
                     f"Its window was given {UI_SETTLE:.0f} s to list the folder first, which isn't counted.")
    else:
        ops_how = f"{label} receives them through its D-Bus file-operations service, as when another app asks it to."
        ops_limit = (f"{owns} D-Bus service returns straight away, so its times were measured by watching the files "
                     f"until the operation had finished, to within about 50 ms.")
    renderers = "Qt's raster engine, GTK's cairo renderer" if OTHERS[other]["gtk"] else "Qt's raster engine, both being Qt apps"
    features = OTHERS[other].get("features") or (f"{label} has no equivalent (\"Generate Previews\", folder mosaics, "
                                                 f"metadata panels) or can't be timed from outside (search)")
    return f"""{heading}

Kestrel Explorer is compared here with {version}, {intro}.

**Test machine:** {cpu} ({threads} threads), {distro}. The test data is on a RAM disk: 600 JPEGs at 1600×1200 with camera EXIF, 40 videos, 40 PDFs, 150 folders of 4 images, a tree of 50,000 files, 20,000 small files plus 250 MB, a folder of 10,000 files, and 200 PNGs with Stable Diffusion prompts.

**How it was measured:** each test ran {runs} times, and the tables show medians. Every run started with a fresh home folder, so the thumbnail cache was empty. Both apps ran on a headless X server with software rendering ({renderers}), on a private session bus where only the desktop's settings and virtual file system (gvfs) services could start{OTHERS[other].get("dbus_text", "")}, so no file indexer ran.{kde} The benchmark is in [bench/]({bench_link}) and is run with {run_with}.

### Compared with {label}

Both apps are measured in the same way:
- **Startup:** timed until the window is on screen.
- **Opening a folder:** timed from launch until the first {FIRST} files' thumbnails are in the shared thumbnail cache.
- **Opening a folder of 10,000 files:** timed from launch until the window has stopped changing: the list drawn, with its count of items where the app shows one. The window is captured as often as it can be (every few ms), and the time is that of its last change, once nothing has changed for {LIST_QUIET:.0f} s.
- **Going to a folder in an open window:** the folder's path typed into the location bar of a window that's already open, timed from Enter until the window has stopped changing, as above. Most folders are opened this way rather than by starting the app.
- **Jumping to the end of the 600-image folder:** once the first {FIRST} thumbnails are there, End is pressed, timed until the last {LAST} files have thumbnails. With it, the CPU time used from launch to the last thumbnail, by the app and every helper it ran (thumbnailers, KIO workers, services), so an app that spreads the work over threads or processes can't look cheaper than it is.
- **Files arriving in an open folder:** {LIVE_FILES:,} small files created at once in the folder a window shows, as a download, an unpacked archive or a build would, timed until the window has stopped changing, with the CPU time used meanwhile.
- **Window answering while it copies:** the 20,000 files + 250 MB copied and pasted in the app's window, and while the paste runs, Back and Forward pressed in turn, each timed until the window's title shows the folder it went to. The longest and the median wait, and in brackets the median with nothing running (what pressing a key and reading the title cost by themselves).
- **File operations:** Kestrel runs them with its own copy and trash code, the same code its menus use. {ops_how}
- **"Peak memory":** the app's highest memory use.
- **Several folders opened from other apps:** the {WINDOWS} folders are opened one after another, as from a browser's "Show in folder". The figure is the private memory of everything the app then runs: what closing it would give back, not counting the libraries it shares with other apps. Kestrel normally starts a new process for each window ({summary['procs']} processes here, counting the tower that keeps them in sync); with "open folders as tabs" on, they become tabs in one window.
- **CPU while idle:** the CPU time all the app's processes use in {IDLE_SECONDS} s with a folder open and nothing happening. It's counted in 10 ms steps.

{chr(10).join(t1)}

Kestrel compared with {label}:
{summary['vs']}

**Limits of this comparison:**
- **Opening a folder:** the two apps don't do the same amount of work.
  - {thumbs}
  - Kestrel makes them only for what's on screen ({summary['kestrel_n']} images here), and the rest as you scroll.
  - To thumbnail a whole folder at once, Kestrel has "Generate Previews": {summary['kestrel_all']} for these 600 images (table below).
- **File operations:** {ops_limit}{untimed}
- **Memory:** {OTHERS[other].get("memory") or f"{label} makes thumbnails in {OTHERS[other]['helpers']}, whose memory isn't counted in its figures. Kestrel makes them inside the app."}
- **Search:** {owns} search can't be timed from outside{OTHERS[other]['search']}, so it isn't compared.

### Kestrel's own features

These are measured inside the app, because {features}.

{chr(10).join(t2)}
"""


def write_results(sections):
    """bench/results.md: a Performance section (docs/performance.md) for each app compared with ("Kestrel vs GNOME Files", and/or
    "Kestrel vs Nemo" from a run on Linux Mint, "Kestrel vs Dolphin on Kubuntu" from bench/kubuntu.sh), to paste into
    docs/performance.md."""
    path = os.path.join(HERE, "results.md")
    with open(path, "w") as f:
        f.write("\n".join(sections))
    print(f"\nWrote {path}")


# ---------------------------------------------------------------- main

def build():
    print("Building Kestrel and its benchmark harness…", flush=True)
    log = open(os.path.join(HERE, "build.log"), "w")
    for cmd in ([CMAKE, "-S", ROOT, "-B", os.path.join(ROOT, BUILD), "-DCMAKE_BUILD_TYPE=Release"],
                [CMAKE, "--build", os.path.join(ROOT, BUILD), f"-j{os.cpu_count()}"],
                [CMAKE, "-S", HERE, "-B", os.path.join(HERE, BUILD), f"-DKES_SRC={ROOT}/src"],
                [CMAKE, "--build", os.path.join(HERE, BUILD), f"-j{os.cpu_count()}"]):
        if subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT).returncode:
            sys.exit(f"Build failed (log: {os.path.join(HERE, 'build.log')})")


def runs_for(test, runs):
    if test == "startup":
        return runs + 2
    if test == "idle":
        return min(runs, 2)   # each takes IDLE_SECONDS
    return runs


def report(res, args):
    runs = max((len(v) for (a, t), v in res.items() if t not in ("startup", "idle")), default=args.runs)
    others = [o for o in OTHERS if any(a == o for a, _t in res)]
    t2, sections = None, []
    for other in others or [next(iter(OTHERS))]:
        t1, t2, summary = tables(res, other)
        print()
        print("\n".join(t1))
        if others:
            sections.append(section(t1, t2, summary, runs, other))
    print()
    print("\n".join(t2))
    if sections:
        write_results(sections)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--only", default="")
    ap.add_argument("--apps", default="", help="measure only these apps (kestrel, " + ", ".join(OTHERS) + ")")
    ap.add_argument("--report", action="store_true", help="rebuild the tables from results.json without running")
    ap.add_argument("--measure", nargs=2, metavar=("APP", "TEST"), help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.measure:
        print(json.dumps(measure(*args.measure)), flush=True)
        os._exit(0)

    results_file = os.path.join(HERE, "results.json")
    saved = {}
    if os.path.exists(results_file):
        with open(results_file) as f:
            saved = {tuple(k.split(" ", 1)): v for k, v in json.load(f).items()}
    if args.report:
        if not saved:
            sys.exit("No results.json yet: run the benchmark first")
        report(saved, args)
        return

    missing = [t for t in ("Xvfb", "xdotool", "dbus-run-session", "xwd") if not shutil.which(t)]
    if missing:
        sys.exit(f"Missing: {', '.join(missing)} (sudo apt install xvfb xdotool dbus-daemon x11-apps)")
    apps = [a for a in APPS if a not in OTHERS or shutil.which(OTHERS[a]["cmd"][0])]
    chosen = [a for a in args.apps.split(",") if a]
    unknown = [a for a in chosen if a not in APPS]
    if unknown:
        sys.exit(f"Unknown app(s): {', '.join(unknown)} (apps: {', '.join(APPS)})")
    if chosen:
        apps = [a for a in apps if a in chosen]
    only = [t for t in args.only.split(",") if t]
    unknown = [t for t in only if t not in COMMON + KESTREL_ONLY]
    if unknown:
        sys.exit(f"Unknown test(s): {', '.join(unknown)} (tests: {', '.join(COMMON + KESTREL_ONLY)})")
    tests = [t for t in COMMON + KESTREL_ONLY if not only or t in only]
    build()
    subprocess.run(["/usr/bin/python3", os.path.join(HERE, "make_data.py"), DATA], check=True)

    box = Sandbox()
    res = dict(saved) if only or chosen else {}   # --only, --apps: keep the other measurements from the last run
    try:
        for test in tests:
            for app in apps:
                if app in OTHERS and test in KESTREL_ONLY:
                    continue
                if app in OTHERS and ((test in THUMB_TESTS and (not OTHERS[app]["thumbnails"]
                                                                 or test in OTHERS[app]["skip"]))
                                      or (test == RESPONSIVE and OTHERS[app].get("no_responsive"))):
                    res.pop((app, test), None)   # --only keeps saved results: drop any from before
                    continue
                if test == "videos" and not os.path.isdir(f"{DATA}/videos"):
                    continue
                n = runs_for(test, args.runs)
                res[(app, test)] = []
                for i in range(n):
                    r = box.run(app, test)
                    res[(app, test)].append(r)
                    detail = "".join(f", {k} {r[k]:.0f}" for k in ("rss_mb", "mem_mb", "cpu_ms") if r.get(k))
                    if r.get("empty_s") is not None:
                        detail += f", empty {r['empty_s']:.3f} s"
                    elif r.get("stuck_empty"):
                        detail += ", emptying didn't finish"
                    took = (f"{r['s']:.3f} s" if r["s"] is not None else "didn't finish" if r.get("stuck")
                            else "no thumbnails" if r.get("no_thumbs") else "kept changing" if r.get("kept_changing")
                            else "title doesn't follow the folder" if r.get("same_title") else "not offered")
                    if r.get("median_s") is not None:
                        took += f" longest, {r['median_s']:.3f} s median, {r['idle_s']:.3f} s idle"
                    print(f"  {test:13} {app:9} run {i + 1}/{n}: {took}{detail}", flush=True)
                    if max(r.get("s") or 0, r.get("empty_s") or 0) > SLOW and i + 1 < n:
                        print(f"  {test:13} {app:9} over {SLOW // 60} minutes: measured once", flush=True)
                        break
                with open(results_file, "w") as f:   # saved as it goes, so an interrupted run isn't lost
                    json.dump({f"{a} {t}": v for (a, t), v in res.items()}, f, indent=1)
    finally:
        box.close()
    report(res, args)


if __name__ == "__main__":
    main()
