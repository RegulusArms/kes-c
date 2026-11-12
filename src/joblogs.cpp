#include "joblogs.h"

#include "fileops.h"
#include "i18n.h"
#include "prefs.h"
#include "util.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QThreadPool>

#include <sys/stat.h>

using namespace util;

namespace joblogs {

QString folder() { return join(APP_CACHE(), "job-logs"); }

bool enabled() { return prefs::get("jobs_save_logs").toBool(); }

QString text(const FinishedTask &f)
{
    QStringList out{f.title};
    QDateTime started = f.ended.addMSecs(-f.took_ms);
    QString line = tx("%1 · started %2 · ended %3 · took %4")
                       .arg(fileops::state_label(f.state), i18n::date_time_secs(started), i18n::date_time_secs(f.ended),
                            fileops::wait_text(f.took_ms));
    if (f.admin)
        line += " · " + tx("as administrator");
    out << line;
    if (!f.steps.isEmpty()) {
        out << QString() << txn("%Ln step(s):", f.steps.size());
        for (const QString &s : f.steps)
            out << "  " + s;
    }
    out << QString() << tx("Log:") << f.log;
    return out.join('\n') + '\n';
}

// a file name from the title: no slashes or control characters, not too long
static QString name_for(const FinishedTask &f, int n)
{
    QString title = f.title;
    title.replace(QRegularExpression("[/\\x00-\\x1f\\x7f]"), "_");
    if (title.size() > 80)
        title = title.left(79) + "…";
    QString stem = f.ended.toString("HHmmss") + "-" + QString::number(QCoreApplication::applicationPid()) + "-" + f.id;
    return stem + (n > 1 ? "-" + QString::number(n) : QString()) + " " + title + ".log";
}

static QThreadPool *writer()
{
    static QThreadPool *p = [] {
        auto *pool = new QThreadPool;
        pool->setMaxThreadCount(1);   // one after another: the pruning never meets a write
        if (QCoreApplication::instance())   // a log being written when Kestrel quits is finished first
            QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [] { wait(); });
        return pool;
    }();
    return p;
}

QString save(const FinishedTask &f)
{
    if (!enabled())
        return QString();
    QString day = join(folder(), f.ended.date().toString("yyyy-MM-dd"));
    QString path = join(day, name_for(f, 1));
    QByteArray data = text(f).toUtf8();
    QString top = folder();
    QDate today = QDate::currentDate();
    writer()->start([f, day, path, data, top, today]() {
        try {
            makedirs(day, true);
            // a new name, claimed without replacing anything: built under a part name and renamed into place
            // (write_text's exclusive), so a failure leaves nothing half-written under it
            for (int n = 1;; ++n) {
                try {
                    write_text(n == 1 ? path : join(day, name_for(f, n)), data, true);
                    break;
                } catch (const OSError &e) {
                    if (e.code != EEXIST || n >= 20)
                        throw;
                }
            }
        } catch (const OSError &e) {   // a log not saved: said on the terminal, nothing else to do
            qWarning("Kestrel: a job's log couldn't be saved: %s", qPrintable(e.message()));
        }
        prune(top, KEEP_DAYS, MAX_BYTES, today);
    });
    return path;
}

static qint64 size_of(const QString &dir)
{
    qint64 n = 0;
    walk(dir, [&](const QString &d, QStringList &, QStringList &files) {
        for (const QString &f : files) {
            struct stat st;
            if (lstat_(join(d, f), st))
                n += st.st_size;
        }
        return true;
    });
    return n;
}

void prune(const QString &top, int keep_days, qint64 max_bytes, const QDate &today)
{
    QStringList days;
    try {
        for (const QString &n : listdir(top))
            if (QDate::fromString(n, "yyyy-MM-dd").isValid() && isdir(join(top, n)))
                days << n;
    } catch (const OSError &) {
        return;
    }
    days.sort();   // oldest first (the names sort as the dates do)
    QString keep_from = today.addDays(-keep_days).toString("yyyy-MM-dd"), now = today.toString("yyyy-MM-dd");
    while (!days.isEmpty() && days.first() < keep_from)
        rmtree(join(top, days.takeFirst()));
    QList<qint64> sizes;
    qint64 total = 0;
    for (const QString &d : std::as_const(days)) {
        sizes << size_of(join(top, d));
        total += sizes.last();
    }
    for (int i = 0; i < days.size() && total > max_bytes && days[i] != now; ++i) {
        rmtree(join(top, days[i]));
        total -= sizes[i];
    }
}

void wait() { writer()->waitForDone(); }

}  // namespace joblogs
