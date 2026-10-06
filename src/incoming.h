// Requests from outside this Kestrel: changes other Kestrels report through the tower, folders handed over to open as
// tabs, and other apps' FileManager1 requests ("Show in folder"). Python: kestrel/incoming.py.
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

class QWindow;

void on_atc(const QJsonObject &msg);   // a change reported through the tower (atc.h)
void handle_fm1(const QString &method, const QStringList &uris, const QString &startup_id);   // fm1.h
void report_windows();                 // tell the tower whether we have windows, and when one was last used
void window_focused(QWindow *win);     // (focusWindowChanged) remembers which window was used last
bool open_in_tabs();                   // Preferences → "Open folders from other apps as tabs…" applies here
