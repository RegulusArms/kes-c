#include "uwp.h"

#include "proc.h"
#include "util.h"

#include <gio/gio.h>

#include <unistd.h>

namespace uwp {

static const char *BUS_NAME = "io.github.RegulusArms.UWP";
static const char *OBJECT_PATH = "/io/github/RegulusArms/UWP";
static const char *ACTIONS = "org.gtk.Actions";

QString command()
{
    QString found = util::which_path("uwp");
    if (!found.isEmpty())
        return found;
    QString local = util::expanduser("~/.local/bin/uwp");   // install.sh's location; not always on PATH for GUI apps
    return util::access(local, X_OK) ? local : QString();
}

bool available() { return !command().isEmpty(); }

QString wallpaper_label() { return available() ? "Set as Wallpaper (UWP)" : "Set as Wallpaper"; }

bool set_wallpaper(const QString &path)
{
    QString cmd = command();
    if (cmd.isEmpty())
        return false;
    proc::start_detached({cmd, "--set-wallpaper", util::abspath(path)}, QString(), true);
    return true;
}

static GVariant *call(const char *method, GVariant *params, int timeout_ms)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) {
        g_variant_unref(g_variant_ref_sink(params));
        return nullptr;
    }
    GVariant *res = g_dbus_connection_call_sync(bus, BUS_NAME, OBJECT_PATH, ACTIONS, method, params, nullptr,
                                                G_DBUS_CALL_FLAGS_NO_AUTO_START, timeout_ms, nullptr, nullptr);
    g_object_unref(bus);
    return res;
}

bool editor_open()
{
    GVariant *res = call("Describe", g_variant_new("(s)", "add-to-selected"), 300);
    if (!res)
        return false;
    // reply: ((bsav),) — enabled, parameter type, state
    gboolean enabled = FALSE;
    GVariant *desc = g_variant_get_child_value(res, 0);
    g_variant_get_child(desc, 0, "b", &enabled);
    g_variant_unref(desc);
    g_variant_unref(res);
    return enabled;
}

bool add_to_selected(const QString &path)
{
    QByteArray p = util::abspath(path).toUtf8();
    GVariantBuilder params;
    g_variant_builder_init(&params, G_VARIANT_TYPE("av"));
    g_variant_builder_add(&params, "v", g_variant_new_string(p.constData()));
    GVariantBuilder platform;
    g_variant_builder_init(&platform, G_VARIANT_TYPE("a{sv}"));
    GVariant *res = call("Activate", g_variant_new("(sava{sv})", "add-to-selected", &params, &platform), 2000);
    if (!res)
        return false;
    g_variant_unref(res);
    return true;
}

}  // namespace uwp
