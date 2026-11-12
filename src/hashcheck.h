// Checksum files (.sfv, .md5, .sha256, SHA256SUMS…): reading them, checking the files they list, and the Verify
// Checksums dialog that opening one shows; making them (Create Checksum File…).
#pragma once

#include <QDialog>
#include <QList>
#include <QMutex>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

class MainWindow;
class QCryptographicHash;
class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QRadioButton;
class QPushButton;
class QTreeWidget;
class Task;

namespace hashcheck {

// One file a checksum file lists: its path (resolved against the checksum file's folder), the hash it should have
// (lowercase hex) and the algorithm: "crc32", "md5", "sha1", "sha224", "sha256", "sha384", "sha512" or "blake2b".
struct Entry {
    QString path, name, expected, algo;
    qint64 size = -1;   // read with the checksum file, in the background (-1: not known)
};

bool is_hash_file(const QString &path);   // by its name: .sfv, .md5, .sha256, …, SHA256SUMS, …-CHECKSUM
QString file_filter();                    // for a file dialog: checksum files, all files
// The entries in a checksum file, in its order; lines that aren't checksums (comments, a PGP signature) are skipped.
// A file holding only a hash is for the file of the same name without the extension (disk.iso.sha256 → disk.iso).
QList<Entry> parse(const QString &path);
QList<Entry> parse_text(const QString &text, const QString &path);   // the same, for text read from `path`
// The entry for `file` in a checksum file's entries: by its path, else by its name; nullptr if it isn't listed.
const Entry *find(const QList<Entry> &entries, const QString &file);
// The file's hash (lowercase hex). On a thread: progress(bytes done, size) after each chunk, check() between them
// (it may throw to stop). Throws OSError if it can't be read.
QString hash_file(const QString &path, const QString &algo, const std::function<void(qint64, qint64)> &progress = nullptr,
                  const std::function<void()> &check = nullptr);
// The same, for several algorithms in one read of the file (hashes in `algos`' order).
QStringList hash_files(const QString &path, const QStringList &algos,
                       const std::function<void(qint64, qint64)> &progress = nullptr,
                       const std::function<void()> &check = nullptr);
QString algo_label(const QString &algo);   // "SHA256", "CRC32", "BLAKE2b"
// Hashes of data given piece by piece, with several algorithms at once (bytes on their way somewhere: an archive as it's
// written). Not thread-safe.
class Hasher {
public:
    explicit Hasher(const QStringList &algos);
    ~Hasher();
    void add(const char *data, qint64 n);
    QStringList results();   // lowercase hex, in algos' order
private:
    std::vector<std::unique_ptr<QCryptographicHash>> hashes;   // nullptr for CRC32
    quint32 crc = 0;
};

// Each listed file's result: "ok", "failed", "missing" or "error" (with the message).
struct Result {
    QString status, actual, error;
};
// task: progress and a log line for it. before, total: bytes already checked and in all (a job checking several
// files shows its progress over all of them); total 0: this file's own.
Result verify(const Entry &e, Task *task = nullptr, qint64 before = 0, qint64 total = 0);

// What a checksum file's check has found so far, shared by its job and the window showing it (the job fills it).
struct VerifyState {
    QMutex mtx;
    QList<Result> results;   // in entries' order; empty status while not checked yet
};
// Checks every file a checksum file lists, in order, as one job: each file's result in its log, the ones that failed
// listed at its end (and the job ends "done with errors"). Not tied to a window: closing the one that started it (or
// its Kestrel window) doesn't stop it, and it keeps Kestrel running until it ends, as copies do. parent: whose status
// bar shows it. state (may be null) gets each result as it's known. on_done(valid: finished, invalid: stopped) runs
// on the UI thread, also when parent has gone (it mustn't need it).
Task *verify_job(QWidget *parent, const QString &path, const QList<Entry> &entries, std::shared_ptr<VerifyState> state,
                 std::function<void(const QVariant &)> on_done = nullptr);

// Shows the checksum file's entries and the results of their check (verify_job), with progress; Verify Again, Stop,
// Close. The window only shows the job: closing it, or the Kestrel window it belongs to, leaves the job running (the
// Jobs window has it and its log).
class VerifyDialog : public QDialog {
    Q_OBJECT
public:
    VerifyDialog(QWidget *parent, const QString &path, const QList<Entry> &entries);
    QList<Result> results;   // in entries' order; empty status while not checked yet
    bool complete() const { return finished; }   // every file checked
    Task *job() const { return task; }           // the check running (nullptr once it has ended)

private:
    void start();
    void update_rows();   // the results the job has found since the last time
    void on_done(const QVariant &res);
    void show_row(int i);
    void summary();

    QString path;
    QList<Entry> entries;
    std::shared_ptr<VerifyState> state;
    bool finished = false;
    QPointer<Task> task;
    QTreeWidget *tree;
    QLabel *status;
    QProgressBar *bar;
    QPushButton *again, *stop_btn;
};

// Verify Checksums for path: the file is read and parsed on a worker (a checksum file on a tape or floppy can take long to
// read), then the window opens; one that lists nothing calls not_listing() instead (it opens as an ordinary file).
void open_dialog(QWidget *parent, const QString &path, std::function<void()> not_listing);

// ---- making checksum files

const QStringList &create_algorithms();   // offered, in order: crc32, md5, sha1, sha256, sha512, blake2b
// The files to checksum: files as they are, folders' files inside them (not through symlinked folders), in order.
QStringList files_in(const QStringList &paths, const std::function<void()> &check = nullptr);
// The checksum files a job writes in `dir`: one per algorithm (stem.sha256, stem.md5, stem.sfv for CRC32…), or with
// one_file a single stem-CHECKSUM with every algorithm as BSD tags.
QStringList output_paths(const QString &dir, const QString &stem, const QStringList &algos, bool one_file);
// One line of a checksum file: "hash  name" (md5sum's), "SHA256 (name) = hash" (bsd) or "name CRC32" (sfv); names
// with a backslash or line break are escaped as md5sum does.
QString format_line(const QString &name, const QString &algo, const QString &hex, const QString &style);
// Hash every file once with all the algorithms and write output_paths(); names are relative to `dir`. Files that can't
// be read are left out and named in *errors. Returns the files written. On a thread (task may be nullptr).
QStringList create(Task *task, const QStringList &paths, const QString &dir, const QString &stem,
                   const QStringList &algos, bool one_file, QStringList *errors);
// Write output_paths() for files already hashed: (name relative to dir, hashes in algos' order). Each file is written
// in one step, replacing an older one. Returns the files written.
QStringList write_checksums(const QString &dir, const QString &stem, const QList<QPair<QString, QStringList>> &hashed,
                            const QStringList &algos, bool one_file);

// Pick the algorithms, one file or one per algorithm, the name and the folder; settings remember the first two.
class CreateDialog : public QDialog {
    Q_OBJECT
public:
    CreateDialog(QWidget *parent, const QStringList &paths);
    QStringList algos() const;
    bool one_file() const;
    QString dir() const;
    QString stem() const;
    QStringList outputs() const { return output_paths(dir(), stem(), algos(), one_file()); }

private:
    void update();
    void ok();

    QStringList paths;
    QMap<QString, QCheckBox *> boxes;
    QRadioButton *each, *single;
    QLineEdit *name, *folder;
    QLabel *preview;
    QPushButton *create_btn;
};

// The context menu's Create Checksum File…: the dialog, then the job in the status bar; selects the result.
void create_dialog(MainWindow *win, const QStringList &paths);
void run_create(MainWindow *win, const QStringList &paths, const QString &dir, const QString &stem,
                const QStringList &algos, bool one_file);

}   // namespace hashcheck
