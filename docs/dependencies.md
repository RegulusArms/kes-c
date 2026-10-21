# Dependencies

[← Back to the README](../README.md)

Package names below are Ubuntu's; Linux Mint uses the same ones.

**Anaconda / conda:** if `conda init` put Anaconda first on your PATH, Kestrel still uses the system's programs and libraries (gsettings, gio, ffmpeg, xz…), because Anaconda's copies don't match the desktop (its `gsettings` can't see your real settings, for example). Anaconda's folders are moved to the end of the search paths at startup. If you start Kestrel from an environment you activated yourself (`conda activate myenv`; anything but the auto-activated `base`), the environment is left exactly as it is.

**To build**

| Package | Used for |
|---|---|
| `g++`, `cmake`, `pkg-config` | Compiling |
| `qt6-base-dev` | The whole user interface (Qt 6 Widgets), image decoding, settings |
| `libglib2.0-dev` | GIO: default and "Open With" apps, file type detection, trash, drive and network mounting, D-Bus for UWP |
| `libxcb1-dev` | Giving the focus to the app files are dropped into, on X11 |
| `libsecret-1-dev` | Remembering archive passwords in the desktop's keyring (GNOME Keyring, KWallet) |

At run time these become the ordinary `libqt6widgets6`, `libglib2.0`, `libxcb1` and `libsecret-1-0` libraries, which every Ubuntu desktop already has. PyQt6 and Pillow are not needed. The system's `python3` (with `python3-gi`) is used only to read the GTK theme's colours: always on Qt 6.4 (see [Linux Mint 22 and Ubuntu 24.04](install.md#linux-mint-22-and-ubuntu-2404)), and on newer Qt when the desktop wants the dark style but Qt's own colours are light. Without it, Kestrel uses its built-in dark colours there.

**Recommended**: run `./install.sh --install-recommended` to install any of these you're missing. Ubuntu already includes most of them. The ones you'll usually need to add are `kimageformat6-plugins` (RAW and HEIC previews) and some of the archive tools, such as `unrar`, `pigz`, `zpaq` and `lzip`.

| Package | Used for |
|---|---|
| `libglib2.0-bin` | `gio` (launching `.desktop` shortcuts, unmounting) and `gsettings` (icon theme, set as wallpaper) |
| `xdg-utils` | `xdg-open` (opening files if GIO can't) and `xdg-mime` (making Kestrel the default folder app) |
| `gvfs`, `gvfs-backends` | Network locations (`smb://`, `sftp://`, `nfs://`, `ftp://`…), the drive list on the Overview page, and phones and cameras (iPhone, Android, PTP cameras; iPhones also use `usbmuxd`, which comes with them) |
| `udisks2` | Mounting, unlocking and ejecting drives from the Overview page |
| `qt6-image-formats-plugins` | WebP, TIFF, TGA, ICNS and MNG images |
| `qt6-svg-plugins` (in `libqt6svg6` on Ubuntu 24.04 / Linux Mint 22) | SVG images |
| `kimageformat6-plugins` | Camera RAW (`.raf`, `.cr2`, `.cr3`, `.nef`, `.arw`, `.dng`…), HEIC, AVIF, JPEG XL and PSD. Without it these files get no preview and don't open in the viewer |
| `adwaita-icon-theme` | Fallback icons when your icon theme is missing one |
| `qt6-gtk-platformtheme` | The desktop's theme on GTK desktops (GNOME, Cinnamon, MATE, Xfce, Budgie): its colours, accent and fonts. Without it Qt uses its own generic look there, though Kestrel still follows the desktop's light or dark style. KDE Plasma has its own (`plasma-integration`) |
| `7zip` (or `p7zip-full` on older releases) | Creating and extracting 7z and zip (with AES-256 passwords and split volumes), and extracting iso, cab, deb, rpm and other formats |
| `unrar` (in Ubuntu's multiverse section) | Extracting `.rar`, including encrypted and multi-volume archives. 7-Zip on Ubuntu can't decompress RAR |
| `zip`, `unzip` | zip archives without 7-Zip |
| `pigz` | Fast multi-core `.gz` / `.tar.gz` (falls back to `gzip`) |
| `zpaq` | `.zpaq` journaling archives, with very high compression and passwords |
| `zstd`, `xz-utils`, `bzip2`, `lzip` | `.tar.zst`, `.tar.xz`, `.tar.bz2`, `.tar.lz` and single-file `.zst`/`.xz`/`.bz2`/`.lz` |
| A terminal (`ptyxis`, `gnome-terminal`, `kgx`, `konsole` or `xfce4-terminal`) | "Open in Terminal" (not installed by `install.sh`; any one of these works) |

**Optional** (features are hidden or fall back when missing)

`kes-setup --install-optional` installs the ones apt has that are missing (`zfsutils-linux` only when a ZFS file system is mounted; `rar` needs Ubuntu's multiverse section). UWP isn't an apt package.

| Package | Used for |
|---|---|
| `libimage-exiftool-perl` | The full Metadata tab, plus editing, adding and clearing metadata |
| `ffmpeg` | Video thumbnails and video stills in folder previews (`ffmpeg` and `ffprobe`) |
| `zfsutils-linux` | Showing a ZFS pool as one card with pool-level usage on the Overview page |
| `pkexec` | The **admin session**: retrying operations that fail with "permission denied" as administrator (usually already installed) |
| `rar` (non-free, multiverse; a trial version of WinRAR's command-line tool) | Creating `.rar` archives (passwords, volumes, recovery records) |
| `bleachbit` | **Shred with BleachBit…** and **Empty Trash with BleachBit…**, and the cleaner `kes-setup --bleachbit` adds to it |
| `pbzip2` or `lbzip2`, `plzip`, `lz4` | Multi-core `.bz2` and `.lz`, and `.lz4` archives. Kestrel offers whichever are installed |
| `ripgrep` | Searching inside files by reading them (search bar → *File contents*), much faster than `grep`, which is used without it |
| [UWP](https://github.com/RegulusArms/UWP) (not an apt package; install with its `install.sh`) | "Set as Wallpaper" through UWP profiles, and "Add to Selected UWP Monitor". Found as `uwp` on your PATH or in `~/.local/bin` |
