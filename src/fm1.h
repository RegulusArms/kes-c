// The org.freedesktop.FileManager1 D-Bus service: how browsers and other apps open "the file manager".
//
// Chromium-based browsers ("Show in folder"), Firefox ("Open Containing Folder") and many other apps call
// ShowItems / ShowFolders on this service instead of using the default folder app (xdg-mime). GNOME Files normally
// provides it; while Kestrel owns the name, those requests open Kestrel. install.sh --default also installs a D-Bus
// activation file so the bus starts `kes --dbus-service` when no Kestrel is running.
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace fm1 {

// Try to own the service name. handler(method, uris, startup_id) runs on the UI thread for each request; on_lost() runs if the
// name couldn't be owned (another file manager has it) or was taken away.
void start(std::function<void(const QString &, const QStringList &, const QString &)> handler, std::function<void()> on_lost = nullptr);

}  // namespace fm1
