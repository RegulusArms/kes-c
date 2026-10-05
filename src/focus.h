// Giving the focus to the window a drag from Kestrel was dropped into (a browser, an editor, another app).
// On X11 Kestrel asks the window manager to activate the window under the pointer. On GNOME on Wayland an app can't
// focus another app's window, so a small GNOME Shell extension does it (data/gnome-shell/, installed and enabled by
// kes-setup --default); without it the focus stays on Kestrel.
#pragma once

namespace focus {

void activate_at_pointer();   // the window under the pointer gets the focus (if the desktop lets us)

}  // namespace focus
