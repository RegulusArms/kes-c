// Admin session: enter your password once, then run privileged file operations without asking again.
//
// The first privileged operation starts kes-admin-helper (next to the kes binary) as root through pkexec (the
// system password prompt). Kestrel itself keeps running as you; it only sends the helper individual operations
// over a private pipe. The session ends when you click the 🛡 Admin indicator in the status bar and choose End,
// after IDLE_MINUTES without admin activity, or when Kestrel quits.
#pragma once

#include "util.h"

#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QToolButton>
#include <QVariantMap>

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

class Task;

namespace admin {

constexpr int IDLE_MINUTES = 15;

class AdminError : public Error {
public:
    using Error::Error;
};

QString helper_path();

class AdminSession : public QObject {
    Q_OBJECT
public:
    AdminSession();
    ~AdminSession() override;

    static bool available();
    bool active();
    // Start the root helper (blocks while the password prompt is up; call from a worker thread).
    void start();
    // Run one operation as root (call from a worker thread). Progress goes to task; Cancel is passed on.
    // Raises AdminError, or Cancelled.
    void call(Task *task, const QString &op, const QVariantMap &args);
    void end();   // close the session: the helper exits as soon as its input closes

Q_SIGNALS:
    void changed(bool active);   // emitted from any thread; receivers are queued

private:
    struct Queue {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::shared_ptr<QJsonObject>> items;
    };
    struct Helper;   // one running helper process

    void send(const QByteArray &line);
    void idle_check();

    std::mutex lock, start_lock;
    std::shared_ptr<Helper> helper;
    std::map<qint64, std::shared_ptr<Queue>> pending;
    qint64 next_id = 1;
    int busy = 0;
    qint64 last_used = 0;
    QTimer timer;
};

AdminSession &session();   // the app-wide admin session (create from the UI thread)

// 🛡 Admin in the status bar while a session is open; click to end it.
class Indicator : public QToolButton {
    Q_OBJECT
public:
    explicit Indicator(QWidget *parent = nullptr);
};

// Open a session ahead of time (from the menu).
void start_session(QWidget *parent);

// After a permission error: run work(task) as administrator. With a session open it just runs; otherwise the
// user is asked first (and then enters their password once). work() uses session().call(); it may return a list
// of per-item error strings. on_done(ok) runs afterwards.
void retry_as_admin(QWidget *parent, const QString &title, const QString &message,
                    std::function<QStringList(Task *)> work, std::function<void(bool)> on_done = nullptr);

}  // namespace admin
