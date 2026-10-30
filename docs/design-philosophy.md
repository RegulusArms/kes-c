# Design philosophy

← Back to [the README](../README.md)

Kestrel doesn't make its own version of things Linux already does well. When a feature needs real work done (compressing, encrypting, syncing, converting video, reading metadata), the first question is: **which command-line tool do Linux users already trust for this, the one that's already the default?** Kestrel then wraps that tool in a file manager's interface. The tool does the work; Kestrel makes it easy to reach, safe to use and clear when something goes wrong.

## Why

- **Trust.** Tools like 7-Zip, GnuPG, rsync, FFmpeg and ExifTool have had decades of use on millions of machines. Their edge cases have been found and fixed. A new implementation inside a file manager would start from zero.
- **No lock-in.** What Kestrel makes is exactly what the tool makes. An archive, a signature check or a converted video can be opened, checked or redone from a terminal, or with any other program that uses the same tool.
- **Updates for free.** When your distribution updates a tool, Kestrel gets its fixes, speed-ups and new formats without a Kestrel update.
- **A smaller program.** Kestrel's own code goes into what only a file manager can do: the interface, keeping your files safe, and fitting into the desktop. It doesn't go into codecs, cryptography or sync algorithms.

## Tools in Kestrel today

| Feature | Tool |
|---|---|
| Extracting and compressing archives | `7z`, `unrar` / `rar`, `zpaq`, `tar` with `gzip`/`pigz`, `xz`, `zstd`, `bzip2`, `lzip`, `lz4`… |
| Passwords on tar archives, **Verify Signature** | `gpg` |
| **Sync To…** | `rsync` |
| **Convert Video…**, **Extract Audio…**, video thumbnails | `ffmpeg`, `ffprobe` |
| **Convert Image…** to HEIC and AVIF | `heif-enc` |
| The full Metadata tab and editing metadata | `exiftool` |
| Lossless **Rotate** and **Straighten** of JPEGs | `jpegtran` |
| Searching inside files | `localsearch` (the desktop's index), else `rg` (ripgrep), else `grep` |
| Writing files onto a tape with dd | `dd` |
| **Network Sharing…**, **Send To** | `net` (Samba), `xdg-email`, `bluetooth-sendto` |
| ZFS pools on the Overview | `zfs`, `zpool` |
| Thumbnails of PDFs, fonts and other files | The system's thumbnailers (as GNOME Files uses) |
| **Shred with BleachBit…** | `bleachbit` |
| The admin session | `pkexec` |
| Drives, trash, network locations, default apps | GIO, gvfs, udisks |
| Remembered archive passwords | The desktop's keyring (libsecret) |

[Dependencies](dependencies.md) lists the packages and what each one unlocks.

## Choosing a tool

1. **The default one.** Kestrel picks the tool that's in Ubuntu's and Linux Mint's repositories and that most guides and scripts assume, not a niche alternative. Among rivals, the more widely installed one wins.
2. **It must run on the oldest supported system.** That's Ubuntu 24.04 (and Linux Mint 22). Abilities only a newer version has are used when they're there, and the older behaviour stays.
3. **Faster drop-in replacements when they're installed.** If a faster tool gives the same result, Kestrel uses it when it's there and falls back otherwise: `pigz` over `gzip`, `pbzip2`/`lbzip2` over `bzip2`, `rg` over `grep`.
4. **The tool's real abilities, shown.** A wrapper offers what the tool can actually do: 7-Zip's AES-256 and split volumes, RAR's recovery records, FFmpeg's codecs as the installed build lists them (`ffmpeg -encoders`). Kestrel doesn't add half-features on top. If the tool can't do something, Kestrel doesn't claim to.

## When a tool is missing

Only Qt, GLib and a few basic libraries are required. Every wrapped tool is optional, so Kestrel stays small and gains features as you install tools.

- A feature whose tool is missing is hidden, greyed out with the package to install, or falls back to a simpler way that's still correct. For example, without `exiftool` the Metadata tab shows what Kestrel reads itself, and without `rsync` Sync To… uses Kestrel's own copy.
- Kestrel never fakes a result it can't produce.
- `kes-setup --install-optional` installs the optional tools apt has.
- Kestrel uses the system's copy of a tool, even when another environment (such as Anaconda) puts its own copy first on the PATH. Those copies often don't match the desktop.

## When a tool changes

Kestrel depends on programs it doesn't control. A tool update can change its output, rename an option or drop a feature. Kestrel tries to keep working through such changes, and when it can't, to fail clearly rather than quietly.

- **Machine-readable output.** Kestrel asks for the forms meant for scripts, which tool authors try to keep stable, not the text meant for people. Examples: `7z l -slt`, ExifTool's JSON, `gpg --batch`, FFmpeg's `-progress`.
- **A fixed language.** Tools whose output Kestrel reads run with `LC_ALL=C.UTF-8`, so a translated message can't break the reading.
- **Exit codes first.** What a tool returns decides whether something worked. Its messages are used to explain a failure, not to detect one. An example is tar's exit code 1, which means "finished with warnings".
- **Abilities rather than version numbers.** Where versions of a tool differ in what they can do, Kestrel checks for the ability (as with FFmpeg's encoders) instead of comparing version numbers. The old `p7zip` and the newer `7zip` packages are an example: the same command, different abilities. Today most checks only ask whether a tool is installed; this principle is where they should go.
- **Fail loudly.** If a tool's output stops making sense, Kestrel reports an error with the tool's own message. It doesn't guess or carry on with half-read results.
- **Your files are safe even if the tool breaks.** Whatever a tool writes goes into a hidden part file that's renamed into place only when it's complete. A tool that crashes or misbehaves can't damage what was there. Where that isn't possible (a tool writing its own outputs), Kestrel keeps each piece whole and loses nothing.
- **Replacing a tool.** When a distribution drops a tool or a new default takes over, Kestrel moves to the new one and keeps the old one as a fallback while it's still common. The change goes in the changelog.

## Running tools safely

- **No shell.** Arguments are passed to the tool as a list, so file names with spaces, quotes or `$` are never interpreted.
- **Paths after `--`.** A path goes after `--` where the tool supports it, so a file named `-rf` isn't taken for an option.
- **Passwords never on the command line.** Archive passwords are sent through a pipe, not as arguments, which every user on the machine could see with `ps`. The exception is `zpaq`, which has no other way. Remembered passwords are kept in the desktop's keyring.
- **Respecting your setup.** Kestrel uses your existing setup and doesn't keep private copies of it: your GnuPG keyring, your desktop's keyring, your SSH configuration and your default apps.

## Where Kestrel does its own work

Kestrel writes its own code only where a tool can't do the job well inside a file manager:

- **Speed while browsing.** Thumbnails and list columns need a photo's date, size and orientation for thousands of files at once. Running ExifTool per file would be far too slow, so Kestrel reads basic EXIF and PNG data itself and uses ExifTool for the full Metadata tab and for editing.
- **Safety a tool can't give.** Copying and moving are Kestrel's own, because Kestrel's copy can be failure-atomic: each file is written under a part name and renamed into place when it's whole, with progress, a cancel that loses nothing, and the job queue. `rsync` is used where its strengths matter (Sync To…).
- **Fitting into the desktop.** Where the standard is a library rather than a command, Kestrel uses the library: GIO for the trash, mounting and default apps, libsecret for the keyring, udisks for drives.

## What the wrapper adds

A tool does the work; Kestrel adds what a file manager should:

- **Discoverability.** Right-click entries, dialogs with sensible defaults, and the options the tool offers, without needing to know its flags.
- **Progress and control.** Jobs in the queue with progress and time left, which can be cancelled cleanly.
- **Safety.** Part files and atomic renames, never replacing something you didn't ask to replace, passwords kept out of sight.
- **Clear errors.** A failure is reported in words a person can act on, with the tool's own message alongside.
- **Transparency.** Where it's useful, Kestrel shows the command it will run (Convert Video… does), so you can see what's happening and run it yourself.

## Testing against the real tools

Kestrel's code can be correct and still break because a tool under it changed. Kestrel's source didn't change, so reviewing it can't catch that; only running the real tool and checking the result can. That's why Kestrel's tests run the actual tools rather than imitations of them:

- **Silent breakage.** A changed output format usually doesn't crash anything. It gives a wrong answer, such as an encrypted archive read as unencrypted or a file missing from a listing. The tests check the answers.
- **Before a release, not after.** CI installs the main tools in clean Ubuntu 24.04 and 26.04 environments and runs the test suite on both. A failing test stops the package from being built, so a breaking change blocks a release instead of reaching users.
- **The safety promises.** Tests make jobs fail part-way, by cancelling them or limiting file size, and check that the original is intact and nothing half-made is left behind.
- **Both ways.** Features with a fallback are tested with the tool and without it (for example, content search with ripgrep and with grep).
- **Changing tools safely.** When a default changes, the same tests run against the new tool.

A test whose tool isn't installed is skipped, not failed. That's right on a developer's machine, but it means the main test run only covers the tools CI installs. A separate **tool check** is planned to cover the rest:

- It will install every tool Kestrel can use, run each one through Kestrel's own code, and report each tool as working, broken or missing, with its version and, for a broken one, the error.
- It will test both 7-Zips (`7zip` and `p7zip-full`), on Ubuntu 24.04 and 26.04, so a newer tool that behaves differently shows up before users find it.
- It will run on its own, also on a schedule, since tools change when the distribution updates them, not when Kestrel does.
- It won't hold up or fail the build, the package or a release. It's an early warning, not a gate.

## Proposing a new feature

Before writing a feature, go through these:

1. Is there a standard tool that already does this? If so, wrap it.
2. Is it in Ubuntu 24.04's repositories? If not, what do users of the oldest supported system get?
3. Does it have a machine-readable mode or stable output to read?
4. What happens when it's missing: hidden, greyed out with the package to install, or a correct fallback?
5. Is what it writes failure-atomic (a part file renamed into place), and does a cancel lose nothing?
6. Are passwords and other secrets kept off the command line?
7. Is it listed in [Dependencies](dependencies.md) and in `kes-setup --install-optional`?
8. Is there a test that runs the real tool, and that skips cleanly without it?
