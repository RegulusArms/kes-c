#include "focus.h"
#include "util.h"

#include <QGuiApplication>

#include <gio/gio.h>
#include <xcb/xcb.h>

namespace focus {

static xcb_atom_t atom(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, strlen(name), name), nullptr);
    xcb_atom_t a = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
    free(r);
    return a;
}

static bool has_property(xcb_connection_t *c, xcb_window_t w, xcb_atom_t prop)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(c, xcb_get_property(c, 0, w, prop, XCB_ATOM_ANY, 0, 0), nullptr);
    bool has = r && r->type != XCB_ATOM_NONE;
    free(r);
    return has;
}

static xcb_window_t client_window(xcb_connection_t *c, xcb_window_t w, xcb_atom_t wm_state, int depth = 0)
{
    // the app's own window in a top-level (the window manager's frame around it): the one with WM_STATE
    if (has_property(c, w, wm_state))
        return w;
    if (depth > 4)
        return XCB_WINDOW_NONE;
    xcb_query_tree_reply_t *t = xcb_query_tree_reply(c, xcb_query_tree(c, w), nullptr);
    if (!t)
        return XCB_WINDOW_NONE;
    xcb_window_t found = XCB_WINDOW_NONE;
    xcb_window_t *kids = xcb_query_tree_children(t);
    for (int i = xcb_query_tree_children_length(t) - 1; i >= 0 && !found; --i)   // topmost first
        found = client_window(c, kids[i], wm_state, depth + 1);
    free(t);
    return found;
}

static void activate_x11()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    xcb_connection_t *c = x11 ? x11->connection() : nullptr;
    if (!c)
        return;
    xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    xcb_query_pointer_reply_t *p = xcb_query_pointer_reply(c, xcb_query_pointer(c, root), nullptr);
    xcb_window_t top = p ? p->child : xcb_window_t(XCB_WINDOW_NONE);
    free(p);
    if (!top)
        return;
    xcb_window_t win = client_window(c, top, atom(c, "WM_STATE"));
    if (!win)
        return;
    // _NET_ACTIVE_WINDOW from a pager (source 2): window managers honour it, unlike an app's own request
    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = win;
    ev.type = atom(c, "_NET_ACTIVE_WINDOW");
    ev.data.data32[0] = 2;
    ev.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(c, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&ev));
    xcb_flush(c);
}

static void activate_gnome_shell()
{
    // the Kestrel drop focus extension (data/gnome-shell/); without it the call fails, and nothing happens
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus)
        return;
    g_dbus_connection_call(bus, "org.gnome.Shell", "/com/regulusarms/KestrelFocus", "com.regulusarms.KestrelFocus",
                           "ActivateAtPointer", nullptr, nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr,
                           nullptr, nullptr);
    g_object_unref(bus);
}

// KWin (KDE Plasma): a script, loaded into KWin over D-Bus for each drop, run once and unloaded. It activates the
// topmost window under the pointer on the current desktop, as the GNOME Shell extension does.
static const char *KWIN_SCRIPT = R"(// Kestrel Explorer drop focus: the window under the pointer gets the focus
(function () {
    var pos = workspace.cursorPos, windows = workspace.stackingOrder;   // bottom to top
    var desktop = workspace.currentDesktop;
    for (var i = windows.length - 1; i >= 0; i--) {
        var w = windows[i];
        if (w.minimized || w.hidden || !(w.normalWindow || w.dialog || w.utility))
            continue;
        if (!w.onAllDesktops && !w.desktops.some(function (d) { return d.id == desktop.id; }))
            continue;
        var g = w.frameGeometry;
        if (pos.x >= g.x && pos.x < g.x + g.width && pos.y >= g.y && pos.y < g.y + g.height) {
            if (workspace.activeWindow !== w)
                workspace.activeWindow = w;
            return;
        }
    }
})();
)";

static void kwin_unload(GDBusConnection *bus, const QByteArray &name)
{
    g_dbus_connection_call(bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript",
                           g_variant_new("(s)", name.constData()), nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr,
                           nullptr, nullptr);
}

static void activate_kwin()
{
    // the script's file, in the user's runtime folder (written once per Kestrel; the same text from every Kestrel)
    static QString file;
    if (file.isEmpty()) {
        QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
        if (dir.isEmpty() || !util::isdir(dir))
            return;
        try {
            util::write_atomic(util::join(dir, "kestrel-focus.js"), KWIN_SCRIPT);
        } catch (const std::exception &) {
            return;
        }
        file = util::join(dir, "kestrel-focus.js");
    }
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus)
        return;
    // load (it gives the script's number), run, unload: each call answered before the next, none waited for
    auto *name = new QByteArray("kestrel-focus-" + QByteArray::number(QCoreApplication::applicationPid()));
    g_dbus_connection_call(
        bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "loadScript",
        g_variant_new("(ss)", util::enc(file).constData(), name->constData()), G_VARIANT_TYPE("(i)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
            auto *bus = G_DBUS_CONNECTION(src);
            auto *name = static_cast<QByteArray *>(data);
            GVariant *r = g_dbus_connection_call_finish(bus, res, nullptr);
            gint32 id = -1;
            if (r) {
                g_variant_get(r, "(i)", &id);
                g_variant_unref(r);
            }
            if (id < 0) {   // no KWin, or a script of that name left loaded: unloaded for the next drop
                if (r)
                    kwin_unload(bus, *name);
                delete name;
                g_object_unref(bus);
                return;
            }
            g_dbus_connection_call(
                bus, "org.kde.KWin", ("/Scripting/Script" + QByteArray::number(id)).constData(), "org.kde.kwin.Script",
                "run", nullptr, nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr,
                [](GObject *src, GAsyncResult *res, gpointer data) {
                    auto *bus = G_DBUS_CONNECTION(src);
                    auto *name = static_cast<QByteArray *>(data);
                    if (GVariant *r = g_dbus_connection_call_finish(bus, res, nullptr))
                        g_variant_unref(r);
                    kwin_unload(bus, *name);
                    delete name;
                    g_object_unref(bus);
                },
                name);
        },
        name);
}

void activate_at_pointer()
{
    if (QGuiApplication::platformName() == "xcb")
        activate_x11();
    else if (QGuiApplication::platformName().startsWith("wayland") && util::is_kde())
        activate_kwin();
    else if (QGuiApplication::platformName().startsWith("wayland"))
        activate_gnome_shell();
}

}  // namespace focus
