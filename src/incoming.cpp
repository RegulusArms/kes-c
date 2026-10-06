// Requests from outside this Kestrel: changes other Kestrels report through the tower (on_atc), folders handed over to
// open as tabs (Preferences → "Open folders from other apps as tabs in an open Kestrel window"), and other apps'
// FileManager1 requests such as a browser's "Show in folder" (handle_fm1).
#include "incoming.h"

#include "app.h"
#include "animate.h"
#include "atc.h"
#include "places.h"
#include "thumbs.h"
#include "util.h"
#include "widgets.h"

#include <QDateTime>
#include <QJsonArray>
#include <QPointer>
#include <QWindow>

using namespace util;

static QPointer<MainWindow> last_active;   // the window used most recently
static qint64 last_active_ms = 0;

void report_windows()
{
    // for the tower's Handoff: whether we have windows, and when one was last used
    atc::announce("windows", {{"count", int(WINDOWS.size())}, {"active", double(last_active_ms)}}, true);
}

bool open_in_tabs()
{
    // never from a conda environment the user activated: the Kestrel taking over may run in a different one
    return settings().value("open_in_tabs", false).toBool() && !explicit_conda_env();
}

void window_focused(QWindow *win)
{
    for (MainWindow *w : WINDOWS)
        if (win && w->windowHandle() == win) {
            last_active = w;
            last_active_ms = QDateTime::currentMSecsSinceEpoch();
            report_windows();
        }
}

static MainWindow *recent_window()
{
    if (last_active && WINDOWS.contains(last_active.data()))
        return last_active;
    return WINDOWS.isEmpty() ? nullptr : WINDOWS.last();
}

static void use_token(const QString &token)
{
    // the launcher's activation token: without it GNOME (Wayland) won't let the window come to the front
    if (!token.isEmpty()) {
        qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
        qputenv("DESKTOP_STARTUP_ID", token.toUtf8());
    }
}

static void open_as_tabs(MainWindow *w, const QStringList &folders, const QStringList &select)
{
    // folders as new tabs (selecting select[i] in folders[i] when given); the first becomes current
    int first = w->tabs->count();
    for (int i = 0; i < folders.size(); ++i) {
        if (select.value(i).isEmpty()) {
            w->open_location(folders[i], true);
        } else if (Pane *p = w->new_tab(folders[i], false)) {
            p->select_later(select[i]);
        }
    }
    if (w->tabs->count() > first) {
        w->tabs->setCurrentIndex(first);
        w->pane()->view()->setFocus();
    }
    w->setWindowState(w->windowState() & ~Qt::WindowMinimized);
    w->raise();
    w->activateWindow();
}

static bool tabs_instead(const QStringList &folders, const QStringList &select)
{
    // true if the folders went to an open window as tabs: one of ours, or another Kestrel's (through the tower)
    if (!open_in_tabs())
        return false;
    if (MainWindow *w = recent_window()) {
        open_as_tabs(w, folders, select);
        return true;
    }
    return !atc::hand_off(folders, select).isEmpty();
}

void handle_fm1(const QString &method, const QStringList &uris, const QString &startup_id)
{
    // a request to the org.freedesktop.FileManager1 service (see fm1.h), e.g. a browser's "Show in folder"
    QStringList paths;
    for (const QString &u : uris) {
        QString p = uri_to_path(u);
        if (!p.isEmpty())
            paths << p;
    }
    if (paths.isEmpty())
        return;
    if (!startup_id.isEmpty()) {
        // the caller's activation token: without it GNOME (Wayland) won't let the new window take focus
        qputenv("XDG_ACTIVATION_TOKEN", startup_id.toUtf8());
        qputenv("DESKTOP_STARTUP_ID", startup_id.toUtf8());
    }
    if (method == "ShowItemProperties") {
        MainWindow *w = WINDOWS.isEmpty() ? open_window({dirname(paths.first())}) : WINDOWS.last();
        w->properties(paths);
        return;
    }
    MainWindow *w;
    if (method == "ShowFolders") {
        if (tabs_instead(paths, {}))
            return;
        w = open_window(paths);
    } else {
        // ShowItems: each item's folder in a tab, with the item selected, scrolled to and focused (folders too:
        // they're shown in their parent, not opened)
        QStringList folders, firsts;
        for (const QString &p : paths) {
            QString folder = dirname(rstrip(p, '/'));
            if (folder.isEmpty())
                folder = "/";
            if (!folders.contains(folder)) {
                folders << folder;
                firsts << p;
            }
        }
        if (tabs_instead(folders, firsts))
            return;
        w = open_window({folders.first()});
        for (int i = 0; i < folders.size(); ++i) {
            Pane *pane = i == 0 ? w->pane() : w->new_tab(folders[i], false);
            if (pane)
                pane->select_later(firsts[i]);
        }
    }
    w->raise();
    w->activateWindow();
    if (w->pane()) {
        w->tabs->setCurrentIndex(0);
        w->pane()->view()->setFocus();
    }
}

void on_atc(const QJsonObject &m)
{
    // a change reported by another Kestrel through the tower (see atc.h), or by this one ("own")
    QString type = m.value("type").toString();
    bool own = m.value("own").toBool();
    if (type == "sidebar") {   // its order or collapsed sections; also refreshes this process's other windows
        if (!own)
            settings().sync();
        for (MainWindow *w : WINDOWS)
            w->sidebar->refresh();
    }
    if (type == "bookmarks") {   // also refreshes this process's other windows
        for (MainWindow *w : WINDOWS) {
            w->sidebar->refresh();
            for (Pane *p : w->panes())
                if (p->is_overview())
                    p->refresh();
        }
    }
    if (own)
        return;   // the rest was already applied where it was changed
    if (type == "open" && m.value("flight").toString() == atc::radio()->flight()) {
        // folders another Kestrel handed over (open_in_tabs)
        QStringList folders, select;
        for (const QJsonValue &v : m.value("folders").toArray())
            folders << v.toString();
        for (const QJsonValue &v : m.value("select").toArray())
            select << v.toString();
        if (folders.isEmpty())
            return;
        use_token(m.value("token").toString());
        if (MainWindow *w = recent_window()) {
            open_as_tabs(w, folders, select);
        } else {   // our last window closed in the meantime
            MainWindow *nw = open_window(folders.mid(0, 1));
            open_as_tabs(nw, folders.mid(1), select.mid(1));
            if (nw->pane() && !select.value(0).isEmpty())
                nw->pane()->select_later(select[0]);
        }
    } else if (type == "settings") {
        settings().sync();
        apply_preferences();
    } else if (type == "starred") {
        places::reload_starred();
    } else if (type == "folders") {
        g_thumbs->reload_styles();
        for (const QJsonValue &v : m.value("paths").toArray())
            g_thumbs->invalidate(v.toString(), false);   // the sender already removed the cached mosaic
    } else if (type == "thumbs_cleared") {
        g_thumbs->clear_memory();
        for (MainWindow *w : WINDOWS)
            for (Pane *p : w->panes())
                p->animator->clear();
        repaint_all();
    }
}
