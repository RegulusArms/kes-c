#include "fm1.h"

#include <QCoreApplication>
#include <QTimer>

#include <gio/gio.h>

namespace fm1 {

static const char *NAME = "org.freedesktop.FileManager1";
static const char *PATH = "/org/freedesktop/FileManager1";
static const char *XML = R"(
<node>
  <interface name="org.freedesktop.FileManager1">
    <method name="ShowFolders"><arg type="as" name="URIs" direction="in"/><arg type="s" name="StartupId" direction="in"/></method>
    <method name="ShowItems"><arg type="as" name="URIs" direction="in"/><arg type="s" name="StartupId" direction="in"/></method>
    <method name="ShowItemProperties"><arg type="as" name="URIs" direction="in"/><arg type="s" name="StartupId" direction="in"/></method>
  </interface>
</node>
)";

struct State {
    std::function<void(const QString &, const QStringList &, const QString &)> handler;
    std::function<void()> on_lost;
    GDBusNodeInfo *node = nullptr;
};
static State *state = nullptr;

static void on_call(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *method,
                    GVariant *params, GDBusMethodInvocation *invocation, gpointer)
{
    GVariantIter *iter = nullptr;
    const gchar *startup_id = nullptr;
    g_variant_get(params, "(as&s)", &iter, &startup_id);
    QStringList uris;
    const gchar *uri;
    while (g_variant_iter_next(iter, "&s", &uri))
        uris << QString::fromUtf8(uri);
    g_variant_iter_free(iter);
    g_dbus_method_invocation_return_value(invocation, nullptr);   // answer straight away: the caller doesn't wait
    QString m = QString::fromUtf8(method), sid = QString::fromUtf8(startup_id);
    QTimer::singleShot(0, qApp, [m, uris, sid]() { state->handler(m, uris, sid); });
}

static const GDBusInterfaceVTable vtable = {on_call, nullptr, nullptr, {nullptr}};

void start(std::function<void(const QString &, const QStringList &, const QString &)> handler, std::function<void()> on_lost)
{
    if (state)
        return;
    state = new State{std::move(handler), std::move(on_lost), g_dbus_node_info_new_for_xml(XML, nullptr)};
    g_bus_own_name(
        G_BUS_TYPE_SESSION, NAME, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
        [](GDBusConnection *conn, const gchar *, gpointer) {
            g_dbus_connection_register_object(conn, PATH, state->node->interfaces[0], &vtable, nullptr, nullptr, nullptr);
        },
        nullptr,
        [](GDBusConnection *, const gchar *, gpointer) {
            if (state->on_lost)
                QTimer::singleShot(0, qApp, []() { state->on_lost(); });
        },
        nullptr, nullptr);
}

}  // namespace fm1
