// Running command-line tools: spawn with pipes (in their own session, so a whole job can be killed), wait,
// and capture output with a timeout.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <memory>
#include <sys/types.h>

namespace proc {

// stdin/stdout/stderr modes; any value >= 0 is a file descriptor to use
enum : int { PIPE = -1, DEVNULL = -2, STDOUT = -3, INHERIT = -4 };

struct Options {
    QString cwd;
    int in = DEVNULL;
    int out = PIPE;
    int err = PIPE;
    bool new_session = true;   // start_new_session=True: no controlling terminal, its own process group
    bool c_utf8 = false;       // LC_ALL=C.UTF-8: English messages, UTF-8 file names
};

// A running program. Destroying it while the program still runs stops it, with its process group, and reaps it;
// programs meant to outlive Kestrel are started with start_detached(). Not thread-safe: its owner serialises calls.
class Process {
public:
    ~Process();
    pid_t pid = -1;
    int in = -1, out = -1, err = -1;   // parent ends of the pipes (-1 if not a pipe)
    bool done = false;
    int returncode = -1;               // exit code, or -signal

    bool poll();          // true once it has exited
    int wait();
    bool wait_for(int ms);   // wait up to ms for it to exit; true if it has
    void kill_group();    // SIGKILL its whole process group, then reap it
    void close_in();
    void close_out();
    QByteArray read_all_out();   // until EOF
    QByteArray read_all_err();

private:
    void set_status(int status);
};

// Start argv[0] (searched on PATH). Raises OSError if it can't be started.
std::unique_ptr<Process> spawn(const QStringList &argv, const Options &opts = Options());

struct Result {
    int rc = -1;
    QByteArray out, err;
    bool timed_out = false;
    bool failed = false;   // could not start
};

// subprocess.run(argv, capture_output=True, timeout=...) — never raises. The timeout (ms, -1: none) covers the whole
// run, with or without pipes: a program still running then is killed, with its process group, and timed_out is set.
Result run(const QStringList &argv, int timeout_ms = -1, const Options &opts = Options(),
           const QByteArray &input = QByteArray());

// Popen(..., start_new_session=True) without waiting for it
bool start_detached(const QStringList &argv, const QString &cwd = QString(), bool quiet = false);

}  // namespace proc
