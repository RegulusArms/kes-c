// Kestrel's admin helper: runs as root (started once through pkexec) for an "admin session".
//
// Kestrel writes one JSON request per line to this process's stdin and reads JSON replies from its stdout.
// Nothing else can talk to it: the pipe belongs to the Kestrel process that started it. The helper exits as soon
// as stdin closes (the session ends, Kestrel quits or crashes). Qt Core is used only for JSON.
//
// Requests:  {"id": n, "op": "...", ...}   or   {"op": "cancel", "id": n}
// Replies:   {"id": n, "progress": [done, total, text]}   then   {"id": n, "ok": true} / {"id": n, "ok": false,
//            "error": "..."}
//
// Ops: delete(path) · copy/move(src, dst, merge) · rename(src, dst) · mkdir(path) · copyfile(src, dst) ·
//      touch(path) · symlink(target, link) · hardlink(target, link) · write(path, text, mode) · chmod(path, mode)

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

// Never delete or overwrite these, whatever Kestrel asks (a guard against a slip turning into a disaster).
static const std::set<std::string> PROTECTED = {
    "/", "/bin", "/boot", "/dev", "/etc", "/home", "/lib", "/lib32", "/lib64", "/libx32", "/media", "/mnt", "/opt",
    "/proc", "/root", "/run", "/sbin", "/srv", "/sys", "/tmp", "/usr", "/var", "/snap", "/usr/bin", "/usr/lib",
    "/usr/sbin", "/usr/share", "/usr/local", "/var/lib", "/etc/ssh"};

static std::mutex out_lock, cancel_lock;
static std::set<qint64> cancelled;

struct Cancelled {};
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

static void send(const QJsonObject &obj)
{
    std::lock_guard<std::mutex> g(out_lock);
    QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    fwrite(line.constData(), 1, size_t(line.size()), stdout);
    fflush(stdout);
}

[[noreturn]] static void fail_errno(int e = -1) { throw Failure(std::strerror(e >= 0 ? e : errno)); }

static std::string normpath(const std::string &p)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos)
            j = p.size();
        std::string c = p.substr(i, j - i);
        if (c == "..") {
            if (!out.empty())
                out.pop_back();
        } else if (!c.empty() && c != ".") {
            out.push_back(c);
        }
        i = j + 1;
    }
    std::string r;
    for (auto &c : out)
        r += "/" + c;
    return r.empty() ? "/" : r;
}

static std::string dirname(const std::string &p)
{
    size_t i = p.rfind('/');
    if (i == std::string::npos)
        return "";
    std::string head = p.substr(0, i + 1);
    while (head.size() > 1 && head.back() == '/')
        head.pop_back();
    return head;
}

static std::string basename(const std::string &p)
{
    size_t i = p.rfind('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

static std::string join(const std::string &a, const std::string &b) { return a.back() == '/' ? a + b : a + "/" + b; }

static std::string path_arg(const QJsonObject &req, const char *key)
{
    QJsonValue v = req.value(key);
    std::string p = v.isString() ? v.toString().toStdString() : std::string();
    if (p.empty() || p[0] != '/')
        throw Failure("not an absolute path: " + p);
    return normpath(p);
}

// top-level folders (/usr, /data, …) and home folders themselves (/home/name) are never deleted or replaced
static void guard(const std::string &p)
{
    std::string parent = dirname(p);
    if (PROTECTED.count(p) || parent == "/" || parent == "/home")
        throw Failure("refusing to delete or replace " + p);
}

static bool lexists(const std::string &p)
{
    struct stat st;
    return lstat(p.c_str(), &st) == 0;
}

static bool islink(const std::string &p)
{
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

static bool isdir(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static std::vector<std::string> listdir(const std::string &p)
{
    DIR *d = opendir(p.c_str());
    if (!d)
        fail_errno();
    std::vector<std::string> out;
    while (struct dirent *e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            out.emplace_back(e->d_name);
    closedir(d);
    return out;
}

static void copystat(const std::string &src, const std::string &dst, bool follow)
{
    struct stat st;
    if ((follow ? stat(src.c_str(), &st) : lstat(src.c_str(), &st)) != 0)
        fail_errno();
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    utimensat(AT_FDCWD, dst.c_str(), times, follow ? 0 : AT_SYMLINK_NOFOLLOW);
    if (!S_ISLNK(st.st_mode) || follow)
        chmod(dst.c_str(), st.st_mode & 07777);
}

static void copyfile_raw(const std::string &src, const std::string &dst, int flags)
{
    int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        fail_errno();
    int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | flags, 0666);
    if (out < 0) {
        int e = errno;
        close(in);
        fail_errno(e);
    }
    std::vector<char> buf(1 << 20);
    for (;;) {
        ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            int e = errno;
            close(in);
            close(out);
            if (n < 0)
                fail_errno(e);
            return;
        }
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(out, buf.data() + off, size_t(n - off));
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0) {
                int e = errno;
                close(in);
                close(out);
                fail_errno(e);
            }
            off += w;
        }
    }
}

class Job {
public:
    explicit Job(qint64 id) : id(id) {}
    qint64 id;
    double done = 0, total = 0;

    void check()
    {
        std::lock_guard<std::mutex> g(cancel_lock);
        if (cancelled.count(id))
            throw Cancelled();
    }

    void report(const std::string &text, bool force = false)
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + ts.tv_nsec / 1e9;
        if (force || now - last >= 0.1) {
            last = now;
            send(QJsonObject{{"id", id}, {"progress", QJsonArray{done, total, QString::fromStdString(text)}}});
        }
    }

    double count(const std::string &path)
    {
        double n = 1;
        if (isdir(path) && !islink(path)) {
            check();
            for (const std::string &name : listdir(path))
                n += count(join(path, name));
        }
        return n;
    }

    void remove(const std::string &path)
    {
        guard(path);
        remove_tree(path);
        done += 1;
    }

    void copy(const std::string &src, const std::string &dst, bool merge)
    {
        if (islink(src)) {
            if (lexists(dst) && unlink(dst.c_str()) != 0)
                fail_errno();
            char target[PATH_MAX];
            ssize_t n = readlink(src.c_str(), target, sizeof target - 1);
            if (n < 0)
                fail_errno();
            target[n] = 0;
            if (symlink(target, dst.c_str()) != 0)
                fail_errno();
        } else if (isdir(src)) {
            if (lexists(dst) && !merge)
                remove(dst);
            if (mkdir(dst.c_str(), 0777) != 0 && !(errno == EEXIST && isdir(dst)))
                fail_errno();
            for (const std::string &name : listdir(src))
                copy(join(src, name), join(dst, name), merge);
            copystat(src, dst, false);
        } else {
            check();
            copyfile_raw(src, dst, O_TRUNC);
            copystat(src, dst, true);
        }
        done += 1;
        report(basename(src));
    }

    void move(const std::string &src, const std::string &dst, bool merge)
    {
        struct stat a, b;
        if (!merge && lstat(src.c_str(), &a) == 0 && stat(dirname(dst).c_str(), &b) == 0 && a.st_dev == b.st_dev) {
            if (lexists(dst))
                remove(dst);
            if (rename(src.c_str(), dst.c_str()) == 0) {
                done = total;
                return;
            }
        }
        copy(src, dst, merge);
        remove(src);
    }

private:
    double last = 0;

    void remove_tree(const std::string &path)
    {
        if (isdir(path) && !islink(path)) {
            for (const std::string &name : listdir(path)) {
                check();
                std::string p = join(path, name);
                if (isdir(p) && !islink(p)) {
                    remove_tree(p);
                } else if (unlink(p.c_str()) != 0) {
                    fail_errno();
                }
                done += 1;
                report(name);
            }
            if (rmdir(path.c_str()) != 0)
                fail_errno();
        } else if (unlink(path.c_str()) != 0) {
            fail_errno();
        }
    }
};

static void handle(const QJsonObject &req)
{
    std::string op = req.value("op").toString().toStdString();
    Job job(req.value("id").toVariant().toLongLong());
    if (op == "delete") {
        std::string p = path_arg(req, "path");
        guard(p);
        job.total = job.count(p);
        job.remove(p);
    } else if (op == "copy" || op == "move") {
        std::string src = path_arg(req, "src"), dst = path_arg(req, "dst");
        guard(dst);
        job.total = job.count(src) * (op == "move" ? 2 : 1);
        bool merge = req.value("merge").toBool();
        if (op == "copy")
            job.copy(src, dst, merge);
        else
            job.move(src, dst, merge);
    } else if (op == "rename") {
        std::string src = path_arg(req, "src"), dst = path_arg(req, "dst");
        guard(src);
        if (lexists(dst))
            throw Failure(basename(dst) + " already exists");
        if (rename(src.c_str(), dst.c_str()) != 0)
            fail_errno();
    } else if (op == "mkdir") {
        std::string p = path_arg(req, "path");
        std::string cur;
        size_t i = 1;
        while (i <= p.size()) {   // os.makedirs: parents as needed, the last one must be new
            size_t j = p.find('/', i);
            if (j == std::string::npos)
                j = p.size();
            cur = p.substr(0, j);
            bool last = j == p.size();
            if (mkdir(cur.c_str(), 0777) != 0 && (last || errno != EEXIST))
                fail_errno();
            i = j + 1;
        }
    } else if (op == "touch") {
        std::string p = path_arg(req, "path");
        int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd < 0)
            fail_errno();
        close(fd);
    } else if (op == "copyfile") {
        std::string dst = path_arg(req, "dst");
        if (lexists(dst))
            throw Failure(basename(dst) + " already exists");
        copyfile_raw(path_arg(req, "src"), dst, O_TRUNC);
    } else if (op == "symlink") {
        std::string target = req.value("target").toString().toStdString();
        if (symlink(target.c_str(), path_arg(req, "link").c_str()) != 0)
            fail_errno();
    } else if (op == "hardlink") {
        if (link(path_arg(req, "target").c_str(), path_arg(req, "link").c_str()) != 0)
            fail_errno();
    } else if (op == "write") {
        std::string p = path_arg(req, "path");
        QByteArray text = req.value("text").toString().toUtf8();
        int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd < 0)
            fail_errno();
        ssize_t w = write(fd, text.constData(), size_t(text.size()));
        close(fd);
        if (w != text.size())
            fail_errno();
        int mode = req.contains("mode") ? req.value("mode").toInt() : 0644;
        if (chmod(p.c_str(), mode_t(mode)) != 0)
            fail_errno();
    } else if (op == "chmod") {
        std::string p = path_arg(req, "path");
        guard(p);
        if (chmod(p.c_str(), mode_t(req.value("mode").toInt()) & 07777) != 0)
            fail_errno();
    } else {
        throw Failure("unknown operation '" + op + "'");
    }
}

static std::mutex jobs_lock;
static std::condition_variable jobs_cv;
static std::deque<QJsonObject> jobs;

static void worker()
{
    for (;;) {
        QJsonObject req;
        {
            std::unique_lock<std::mutex> g(jobs_lock);
            jobs_cv.wait(g, [] { return !jobs.empty(); });
            req = jobs.front();
            jobs.pop_front();
        }
        qint64 id = req.value("id").toVariant().toLongLong();
        try {
            handle(req);
            send(QJsonObject{{"id", id}, {"ok", true}});
        } catch (const Cancelled &) {
            send(QJsonObject{{"id", id}, {"ok", false}, {"error", "cancelled"}, {"cancelled", true}});
        } catch (const std::exception &e) {
            send(QJsonObject{{"id", id}, {"ok", false}, {"error", QString::fromStdString(e.what())}});
        }
        std::lock_guard<std::mutex> g(cancel_lock);
        cancelled.erase(id);
    }
}

int main()
{
    umask(022);
    std::thread(worker).detach();
    send(QJsonObject{{"hello", true}, {"uid", int(getuid())}, {"pid", int(getpid())}});
    std::string line;
    while (std::getline(std::cin, line)) {
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(line));
        if (!doc.isObject())
            continue;
        QJsonObject req = doc.object();
        if (req.value("op").toString() == "cancel") {
            std::lock_guard<std::mutex> g(cancel_lock);
            cancelled.insert(req.value("id").toVariant().toLongLong());
        } else if (req.contains("id") && req.contains("op")) {
            std::lock_guard<std::mutex> g(jobs_lock);
            jobs.push_back(req);
            jobs_cv.notify_one();
        }
    }
    _exit(0);   // stdin closed: the session is over; stop whatever is running
}
