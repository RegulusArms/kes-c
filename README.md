# Kestrel Explorer (C++)

[![Release build](https://img.shields.io/github/actions/workflow/status/RegulusArms/kes-c/deb.yml?branch=main&label=release%20build)](https://github.com/RegulusArms/kes-c/actions/workflows/deb.yml?query=branch%3Amain)
[![Dev build](https://img.shields.io/github/actions/workflow/status/RegulusArms/kes-c/deb.yml?branch=DEV&label=dev%20build)](https://github.com/RegulusArms/kes-c/actions/workflows/deb.yml?query=branch%3ADEV)
[![Release](https://img.shields.io/github/v/release/RegulusArms/kes-c?include_prereleases&sort=semver)](https://github.com/RegulusArms/kes-c/releases)
[![apt repository](https://img.shields.io/badge/apt-regulusarms.github.io%2Fkes--c-blue?logo=debian&logoColor=white)](https://regulusarms.github.io/kes-c/)
[![Runs on](https://img.shields.io/badge/runs%20on-Ubuntu%2024.04%20%7C%2026.04%20%C2%B7%20Kubuntu%2026.04%20%C2%B7%20Mint%2022-E95420?logo=ubuntu&logoColor=white)](#supported-systems)
[![C++ / Qt 6](https://img.shields.io/badge/C%2B%2B%20%2F%20Qt-6-41CD52?logo=qt&logoColor=white)](docs/development.md)
[![Languages](https://img.shields.io/badge/languages-11-8A2BE2)](docs/translations.md)
[![License: MIT](https://img.shields.io/github/license/RegulusArms/kes-c)](LICENSE)

**Version 0.3.3-alpha.0.** This is an early alpha release, so expect rough edges. 

> **Why do Linux users still need five separate utilities and a terminal to do normal filesystem work?**

That's the question Kestrel sets out to answer, and why I'm building it.

A file manager for Ubuntu and Linux Mint that's easy to pick up and puts many jobs you'd normally do in a terminal into the window. It also adds quality-of-life improvements over GNOME Files. It's written in C++ with Qt 6.

![Kestrel Explorer's Overview page: drives with usage bars, phones and cameras, network locations and bookmarks](docs/screenshot-overview.png)

**Terminal jobs, in the window:**
- **Archives:** create and extract 7z, zip, rar, zpaq and every tar format, with the options the command-line tools offer (level, method, threads, passwords, split volumes), and a preview of the exact command that will run. Double-click an archive to extract it.
- **Admin rights only when needed:** when an everyday file operation fails with "permission denied", choose **Retry as Administrator** and enter your password once per session, instead of running a file manager as root. It's for simple copies, moves, renames and deletes, not a replacement for the terminal for system administration.
- **Permissions and links:** permissions, including setuid, setgid and the sticky bit; symbolic, relative and hard links; and MD5/SHA checksums with verification. Double-click a checksum file (`.sfv`, `.md5`, `.sha256`, `SHA256SUMS`…) to check every file it lists. Right-click files or folders and choose **Create Checksum File…** to make one.
- **Metadata:** view and edit EXIF, XMP and other metadata with `exiftool`, from a searchable list of tags.
- **Search and rename:** search subfolders with wildcards or inside files, and batch rename with templates or regular expressions.
- **Shortcuts out:** Open in Terminal or VS Code, your Nautilus scripts, and Samba network sharing.
- **Secure deletion:** with [BleachBit](https://www.bleachbit.org) installed, shred files and folders or empty the trash so they can't be recovered, from the right-click menu; `kes-setup --bleachbit` also adds a cleaner for Kestrel's own cache and history to BleachBit.

**Compared with GNOME Files, Nemo and Dolphin:** what Kestrel adds over each, what it doesn't have yet, and how it installs alongside them without changing them: [Kestrel compared with other file managers](docs/comparison.md).

## Install

**With apt** (amd64), from the releases channel. Each Ubuntu release has its own package, so use your system's
commands (`grep UBUNTU_CODENAME /etc/os-release` prints `resolute` on Ubuntu 26.04 and Kubuntu 26.04, `noble` on 24.04 and Linux Mint 22).

**Ubuntu 26.04 and Kubuntu 26.04** (and the other 26.04 flavours):

```bash
sudo install -d -m 755 /etc/apt/keyrings
sudo curl -fsSLo /etc/apt/keyrings/kestrel.asc https://regulusarms.github.io/kes-c/kestrel.asc
printf 'Types: deb\nURIs: https://regulusarms.github.io/kes-c\nSuites: resolute\nComponents: main\nSigned-By: /etc/apt/keyrings/kestrel.asc\n' \
  | sudo tee /etc/apt/sources.list.d/kestrel.sources
sudo apt update && sudo apt install kestrel-explorer
kes-setup --default   # optional: make it your file manager (kes-setup --undo puts things back)
```

On Kubuntu, `kes-setup --default` makes Kestrel open folders in place of Dolphin (and, if Dolphin is pinned in the taskbar,
asks whether to put Kestrel in its place); Dolphin stays installed and keeps working, and `kes-setup --undo` gives folders back to it. See
[Kubuntu 26.04](docs/install.md#kubuntu-2604-kde-plasma) in the install guide.

**Ubuntu 24.04 and Linux Mint 22:**

```bash
sudo install -d -m 755 /etc/apt/keyrings
sudo curl -fsSLo /etc/apt/keyrings/kestrel.asc https://regulusarms.github.io/kes-c/kestrel.asc
printf 'Types: deb\nURIs: https://regulusarms.github.io/kes-c\nSuites: noble\nComponents: main\nSigned-By: /etc/apt/keyrings/kestrel.asc\n' \
  | sudo tee /etc/apt/sources.list.d/kestrel.sources
sudo apt update && sudo apt install kestrel-explorer
kes-setup --default   # optional: make it your file manager (kes-setup --undo puts things back)
```

**From source:**

```bash
./install.sh             # installs build tools if needed, builds, adds `kes` to ~/.local/bin and the app grid
./install.sh --default   # ...and makes it your file manager
```

The [install guide](docs/install.md) covers the development-builds channel, switching channels (and from the older
`stable`/`dev` suites), which .deb to download by hand, what `--default` and `--dock` change, building by hand,
BleachBit and uninstalling.

## Supported systems

- **Ubuntu 26.04 (GNOME):** the main platform; everything is tested here.
- **Linux Mint 22 (Cinnamon):** supported since 0.2.0. Kestrel builds and runs there, and the [benchmark against Nemo](docs/performance.md#performance-kestrel-vs-nemo) was run there.
- **Ubuntu 24.04 (GNOME):** tested; Kestrel installs and runs there (from the apt repository too). It has the same base as Linux Mint 22.
- **Kubuntu 26.04 (KDE Plasma):** supported since 0.3.2-alpha.33; Kestrel follows Plasma's look and settings (see the [install guide](docs/install.md#kubuntu-2604-kde-plasma)). Some parts are still to be tried in a full Plasma session: passwords in KWallet, network shares and phones, and drop focus on Wayland.

## Documentation

| Page | What's in it |
|---|---|
| [Install guide](docs/install.md) | apt channels, building from source, making Kestrel the default file manager, the file chooser, BleachBit, Linux Mint, Ubuntu 24.04 and Kubuntu notes |
| [Dependencies](docs/dependencies.md) | The packages needed to build, the recommended ones and the optional ones, and what each is used for |
| [Compared with other file managers](docs/comparison.md) | What Kestrel adds over GNOME Files, Nemo and Dolphin, what it doesn't have yet, and how it installs alongside them |
| [Features](docs/features.md) | File management, galleries and folder previews, the Overview page, phones and cameras, the image viewer, checksum files |
| [Right-click menus](docs/context-menus.md) | Every right-click menu, what each entry does and what it works on |
| [Archives](docs/archives.md) | Extracting and compressing, formats, progress and cancelling, passwords and security, known limitations |
| [Translations](docs/translations.md) | The languages Kestrel is shown in, and how to report a wrong translation |
| [Admin session](docs/admin.md) | Retry as Administrator: how it works, what it covers, and how the root helper is kept safe |
| [Performance](docs/performance.md) | Benchmarks against GNOME Files, Nemo, Dolphin, Thunar, Krusader and PCManFM |
| [Bugs found elsewhere](docs/bugs-elsewhere.md) | Problems in the desktop and other programs that get in Kestrel's way, how they were found, and how to work around them |
| [Design philosophy](docs/design-philosophy.md) | Why Kestrel wraps the standard command-line tools instead of rebuilding them, how it copes when a tool changes, and how it's tested |
| [Development](docs/development.md) | Architecture, the source layout, tests and performance counters |
| [Tests](tests/README.md) | Running the test suite, what each test checks, CI, and adding a test ([test changelog](tests/CHANGELOG.md)) |
| [Changelog](CHANGELOG.md) | What changed in each version |
| [To do](TODO.md) | Planned additions, fixes and changes |

## License

MIT. See [LICENSE](LICENSE).
