#include "util.h"

#include "atc.h"

#include "proc.h"
#include "uwp.h"

#include <QApplication>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QPalette>
#include <QProcess>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImageReader>
#include <QMimeDatabase>
#include <QMutex>
#include <QRegularExpression>
#include <QStandardPaths>

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

// ---------------------------------------------------------------- errors

bool OSError::permission() const { return code == EACCES || code == EPERM; }

QString errno_text(int code, const QString &path)
{
    QString s = QString("[Errno %1] %2").arg(code).arg(QString::fromLocal8Bit(std::strerror(code)));
    if (!path.isNull())
        s += QString(": '%1'").arg(path);
    return s;
}

void throw_errno(const QString &path, int code)
{
    int e = code >= 0 ? code : errno;
    if (e == EPIPE)
        throw BrokenPipe(e, errno_text(e, path));
    throw OSError(e, errno_text(e, path));
}

namespace util {

const char *APP_NAME = "Kestrel Explorer";
const char *APP_ID = "kestrel-explorer";
const char *APP_COMMAND = "kes";
const QStringList LEGACY_NAMES = {"Folder Explorer"};
const char *LEGACY_ID = "folder-explorer";
const char *VERSION = KES_VERSION;

static QString env_or(const char *var, const QString &fallback)
{
    QByteArray v = qgetenv(var);
    return v.isEmpty() ? fallback : QString::fromLocal8Bit(v);
}

QString HOME()
{
    static const QString h = QDir::homePath();
    return h;
}
QString CONFIG_DIR() { return join(env_or("XDG_CONFIG_HOME", HOME() + "/.config"), APP_ID); }
QString CACHE_ROOT() { return env_or("XDG_CACHE_HOME", HOME() + "/.cache"); }
QString THUMB_DIR() { return join(CACHE_ROOT(), "thumbnails"); }
QString APP_CACHE() { return join(CACHE_ROOT(), APP_ID); }
QString TRASH_DIR() { return join(env_or("XDG_DATA_HOME", HOME() + "/.local/share"), "Trash"); }
QString GTK_BOOKMARKS() { return HOME() + "/.config/gtk-3.0/bookmarks"; }

const QSet<QString> VIDEO_EXTS = {".mp4", ".mkv", ".webm", ".mov", ".avi", ".m4v", ".wmv", ".flv", ".mpg",
                                  ".mpeg", ".3gp", ".ts", ".m2ts"};
// camera RAW formats (decoded by the kimageformats/LibRaw plugin)
const QSet<QString> RAW_EXTS = {".3fr", ".arw", ".crw", ".cr2", ".cr3", ".dcr", ".dng", ".erf", ".fff", ".iiq",
                                ".k25", ".kdc", ".mdc", ".mef", ".mos", ".mrw", ".nef", ".nrw", ".orf", ".pef",
                                ".raf", ".raw", ".rw2", ".rwl", ".sr2", ".srf", ".srw", ".x3f"};

bool explicit_conda_env()
{
    QByteArray env = qgetenv("CONDA_DEFAULT_ENV");
    return !env.isEmpty() && env != "base";
}

void prefer_system_environment()
{
    if (explicit_conda_env())
        return;
    // conda installs that aren't named in the environment variables (e.g. a PATH entry left by a shell profile)
    static const QRegularExpression conda_dir("/(ana|mini)conda\\d*(/|$)|/miniforge\\d*(/|$)|/mambaforge(/|$)|/micromamba(/|$)");
    QStringList roots;
    QByteArray prefix = qgetenv("CONDA_PREFIX");
    if (!prefix.isEmpty())
        roots << rstrip(QString::fromLocal8Bit(prefix), '/');
    for (const char *var : {"CONDA_EXE", "CONDA_PYTHON_EXE"}) {   // <root>/bin/conda
        QByteArray exe = qgetenv(var);
        if (!exe.isEmpty())
            roots << rstrip(dirname(dirname(QString::fromLocal8Bit(exe))), '/');
    }
    roots.removeAll(QString());
    auto is_conda = [&](const QString &p) {
        for (const QString &r : roots)
            if (p == r || p.startsWith(r + "/"))
                return true;
        return conda_dir.match(p).hasMatch();
    };
    for (const char *var : {"PATH", "LD_LIBRARY_PATH", "XDG_DATA_DIRS"}) {
        QByteArray value = qgetenv(var);
        if (value.isEmpty())
            continue;
        QStringList keep, conda;
        for (const QString &p : QString::fromLocal8Bit(value).split(':'))
            (!p.isEmpty() && is_conda(p) ? conda : keep) << p;
        if (!conda.isEmpty())
            qputenv(var, (keep + conda).join(':').toLocal8Bit());
    }
    for (const char *var : {"GSETTINGS_SCHEMA_DIR", "GIO_MODULE_DIR", "GIO_EXTRA_MODULES", "QT_PLUGIN_PATH",
                            "QT_QPA_PLATFORM_PLUGIN_PATH"}) {
        QByteArray value = qgetenv(var);
        if (value.isEmpty())
            continue;
        QStringList keep;
        for (const QString &p : QString::fromLocal8Bit(value).split(':'))
            if (!p.isEmpty() && !is_conda(p))
                keep << p;
        if (keep.isEmpty())
            qunsetenv(var);
        else
            qputenv(var, keep.join(':').toLocal8Bit());
    }
}

QSettings &settings()
{
    static QSettings s(APP_ID, APP_ID);
    return s;
}

// ---------------------------------------------------------------- paths

QByteArray enc(const QString &path) { return QFile::encodeName(path); }
QString dec(const char *path) { return QFile::decodeName(path); }

QString dirname(const QString &p)
{
    int i = p.lastIndexOf('/') + 1;
    QString head = p.left(i);
    if (!head.isEmpty() && head != QString(head.size(), '/'))
        head = rstrip(head, '/');
    return head;
}

QString basename(const QString &p) { return p.mid(p.lastIndexOf('/') + 1); }

QString join(const QString &a, const QString &b)
{
    if (b.startsWith('/') || a.isEmpty())
        return b;
    if (a.endsWith('/'))
        return a + b;
    return a + '/' + b;
}

QString normpath(const QString &p)
{
    if (p.isEmpty())
        return ".";
    int initial = p.startsWith('/') ? 1 : 0;
    if (initial && p.startsWith("//") && !p.startsWith("///"))
        initial = 2;
    QStringList out;
    for (const QString &c : p.split('/')) {
        if (c.isEmpty() || c == ".")
            continue;
        if (c != ".." || (!initial && out.isEmpty()) || (!out.isEmpty() && out.last() == ".."))
            out << c;
        else if (!out.isEmpty())
            out.removeLast();
    }
    QString r = QString(initial, '/') + out.join('/');
    return r.isEmpty() ? "." : r;
}

QString abspath(const QString &p)
{
    if (p.startsWith('/'))
        return normpath(p);
    return normpath(join(QDir::currentPath(), p));
}

QString realpath(const QString &p)
{
    QByteArray e = enc(abspath(p));
    char buf[PATH_MAX];
    if (::realpath(e.constData(), buf))
        return dec(buf);
    QString a = abspath(p);
    if (a == "/")
        return a;
    return join(realpath(dirname(a)), basename(a));
}

QString relpath(const QString &p, const QString &start)
{
    QStringList s = abspath(start).split('/', Qt::SkipEmptyParts);
    QStringList t = abspath(p).split('/', Qt::SkipEmptyParts);
    int i = 0;
    while (i < s.size() && i < t.size() && s[i] == t[i])
        ++i;
    QStringList rel;
    for (int k = i; k < s.size(); ++k)
        rel << "..";
    rel += t.mid(i);
    return rel.isEmpty() ? "." : rel.join('/');
}

QString commonpath(const QStringList &paths)
{
    if (paths.isEmpty())
        return QString();
    bool abs = paths[0].startsWith('/');
    QList<QStringList> split;
    for (const QString &p : paths) {
        QStringList parts;
        for (const QString &c : p.split('/'))
            if (!c.isEmpty() && c != ".")
                parts << c;
        split << parts;
    }
    QStringList common = split[0];
    for (const QStringList &s : split) {
        int i = 0;
        while (i < common.size() && i < s.size() && common[i] == s[i])
            ++i;
        common = common.mid(0, i);
    }
    return (abs ? "/" : "") + common.join('/');
}

QString expanduser(const QString &p)
{
    if (p == "~")
        return HOME();
    if (p.startsWith("~/"))
        return HOME() + p.mid(1);
    return p;
}

QString rstrip(const QString &s, QChar c)
{
    int n = s.size();
    while (n > 0 && s[n - 1] == c)
        --n;
    return s.left(n);
}

QString strip(const QString &s, const QString &chars)
{
    int a = 0, b = s.size();
    while (a < b && chars.contains(s[a]))
        ++a;
    while (b > a && chars.contains(s[b - 1]))
        --b;
    return s.mid(a, b - a);
}

QString splitext_ext(const QString &p)
{
    int sep = p.lastIndexOf('/');
    int dot = p.lastIndexOf('.');
    if (dot > sep) {
        for (int i = sep + 1; i < dot; ++i)
            if (p[i] != '.')
                return p.mid(dot);
    }
    return QString();
}

QString ext_of(const QString &p) { return splitext_ext(p).toLower(); }

// ---------------------------------------------------------------- file system

bool stat_(const QString &p, struct stat &st) { return ::stat(enc(p).constData(), &st) == 0; }
bool lstat_(const QString &p, struct stat &st) { return ::lstat(enc(p).constData(), &st) == 0; }

bool exists(const QString &p)
{
    struct stat st;
    return stat_(p, st);
}

bool lexists(const QString &p)
{
    struct stat st;
    return lstat_(p, st);
}

bool isdir(const QString &p)
{
    struct stat st;
    return stat_(p, st) && S_ISDIR(st.st_mode);
}

bool isfile(const QString &p)
{
    struct stat st;
    return stat_(p, st) && S_ISREG(st.st_mode);
}

bool islink(const QString &p)
{
    struct stat st;
    return lstat_(p, st) && S_ISLNK(st.st_mode);
}

bool access(const QString &p, int mode) { return ::access(enc(p).constData(), mode) == 0; }

qint64 getsize(const QString &p)
{
    struct stat st;
    if (!stat_(p, st))
        throw_errno(p);
    return st.st_size;
}

QStringList listdir(const QString &p)
{
    DIR *d = ::opendir(enc(p).constData());
    if (!d)
        throw_errno(p);
    QStringList out;
    while (struct dirent *e = ::readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
            continue;
        out << dec(e->d_name);
    }
    ::closedir(d);
    return out;
}

void mkdir(const QString &p, mode_t mode)
{
    if (::mkdir(enc(p).constData(), mode) != 0)
        throw_errno(p);
}

void makedirs(const QString &p, bool exist_ok, mode_t mode)
{
    QString head = dirname(rstrip(p, '/'));
    if (!head.isEmpty() && head != p && !exists(head)) {
        try {
            makedirs(head, true, mode);
        } catch (const OSError &e) {
            if (e.code != EEXIST)
                throw;
        }
    }
    if (::mkdir(enc(p).constData(), mode) != 0) {
        int e = errno;
        if (!exist_ok || !isdir(p))
            throw_errno(p, e);
    }
}

void rename(const QString &a, const QString &b)
{
    if (::rename(enc(a).constData(), enc(b).constData()) != 0)
        throw_errno(a);
}

void unlink(const QString &p)
{
    if (::unlink(enc(p).constData()) != 0)
        throw_errno(p);
}

void rmdir(const QString &p)
{
    if (::rmdir(enc(p).constData()) != 0)
        throw_errno(p);
}

void symlink(const QString &target, const QString &link)
{
    if (::symlink(enc(target).constData(), enc(link).constData()) != 0)
        throw_errno(link);
}

void link(const QString &target, const QString &l)
{
    if (::link(enc(target).constData(), enc(l).constData()) != 0)
        throw_errno(l);
}

QString readlink(const QString &p)
{
    char buf[PATH_MAX];
    ssize_t n = ::readlink(enc(p).constData(), buf, sizeof buf - 1);
    if (n < 0)
        throw_errno(p);
    buf[n] = 0;
    return dec(buf);
}

void chmod(const QString &p, mode_t mode)
{
    if (::chmod(enc(p).constData(), mode) != 0)
        throw_errno(p);
}

void copystat(const QString &src, const QString &dst, bool follow)
{
    struct stat st;
    if (!(follow ? stat_(src, st) : lstat_(src, st)))
        throw_errno(src);
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    if (::utimensat(AT_FDCWD, enc(dst).constData(), times, follow ? 0 : AT_SYMLINK_NOFOLLOW) != 0)
        throw_errno(dst);
    if (!follow && S_ISLNK(st.st_mode))
        return;   // Linux can't chmod a link itself
    if (::chmod(enc(dst).constData(), st.st_mode & 07777) != 0)
        throw_errno(dst);
}

void copyfile(const QString &src, const QString &dst)
{
    int in = ::open(enc(src).constData(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        throw_errno(src);
    int out = ::open(enc(dst).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (out < 0) {
        int e = errno;
        ::close(in);
        throw_errno(dst, e);
    }
    std::vector<char> buf(1 << 20);
    for (;;) {
        ssize_t n = ::read(in, buf.data(), buf.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            int e = errno;
            ::close(in);
            ::close(out);
            throw_errno(src, e);
        }
        if (n == 0)
            break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = ::write(out, buf.data() + off, size_t(n - off));
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0) {
                int e = errno;
                ::close(in);
                ::close(out);
                throw_errno(dst, e);
            }
            off += w;
        }
    }
    ::close(in);
    if (::close(out) != 0)
        throw_errno(dst);
}

static void copytree(const QString &src, const QString &dst)
{
    struct stat st;
    if (!lstat_(src, st))
        throw_errno(src);
    if (S_ISLNK(st.st_mode)) {
        symlink(readlink(src), dst);
    } else if (S_ISDIR(st.st_mode)) {
        makedirs(dst, true);
        for (const QString &n : listdir(src))
            copytree(join(src, n), join(dst, n));
        copystat(src, dst);
    } else {
        copyfile(src, dst);
        copystat(src, dst);
    }
}

void move(const QString &src, const QString &dst)
{
    if (::rename(enc(src).constData(), enc(dst).constData()) == 0)
        return;
    if (errno != EXDEV)
        throw_errno(src);
    copytree(src, dst);
    struct stat st;
    if (lstat_(src, st) && S_ISDIR(st.st_mode)) {
        rmtree(src);
        if (lexists(src))
            throw_errno(src, EACCES);
    } else {
        unlink(src);
    }
}

void rmtree(const QString &p)
{
    struct stat st;
    if (!lstat_(p, st))
        return;
    if (S_ISDIR(st.st_mode)) {
        try {
            for (const QString &n : listdir(p))
                rmtree(join(p, n));
        } catch (const OSError &) {
        }
        ::rmdir(enc(p).constData());
    } else {
        ::unlink(enc(p).constData());
    }
}

void write_text(const QString &p, const QByteArray &data, bool exclusive)
{
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (exclusive ? O_EXCL : O_TRUNC);
    int fd = ::open(enc(p).constData(), flags, 0666);
    if (fd < 0)
        throw_errno(p);
    qsizetype off = 0;
    while (off < data.size()) {
        ssize_t w = ::write(fd, data.constData() + off, size_t(data.size() - off));
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0) {
            int e = errno;
            ::close(fd);
            throw_errno(p, e);
        }
        off += w;
    }
    ::close(fd);
}

QByteArray read_file(const QString &p, bool *ok)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) {
        if (ok)
            *ok = false;
        return QByteArray();
    }
    if (ok)
        *ok = true;
    return f.readAll();
}

static bool walk_impl(const QString &top, const WalkFn &fn, bool topdown)
{
    DIR *d = ::opendir(enc(top).constData());
    if (!d)
        return true;
    QStringList dirs, files, walk_into;
    while (struct dirent *e = ::readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
            continue;
        QString name = dec(e->d_name);
        bool is_dir = false, is_link = false;
        if (e->d_type == DT_DIR) {
            is_dir = true;
        } else if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) {
            struct stat st;
            QString full = join(top, name);
            if (e->d_type == DT_UNKNOWN) {
                if (lstat_(full, st))
                    is_link = S_ISLNK(st.st_mode);
                is_dir = !is_link && S_ISDIR(st.st_mode);
            } else {
                is_link = true;
            }
            if (is_link && stat_(full, st))
                is_dir = S_ISDIR(st.st_mode);
        }
        (is_dir ? dirs : files) << name;
        if (is_dir && !is_link)
            walk_into << name;
    }
    ::closedir(d);
    if (topdown) {
        if (!fn(top, dirs, files))
            return false;
        for (const QString &n : dirs) {
            QString p = join(top, n);
            if (islink(p))
                continue;
            if (!walk_impl(p, fn, topdown))
                return false;
        }
        return true;
    }
    for (const QString &n : walk_into)
        if (!walk_impl(join(top, n), fn, topdown))
            return false;
    return fn(top, dirs, files);
}

void walk(const QString &top, const WalkFn &fn, bool topdown) { walk_impl(top, fn, topdown); }

// ---------------------------------------------------------------- strings

QString file_uri(const QString &path)
{
    static const QByteArray safe = "/!$&'()*+,;=:@-._~";
    QByteArray raw = enc(abspath(path));
    QByteArray out = "file://";
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : raw) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || safe.contains(char(c))) {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return QString::fromLatin1(out);
}

QString uri_to_path(const QString &uri)
{
    static const QRegularExpression scheme("^([a-zA-Z][a-zA-Z0-9+.-]*):");
    auto m = scheme.match(uri);
    QString rest = uri;
    if (m.hasMatch()) {
        if (m.captured(1).toLower() != "file")
            return QString();
        rest = uri.mid(m.capturedLength());
        if (rest.startsWith("//")) {
            int slash = rest.indexOf('/', 2);
            rest = slash < 0 ? QString() : rest.mid(slash);
        }
    }
    int q = rest.indexOf('?');
    int h = rest.indexOf('#');
    if (m.hasMatch()) {
        int cut = q >= 0 ? q : h;
        if (h >= 0 && (cut < 0 || h < cut))
            cut = h;
        if (cut >= 0)
            rest = rest.left(cut);
    }
    return QString::fromUtf8(QByteArray::fromPercentEncoding(rest.toUtf8()));
}

QString md5(const QString &s)
{
    return QString::fromLatin1(QCryptographicHash::hash(enc(s), QCryptographicHash::Md5).toHex());
}

QString human_size(double n)
{
    const char *units[] = {"bytes", "kB", "MB", "GB", "TB", "PB"};
    for (int i = 0; i < 6; ++i) {
        if (n < 1000 || i == 5) {
            if (i == 0)
                return QString("%1 bytes").arg(qint64(n));
            return QString("%1 %2").arg(n, 0, 'f', 1).arg(units[i]);
        }
        n /= 1000;
    }
    return QString();
}

QString human_size(qint64 n) { return human_size(double(n)); }

QString group_digits(qint64 n)
{
    QString s = QString::number(n < 0 ? -n : n);
    for (int i = s.size() - 3; i > 0; i -= 3)
        s.insert(i, ',');
    return n < 0 ? "-" + s : s;
}

// natural_key: compare runs of digits by value and other text case-insensitively
bool natural_less(const QString &a, const QString &b)
{
    int i = 0, j = 0;
    const int na = a.size(), nb = b.size();
    while (true) {
        // text run
        int ia = i, jb = j;
        while (i < na && !a[i].isDigit())
            ++i;
        while (j < nb && !b[j].isDigit())
            ++j;
        QString ta = a.mid(ia, i - ia).toLower(), tb = b.mid(jb, j - jb).toLower();
        if (ta != tb)
            return ta < tb;
        bool ea = i >= na, eb = j >= nb;
        if (ea || eb)
            return ea && !eb;
        // number run
        ia = i;
        jb = j;
        while (i < na && a[i].isDigit())
            ++i;
        while (j < nb && b[j].isDigit())
            ++j;
        QString da, db;
        for (int k = ia; k < i; ++k)
            da += QChar('0' + a[k].digitValue());
        for (int k = jb; k < j; ++k)
            db += QChar('0' + b[k].digitValue());
        while (da.size() > 1 && da[0] == '0')
            da.remove(0, 1);
        while (db.size() > 1 && db[0] == '0')
            db.remove(0, 1);
        if (da.size() != db.size())
            return da.size() < db.size();
        if (da != db)
            return da < db;
        // after a number, Python's list has another (possibly empty) text element
        if (i >= na && j >= nb)
            return false;
    }
}

void natural_sort(QStringList &list) { std::stable_sort(list.begin(), list.end(), natural_less); }

QPair<QString, QString> split_ext(const QString &name)
{
    QString low = name.toLower();
    for (const char *e : {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst"}) {
        int n = int(std::strlen(e));
        if (low.endsWith(e) && name.size() > n)
            return {name.left(name.size() - n), name.right(n)};
    }
    QString ext = splitext_ext(name);
    QString stem = name.left(name.size() - ext.size());
    if (stem.isEmpty())
        return {name, QString()};
    return {stem, ext};
}

QString unique_path(const QString &directory, const QString &name, const QString &style)
{
    QString target = join(directory, name);
    if (!lexists(target))
        return target;
    auto [stem, ext] = split_ext(name);
    static const QRegularExpression suffix_re(R"( \((copy|copy \d+|\d+)\)$)");
    stem.remove(suffix_re);
    for (int i = 1;; ++i) {
        QString suffix;
        if (style == "copy")
            suffix = i == 1 ? QString(" (copy)") : QString(" (copy %1)").arg(i);
        else
            suffix = QString(" (%1)").arg(i + 1);
        target = join(directory, stem + suffix + ext);
        if (!lexists(target))
            return target;
    }
}

QString fmt_time(qint64 ts, const char *fmt)
{
    time_t t = time_t(ts);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[128];
    std::strftime(buf, sizeof buf, fmt, &tm);
    return QString::fromLocal8Bit(buf);
}

QStringList shlex_split(const QString &s, bool *ok, QString *err)
{
    QStringList out;
    QString cur;
    bool in_word = false;
    QChar quote;
    for (int i = 0; i < s.size(); ++i) {
        QChar c = s[i];
        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            } else if (quote == '"' && c == '\\' && i + 1 < s.size() &&
                       QString("\\\"$`\n").contains(s[i + 1])) {
                cur += s[++i];
            } else {
                cur += c;
            }
            continue;
        }
        if (c.isSpace()) {
            if (in_word) {
                out << cur;
                cur.clear();
                in_word = false;
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
            in_word = true;
        } else if (c == '\\') {
            in_word = true;
            if (i + 1 >= s.size()) {
                if (ok)
                    *ok = false;
                if (err)
                    *err = "No escaped character";
                return {};
            }
            cur += s[++i];
        } else {
            cur += c;
            in_word = true;
        }
    }
    if (!quote.isNull()) {
        if (ok)
            *ok = false;
        if (err)
            *err = "No closing quotation";
        return {};
    }
    if (in_word)
        out << cur;
    if (ok)
        *ok = true;
    return out;
}

QString shlex_quote(const QString &s)
{
    if (s.isEmpty())
        return "''";
    static const QRegularExpression unsafe(R"([^\w@%+=:,./-])", QRegularExpression::UseUnicodePropertiesOption);
    if (!unsafe.match(s).hasMatch())
        return s;
    QString q = s;
    q.replace("'", "'\"'\"'");
    return "'" + q + "'";
}

QString shlex_join(const QStringList &args)
{
    QStringList q;
    for (const QString &a : args)
        q << shlex_quote(a);
    return q.join(' ');
}

// ---------------------------------------------------------------- file types

const QSet<QString> &image_exts()
{
    static const QSet<QString> exts = [] {
        QSet<QString> s;
        const QSet<QString> skip = {"ani", "cur", "pdf"};
        for (const QByteArray &f : QImageReader::supportedImageFormats()) {
            QString n = QString::fromLatin1(f).toLower();
            if (!skip.contains(n))
                s << "." + n;
        }
        return s;
    }();
    return exts;
}

bool is_image(const QString &path) { return image_exts().contains(ext_of(path)); }
bool is_raw(const QString &path) { return RAW_EXTS.contains(ext_of(path)); }
bool is_video(const QString &path) { return VIDEO_EXTS.contains(ext_of(path)); }

bool is_device_path(const QString &path)
{
    static const QRegularExpression re("^/run/user/\\d+/gvfs/(afc|gphoto2|mtp):");
    return re.match(path).hasMatch();
}

bool needs_local_copy(const QString &path)
{
    static const QRegularExpression re("^/run/user/\\d+/gvfs/gphoto2:");
    return re.match(path).hasMatch();
}

QString device_uri(const QString &path)
{
    QString out;
    GVolumeMonitor *mon = g_volume_monitor_get();
    GList *mounts = g_volume_monitor_get_mounts(mon);
    for (GList *l = mounts; l && out.isEmpty(); l = l->next) {
        GFile *root = g_mount_get_root(G_MOUNT(l->data));
        char *rp = g_file_get_path(root);
        QString r = rp ? QString::fromUtf8(rp) : QString();
        g_free(rp);
        if (!r.isEmpty() && path.startsWith(r + "/")) {
            GFile *f = g_file_resolve_relative_path(root, path.mid(r.size() + 1).toUtf8().constData());
            char *u = g_file_get_uri(f);
            out = QString::fromUtf8(u);
            g_free(u);
            g_object_unref(f);
        }
        g_object_unref(root);
    }
    g_list_free_full(mounts, g_object_unref);
    g_object_unref(mon);
    return out;
}

static QMimeDatabase &mime_db()
{
    static QMimeDatabase db;
    return db;
}

QMimeType mime_for(const QString &path, int is_dir)
{
    if (is_dir < 0 ? isdir(path) : is_dir)
        return mime_db().mimeTypeForName("inode/directory");
    return mime_db().mimeTypeForFile(path, QMimeDatabase::MatchExtension);
}

QMimeType mime_for_content(const QString &path) { return mime_db().mimeTypeForFile(path, QMimeDatabase::MatchDefault); }

// ---------------------------------------------------------------- icons

QHash<QString, QString> SPECIAL_DIR_ICONS;

static QHash<QString, QString> &xdg_dirs()
{
    static QHash<QString, QString> d;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        QString cfg = join(env_or("XDG_CONFIG_HOME", HOME() + "/.config"), "user-dirs.dirs");
        bool ok = false;
        QByteArray text = read_file(cfg, &ok);
        static const QRegularExpression re(R"re(^XDG_(\w+)_DIR="(.*)")re");
        for (const QByteArray &line : text.split('\n')) {
            auto m = re.match(QString::fromUtf8(line).trimmed());
            if (m.hasMatch())
                d[m.captured(1)] = m.captured(2).replace("$HOME", HOME());
        }
    }
    return d;
}

QString xdg_user_dir(const QString &key)
{
    static const QHash<QString, QString> defaults = {
        {"DESKTOP", "Desktop"}, {"DOCUMENTS", "Documents"}, {"DOWNLOAD", "Downloads"}, {"MUSIC", "Music"},
        {"PICTURES", "Pictures"}, {"VIDEOS", "Videos"}, {"TEMPLATES", "Templates"}, {"PUBLICSHARE", "Public"}};
    QString v = xdg_dirs().value(key);
    if (!v.isEmpty())
        return v;
    QString d = defaults.value(key);
    if (d.isEmpty())
        d = key.left(1).toUpper() + key.mid(1).toLower();
    return join(HOME(), d);
}

const QHash<QString, QString> &special_dir_icons()
{
    if (SPECIAL_DIR_ICONS.isEmpty()) {
        const QList<QPair<QString, QString>> names = {
            {"DESKTOP", "user-desktop"}, {"DOCUMENTS", "folder-documents"}, {"DOWNLOAD", "folder-download"},
            {"MUSIC", "folder-music"}, {"PICTURES", "folder-pictures"}, {"VIDEOS", "folder-videos"},
            {"TEMPLATES", "folder-templates"}, {"PUBLICSHARE", "folder-publicshare"}};
        for (const auto &[key, icon] : names) {
            QString p = xdg_user_dir(key);
            if (!p.isEmpty() && p != HOME())
                SPECIAL_DIR_ICONS[p] = icon;
        }
        SPECIAL_DIR_ICONS[HOME()] = "user-home";
        SPECIAL_DIR_ICONS[join(TRASH_DIR(), "files")] = "user-trash";
    }
    return SPECIAL_DIR_ICONS;
}

QIcon theme_icon(const QStringList &names)
{
    static QHash<QString, QIcon> cache;
    QString key = names.join('\x1f');
    auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return *it;
    QIcon icon;
    for (const QString &n : names) {
        if (!n.isEmpty() && QIcon::hasThemeIcon(n)) {
            icon = QIcon::fromTheme(n);
            break;
        }
    }
    cache.insert(key, icon);
    return icon;
}

QIcon icon_for_path(const QString &path, int is_dir)
{
    if (is_dir < 0)
        is_dir = isdir(path);
    if (is_dir)
        return theme_icon({special_dir_icons().value(path, "folder"), "folder"});
    QMimeType m = mime_for(path, 0);
    return theme_icon({m.iconName(), m.genericIconName(), "text-x-generic"});
}

bool has_schema_key(const QString &schema, const QString &key)
{
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *s = src ? g_settings_schema_source_lookup(src, schema.toUtf8().constData(), TRUE) : nullptr;
    if (!s)
        return false;
    bool ok = key.isEmpty() || g_settings_schema_has_key(s, key.toUtf8().constData());
    g_settings_schema_unref(s);
    return ok;
}

QString desktop_schema(const QString &gnome_schema)
{
    QStringList desktops = QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP")).toLower().split(':');
    if (desktops.contains("x-cinnamon") && gnome_schema.startsWith("org.gnome.")) {
        QString cinnamon = "org.cinnamon." + gnome_schema.mid(10);
        if (has_schema_key(cinnamon))
            return cinnamon;
    }
    return gnome_schema;
}

void setup_icon_theme()
{
    QStringList paths = QIcon::themeSearchPaths();
    QStringList data_dirs = env_or("XDG_DATA_DIRS", "/usr/local/share:/usr/share").split(':');
    data_dirs.prepend(join(HOME(), ".local/share"));
    for (const QString &d : data_dirs) {
        QString p = join(d, "icons");
        if (isdir(p) && !paths.contains(p))
            paths << p;
    }
    QIcon::setThemeSearchPaths(paths);
    if (QIcon::themeName().isEmpty() || QIcon::themeName() == "hicolor") {
        QString theme = "Adwaita";
        auto r = proc::run({"gsettings", "get", desktop_schema("org.gnome.desktop.interface"), "icon-theme"}, 2000);
        QString out = strip(QString::fromUtf8(r.out).trimmed(), "'");
        if (r.rc == 0 && !out.isEmpty())
            theme = out;
        QIcon::setThemeName(theme);
    }
    QIcon::setFallbackThemeName("Adwaita");
}

// ---------------------------------------------------------------- theme

bool dark_theme() { return QGuiApplication::palette().color(QPalette::Window).lightness() < 128; }

QColor blend(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t));
}

QColor card_color()
{
    // the theme's base colour when it differs from the window's (most light themes); otherwise a shade towards the text
    QPalette pal = QGuiApplication::palette();
    QColor base = pal.color(QPalette::Base), window = pal.color(QPalette::Window);
    if (std::abs(base.lightness() - window.lightness()) >= 8)
        return base;
    return blend(window, pal.color(QPalette::Text), 0.05);
}

QColor card_border()
{
    QPalette pal = QGuiApplication::palette();
    return blend(pal.color(QPalette::Window), pal.color(QPalette::Text), 0.14);
}

QColor error_color() { return QColor(dark_theme() ? "#ff7b63" : "#c01c28"); }   // GNOME's error colours

QColor accent_color() { return QGuiApplication::palette().color(QPalette::Highlight); }

namespace {

// Hears the application's palette change: every widget gets ApplicationPaletteChange, this hidden one included.
class PaletteWatcher : public QWidget {
public:
    QList<QPair<QPointer<QObject>, std::function<void()>>> fns;

protected:
    bool event(QEvent *ev) override
    {
        if (ev->type() == QEvent::ApplicationPaletteChange && !queued) {
            queued = true;   // a theme switch can change the palette several times in a row
            QTimer::singleShot(0, this, [this]() {
                queued = false;
                apply();
            });
        }
        return QWidget::event(ev);
    }

private:
    bool queued = false;
    void apply()
    {
        for (QWidget *w : QApplication::allWidgets()) {
            QString ss = w->styleSheet();
            if (ss.contains("palette(")) {
                w->setStyleSheet(QString());
                w->setStyleSheet(ss);
            }
        }
        fns.removeIf([](const auto &f) { return f.first.isNull(); });
        auto now = fns;
        for (const auto &[owner, fn] : now)
            if (owner)
                fn();
    }
};

}  // namespace

void on_palette_change(QObject *owner, std::function<void()> fn)
{
    static PaletteWatcher *watcher = new PaletteWatcher;
    watcher->fns << qMakePair(QPointer<QObject>(owner), std::move(fn));
}

// ---------------------------------------------------------------- the GTK theme's colours (Qt < 6.5)

// prints the theme's named colours as JSON (GTK 3 reads the desktop's current theme when it starts)
const char *GTK_COLORS_SCRIPT = R"(import json, gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
ctx = Gtk.Window().get_style_context()
out = {}
for name in ("theme_bg_color", "theme_fg_color", "theme_base_color", "theme_text_color", "theme_selected_bg_color",
             "theme_selected_fg_color", "insensitive_fg_color", "link_color"):
    found, c = ctx.lookup_color(name)
    if found:
        out[name] = "#%02x%02x%02x" % (round(c.red * 255), round(c.green * 255), round(c.blue * 255))
print(json.dumps(out))
)";

bool needs_gtk_palette(const QString &qt_version)
{
    QStringList v = qt_version.split('.');
    return v.value(0).toInt() == 6 && v.value(1).toInt() < 5;
}

QPalette gtk_palette_from(const QJsonObject &colors, bool *ok)
{
    auto color = [&](const char *name, const QColor &fallback = QColor()) {
        QColor c(colors.value(name).toString());
        return c.isValid() ? c : fallback;
    };
    QColor bg = color("theme_bg_color"), fg = color("theme_fg_color"), sel = color("theme_selected_bg_color");
    *ok = bg.isValid() && fg.isValid() && sel.isValid();
    if (!*ok)
        return QPalette();
    QColor base = color("theme_base_color", bg), text = color("theme_text_color", fg);
    QColor sel_text = color("theme_selected_fg_color", QColor(sel.lightness() < 150 ? Qt::white : Qt::black));
    QColor disabled = color("insensitive_fg_color", blend(fg, bg, 0.5)), link = color("link_color", sel);
    QPalette p(bg, bg);   // derives the frame shades (light, mid, dark, shadow) from the background
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, fg);
    p.setColor(QPalette::Button, bg);
    p.setColor(QPalette::ButtonText, fg);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, blend(base, text, 0.04));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, blend(text, base, 0.45));
    p.setColor(QPalette::Highlight, sel);
    p.setColor(QPalette::HighlightedText, sel_text);
    p.setColor(QPalette::Link, link);
    p.setColor(QPalette::LinkVisited, link);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    for (QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, r, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, blend(sel, bg, 0.5));
    return p;
}

static void apply_gtk_palette(bool wait)
{
    // read the theme's colours with GTK in a helper process (Kestrel itself doesn't link GTK)
    auto *proc = new QProcess(qApp);
    auto apply = [proc]() {
        bool ok = false;
        QPalette p = gtk_palette_from(QJsonDocument::fromJson(proc->readAllStandardOutput()).object(), &ok);
        if (ok && p != QGuiApplication::palette())
            QApplication::setPalette(p);   // on_palette_change() then updates what was drawn in the old colours
        proc->deleteLater();
    };
    if (wait) {   // at startup: the first window opens in the theme's colours
        proc->start("/usr/bin/python3", {"-c", GTK_COLORS_SCRIPT});
        proc->waitForFinished(3000);
        apply();
        return;
    }
    QObject::connect(proc, &QProcess::finished, qApp, apply);
    QObject::connect(proc, &QProcess::errorOccurred, proc, &QObject::deleteLater);
    proc->start("/usr/bin/python3", {"-c", GTK_COLORS_SCRIPT});
}

void follow_gtk_theme()
{
    QString platform = QGuiApplication::platformName();
    if (!needs_gtk_palette(QString::fromLatin1(qVersion())) || (platform != "xcb" && platform != "wayland"))
        return;
    apply_gtk_palette(true);
    static QTimer *later = nullptr;   // the desktop changes several settings at once; GTK picks them up shortly after
    later = new QTimer(qApp);
    later->setSingleShot(true);
    later->setInterval(600);
    QObject::connect(later, &QTimer::timeout, qApp, []() { apply_gtk_palette(false); });
    QString schema = desktop_schema("org.gnome.desktop.interface");   // GNOME's, or Cinnamon's own
    if (!has_schema_key(schema))
        return;
    GSettings *s = g_settings_new(schema.toUtf8().constData());   // kept for the life of the app
    for (const char *key : {"gtk-theme", "color-scheme"})
        if (has_schema_key(schema, key))
            g_signal_connect(s, QString("changed::%1").arg(key).toUtf8().constData(),
                             G_CALLBACK(+[](GSettings *, gchar *, gpointer) { later->start(); }), nullptr);
}

// ---------------------------------------------------------------- applications (GIO)

AppRef app_ref(GAppInfo *app)
{
    if (!app)
        return AppRef();
    return AppRef(app, [](GAppInfo *a) { g_object_unref(a); });
}

QString app_name(const AppRef &app) { return app ? QString::fromUtf8(g_app_info_get_name(app.get())) : QString(); }

QString app_id(const AppRef &app)
{
    const char *id = app ? g_app_info_get_id(app.get()) : nullptr;
    return id ? QString::fromUtf8(id) : QString();
}

QIcon app_icon(const AppRef &app) { return app ? gicon_to_qicon(g_app_info_get_icon(app.get())) : QIcon(); }

QIcon gicon_to_qicon(GIcon *gicon)
{
    if (!gicon)
        return QIcon();
    if (G_IS_THEMED_ICON(gicon)) {
        QStringList names;
        for (const gchar *const *n = g_themed_icon_get_names(G_THEMED_ICON(gicon)); n && *n; ++n)
            names << QString::fromUtf8(*n);
        names << "application-x-executable";
        return theme_icon(names);
    }
    if (G_IS_FILE_ICON(gicon)) {
        char *p = g_file_get_path(g_file_icon_get_file(G_FILE_ICON(gicon)));
        QIcon icon(p ? QString::fromUtf8(p) : QString());
        g_free(p);
        return icon;
    }
    char *s = g_icon_to_string(gicon);
    QString str = s ? QString::fromUtf8(s) : QString();
    g_free(s);
    return str.startsWith('/') ? QIcon(str) : theme_icon({str, "application-x-executable"});
}

static QString take_gerror(GError *err)
{
    QString m = err ? QString::fromUtf8(err->message) : QString("failed");
    if (err)
        g_error_free(err);
    return m;
}

bool open_default(const QString &path)
{
    GError *err = nullptr;
    if (g_app_info_launch_default_for_uri(file_uri(path).toUtf8().constData(), nullptr, &err))
        return true;
    if (err)
        g_error_free(err);
    return proc::start_detached({"xdg-open", path});
}

QString content_type(const QString &path)
{
    GFile *f = g_file_new_for_path(enc(path).constData());
    GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE,
                                        nullptr, nullptr);
    g_object_unref(f);
    if (info) {
        const char *ct = g_file_info_get_content_type(info);
        QString r = ct ? QString::fromUtf8(ct) : QString();
        g_object_unref(info);
        if (!r.isEmpty())
            return r;
    }
    return mime_for_content(path).name();
}

static QList<AppRef> take_app_list(GList *list)
{
    QList<AppRef> out;
    for (GList *l = list; l; l = l->next)
        out << app_ref(G_APP_INFO(l->data));   // each element's reference moves into the AppRef
    g_list_free(list);
    return out;
}

static void sort_by_name(QList<AppRef> &apps)
{
    std::sort(apps.begin(), apps.end(),
              [](const AppRef &a, const AppRef &b) { return app_name(a).toLower() < app_name(b).toLower(); });
}

QPair<QList<AppRef>, QList<AppRef>> apps_for(const QString &path)
{
    QString ct = content_type(path);
    QList<AppRef> rec = take_app_list(g_app_info_get_recommended_for_type(ct.toUtf8().constData()));
    QSet<QString> ids;
    for (const AppRef &a : rec)
        ids << app_id(a);
    QList<AppRef> others;
    for (const AppRef &a : take_app_list(g_app_info_get_all()))
        if (g_app_info_should_show(a.get()) && !ids.contains(app_id(a)))
            others << a;
    sort_by_name(others);
    return {rec, others};
}

AppRef default_app_for_type(const QString &ct)
{
    return app_ref(g_app_info_get_default_for_type(ct.toUtf8().constData(), FALSE));
}

AppRef default_app(const QString &path) { return default_app_for_type(content_type(path)); }

QList<AppRef> apps_for_type(const QString &ct)
{
    QList<AppRef> apps;
    for (const AppRef &a : take_app_list(g_app_info_get_all_for_type(ct.toUtf8().constData())))
        if (g_app_info_should_show(a.get()))
            apps << a;
    sort_by_name(apps);
    return apps;
}

AppRef app_by_id(const QString &id)
{
    if (id.isEmpty())
        return AppRef();
    GDesktopAppInfo *a = g_desktop_app_info_new(id.toUtf8().constData());
    return a ? app_ref(G_APP_INFO(a)) : AppRef();
}

void launch_app(const AppRef &app, const QStringList &paths)
{
    GList *uris = nullptr;
    QList<QByteArray> keep;
    for (const QString &p : paths)
        keep << file_uri(p).toUtf8();
    for (const QByteArray &u : keep)
        uris = g_list_append(uris, (gpointer)u.constData());
    GError *err = nullptr;
    gboolean ok = g_app_info_launch_uris(app.get(), uris, nullptr, &err);
    g_list_free(uris);
    if (!ok)
        throw Error(take_gerror(err));
}

void set_default_for_type(const AppRef &app, const QString &ct)
{
    GError *err = nullptr;
    if (!g_app_info_set_as_default_for_type(app.get(), ct.toUtf8().constData(), &err))
        throw Error(take_gerror(err));
}

QString which_path(const QString &name) { return QStandardPaths::findExecutable(name); }
bool which(const QString &name) { return !which_path(name).isEmpty(); }

bool open_terminal(const QString &directory)
{
    const QList<QStringList> candidates = {
        {"ptyxis", "--new-window", "--working-directory=" + directory},
        {"gnome-terminal", "--working-directory=" + directory},
        {"kgx", "--working-directory=" + directory},
        {"konsole", "--workdir", directory},
        {"xfce4-terminal", "--working-directory=" + directory},
        {"x-terminal-emulator"},
    };
    for (const QStringList &cmd : candidates) {
        if (which(cmd[0])) {
            proc::start_detached(cmd, directory);
            return true;
        }
    }
    return false;
}

void set_wallpaper(const QString &path)
{
    // through UWP when it's installed (a new UWP profile with the image on every monitor), else the desktop's own
    if (uwp::set_wallpaper(path))
        return;
    QString uri = file_uri(path);
    QString schema = desktop_schema("org.gnome.desktop.background");
    for (const char *key : {"picture-uri", "picture-uri-dark"})   // Cinnamon has no -dark one
        if (has_schema_key(schema, key))
            proc::run({"gsettings", "set", schema, key, uri}, 5000);
}

void trash(const QString &path)
{
    GFile *f = g_file_new_for_path(enc(path).constData());
    GError *err = nullptr;
    gboolean ok = g_file_trash(f, nullptr, &err);
    g_object_unref(f);
    if (!ok) {
        int code = (err && err->domain == G_IO_ERROR && err->code == G_IO_ERROR_PERMISSION_DENIED) ? EACCES : EIO;
        throw OSError(code, take_gerror(err));
    }
}

void mark_trusted(const QString &path)
{
    GFile *f = g_file_new_for_path(enc(path).constData());
    g_file_set_attribute_string(f, "metadata::trusted", "true", G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    g_object_unref(f);
}

// ---------------------------------------------------------------- trash on every drive

// filesystems that never hold a trash folder, or that could hang when probed (network mounts)
static const QSet<QString> NO_TRASH_FS = {
    "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "ramfs", "cgroup", "cgroup2", "securityfs", "debugfs",
    "tracefs", "pstore", "bpf", "mqueue", "hugetlbfs", "configfs", "fusectl", "autofs", "squashfs", "overlay",
    "nsfs", "binfmt_misc", "efivarfs", "rpc_pipefs", "fuse.portal", "fuse.gvfsd-fuse", "fuse.snapfuse", "cifs",
    "smb3", "smbfs", "nfs", "nfs4", "fuse.sshfs", "sshfs", "davfs", "fuse.davfs2", "fuse.rclone", "9p", "afs",
    "ceph", "glusterfs"};

QStringList trash_dirs()
{
    // the home trash, plus the per-drive $topdir/.Trash-$uid and $topdir/.Trash/$uid folders
    uid_t uid = ::getuid();
    QStringList out;
    if (isdir(join(TRASH_DIR(), "files")))
        out << TRASH_DIR();
    QSet<QString> seen(out.begin(), out.end());
    bool ok = false;
    QByteArray mounts = read_file("/proc/self/mounts", &ok);
    for (const QByteArray &line : mounts.split('\n')) {
        QList<QByteArray> fields = line.split(' ');
        if (fields.size() < 3 || NO_TRASH_FS.contains(QString::fromUtf8(fields[2])))
            continue;
        QString top = QString::fromUtf8(fields[1]).replace("\\040", " ").replace("\\011", "\t").replace("\\134", "\\");
        for (const QString &d : {join(top, QString(".Trash-%1").arg(uid)), join(top, QString(".Trash/%1").arg(uid))}) {
            if (!seen.contains(d) && isdir(join(d, "files"))) {
                seen << d;
                out << d;
            }
        }
    }
    return out;
}

QString trash_root(const QString &path)
{
    QString files = dirname(path);
    if (basename(files) != "files")
        return QString();
    QString root = dirname(files);
    QString name = basename(root);
    if (root == TRASH_DIR() || name.startsWith(".Trash-") ||
        (basename(dirname(root)) == ".Trash" && name == QString::number(::getuid())))
        return root;
    return QString();
}

bool in_trash(const QString &path) { return !trash_root(path).isNull(); }

QString trash_info_path(const QString &trashed)
{
    QString root = trash_root(trashed);
    return root.isNull() ? QString() : join(join(root, "info"), basename(trashed) + ".trashinfo");
}

QString trash_original_path(const QString &trashed)
{
    QString info = trash_info_path(trashed);
    if (info.isNull())
        return QString();
    bool ok = false;
    QByteArray text = read_file(info, &ok);
    if (!ok)
        return QString();
    for (const QByteArray &line : text.split('\n')) {
        if (!line.startsWith("Path="))
            continue;
        QString orig = QString::fromUtf8(QByteArray::fromPercentEncoding(line.mid(5).trimmed()));
        if (orig.startsWith('/'))
            return orig;
        QString root = trash_root(trashed);
        QString top = basename(root).startsWith(".Trash-") ? dirname(root) : dirname(dirname(root));
        return join(top, orig);
    }
    return QString();
}

QList<QPair<QString, QString>> trashed_items()
{
    QList<QPair<QString, QString>> out;
    for (const QString &root : trash_dirs()) {
        QStringList entries;
        try {
            entries = listdir(join(root, "files"));
        } catch (const OSError &) {
            continue;
        }
        for (const QString &n : entries) {
            QString p = join(join(root, "files"), n);
            out << qMakePair(p, trash_original_path(p));
        }
    }
    return out;
}

bool trash_is_empty()
{
    for (const QString &root : trash_dirs()) {
        DIR *d = ::opendir(enc(join(root, "files")).constData());
        if (!d)
            continue;
        bool any = false;
        while (struct dirent *e = ::readdir(d)) {
            if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) {
                any = true;
                break;
            }
        }
        ::closedir(d);
        if (any)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------- bookmarks

static QString unquote(const QString &s) { return QString::fromUtf8(QByteArray::fromPercentEncoding(s.toUtf8())); }

QList<QPair<QString, QString>> read_bookmarks()
{
    QList<QPair<QString, QString>> out;
    bool ok = false;
    QByteArray text = read_file(GTK_BOOKMARKS(), &ok);
    if (!ok)
        return out;
    for (const QString &line : QString::fromUtf8(text).split('\n')) {
        if (line.trimmed().isEmpty())
            continue;
        int sp = line.indexOf(' ');
        QString uri = sp < 0 ? line : line.left(sp);
        QString label = sp < 0 ? QString() : line.mid(sp + 1).trimmed();
        QString target = uri_to_path(uri);
        if (target.isEmpty())
            target = uri;   // keep network bookmarks (smb://, sftp://) as URIs
        QString name = basename(rstrip(target, '/'));
        if (name.isEmpty())
            name = target;
        out << qMakePair(target, label.isEmpty() ? unquote(name) : label);
    }
    return out;
}

void write_bookmarks(const QList<QPair<QString, QString>> &items)
{
    makedirs(dirname(GTK_BOOKMARKS()), true);
    QStringList lines;
    for (const auto &[target, label] : items) {
        QString base = basename(rstrip(target, '/'));
        QString def = unquote(base.isEmpty() ? target : base);
        QString uri = target.startsWith('/') ? file_uri(target) : target;
        lines << uri + (label == def ? QString() : " " + label);
    }
    write_text(GTK_BOOKMARKS(), (lines.join('\n') + "\n").toUtf8());
    atc::announce("bookmarks");
}

QList<QUrl> url_list(const QStringList &paths)
{
    QList<QUrl> out;
    for (const QString &p : paths)
        out << QUrl::fromLocalFile(p);
    return out;
}

static QString desktop_entry_path()
{
    return join(join(env_or("XDG_DATA_HOME", HOME() + "/.local/share"), "applications"), QString(APP_ID) + ".desktop");
}

void ensure_desktop_entry()
{
    // On GNOME/Wayland the top bar and dock take the app's icon from the .desktop file whose name matches the
    // app id; the window icon is ignored. install.sh writes a fuller entry; this only fills the gap.
    QString entry = desktop_entry_path();
    if (exists(entry))
        return;
    QString launcher = QCoreApplication::applicationFilePath();
    try {
        makedirs(dirname(entry), true);
        write_text(entry, QString("[Desktop Entry]\nType=Application\nName=%1\nGenericName=File Manager\n"
                                  "Comment=Manage files, with archive, admin, permission and metadata tools built in\n"
                                  "Exec=%2 %U\nIcon=folder\nTerminal=false\n"
                                  "Categories=System;FileTools;FileManager;Viewer;\n"
                                  "MimeType=inode/directory;x-directory/normal;\nStartupWMClass=%3\n")
                                  .arg(APP_NAME, launcher, APP_ID)
                                  .toUtf8());
    } catch (const OSError &) {
    }
}

void migrate_legacy()
{
    // one-time move of settings/caches/desktop entry from the old "folder-explorer" name
    QString config_root = dirname(CONFIG_DIR());
    QString old_cfg = join(config_root, LEGACY_ID), new_cfg = CONFIG_DIR();
    try {
        if (isdir(old_cfg) && !exists(new_cfg)) {
            rename(old_cfg, new_cfg);
            QString old_conf = join(new_cfg, QString(LEGACY_ID) + ".conf");
            if (exists(old_conf))
                rename(old_conf, join(new_cfg, QString(APP_ID) + ".conf"));
        }
        QString old_cache = join(CACHE_ROOT(), LEGACY_ID);
        if (isdir(old_cache) && !exists(APP_CACHE()))
            rename(old_cache, APP_CACHE());
        QString old_desktop = join(dirname(desktop_entry_path()), QString(LEGACY_ID) + ".desktop");
        if (exists(old_desktop)) {
            QString text = QString::fromUtf8(read_file(old_desktop));
            bool ours = false;
            for (const QString &n : LEGACY_NAMES)
                ours = ours || text.contains(n);
            if (ours) {
                unlink(old_desktop);
                auto r = proc::run({"xdg-mime", "query", "default", "inode/directory"}, 5000);
                if (QString::fromUtf8(r.out).trimmed() == QString(LEGACY_ID) + ".desktop") {
                    ensure_desktop_entry();
                    proc::run({"xdg-mime", "default", QString(APP_ID) + ".desktop", "inode/directory"}, 5000);
                }
            }
        }
    } catch (const OSError &) {
    }
}

}  // namespace util
