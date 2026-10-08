// Checksum files (.sfv, .md5, .sha256, SHA256SUMS…): reading them, checking the files they list, and the Verify
// Checksums dialog that opening one shows.
#pragma once

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>

#include <functional>

class QLabel;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class Task;

namespace hashcheck {

// One file a checksum file lists: its path (resolved against the checksum file's folder), the hash it should have
// (lowercase hex) and the algorithm: "crc32", "md5", "sha1", "sha224", "sha256", "sha384", "sha512" or "blake2b".
struct Entry {
    QString path, name, expected, algo;
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
QString algo_label(const QString &algo);   // "SHA256", "CRC32", "BLAKE2b"

// Each listed file's result: "ok", "failed", "missing" or "error" (with the message).
struct Result {
    QString status, actual, error;
};
Result verify(const Entry &e, Task *task = nullptr);

// Shows the checksum file's entries and checks them one after another, with progress; Verify Again, Stop, Close.
class VerifyDialog : public QDialog {
    Q_OBJECT
public:
    VerifyDialog(QWidget *parent, const QString &path, const QList<Entry> &entries);
    ~VerifyDialog() override;
    QList<Result> results;   // in entries' order; empty status while not checked yet
    bool complete() const { return next >= entries.size() && !task; }   // every file checked

protected:
    void hideEvent(QHideEvent *ev) override;

private:
    void start();
    void step();
    void on_result(const QVariant &res);
    void stop();
    void show_row(int i);
    void summary();

    QString path;
    QList<Entry> entries;
    QList<qint64> sizes;
    qint64 total = 0, before = 0;   // bytes in all files; in the files already checked
    int next = 0;
    int gen = 0;   // which run a task's result belongs to
    QPointer<Task> task;
    QTreeWidget *tree;
    QLabel *status;
    QProgressBar *bar;
    QPushButton *again, *stop_btn;
};

// Opening a checksum file: the Verify Checksums dialog, or false if it lists nothing (open it as a text file).
bool open_dialog(QWidget *parent, const QString &path);

}   // namespace hashcheck
