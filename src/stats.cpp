#include "stats.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMap>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace stats {

namespace {
struct Sample {
    qint64 n = 0;
    double sum = 0, max = 0;
};
std::mutex lock;
QMap<QString, Sample> samples;
QMap<QString, qint64> counts, peaks;
}   // namespace

static std::atomic<bool> &on()
{
    static std::atomic<bool> value([] {
        const char *v = std::getenv("KESTREL_STATS");
        return v && *v && QByteArray(v) != "0";
    }());
    return value;
}

bool enabled() { return on(); }

void enable() { on() = true; }

void sample(const char *what, double value)
{
    if (!enabled())
        return;
    std::lock_guard<std::mutex> g(lock);
    Sample &s = samples[what];
    s.max = s.n ? std::max(s.max, value) : value;
    s.n += 1;
    s.sum += value;
}

void count(const char *what, qint64 n)
{
    if (!enabled())
        return;
    std::lock_guard<std::mutex> g(lock);
    counts[what] += n;
}

void peak(const char *what, qint64 value)
{
    if (!enabled())
        return;
    std::lock_guard<std::mutex> g(lock);
    qint64 &p = peaks[what];
    p = std::max(p, value);
}

qint64 now_ms()
{
    static QElapsedTimer t = [] {
        QElapsedTimer e;
        e.start();
        return e;
    }();
    return t.elapsed();
}

QString summary()
{
    std::lock_guard<std::mutex> g(lock);
    QMap<QString, QString> lines;   // sorted by name
    for (auto it = samples.begin(); it != samples.end(); ++it)
        lines[it.key()] = QString("%1×, average %2, largest %3")
                              .arg(QString::number(it->n), QString::number(it->sum / it->n, 'f', 1),
                                   QString::number(it->max, 'f', 1));
    for (auto it = counts.begin(); it != counts.end(); ++it)
        lines[it.key()] = QString::number(it.value());
    for (auto it = peaks.begin(); it != peaks.end(); ++it)
        lines[it.key()] = "most " + QString::number(it.value());
    if (lines.isEmpty())
        return QString();
    QString out = "Kestrel stats (KESTREL_STATS):\n";
    for (auto it = lines.begin(); it != lines.end(); ++it)
        out += "  " + it.key() + ": " + it.value() + "\n";
    return out;
}

void print_at_quit()
{
    if (!enabled())
        return;
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, []() {
        QByteArray s = summary().toUtf8();
        std::fwrite(s.constData(), 1, size_t(s.size()), stderr);
    });
}

}  // namespace stats
