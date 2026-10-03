// What GNOME Files offers through extensions: open folders in code editors, Nautilus scripts, Send To (email,
// Bluetooth), and sharing folders on the network (Samba usershares).
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QMenu;
class QWidget;

namespace sharing {

QString scripts_dir();   // ~/.local/share/nautilus/scripts

// Open in <editor> items and an Open With submenu for a folder
void add_open_folder_menu(QMenu *menu, const QString &folder, std::function<void()> on_other);
// a Scripts submenu, when the Nautilus scripts folder has any
void add_scripts_menu(QMenu *menu, const QStringList &paths, const QString &cur_dir,
                      std::function<void(const QString &)> open_folder);
// Run a Nautilus script the way GNOME Files does: in the current folder, with the selected files as arguments and
// NAUTILUS_SCRIPT_* variables.
bool run_script(const QString &script, const QStringList &paths, const QString &cur_dir);
void add_send_to_menu(QMenu *menu, const QStringList &paths);   // Send To → Email / Bluetooth, for files
bool can_share();
void share_dialog(QWidget *parent, const QString &folder);      // like GNOME Files' Sharing Options

}  // namespace sharing
