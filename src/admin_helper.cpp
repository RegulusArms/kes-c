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
//
// Safety: the files being worked on can change while root works on them (another user, or a program, swapping a
// folder for a symlink). So the helper never trusts a path string:
// - Every path is walked one folder at a time from "/", each opened without following symlinks (walk()). A symlink
//   on the way is followed only if root controls it: owned by root, in a folder only root can write to (system links
//   such as /lib → usr/lib). Any other symlink is refused. Kestrel resolves the user's own symlinked folders before
//   sending a request (admin.cpp), so those reach the helper as real paths.
// - The operation then works relative to the opened folder (mkdirat, openat, unlinkat, renameat…), on the name
//   itself: a symlink there is the link, never what it points to.
// - Recursive copy and delete go folder by folder through open descriptors, and stop if anything changed under them.
// - One policy, POLICY below, says which paths an operation may delete, replace or change: never the protected
//   folders, top-level folders (/data) or home folders themselves (/home/name). It's checked on the real path walked.

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
#include <map>
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

// What each operation does to each of its paths. "delete", "replace" and "change" are refused for protected,
// top-level and home folders; "create" makes a new name only (it fails if the name exists), so it's allowed anywhere;
// "read" only reads. Every path argument of every op is listed here, and handle() walks them all through check().
enum Kind { READ, CREATE, DELETE, REPLACE, CHANGE };
static const std::map<std::string, std::vector<std::pair<const char *, Kind>>> POLICY = {
    {"delete", {{"path", DELETE}}},
    {"copy", {{"src", READ}, {"dst", REPLACE}}},
    {"move", {{"src", DELETE}, {"dst", REPLACE}}},
    {"rename", {{"src", DELETE}, {"dst", CREATE}}},
    {"mkdir", {{"path", CREATE}}},
    {"touch", {{"path", CREATE}}},
    {"copyfile", {{"src", READ}, {"dst", CREATE}}},
    {"symlink", {{"link", CREATE}}},
    {"hardlink", {{"target", READ}, {"link", CREATE}}},
    {"write", {{"path", CREATE}}},
    {"chmod", {{"path", CHANGE}}},
};

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

static std::vector<std::string> components(const std::string &p)   // normalised: no "", "." or ".."
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
    return out;
}

static std::string dirname(const std::string &p)
{
    size_t i = p.rfind('/');
    return i == 0 || i == std::string::npos ? "/" : p.substr(0, i);
}

// an open descriptor, closed when it goes out of scope
class Fd {
public:
    explicit Fd(int fd = -1) : fd(fd) {}
    ~Fd()
    {
        if (fd >= 0)
            close(fd);
    }
    Fd(Fd &&o) noexcept : fd(o.fd) { o.fd = -1; }
    Fd &operator=(Fd &&o) noexcept
    {
        std::swap(fd, o.fd);
        return *this;
    }
    Fd(const Fd &) = delete;
    int get() const { return fd; }

private:
    int fd;
};

static bool stat_at(int dirfd, const std::string &name, struct stat *st)   // lstat, relative to dirfd
{
    return fstatat(dirfd, name.c_str(), st, AT_SYMLINK_NOFOLLOW) == 0;
}

static bool same_file(const struct stat &a, const struct stat &b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }

// Open the folder `name` in dirfd, which must still be the folder `st` described (not swapped for a symlink or
// another folder since): O_NOFOLLOW refuses a symlink, and the inode is compared.
static Fd open_dir_at(int dirfd, const std::string &name, const struct stat &st, int flags = O_RDONLY)
{
    Fd fd(openat(dirfd, name.c_str(), flags | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (fd.get() < 0)
        fail_errno();
    struct stat now;
    if (fstat(fd.get(), &now) != 0 || !same_file(now, st))
        throw Failure(name + " changed while it was being worked on");
    return fd;
}

// The folder a path's last part is in, opened safely (see the top of this file), and that last part.
struct Walk {
    Fd dir;
    std::string name;   // the last part ("" for "/")
    std::string path;   // the real path walked: no symlinks, no ".."
};

static bool trusted_link(int dirfd, const struct stat &link)   // a symlink only root can have made or changed
{
    struct stat dir;
    return link.st_uid == 0 && fstat(dirfd, &dir) == 0 && dir.st_uid == 0 && !(dir.st_mode & (S_IWGRP | S_IWOTH));
}

static Walk walk(std::string path, bool make_parents = false)
{
    for (int links = 0;; ++links) {
        if (links > 40)
            fail_errno(ELOOP);
        std::vector<std::string> parts = components(path);
        Fd dir(open("/", O_PATH | O_DIRECTORY | O_CLOEXEC));
        if (dir.get() < 0)
            fail_errno();
        std::string real;
        bool restarted = false;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            const std::string &c = parts[i];
            struct stat st;
            if (!stat_at(dir.get(), c, &st)) {
                if (errno != ENOENT || !make_parents)
                    fail_errno();
                if (mkdirat(dir.get(), c.c_str(), 0777) != 0 && errno != EEXIST)
                    fail_errno();
                if (!stat_at(dir.get(), c, &st))
                    fail_errno();
            }
            if (S_ISLNK(st.st_mode)) {
                if (!trusted_link(dir.get(), st))
                    throw Failure("refusing to follow the symlink " + real + "/" + c + " (it isn't owned by root)");
                char target[PATH_MAX];
                ssize_t n = readlinkat(dir.get(), c.c_str(), target, sizeof target - 1);
                if (n < 0)
                    fail_errno();
                target[n] = 0;
                std::string next = target[0] == '/' ? std::string(target) : real + "/" + target;
                for (size_t k = i + 1; k < parts.size(); ++k)
                    next += "/" + parts[k];
                path = next;
                restarted = true;
                break;
            }
            if (!S_ISDIR(st.st_mode))
                fail_errno(ENOTDIR);
            dir = open_dir_at(dir.get(), c, st, O_PATH);
            real += "/" + c;
        }
        if (restarted)
            continue;
        std::string name = parts.empty() ? std::string() : parts.back();
        return Walk{std::move(dir), name, parts.empty() ? "/" : real + "/" + name};
    }
}

static std::string path_arg(const QJsonObject &req, const char *key)
{
    QJsonValue v = req.value(key);
    std::string p = v.isString() ? v.toString().toStdString() : std::string();
    if (p.empty() || p[0] != '/')
        throw Failure("not an absolute path: " + p);
    return p;
}

// the policy (see POLICY): top-level folders (/usr, /data, …) and home folders themselves (/home/name) are never
// deleted, replaced or changed
static void check(const Walk &w, Kind kind)
{
    if (w.name.empty())
        throw Failure("refusing to work on /");
    if (kind == READ || kind == CREATE)
        return;
    std::string parent = dirname(w.path);
    if (PROTECTED.count(w.path) || parent == "/" || parent == "/home")
        throw Failure("refusing to delete, replace or change " + w.path);
}

static std::vector<std::string> list_dir(int dirfd)   // the names in an open folder
{
    int dup_fd = fcntl(dirfd, F_DUPFD_CLOEXEC, 0);
    DIR *d = dup_fd >= 0 ? fdopendir(dup_fd) : nullptr;
    if (!d) {
        if (dup_fd >= 0)
            close(dup_fd);
        fail_errno();
    }
    rewinddir(d);
    std::vector<std::string> out;
    while (struct dirent *e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            out.emplace_back(e->d_name);
    closedir(d);
    return out;
}

static void copy_data(int in, int out)
{
    std::vector<char> buf(1 << 20);
    for (;;) {
        ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            fail_errno();
        if (n == 0)
            return;
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(out, buf.data() + off, size_t(n - off));
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0)
                fail_errno();
            off += w;
        }
    }
}

// the permissions and times of an open file or folder (on a descriptor: never through a symlink)
static void copy_stat_fd(int fd, const struct stat &st)
{
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    futimens(fd, times);
    fchmod(fd, st.st_mode & 07777);
}

class Job {
public:
    explicit Job(qint64 id) : id(id) {}
    qint64 id;
    double done = 0, total = 0;

    void check_cancel()
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

    // how many items a tree has, for progress (only reads; never follows a symlink)
    double count(int dirfd, const std::string &name)
    {
        struct stat st;
        if (!stat_at(dirfd, name, &st))
            return 1;
        double n = 1;
        if (S_ISDIR(st.st_mode)) {
            check_cancel();
            try {
                Fd d = open_dir_at(dirfd, name, st);
                for (const std::string &e : list_dir(d.get()))
                    n += count(d.get(), e);
            } catch (const Failure &) {
            }
        }
        return n;
    }

    // delete `name` in dirfd, and everything in it, without leaving the filesystem it's on
    void remove(int dirfd, const std::string &name)
    {
        struct stat st;
        if (!stat_at(dirfd, name, &st))
            fail_errno();
        remove_tree(dirfd, name, st, st.st_dev);
        done += 1;
    }

    void copy(int src_dir, const std::string &src, int dst_dir, const std::string &dst, bool merge)
    {
        struct stat st, dst_st;
        if (!stat_at(src_dir, src, &st))
            fail_errno();
        bool dst_exists = stat_at(dst_dir, dst, &dst_st);
        if (dst_exists && S_ISDIR(dst_st.st_mode) && !S_ISDIR(st.st_mode))
            throw Failure(dst + " is a folder");   // a file or link never replaces a whole folder
        if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            ssize_t n = readlinkat(src_dir, src.c_str(), target, sizeof target - 1);
            if (n < 0)
                fail_errno();
            target[n] = 0;
            if (dst_exists)
                remove(dst_dir, dst);
            if (symlinkat(target, dst_dir, dst.c_str()) != 0)
                fail_errno();
            struct timespec times[2] = {st.st_atim, st.st_mtim};
            utimensat(dst_dir, dst.c_str(), times, AT_SYMLINK_NOFOLLOW);
        } else if (S_ISDIR(st.st_mode)) {
            Fd in = open_dir_at(src_dir, src, st);
            if (dst_exists && (!merge || !S_ISDIR(dst_st.st_mode))) {
                if (merge)
                    throw Failure(dst + " exists and isn't a folder");
                remove(dst_dir, dst);
                dst_exists = false;
            }
            if (!dst_exists && mkdirat(dst_dir, dst.c_str(), 0700) != 0)
                fail_errno();
            if (!stat_at(dst_dir, dst, &dst_st))
                fail_errno();
            Fd out = open_dir_at(dst_dir, dst, dst_st);
            for (const std::string &e : list_dir(in.get()))
                copy(in.get(), e, out.get(), e, merge);
            copy_stat_fd(out.get(), st);
        } else if (S_ISREG(st.st_mode)) {
            check_cancel();
            Fd in(openat(src_dir, src.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
            struct stat now;
            if (in.get() < 0)
                fail_errno();
            if (fstat(in.get(), &now) != 0 || !same_file(now, st))
                throw Failure(src + " changed while it was being worked on");
            if (dst_exists)
                remove(dst_dir, dst);   // a new file: never writes through a link to another one
            Fd out(openat(dst_dir, dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            if (out.get() < 0)
                fail_errno();
            copy_data(in.get(), out.get());
            copy_stat_fd(out.get(), st);
        } else {
            throw Failure(src + " isn't a regular file, folder or link");
        }
        done += 1;
        report(src);
    }

    void move(const Walk &src, const Walk &dst, bool merge)
    {
        struct stat st, dst_dir_st;
        if (!merge && stat_at(src.dir.get(), src.name, &st) && fstat(dst.dir.get(), &dst_dir_st) == 0 &&
            st.st_dev == dst_dir_st.st_dev) {
            struct stat existing;
            if (stat_at(dst.dir.get(), dst.name, &existing))
                remove(dst.dir.get(), dst.name);
            if (renameat(src.dir.get(), src.name.c_str(), dst.dir.get(), dst.name.c_str()) == 0) {
                done = total;
                return;
            }
        }
        copy(src.dir.get(), src.name, dst.dir.get(), dst.name, merge);
        remove(src.dir.get(), src.name);
    }

private:
    double last = 0;

    void remove_tree(int dirfd, const std::string &name, const struct stat &st, dev_t dev)
    {
        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev != dev)
                throw Failure(name + " is on another drive (a mount point); not deleting it");
            {
                Fd d = open_dir_at(dirfd, name, st);
                for (const std::string &e : list_dir(d.get())) {
                    check_cancel();
                    struct stat est;
                    if (!stat_at(d.get(), e, &est))
                        fail_errno();
                    remove_tree(d.get(), e, est, dev);
                    done += 1;
                    report(e);
                }
            }
            if (unlinkat(dirfd, name.c_str(), AT_REMOVEDIR) != 0)
                fail_errno();
        } else if (unlinkat(dirfd, name.c_str(), 0) != 0) {   // a file or a symlink: the name itself
            fail_errno();
        }
    }
};

static Fd open_new_file(const Walk &w)
{
    Fd fd(openat(w.dir.get(), w.name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666));
    if (fd.get() < 0)
        fail_errno();
    return fd;
}

static void handle(const QJsonObject &req)
{
    std::string op = req.value("op").toString().toStdString();
    auto rules = POLICY.find(op);
    if (rules == POLICY.end())
        throw Failure("unknown operation '" + op + "'");
    // every path argument, walked and checked against the policy before anything is done
    std::map<std::string, Walk> w;
    for (const auto &[key, kind] : rules->second) {
        Walk walked = walk(path_arg(req, key), op == "mkdir");
        check(walked, kind);
        w.emplace(key, std::move(walked));
    }
    Job job(req.value("id").toVariant().toLongLong());
    if (op == "delete") {
        const Walk &p = w.at("path");
        job.total = job.count(p.dir.get(), p.name);
        job.remove(p.dir.get(), p.name);
    } else if (op == "copy" || op == "move") {
        const Walk &src = w.at("src"), &dst = w.at("dst");
        job.total = job.count(src.dir.get(), src.name) * (op == "move" ? 2 : 1);
        bool merge = req.value("merge").toBool();
        if (op == "copy")
            job.copy(src.dir.get(), src.name, dst.dir.get(), dst.name, merge);
        else
            job.move(src, dst, merge);
    } else if (op == "rename") {
        const Walk &src = w.at("src"), &dst = w.at("dst");
        if (renameat2(src.dir.get(), src.name.c_str(), dst.dir.get(), dst.name.c_str(), RENAME_NOREPLACE) != 0) {
            if (errno == EEXIST)
                throw Failure(dst.name + " already exists");
            if (errno != EINVAL)   // a filesystem without RENAME_NOREPLACE: check, then rename
                fail_errno();
            struct stat st;
            if (stat_at(dst.dir.get(), dst.name, &st))
                throw Failure(dst.name + " already exists");
            if (renameat(src.dir.get(), src.name.c_str(), dst.dir.get(), dst.name.c_str()) != 0)
                fail_errno();
        }
    } else if (op == "mkdir") {   // the parents were made by walk(); the last one must be new
        const Walk &p = w.at("path");
        if (mkdirat(p.dir.get(), p.name.c_str(), 0777) != 0)
            fail_errno();
    } else if (op == "touch") {
        open_new_file(w.at("path"));
    } else if (op == "copyfile") {
        const Walk &src = w.at("src"), &dst = w.at("dst");
        struct stat st;
        if (stat_at(dst.dir.get(), dst.name, &st))
            throw Failure(dst.name + " already exists");
        Fd in(openat(src.dir.get(), src.name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (in.get() < 0)
            fail_errno();
        Fd out = open_new_file(dst);
        copy_data(in.get(), out.get());
    } else if (op == "symlink") {
        std::string target = req.value("target").toString().toStdString();
        const Walk &link = w.at("link");
        if (symlinkat(target.c_str(), link.dir.get(), link.name.c_str()) != 0)
            fail_errno();
    } else if (op == "hardlink") {
        const Walk &target = w.at("target"), &link = w.at("link");
        if (linkat(target.dir.get(), target.name.c_str(), link.dir.get(), link.name.c_str(), 0) != 0)
            fail_errno();
    } else if (op == "write") {
        QByteArray text = req.value("text").toString().toUtf8();
        Fd fd = open_new_file(w.at("path"));
        ssize_t n = write(fd.get(), text.constData(), size_t(text.size()));
        if (n != text.size())
            fail_errno();
        int mode = req.contains("mode") ? req.value("mode").toInt() : 0644;
        if (fchmod(fd.get(), mode_t(mode) & 07777) != 0)
            fail_errno();
    } else if (op == "chmod") {
        // the file itself, opened without following a symlink; chmod through /proc/self/fd changes that inode
        const Walk &p = w.at("path");
        Fd fd(openat(p.dir.get(), p.name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
        struct stat st;
        if (fd.get() < 0 || fstat(fd.get(), &st) != 0)
            fail_errno();
        if (S_ISLNK(st.st_mode))
            throw Failure("refusing to change the permissions of a symlink: " + p.path);
        std::string proc_path = "/proc/self/fd/" + std::to_string(fd.get());
        if (chmod(proc_path.c_str(), mode_t(req.value("mode").toInt()) & 07777) != 0)
            fail_errno();
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
