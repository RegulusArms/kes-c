// Every setting Kestrel keeps, in one list: what Preferences shows (its tab, group and label), its default, and where
// else in the windows it can be changed. Preferences is built from this list; a test checks it covers the settings
// the code reads.
//
// Changing one goes through set(): it's saved, applied at once to every open window of this Kestrel (apply_setting,
// app.cpp), and the other Kestrels are told which keys changed (the "settings" message), so they apply them too.
// Changes made elsewhere (the View menu, the zoom slider, the Compress window…) call notify(), so an open Preferences
// window shows them.
#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace prefs {

enum Kind {
    BOOL,     // a check box
    INT,      // a number (min..max, suffix)
    CHOICE,   // one of `choices` (label, stored value)
    CUSTOM,   // a widget of its own (Preferences builds it by key)
};

struct Pref {
    QString key;      // the QSettings key ("chooser/…": a file chooser window's own; see MainWindow::view_value)
    QString tab;      // General, Views, Sidebar, Images & Videos, Files, Archives, Windows
    QString group;    // a heading within the tab (may be empty)
    QString label;
    Kind kind = BOOL;
    QVariant def;     // the default (what's used while it isn't set)
    QString where;    // where else it's changed, e.g. "View → Show Hidden Files (Ctrl+H)"
    QString tip;
    int min = 0, max = 0;
    QString suffix;
    QList<QPair<QString, QVariant>> choices;
};

const QList<Pref> &all();
const Pref *find(const QString &key);
QStringList tabs();   // in order (Menus, the right-click menus' tab, isn't a list of settings: menus.h)

// The value in use: the saved one, else the default (a chooser key without its own: the main windows' value)
QVariant get(const QString &key);
// Save it (an invalid value: back to the default), apply it at once to every open window here, and tell the other
// Kestrels. Several keys at once: set_many (one message).
void set(const QString &key, const QVariant &value);
void set_many(const QList<QPair<QString, QVariant>> &values);
void reset(const QStringList &keys);   // back to their defaults (removed), applied and told like set()

// a setting changed (here or, once read again, in another Kestrel); keys: which ones
class Notifier : public QObject {
    Q_OBJECT
public:
    static Notifier *instance();
Q_SIGNALS:
    void changed(const QStringList &keys);
};
void notify(const QStringList &keys);

// set by the app (app.cpp): applies changed keys to every open window of this Kestrel
extern void (*apply_hook)(const QStringList &keys);

}  // namespace prefs
