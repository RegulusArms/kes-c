#include "diskimage.h"

#include "admin.h"
#include "app.h"
#include "archive.h"
#include "archive_ui.h"
#include "drives.h"
#include "fileops.h"
#include "i18n.h"
#include "keyring.h"
#include "overview.h"
#include "proc.h"
#include "util.h"
#include "windows.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStatusBar>
#include <QVBoxLayout>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <future>
#include <memory>
#include <thread>
#include <vector>

using namespace util;

namespace diskimage {

// ---------------------------------------------------------------- devices (from /sys: no drive is asked anything)

static QString dev_name(const QString &dev) { return basename(realpath(dev)); }

QString disk_of(const QString &device)
{
    QString name = dev_name(device), sys = "/sys/class/block/" + name;
    if (exists(sys + "/partition"))
        return "/dev/" + basename(dirname(realpath(sys)));
    return "/dev/" + name;
}

// a block device's name, the disk it's a partition of, and (for device-mapper ones: LUKS, LVM) what it's made from,
// down to the disks
static QSet<QString> bases(const QString &name, int depth = 0)
{
    QSet<QString> out{name};
    if (depth > 8)
        return out;
    out << basename(disk_of("/dev/" + name));
    QDir slaves("/sys/class/block/" + name + "/slaves");
    for (const QString &s : slaves.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System))
        out += bases(s, depth + 1);
    return out;
}

struct MountLine {
    QString source, root;
    bool rw = false;
};

static QList<MountLine> mount_lines()   // /proc/self/mounts, with whether each is read-write
{
    QList<MountLine> out;
    auto unescape = [](QString s) {
        static const QRegularExpression oct("\\\\([0-7]{3})");
        QString r;
        int last = 0;
        for (auto it = oct.globalMatch(s); it.hasNext();) {
            auto m = it.next();
            r += s.mid(last, m.capturedStart() - last) + QChar(m.captured(1).toInt(nullptr, 8));
            last = int(m.capturedEnd());
        }
        return r + s.mid(last);
    };
    for (const QByteArray &line : read_file("/proc/self/mounts").split('\n')) {
        QList<QByteArray> f = line.split(' ');
        if (f.size() < 4)
            continue;
        MountLine m;
        m.source = unescape(QString::fromUtf8(f[0]));
        m.root = unescape(QString::fromUtf8(f[1]));
        m.rw = f[3].split(',').contains("rw");
        out << m;
    }
    return out;
}

QString refuse_reason(const QString &device, const QString &dest_dir)
{
    QString root = drives::mount_root(dest_dir), source;
    for (const MountLine &m : mount_lines())
        if (m.root == root)
            source = m.source;   // (the last one mounted there is what's seen)
    if (!source.startsWith("/dev/"))
        return QString();
    if (bases(dev_name(source)).contains(dev_name(disk_of(device))))
        return tx("“%1” is on the drive being read: the image would be written onto what's being copied. Save it on "
                  "another drive.").arg(dest_dir);
    return QString();
}

QStringList mounted_writable(const QString &device)
{
    QStringList out;
    QString name = dev_name(device);
    for (const MountLine &m : mount_lines())
        if (m.rw && m.source.startsWith("/dev/") && bases(dev_name(m.source)).contains(name))
            out << m.root;
    return out;
}

// ---------------------------------------------------------------- the image from a stream

using Stage = std::function<std::unique_ptr<proc::Process>(proc::Options)>;

// Programs a stream goes through, in order: the first reading `in` (a descriptor, or proc::PIPE for Kestrel to write
// to), each writing into the next, the last writing `out` (a descriptor, or proc::PIPE for Kestrel to read)
static std::vector<std::unique_ptr<proc::Process>> chain(const QList<Stage> &stages, int in, int out)
{
    std::vector<std::unique_ptr<proc::Process>> procs;
    int next_in = in;
    for (int i = 0; i < stages.size(); ++i) {
        proc::Options o;
        o.in = next_in;
        o.out = i == stages.size() - 1 ? out : proc::PIPE;
        o.err = proc::PIPE;
        o.c_utf8 = true;
        auto p = stages[i](o);
        if (!procs.empty())
            procs.back()->close_out();   // (the next one has it now)
        next_in = p->out;
        procs.push_back(std::move(p));
    }
    return procs;
}

// once the stream has ended: each program waited for, and an error if one failed (gpg's: archive::check_gpg)
static void finish_chain(std::vector<std::unique_ptr<proc::Process>> &procs, const QStringList &names)
{
    for (size_t i = 0; i < procs.size(); ++i) {
        procs[i]->wait();
        if (names.value(int(i)) == "gpg") {
            archive::check_gpg(procs[i].get());
        } else if (procs[i]->returncode != 0) {
            QString err = QString::fromUtf8(procs[i]->read_all_err()).trimmed();
            throw Error(err.isEmpty() ? tx("%1 failed (exit code %2)").arg(names.value(int(i)), QString::number(procs[i]->returncode))
                                      : names.value(int(i)) + ": " + err);
        }
    }
}

static QString progress_text(Task *t, qint64 done, qint64 total)
{
    QString base = total > 0 ? tx("%1 of %2").arg(human_size(done), human_size(total)) : human_size(done);
    QString speed = t->speed(done);
    return speed.isEmpty() ? base : tx("%1 (%2)").arg(base, speed);
}

void write_image(Task *t, int in_fd, qint64 total, const QString &dest, bool compress, const QString &password,
                 const std::function<bool()> &whole)
{
    QString part;
    int out = open_part_at(AT_FDCWD, dirname(dest) + "/", &part);
    ::fchmod(out, 0600);   // a drive's whole contents: for the user alone
    std::vector<std::unique_ptr<proc::Process>> procs;
    try {
        QList<Stage> stages;
        QStringList names;
        if (compress) {
            stages << [](proc::Options o) { return proc::spawn({"zstd", "-q", "-c", "-T0", "-3"}, o); };
            names << "zstd";
        }
        if (!password.isEmpty()) {
            stages << [password](proc::Options o) { return archive::spawn_gpg_encrypt(password, o); };
            names << "gpg";
        }
        if (!stages.isEmpty())
            procs = chain(stages, proc::PIPE, out);
        int sink = procs.empty() ? out : procs.front()->in;
        std::vector<char> buf(4 << 20);
        qint64 done = 0;
        for (;;) {
            t->check();
            ssize_t n = ::read(in_fd, buf.data(), buf.size());
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                throw_errno();
            if (n == 0)
                break;
            if (procs.empty()) {
                write_all(out, buf.data(), n, dest);
            } else {
                for (ssize_t off = 0; off < n;) {
                    ssize_t w = proc::write_pipe(sink, buf.data() + off, size_t(n - off));
                    if (w < 0 && errno == EINTR)
                        continue;
                    if (w < 0) {   // the program stopped taking it: its own error says why
                        procs.front()->close_in();
                        finish_chain(procs, names);
                        throw_errno(dest);
                    }
                    off += w;
                }
            }
            done += n;
            if (t->due())
                t->report(double(done), double(total > 0 ? total : 0), progress_text(t, done, total));
        }
        if (!procs.empty()) {
            procs.front()->close_in();
            finish_chain(procs, names);
        }
        if (!whole() || (total > 0 && done != total))
            throw Error(tx("The drive wasn't read to its end, so no image was made."));
        int fd = out;
        out = -1;
        if (::close(fd) != 0)
            throw_errno(dest);
        put_new(part, dest);   // (never over something that appeared meanwhile)
        t->log(tx("Wrote %1 from the drive into %2").arg(human_size(done), dest));
    } catch (...) {
        procs.clear();   // (stops any still running)
        if (out >= 0)
            ::close(out);
        ::unlink(enc(part).constData());
        throw;
    }
}

Task *start_backup(QWidget *parent, const Backup &b)
{
    QString device = realpath(b.device);
    qint64 total = drives::device_bytes(device);
    QStringList intro{tx("Back Up Drive to Image: %1 (%2) to %3")
                          .arg(device, total > 0 ? human_size(total) : tx("size unknown"), b.dest)};
    if (b.compress)
        intro << tx("Compressed with zstd");
    if (!b.password.isEmpty())
        intro << tx("Encrypted with a password (gpg, AES-256)");
    QPointer<QWidget> owner(parent);
    return fileops::queue_job(
        parent, tx("Backing up %1 to “%2”").arg(device, basename(b.dest)), {dirname(b.dest)},
        [b, device, total](Task *t) -> QVariant {
            // the FIFO the helper writes the device into, in a folder of the user's own
            QString base = qEnvironmentVariable("XDG_RUNTIME_DIR");
            if (base.isEmpty() || !isdir(base))
                base = QDir::tempPath();
            QByteArray tmpl = enc(join(base, "kes-image-XXXXXX"));
            if (!::mkdtemp(tmpl.data()))
                throw_errno(base);
            QString dir = dec(tmpl.constData()), fifo = join(dir, "stream");
            struct Cleanup {
                QString dir, fifo;
                ~Cleanup()
                {
                    ::unlink(enc(fifo).constData());
                    ::rmdir(enc(dir).constData());
                }
            } cleanup{dir, fifo};
            if (::mkfifo(enc(fifo).constData(), 0600) != 0)
                throw_errno(fifo);
            std::promise<bool> answered;
            std::shared_future<bool> helper_ok = answered.get_future().share();
            std::exception_ptr made_err;
            std::atomic<bool> reader_done{false};
            std::thread reader([&]() {
                int fd = ::open(enc(fifo).constData(), O_RDONLY | O_CLOEXEC);   // (waits for the helper to open it)
                try {
                    if (fd < 0)
                        throw_errno(fifo);
                    write_image(t, fd, total, b.dest, b.compress, b.password, [helper_ok]() { return helper_ok.get(); });
                } catch (...) {
                    made_err = std::current_exception();
                }
                if (fd >= 0)
                    ::close(fd);
                reader_done = true;
            });
            std::exception_ptr read_err;
            try {
                admin::session().call(t, "readdev", {{"device", device}, {"fifo", fifo}});
            } catch (...) {
                read_err = std::current_exception();
            }
            answered.set_value(!read_err);
            while (read_err && !reader_done) {   // a reader still waiting for the helper to open the FIFO: let it go
                int w = ::open(enc(fifo).constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
                if (w >= 0)
                    ::close(w);
                ::usleep(20000);
            }
            reader.join();
            if (t->cancelled)
                throw Cancelled();
            if (read_err)
                std::rethrow_exception(read_err);
            if (made_err)
                std::rethrow_exception(made_err);
            return QVariant(b.dest);
        },
        [owner, b, device](const QVariant &res) {
            if (owner && !res.toString().isEmpty())
                inform(owner, tx("Back Up Drive to Image"), tx("The image of %1 is saved as “%2”.").arg(device, b.dest));
        },
        true, {}, false, intro, true);
}

// ---------------------------------------------------------------- the Back Up window

class BackupDialog : public QDialog {
public:
    BackupDialog(QWidget *parent, const QString &device);

private:
    QComboBox *what;
    QLineEdit *folder, *name, *pw, *pw2;
    QCheckBox *compress, *encrypt;
    QLabel *info;
    QWidget *pw_rows;
    QString label;
    bool name_theirs = false;   // typed by the user: no longer changed for them
    QString chosen() const { return what->currentData().toString(); }
    void update_name();
    void update_info();
    void go();
};

BackupDialog::BackupDialog(QWidget *parent, const QString &device) : QDialog(parent)
{
    setObjectName("backup_drive");
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tx("Back Up Drive to Image"));
    QString dev = realpath(device), disk = disk_of(dev);
    label = drives::device_label(dev);
    auto *lay = new QVBoxLayout(this);
    auto *intro = new QLabel(tx("Copies everything on the drive, byte for byte, into an image file, which Mount Image "
                                "can open later. Reading the drive needs your administrator password."));
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto *form = new QFormLayout;
    what = new QComboBox;
    auto size_of = [](const QString &d) {
        qint64 n = drives::device_bytes(d);
        return n > 0 ? human_size(n) : tx("size unknown");
    };
    if (disk != dev) {
        what->addItem(tx("This partition: %1 (%2)").arg(dev, size_of(dev)), dev);
        what->addItem(tx("The whole disk: %1 (%2)").arg(disk, size_of(disk)), disk);
    } else {
        what->addItem(tx("%1 (%2)").arg(dev, size_of(dev)), dev);
    }
    form->addRow(tx("Copy:"), what);
    folder = new QLineEdit(HOME());
    auto *browse = new QPushButton(tx("Choose…"));
    auto *frow = new QHBoxLayout;
    frow->addWidget(folder, 1);
    frow->addWidget(browse);
    form->addRow(tx("Save in:"), frow);
    name = new QLineEdit;
    form->addRow(tx("Name:"), name);
    compress = new QCheckBox(tx("Compress (zstd): smaller, as the drive's empty space takes almost nothing"));
    if (!which("zstd")) {
        compress->setEnabled(false);
        compress->setToolTip(tx("Install zstd to compress images"));
    }
    form->addRow(QString(), compress);
    encrypt = new QCheckBox(tx("Encrypt with a password (gpg): the image is never on the disk unencrypted"));
    if (!which("gpg")) {
        encrypt->setEnabled(false);
        encrypt->setToolTip(tx("Install gpg to encrypt images"));
    }
    form->addRow(QString(), encrypt);
    pw_rows = new QWidget;
    auto *pf = new QFormLayout(pw_rows);
    pf->setContentsMargins(0, 0, 0, 0);
    pw = new QLineEdit;
    pw->setEchoMode(QLineEdit::Password);
    pw2 = new QLineEdit;
    pw2->setEchoMode(QLineEdit::Password);
    pf->addRow(tx("Password:"), pw);
    pf->addRow(tx("Again:"), pw2);
    pw_rows->hide();
    form->addRow(QString(), pw_rows);
    lay->addLayout(form);
    info = new QLabel;
    info->setWordWrap(true);
    lay->addWidget(info);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(tx("Back Up"));
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &BackupDialog::go);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(browse, &QPushButton::clicked, this, [this]() {
        QPointer<BackupDialog> self(this);
        choose_folder(this, tx("Save the Image In"), folder->text(), [self](const QString &d) {
            if (self) {
                self->folder->setText(d);
                self->update_info();
            }
        });
    });
    connect(folder, &QLineEdit::textChanged, this, &BackupDialog::update_info);
    connect(name, &QLineEdit::textEdited, this, [this]() { name_theirs = true; });
    connect(what, &QComboBox::currentIndexChanged, this, [this]() {
        update_name();
        update_info();
    });
    for (QCheckBox *c : {compress, encrypt})
        connect(c, &QCheckBox::toggled, this, [this]() {
            pw_rows->setVisible(encrypt->isChecked());
            update_name();
            update_info();
        });
    update_name();
    update_info();
    resize(560, sizeHint().height());
}

void BackupDialog::update_name()
{
    if (name_theirs)
        return;
    QString base = label.isEmpty() || chosen() != what->itemData(0).toString() ? basename(chosen()) : label;
    base.replace('/', '-');
    // (a date in the file name: sortable, the same in every language)
    QString n = base + "-" + QDate::currentDate().toString("yyyy-MM-dd") + ".img";
    if (compress->isChecked())
        n += ".zst";
    if (encrypt->isChecked())
        n += ".gpg";
    name->setText(n);
}

void BackupDialog::update_info()
{
    qint64 size = drives::device_bytes(chosen()), total = 0, free = -1;
    QStringList lines;
    if (drives::statfs_(folder->text(), &total, &free) && free >= 0 && size > 0)
        lines << tx("The drive holds %1; %2 is free there.").arg(human_size(size), human_size(free));
    QString why = refuse_reason(chosen(), folder->text());
    if (!why.isEmpty())
        lines << why;
    QStringList rw = mounted_writable(chosen());
    if (!rw.isEmpty())
        lines << tx("Mounted for writing: %1. Unmounting it first gives the most faithful copy.").arg(rw.join(", "));
    info->setText(lines.join("\n"));
}

void BackupDialog::go()
{
    const QString title = tx("Back Up Drive to Image");
    QString dev = chosen(), dir = folder->text().trimmed(), n = name->text().trimmed();
    if (!admin::AdminSession::available()) {
        warn(this, title, tx("Reading a drive needs pkexec. Install it with:\n\nsudo apt install pkexec"));
        return;
    }
    if (!isdir(dir)) {
        warn(this, title, tx("“%1” isn't a folder.").arg(dir));
        return;
    }
    if (n.isEmpty() || n.contains('/') || n == "." || n == "..") {
        warn(this, title, tx("That name can't be used."));
        return;
    }
    QString dest = join(dir, n);
    if (lexists(dest)) {
        warn(this, title, tx("“%1” is already there. Choose another name.").arg(dest));
        return;
    }
    QString why = refuse_reason(dev, dir);
    if (!why.isEmpty()) {
        warn(this, title, why);
        return;
    }
    if (encrypt->isChecked() && pw->text().isEmpty()) {
        warn(this, title, tx("Enter the password to encrypt the image with."));
        return;
    }
    if (encrypt->isChecked() && pw->text() != pw2->text()) {
        warn(this, title, tx("The two passwords aren't the same."));
        return;
    }
    qint64 size = drives::device_bytes(dev), total = 0, free = -1;
    drives::statfs_(dir, &total, &free);
    QStringList asks;
    if (size > 0 && free >= 0 && free < size) {
        if (!compress->isChecked()) {
            warn(this, title, tx("There isn't room for the image there: it takes %1, and %2 is free.")
                                  .arg(human_size(size), human_size(free)));
            return;
        }
        asks << tx("Compressed, the image's size can't be known beforehand: it may not fit in the %1 free there (the "
                   "drive holds %2).").arg(human_size(free), human_size(size));
    }
    QStringList rw = mounted_writable(dev);
    if (!rw.isEmpty())
        asks << tx("It's mounted for writing (%1): programs may change it while it's read, and the image could then be "
                   "inconsistent. Unmounting it first, or mounting it read-only, is safer.").arg(rw.join(", "));
    Backup b{dev, dest, compress->isChecked(), encrypt->isChecked() ? pw->text() : QString()};
    QPointer<BackupDialog> self(this);
    auto start = [self, b]() {
        if (!self)
            return;
        start_backup(self->parentWidget() ? self->parentWidget() : self.data(), b);
        self->close();
    };
    if (asks.isEmpty())
        start();
    else
        ask_yes(this, title, tx("%1\n\nBack up anyway?").arg(asks.join("\n\n")), start);
}

void backup_window(QWidget *parent, const QString &device)
{
    if (!device.startsWith("/dev/") || !exists(device)) {
        warn(parent, tx("Back Up Drive to Image"), tx("Kestrel can't tell which device this drive is."));
        return;
    }
    show_free(new BackupDialog(parent, device));
}

// ---------------------------------------------------------------- mounting images

static const QStringList BASES = {".img", ".iso", ".raw"}, PACKED = {".zst", ".xz", ".gz"};

// what an image's name says: compressed (".zst"…, or empty), encrypted (.gpg), and its name once expanded
static bool parse(const QString &path, QString *packed = nullptr, bool *gpg = nullptr, QString *inner = nullptr)
{
    QString n = basename(path), l = n.toLower();
    bool g = l.endsWith(".gpg");
    if (g) {
        n.chop(4);
        l.chop(4);
    }
    QString p;
    for (const QString &x : PACKED)
        if (l.endsWith(x)) {
            p = x;
            n.chop(int(x.size()));
            l.chop(int(x.size()));
        }
    bool image = std::any_of(BASES.begin(), BASES.end(), [&l](const QString &b) { return l.endsWith(b) && l.size() > b.size(); });
    if (packed)
        *packed = p;
    if (gpg)
        *gpg = g;
    if (inner)
        *inner = n;
    return image;
}

bool is_image(const QString &path) { return parse(path); }

bool needs_expanding(const QString &path)
{
    QString packed;
    bool gpg = false;
    return parse(path, &packed, &gpg) && (gpg || !packed.isEmpty());
}

bool can_write(const QString &path)
{
    return is_image(path) && !needs_expanding(path) && !path.toLower().endsWith(".iso") && util::access(path, W_OK);
}

QString cache_dir() { return join(APP_CACHE(), "images"); }

static QSet<QString> setting_up;   // loop devices a Mounter is still mounting (tidy() leaves them alone)
static QSet<QString> tearing;      // being detached
static QHash<QString, QString> ours;   // loop devices this Kestrel set up and mounted something from: their image

// what's on a loop device: itself (a file system on the whole image, an .iso) or its partitions, each with its type
static QList<QPair<QString, QString>> blocks_of(const QString &loop)
{
    QString name = basename(loop);
    QString whole = overview::udev_fstype(loop);
    if (!whole.isEmpty())
        return {{loop, whole}};
    QList<QPair<QString, QString>> out;
    QStringList parts = QDir("/sys/class/block/" + name).entryList({name + "p*"}, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
    natural_sort(parts);
    for (const QString &p : parts)
        out << qMakePair("/dev/" + p, overview::udev_fstype("/dev/" + p));
    return out;
}

static QString udisks_error(const proc::Result &r)
{
    QString e = QString::fromUtf8(r.err).trimmed();
    if (e.isEmpty())
        e = QString::fromUtf8(r.out).trimmed();
    // "Error mounting /dev/loop0p1: GDBus.Error:org.freedesktop.UDisks2.Error.Failed: …": the part after the last ": "
    static const QRegularExpression dbus("GDBus\\.Error:[\\w.]+:\\s*");
    e.remove(dbus);
    return e.isEmpty() ? tx("udisksctl failed (exit code %1)").arg(r.rc) : e;
}

// detach a loop device: its LUKS file systems locked first (what holds a block open), then the loop; an expanded copy
// in the cache deleted. On a worker.
static void teardown(const QString &loop, const QString &backing)
{
    QString name = basename(loop);
    QStringList blocks{name};
    blocks << QDir("/sys/class/block/" + name).entryList({name + "p*"}, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
    for (const QString &b : blocks)
        if (!QDir("/sys/class/block/" + b + "/holders").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System).isEmpty())
            proc::run({"udisksctl", "lock", "-b", "/dev/" + b}, 60000);
    if (exists("/sys/block/" + name + "/loop"))
        proc::run({"udisksctl", "loop-delete", "-b", loop}, 60000);
    if (!backing.isEmpty() && dirname(backing) == cache_dir())
        ::unlink(enc(backing).constData());
}

// anything from this loop device mounted (its partitions, or LUKS opened on them)
static bool in_use(const QString &loop, const QList<MountLine> &mounts)
{
    QString name = basename(loop);
    for (const MountLine &m : mounts)
        if (m.source.startsWith("/dev/")) {
            QString src = dev_name(m.source);
            QSet<QString> b = bases(src);
            for (const QString &x : b)
                if (x == name || x.startsWith(name + "p"))
                    return true;
        }
    return false;
}

void tidy()
{
    // detached by the kernel already (autoclear): an expanded copy goes now
    for (auto it = ours.begin(); it != ours.end();) {
        if (!exists("/sys/block/" + basename(it.key()) + "/loop")) {
            if (dirname(it.value()) == cache_dir())
                ::unlink(enc(it.value()).constData());
            it = ours.erase(it);
        } else {
            ++it;
        }
    }
    QList<MountLine> mounts = mount_lines();
    QStringList loops = QDir("/sys/block").entryList({"loop*"}, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
    QSet<QString> backing_files;
    for (const QString &n : loops) {
        QString loop = "/dev/" + n;
        QString backing = drives::user_loop(loop);
        if (backing.isEmpty())
            continue;
        backing_files << backing;
        bool cached = dirname(backing) == cache_dir();
        if (setting_up.contains(loop) || tearing.contains(loop) || (!ours.contains(loop) && !cached))
            continue;
        if (in_use(loop, mounts))
            continue;
        // a copy another Kestrel has only just set up, not mounted yet: left to it
        struct stat st;
        if (!ours.contains(loop) && stat_(backing, st) && QDateTime::currentSecsSinceEpoch() - st.st_mtime < 120)
            continue;
        tearing << loop;
        ours.remove(loop);
        fileops::run_task(
            nullptr, tx("Detaching %1").arg(basename(backing)),
            [loop, backing]() {
                teardown(loop, backing);
                return QVariant();
            },
            [loop](const QVariant &) { tearing.remove(loop); }, true);
    }
    // copies left from before (a Kestrel that ended with an image mounted, then it was unmounted)
    for (const QString &n : QDir(cache_dir()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot)) {
        QString p = join(cache_dir(), n);
        struct stat st;
        if (!n.startsWith(".kes-") && !backing_files.contains(p) && stat_(p, st) &&
            QDateTime::currentSecsSinceEpoch() - st.st_mtime >= 120)
            ::unlink(enc(p).constData());
    }
}

// One Mount Image, step by step (each step that waits for the system on a worker, each question in a free window)
class Mounter : public QObject {
public:
    Mounter(MainWindow *w, const QString &path, bool writable) : QObject(w), win(w), path(path), writable(writable) {}
    void start();

private:
    QPointer<MainWindow> win;
    QString path, file, loop;
    bool writable, cached = false, keyring_ok = false;
    QString saved;   // a password saved in the keyring for it
    QList<QPair<QString, QString>> blocks;
    int at = 0;
    QStringList roots, errors;
    QString title() const { return tx("Mount Image"); }
    void password(archive_ui::Ask reason);
    void expand(const QString &pw, bool remember, bool from_saved);
    void setup();
    void next();
    void unlock(const QString &dev, const QString &pw, bool remember, bool from_saved);
    void mount(const QString &dev);
    void finish();
    void done() { deleteLater(); }
    void look_up(std::function<void()> then);   // the keyring: is there one, and a password saved for this image
};

void Mounter::look_up(std::function<void()> then)
{
    QPointer<Mounter> self(this);
    QString p = path;
    fileops::run_task(
        win, tx("Looking for a saved password"),
        [p]() {
            bool ok = keyring::available();
            return QVariant(QVariantList{ok, ok ? keyring::lookup(keyring::fingerprint(p)) : QString()});
        },
        [self, then](const QVariant &res) {
            if (!self)
                return;
            self->keyring_ok = res.toList().value(0).toBool();
            self->saved = res.toList().value(1).toString();
            then();
        },
        true);
}

void Mounter::start()
{
    if (!which("udisksctl")) {
        warn(win, title(), tx("Mounting disk images needs udisks2 (udisksctl). Install it with:\n\nsudo apt install udisks2"));
        return done();
    }
    if (!needs_expanding(path)) {
        file = path;
        return setup();
    }
    QString packed, inner;
    bool gpg = false;
    parse(path, &packed, &gpg, &inner);
    for (const QString &tool : {gpg ? QString("gpg") : QString(), packed == ".zst" ? QString("zstd") : packed == ".xz" ? QString("xz") : packed == ".gz" ? QString("gzip") : QString()})
        if (!tool.isEmpty() && !which(tool)) {
            warn(win, title(), tx("Opening “%1” needs %2, which isn't installed.").arg(basename(path), tool));
            return done();
        }
    makedirs(cache_dir(), true, 0700);
    qint64 total = 0, free = -1, size = 0;
    drives::statfs_(cache_dir(), &total, &free);
    struct stat st;
    if (stat_(path, st))
        size = st.st_size;
    QString text = gpg ? tx("“%1” is encrypted. To mount it, Kestrel decrypts it into a copy in your cache folder (%2 "
                            "free there; the image takes %3, and more once expanded if it's also compressed). The copy "
                            "isn't encrypted while it's there; it's mounted read-only and deleted once unmounted.")
                             .arg(basename(path), human_size(free), human_size(size))
                       : tx("“%1” is compressed. To mount it, Kestrel expands it into a copy in your cache folder (%2 "
                            "free there; it may need several times the %3 it takes now). The copy is mounted read-only "
                            "and deleted once unmounted.").arg(basename(path), human_size(free), human_size(size));
    QPointer<Mounter> self(this);
    auto *box = ask(win, QMessageBox::Question, title(), text, QMessageBox::Ok | QMessageBox::Cancel,
                    [self, gpg](QMessageBox::StandardButton b) {
                        if (!self)
                            return;
                        if (b != QMessageBox::Ok)
                            return self->done();
                        if (!gpg)
                            return self->expand(QString(), false, false);
                        self->look_up([self]() {
                            if (!self)
                                return;
                            if (!self->saved.isEmpty())
                                self->expand(self->saved, false, true);
                            else
                                self->password(archive_ui::Ask::First);
                        });
                    });
    Q_UNUSED(box);
}

void Mounter::password(archive_ui::Ask reason)
{
    QPointer<Mounter> self(this);
    archive_ui::ask_password(
        win, path, reason, keyring_ok,
        [self](const QString &pw, bool remember) {
            if (self)
                self->expand(pw, remember, false);
        },
        [self]() {
            if (self)
                self->done();
        },
        tx("Mount"));
}

void Mounter::expand(const QString &pw, bool remember, bool from_saved)
{
    QString packed, inner;
    bool gpg = false;
    parse(path, &packed, &gpg, &inner);
    QPointer<Mounter> self(this);
    QString src = path;
    fileops::queue_job(
        win, tx("Expanding “%1” to mount it").arg(basename(path)), {path, cache_dir()},
        [src, packed, gpg, inner, pw, remember](Task *t) -> QVariant {
            QString part;
            int out = open_part_at(AT_FDCWD, cache_dir() + "/", &part);
            ::fchmod(out, 0600);
            std::vector<std::unique_ptr<proc::Process>> procs;
            QStringList names;
            int in_fd = -1;
            try {
                if (gpg) {
                    procs.push_back(archive::spawn_gpg_decrypt(src, pw));
                    names << "gpg";
                }
                if (!packed.isEmpty()) {
                    QStringList argv = packed == ".zst" ? QStringList{"zstd", "-d", "-q", "-c"}
                                       : packed == ".xz" ? QStringList{"xz", "-d", "-c"}
                                                         : QStringList{"gzip", "-d", "-c"};
                    proc::Options o;
                    if (gpg) {
                        o.in = procs.back()->out;
                    } else {
                        in_fd = ::open(enc(src).constData(), O_RDONLY | O_CLOEXEC);
                        if (in_fd < 0)
                            throw_errno(src);
                        o.in = in_fd;
                    }
                    o.out = proc::PIPE;
                    o.err = proc::PIPE;
                    o.c_utf8 = true;
                    procs.push_back(proc::spawn(argv, o));
                    names << argv.first();
                    if (gpg)
                        procs.front()->close_out();
                    if (in_fd >= 0) {
                        ::close(in_fd);
                        in_fd = -1;
                    }
                }
                int from = procs.back()->out;
                std::vector<char> buf(4 << 20);
                qint64 done = 0;
                for (;;) {
                    t->check();
                    ssize_t n = ::read(from, buf.data(), buf.size());
                    if (n < 0 && errno == EINTR)
                        continue;
                    if (n < 0)
                        throw_errno();
                    if (n == 0)
                        break;
                    write_all(out, buf.data(), n, part);
                    done += n;
                    if (t->due())
                        t->report(0, 0, progress_text(t, done, -1));
                }
                procs.back()->close_out();
                finish_chain(procs, names);
                int fd = out;
                out = -1;
                if (::close(fd) != 0)
                    throw_errno(part);
                QString final = unique_path(cache_dir(), inner, "num");
                put_new(part, final);
                t->log(tx("Expanded into %1 (%2)").arg(final, human_size(done)));
                if (remember)
                    keyring::store(keyring::fingerprint(src), basename(src), pw);
                return QVariantMap{{"file", final}};
            } catch (const archive::WrongPassword &) {
                procs.clear();
                if (out >= 0)
                    ::close(out);
                ::unlink(enc(part).constData());
                return QVariantMap{{"wrong", true}};
            } catch (...) {
                procs.clear();
                if (in_fd >= 0)
                    ::close(in_fd);
                if (out >= 0)
                    ::close(out);
                ::unlink(enc(part).constData());
                throw;
            }
        },
        [self, from_saved](const QVariant &res) {
            if (!self)
                return;
            QVariantMap m = res.toMap();
            if (m.value("wrong").toBool())
                return self->password(from_saved ? archive_ui::Ask::First : archive_ui::Ask::Wrong);
            if (m.value("file").toString().isEmpty())
                return self->done();   // (cancelled, or the job said what failed)
            self->file = m.value("file").toString();
            self->cached = true;
            self->setup();
        },
        true, {}, false, {}, true);
}

void Mounter::setup()
{
    QPointer<Mounter> self(this);
    QString f = file;
    bool ro = !writable || cached;
    fileops::run_task(
        win, tx("Setting up “%1”").arg(basename(path)),
        [f, ro]() -> QVariant {
            QStringList argv{"udisksctl", "loop-setup", "-f", f};
            if (ro)
                argv << "-r";
            proc::Result r = proc::run(argv, 120000);
            static const QRegularExpression loop_re("(/dev/loop\\d+)");
            QRegularExpressionMatch m = loop_re.match(QString::fromUtf8(r.out));
            if (r.rc != 0 || !m.hasMatch())
                return QVariantMap{{"error", udisks_error(r)}};
            QString loop = m.captured(1);
            // detached by the kernel once nothing uses it any more (also if Kestrel isn't running then)
            if (which("gdbus"))
                proc::run({"gdbus", "call", "--system", "--dest", "org.freedesktop.UDisks2", "--object-path",
                           "/org/freedesktop/UDisks2/block_devices/" + basename(loop), "--method",
                           "org.freedesktop.UDisks2.Loop.SetAutoclear", "true", "{}"},
                          20000);
            if (which("udevadm"))
                proc::run({"udevadm", "settle", "-t", "5"}, 10000);
            QVariantList blocks;
            for (int i = 0; i < 30; ++i) {   // its partitions appear a moment after
                auto bl = blocks_of(loop);
                bool typed = std::any_of(bl.begin(), bl.end(), [](const QPair<QString, QString> &b) { return !b.second.isEmpty(); });
                if (typed || i == 29) {
                    for (const auto &[dev, fs] : bl)
                        blocks << QVariantList{dev, fs};
                    break;
                }
                ::usleep(100000);
            }
            return QVariantMap{{"loop", loop}, {"blocks", blocks}};
        },
        [self](const QVariant &res) {
            if (!self)
                return;
            QVariantMap m = res.toMap();
            if (m.contains("error") || m.value("loop").toString().isEmpty()) {
                warn(self->win, self->title(), tx("“%1” couldn't be set up as a drive:\n\n%2")
                                                   .arg(basename(self->path), m.value("error").toString()));
                if (self->cached)
                    ::unlink(enc(self->file).constData());
                return self->done();
            }
            self->loop = m.value("loop").toString();
            setting_up << self->loop;
            for (const QVariant &b : m.value("blocks").toList()) {
                QString fs = b.toList().value(1).toString();
                if (!fs.isEmpty() && fs != "swap" && fs != "LVM2_member")   // (nothing to mount on those)
                    self->blocks << qMakePair(b.toList().value(0).toString(), fs);
            }
            self->next();
        });
}

void Mounter::next()
{
    if (at >= blocks.size())
        return finish();
    auto [dev, fs] = blocks[at];
    if (fs != "crypto_LUKS")
        return mount(dev);
    QPointer<Mounter> self(this);
    auto ask_it = [self, dev](archive_ui::Ask reason) {
        if (!self)
            return;
        archive_ui::ask_password(
            self->win, self->path, reason, self->keyring_ok,
            [self, dev](const QString &pw, bool remember) {
                if (self)
                    self->unlock(dev, pw, remember, false);
            },
            [self]() {
                if (!self)
                    return;
                self->errors << tx("%1: not unlocked").arg(self->blocks[self->at].first);
                self->at++;
                self->next();
            },
            tx("Unlock"));
    };
    look_up([self, dev, ask_it]() {
        if (!self)
            return;
        if (!self->saved.isEmpty())
            self->unlock(dev, self->saved, false, true);
        else
            ask_it(archive_ui::Ask::First);
    });
}

void Mounter::unlock(const QString &dev, const QString &pw, bool remember, bool from_saved)
{
    QPointer<Mounter> self(this);
    QString p = path;
    fileops::run_task(
        win, tx("Unlocking %1").arg(dev),
        [dev, pw, remember, p]() -> QVariant {
            // the password on udisksctl's stdin (never on a command line)
            proc::Result r = proc::run({"udisksctl", "unlock", "-b", dev, "--key-file", "/dev/stdin"}, 120000,
                                       proc::Options(), pw.toUtf8());
            static const QRegularExpression dm_re(" as (/dev/\\S+?)\\.?\\s*$");
            QRegularExpressionMatch m = dm_re.match(QString::fromUtf8(r.out).trimmed());
            if (r.rc != 0 || !m.hasMatch()) {
                QString e = udisks_error(r);
                bool wrong = e.contains("passphrase", Qt::CaseInsensitive) || e.contains("No key available", Qt::CaseInsensitive);
                return QVariantMap{{"error", e}, {"wrong", wrong}};
            }
            if (remember)
                keyring::store(keyring::fingerprint(p), basename(p), pw);
            return QVariantMap{{"dev", m.captured(1)}};
        },
        [self, from_saved](const QVariant &res) {
            if (!self)
                return;
            QVariantMap m = res.toMap();
            if (m.value("wrong").toBool()) {
                QString dev = self->blocks[self->at].first;
                self->saved.clear();   // (asked from now on)
                archive_ui::ask_password(
                    self->win, self->path, from_saved ? archive_ui::Ask::First : archive_ui::Ask::Wrong,
                    self->keyring_ok,
                    [self, dev](const QString &pw, bool remember) {
                        if (self)
                            self->unlock(dev, pw, remember, false);
                    },
                    [self]() {
                        if (!self)
                            return;
                        self->errors << tx("%1: not unlocked").arg(self->blocks[self->at].first);
                        self->at++;
                        self->next();
                    },
                    tx("Unlock"));
                return;
            }
            if (m.contains("error")) {
                self->errors << self->blocks[self->at].first + ": " + m.value("error").toString();
                self->at++;
                return self->next();
            }
            self->mount(m.value("dev").toString());
        });
}

void Mounter::mount(const QString &dev)
{
    QPointer<Mounter> self(this);
    bool ro = !writable || cached;
    fileops::run_task(
        win, tx("Mounting %1").arg(dev),
        [dev, ro]() -> QVariant {
            QStringList argv{"udisksctl", "mount", "-b", dev};
            if (ro)
                argv << "-o" << "ro";
            proc::Result r = proc::run(argv, 120000);
            static const QRegularExpression at_re(" at (.+?)\\.?\\s*$");
            QRegularExpressionMatch m = at_re.match(QString::fromUtf8(r.out).trimmed());
            if (r.rc != 0 || !m.hasMatch())
                return QVariantMap{{"error", udisks_error(r)}};
            return QVariantMap{{"root", m.captured(1)}};
        },
        [self, dev](const QVariant &res) {
            if (!self)
                return;
            QVariantMap m = res.toMap();
            if (m.contains("root"))
                self->roots << m.value("root").toString();
            else
                self->errors << dev + ": " + m.value("error").toString();
            self->at++;
            self->next();
        });
}

void Mounter::finish()
{
    setting_up.remove(loop);
    if (roots.isEmpty()) {
        QString why = errors.isEmpty() ? tx("No file system Kestrel can mount was found in it.") : errors.join('\n');
        warn(win, title(), tx("Nothing in “%1” could be mounted:\n\n%2").arg(basename(path), why));
        QString l = loop, f = cached ? file : QString();
        tearing << l;
        fileops::run_task(
            nullptr, tx("Detaching %1").arg(basename(path)),
            [l, f]() {
                teardown(l, f);
                return QVariant();
            },
            [l](const QVariant &) { tearing.remove(l); }, true);
        return done();
    }
    ours.insert(loop, file);
    if (!errors.isEmpty())
        warn(win, title(), tx("Some of “%1” couldn't be mounted:\n\n%2").arg(basename(path), errors.join('\n')));
    drives::reread_mounts();
    if (win) {
        win->open_location(roots.first());
        win->statusBar()->showMessage(txn("Mounted %Ln file system(s) from “%1”", roots.size()).arg(basename(path)), 6000);
    }
    done();
}

void mount_image(MainWindow *win, const QString &path, bool writable)
{
    (new Mounter(win, path, writable && can_write(path)))->start();
}

}  // namespace diskimage
