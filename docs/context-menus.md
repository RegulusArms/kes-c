# Right-click menus

[← Back to the README](../README.md)

Every right-click menu in Kestrel, what each entry does, and what it works on. An entry only shows when it applies
(the **Shows** column); otherwise it's left out of the menu.

The first six menus below can be rearranged in **Preferences → Menus**: untick an entry to hide it, drag entries to
reorder them or move them into or out of a submenu, and add separators and submenus of your own (see
[Features](features.md#preferences)). The order here is the default one.

- [A file](#a-file)
- [A folder](#a-folder)
- [An archive](#an-archive)
- [Several items](#several-items)
- [Empty space in a folder](#empty-space-in-a-folder)
- [The image viewer](#the-image-viewer)
- [Items in the Trash](#items-in-the-trash)
- [Empty space in Starred or Recent](#empty-space-in-starred-or-recent)
- [A tab](#a-tab)
- [The sidebar](#the-sidebar)
- [Overview cards](#overview-cards)
- [Open as Archive window](#open-as-archive-window)
- [Properties → Metadata](#properties--metadata)
- [Dropping with Alt held](#dropping-with-alt-held)

## Shared entries

These entries are on the file, folder, archive and several-items menus. The sections below list only what differs.

| Entry | What it does | Shows |
|---|---|---|
| **Open** | Opens the item(s): folders in this tab, files in their default app (or the app chosen in Preferences for images and videos) | Always |
| **Show in Folder** | Goes to the folder the item is in and selects it | In search results |
| **Collapse This Group** | Collapses the date group the item is in | When the folder is grouped by date |
| **Cut** / **Copy** | Puts the item(s) on the clipboard to paste elsewhere (also into GNOME Files and other apps) | Always |
| **Move To…** / **Copy To…** | Asks for a folder and moves or copies the item(s) there | Always |
| **Duplicate** | Makes a copy beside each item, under the next free name | Always |
| **New Folder with Selection…** | Makes a folder beside the item(s), named as you choose, and moves them into it | Always |
| **Rename…** | Renames the item; with several selected it's **Rename N Items…**, a batch rename | Always |
| **Copy Path / Name ▸** | **Copy Full Path**, **Copy Name** or **Copy URI** (`file://…`) as text, one per line for several | Always |
| **Star** / **Unstar** | Adds to or removes from Starred in the sidebar (★ on the icon) | Always |
| **Pin** / **Unpin** | Pins the item in its own folder (a red pin); with Pinned & Starred First it sorts to the top | Always |
| **Links & Shortcuts ▸** | See below | Always |
| **Compress to “name.ext”** | Compresses straight away, in the format you used last, named after the item | Once you've compressed something |
| **Compress…** | Opens the compress window: format, name, password, level (see [Archives](archives.md)) | Always |
| **Create Checksum File…** | Writes `md5sum` / `sha256sum`-style checksum files for the files (inside folders too) | Always |
| **Scripts ▸** | Runs one of your scripts on the selection (Nautilus-style, from `~/.local/share/nautilus/scripts`); **Open Scripts Folder** at the end | When you have scripts |
| **Move to Trash** | Moves the item(s) to the trash (Ctrl+Z puts them back) | Always |
| **Delete Permanently…** | Asks, then deletes without the trash | Always |
| **Shred with BleachBit…** | Asks, then overwrites and deletes the item(s) so they can't be recovered | With BleachBit installed |
| **Properties** | Opens the Properties window (size, permissions, metadata, checksums…) | Always |

**Links & Shortcuts ▸**

| Entry | What it does | Shows |
|---|---|---|
| **Create Symbolic Link Here** | Makes a symbolic link to each item in this folder | In a real folder (not Starred or Recent) |
| **Create Relative Symbolic Link Here** | The same, with a relative target, so the link keeps working if both are moved together | In a real folder |
| **Create Hard Link Here** | Makes a hard link to each file in this folder | Files only (not folders or links), in a real folder |
| **Create Symbolic Link In…** | Asks for a folder and makes the links there | Always |
| **Send Link to Desktop** | Makes a symbolic link on the Desktop | Always |
| **Create Desktop Shortcut (.desktop) Here** | Makes a `.desktop` launcher that opens the item | In a real folder |
| **Send Shortcut (.desktop) to Desktop** | The same, on the Desktop | Always |
| **Open Link Target Location** | Goes to the folder of what the link points to and selects it | When a symbolic link is selected |
| **Add to Bookmarks** | Adds the folder to the sidebar's bookmarks | Folders only |

## A file

The shared entries, plus:

| Entry | What it does | Shows |
|---|---|---|
| **View Image** | Opens the image in Kestrel's own viewer, whatever app images open with | Images |
| **Open With ▸** | The apps that can open it (up to 8), and **Other Application…** to pick any | Always |
| **Verify Signature** | Checks the signature with `gpg` against the file it signs (the same name without the extension; a clearsigned file against itself) and says plainly what it means: valid and by whom (and whether that's a key you've verified), a bad signature, or the signer's key missing (with **Fetch Key…**, asking first). A signed checksum file's files are checked next | Signatures (`.sig`, `.asc`, `.gpg`, `.sign`), not encrypted archives |
| **Add to Selected UWP Monitor** | Puts the image or video on the monitors selected in UWP's editor | Images and videos, while UWP's editor is open |
| **Set as Wallpaper (UWP)** | Makes the video the wallpaper on every monitor, through UWP | Videos, with UWP installed |
| **Image ▸** | **Set as Wallpaper** (UWP's when installed, otherwise the desktop background), **Use as Folder Cover** (this image on its folder's icon), **Copy Image to Clipboard** (the picture itself, to paste into other apps) | Images |
| **Wipe Metadata…** | Takes out all of the image's metadata (camera details, GPS location, dates, comments, image-generation prompts), the pixels untouched: by default as a copy beside it (`photo (no metadata).jpg`; Undo trashes the copy), or from the original with no backup (asks first), as set in Preferences → Images & Videos. Greyed out as **Wipe Metadata (install exiftool)** without exiftool | Images (JPEG, PNG, WebP, HEIC, AVIF, JPEG XL, GIF, TIFF; not camera raw files) |
| **Send To ▸** | **Email…** (attaches it in your mail app) or **Bluetooth Device…** | With `xdg-email` or `bluetooth-sendto` installed |

## A folder

The shared entries, plus:

| Entry | What it does | Shows |
|---|---|---|
| **Open in New Tab** / **Open in New Window** | Opens the folder in a new tab or window | Always |
| **Open in (code editor)** | Opens the folder in VS Code, VSCodium, Cursor, Zed or Sublime Text | Each one that's installed |
| **Open With ▸** | Other apps that open folders (not file managers), and **Other Application…** | Always |
| **Paste Into Folder** | Pastes the clipboard's files into this folder | Enabled when files are on the clipboard |
| **Find Duplicates…** | Finds the files with the same contents in the folder (and its subfolders), to keep some and move the others to the trash or replace them with hard links (see [Features](features.md)) | Always |
| **Compare With…** | Asks for another folder and lists what differs between the two: only on one side, newer on one side, other contents; copy across or trash from the list | Always |
| **Sync To…** | Makes another folder match this one (copying, replacing and, if chosen, deleting), after a preview; with rsync when installed. Pairs can be saved to run again | Always |
| **Folder Colour ▸** | Gives the folder's icon one of 10 colours, or **Default** | Always |
| **Show Image Previews** | Turns the mosaic of images on the folder's icon on or off for this folder | Always |
| **Reset Folder Cover** | Removes the cover image you chose, back to the automatic mosaic | When it has a chosen cover |
| **Network Sharing…** | Shares the folder on the local network (Samba): name, comment, write access, guests | With Samba (`net`) installed |
| **Regenerate Preview** | Rebuilds this folder's mosaic | Always |
| **Previews ▸** | Builds thumbnails and folder previews ahead of time, as a job (see [Previews](#previews)) | Always |
| **Open in Terminal** | Opens a terminal in the folder | Always |

Folders have no Send To.

### Previews

The **Previews ▸** submenu, on a folder and on empty space in one. *Generate* makes only what isn't made yet;
*Regenerate* makes everything again, replacing the cached copies (for previews that look wrong or old). *Folder
previews* are only the folders' mosaics; without *Recursively*, it's this folder and the folders directly in it.

| Entry | What it does |
|---|---|
| **Generate Previews Recursively** | Thumbnails and folder previews for everything below the folder |
| **Generate Folder Previews** | Mosaics for this folder and the folders directly in it |
| **Generate Folder Previews Recursively** | Mosaics for every folder below |
| **Regenerate Previews** | Thumbnails and previews in this folder, made again |
| **Regenerate Previews Recursively** | The same for everything below |
| **Regenerate Folder Previews** | Mosaics for this folder and the folders directly in it, made again |
| **Regenerate Folder Previews Recursively** | Mosaics for every folder below, made again |

## An archive

A single archive (zip, 7z, rar, tar…), or a split set (`x.part1.rar`, `x.part2.rar`…) selected whole. The file
menu's entries, with these after Links & Shortcuts:

| Entry | What it does | Shows |
|---|---|---|
| **Extract Here** | Extracts beside the archive into a new folder named after it (or the archive's own single top folder); asks for a password when needed | Always |
| **Extract To…** | Asks where to extract, with options | Always |
| **Open as Archive** | Opens a window listing what's inside, to open or extract single items (see [below](#open-as-archive-window)) | When its program can list it |
| **Test Archive** | Checks the archive for damage without extracting | Always |
| **Extract (install …)** | Greyed out in place of the above, naming the package that extracts this format | When that program is missing |

See [Archives](archives.md) for formats, passwords and limits.

## Several items

The shared entries, plus:

| Entry | What it does | Shows |
|---|---|---|
| **Open in New Tab** / **Open in New Window** | One new tab (or one window with a tab each) per folder | When all are folders |
| **View Images** | Opens the selected images in the viewer | When images are selected |
| **Extract Here** / **Extract To…** / **Open as Archive** / **Test Archive** | As for one archive | A split set selected whole |
| **Extract All Here** / **Extract All To…** | Extracts each archive, one after another | When all are archives |
| **Test Archives** | Tests each archive | When all are archives |
| **Find Duplicates…** | As for one folder, in all of them together | When all are folders |
| **Wipe Metadata…** | As for one image, for each image selected (other files are left alone), as one job | When images are selected |
| **Folder Colour ▸** / **Show Image Previews** | As for one folder, for all of them | When all are folders |
| **Send To ▸** | As for a file, with all of them | No folders selected, with an app to send to |

## Empty space in a folder

| Entry | What it does | Shows |
|---|---|---|
| **New Folder…** | Makes a folder here, named as you choose | Always |
| **New Document ▸** | **Empty File…**, and a new file from each template in your Templates folder | Always |
| **Paste** | Pastes the clipboard's files here; an image on the clipboard (a screenshot) is saved as a file | Enabled with files or an image on the clipboard |
| **Paste as Link** | Makes symbolic links here to the clipboard's files | Enabled with files on the clipboard |
| **Select All** | Selects everything in the folder | Always |
| **Sort By ▸** | The column (Name, Size, Type, Modified) and order; **Pinned & Starred First**; **Group by Period** / **Group by Day** and **Date to Group By**; **Set as Default** (this sort for folders without their own); **Apply to Subfolders** | Always |
| **Show Hidden Files** | Shows or hides dot files (Ctrl+H) | Always |
| **Collapse All Groups** / **Expand All Groups** | Collapses or expands every date group | When grouped by date |
| **Open in Terminal** | Opens a terminal here | Always |
| **Bookmark This Folder** | Adds this folder to the sidebar's bookmarks | Always |
| **Previews ▸** | As on a folder ([Previews](#previews)), for this folder | Always |
| **Empty Trash** | Empties the trash, asking first | In the Trash |
| **Empty Trash with BleachBit…** | Shreds everything in the trash, on every drive | In the Trash, with BleachBit installed |
| **Scripts ▸** | Runs one of your scripts in this folder | When you have scripts |
| **Properties** | Properties of this folder | Always |

## The image viewer

Right-click the picture in Kestrel's image viewer. The keys are shown in the menu too.

| Entry | What it does | Shows |
|---|---|---|
| **Next** / **Previous** | The next or previous image (Right, Left) | Always |
| **View ▸** | **Fullscreen** (F); **Start / Stop Slideshow** (S); **Fit to Window** (0); **Actual Size** (1); **Keep Zoom Between Images** (K); **Filmstrip** (T), **Info Panel** (I), **Info Line** (Tab); **Compare With the Next Image** (X) side by side, **One at a Time (A/B)** (V) while comparing, and **Show A / Show B** (B) when comparing one at a time | Always |
| **Rotate and Edit ▸** | **Rotate Right** (R), **Rotate Left** (L), **Flip Horizontally** (H); **Save Rotation / Save Changes…** (Ctrl+S, lossless for JPEGs where possible); **Undo the Edits** (Esc) when there are crops or resizes not saved; **Crop…** (Shift+C), **Resize…** (Shift+S); **Straighten** (Shift+O: turns a JPEG's pixels the way its orientation tag says) | Always |
| **Star** / **Unstar** | As in a folder (*) | Always |
| **Pin in Its Folder** / **Unpin** | As in a folder (P) | Always |
| **Copy To ▸** / **Move To ▸** | The last folders used (Alt+1–9 copies, Ctrl+1–9 moves), and **Other Folder…** (C, M) | Always |
| **Show in Folder** | Shows the image in its folder in a Kestrel window (G) | When opened from Kestrel |
| **Open With Default Application** | Opens the image in the system's image app | Always |
| **Open With…** | Picks an app to open it with (O) | When opened from Kestrel |
| **Copy Image** | Copies the picture to the clipboard (Ctrl+C) | Always |
| **Copy Path** | Copies the image's full path as text | Always |
| **Set as Wallpaper** | Sets it as the wallpaper (through UWP when installed) | Always |
| **Add to Selected UWP Monitor** | Puts it on the monitors selected in UWP's editor | While UWP's editor is open |
| **Properties** | The image's Properties window (Alt+Return) | When opened from Kestrel |
| **Move to Trash** | Trashes the image and goes to the next one (Delete) | When opened from Kestrel |
| **Undo** | Undoes the last action, putting a trashed image back in the viewer (Ctrl+Z) | When opened from Kestrel |

## Items in the Trash

When everything selected is in the trash (a mix with other files gets the usual menus):

| Entry | What it does | Shows |
|---|---|---|
| **Restore** | Puts the item(s) back where they were | Always |
| **Delete Permanently** | Deletes them from the trash | Always |
| **Shred with BleachBit…** | Overwrites and deletes them | With BleachBit installed |
| **Properties** | Properties | Always |

## Empty space in Starred or Recent

| Entry | What it does |
|---|---|
| **Select All** | Selects everything listed |

## A tab

Right-click a tab's title.

| Entry | What it does | Shows |
|---|---|---|
| **Duplicate Tab** | Opens a copy of the tab, at the same place | Always |
| **Open in New Window** | Moves the tab into a window of its own | Not in Open / Save windows |
| **Close Tab** | Closes the tab | Always |

## The sidebar

| Entry | What it does | Works on |
|---|---|---|
| **Open** / **Open in New Tab** | Opens the place here or in a new tab | Every entry |
| **Open in Terminal** | Opens a terminal there | Folders, drives and mounted shares (not phones or servers) |
| **Find Duplicates…** | Finds the files with the same contents on the drive (as for a folder) | Drives |
| **Edit Bookmark…** | Changes the bookmark's name and location | Bookmarks |
| **Remove Bookmark** | Removes the bookmark (the folder stays) | Bookmarks |
| **Move Up** / **Move Down** | Moves the bookmark in the list | Bookmarks |
| **Empty Trash** / **Empty Trash with BleachBit…** | As on the Trash's empty space | Trash (BleachBit's when installed) |
| **Write Tape Index Now** | Writes the LTFS index to the tape now, so what's written so far is kept if the drive loses power | LTFS tapes |
| **Show Previews on This Drive** | Allows thumbnails, folder previews and image details, which make the drive seek (off by default) | Tapes and floppies |
| **Unmount** | Unmounts the drive (a tape writes its index first); asks first while a job still uses it | Drives, mounted network shares |
| **Remove from Recent Servers** | Forgets a recently used server | Servers not connected |
| **Eject** / **Unmount** | Disconnects the phone or camera | Mounted phones and cameras |

## Overview cards

On the Overview page.

| Entry | What it does | Works on |
|---|---|---|
| **Open** / **Open in New Tab** | Opens it here or in a new tab | Drives, bookmarks, save locations |
| **Open in Terminal** | Opens a terminal at the drive | Mounted drives |
| **Add to Bookmarks** | Bookmarks the drive | Mounted drives |
| **Properties** | The drive's Properties | Mounted drives |
| **Find Duplicates…** | As in the sidebar | Mounted drives |
| **Write Tape Index Now** / **Show Previews on This Drive** | As in the sidebar | Tapes (both), floppies (Show Previews) |
| **Unmount** / **Eject** | Unmounts or ejects it | Drives that can be |
| **Edit Bookmark…** / **Remove Bookmark** | As in the sidebar | Bookmarks |
| **Move Earlier** / **Move Later** | Reorders the save locations | Save locations (a Save window's Overview) |
| **Remove from Save Locations** | Takes the folder off the list (the folder stays) | Save locations |

## Open as Archive window

Right-click items listed inside an archive.

| Entry | What it does | Shows |
|---|---|---|
| **Open** | Extracts a read-only temporary copy of the file and opens it with its app | One file selected |
| **Extract Here** | Extracts the selected items beside the archive | Always |
| **Extract To…** | Asks for a folder and extracts them there | Always |

## Properties → Metadata

Right-click a row on the Metadata tab of the Properties window.

| Entry | What it does | Shows |
|---|---|---|
| **Copy Value** | Copies the tag's value | Always |
| **Copy Row** | Copies `group:tag = value` | Always |
| **View Value…** | Shows the whole value in a window (for long ones) | Always |
| **Edit Value…** | Changes the value | Files whose metadata can be written, on tags that can be edited |
| **Remove Tag…** / **Remove N Tags…** | Removes the selected tags | Files whose metadata can be written, with tags selected |
| **Add Tag…** | Adds a tag | Files whose metadata can be written |

## Dropping with Alt held

Not a right-click, but a menu of its own: drop files while holding **Alt** to choose (onto a folder, empty space, the sidebar or a tab).

| Entry | What it does |
|---|---|
| **Move Here** | Moves the files to where they're dropped |
| **Copy Here** | Copies them |
| **Link Here** | Makes symbolic links to them |
