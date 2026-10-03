#include "atc.h"

#include "util.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>

#include <gio/gio.h>
#include <unistd.h>

namespace atc {

const char *NAME = "kestrel_explorer.ATC";
static const char *PATH = "/kestrel_explorer/ATC";
static const char *IFACE = "kestrel_explorer.ATC";
static const char *XML = R"(
<node>
  <interface name="kestrel_explorer.ATC">
    <method name="CheckIn"><arg type="s" name="Info" direction="in"/></method>
    <method name="Report"><arg type="s" name="Message" direction="in"/></method>
    <method name="Flights"><arg type="s" name="Flights" direction="out"/></method>
    <method name="Kept"><arg type="s" name="Messages" direction="out"/></method>
    <signal name="Broadcast"><arg type="s" name="Flight"/><arg type="s" name="Message"/></signal>
  </interface>
</node>
)";
static const int LAND_MS = 10000;   // the tower lands this long after the last flight has left (or if none checks in)

static QByteArray compact(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

// ---------------------------------------------------------------- the tower

struct Tower {
    GDBusNodeInfo *node = nullptr;
    QHash<QString, QJsonObject> flights;   // unique bus name -> what it said when checking in
    QHash<QString, QHash<QString, QJsonObject>> kept;   // flight -> type -> its latest "keep" message
    QTimer land;
};
static Tower *tower = nullptr;

static void tower_call(GDBusConnection *conn, const gchar *sender, const gchar *, const gchar *, const gchar *method,
                       GVariant *params, GDBusMethodInvocation *invocation, gpointer)
{
    QString who = QString::fromUtf8(sender), m = QString::fromUtf8(method);
    if (m == "Kept") {   // the current state for a flight that just checked in (e.g. the others' running tasks)
        QJsonArray list;
        for (auto f = tower->kept.begin(); f != tower->kept.end(); ++f)
            for (const QJsonObject &msg : f.value())
                list << QJsonObject{{"flight", f.key()}, {"message", msg}};
        QByteArray out = QJsonDocument(list).toJson(QJsonDocument::Compact);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", out.constData()));
        return;
    }
    if (m == "Flights") {
        QJsonArray list;
        for (auto it = tower->flights.begin(); it != tower->flights.end(); ++it) {
            QJsonObject f = it.value();
            f["flight"] = it.key();
            list << f;
        }
        QByteArray out = QJsonDocument(list).toJson(QJsonDocument::Compact);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", out.constData()));
        return;
    }
    const gchar *arg = nullptr;
    g_variant_get(params, "(&s)", &arg);
    if (m == "CheckIn") {
        tower->flights[who] = QJsonDocument::fromJson(arg).object();
    } else {   // Report: pass it on to every flight (the sender ignores its own)
        if (!tower->flights.contains(who))
            tower->flights[who] = QJsonObject();
        QJsonObject msg = QJsonDocument::fromJson(arg).object();
        if (msg.value("keep").toBool())   // state, not an event: remembered for flights that check in later
            tower->kept[who][msg.value("type").toString()] = msg;
        g_dbus_connection_emit_signal(conn, nullptr, PATH, IFACE, "Broadcast", g_variant_new("(ss)", sender, arg),
                                      nullptr);
    }
    tower->land.stop();
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

static const GDBusInterfaceVTable tower_vtable = {tower_call, nullptr, nullptr, {nullptr}};

static void owner_changed(GDBusConnection *conn, const gchar *, const gchar *, const gchar *, const gchar *,
                          GVariant *params, gpointer)
{
    // a flight that quit or crashed leaves the bus: tell the others, so they forget its state (e.g. its tasks)
    const gchar *name, *old_owner, *new_owner;
    g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
    QString who = QString::fromUtf8(name);
    if (*new_owner || !tower->flights.remove(who))
        return;
    tower->kept.remove(who);
    g_dbus_connection_emit_signal(conn, nullptr, PATH, IFACE, "Broadcast", g_variant_new("(ss)", name, R"({"type":"left"})"),
                                  nullptr);
    if (tower->flights.isEmpty())
        tower->land.start();
}

int run_tower(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    tower = new Tower;
    tower->node = g_dbus_node_info_new_for_xml(XML, nullptr);
    tower->land.setSingleShot(true);
    tower->land.setInterval(LAND_MS);
    QObject::connect(&tower->land, &QTimer::timeout, &app, &QCoreApplication::quit);
    tower->land.start();
    g_bus_own_name(
        G_BUS_TYPE_SESSION, NAME, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
        [](GDBusConnection *conn, const gchar *, gpointer) {
            g_dbus_connection_register_object(conn, PATH, tower->node->interfaces[0], &tower_vtable, nullptr, nullptr,
                                              nullptr);
            g_dbus_connection_signal_subscribe(conn, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                                               "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE, owner_changed,
                                               nullptr, nullptr);
        },
        nullptr,
        // another tower got there first (or there's no session bus): leave it to that one
        [](GDBusConnection *, const gchar *, gpointer) { QTimer::singleShot(0, qApp, []() { QCoreApplication::quit(); }); },
        nullptr, nullptr);
    return app.exec();
}

// ---------------------------------------------------------------- a flight's radio

struct RadioState {
    GDBusConnection *conn = nullptr;
    QHash<QString, QJsonObject> kept;   // our latest "keep" message of each type, sent again to a new tower
    QString tower;          // the tower's unique bus name while it's up
    guint sub = 0;
    qint64 last_launch = 0;
    bool started = false;
};
static RadioState rs;

Radio *radio()
{
    static Radio *r = new Radio;
    return r;
}

static void on_broadcast(GDBusConnection *conn, const gchar *sender, const gchar *, const gchar *, const gchar *,
                         GVariant *params, gpointer)
{
    if (QString::fromUtf8(sender) != rs.tower)
        return;   // only the tower speaks on this channel
    const gchar *flight, *msg;
    g_variant_get(params, "(&s&s)", &flight, &msg);
    if (g_strcmp0(flight, g_dbus_connection_get_unique_name(conn)) == 0)
        return;   // our own report: already delivered here by announce()
    QJsonObject o = QJsonDocument::fromJson(msg).object();
    if (o.value("type").toString().isEmpty())
        return;
    o["from"] = QString::fromUtf8(flight);
    Q_EMIT radio()->heard(o);
}

static void tower_appeared(GDBusConnection *conn, const gchar *, const gchar *owner, gpointer)
{
    if (!rs.conn)
        rs.conn = G_DBUS_CONNECTION(g_object_ref(conn));
    rs.tower = QString::fromUtf8(owner);
    if (!rs.sub)
        rs.sub = g_dbus_connection_signal_subscribe(conn, nullptr, IFACE, "Broadcast", PATH, nullptr,
                                                    G_DBUS_SIGNAL_FLAGS_NONE, on_broadcast, nullptr, nullptr);
    QJsonObject info{{"pid", qint64(getpid())}, {"impl", "c++"}, {"version", util::VERSION}};
    g_dbus_connection_call(conn, NAME, PATH, IFACE, "CheckIn", g_variant_new("(s)", compact(info).constData()), nullptr,
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    for (const QJsonObject &m : rs.kept)   // a new tower: tell it our state again
        g_dbus_connection_call(conn, NAME, PATH, IFACE, "Report", g_variant_new("(s)", compact(m).constData()), nullptr,
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    // then catch up with the others' state (calls on one connection are answered in order)
    g_dbus_connection_call(
        conn, NAME, PATH, IFACE, "Kept", nullptr, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer) {
            GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
            if (!r)
                return;
            const gchar *s;
            g_variant_get(r, "(&s)", &s);
            QJsonArray list = QJsonDocument::fromJson(s).array();
            g_variant_unref(r);
            QString me = radio()->flight();
            Q_EMIT radio()->reset();
            for (const QJsonValue &v : list) {
                QString from = v.toObject().value("flight").toString();
                QJsonObject m = v.toObject().value("message").toObject();
                if (from == me || m.value("type").toString().isEmpty())
                    continue;
                m["from"] = from;
                Q_EMIT radio()->heard(m);
            }
        },
        nullptr);
}

static void launch_tower()
{
    // no tower (yet, or it went down): start one. If several flights do this at once, only one gets the name.
    if (!rs.tower.isEmpty() || QCoreApplication::closingDown())
        return;
    qint64 wait = rs.last_launch + 3000 - QDateTime::currentMSecsSinceEpoch();
    if (wait > 0) {   // launched one moment ago: give it time, then try again if it still isn't up
        QTimer::singleShot(wait, qApp, launch_tower);
        return;
    }
    rs.last_launch = QDateTime::currentMSecsSinceEpoch();
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {"--atc"}, "/");
}

static void tower_vanished(GDBusConnection *conn, const gchar *, gpointer)
{
    rs.tower.clear();
    if (conn)
        launch_tower();
}

void Radio::start()
{
    if (rs.started)
        return;
    rs.started = true;
    g_bus_watch_name(G_BUS_TYPE_SESSION, NAME, G_BUS_NAME_WATCHER_FLAGS_NONE, tower_appeared, tower_vanished, nullptr,
                     nullptr);
}

QString Radio::flight() const
{
    return rs.conn ? QString::fromUtf8(g_dbus_connection_get_unique_name(rs.conn)) : QString();
}

void Radio::announce(const QString &type, const QJsonObject &extra, bool keep)
{
    QJsonObject m = extra;
    m["type"] = type;
    if (keep) {
        m["keep"] = true;
        rs.kept[type] = m;
    }
    if (rs.conn && !rs.tower.isEmpty())
        g_dbus_connection_call(rs.conn, NAME, PATH, IFACE, "Report", g_variant_new("(s)", compact(m).constData()),
                               nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    m["own"] = true;
    QTimer::singleShot(0, this, [this, m]() { Q_EMIT heard(m); });
}

}  // namespace atc
