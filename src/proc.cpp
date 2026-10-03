#include "proc.h"

#include "util.h"

#include <QElapsedTimer>
#include <QProcess>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace proc {

Process::~Process()
{
    for (int fd : {in, out, err})
        if (fd >= 0)
            ::close(fd);
    if (pid > 0 && !done)
        poll();   // reap if already finished; a still-running child is left alone
}

void Process::set_status(int status)
{
    done = true;
    if (WIFEXITED(status))
        returncode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        returncode = -WTERMSIG(status);
}

bool Process::poll()
{
    if (done)
        return true;
    int status = 0;
    pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid)
        set_status(status);
    else if (r < 0 && errno == ECHILD) {
        done = true;
        returncode = -1;
    }
    return done;
}

int Process::wait()
{
    while (!done) {
        int status = 0;
        pid_t r = ::waitpid(pid, &status, 0);
        if (r == pid)
            set_status(status);
        else if (r < 0 && errno != EINTR) {
            done = true;
            returncode = -1;
        }
    }
    return returncode;
}

void Process::kill_group()
{
    if (pid <= 0 || poll())
        return;
    if (::killpg(pid, SIGKILL) != 0)
        ::kill(pid, SIGKILL);
    wait();
}

void Process::close_in()
{
    if (in >= 0) {
        ::close(in);
        in = -1;
    }
}

void Process::close_out()
{
    if (out >= 0) {
        ::close(out);
        out = -1;
    }
}

static QByteArray read_fd(int fd)
{
    QByteArray data;
    if (fd < 0)
        return data;
    char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0)
            data.append(buf, n);
        else if (n == 0 || errno != EINTR)
            break;
    }
    return data;
}

QByteArray Process::read_all_out() { return read_fd(out); }
QByteArray Process::read_all_err() { return read_fd(err); }

std::unique_ptr<Process> spawn(const QStringList &argv, const Options &opts)
{
    if (argv.isEmpty())
        throw OSError(EINVAL, "empty command");
    // everything the child needs is prepared before fork(): only async-signal-safe calls after it
    std::vector<QByteArray> args;
    for (const QString &a : argv)
        args.push_back(util::enc(a));
    std::vector<char *> cargv;
    for (auto &a : args)
        cargv.push_back(a.data());
    cargv.push_back(nullptr);
    std::vector<QByteArray> envs;
    std::vector<char *> cenv;
    for (char **e = environ; *e; ++e) {
        if (opts.c_utf8 && std::strncmp(*e, "LC_ALL=", 7) == 0)
            continue;
        envs.emplace_back(*e);
    }
    if (opts.c_utf8)
        envs.emplace_back("LC_ALL=C.UTF-8");
    for (auto &e : envs)
        cenv.push_back(e.data());
    cenv.push_back(nullptr);
    QByteArray cwd = opts.cwd.isEmpty() ? QByteArray() : util::enc(opts.cwd);

    int pin[2] = {-1, -1}, pout[2] = {-1, -1}, perr[2] = {-1, -1}, pexec[2] = {-1, -1};
    auto closeall = [&]() {
        for (int *p : {pin, pout, perr, pexec})
            for (int i = 0; i < 2; ++i)
                if (p[i] >= 0)
                    ::close(p[i]);
    };
    if ((opts.in == PIPE && ::pipe2(pin, O_CLOEXEC) != 0) || (opts.out == PIPE && ::pipe2(pout, O_CLOEXEC) != 0) ||
        (opts.err == PIPE && ::pipe2(perr, O_CLOEXEC) != 0) || ::pipe2(pexec, O_CLOEXEC) != 0) {
        int e = errno;
        closeall();
        throw OSError(e, errno_text(e));
    }
    int devnull = -1;
    if (opts.in == DEVNULL || opts.out == DEVNULL || opts.err == DEVNULL)
        devnull = ::open("/dev/null", O_RDWR | O_CLOEXEC);

    pid_t pid = ::fork();
    if (pid < 0) {
        int e = errno;
        closeall();
        if (devnull >= 0)
            ::close(devnull);
        throw OSError(e, errno_text(e));
    }
    if (pid == 0) {
        if (opts.new_session)
            ::setsid();
        auto setup = [&](int mode, int pipe_end, int target) {
            int fd = -1;
            if (mode == PIPE)
                fd = pipe_end;
            else if (mode == DEVNULL)
                fd = devnull;
            else if (mode == STDOUT)
                fd = 1;
            else if (mode >= 0)
                fd = mode;
            if (fd >= 0) {
                if (fd == target)
                    ::fcntl(fd, F_SETFD, 0);
                else
                    ::dup2(fd, target);
            }
        };
        setup(opts.in, pin[0], 0);
        setup(opts.out, pout[1], 1);
        setup(opts.err, perr[1], 2);
        ::signal(SIGPIPE, SIG_DFL);
        if (!cwd.isEmpty() && ::chdir(cwd.constData()) != 0) {
            int e = errno;
            (void)!::write(pexec[1], &e, sizeof e);
            ::_exit(127);
        }
        ::execvpe(cargv[0], cargv.data(), cenv.data());
        int e = errno;
        (void)!::write(pexec[1], &e, sizeof e);
        ::_exit(127);
    }
    if (devnull >= 0)
        ::close(devnull);
    ::close(pexec[1]);
    int child_errno = 0;
    ssize_t n;
    do {
        n = ::read(pexec[0], &child_errno, sizeof child_errno);
    } while (n < 0 && errno == EINTR);
    ::close(pexec[0]);
    auto p = std::make_unique<Process>();
    p->pid = pid;
    if (pin[0] >= 0) {
        ::close(pin[0]);
        p->in = pin[1];
    }
    if (pout[1] >= 0) {
        ::close(pout[1]);
        p->out = pout[0];
    }
    if (perr[1] >= 0) {
        ::close(perr[1]);
        p->err = perr[0];
    }
    if (n == sizeof child_errno) {
        p->wait();
        throw OSError(child_errno, errno_text(child_errno, argv[0]));
    }
    return p;
}

Result run(const QStringList &argv, int timeout_ms, const Options &opts_in, const QByteArray &input)
{
    Result res;
    Options opts = opts_in;
    if (!input.isNull())
        opts.in = PIPE;
    std::unique_ptr<Process> p;
    try {
        p = spawn(argv, opts);
    } catch (const OSError &) {
        res.failed = true;
        return res;
    }
    qsizetype written = 0;
    if (p->in >= 0 && input.isEmpty())
        p->close_in();
    QElapsedTimer timer;
    timer.start();
    while (p->out >= 0 || p->err >= 0 || p->in >= 0) {
        struct pollfd fds[3];
        int nfds = 0;
        if (p->out >= 0)
            fds[nfds++] = {p->out, POLLIN, 0};
        if (p->err >= 0)
            fds[nfds++] = {p->err, POLLIN, 0};
        if (p->in >= 0)
            fds[nfds++] = {p->in, POLLOUT, 0};
        int wait_ms = 200;
        if (timeout_ms >= 0) {
            qint64 left = timeout_ms - timer.elapsed();
            if (left <= 0) {
                res.timed_out = true;
                break;
            }
            wait_ms = int(std::min<qint64>(left, 200));
        }
        int r = ::poll(fds, nfds, wait_ms);
        if (r < 0 && errno != EINTR)
            break;
        for (int i = 0; i < nfds; ++i) {
            if (!fds[i].revents)
                continue;
            int fd = fds[i].fd;
            if (fd == p->in) {
                ssize_t w = ::write(fd, input.constData() + written, size_t(input.size() - written));
                if (w > 0)
                    written += w;
                if (w < 0 || written >= input.size())
                    p->close_in();
                continue;
            }
            char buf[65536];
            ssize_t got = ::read(fd, buf, sizeof buf);
            if (got > 0) {
                (fd == p->out ? res.out : res.err).append(buf, got);
            } else if (got == 0 || errno != EINTR) {
                ::close(fd);
                if (fd == p->out)
                    p->out = -1;
                else
                    p->err = -1;
            }
        }
    }
    if (res.timed_out) {
        p->kill_group();
        res.rc = -1;
        return res;
    }
    res.rc = p->wait();
    return res;
}

bool start_detached(const QStringList &argv, const QString &cwd, bool quiet)
{
    if (argv.isEmpty())
        return false;
    QProcess proc;
    proc.setProgram(argv[0]);
    proc.setArguments(argv.mid(1));
    if (!cwd.isEmpty())
        proc.setWorkingDirectory(cwd);
    if (quiet) {
        proc.setStandardOutputFile(QProcess::nullDevice());
        proc.setStandardErrorFile(QProcess::nullDevice());
    }
    return proc.startDetached();
}

}  // namespace proc
