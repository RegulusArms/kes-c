# Admin session

[← Back to the README](../README.md)

Running a whole file manager as root (`sudo kes`) is risky and mostly doesn't work on a modern desktop. Kestrel instead keeps running as you, and does only the operations that need admin rights as root.

## What it's for

The admin session is for **everyday file operations that happen to need admin rights**: copying a file into `/opt`, deleting something root left in your Downloads, renaming a file in a system folder. It does the same simple operations Kestrel does for you anywhere else, one deliberate action at a time.

It is **not a tool for administering your system,** and isn't meant to replace the terminal. Kestrel is a file manager for everyday use, not a hardened tool for root work, so it deliberately leaves out operations where one mistake as root could break the system or open a security hole. For that kind of work, use:

- **the terminal** (`sudo`), where you see exactly what runs, for changes to system files, bulk changes and anything you'd want to review first;
- **your package manager** (`apt`, the Software app) to install or remove software, rather than copying or extracting it into system folders;
- **your desktop's settings tools** for users, permissions of devices, services and the like.

Requests to extend the admin session into those areas are out of scope and won't be added.

## How it works

1. An operation fails with "permission denied". Kestrel lists what failed and offers **Retry as Administrator**.
2. The first time, the normal system password prompt appears. It says something like *"Authentication is needed to run `/usr/bin/kes-admin-helper` as the super user"* (`…/build/kes-admin-helper` when built from source): that's Kestrel's small helper (`src/admin_helper.cpp`), started through `pkexec`.
3. The helper keeps running as root in the background, and the status bar shows 🛡 **Admin**. From then on, operations that need admin rights are handed to it straight away, with no password and no extra question. Progress and ✕ Cancel work as usual.
4. The session ends when you click 🛡 Admin → **End Admin Session**, after 15 minutes without admin actions, or when Kestrel quits. The next admin operation asks for your password again.

☰ → **Start Admin Session…** opens a session ahead of time, for example before a batch of work in system folders.

## What it covers

- Copy, move, paste, drag and drop, duplicate, Move To / Copy To
- Delete permanently and empty trash (including files owned by root in the trash)
- Rename (F2 and the Properties name field)
- New folder, new empty file
- Symbolic links, hard links and `.desktop` shortcuts
- Changing permissions in Properties

## What it doesn't do, on purpose

These fail with "permission denied" as usual, with no **Retry as Administrator**. They're left out by design, not waiting to be added:

- **Batch rename.** One pattern applied to many files at once is easy to get wrong, and in a system folder a wrong rename can stop the computer from starting, with no easy way back. Rename system files one at a time (F2), or in the terminal.
- **Extracting archives into protected folders.** An archive's contents are whatever its author put in it. Unpacked into a system folder, they could replace programs or settings, and files left owned by you there would let anything running as you change them later. Extract into your own folders; install software with your package manager.
- **Editing metadata** of files you don't own.
- **Move to Trash.** Your trash is yours: an item that can't be trashed can be deleted permanently instead, and that can use the session.
- **Opening files or running programs as root,** and editing files' contents. Kestrel never starts an app, editor or terminal as administrator; use `sudo` (or `sudoedit` for a file) in a terminal.
- **Changing owners** (`chown`) and anything beyond a file's permissions in Properties.

## Safety

- **Only the Kestrel window that started the helper can talk to it.** It reads requests from a private pipe and exits as soon as that pipe closes, including if Kestrel crashes.
- **It only accepts a fixed set of file operations,** on absolute paths. It refuses to delete, replace or change the permissions of `/`, top-level folders (`/usr`, `/etc`, `/home`, `/var`, and any other folder directly under `/`) and home folders themselves (`/home/name`). One table in the helper lists what each operation may do to each of its paths.
- **It can't be redirected by a symlink.** Files can change while root works on them: another user, or a program, could swap a folder for a symlink to `/etc`. So the helper never trusts a path as text. It opens each folder on the way one at a time without following symlinks, then works on the name inside the folder it opened. It follows a symlink on the way only if root controls it (owned by root, in a folder only root can write to), such as `/lib` → `usr/lib`. Kestrel resolves your own symlinked folders first, so those keep working. Recursive copies and deletes go folder by folder the same way, and stop if a folder is swapped while they run.
- **A replacement never leaves you with half of something.** A file, link or folder it copies or moves over another is built under a hidden name (`.kes-….part`) beside it and swapped in only when complete: if the disk fills up, you cancel, or the session ends part-way, the old one is still there as it was and the half-made copy is removed. Cancelling stops even in the middle of one large file, and progress counts the bytes copied. (Kestrel's own copies and moves, without the admin session, work the same way.)
- **It won't hand out root by accident.** A copy it makes is root's, so it drops the set-user-ID bit from someone else's program (a move between drives keeps the owner instead). It only hard-links your own files, and never deletes a mount point (the drive mounted there would be emptied).
- **The trade-off, when built from source:** the helper binary is in the Kestrel build folder, which your account can edit. Anything running as you could change that file before your next admin session. Installed from the .deb, it's `/usr/bin/kes-admin-helper`, which only root can change. That's the same level of trust as typing `sudo` in your own terminal, which is fine on a personal computer.
