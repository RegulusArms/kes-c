#include "atc.h"

#include "stats.h"
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
    <method name="UndoPush"><arg type="s" name="Op" direction="in"/></method>
    <method name="UndoPop"><arg type="s" name="Op" direction="out"/></method>
    <method name="Handoff"><arg type="s" name="Request" direction="in"/><arg type="s" name="Flight" direction="out"/></method>
    <signal name="Broadcast"><arg type="s" name="Flight"/><arg type="s" name="Message"/></signal>
  </interface>
</node>
)";
static const int LAND_MS = 10000;
static const int UNDO_MAX = 50;   // the tower lands this long after the last flight has left (or if none checks in)

static QByteArray compact(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

// ---------------------------------------------------------------- the protocol

// Each message type and its fields. A field that's missing is fine (the receiver uses a default); one that's there
// must have this type. PATHS: a list of absolute paths; STRS: a list of strings; TASKS: a list of tasks (fields below).
enum Field { STR, NUM, BOOL, PATHS, STRS, TASKS };
static const QHash<QString, QList<QPair<QString, Field>>> TYPES = {
    {"settings", {}},        // Preferences were saved
    {"sidebar", {}},         // the sidebar's order or collapsed sections changed
    {"bookmarks", {}},       // the GTK bookmarks changed
    {"starred", {}},         // starred.json changed
    {"thumbs_cleared", {}},  // the preview cache was cleared
    {"left", {}},            // (the tower's) a flight has gone
    {"folders", {{"paths", PATHS}}},   // folder colours, covers or previews changed
    {"tasks", {{"tasks", TASKS}}},     // keep: the flight's running jobs
    {"cancel", {{"flight", STR}, {"task", STR}}},   // cancel a job of that flight
    {"windows", {{"count", NUM}, {"active", NUM}}},   // keep: how many windows, when one was last used
    {"undo_changed", {{"label", STR}}},   // keep (the tower's): what Ctrl+Z would undo now
    {"open", {{"flight", STR}, {"folders", PATHS}, {"select", STRS}, {"token", STR}}},   // (the tower's) a hand-off
};
static const QList<QPair<QString, Field>> TASK_FIELDS = {{"id", STR},          {"title", STR},      {"text", STR},
                                                         {"fraction", NUM},    {"cancellable", BOOL},
                                                         {"cancelling", BOOL}, {"admin", BOOL}};
static const QStringList UNDO_KINDS = {"move", "rename", "trash", "create"};

static bool valid_fields(const QJsonObject &o, const QList<QPair<QString, Field>> &fields);

static bool valid_field(const QJsonValue &v, Field f)
{
    switch (f) {
    case STR:
        return v.isString();
    case NUM:
        return v.isDouble();
    case BOOL:
        return v.isBool();
    case PATHS:
    case STRS:
    case TASKS:
        if (!v.isArray())
            return false;
        for (const QJsonValue &e : v.toArray()) {
            if (f == TASKS ? !e.isObject() || !valid_fields(e.toObject(), TASK_FIELDS)
                           : !e.isString() || (f == PATHS && !e.toString().startsWith('/')))
                return false;
        }
        return true;
    }
    return false;
}

static bool valid_fields(const QJsonObject &o, const QList<QPair<QString, Field>> &fields)
{
    for (const auto &[name, f] : fields)
        if (o.contains(name) && !valid_field(o.value(name), f))
            return false;
    return true;
}

bool valid_message(const QJsonObject &msg)
{
    auto it = TYPES.constFind(msg.value("type").toString());
    return it != TYPES.constEnd() && valid_fields(msg, *it) && (!msg.contains("keep") || msg.value("keep").isBool());
}

bool valid_undo(const QJsonObject &op)
{
    if (!UNDO_KINDS.contains(op.value("kind").toString()) || !op.value("label").isString() || !op.value("items").isArray())
        return false;
    for (const QJsonValue &v : op.value("items").toArray()) {   // [a, b]: absolute paths (b is "" for trash and create)
        QJsonArray pair = v.toArray();
        QString b = pair.at(1).toString();
        if (pair.size() != 2 || !pair.at(0).isString() || !pair.at(1).isString() || !pair.at(0).toString().startsWith('/') ||
            !(b.isEmpty() || b.startsWith('/')))
            return false;
    }
    return true;
}

// a JSON object from a message argument, or an empty one if it's too big or isn't one
static QJsonObject parse(const char *arg)
{
    QByteArray raw(arg);
    if (raw.size() > MAX_MESSAGE)
        return QJsonObject();
    return QJsonDocument::fromJson(raw).object();
}

static bool speaks_our_protocol(const QJsonObject &info)
{
    return !info.contains("protocol") || info.value("protocol").toInt(-1) == PROTOCOL;
}

// ---------------------------------------------------------------- the tower

struct Tower {
    GDBusNodeInfo *node = nullptr;
    QHash<QString, QJsonObject> flights;   // unique bus name -> what it said when checking in
    QHash<QString, QHash<QString, QJsonObject>> kept;   // flight -> type -> its latest "keep" message
    QList<QJsonObject> undo;   // the shared undo history (Preferences → Share undo…), newest last
    QTimer land;
};
static Tower *tower = nullptr;

static void undo_changed(GDBusConnection *conn)
{
    // tell every flight what Ctrl+Z would undo now; kept, so flights checking in later know it too
    QString me = QString::fromUtf8(g_dbus_connection_get_unique_name(conn));
    QJsonObject msg{{"type", "undo_changed"},
                    {"label", tower->undo.isEmpty() ? QString() : tower->undo.last().value("label").toString()},
                    {"keep", true}};
    tower->kept[me]["undo_changed"] = msg;
    g_dbus_connection_emit_signal(conn, nullptr, PATH, IFACE, "Broadcast",
                                  g_variant_new("(ss)", me.toUtf8().constData(), compact(msg).constData()), nullptr);
}

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
    if (m == "UndoPop") {   // each entry is handed out once, so two windows can't undo the same thing
        QByteArray out;
        if (!tower->undo.isEmpty()) {
            out = compact(tower->undo.takeLast());
            undo_changed(conn);
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", out.constData()));
        return;
    }
    if (m == "Handoff") {   // a new Kestrel's folders, for the flight whose window was used last
        const gchar *arg = nullptr;
        g_variant_get(params, "(&s)", &arg);
        QString best;
        double best_active = -1;
        for (auto f = tower->kept.begin(); f != tower->kept.end(); ++f) {
            QJsonObject w = f.value().value("windows");
            if (tower->flights.contains(f.key()) && speaks_our_protocol(tower->flights.value(f.key())) &&
                w.value("count").toInt() > 0 && w.value("active").toDouble() > best_active) {
                best = f.key();
                best_active = w.value("active").toDouble();
            }
        }
        QJsonObject msg = parse(arg);
        msg["type"] = "open";
        msg["flight"] = best;
        if (!valid_message(msg))
            best.clear();   // not a hand-off: nobody takes it
        if (!best.isEmpty()) {
            g_dbus_connection_emit_signal(conn, nullptr, PATH, IFACE, "Broadcast",
                                          g_variant_new("(ss)", g_dbus_connection_get_unique_name(conn), compact(msg).constData()),
                                          nullptr);
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", best.toUtf8().constData()));
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
        tower->flights[who] = parse(arg);
    } else if (m == "UndoPush") {
        QJsonObject op = parse(arg);
        if (valid_undo(op) && speaks_our_protocol(tower->flights.value(who))) {
            tower->undo << op;
            while (tower->undo.size() > UNDO_MAX)
                tower->undo.removeFirst();
            undo_changed(conn);
        }
    } else {   // Report: pass it on to every flight (the sender ignores its own)
        if (!tower->flights.contains(who))
            tower->flights[who] = QJsonObject();
        QJsonObject msg = parse(arg);
        // only a flight's own news: "left", "open" and "undo_changed" are the tower's
        static const QStringList TOWER_ONLY = {"left", "open", "undo_changed"};
        if (valid_message(msg) && !TOWER_ONLY.contains(msg.value("type").toString()) &&
            speaks_our_protocol(tower->flights.value(who))) {
            if (msg.value("keep").toBool())   // state, not an event: remembered for flights that check in later
                tower->kept[who][msg.value("type").toString()] = msg;
            g_dbus_connection_emit_signal(conn, nullptr, PATH, IFACE, "Broadcast", g_variant_new("(ss)", sender, arg),
                                          nullptr);
        }
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
    QJsonObject o = parse(msg);
    if (!valid_message(o))
        return;
    if (o.value("sent").isDouble())   // KESTREL_STATS (both ends): how long it took to get here
        stats::sample("tower delay (ms)", double(QDateTime::currentMSecsSinceEpoch()) - o.value("sent").toDouble());
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
    QJsonObject info{{"pid", qint64(getpid())}, {"impl", "c++"}, {"version", util::VERSION}, {"protocol", PROTOCOL}};
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
                if (from == me || !valid_message(m))
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

QString hand_off(const QStringList &folders, const QStringList &select)
{
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!conn)
        return QString();
    QString token = qEnvironmentVariable("XDG_ACTIVATION_TOKEN", qEnvironmentVariable("DESKTOP_STARTUP_ID"));
    QJsonObject req{{"folders", QJsonArray::fromStringList(folders)}, {"select", QJsonArray::fromStringList(select)},
                    {"token", token}};
    // no tower, no answer within a moment: open our own window
    GVariant *r = g_dbus_connection_call_sync(conn, NAME, PATH, IFACE, "Handoff", g_variant_new("(s)", compact(req).constData()),
                                              G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr, nullptr);
    g_object_unref(conn);
    if (!r)
        return QString();
    const gchar *s;
    g_variant_get(r, "(&s)", &s);
    QString out = QString::fromUtf8(s);
    g_variant_unref(r);
    return out;
}

bool Radio::tower_up() const { return rs.conn && !rs.tower.isEmpty(); }

QString Radio::request(const QString &method, const QString &arg)
{
    if (!tower_up())
        return QString();
    QByteArray a = arg.toUtf8();
    GVariant *r = g_dbus_connection_call_sync(rs.conn, NAME, PATH, IFACE, method.toUtf8().constData(),
                                              arg.isNull() ? nullptr : g_variant_new("(s)", a.constData()), nullptr,
                                              G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    if (!r)
        return QString();
    QString out = "";
    if (g_variant_n_children(r)) {
        const gchar *s;
        g_variant_get(r, "(&s)", &s);
        out = QString::fromUtf8(s);
    }
    g_variant_unref(r);
    return out;
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
    if (rs.conn && !rs.tower.isEmpty()) {
        QJsonObject sent = m;
        if (stats::enabled())   // a field other Kestrels ignore unless they count too
            sent["sent"] = double(QDateTime::currentMSecsSinceEpoch());
        g_dbus_connection_call(rs.conn, NAME, PATH, IFACE, "Report", g_variant_new("(s)", compact(sent).constData()),
                               nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    }
    m["own"] = true;
    QTimer::singleShot(0, this, [this, m]() { Q_EMIT heard(m); });
}

}  // namespace atc
