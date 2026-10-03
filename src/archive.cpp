#include "archive.h"

#include "fileops.h"
#include "proc.h"

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>

#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

using namespace util;

namespace archive {

static const QStringList SYSTEM_PATH = {"/usr/local/bin", "/usr/bin", "/bin"};

QString tool(const QString &name)
{
    QString p = QStandardPaths::findExecutable(name, SYSTEM_PATH);
    return p.isEmpty() ? QStandardPaths::findExecutable(name) : p;
}

// path of a tool, or just its name (for previews of a command whose tool isn't installed)
static QString exe(const QString &name)
{
    QString t = tool(name);
    return t.isEmpty() ? name : t;
}

int cpu_count() { return std::max(1, QThread::idealThreadCount()); }

// ---------------------------------------------------------------- formats you can create

struct Compressor {
    int min, max, def;
    std::function<QStringList(int)> threads;   // threads switch builder, or empty
};

static const QMap<QString, Compressor> &compressors()
{
    static const QMap<QString, Compressor> c = {
        {"pigz", {1, 9, 6, [](int n) { return QStringList{"-p", QString::number(n)}; }}},
        {"gzip", {1, 9, 6, nullptr}},
        {"pbzip2", {1, 9, 9, [](int n) { return QStringList{QString("-p%1").arg(n)}; }}},
        {"lbzip2", {1, 9, 9, [](int n) { return QStringList{"-n", QString::number(n)}; }}},
        {"bzip2", {1, 9, 9, nullptr}},
        {"xz", {0, 9, 6, [](int n) { return QStringList{QString("-T%1").arg(n)}; }}},
        {"zstd", {1, 22, 3, [](int n) { return QStringList{QString("-T%1").arg(n)}; }}},
        {"plzip", {0, 9, 6, [](int n) { return QStringList{"-n", QString::number(n)}; }}},
        {"lzip", {0, 9, 6, nullptr}},
        {"lz4", {1, 12, 1, nullptr}},
    };
    return c;
}

// compressed-tar / single-file flavours: suffix -> compressors in order of preference
static const QList<QPair<QString, QStringList>> STREAMS = {
    {"gz", {"pigz", "gzip"}}, {"bz2", {"pbzip2", "lbzip2", "bzip2"}}, {"xz", {"xz"}},
    {"zst", {"zstd"}},        {"lz", {"plzip", "lzip"}},              {"lz4", {"lz4"}}};

static QStringList stream_tools(const QString &suf)
{
    for (const auto &[s, tools] : STREAMS)
        if (s == suf)
            return tools;
    return {};
}

static const QMap<QString, QString> STREAM_NAMES = {{"gz", "gzip"}, {"bz2", "bzip2"}, {"xz", "XZ"},
                                                    {"zst", "Zstandard"}, {"lz", "lzip"}, {"lz4", "LZ4"}};

const QMap<int, QString> LEVEL_NAMES_7Z = {{0, "Store"}, {1, "Fastest"}, {3, "Fast"},
                                           {5, "Normal"}, {7, "Maximum"}, {9, "Ultra"}};
static const QStringList METHODS_7Z = {"LZMA2", "LZMA", "PPMd", "BZip2", "Deflate", "Copy"};
static const QStringList METHODS_ZIP = {"Deflate", "Deflate64", "BZip2", "LZMA", "PPMd", "Copy"};

QList<Format> formats()
{
    QList<Format> out;
    Format f;
    f = Format{};
    f.id = "7z", f.label = "7z", f.ext = ".7z", f.tools = {"7z"}, f.levels = Levels{0, 9, 5};
    f.threads = f.password = f.encrypt_names = f.volumes = f.solid = true;
    f.methods = METHODS_7Z;
    out << f;
    f = Format{};
    f.id = "zip", f.label = "zip", f.ext = ".zip", f.tools = {"7z", "zip"}, f.levels = Levels{0, 9, 5};
    f.threads = f.password = f.volumes = true;
    f.methods = METHODS_ZIP;
    out << f;
    f = Format{};
    f.id = "rar", f.label = "rar", f.ext = ".rar", f.tools = {"rar"}, f.levels = Levels{0, 5, 3};
    f.threads = f.password = f.encrypt_names = f.volumes = f.solid = f.recovery = true;
    out << f;
    f = Format{};
    f.id = "zpaq", f.label = "zpaq (journaling, very high ratio)", f.ext = ".zpaq", f.tools = {"zpaq"};
    f.levels = Levels{1, 5, 1};
    f.threads = f.password = true;
    out << f;
    f = Format{};
    f.id = "tar", f.label = "tar (no compression)", f.ext = ".tar", f.tools = {"tar"};
    out << f;
    for (const auto &[suf, tools] : STREAMS) {
        f = Format{};
        f.id = "tar." + suf;
        f.label = QString("tar.%1 (%2)").arg(suf, STREAM_NAMES.value(suf));
        f.ext = ".tar." + suf;
        f.tools = tools;
        f.stream = suf;
        out << f;
    }
    for (const auto &[suf, tools] : STREAMS) {
        f = Format{};
        f.id = suf;
        f.label = QString("%1 (%2, one file only)").arg(suf, STREAM_NAMES.value(suf));
        f.ext = "." + suf;
        f.tools = tools;
        f.stream = suf;
        f.single = true;
        out << f;
    }
    for (Format &fm : out)
        for (const QString &t : fm.tools)
            if (!tool(t).isEmpty())
                fm.available << t;
    return out;
}

std::optional<Levels> tool_levels(const Format &fmt, const QString &tool_name)
{
    if (!fmt.stream.isEmpty()) {
        const Compressor &c = compressors()[tool_name];
        return Levels{c.min, c.max, c.def};
    }
    if (fmt.id == "zip" && tool_name == "zip")
        return Levels{0, 9, 6};
    return fmt.levels;
}

bool tool_threads(const Format &fmt, const QString &tool_name)
{
    if (!fmt.stream.isEmpty())
        return bool(compressors()[tool_name].threads);
    return fmt.threads && !(fmt.id == "zip" && tool_name == "zip");
}

QString install_hint(const QString &name)
{
    static const QMap<QString, QString> hints = {
        {"7z", "7zip"},     {"rar", "rar"},   {"zpaq", "zpaq"},     {"pigz", "pigz"}, {"pbzip2", "pbzip2"},
        {"lbzip2", "lbzip2"}, {"plzip", "plzip"}, {"lzip", "lzip"}, {"lz4", "lz4"},   {"zstd", "zstd"},
        {"xz", "xz-utils"}, {"unrar", "unrar"}, {"zip", "zip"},     {"gzip", "gzip"}, {"bzip2", "bzip2"}};
    return hints.value(name, name);
}

// ---------------------------------------------------------------- what you can extract

static const QStringList SEVENZ_EXTS = {".7z",  ".zip", ".jar", ".apk", ".xpi", ".iso",  ".cab",      ".arj",
                                        ".lzh", ".lha", ".wim", ".deb", ".rpm", ".cpio", ".xar",      ".dmg",
                                        ".vhd", ".vhdx", ".msi", ".chm", ".squashfs", ".001", ".zipx"};
static const QList<QPair<QString, QString>> TAR_EXTS = {
    {".tar", ""},      {".tar.gz", "gz"},  {".tgz", "gz"},      {".tar.bz2", "bz2"}, {".tbz2", "bz2"},
    {".tbz", "bz2"},   {".tar.xz", "xz"},  {".txz", "xz"},      {".tar.zst", "zst"}, {".tzst", "zst"},
    {".tar.lz", "lz"}, {".tlz", "lz"},     {".tar.lz4", "lz4"}};
static const QList<QPair<QString, QString>> SINGLE_EXTS = {{".gz", "gz"}, {".bz2", "bz2"}, {".xz", "xz"},
                                                           {".zst", "zst"}, {".lz", "lz"}, {".lz4", "lz4"}};

QPair<QString, QString> kind(const QString &path)
{
    QString low = path.toLower();
    static const QRegularExpression rnn("\\.r\\d\\d$");
    static const QRegularExpression split7("\\.7z\\.\\d{3}$|\\.zip\\.\\d{3}$");
    if (low.endsWith(".rar") || rnn.match(low).hasMatch())
        return {"rar", ""};
    if (low.endsWith(".zpaq"))
        return {"zpaq", ""};
    auto tars = TAR_EXTS;
    std::stable_sort(tars.begin(), tars.end(), [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
    for (const auto &[ext, suf] : tars)
        if (low.endsWith(ext))
            return {"tar", suf};
    for (const auto &[ext, suf] : SINGLE_EXTS)
        if (low.endsWith(ext))
            return {"single", suf};
    for (const QString &e : SEVENZ_EXTS)
        if (low.endsWith(e))
            return {"7z", ""};
    if (split7.match(low).hasMatch())
        return {"7z", ""};
    return {"", ""};
}

QString first_volume(const QString &path)
{
    // for a later part of a split archive (x.part3.rar, x.7z.003, x.zip.002, x.r05), the part to start from
    static const QRegularExpression part_re("\\.part0*\\d+\\.rar$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression num_re("\\.(7z|zip)\\.\\d{3}$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression r_re("\\.r\\d\\d$", QRegularExpression::CaseInsensitiveOption);
    QStringList cands;
    QRegularExpressionMatch m;
    if ((m = part_re.match(path)).hasMatch()) {
        static const QRegularExpression digits("\\d+");
        int width = digits.match(m.captured(0)).capturedLength();
        QString head = path.left(m.capturedStart());
        cands << head + QString(".part%1.rar").arg(1, width, 10, QChar('0')) << head + ".part1.rar";
    } else if ((m = num_re.match(path)).hasMatch()) {
        cands << path.left(m.capturedStart()) + QString(".%1.001").arg(m.captured(1));
    } else if ((m = r_re.match(path)).hasMatch()) {
        cands << path.left(m.capturedStart()) + ".rar";
    } else {
        return path;
    }
    for (const QString &c : cands)
        if (exists(c))
            return c;
    return path;
}

QString extract_tool(const QString &path)
{
    auto [k, suf] = kind(path);
    if (k == "rar")
        return tool("unrar").isEmpty() ? QString() : "unrar";
    if (k == "7z") {
        if (!tool("7z").isEmpty())
            return "7z";
        if (path.toLower().endsWith(".zip") && !tool("unzip").isEmpty())
            return "unzip";
        return QString();
    }
    if (k == "zpaq")
        return tool("zpaq").isEmpty() ? QString() : "zpaq";
    if (k == "tar" || k == "single") {
        if (suf.isEmpty())
            return tool("tar").isEmpty() ? QString() : "tar";
        for (const QString &t : stream_tools(suf))
            if (!tool(t).isEmpty())
                return t;
    }
    return QString();
}

bool can_extract(const QString &path) { return !kind(path).first.isEmpty(); }

QString missing_extract_tool(const QString &path)
{
    if (!extract_tool(path).isEmpty())
        return QString();
    auto [k, suf] = kind(path);
    if (k == "rar")
        return "unrar";
    if (k == "7z")
        return "7zip";
    if (k == "zpaq")
        return "zpaq";
    return install_hint(suf.isEmpty() ? "tar" : stream_tools(suf).last());
}

QString archive_stem(const QString &path)
{
    // name for the folder an archive extracts into: photos.tar.gz -> photos, a.part1.rar -> a
    QString name = basename(path);
    QString low = name.toLower();
    QStringList exts;
    for (const auto &e : TAR_EXTS)
        exts << e.first;
    for (const auto &e : SINGLE_EXTS)
        exts << e.first;
    exts << SEVENZ_EXTS << ".rar" << ".zpaq";
    std::stable_sort(exts.begin(), exts.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    for (const QString &ext : exts) {
        if (low.endsWith(ext)) {
            name.chop(ext.size());
            break;
        }
    }
    static const QRegularExpression part1("\\.part0*1$", QRegularExpression::CaseInsensitiveOption);
    name.remove(part1);
    return name.isEmpty() ? "archive" : name;
}

// ---------------------------------------------------------------- running tools

static proc::Options tool_opts(const QString &cwd = QString(), int in = proc::DEVNULL)
{
    proc::Options o;
    o.cwd = cwd;
    o.in = in;
    o.out = proc::PIPE;
    o.err = proc::STDOUT;
    o.new_session = true;
    o.c_utf8 = true;   // English messages (Kestrel parses them) but UTF-8 file names
    return o;
}

static void kill_all(std::initializer_list<proc::Process *> procs)
{
    for (proc::Process *p : procs)
        if (p)
            p->kill_group();
}

static QString elapsed(double seconds)
{
    int s = int(seconds);
    int m = s / 60, sec = s % 60;
    if (m >= 60)
        return QString("%1:%2:%3").arg(m / 60).arg(m % 60, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
}

static void write_all(int fd, const char *data, qsizetype n)
{
    qsizetype off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, data + off, size_t(n - off));
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0)
            throw_errno();
        off += w;
    }
}

// Run a tool, turning its output into progress. Returns (exit code, output text).
//
// progress: "percent" for tools printing an overall "NN%" (7z, rar, unrar, zpaq); "zipcount" for zip -dc's
// "done/remaining" file counter (zip's own percentages are compression ratios, not progress); empty to show only a
// busy bar. Percentages never go backwards, and a number split across two reads isn't misread.
//
// The tool runs under stdbuf so its progress lines arrive as they're printed: in a pipe, programs like zpaq
// otherwise hold their output back until a buffer fills or they exit, which freezes the bar. Long jobs show the
// elapsed time so it's clear they're still working when a tool goes quiet.
//
// read_phase: for tools whose percentage only counts input *read* (zpaq add), the text to show once it reaches
// 100% while the real work carries on silently; the bar then shows busy instead of a stuck number.
static QPair<int, QString> run_reporting(Task *task, QStringList argv, const QString &label, const QString &cwd,
                                         const QString *stdin_text, const QString &read_phase,
                                         const QString &progress)
{
    static const QRegularExpression percent_re("(\\d{1,3})(?:\\.\\d+)?%");
    static const QRegularExpression zip_count_re("(\\d+)/\\s*(\\d+) ");
    QString stdbuf = tool("stdbuf");
    if (!stdbuf.isEmpty())
        argv = QStringList{stdbuf, "-o0", "-e0"} + argv;
    auto p = proc::spawn(argv, tool_opts(cwd, stdin_text ? proc::PIPE : proc::DEVNULL));
    if (stdin_text) {
        QByteArray data = stdin_text->toUtf8();
        try {
            write_all(p->in, data.constData(), data.size());
        } catch (const BrokenPipe &) {
        }
        p->close_in();
    }
    QByteArray out;
    QElapsedTimer start;
    start.start();
    int pct = -1;
    QByteArray carry;
    try {
        int fd = p->out;
        for (;;) {
            task->check();
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(fd, &rf);
            struct timeval tv = {0, 200000};
            int r = ::select(fd + 1, &rf, nullptr, nullptr, &tv);
            if (r > 0) {
                char buf[65536];
                ssize_t n = ::read(fd, buf, sizeof buf);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    break;
                QByteArray chunk(buf, n);
                out += chunk;
                if (out.size() > 2000000)
                    out.remove(0, 1000000);
                QString text = QString::fromLatin1(carry + chunk);
                carry = (carry + chunk).right(16);
                QList<int> found;
                if (progress == "percent") {
                    auto it = percent_re.globalMatch(text);
                    while (it.hasNext())
                        found << it.next().captured(1).toInt();
                } else if (progress == "zipcount") {
                    auto it = zip_count_re.globalMatch(text);
                    while (it.hasNext()) {
                        auto m = it.next();
                        qint64 d = m.captured(1).toLongLong(), rem = m.captured(2).toLongLong();
                        found << int(d * 100 / std::max<qint64>(d + rem, 1));
                    }
                }
                if (!found.isEmpty())
                    pct = std::max(std::max(pct, 0), std::min(found.last(), 100));
            } else if (p->poll()) {
                break;
            }
            double secs = start.elapsed() / 1000.0;
            QString clock = secs >= 5 ? " · " + elapsed(secs) : QString();
            if (!read_phase.isEmpty() && pct >= 100)
                task->report(0, 0, QString("%1 — %2%3").arg(label, read_phase, clock));
            else if (pct >= 0)
                task->report(pct, 100, label + " — " + (read_phase.isEmpty() ? "" : "reading files ") +
                                           QString::number(pct) + "%" + clock);
            else
                task->report(0, 0, label + clock);
        }
        p->wait();
    } catch (const Cancelled &) {
        p->kill_group();
        throw;
    }
    return {p->returncode, QString::fromUtf8(out)};
}

// Copy bytes from fd src to fd dst, reporting progress against `total`. Returns bytes copied.
static qint64 relay(Task *task, int src, int dst, qint64 total, const QString &label, qint64 done = 0)
{
    std::vector<char> buf(1 << 20);
    for (;;) {
        task->check();
        ssize_t n = ::read(src, buf.data(), buf.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw_errno();
        if (n == 0)
            return done;
        write_all(dst, buf.data(), n);
        done += n;
        task->report(double(done), double(total),
                     QString("%1 — %2 of %3").arg(label, human_size(done), human_size(total)));
    }
}

static QString tail(const QString &text, int n = 8)
{
    static const QRegularExpression useful_re(
        "error|cannot|can't|fail|denied|incorrect|wrong|unsupported|not |no such|missing|corrupt|unexpected",
        QRegularExpression::CaseInsensitiveOption);
    QStringList lines, useful;
    for (const QString &ln : QString(text).remove('\b').split('\n')) {
        QString t = ln.trimmed();
        if (t.isEmpty())
            continue;
        lines << t;
        if (useful_re.match(t).hasMatch())
            useful << t;
    }
    const QStringList &src = useful.isEmpty() ? lines : useful;
    return src.mid(std::max<qsizetype>(0, src.size() - n)).join('\n');
}

// ---------------------------------------------------------------- compress

// names starting with "-" get a "./" prefix so no tool can mistake them for options (zpaq has no "--")
static QStringList safe_rels(const QStringList &rels)
{
    QStringList out;
    for (const QString &r : rels)
        out << (r.startsWith('-') ? "./" + r : r);
    return out;
}

struct Command {
    QStringList argv;
    std::optional<QString> stdin_text;
    QStringList display;   // with the password masked
    QString cwd;
};

// argv, stdin text, display argv and cwd for 7z/zip/rar/zpaq/tar jobs (stream formats: stream_compressor())
static Command compress_command(const Spec &spec)
{
    const Format &fmt = spec.format;
    const QString &t = spec.tool;
    QStringList rels = safe_rels(spec.rels);
    QString pw = spec.password;
    int lvl = spec.level.value_or(0);
    int thr = spec.threads;
    Command c;
    c.cwd = spec.base;
    QString secret;
    if (t == "7z") {
        c.argv = QStringList{exe("7z"), "a", "-y", "-bso0", "-bsp1", "-bse1", "-snl",   // -snl: keep symlinks as links
                             QString("-t%1").arg(fmt.id == "7z" ? "7z" : "zip"), QString("-mx=%1").arg(lvl)};
        if (!spec.method.isEmpty())
            c.argv << (fmt.id == "7z" ? "-m0=" + spec.method : "-mm=" + spec.method);
        if (thr)
            c.argv << QString("-mmt=%1").arg(thr);
        if (fmt.id == "7z" && spec.solid)
            c.argv << QString("-ms=%1").arg(*spec.solid ? "on" : "off");
        if (!pw.isEmpty()) {
            c.argv << "-p";
            c.stdin_text = QString("%1\n%1\n").arg(pw);
            if (fmt.id == "7z" && spec.encrypt_names)
                c.argv << "-mhe=on";
            if (fmt.id == "zip")
                c.argv << "-mem=" + (spec.zip_encryption.isEmpty() ? QString("AES256") : spec.zip_encryption);
        }
        if (spec.volume_mb)
            c.argv << QString("-v%1m").arg(spec.volume_mb);
        c.argv << spec.extra << "--" << spec.out << rels;
    } else if (t == "zip") {
        c.argv = QStringList{exe("zip"), "-r", "-y", "-dc", QString("-%1").arg(lvl)};   // -dc: "done/remaining" count
        if (!pw.isEmpty()) {
            c.argv << "-P" << pw;
            secret = pw;
        }
        c.argv << spec.extra << spec.out << "--" << rels;
    } else if (t == "rar") {
        c.argv = QStringList{exe("rar"), "a", "-y", "-idc", "-idd", "-ol", QString("-m%1").arg(lvl), "-r"};   // -ol: links
        if (thr)
            c.argv << QString("-mt%1").arg(thr);
        if (spec.solid.value_or(false))
            c.argv << "-s";
        if (spec.recovery)
            c.argv << QString("-rr%1%").arg(spec.recovery);
        if (spec.volume_mb)
            c.argv << QString("-v%1m").arg(spec.volume_mb);
        if (!pw.isEmpty()) {
            c.argv << QString("-%1%2").arg(spec.encrypt_names ? "hp" : "p", pw);
            secret = pw;
        }
        c.argv << spec.extra << "--" << spec.out << rels;
    } else if (t == "zpaq") {
        c.argv = QStringList{exe("zpaq"), "add", spec.out} + rels + QStringList{QString("-m%1").arg(lvl)};
        if (thr)
            c.argv << "-threads" << QString::number(thr);
        if (!pw.isEmpty()) {
            c.argv << "-key" << pw;
            secret = pw;
        }
        c.argv << spec.extra;
    } else if (t == "tar") {
        c.argv = QStringList{exe("tar"), "-cf", "-", "--"} + rels;
    } else {
        throw Error(t);
    }
    c.display = c.argv;
    if (!secret.isEmpty())
        for (QString &a : c.display)
            a.replace(secret, QString(8, QChar(0x2022)));
    return c;
}

static QStringList stream_compressor(const Spec &spec)
{
    const QString &t = spec.tool;
    int lvl = spec.level.value_or(0), thr = spec.threads;
    QStringList argv{exe(t), "-c", QString("-%1").arg(lvl)};
    if (t == "zstd" && lvl > 19)
        argv.insert(2, "--ultra");
    const Compressor &c = compressors()[t];
    if (thr && c.threads)
        argv << c.threads(thr);
    return argv + spec.extra;
}

QString command_preview(const Spec &spec)
{
    const Format &fmt = spec.format;
    if (!fmt.stream.isEmpty()) {
        QStringList comp_argv = stream_compressor(spec);
        comp_argv[0] = basename(comp_argv[0]);
        QString comp = shlex_join(comp_argv);
        QString src = shlex_join(spec.rels);
        if (fmt.single)
            return QString("%1 < %2 > %3").arg(comp, src, shlex_quote(basename(spec.out)));
        return QString("tar -cf - -- %1 | %2 > %3").arg(src, comp, shlex_quote(basename(spec.out)));
    }
    Command c = compress_command(spec);
    QString out_rel = relpath(spec.out, spec.base);   // the tool runs in the source folder
    QStringList shown{basename(c.display[0])};
    for (int i = 1; i < c.display.size(); ++i)
        shown << (c.display[i] == spec.out ? out_rel : c.display[i]);
    QString text = shlex_join(shown);
    if (fmt.id == "tar")
        text += " > " + shlex_quote(basename(spec.out));
    if (!spec.password.isEmpty() && spec.tool == "7z")
        text += "    (password passed on stdin)";
    return text;
}

static QStringList volume_files(const QString &out)
{
    QString d = dirname(out);
    if (d.isEmpty())
        d = ".";
    QString name = basename(out);
    QString stem = name;
    if (stem.endsWith(".rar"))
        stem.chop(4);
    QRegularExpression pat("^(" + QRegularExpression::escape(name) + "\\.\\d{3}|" + QRegularExpression::escape(stem) +
                           "\\.part\\d+\\.rar)$");
    QStringList res;
    try {
        for (const QString &f : listdir(d))
            if (pat.match(f).hasMatch())
                res << join(d, f);
    } catch (const OSError &) {
    }
    return res;
}

static void compress_tar_plain(Task *task, const QStringList &argv, const QString &cwd, const QString &out,
                               qint64 total, const QString &label)
{
    auto p = proc::spawn(argv, tool_opts(cwd));
    try {
        int fo = ::open(enc(out).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fo < 0)
            throw_errno(out);
        try {
            relay(task, p->out, fo, total, label);
        } catch (...) {
            ::close(fo);
            throw;
        }
        ::close(fo);
        p->wait();
    } catch (...) {
        p->kill_group();
        throw;
    }
    if (p->returncode != 0 && p->returncode != 1)
        throw Error(QString("tar failed (exit code %1)").arg(p->returncode));
}

// After all input is handed over: multi-threaded compressors (xz, zstd -T) buffer a lot of it and may need much
// longer to finish than it took to feed them, so show that instead of a bar stuck at 100%.
static void wait_finishing(Task *task, proc::Process *p, const QString &label, const QString &name)
{
    QElapsedTimer start;
    start.start();
    while (!p->poll()) {
        task->check();
        double secs = start.elapsed() / 1000.0;
        if (secs >= 0.5)
            task->report(0, 0, QString("%1 — %2 is compressing the last data · %3").arg(label, name, elapsed(secs)));
        QThread::msleep(100);
    }
}

static void compress_stream(Task *task, const Spec &spec, const QString &label)
{
    QStringList comp_argv = stream_compressor(spec);
    int fo = ::open(enc(spec.out).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fo < 0)
        throw_errno(spec.out);
    proc::Options co;
    co.in = proc::PIPE;
    co.out = fo;
    co.err = proc::PIPE;
    co.c_utf8 = true;
    std::unique_ptr<proc::Process> comp, src;
    try {
        comp = proc::spawn(comp_argv, co);
    } catch (...) {
        ::close(fo);
        throw;
    }
    ::close(fo);
    try {
        if (spec.format.single) {
            QString in_path = join(spec.base, spec.rels.first());
            int fi = ::open(enc(in_path).constData(), O_RDONLY | O_CLOEXEC);
            if (fi < 0)
                throw_errno(in_path);
            try {
                relay(task, fi, comp->in, spec.total, label);
            } catch (...) {
                ::close(fi);
                throw;
            }
            ::close(fi);
        } else {
            src = proc::spawn(QStringList{tool("tar"), "-cf", "-", "--"} + safe_rels(spec.rels), tool_opts(spec.base));
            relay(task, src->out, comp->in, spec.total, label);
            src->wait();
        }
        comp->close_in();
        wait_finishing(task, comp.get(), label, spec.tool);
    } catch (...) {
        kill_all({src.get(), comp.get()});
        throw;
    }
    if (src && src->returncode != 0 && src->returncode != 1)
        throw Error(QString("tar failed (exit code %1)").arg(src->returncode));
    if (comp->returncode != 0) {
        QString err = QString::fromUtf8(comp->read_all_err());
        QString t = tail(err);
        throw Error(!t.isEmpty() ? t : QString("%1 failed (exit code %2)").arg(basename(comp_argv[0]), QString::number(comp->returncode)));
    }
}

QString compress(Task *task, const Spec &spec)
{
    QString out = spec.out;
    QString label = "Compressing " + basename(out);
    try {
        if (!spec.format.stream.isEmpty()) {
            compress_stream(task, spec, label);
        } else {
            Command c = compress_command(spec);
            if (spec.format.id == "tar") {
                compress_tar_plain(task, c.argv, c.cwd, out, spec.total, label);
            } else {
                QString read_phase =
                    spec.tool == "zpaq" ? "compressing (zpaq reports no progress for this step)" : QString();
                auto [rc, text] = run_reporting(task, c.argv, label, c.cwd,
                                                c.stdin_text ? &*c.stdin_text : nullptr, read_phase,
                                                spec.tool == "zip" ? "zipcount" : "percent");
                bool ok = rc == 0 || ((spec.tool == "7z" || spec.tool == "rar") && rc == 1);   // 1 = warnings
                if (!ok) {
                    QString t = tail(text);
                    throw Error(!t.isEmpty() ? t : QString("%1 failed (exit code %2)").arg(basename(c.argv[0]), QString::number(rc)));
                }
            }
        }
    } catch (...) {
        for (const QString &f : QStringList{out} + volume_files(out))
            ::unlink(enc(f).constData());
        throw;
    }
    if (!exists(out)) {
        QStringList vols = volume_files(out);
        vols.sort();
        return vols.isEmpty() ? out : vols.first();
    }
    return out;
}

// ---------------------------------------------------------------- extract

ProbeResult probe(const QString &path_in)
{
    QString path = first_volume(path_in);
    QString t = extract_tool(path);
    if (t.isEmpty())
        return {false, QString("No tool to open this archive. Install %1.").arg(missing_extract_tool(path))};
    proc::Options o;
    o.c_utf8 = true;
    if (t == "7z") {
        auto r = proc::run({tool("7z"), "l", "-slt", "-p", "--", path}, 60000, o);
        if (r.timed_out)
            return {};
        QString text = QString::fromUtf8(r.out + r.err);
        if (text.contains("Encrypted = +") || text.contains("Cannot open encrypted archive") ||
            text.contains("Wrong password"))
            return {true, QString()};
        if (r.rc != 0 && text.contains("Can not open the file as archive"))
            return {false, "This file isn't a valid archive, or it's damaged."};
    } else if (t == "unrar") {
        auto r = proc::run({tool("unrar"), "lt", "-p-", "--", path}, 60000, o);
        if (r.timed_out)
            return {};
        QString text = QString::fromUtf8(r.out + r.err);
        if (r.rc == 11 || text.contains("Flags: encrypted") || text.contains("encrypted headers"))
            return {true, QString()};
    } else if (t == "zpaq") {
        auto r = proc::run({tool("zpaq"), "l", path}, 60000, o);
        if (r.timed_out)
            return {};
        if (QString::fromUtf8(r.out + r.err).contains("password incorrect"))
            return {true, QString()};
    }
    return {};
}

struct ExtractCommand {
    QString tool;
    QStringList argv;
    std::optional<QString> stdin_text;
};

static ExtractCommand extract_command(const QString &path, const QString &dest, const QString &password,
                                      const QString &overwrite, int threads)
{
    ExtractCommand c;
    c.tool = extract_tool(path);
    const QString &t = c.tool;
    if (t == "7z") {
        QString policy = overwrite == "overwrite" ? "-aoa" : overwrite == "skip" ? "-aos" : "-aou";
        c.argv = QStringList{tool("7z"), "x", "-y", "-bso0", "-bsp1", "-bse1", "-o" + dest, policy};
        if (threads)
            c.argv << QString("-mmt=%1").arg(threads);
        c.argv << "--" << path;
        c.stdin_text = password + "\n";   // no -p switch: on extract a bare -p means "empty password"
    } else if (t == "unzip") {
        c.argv = QStringList{tool("unzip"), overwrite == "overwrite" ? "-o" : "-n"};
        if (!password.isEmpty())
            c.argv << "-P" << password;
        c.argv << "--" << path << "-d" << dest;
    } else if (t == "unrar") {
        QString policy = overwrite == "overwrite" ? "-o+" : overwrite == "skip" ? "-o-" : "-or";
        c.argv = QStringList{tool("unrar"), "x", "-y", "-idc", "-idd", policy,
                             password.isEmpty() ? QString("-p-") : "-p" + password};
        if (threads)
            c.argv << QString("-mt%1").arg(threads);
        c.argv << "--" << path << rstrip(dest, '/') + "/";
    } else if (t == "zpaq") {
        c.argv = QStringList{tool("zpaq"), "x", path, "-to", dest};
        if (overwrite == "overwrite")
            c.argv << "-force";
        if (threads)
            c.argv << "-threads" << QString::number(threads);
        if (!password.isEmpty())
            c.argv << "-key" << password;
    } else {
        throw Error(t);
    }
    return c;
}

static QString extract_stream(Task *task, const QString &path, const QString &dest, const QString &k,
                              const QString &suf, const QString &overwrite, const QString &label)
{
    qint64 total = std::max<qint64>(getsize(path), 1);
    QStringList dec_argv;
    if (!suf.isEmpty()) {
        QString name;
        for (const QString &t : stream_tools(suf))
            if (!tool(t).isEmpty()) {
                name = t;
                break;
            }
        dec_argv = QStringList{tool(name), "-dc"};
        if (name == "xz" || name == "zstd")
            dec_argv << "-T0";
    }
    int fi = ::open(enc(path).constData(), O_RDONLY | O_CLOEXEC);
    if (fi < 0)
        throw_errno(path);
    struct CloseFd {
        int fd;
        ~CloseFd() { ::close(fd); }
    } close_fi{fi};
    if (k == "single") {
        QString out = join(dest, archive_stem(path));
        if (exists(out)) {
            if (overwrite == "skip")
                return dest;
            if (overwrite == "rename")
                out = unique_path(dest, basename(out), "num");
        }
        int fo = ::open(enc(out).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fo < 0)
            throw_errno(out);
        proc::Options o;
        o.in = proc::PIPE;
        o.out = fo;
        o.err = proc::PIPE;
        o.c_utf8 = true;
        std::unique_ptr<proc::Process> dec;
        try {
            dec = proc::spawn(dec_argv, o);
        } catch (...) {
            ::close(fo);
            throw;
        }
        ::close(fo);
        try {
            relay(task, fi, dec->in, total, label);
            dec->close_in();
            dec->wait();
        } catch (...) {
            dec->kill_group();
            ::unlink(enc(out).constData());
            throw;
        }
        if (dec->returncode != 0) {
            ::unlink(enc(out).constData());
            QString t = tail(QString::fromUtf8(dec->read_all_err()));
            throw Error(t.isEmpty() ? QString("decompression failed") : t);
        }
        return dest;
    }
    QString policy = overwrite == "overwrite" ? "--overwrite" : overwrite == "skip" ? "--skip-old-files" : "--backup=numbered";
    QStringList tar_argv{tool("tar"), "-xf", "-", "-C", dest, "--no-same-owner", policy};
    std::unique_ptr<proc::Process> dec, tar;
    int feed;
    if (!suf.isEmpty()) {
        proc::Options o;
        o.in = proc::PIPE;
        o.out = proc::PIPE;
        o.err = proc::PIPE;
        o.c_utf8 = true;
        dec = proc::spawn(dec_argv, o);
        try {
            tar = proc::spawn(tar_argv, tool_opts(QString(), dec->out));
        } catch (...) {
            dec->kill_group();
            throw;
        }
        dec->close_out();
        feed = dec->in;
    } else {
        tar = proc::spawn(tar_argv, tool_opts(QString(), proc::PIPE));
        feed = tar->in;
    }
    QString out;
    try {
        try {
            relay(task, fi, feed, total, label);
            if (dec)
                dec->close_in();
            else
                tar->close_in();
            if (dec)
                dec->wait();
            out = QString::fromUtf8(tar->read_all_out());
            tar->wait();
        } catch (const BrokenPipe &) {
            if (dec)
                dec->kill_group();
            if (dec)
                dec->close_in();
            else
                tar->close_in();
            out = QString::fromUtf8(tar->read_all_out());
            tar->wait();
        }
    } catch (...) {
        kill_all({dec.get(), tar.get()});
        throw;
    }
    if (dec && dec->returncode != 0) {
        QString t = tail(QString::fromUtf8(dec->read_all_err()));
        throw Error(t.isEmpty() ? QString("decompression failed") : t);
    }
    if (tar->returncode != 0 && tar->returncode != 1) {
        QString t = tail(out);
        throw Error(t.isEmpty() ? QString("tar failed (exit code %1)").arg(tar->returncode) : t);
    }
    return dest;
}

QString extract(Task *task, const QString &path_in, const QString &dest, const QString &password,
                const QString &overwrite, int threads)
{
    QString path = first_volume(path_in);
    auto [k, suf] = kind(path);
    QString label = "Extracting " + basename(path);
    if (k == "tar" || k == "single")
        return extract_stream(task, path, dest, k, suf, overwrite, label);
    ExtractCommand c = extract_command(path, dest, password, overwrite, threads);
    auto [rc, text] = run_reporting(task, c.argv, label, QString(), c.stdin_text ? &*c.stdin_text : nullptr,
                                    QString(), c.tool == "unzip" ? QString() : "percent");   // unzip's % are ratios
    const QString &t = c.tool;
    bool wrong = (t == "unrar" && rc == 11) || text.contains("Wrong password") || text.contains("password incorrect") ||
                 text.toLower().contains("incorrect password");
    if (wrong)
        throw WrongPassword();
    if (t == "7z" && rc == 2 && text.toLower().contains("encrypted"))
        throw WrongPassword();
    if (rc != 0 && !((t == "7z" || t == "unrar") && rc == 1) && !(t == "unzip" && rc == 1)) {
        QString tl = tail(text);
        throw Error(tl.isEmpty() ? QString("%1 failed (exit code %2)").arg(t, QString::number(rc)) : tl);
    }
    return dest;
}

}  // namespace archive
