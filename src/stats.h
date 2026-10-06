// Performance counters, for KESTREL_STATS=1 (off otherwise: each call is one check). Kestrel then prints a summary to
// stderr when it quits: folder listing times, thumbnail cache use and queue length, copy speed, archive job times,
// tower message delay, and the most tasks at once. The same counters and output as the Python version (stats.py);
// bench/ can collect them. Thread-safe: thumbnail workers and file-operation tasks report from their own threads.
#pragma once

#include <QString>

namespace stats {

bool enabled();
void enable();   // as if KESTREL_STATS=1 (tests)
void sample(const char *what, double value);   // a timing or a rate: how many, average, largest
void count(const char *what, qint64 n = 1);
void peak(const char *what, qint64 value);      // the largest seen
qint64 now_ms();                                 // for timings
QString summary();                               // the report (empty if nothing was counted)
void print_at_quit();                            // prints summary() to stderr when the app quits, if enabled

}  // namespace stats
