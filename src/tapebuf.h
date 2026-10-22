// A RAM write buffer for copies onto a tape, so the drive keeps streaming instead of stopping and rewinding
// ("shoe-shining") whenever the source slows down.
//
// A reader thread reads the files to copy ahead, in the order the copy will write them, into blocks of memory, as fast
// as the source allows and the whole time. The writer (the copy, on the job's thread) takes them out: it starts only
// once the buffer is full (or holds everything left), then writes at the tape's pace. When it runs dry, it either
// carries on at the source's pace, when the source is at least as fast as the drive's lowest streaming speed (the
// drive slows down to match it), or waits until the buffer is full again and writes another full-speed run.
//
// The buffer uses the memory available less a share kept free for other programs (Preferences → Files → Tapes), is
// re-checked as it runs (giving memory back when others need it), and is never one large allocation. A file far larger
// than the buffer streams through it. Sources are read with posix_fadvise so the kernel doesn't keep a second copy.
#pragma once

#include <QHash>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include <sys/types.h>

class Task;

namespace tapebuf {

struct Memory {
    qint64 total = 0, available = 0;   // bytes (MemTotal, MemAvailable)
};
Memory memory();   // /proc/meminfo (KESTREL_TEST_MEMINFO: a file in its format, for the tests)
// The most the buffer may hold now: what's available (in_use: what it already holds counts as available to it), less
// the share of the memory kept free (tape_buffer_keep, % of the total, default 10), and no more than the largest size
// (tape_buffer_gb, default 16 GB, 0: no limit); at least 64 MB.
qint64 budget(const Memory &m, qint64 in_use = 0);
qint64 min_stream_rate();   // bytes/s: the drive's lowest streaming speed (tape_min_speed, MB/s); huge: always refill

class Buffer {
public:
    // files: the regular files the copy will write, in order; capacity: bytes (-1: the budget, re-checked as it runs)
    Buffer(Task *task, const QStringList &files, qint64 capacity = -1);
    ~Buffer();   // stops the reader; what's buffered is dropped

    // The writer starts a file (dev and ino: what it opened, which the buffered data must be from). False: it isn't in
    // the buffer (not planned, or changed since): read it directly. Data of files skipped before it is dropped.
    bool begin(const QString &path, dev_t dev, ino_t ino);
    // The next piece of the file begun: its size (0: the end of the file), *data valid until the next call. Waits as
    // the fill-first rule says. Throws OSError (the source couldn't be read) or Cancelled.
    qint64 next(const char **data);
    void wrote(qint64 bytes, qint64 ns);   // the writer's time writing a piece to the tape (for the tape's speed)

    std::function<void(const QString &)> waiting;   // what to show while waiting for the buffer ("Filling the buffer…")

    qint64 capacity() const { return cap.load(); }
    qint64 filled() const { return held.load(); }
    double source_rate() const;   // bytes/s, recent
    double tape_rate() const;     // bytes/s, while writing
    int waits() const { return wait_count.load(); }   // times the writer ran out and had to wait (the tape stopped)
    bool source_too_slow() const;   // slower than the drive's lowest streaming speed (after a few seconds of reading)
    QString status() const;   // "buffer 85% of 16 GB · reading 150 MB/s · writing 280 MB/s"
    QString summary() const;  // for the job's log, at the end

    // for the tests: how full it was when writing first started, and the least it held when writing went on after
    // running out in burst mode (-1: never)
    qint64 first_write_fill = -1, min_resume_fill = -1;

private:
    struct Chunk {
        enum Kind { BEGIN, DATA, END, ERROR } kind = DATA;
        int file = 0;
        char *data = nullptr;
        qint64 size = 0;
        dev_t dev = 0;
        ino_t ino = 0;
        int err = 0;
    };
    void read_all();
    void push(const Chunk &c);
    void release(Chunk &c);
    qint64 chunk_size() const;
    void wait_a_little(std::unique_lock<std::mutex> &l);   // up to 200 ms; a cancel or pause is handled meanwhile

    Task *task;
    QStringList files;
    QHash<QString, QList<int>> where;   // path -> its indexes in files
    bool fixed;                          // capacity given (the tests), not re-checked
    std::atomic<qint64> cap{0}, held{0};
    std::atomic<bool> stop{false}, reader_done{false};
    std::atomic<int> wait_count{0};
    mutable std::mutex m;
    std::condition_variable cv;
    std::deque<Chunk> queue;
    Chunk current;                       // the writer's piece (freed at the next call)
    bool has_current = false;
    int pos = 0;                         // the writer's file
    bool filling = true;                 // waiting for the buffer to fill before writing (again)
    bool was_writing = false;
    std::thread reader;
    // speeds
    qint64 read_bytes = 0, read_ns = 0;        // (under m)
    double read_recent = 0;                    // bytes/s, smoothed
    qint64 started_ms = 0;
    qint64 write_bytes = 0, write_ns = 0;
};

}  // namespace tapebuf
