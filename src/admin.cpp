#include "admin.h"

#include "fileops.h"
#include "proc.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>

#include <cerrno>
#include <thread>
#include <time.h>
#include <unistd.h>

namespace admin {

static qint64 monotonic_s()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

QString helper_path() { return util::join(QCoreApplication::applicationDirPath(), "kes-admin-helper"); }

// A path with its symlinks resolved, as far as it exists (the rest is kept as it is).
static QString resolve_existing(const QString &p)
{
    QString head = p, tail;
    for (;;) {
        QString real = QFileInfo(head).canonicalFilePath();
        if (!real.isEmpty())
            return tail.isEmpty() ? real : util::join(real, tail);
        if (head == "/" || head.isEmpty())
            return p;
        QString name = util::basename(head);
        tail = tail.isEmpty() ? name : util::join(name, tail);
        head = util::dirname(head);
    }
}

// The helper refuses to follow a symlink that root doesn't control (see admin_helper.cpp), so the folders in each
// path are resolved here first, as the user: their own symlinked folders (~/Shared → /mnt/data) keep working. The
// last part stays as it is (a symlink there is worked on as the link), except where the op means what a link points
// to: chmod, and copyfile's template.
QVariantMap resolve_paths(const QString &op, QVariantMap args)
{
    static const QMap<QString, QStringList> PATHS = {
        {"delete", {"path"}},  {"copy", {"src", "dst"}},   {"move", {"src", "dst"}},       {"rename", {"src", "dst"}},
        {"mkdir", {"path"}},   {"touch", {"path"}},        {"copyfile", {"src", "dst"}},   {"symlink", {"link"}},
        {"hardlink", {"target", "link"}}, {"write", {"path"}}, {"chmod", {"path"}}};
    for (const QString &key : PATHS.value(op)) {
        QString p = args.value(key).toString();
        if (!p.startsWith('/'))
            continue;   // the helper refuses it
        bool whole = (op == "chmod" && key == "path") || (op == "copyfile" && key == "src");
        args[key] = whole ? resolve_existing(p) : util::join(resolve_existing(util::dirname(p)), util::basename(p));
    }
    return args;
}

struct AdminSession::Helper {
    std::unique_ptr<proc::Process> p;
    std::atomic<bool> alive{true};
};

// reads one line from fd (blocking); false at EOF
static bool read_line(int fd, QByteArray &buf, QByteArray &line)
{
    for (;;) {
        int nl = buf.indexOf('\n');
        if (nl >= 0) {
            line = buf.left(nl);
            buf.remove(0, nl + 1);
            return true;
        }
        char chunk[4096];
        ssize_t n = ::read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!buf.isEmpty()) {
                line = buf;
                buf.clear();
                return true;
            }
            return false;
        }
        buf.append(chunk, n);
    }
}

AdminSession::AdminSession()
{
    connect(&timer, &QTimer::timeout, this, &AdminSession::idle_check);
    timer.start(30000);
}

AdminSession::~AdminSession() { end(); }

bool AdminSession::available() { return util::which("pkexec") && util::exists(helper_path()); }

bool AdminSession::active()
{
    std::lock_guard<std::mutex> g(lock);
    return helper && helper->alive && !helper->p->poll();
}

void AdminSession::start()
{
    {
        std::lock_guard<std::mutex> sg(start_lock);
        if (active())
            return;
        QString pkexec = util::which_path("pkexec");
        if (pkexec.isEmpty())
            throw AdminError("Administrator actions need pkexec. Install it with:  sudo apt install pkexec");
        proc::Options o;
        o.in = proc::PIPE;
        o.out = proc::PIPE;
        o.err = proc::PIPE;
        o.new_session = false;
        auto h = std::make_shared<Helper>();
        try {
            h->p = proc::spawn({pkexec, helper_path()}, o);
        } catch (const OSError &e) {
            throw AdminError(e.message());
        }
        QByteArray buf, line;
        QJsonObject hello;
        if (read_line(h->p->out, buf, line))
            hello = QJsonDocument::fromJson(line).object();
        if (!hello.value("hello").toBool()) {
            int rc = h->p->wait();
            if (rc == 126 || rc == 127)
                throw AdminError("Not authorized: the password prompt was dismissed or the password was wrong.");
            QString err = QString::fromUtf8(h->p->read_all_err()).trimmed();
            throw AdminError(!err.isEmpty() ? err : QString("The admin helper failed to start (exit code %1).").arg(rc));
        }
        {
            std::lock_guard<std::mutex> g(lock);
            helper = h;
            last_used = monotonic_s();
        }
        std::thread([this, h, buf]() mutable {
            QByteArray line;
            while (read_line(h->p->out, buf, line)) {
                QJsonObject msg = QJsonDocument::fromJson(line).object();
                std::shared_ptr<Queue> q;
                {
                    std::lock_guard<std::mutex> g(lock);
                    auto it = pending.find(msg.value("id").toVariant().toLongLong());
                    if (it != pending.end())
                        q = it->second;
                }
                if (q) {
                    std::lock_guard<std::mutex> g(q->m);
                    q->items.push_back(std::make_shared<QJsonObject>(msg));
                    q->cv.notify_all();
                }
            }
            std::vector<std::shared_ptr<Queue>> waiting;
            {
                std::lock_guard<std::mutex> g(lock);
                h->alive = false;
                h->p->wait();
                if (helper == h)
                    helper.reset();
                for (auto &[id, q] : pending)
                    waiting.push_back(q);
            }
            for (auto &q : waiting) {
                std::lock_guard<std::mutex> g(q->m);
                q->items.push_back(std::make_shared<QJsonObject>(
                    QJsonObject{{"ok", false}, {"error", "The admin session ended."}}));
                q->cv.notify_all();
            }
            Q_EMIT changed(false);
        }).detach();
    }
    Q_EMIT changed(true);
}

void AdminSession::send(const QByteArray &line)
{
    std::lock_guard<std::mutex> g(lock);
    if (!helper || !helper->alive || helper->p->in < 0)
        throw AdminError("The admin session has ended.");
    QByteArray data = line + "\n";
    qsizetype off = 0;
    while (off < data.size()) {
        ssize_t w = ::write(helper->p->in, data.constData() + off, size_t(data.size() - off));
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0)
            throw AdminError("The admin session has ended.");
        off += w;
    }
}

void AdminSession::call(Task *task, const QString &op, const QVariantMap &args)
{
    start();
    qint64 rid;
    auto q = std::make_shared<Queue>();
    {
        std::lock_guard<std::mutex> g(lock);
        rid = next_id++;
        pending[rid] = q;
        busy += 1;
    }
    struct Finally {
        AdminSession *s;
        qint64 rid;
        ~Finally()
        {
            std::lock_guard<std::mutex> g(s->lock);
            s->pending.erase(rid);
            s->busy -= 1;
            s->last_used = monotonic_s();
        }
    } fin{this, rid};
    QJsonObject req = QJsonObject::fromVariantMap(resolve_paths(op, args));
    req["id"] = rid;
    req["op"] = op;
    send(QJsonDocument(req).toJson(QJsonDocument::Compact));
    bool cancel_sent = false;
    for (;;) {
        // checked on every message: progress arrives ~10×/s, so waiting for a quiet moment would never cancel
        if (task && task->cancelled && !cancel_sent) {
            send(QJsonDocument(QJsonObject{{"op", "cancel"}, {"id", rid}}).toJson(QJsonDocument::Compact));
            cancel_sent = true;
        }
        std::shared_ptr<QJsonObject> msg;
        {
            std::unique_lock<std::mutex> g(q->m);
            q->cv.wait_for(g, std::chrono::milliseconds(200), [&] { return !q->items.empty(); });
            if (q->items.empty())
                continue;
            msg = q->items.front();
            q->items.pop_front();
        }
        if (msg->contains("progress")) {
            if (task) {
                QJsonArray pr = msg->value("progress").toArray();
                task->report(pr.at(0).toDouble(), pr.at(1).toDouble(), pr.at(2).toString());
            }
            continue;
        }
        if (msg->value("ok").toBool())
            return;
        if (msg->value("cancelled").toBool())
            throw Cancelled();
        QString err = msg->value("error").toString();
        throw AdminError(err.isEmpty() ? "failed" : err);
    }
}

void AdminSession::end()
{
    std::lock_guard<std::mutex> g(lock);
    if (helper && helper->alive && helper->p->in >= 0)
        helper->p->close_in();
}

void AdminSession::idle_check()
{
    bool idle;
    {
        std::lock_guard<std::mutex> g(lock);
        idle = !busy && monotonic_s() - last_used > IDLE_MINUTES * 60;
    }
    if (idle && active())
        end();
}

AdminSession &session()
{
    static AdminSession *s = new AdminSession();
    return *s;
}

// ---------------------------------------------------------------- UI

Indicator::Indicator(QWidget *parent) : QToolButton(parent)
{
    setText("🛡 Admin");
    setAutoRaise(true);
    setToolTip(QString("Admin session open: administrator actions don't ask for your password again.\n"
                       "Ends after %1 minutes without admin actions, or when Kestrel quits.")
                   .arg(IDLE_MINUTES));
    setPopupMode(QToolButton::InstantPopup);
    auto *m = new QMenu(this);
    m->addAction("End Admin Session", []() { session().end(); });
    setMenu(m);
    auto style = [this]() {
        setStyleSheet(QString("QToolButton { color: %1; font-weight: bold; }").arg(util::error_color().name()));
    };
    style();
    util::on_palette_change(this, style);
    connect(&session(), &AdminSession::changed, this, &QWidget::setVisible);
    setVisible(session().active());
}

void start_session(QWidget *parent)
{
    if (!session().available()) {
        QMessageBox::warning(parent, "Admin Session",
                             "Administrator actions need pkexec. Install it with:\n\nsudo apt install pkexec");
        return;
    }
    QPointer<QWidget> p(parent);
    fileops::run_job(
        parent, "Starting admin session",
        [](Task *) -> QVariant {
            try {
                session().start();
            } catch (const AdminError &e) {
                return e.message();
            }
            return QString();
        },
        [p](const QVariant &err) {
            if (!err.toString().isEmpty())
                QMessageBox::warning(p, "Admin Session", err.toString());
        },
        false)
        ->admin = true;
}

void retry_as_admin(QWidget *parent, const QString &title, const QString &message,
                    std::function<QStringList(Task *)> work, std::function<void(bool)> on_done)
{
    AdminSession &s = session();
    if (!s.available()) {
        QMessageBox::warning(parent, title, message + "\n\nInstall pkexec to retry as administrator.");
        if (on_done)
            on_done(false);
        return;
    }
    if (!s.active()) {
        QMessageBox box(QMessageBox::Warning, title,
                        message + QString("\n\nRetry as administrator? You'll be asked for your password once. The "
                                          "admin session then stays open (🛡 Admin in the status bar) until you end "
                                          "it or it's unused for %1 minutes.")
                                      .arg(IDLE_MINUTES),
                        QMessageBox::Cancel, parent);
        QPushButton *retry = box.addButton("Retry as Administrator", QMessageBox::AcceptRole);
        box.setDefaultButton(retry);
        box.exec();
        if (box.clickedButton() != retry) {
            if (on_done)
                on_done(false);
            return;
        }
    }
    auto job = [work](Task *task) -> QVariant {
        QStringList errors;
        try {
            errors = work(task);
        } catch (const AdminError &e) {
            errors = QStringList{e.message()};
        }
        return QVariantMap{{"errors", errors}};
    };
    QPointer<QWidget> p(parent);
    auto done = [p, title, on_done](const QVariant &res) {
        QStringList errors = res.toMap().value("errors").toStringList();
        if (!errors.isEmpty())
            QMessageBox::warning(p, title, errors.mid(0, 20).join('\n'));
        if (on_done)
            on_done(res.isValid() && errors.isEmpty());
    };
    fileops::run_job(parent, title + " (as administrator)", job, done)->admin = true;
}

}  // namespace admin
