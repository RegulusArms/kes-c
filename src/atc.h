// Air traffic control: keeps every running Kestrel ("flight") up to date with what the others change.
//
// Kestrel isn't single-instance: a folder opened from another app may start its own Kestrel process, and a crash only
// takes down that one. The tower (`kes --atc`, no window) is a small service on the D-Bus session bus. Each Kestrel's
// radio checks in with it, reports changes to shared state (Preferences, stars, folder colours and covers, bookmarks,
// cleared caches) and hears what the other flights report. The first Kestrel that finds no tower starts one, and the
// tower lands itself shortly after the last flight leaves. If the tower goes down, the flights start a new one.
//
// Up to 0.3.0-alpha.9 the Python version spoke the same protocol (JSON messages) and shared the tower; the two are
// developed separately since, and a flight that speaks another PROTOCOL is ignored. Admin sessions are never shared:
// each window keeps its own.
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

namespace atc {

extern const char *NAME;   // the tower's bus name

// The protocol (see "the protocol" in atc.cpp for the message types and fields). Bump PROTOCOL when a message changes.
// A flight checks in with {"pid", "impl", "version", "protocol"}; one speaking another protocol is ignored (one that
// names none predates the version and speaks 1). Messages from other programs on the session bus are checked by the
// tower and again by each radio: a JSON object of at most MAX_MESSAGE bytes, a known "type", each known field of the
// right type, absolute paths. Unknown fields are allowed (and ignored), so a newer Kestrel can add some.
constexpr int PROTOCOL = 1;
constexpr int MAX_MESSAGE = 1 << 20;
bool valid_message(const QJsonObject &msg);   // a Report / Broadcast message
bool valid_undo(const QJsonObject &op);       // a shared undo entry: {"kind", "label", "items": [[a, b], …]}

int run_tower(int argc, char **argv);   // `kes --atc`: run the tower until the last flight has left

// Preferences → "Open folders from other apps as tabs": give folders to the Kestrel whose window was used last (it
// opens them as tabs, selecting `select[i]` in folders[i] when given, and comes to the front). Returns that Kestrel's
// bus name, or empty if there's none (no tower, or no Kestrel with a window): then open our own window.
QString hand_off(const QStringList &folders, const QStringList &select = QStringList());

class Radio : public QObject {
    Q_OBJECT
public:
    // Check in with the tower, starting it if there is none (call once the event loop is about to run).
    void start();
    // Tell this process's windows and every other flight. msg has a "type"; heard() repeats it here with "own": true.
    // keep: this is our current state of that type (e.g. our running tasks), not an event. The tower remembers the
    // latest one and hands it to flights that check in later; when we leave, it tells the others ("left").
    void announce(const QString &type, const QJsonObject &extra = QJsonObject(), bool keep = false);
    QString flight() const;   // our name on the bus ("from" in the others' messages), empty if not connected
    bool tower_up() const;
    // Call a tower method and wait for the answer (its string result, "" if it has none); a null QString if the
    // tower isn't up or didn't answer.
    QString request(const QString &method, const QString &arg = QString());

Q_SIGNALS:
    // a change from another flight ("from": its name), or our own ("own": true); {"type": "left"} when a flight has gone
    void heard(const QJsonObject &msg);
    void reset();   // checked in with a (new) tower: forget the others' kept state, it's sent again next
};

Radio *radio();
inline void announce(const QString &type, const QJsonObject &extra = QJsonObject(), bool keep = false)
{
    radio()->announce(type, extra, keep);
}

}  // namespace atc
