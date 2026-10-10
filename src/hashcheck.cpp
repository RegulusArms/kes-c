// Checksum files: parsing (GNU coreutils' "hash  name", BSD tags "SHA256 (name) = hash", SFV's "name crc32", or a lone
// hash), hashing with progress, and the Verify Checksums dialog.
#include "hashcheck.h"

#include "app.h"
#include "fileops.h"
#include "pane.h"
#include "util.h"

#include <QCheckBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <vector>

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

QStringList hash_files(const QString &path, const QStringList &algos,
                       const std::function<void(qint64, qint64)> &progress, const std::function<void()> &check)
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
    std::vector<std::unique_ptr<QCryptographicHash>> hashes;   // nullptr for CRC32
    for (const QString &a : algos)
        hashes.emplace_back(a == "crc32" ? nullptr : new QCryptographicHash(ALGOS.value(a, QCryptographicHash::Md5)));
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
        for (auto &h : hashes) {
            if (h)
                h->addData(QByteArrayView(buf.constData(), n));
            else
                crc = crc32_update(crc, buf.constData(), n);
        }
        done += n;
        if (progress)
            progress(done, size);
    }
    ::close(fd);
    QStringList out;
    for (auto &h : hashes)
        out << (h ? QString::fromLatin1(h->result().toHex()) : QString("%1").arg(crc, 8, 16, QChar('0')));
    return out;
}

QString hash_file(const QString &path, const QString &algo, const std::function<void(qint64, qint64)> &progress,
                  const std::function<void()> &check)
{
    return hash_files(path, {algo}, progress, check).first();
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

// ---------------------------------------------------------------- making checksum files

namespace hashcheck {

namespace {

const QMap<QString, QString> OUT_EXT = {{"crc32", ".sfv"},    {"md5", ".md5"},       {"sha1", ".sha1"},
                                        {"sha256", ".sha256"}, {"sha512", ".sha512"}, {"blake2b", ".b2"}};

// `data` to path in one step (write_parts; fsynced, since it may replace an older checksum file)
void write_replacing(const QString &path, const QByteArray &data)
{
    write_parts(path, [&](int fd) { write_all(fd, data.constData(), data.size(), path); }, false, true);
}

}   // namespace

const QStringList &create_algorithms()
{
    static const QStringList a = {"crc32", "md5", "sha1", "sha256", "sha512", "blake2b"};
    return a;
}

QStringList files_in(const QStringList &paths, const std::function<void()> &check)
{
    QStringList out;
    for (const QString &p : paths) {
        if (!isdir(p)) {
            out << p;
            continue;
        }
        walk(p, [&](const QString &root, QStringList &dirs, QStringList &files) {
            if (check)
                check();
            dirs.sort();
            files.sort();
            for (const QString &f : files)
                out << join(root, f);
            return true;
        });
    }
    return out;
}

QStringList output_paths(const QString &dir, const QString &stem, const QStringList &algos, bool one_file)
{
    if (one_file)
        return {join(dir, stem + "-CHECKSUM")};
    QStringList out;
    for (const QString &a : algos)
        out << join(dir, stem + OUT_EXT.value(a));
    return out;
}

QString format_line(const QString &name, const QString &algo, const QString &hex, const QString &style)
{
    if (style == "sfv")
        return name + " " + hex.toUpper();
    bool escape = name.contains('\\') || name.contains('\n') || name.contains('\r');
    QString n = name;
    if (escape)
        n.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r");
    QString prefix = escape ? QString("\\") : QString();
    if (style == "bsd")
        return prefix + algo_label(algo) + " (" + n + ") = " + hex;
    return prefix + hex + "  " + n;
}

QStringList create(Task *task, const QStringList &paths, const QString &dir, const QString &stem,
                   const QStringList &algos, bool one_file, QStringList *errors)
{
    auto check = [task]() {
        if (task)
            task->check();
    };
    if (task)
        task->report(0, 0, "Listing files…");
    QStringList outputs = output_paths(dir, stem, algos, one_file), files, skip;
    for (const QString &o : outputs)
        skip << normpath(o);
    for (const QString &f : files_in(paths, check))
        if (!skip.contains(normpath(f)))   // an older copy of what's being written
            files << f;
    if (files.isEmpty())
        throw Error("There are no files to make checksums of.");
    qint64 total = 0, before = 0;
    QList<qint64> sizes;
    for (const QString &f : files) {
        struct stat st;
        sizes << (stat_(f, st) ? qint64(st.st_size) : 0);
        total += sizes.last();
    }
    QList<QPair<QString, QStringList>> hashed;   // relative name, hashes in algos' order
    for (int i = 0; i < files.size(); i++) {
        QString name = relpath(files[i], dir);
        try {
            QStringList h = hash_files(
                files[i], algos,
                [&](qint64 done, qint64) {
                    if (task)
                        task->report(before + done, std::max<qint64>(total, 1), "Hashing " + basename(files[i]));
                },
                check);
            hashed << qMakePair(name, h);
        } catch (const OSError &e) {
            if (errors)
                *errors << name + ": " + QString::fromLocal8Bit(strerror(e.code));
        }
        before += sizes[i];
    }
    if (hashed.isEmpty())
        throw Error("None of the files could be read.");
    check();
    if (one_file) {
        QString text;
        for (const auto &[name, h] : hashed)
            for (int a = 0; a < algos.size(); a++)
                text += format_line(name, algos[a], h[a], "bsd") + "\n";
        write_replacing(outputs.first(), text.toUtf8());
    } else {
        for (int a = 0; a < algos.size(); a++) {
            QString style = algos[a] == "crc32" ? "sfv" : "gnu";
            QString text = style == "sfv" ? QString("; Made by Kestrel Explorer\n") : QString();
            for (const auto &[name, h] : hashed)
                text += format_line(name, algos[a], h[a], style) + "\n";
            write_replacing(outputs[a], text.toUtf8());
        }
    }
    return outputs;
}

// ---------------------------------------------------------------- Create Checksum File dialog

CreateDialog::CreateDialog(QWidget *parent, const QStringList &paths) : QDialog(parent), paths(paths)
{
    setWindowTitle("Create Checksum File");
    auto *lay = new QVBoxLayout(this);
    QString first = rstrip(paths.first(), '/');
    bool folders = std::any_of(paths.begin(), paths.end(), [](const QString &p) { return isdir(p); });
    auto *head = new QLabel((paths.size() == 1 ? QString("Checksums for “%1”.").arg(basename(first))
                                               : QString("Checksums for %1 items.").arg(paths.size())) +
                            (folders ? QString(" The files inside folders are included.") : QString()));
    head->setWordWrap(true);
    lay->addWidget(head);

    auto *algo_box = new QGroupBox("Algorithms");
    auto *grid = new QGridLayout(algo_box);
    QStringList saved = settings().value("checksum_algorithms", "sha256").toString().split(',', Qt::SkipEmptyParts);
    int i = 0;
    for (const QString &a : create_algorithms()) {
        auto *b = new QCheckBox(a == "crc32" ? QString("CRC32 (SFV)") : algo_label(a));
        b->setChecked(saved.contains(a));
        boxes[a] = b;
        grid->addWidget(b, i / 3, i % 3);
        i++;
        connect(b, &QCheckBox::toggled, this, &CreateDialog::update);
    }
    lay->addWidget(algo_box);

    auto *files_box = new QGroupBox("Checksum files");
    auto *fl = new QVBoxLayout(files_box);
    each = new QRadioButton("One file for each algorithm (as md5sum, sha256sum… write them)");
    single = new QRadioButton("One file with every algorithm (BSD tags, like Fedora's CHECKSUM files)");
    (settings().value("checksum_one_file", false).toBool() ? single : each)->setChecked(true);
    fl->addWidget(each);
    fl->addWidget(single);
    connect(each, &QRadioButton::toggled, this, &CreateDialog::update);
    lay->addWidget(files_box);

    auto *form = new QFormLayout;
    name = new QLineEdit(paths.size() == 1 ? basename(first) : basename(dirname(first)));
    if (name->text().isEmpty())
        name->setText("checksums");
    form->addRow("Name:", name);
    auto *row = new QHBoxLayout;
    folder = new QLineEdit(dirname(first));
    auto *browse = new QPushButton("Browse…");
    row->addWidget(folder, 1);
    row->addWidget(browse);
    form->addRow("Save in:", row);
    lay->addLayout(form);
    connect(name, &QLineEdit::textChanged, this, &CreateDialog::update);
    connect(folder, &QLineEdit::textChanged, this, &CreateDialog::update);
    connect(browse, &QPushButton::clicked, this, [this]() {
        QString d = QFileDialog::getExistingDirectory(this, "Save Checksum File In", folder->text());
        if (!d.isEmpty())
            folder->setText(d);
    });

    preview = new QLabel;
    preview->setWordWrap(true);
    preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(preview);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    create_btn = bb->button(QDialogButtonBox::Ok);
    create_btn->setText("Create");
    connect(bb, &QDialogButtonBox::accepted, this, &CreateDialog::ok);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
    update();
}

QStringList CreateDialog::algos() const
{
    QStringList out;
    for (const QString &a : create_algorithms())
        if (boxes[a]->isChecked())
            out << a;
    return out;
}

bool CreateDialog::one_file() const { return single->isChecked(); }
QString CreateDialog::dir() const { return folder->text().trimmed(); }
QString CreateDialog::stem() const { return name->text().trimmed(); }

void CreateDialog::update()
{
    QString problem;
    if (algos().isEmpty())
        problem = "Choose at least one algorithm.";
    else if (stem().isEmpty() || stem().contains('/'))
        problem = "Enter a name (without “/”).";
    else if (!isdir(dir()))
        problem = "The folder to save in doesn't exist.";
    create_btn->setEnabled(problem.isEmpty());
    if (!problem.isEmpty()) {
        preview->setText(problem);
        return;
    }
    QStringList names, existing;
    for (const QString &o : outputs()) {
        names << basename(o);
        if (lexists(o))
            existing << basename(o);
    }
    preview->setText("Creates: " + names.join(", ") +
                     (existing.isEmpty() ? QString() : "\nReplaces: " + existing.join(", ")));
}

void CreateDialog::ok()
{
    QStringList existing;
    for (const QString &o : outputs())
        if (lexists(o))
            existing << basename(o);
    if (!existing.isEmpty() &&
        QMessageBox::question(this, "Create Checksum File",
                              "Replace " + existing.join(", ") + "?") != QMessageBox::Yes)
        return;
    settings().setValue("checksum_algorithms", algos().join(','));
    settings().setValue("checksum_one_file", one_file());
    accept();
}

void create_dialog(MainWindow *win, const QStringList &paths)
{
    CreateDialog dlg(win, paths);
    if (dlg.exec())
        run_create(win, paths, dlg.dir(), dlg.stem(), dlg.algos(), dlg.one_file());
}

void run_create(MainWindow *win, const QStringList &paths, const QString &dir, const QString &stem,
                const QStringList &algos, bool one_file)
{
    auto work = [=](Task *task) -> QVariant {
        QStringList errors;
        QStringList written = create(task, paths, dir, stem, algos, one_file, &errors);
        return QVariantList{written, errors};
    };
    QPointer<MainWindow> w(win);
    auto done = [w, dir](const QVariant &r) {
        if (!w)
            return;
        if (!r.isValid()) {
            w->statusBar()->showMessage("Creating checksums cancelled", 4000);
            return;
        }
        QStringList written = r.toList().value(0).toStringList(), errors = r.toList().value(1).toStringList();
        QStringList names;
        for (const QString &o : written)
            names << basename(o);
        w->statusBar()->showMessage("Created " + names.join(", "), 6000);
        if (w->pane() && w->pane()->dir() == dir && !written.isEmpty())
            w->pane()->select_later(written.first());
        if (!errors.isEmpty()) {
            QStringList shown = errors.mid(0, 10);
            if (errors.size() > 10)
                shown << QString("…and %1 more").arg(errors.size() - 10);
            QString head = errors.size() == 1
                               ? QString("1 file couldn't be read and was left out:")
                               : QString("%1 files couldn't be read and were left out:").arg(errors.size());
            QMessageBox::warning(w, "Create Checksum File", head + "\n\n" + shown.join("\n"));
        }
    };
    fileops::run_job(win, "Creating checksums", work, done);
}

}   // namespace hashcheck
