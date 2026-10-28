// Verify Signature: checking an OpenPGP signature (.sig, .asc, .gpg, .sign) with gpg, and saying plainly what it
// means: valid, signed by whom and when (and whether that key is one you've verified); a bad signature; or the
// signer's key isn't in your keyring (its key ID and fingerprint, and a button to fetch it from a keyserver, asking
// first). A detached signature is checked against the file it signs (the same name without the extension), a
// clearsigned file against itself. A signed checksum file (SHA256SUMS with SHA256SUMS.gpg / .asc / .sig) is checked
// first, then every file it lists (Verify Checksums).
#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>

#include <functional>

class MainWindow;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class Task;

namespace signature {

bool is_signature_name(const QString &path);   // by its name: .sig, .asc, .sign, .gpg
QString signed_file(const QString &sig);       // the same name without the extension (whether it's there or not)
QString signature_for(const QString &file);    // a signature beside file (file.gpg, .asc, .sig, .sign), or empty

enum Kind {
    NOT_SIGNATURE,   // a key, an encrypted file…: opened as any file
    DETACHED,        // signs another file (signed_file)
    CLEARSIGNED,     // text with its signature around it
    INLINE,          // a signed message (binary or armoured): what it signs is inside it
};
Kind kind_of(const QString &sig);   // reads its beginning (on a thread: a slow drive)

struct Result {
    enum Status { GOOD, BAD, NO_KEY, EXPIRED_KEY, REVOKED_KEY, NO_SIGNATURE, MISSING_FILE, ERROR } status = ERROR;
    QString signer;        // the key's user ID ("Name <mail>")
    QString key_id, fingerprint;
    qint64 when = 0;       // when it was signed (seconds since the epoch)
    bool trusted = false;  // a key you've marked as trusted (or your own)
    QString file;          // the file checked (the signed one, or the signature itself when it holds what it signs)
    QString output;        // gpg's own words
};
// gpg's status lines (--status-fd) and exit code → the result
Result parse_status(const QByteArray &status, int rc);
bool have_gpg();
// Check sig (and the file it signs) with gpg. On a thread.
Result verify(const QString &sig);
QString headline(const Result &r);   // "Valid signature", "Bad signature"…
QString plain(const Result &r);      // what it means, in a few sentences
QString keyserver();                 // where a missing key is fetched from
// Fetch a key from the keyserver into the keyring (gpg --recv-keys). On a thread. Returns an error, or empty.
QString fetch_key(const QString &id);

// Verify Signature for sig: the check runs on a worker, then the window opens; something that isn't a signature
// calls not_signature() instead (it opens as an ordinary file). checksums_after: a signed checksum file's files are
// checked once the signature is good.
void open_window(MainWindow *win, const QString &sig, std::function<void()> not_signature = nullptr);

}  // namespace signature

class SignatureWindow : public QDialog {
    Q_OBJECT
public:
    SignatureWindow(MainWindow *win, const QString &sig, const signature::Result &r);
    void show_result(const signature::Result &r);
    void check_again();
    signature::Result result;
    QLabel *head, *body;
    QPushButton *fetch_btn, *files_btn;

Q_SIGNALS:
    void checked();   // a new result is shown

private:
    void fetch();
    QPointer<MainWindow> win;
    QString sig;
    QLabel *details;
    QPlainTextEdit *output;
};
