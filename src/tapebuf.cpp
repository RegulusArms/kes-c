#include "tapebuf.h"

#include "fileops.h"
#include "prefs.h"
#include "util.h"

#include <QElapsedTimer>
#include <QFile>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

using namespace util;

namespace tapebuf {

static const qint64 MB = 1024 * 1024;
static const qint64 BLOCK = 16 * MB;     // the largest piece read at once (the buffer is many of these, never one block)
static const qint64 FLOOR = 64 * MB;     // the least the buffer gets

static qint64 now_ns()
{
    struct timespec t;
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return qint64(t.tv_sec) * 1000000000 + t.tv_nsec;
}

Memory memory()
{
    QString path = qEnvironmentVariable("KESTREL_TEST_MEMINFO", "/proc/meminfo");
    Memory m;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return m;
    for (const QByteArray &line : f.readAll().split('\n')) {   // "MemTotal:       32768000 kB"
        QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() < 2)
            continue;
        qint64 kb = parts[1].toLongLong();
        if (parts[0] == "MemTotal:")
            m.total = kb * 1024;
        else if (parts[0] == "MemAvailable:")
            m.available = kb * 1024;
    }
    return m;
}

qint64 safe_size(const Memory &m, qint64 in_use)
{
    double keep = std::clamp(settings().value("tape_buffer_keep", 10).toDouble(), 0.0, 95.0) / 100.0;
    qint64 spare = m.available + in_use;   // what the buffer may draw on: what's available, and what it already holds
    // half of it at most (the rest of the system keeps at least as much as the buffer takes, for its own growth, the
    // page cache, other copies), and never so much that less than the share kept free is left
    qint64 b = std::min(spare / 2, spare - qint64(keep * double(m.total)));
    return std::max(b, FLOOR);
}

qint64 budget(const Memory &m, qint64 in_use)
{
    // (16 GB unless chosen, plenty to stream; GB as Kestrel shows sizes: 10^9 bytes)
    qint64 most = prefs::get("tape_buffer_gb").toLongLong() * 1000000000;
    qint64 safe = safe_size(m, in_use);
    return most > 0 ? std::max(std::min(most, safe), FLOOR) : safe;   // the largest size, only where it's safe
}

qint64 min_stream_rate()
{
    double mbs = settings().value("tape_min_speed", 1000000).toDouble();
    return qint64(mbs * 1e6);
}

Buffer::Buffer(Task *task, const QStringList &files, qint64 capacity) : task(task), files(files), fixed(capacity > 0)
{
    for (int i = 0; i < files.size(); ++i)
        where[files[i]] << i;
    cap = fixed ? capacity : budget(memory());
    started_ms = now_ns() / 1000000;
    reader = std::thread([this]() { read_all(); });
}

Buffer::~Buffer()
{
    stop = true;
    cv.notify_all();
    if (reader.joinable())
        reader.join();
    if (has_current)
        release(current);
    for (Chunk &c : queue)
        release(c);
}

qint64 Buffer::chunk_size() const { return std::clamp(cap.load() / 8, qint64(64 * 1024), BLOCK); }

void Buffer::release(Chunk &c)
{
    if (c.data) {
        held -= c.size;
        delete[] c.data;
        c.data = nullptr;
    }
}

void Buffer::push(const Chunk &c)
{
    {
        std::lock_guard<std::mutex> l(m);
        queue.push_back(c);
    }
    cv.notify_all();
}

// The reader: every planned file in turn, as fast as the source gives it, whenever there's room
void Buffer::read_all()
{
    int delay = qEnvironmentVariableIntValue("KESTREL_TEST_TAPEBUF_DELAY_MS");   // the tests: a slow source
    qint64 checked_ms = 0;
    for (int i = 0; i < files.size() && !stop; ++i) {
        int fd = ::open(enc(files[i]).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st;
        if (fd < 0 || ::fstat(fd, &st) != 0) {
            Chunk e;
            e.kind = Chunk::ERROR, e.file = i, e.err = errno;
            if (fd >= 0)
                ::close(fd);
            push(e);
            continue;
        }
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
        Chunk b;
        b.kind = Chunk::BEGIN, b.file = i, b.dev = st.st_dev, b.ino = st.st_ino;
        push(b);
        qint64 off = 0;
        for (;;) {
            qint64 want = std::min(chunk_size(), std::max(qint64(st.st_size) - off, qint64(64 * 1024)));
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait_for(l, std::chrono::milliseconds(200), [&]() {
                    return stop.load() || held.load() + want <= cap.load();
                });
                if (stop)
                    break;
                if (held.load() + want > cap.load())
                    continue;
                held += want;   // reserved before reading, so the writer's fill target sees it coming
            }
            while (task && task->paused() && !task->cancelled && !stop)   // the job is paused: so is the reading
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (task && task->cancelled)
                stop = true;
            if (stop) {
                held -= want;
                break;
            }
            char *data = new char[want];
            qint64 got = 0, t0 = now_ns();
            int err = 0;
            while (got < want) {
                ssize_t n = ::read(fd, data + got, size_t(want - got));
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0) {
                    err = errno;
                    break;
                }
                if (n == 0)
                    break;
                got += n;
            }
            if (delay > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            qint64 took = now_ns() - t0;
            if (got > 0)   // read: the kernel needn't keep it (the buffer has it)
                ::posix_fadvise(fd, off, got, POSIX_FADV_DONTNEED);
            {
                std::lock_guard<std::mutex> l(m);
                held -= want - got;
                read_bytes += got;
                read_ns += took;
                if (took > 0 && got > 0) {
                    double rate = double(got) / (double(took) / 1e9);
                    read_recent = read_recent <= 0 ? rate : read_recent * 0.8 + rate * 0.2;
                }
            }
            off += got;
            if (err) {
                held -= got;
                delete[] data;
                Chunk e;
                e.kind = Chunk::ERROR, e.file = i, e.err = err;
                push(e);
                break;
            }
            if (got == 0) {
                delete[] data;
                Chunk e;
                e.kind = Chunk::END, e.file = i;
                push(e);
                break;
            }
            Chunk d;
            d.kind = Chunk::DATA, d.file = i, d.data = data, d.size = got;
            push(d);
            // others need memory: hold less (a second at most between looks)
            qint64 ms = now_ns() / 1000000;
            if (!fixed && ms - checked_ms >= 1000) {
                checked_ms = ms;
                cap = budget(memory(), held.load());
            }
        }
        ::close(fd);
    }
    reader_done = true;
    cv.notify_all();
}

void Buffer::wait_a_little(std::unique_lock<std::mutex> &l)
{
    cv.wait_for(l, std::chrono::milliseconds(200));
    if (task && (task->cancelled || task->paused())) {
        l.unlock();
        task->check();   // throws on a cancel; waits while paused
        l.lock();
    }
}

bool Buffer::begin(const QString &path, dev_t dev, ino_t ino)
{
    if (has_current) {
        release(current);
        has_current = false;
    }
    int index = -1;
    for (int i : where.value(path))
        if (i >= pos) {
            index = i;
            break;
        }
    if (index < 0)
        return false;
    std::unique_lock<std::mutex> l(m);
    pos = index;
    for (;;) {
        while (!queue.empty() && queue.front().file < index) {   // files skipped (an error, a conflict…)
            release(queue.front());
            queue.pop_front();
        }
        if (!queue.empty() && queue.front().file == index)
            break;
        if (reader_done && queue.empty())
            return false;
        if (waiting && filling)
            waiting(QString("Filling the buffer — %1 of %2").arg(human_size(held.load()), human_size(cap.load())));
        l.unlock();
        cv.notify_all();
        l.lock();
        wait_a_little(l);
    }
    Chunk first = queue.front();
    if (first.kind == Chunk::BEGIN && first.dev == dev && first.ino == ino) {
        queue.pop_front();
        return true;
    }
    if (first.kind == Chunk::ERROR) {   // the writer reads it itself, and gets the error itself
        queue.pop_front();
        pos = index + 1;
        return false;
    }
    pos = index + 1;   // changed since it was read: read directly, and this data is dropped at the next begin
    return false;
}

qint64 Buffer::next(const char **data)
{
    if (has_current) {
        release(current);
        has_current = false;
        cv.notify_all();
    }
    std::unique_lock<std::mutex> l(m);
    for (;;) {
        if (queue.empty() && !reader_done) {
            if (was_writing) {   // ran out
                was_writing = false;
                // within the drive's speed-matching range: carry on at the source's pace (the drive slows down to
                // match); below it: the tape stops, and writes again once the buffer is full
                filling = read_recent < double(min_stream_rate());
                if (filling)
                    ++wait_count;
            }
            if (waiting)
                waiting(filling ? QString("Filling the buffer — %1 of %2").arg(human_size(held.load()), human_size(cap.load()))
                                : QString("Waiting for the source"));
            wait_a_little(l);
            continue;
        }
        if (filling && !reader_done && held.load() < cap.load() - chunk_size()) {
            if (waiting)
                waiting(QString("Filling the buffer — %1 of %2").arg(human_size(held.load()), human_size(cap.load())));
            wait_a_little(l);
            continue;
        }
        if (filling) {
            filling = false;
            if (first_write_fill < 0)
                first_write_fill = held.load();
            else if (!reader_done && (min_resume_fill < 0 || held.load() < min_resume_fill))
                min_resume_fill = held.load();   // (not the last refill, cut short by the source's end)
        }
        if (queue.empty())   // the reader has finished
            return 0;
        Chunk c = queue.front();
        if (c.file != pos) {   // (no END for the file begun: it went wrong)
            return 0;
        }
        queue.pop_front();
        if (c.kind == Chunk::END)
            return 0;
        if (c.kind == Chunk::ERROR) {
            l.unlock();
            throw_errno(files.value(c.file), c.err);
        }
        if (c.kind == Chunk::BEGIN)
            continue;
        was_writing = true;
        current = c;
        has_current = true;
        *data = c.data;
        return c.size;
    }
}

void Buffer::wrote(qint64 bytes, qint64 ns)
{
    std::lock_guard<std::mutex> l(m);
    write_bytes += bytes;
    write_ns += ns;
}

double Buffer::source_rate() const
{
    std::lock_guard<std::mutex> l(m);
    return read_recent;
}

double Buffer::tape_rate() const
{
    std::lock_guard<std::mutex> l(m);
    return write_ns > 0 ? double(write_bytes) / (double(write_ns) / 1e9) : 0;
}

bool Buffer::source_too_slow() const
{
    std::lock_guard<std::mutex> l(m);
    double min = double(min_stream_rate());
    return read_ns > 5000000000LL && read_bytes > 64 * MB && read_recent > 0 && read_recent < min && min < 1e11;
}

static QString rate(double bps) { return QString("%1/s").arg(human_size(qint64(bps))); }

QString Buffer::status() const
{
    qint64 c = cap.load();
    int pct = c > 0 ? int(held.load() * 100 / c) : 0;
    QString s = QString("buffer %1% of %2").arg(QString::number(pct), human_size(c));
    double r = source_rate(), w = tape_rate();
    if (r > 0)
        s += " · reading " + rate(r);
    if (w > 0)
        s += " · writing " + rate(w);
    return s;
}

QString Buffer::summary() const
{
    QString s = QString("Tape buffer: %1; written to the tape at %2 on average, read from the source at %3")
                    .arg(human_size(cap.load()), rate(tape_rate()), rate(source_rate()));
    int n = waits();
    s += n == 0 ? QString("; the tape never had to wait for it.")
                : QString("; the tape had to wait for it %1 time%2.").arg(QString::number(n), n == 1 ? "" : "s");
    return s;
}

}  // namespace tapebuf
