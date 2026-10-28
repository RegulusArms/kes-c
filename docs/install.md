# Installing Kestrel Explorer

[← Back to the README](../README.md)

## Install with apt

Kestrel's apt repository (https://regulusarms.github.io/kes-c/, amd64) has a package built for each supported Ubuntu release, and you get updates with the system's others. Use the commands for your system: each release's package is built against its own Qt and libraries.

| Your system | Releases (each merge to `main`) | Development builds (each push to `DEV`) |
|---|---|---|
| Ubuntu 26.04, and its flavours (Kubuntu, Xubuntu…) | `resolute` | `resolute-dev` |
| Ubuntu 24.04, and its flavours; Linux Mint 22 | `noble` | `noble-dev` |

Not sure which you have? `grep UBUNTU_CODENAME /etc/os-release` prints `resolute` or `noble` (Linux Mint prints the Ubuntu release it's based on). Other releases, such as Ubuntu 26.10, have no package yet: [build from source](#build--install) there.

**Ubuntu 26.04:**

```bash
sudo install -d -m 755 /etc/apt/keyrings
sudo curl -fsSLo /etc/apt/keyrings/kestrel.asc https://regulusarms.github.io/kes-c/kestrel.asc
printf 'Types: deb\nURIs: https://regulusarms.github.io/kes-c\nSuites: resolute\nComponents: main\nSigned-By: /etc/apt/keyrings/kestrel.asc\n' \
  | sudo tee /etc/apt/sources.list.d/kestrel.sources
sudo apt update && sudo apt install kestrel-explorer
```

**Ubuntu 24.04 and Linux Mint 22:**

```bash
sudo install -d -m 755 /etc/apt/keyrings
sudo curl -fsSLo /etc/apt/keyrings/kestrel.asc https://regulusarms.github.io/kes-c/kestrel.asc
printf 'Types: deb\nURIs: https://regulusarms.github.io/kes-c\nSuites: noble\nComponents: main\nSigned-By: /etc/apt/keyrings/kestrel.asc\n' \
  | sudo tee /etc/apt/sources.list.d/kestrel.sources
sudo apt update && sudo apt install kestrel-explorer
```

**Development builds** (the newest build of the `DEV` branch, updated on every push that changes the code): the same commands with `Suites: resolute-dev` (Ubuntu 26.04) or `Suites: noble-dev` (Ubuntu 24.04, Linux Mint 22).

Then, optionally, make it your file manager (see below); `kes-setup --undo` puts things back:

```bash
kes-setup --default
kes-setup --bleachbit   # and/or add Kestrel to BleachBit's cleaners (see "Cleaning up with BleachBit" below)
kes-setup --install-optional   # and/or install the optional packages: exiftool, ffmpeg, BleachBit, rar… (see [Dependencies](dependencies.md))
```

`sudo apt upgrade` keeps you on the channel you picked. To switch, change the `Suites:` line in `/etc/apt/sources.list.d/kestrel.sources` (or run the commands again with the other suite) and `sudo apt update`. Going from releases to development builds upgrades at the next `apt upgrade`. Going back doesn't, because a development build has a higher version than the release it came after: you stay on it until the next release, or go back now with `sudo apt install --allow-downgrades kestrel-explorer=<version>` (`apt policy kestrel-explorer` lists the versions).

**Set up before 0.3.2-alpha.25?** Then your `kestrel.sources` probably says `Suites: stable` or `Suites: dev`. Those still work, but they carry the Ubuntu 24.04 package for every system, and stop updating on a release when Kestrel stops supporting it. Switch to your release's suite; for example, on Ubuntu 26.04 from `stable`:

```bash
sudo sed -i 's/^Suites: .*/Suites: resolute/' /etc/apt/sources.list.d/kestrel.sources
sudo apt update && sudo apt upgrade
```

(`resolute-dev` in place of `resolute` if you were on `dev`; `noble` or `noble-dev` on Ubuntu 24.04 and Linux Mint 22.) On 26.04 this replaces the 24.04 package with the 26.04 one at once: it has a higher version.

**When Kestrel stops supporting an Ubuntu release,** that release's suites keep the last version that installs on it and get no more updates, so you keep a working Kestrel; the [changelog](../CHANGELOG.md) and the [repository's page](https://regulusarms.github.io/kes-c/) say so. Upgrading the system to a newer Ubuntu release? Change `Suites:` to the new release's afterwards: its package sorts higher, so `apt upgrade` moves you to it.

**Downloading a .deb by hand:** each [GitHub release](https://github.com/RegulusArms/kes-c/releases) (and the `dev-latest` pre-release) has one .deb for each Ubuntu release. Take the one whose name ends in yours:

| Your system | File |
|---|---|
| Ubuntu 26.04 | `kestrel-explorer_<version>~ubuntu26.04_amd64.deb` |
| Ubuntu 24.04, Linux Mint 22 | `kestrel-explorer_<version>~ubuntu24.04_amd64.deb` |

Install it with `sudo apt install ./kestrel-explorer_…_amd64.deb`. The 26.04 one won't install on 24.04 (it needs 26.04's libraries), but the 24.04 one does install on 26.04, so on 26.04 make sure you take the `ubuntu26.04` one. A .deb installed by hand doesn't update itself; the apt repository does.

The packages are built by GitHub Actions (`.github/workflows/deb.yml`). Remove Kestrel with `sudo apt remove kestrel-explorer` (run `kes-setup --undo` first if you used `--default`).

## Build / install

```bash
./install.sh             # installs build tools if needed, builds, adds `kes` to ~/.local/bin and the app grid
./install.sh --default   # ...and makes it the default app for opening folders and the trash, and puts kes on your PATH
./install.sh --dock      # ...and puts it in the dock in place of GNOME Files
./install.sh --install-recommended   # ...and installs the recommended packages (RAW/HEIC previews etc.)
./install.sh --uninstall

./build/kes ~/Pictures   # or run the build in place without installing
./build/kes --version    # print the version
kes ~/Pictures           # once installed
```

`--default` also makes Kestrel answer "Show in folder" / "Open containing folder" from browsers and other apps. Those don't use the default folder app: they call the `org.freedesktop.FileManager1` D-Bus service, which GNOME Files normally provides. The installer adds a per-user D-Bus activation file so Kestrel provides it instead, and closes GNOME Files' background service so it lets go. If you open GNOME Files later while no Kestrel window is open, it takes the service back until it quits.

If `~/.local/bin` isn't on your PATH yet (on Ubuntu and Mint it's only added at login, and only if the folder already existed), `--default` adds it for new terminals: a marked block at the end of `~/.bashrc` (and `~/.zshrc`, or a file in `~/.config/fish/conf.d/`, if you use those shells). `--uninstall` takes the block out again and leaves the rest of the file alone.

It also makes Kestrel the system's **file chooser**: the Open and Save dialogs that apps get through the desktop portal (xdg-desktop-portal), such as a browser's "Save image as", Flatpak and Snap apps, and GTK 4 and Qt apps that use the portal. Those dialogs open as a Kestrel window with a bar at the bottom for the file name, the file type, Cancel and Save/Open, so you browse with the sidebar, previews and search as usual. A suggested name that's already taken in the folder becomes the next free one (`photo (1).jpg`), saving over a file you chose asks first, and the next dialog starts where the last one picked something (or, if you choose so in Preferences → Windows, on an Overview of the folders you keep for saving; see [Features](features.md)). The portal chooses its file chooser per desktop, not per app, so this applies to every app that uses it. The installer adds a D-Bus activation file for `kes --file-chooser`, installs the portal definition `/usr/share/xdg-desktop-portal/portals/kestrel.portal` (the portal reads these only from there, so this one file asks for your password), writes `~/.config/xdg-desktop-portal/<desktop>-portals.conf` with your desktop's current choices plus Kestrel for the file chooser (GNOME's stays as the fallback; a portals.conf you already had is kept and changed), and restarts the portal. `--uninstall` puts it all back.

It also installs a small GNOME Shell extension, **Kestrel drop focus**, so that when you drag files from Kestrel into another app (a browser, an editor, a chat window) that app gets the focus, as it would on Windows. On GNOME on Wayland an app can't focus another app's window, so the extension does it when Kestrel asks; GNOME loads a newly installed extension at your next log-in, so log out and back in once. On X11 (Linux Mint, GNOME on Xorg) Kestrel needs no extension. The extension is copied to `~/.local/share/gnome-shell/extensions/` (the .deb installs it for everyone) and added to your enabled extensions; `--uninstall` takes it out again and leaves your other extensions alone.

It also makes Kestrel the app for `trash:///` (the dock's Trash icon, `gio open trash:///`), remembering which app had it so `--uninstall` can put it back. If GNOME Files is pinned in the dock, `--default` asks whether to put Kestrel in its place; `--dock` does that without asking. `--uninstall` puts GNOME Files back if the installer swapped it.

The installer does all of this (everything but the PATH) by running `kes-setup`, which you can also run on its own: `kes-setup --default`, `kes-setup --dock`, and `kes-setup --undo` to put back what they changed. It sets things up for the `kes` next to it, or the one given with `--kes PATH`.

**Cleaning up with BleachBit:** `kes-setup --bleachbit` adds Kestrel Explorer to [BleachBit](https://www.bleachbit.org) (as `~/.config/bleachbit/cleaners/kestrel-explorer.xml`), with six options:
- **Cache:** folder preview mosaics, the video clips played in the file view, copies of phone videos made to play them, and exiftool's tag lists.
- **History:** the servers you connected to recently, and the folder the last Open or Save dialog was left in. These are taken out of Kestrel's settings, and every other setting stays.
- **Starred items**, **Pinned items**, **Save locations** and **Folder covers, colours and sorts:** the lists of starred files and folders, of pinned ones, of the folders kept for other apps' Save windows, and of folders given a cover, a colour, no image previews or a sort of their own. You chose these, so BleachBit warns before cleaning them.

The thumbnails Kestrel shares with GNOME Files and the desktop's Recent files are cleaned by BleachBit's own Thumbnails and Recent documents options, and undo history is never written to disk. BleachBit won't clean while Kestrel is open. `kes-setup --undo` removes the cleaner again.

To build by hand:

```bash
sudo apt install g++ cmake pkg-config qt6-base-dev libglib2.0-dev libxcb1-dev libsecret-1-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

This produces two programs in `build/`: `kes` (the app) and `kes-admin-helper` (the small root helper for the [admin session](admin.md)). They must stay in the same folder.

`install.sh` links `~/.local/bin/kes` to `build/kes` and writes `~/.local/share/applications/kestrel-explorer.desktop`. These are the same names the Python version uses, so installing this version replaces the Python version's `kes` command and app-grid entry. Run the Python version's `install.sh` to switch back. If the .deb is installed too, this per-user entry is the one the app grid and dock use instead of the package's; `./install.sh --uninstall` removes it, and the package's entry takes over again.

After pulling code changes, run `./install.sh` again (or just `cmake --build build`) to rebuild. Your settings, bookmarks and thumbnail cache are kept (`~/.config/kestrel-explorer`, `~/.config/gtk-3.0/bookmarks`, `~/.cache`).

## Linux Mint 22 and Ubuntu 24.04

**What's different on Linux Mint 22 and Ubuntu 24.04.** They're based on an older Ubuntu, with Qt 6.4, the oldest Qt Kestrel works with:
- `kimageformat6-plugins` doesn't exist there, so the installer skips it and RAW and HEIC files get no preview.
- SVG support comes in `libqt6svg6` instead of `qt6-svg-plugins`. The installer picks whichever the system has.
- Qt 6.4 takes no colours from the GTK theme, so Kestrel reads them through GTK itself (with the system's `python3`, `python3-gi` and `gir1.2-gtk-3.0`, which Mint has) and follows theme changes while it runs.
- On Cinnamon, Kestrel uses Cinnamon's own settings for the wallpaper, file history and icon theme. `--default` remembers Nemo and puts it back on `--uninstall`, and the file chooser opens as the app's dialog (X11). `--dock` only works with GNOME's dock for now.
