// Saving finished jobs' logs (Preferences → Files → Jobs, off by default), so they're kept after Clear Finished and
// after Kestrel quits: each job's in a file of its own, ~/.cache/kestrel-explorer/job-logs/<day>/<time> <title>.log,
// with its title, how it ended, when it started and ended, its steps and its log, as the Jobs window shows them.
//
// One file per job (not one per day): each is written whole under a part name and renamed into place, with nothing
// read back and nothing for two Kestrels to share. Kept KEEP_DAYS days and MAX_BYTES in all: older days go, then the
// oldest days while they take more (never today's).
#pragma once

#include <QDate>
#include <QString>

struct FinishedTask;

namespace joblogs {

const int KEEP_DAYS = 90;
const qint64 MAX_BYTES = 100LL * 1000 * 1000;

QString folder();                             // ~/.cache/kestrel-explorer/job-logs
bool enabled();                               // the setting (jobs_save_logs)
QString text(const FinishedTask &f);          // what's written
// When enabled: f written on a thread of its own (one after another), then old logs pruned. The name it's written
// under is returned (it's there once wait() returns; "-2" is added to it if another log took that name), or empty when
// it isn't saved.
QString save(const FinishedTask &f);
// The day folders older than keep_days before today go, then the oldest days while all of it is over max_bytes
void prune(const QString &folder, int keep_days, qint64 max_bytes, const QDate &today);
void wait();   // until what's being written is written

}  // namespace joblogs
