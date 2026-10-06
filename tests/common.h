// Shared helpers for the test programs (see tests/README.md).
//
// Each test is one program that includes app.cpp (for the app's internal state), runs the real code offscreen and
// prints PASS / FAIL lines. run.sh starts every test with a throwaway HOME on a private D-Bus session bus, so your own
// files, settings and running Kestrel windows are never touched.
#pragma once

#include "app.cpp"   // the app's state and statics (WINDOWS, g_thumbs, open_window…); on_atc, handle_fm1: incoming.h
#include "atc.h"
#include "fileops.h"
#include "undo.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QTimer>
#include <QToolButton>

#include <cstdio>
#include <functional>
#include <gio/gio.h>
#include <signal.h>

namespace test {

inline int fails = 0;

inline void check(bool ok, const QString &what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.toUtf8().constData());
    std::fflush(stdout);
    if (!ok)
        ++fails;
}

inline void skip(const QString &what)
{
    std::printf("SKIP %s\n", what.toUtf8().constData());
    std::fflush(stdout);
}

inline void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

inline bool wait_for(const std::function<bool()> &f, int ms = 5000)
{
    for (int t = 0; t < ms; t += 50) {
        if (f())
            return true;
        spin(50);
    }
    return f();
}

// Print the summary and exit without running destructors (worker threads may still be stopping).
[[noreturn]] inline void finish()
{
    std::printf("%s (%d failed)\n", fails ? "FAILED" : "ALL PASSED", fails);
    std::fflush(stdout);
    _exit(fails ? 1 : 0);
}

// The app's startup, minus the window: the thumbnail manager and settings, as kes_main sets them up.
inline void setup_app()
{
    g_thumbs = new ThumbnailManager;
    apply_thumb_settings(g_thumbs);
}

// Every test program can also be the tower: the radio starts `<this program> --atc`.
inline bool is_tower(int argc, char **argv, int *rc)
{
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--atc") {
            *rc = atc::run_tower(argc, argv);
            return true;
        }
    return false;
}

inline QString home_path(const QString &name) { return join(HOME(), name); }

// ---------------------------------------------------------------- a fake second Kestrel on the bus

// Plays "another Kestrel" through its own bus connection: it can check in, report, call tower methods, and records
// every Broadcast it hears.
class FakeFlight {
public:
    QList<QPair<QString, QJsonObject>> heard;   // (flight, message)

    FakeFlight() { connect_bus(); }

    void connect_bus()
    {
        gchar *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
        conn = g_dbus_connection_new_for_address_sync(
            addr,
            GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, nullptr, nullptr);
        g_free(addr);
        g_dbus_connection_signal_subscribe(
            conn, nullptr, "kestrel_explorer.ATC", "Broadcast", "/kestrel_explorer/ATC", nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *p, gpointer self) {
                const gchar *f, *m;
                g_variant_get(p, "(&s&s)", &f, &m);
                static_cast<FakeFlight *>(self)->heard << qMakePair(QString::fromUtf8(f), QJsonDocument::fromJson(m).object());
            },
            this, nullptr);
    }

    void disconnect_bus() { g_dbus_connection_close_sync(conn, nullptr, nullptr); }

    QString name() const { return QString::fromUtf8(g_dbus_connection_get_unique_name(conn)); }

    // a tower method; its string result ("ok" if it has none), or a null QString on error
    QString call(const char *method, const QByteArray &arg = QByteArray())
    {
        GVariant *r = g_dbus_connection_call_sync(conn, atc::NAME, "/kestrel_explorer/ATC", "kestrel_explorer.ATC", method,
                                                  arg.isNull() ? nullptr : g_variant_new("(s)", arg.constData()), nullptr,
                                                  G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr);
        if (!r)
            return QString();
        QString out = "ok";
        if (g_variant_n_children(r)) {
            const gchar *s;
            g_variant_get(r, "(&s)", &s);
            out = QString::fromUtf8(s);
        }
        g_variant_unref(r);
        return out;
    }

    void check_in() { call("CheckIn", R"({"pid":1,"impl":"fake"})"); }
    void report(const QJsonObject &o) { call("Report", QJsonDocument(o).toJson(QJsonDocument::Compact)); }

    bool heard_type(const QString &type, const QString &from) const
    {
        for (const auto &[f, m] : heard)
            if (m.value("type").toString() == type && f == from)
                return true;
        return false;
    }

    int count_type(const QString &type, const QString &from) const
    {
        int n = 0;
        for (const auto &[f, m] : heard)
            n += m.value("type").toString() == type && f == from;
        return n;
    }

    QJsonObject last_of(const QString &type) const
    {
        for (int i = heard.size() - 1; i >= 0; --i)
            if (heard[i].second.value("type").toString() == type)
                return heard[i].second;
        return {};
    }

    bool flights_have(qint64 pid, const QString &impl)
    {
        for (const QJsonValue &v : QJsonDocument::fromJson(call("Flights").toUtf8()).array())
            if (v.toObject().value("pid").toInteger() == pid && v.toObject().value("impl").toString() == impl)
                return true;
        return false;
    }

    qint64 tower_pid()
    {
        GVariant *r = g_dbus_connection_call_sync(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus", "GetConnectionUnixProcessID",
                                                  g_variant_new("(s)", atc::NAME), nullptr, G_DBUS_CALL_FLAGS_NONE, 3000,
                                                  nullptr, nullptr);
        if (!r)
            return 0;
        guint32 pid;
        g_variant_get(r, "(u)", &pid);
        g_variant_unref(r);
        return pid;
    }

private:
    GDBusConnection *conn = nullptr;
};

}  // namespace test
