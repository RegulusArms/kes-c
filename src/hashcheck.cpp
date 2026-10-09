// Checksum files: parsing (GNU coreutils' "hash  name", BSD tags "SHA256 (name) = hash", SFV's "name crc32", or a lone
// hash), hashing with progress, and the Verify Checksums dialog.
#include "hashcheck.h"

#include "fileops.h"
#include "util.h"

#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

using namespace util;

namespace hashcheck {

namespace {

// hex digits per algorithm
const QMap<QString, int> DIGEST_LEN = {{"crc32", 8},    {"md5", 32},    {"sha1", 40},    {"sha224", 56},
                                       {"sha256", 64},  {"sha384", 96}, {"sha512", 128}, {"blake2b", 128}};
// a hash's algorithm from its length, when nothing else says (BLAKE2b needs its name: it's as long as SHA-512)
const QMap<int, QString> BY_LEN = {{8, "crc32"},   {32, "md5"},    {40, "sha1"},  {56, "sha224"},
                                   {64, "sha256"}, {96, "sha384"}, {128, "sha512"}};
// BSD tags, lower-cased without "-"
const QMap<QString, QString> TAGS = {{"crc32", "crc32"},   {"md5", "md5"},       {"sha1", "sha1"},
                                     {"sha224", "sha224"}, {"sha256", "sha256"}, {"sha384", "sha384"},
                                     {"sha512", "sha512"}, {"blake2b", "blake2b"}, {"blake2b512", "blake2b"}};
const QMap<QString, QString> EXTS = {
    {".sfv", "crc32"},       {".md5", "md5"},          {".md5sum", "md5"},       {".sha1", "sha1"},
    {".sha1sum", "sha1"},    {".sha224", "sha224"},    {".sha224sum", "sha224"}, {".sha256", "sha256"},
    {".sha256sum", "sha256"}, {".sha384", "sha384"},   {".sha384sum", "sha384"}, {".sha512", "sha512"},
    {".sha512sum", "sha512"}, {".b2", "blake2b"},      {".b2sum", "blake2b"}};
const QMap<QString, QString> NAMES = {{"md5sums", "md5"},       {"sha1sums", "sha1"},     {"sha224sums", "sha224"},
                                      {"sha256sums", "sha256"}, {"sha384sums", "sha384"}, {"sha512sums", "sha512"},
                                      {"b2sums", "blake2b"}};

// the algorithm a checksum file's name suggests, "" if none
QString hint(const QString &path)
{
    QString name = basename(path).toLower();
    return NAMES.value(name, EXTS.value(ext_of(path)));
}

QString algo_for(const QString &hex, const QString &hint)
{
    if (!hint.isEmpty() && DIGEST_LEN.value(hint) == hex.size())
        return hint;
    return BY_LEN.value(int(hex.size()));
}

// GNU's escaping of names holding a backslash or a line break ("\" before the hash)
QString unescape(const QString &s)
{
    QString out;
    for (int i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            QChar c = s[++i];
            out += c == 'n' ? QChar('\n') : c == 'r' ? QChar('\r') : c;
        } else {
            out += s[i];
        }
    }
    return out;
}

quint32 crc32_update(quint32 crc, const char *data, qint64 n)
{
    static quint32 table[256];
    static bool ready = [] {
        for (quint32 i = 0; i < 256; i++) {
            quint32 c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void)ready;
    crc = ~crc;
    for (qint64 i = 0; i < n; i++)
        crc = table[(crc ^ quint8(data[i])) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

}   // namespace

bool is_hash_file(const QString &path)
{
    QString name = basename(path).toLower();
    return !hint(path).isEmpty() || name == "checksum" || name == "checksums" || name.endsWith("-checksum") ||
           name.endsWith("-checksums");
}

QString file_filter()
{
    return "Checksum files (*.sfv *.md5 *.sha1 *.sha224 *.sha256 *.sha384 *.sha512 *.b2 *.md5sum *.sha1sum *.sha256sum "
           "*.sha512sum *SUMS *sums *CHECKSUM *CHECKSUMS *checksum *checksums);;All files (*)";
}

QList<Entry> parse_text(const QString &text, const QString &path)
{
    static const QRegularExpression sfv_re(R"(^(.*\S)\s+([0-9A-Fa-f]{8})$)");
    static const QRegularExpression bsd_re(R"(^(\\?)([A-Za-z0-9-]+) ?\((.*)\) ?= ?([0-9A-Fa-f]+)$)");
    static const QRegularExpression gnu_re(R"(^(\\?)([0-9A-Fa-f]+) [ *]?(.+)$)");
    static const QRegularExpression hex_re(R"(^[0-9A-Fa-f]+$)");
    QString h = hint(path), base = dirname(path);
    QList<Entry> entries;
    QStringList lines;
    auto add = [&](const QString &name, const QString &hex, const QString &algo) {
        entries << Entry{normpath(join(base, name)), name, hex.toLower(), algo};
    };
    for (const QString &line : text.split('\n')) {
        QString s = line.trimmed();
        if (s.isEmpty() || s.startsWith('#') || s.startsWith(';'))
            continue;
        lines << s;
        if (h == "crc32") {
            auto m = sfv_re.match(s);
            if (m.hasMatch())
                add(m.captured(1), m.captured(2), "crc32");
            continue;
        }
        auto m = bsd_re.match(s);
        if (m.hasMatch()) {
            QString algo = TAGS.value(m.captured(2).toLower().remove('-'));
            if (!algo.isEmpty() && DIGEST_LEN.value(algo) == m.captured(4).size())
                add(m.captured(1).isEmpty() ? m.captured(3) : unescape(m.captured(3)), m.captured(4), algo);
            continue;
        }
        m = gnu_re.match(s);
        if (m.hasMatch()) {
            QString algo = algo_for(m.captured(2), h);
            if (!algo.isEmpty())
                add(m.captured(1).isEmpty() ? m.captured(3) : unescape(m.captured(3)), m.captured(2), algo);
        }
    }
    // only a hash: for the file named like this one without its extension (disk.iso.sha256 → disk.iso)
    if (entries.isEmpty() && lines.size() == 1 && hex_re.match(lines[0]).hasMatch() &&
        !splitext_ext(path).isEmpty()) {
        QString algo = algo_for(lines[0], h);
        QString target = path.left(path.size() - splitext_ext(path).size());
        if (!algo.isEmpty())
            add(basename(target), lines[0], algo);
    }
    return entries;
}

QList<Entry> parse(const QString &path)
{
    bool ok = false;
    QByteArray data = read_file(path, &ok);
    if (!ok || data.size() > (16 << 20))   // not a checksum file
        return {};
    return parse_text(QString::fromUtf8(data), path);
}

const Entry *find(const QList<Entry> &entries, const QString &file)
{
    for (const Entry &e : entries)
        if (e.path == file)
            return &e;
    for (const Entry &e : entries)
        if (basename(e.path) == basename(file))
            return &e;
    return nullptr;
}

QString algo_label(const QString &algo)
{
    return algo == "blake2b" ? QString("BLAKE2b") : algo.toUpper();
}

QString hash_file(const QString &path, const QString &algo, const std::function<void(qint64, qint64)> &progress,
                  const std::function<void()> &check)
{
    static const QMap<QString, QCryptographicHash::Algorithm> ALGOS = {
        {"md5", QCryptographicHash::Md5},       {"sha1", QCryptographicHash::Sha1},
        {"sha224", QCryptographicHash::Sha224}, {"sha256", QCryptographicHash::Sha256},
        {"sha384", QCryptographicHash::Sha384}, {"sha512", QCryptographicHash::Sha512},
        {"blake2b", QCryptographicHash::Blake2b_512}};
    int fd = ::open(enc(path).constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw_errno(path);
    struct stat st;
    qint64 size = ::fstat(fd, &st) == 0 ? st.st_size : 0;
    QCryptographicHash h(ALGOS.value(algo, QCryptographicHash::Md5));
    quint32 crc = 0;
    QByteArray buf(4 << 20, Qt::Uninitialized);
    qint64 done = 0;
    for (;;) {
        if (check)
            check();
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            int e = errno;
            ::close(fd);
            throw_errno(path, e);
        }
        if (n == 0)
            break;
        if (algo == "crc32")
            crc = crc32_update(crc, buf.constData(), n);
        else
            h.addData(QByteArrayView(buf.constData(), n));
        done += n;
        if (progress)
            progress(done, size);
    }
    ::close(fd);
    if (algo == "crc32")
        return QString("%1").arg(crc, 8, 16, QChar('0'));
    return QString::fromLatin1(h.result().toHex());
}

Result verify(const Entry &e, Task *task)
{
    if (!lexists(e.path))
        return {"missing", {}, {}};
    try {
        QString actual = hash_file(
            e.path, e.algo,
            [task, &e](qint64 done, qint64 size) {
                if (task)
                    task->report(done, size, "Verifying " + e.name);
            },
            [task]() {
                if (task)
                    task->check();
            });
        return {actual == e.expected ? "ok" : "failed", actual, {}};
    } catch (const OSError &err) {
        return {"error", {}, QString::fromLocal8Bit(strerror(err.code))};
    }
}

// ---------------------------------------------------------------- the dialog

VerifyDialog::VerifyDialog(QWidget *parent, const QString &path, const QList<Entry> &entries)
    : QDialog(parent), path(path), entries(entries)
{
    setWindowTitle("Verify Checksums — " + basename(path));
    resize(640, 400);
    auto *lay = new QVBoxLayout(this);
    auto *head = new QLabel(QString("Checking the files listed in “%1”.").arg(basename(path)));
    head->setWordWrap(true);
    lay->addWidget(head);
    tree = new QTreeWidget;
    tree->setHeaderLabels({"File", "Algorithm", "Result"});
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    for (const Entry &e : entries) {
        auto *it = new QTreeWidgetItem(tree, {e.name, algo_label(e.algo), ""});
        it->setToolTip(0, e.path);
    }
    lay->addWidget(tree, 1);
    bar = new QProgressBar;
    bar->setRange(0, 1000);
    bar->setAccessibleName("Progress");
    lay->addWidget(bar);
    status = new QLabel;
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(status);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    again = bb->addButton("Verify Again", QDialogButtonBox::ActionRole);
    stop_btn = bb->addButton("Stop", QDialogButtonBox::ActionRole);
    connect(again, &QPushButton::clicked, this, &VerifyDialog::start);
    connect(stop_btn, &QPushButton::clicked, this, [this]() {
        stop();
        summary();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
    start();
}

VerifyDialog::~VerifyDialog() { stop(); }

void VerifyDialog::hideEvent(QHideEvent *ev)
{
    stop();   // closed: nothing more to read
    QDialog::hideEvent(ev);
}

void VerifyDialog::start()
{
    stop();
    results = QList<Result>(entries.size());
    sizes.clear();
    total = before = 0;
    for (const Entry &e : entries) {
        struct stat st;
        sizes << (stat_(e.path, st) ? qint64(st.st_size) : 0);
        total += sizes.last();
    }
    for (int i = 0; i < entries.size(); i++)
        show_row(i);
    next = 0;
    bar->setValue(0);
    step();
}

void VerifyDialog::step()
{
    again->setEnabled(false);
    stop_btn->setEnabled(true);
    if (next >= entries.size()) {
        summary();
        return;
    }
    int i = next, g = gen;
    QTreeWidgetItem *it = tree->topLevelItem(i);
    it->setText(2, "Checking…");
    tree->scrollToItem(it);
    status->setStyleSheet(QString());
    status->setText(QString("Checking %1 of %2: %3").arg(QString::number(i + 1), QString::number(entries.size()),
                                                          entries[i].name));
    Entry e = entries[i];
    // no parent: closing the dialog mustn't destroy a thread that is still reading. The result comes through
    // on_done, not a connection made afterwards, which a quick file could finish before.
    QPointer<VerifyDialog> self(this);
    task = fileops::run_job(
        nullptr, "Verifying " + e.name,
        [e](Task *t) -> QVariant {
            try {
                Result r = verify(e, t);
                return QVariantList{r.status, r.actual, r.error};
            } catch (const Cancelled &) {
                throw;
            } catch (const std::exception &err) {   // as a result: run_job connects on_done before the thread starts
                return QVariantList{"error", "", QString::fromStdString(err.what())};
            }
        },
        [self, g](const QVariant &res) {
            if (self && g == self->gen)
                self->on_result(res);
        },
        true, true);
    connect(task, &Task::progress, this, [this, g, i](double f, const QString &) {
        if (g != gen)
            return;
        qint64 here = before + qint64(std::max(f, 0.0) * sizes[i]);
        bar->setValue(total ? int(1000 * here / total) : 1000 * i / entries.size());
    });
}

void VerifyDialog::on_result(const QVariant &res)
{
    task = nullptr;
    if (!res.isValid())   // stopped
        return;
    QVariantList r = res.toList();
    results[next] = {r.value(0).toString(), r.value(1).toString(), r.value(2).toString()};
    show_row(next);
    before += sizes[next];
    next++;
    bar->setValue(total ? int(1000 * before / total) : 1000 * next / entries.size());
    step();
}

void VerifyDialog::stop()
{
    gen++;   // results still on their way are for the old run
    if (task)
        task->cancel();
    task = nullptr;
}

void VerifyDialog::show_row(int i)
{
    QTreeWidgetItem *it = tree->topLevelItem(i);
    const Result &r = results[i];
    const Entry &e = entries[i];
    QString text, tip;
    if (r.status == "ok") {
        text = "✔ OK";
    } else if (r.status == "failed") {
        text = "✘ Doesn't match";
        tip = QString("Expected: %1\nActual: %2").arg(e.expected, r.actual);
    } else if (r.status == "missing") {
        text = "✘ Missing";
        tip = "Not found: " + e.path;
    } else if (r.status == "error") {
        text = "✘ " + r.error;
        tip = r.error;
    }
    it->setText(2, text);
    it->setToolTip(2, tip);
    QBrush brush = r.status.isEmpty() ? QBrush() : QBrush(r.status == "ok" ? ok_color() : error_color());
    for (int c = 0; c < 3; c++)
        it->setForeground(c, c == 2 ? brush : QBrush());
}

void VerifyDialog::summary()
{
    again->setEnabled(true);
    stop_btn->setEnabled(false);
    int ok = 0, failed = 0, missing = 0, errors = 0, checked = 0;
    for (const Result &r : results) {
        checked += !r.status.isEmpty();
        ok += r.status == "ok";
        failed += r.status == "failed";
        missing += r.status == "missing";
        errors += r.status == "error";
    }
    QStringList parts;
    if (failed)
        parts << QString(failed == 1 ? "%1 doesn't match" : "%1 don't match").arg(failed);
    if (missing)
        parts << QString("%1 missing").arg(missing);
    if (errors)
        parts << QString("%1 couldn't be read").arg(errors);
    if (ok)
        parts << QString("%1 OK").arg(ok);
    QString text;
    if (checked < entries.size())
        text = QString("Stopped after %1 of %2 files").arg(checked).arg(entries.size()) +
               (parts.isEmpty() ? QString(".") : ": " + parts.join(", ") + ".");
    else if (ok == entries.size())
        text = entries.size() == 1 ? QString("✔ The file is OK.") : QString("✔ All %1 files are OK.").arg(ok);
    else
        text = "✘ " + parts.join(", ") + ".";
    bool good = checked == entries.size() && ok == entries.size();
    bool bad = failed || missing || errors;
    status->setText(text);
    status->setStyleSheet(good  ? QString("color: %1").arg(ok_color().name())
                          : bad ? QString("color: %1").arg(error_color().name())
                                : QString());
}

bool open_dialog(QWidget *parent, const QString &path)
{
    QList<Entry> entries = parse(path);
    if (entries.isEmpty())
        return false;
    auto *d = new VerifyDialog(parent, path, entries);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->show();
    d->raise();
    d->activateWindow();
    return true;
}

}   // namespace hashcheck
