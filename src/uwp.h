// Integration with UWP (https://github.com/RegulusArms/UWP), a wallpaper manager for GNOME.
//
// When UWP is installed, "Set as Wallpaper" hands the image to UWP, which adds it to its library and shows it
// on every monitor as a new profile. While UWP's editor window is open, files can also be sent to the monitors
// selected there ("Add to Selected UWP Monitor"). Talks to the running UWP over D-Bus (GApplication actions);
// `uwp --set-wallpaper` also starts it.
#pragma once

#include <QString>

namespace uwp {

QString command();           // path of the `uwp` launcher, or empty if UWP isn't installed
bool available();
QString wallpaper_label();   // menu text for "Set as Wallpaper", saying when it goes through UWP
bool set_wallpaper(const QString &path);   // false if UWP is missing
bool editor_open();          // UWP is running with its editor window open
bool add_to_selected(const QString &path);

}  // namespace uwp
