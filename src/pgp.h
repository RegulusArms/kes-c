// Encrypt… and Decrypt: files encrypted with gpg (OpenPGP), with a password or to people's public keys.
//
// Encrypt… makes an encrypted copy beside each file (name.ext.gpg, or name.ext.asc as text), the original left as it
// is: with a password (AES-256; the password goes to gpg on its fd 3, as password-encrypted tar archives do it, and
// gpg's agent doesn't keep it) or to the keys picked from your keyring (only their owners can open it; your own key
// too, so you can). Decrypt (also double-clicking an encrypted .gpg, .pgp or .asc) looks at the file first: one
// encrypted with a password asks for it (a password saved in the keyring for it is tried first), one encrypted to your
// key is opened by gpg's agent (which asks for your key's passphrase itself), one encrypted only to other people's
// keys says so. The decrypted copy goes beside it under the name without the extension (or the next free one).
//
// Either way the new file is written under a part name and claimed only when gpg has finished and succeeded: a wrong
// password, a cancel or a failure leaves nothing behind, and nothing is ever replaced.
#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class MainWindow;
class QCheckBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QTreeWidget;
class Task;

namespace pgp {

bool have_gpg();
bool is_encrypted_name(const QString &path);   // .gpg, .pgp, .asc (by the name only: a signature or a key may be too)
QString decrypted_name(const QString &path);   // the name without that extension ("notes.txt.gpg" → "notes.txt")

// A key in your keyring that can be encrypted to
struct Key {
    QString fingerprint, user_id;   // the primary key's fingerprint; its first user ID ("Name <mail>")
    bool own = false;               // you have its secret key
    bool verified = false;          // full or ultimate validity: you (or someone you trust) checked it's theirs
};
// gpg --with-colons --list-keys and --list-secret-keys → the keys that can encrypt (not expired, revoked or disabled),
// your own first; *own_ids: the IDs of every key and subkey you have the secret of
QList<Key> parse_keys(const QByteArray &pub, const QByteArray &sec, QStringList *own_ids = nullptr);
QList<Key> keys(QStringList *own_ids = nullptr);   // on a thread

// What gpg --list-packets says is in a file
struct Packets {
    bool encrypted = false;   // holds encrypted data
    bool password = false;    // can be opened with a password
    QStringList to_keys;      // the key IDs it's encrypted to (0000000000000000: a hidden recipient)
};
Packets parse_packets(const QByteArray &listing);
Packets packets(const QString &path);   // on a thread

// How a file can be decrypted here: not encrypted, with its password, with your key, or only with someone else's
enum Way { NOT_ENCRYPTED, PASSWORD, OWN_KEY, OTHERS_KEYS };
Way way_for(const Packets &p, const QStringList &own_ids);

struct Spec {
    QString password;         // encrypt with this password, or
    QStringList recipients;   // to these keys (fingerprints)
    bool armor = false;       // as text (.asc) rather than binary (.gpg)
};
// On a job's thread: an encrypted copy of path beside it, under the next free name; returns its path. Raises Error
// (Cancelled when the job is cancelled), leaving no file behind.
QString encrypt_file(Task *t, const QString &path, const Spec &spec);
// On a job's thread: a decrypted copy of path beside it, with password (empty: by your key, through gpg's agent);
// returns its path, and *signed_by a signature's headline ("✔ Valid signature…" and by whom) when the contents were
// also signed. Raises archive::WrongPassword, or Error; leaves no file behind.
QString decrypt_file(Task *t, const QString &path, const QString &password, QString *signed_by = nullptr);

// The Encrypt… window for paths (files)
void encrypt_dialog(MainWindow *win, const QStringList &paths);
// Decrypt path: looked at on a worker first; not_encrypted() then runs for a file that isn't encrypted (opened as any
// file), or it's said
void decrypt(MainWindow *win, const QString &path, std::function<void()> not_encrypted = nullptr);

}  // namespace pgp

// Encrypt…: with a password (typed twice) or to keys from your keyring (each one's name, and whether you've verified
// it), as text or binary, then Encrypt starts a job
class EncryptWindow : public QDialog {
    Q_OBJECT
public:
    EncryptWindow(MainWindow *win, const QStringList &paths);
    void encrypt();   // checks what's chosen, then starts the job (and closes)
    QStringList paths;
    QRadioButton *by_password, *by_key;
    QLineEdit *password, *again;
    QTreeWidget *key_list;
    QCheckBox *to_self, *armor;
    QLabel *problem;
    QList<pgp::Key> found;   // the keys listed (item data: their index)
    bool listed = false;     // the keyring has been read

Q_SIGNALS:
    void keys_listed();

private:
    void update();
    QPointer<MainWindow> win;
    QLabel *names, *key_note;
};
