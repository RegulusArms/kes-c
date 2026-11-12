#include "pgp.h"

#include "app.h"
#include "archive.h"
#include "archive_ui.h"
#include "fileops.h"
#include "keyring.h"
#include "proc.h"
#include "signature.h"
#include "util.h"
#include "windows.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace util;

namespace pgp {

static const QStringList EXTS = {".gpg", ".pgp", ".asc"};

bool have_gpg() { return which("gpg"); }

bool is_encrypted_name(const QString &path) { return EXTS.contains(ext_of(path)); }

QString decrypted_name(const QString &path)
{
    QString n = basename(path);
    n.chop(splitext_ext(n).size());
    return n.isEmpty() ? tx("decrypted", "the name of a decrypted file whose own name was only “.gpg”") : n;
}

static proc::Result run_gpg(const QStringList &args, int timeout_ms)
{
    proc::Options o;
    o.c_utf8 = true;   // its messages in English
    return proc::run(QStringList{"gpg", "--batch", "--no-tty"} + args, timeout_ms, o);
}

// ---------------------------------------------------------------- keys

static QString unescape(const QString &s)   // --with-colons writes ":" and other characters as \xHH
{
    QByteArray in = s.toUtf8(), out;
    for (int i = 0; i < in.size(); ++i) {
        bool ok = false;
        int v = i + 3 < in.size() && in[i] == '\\' && in[i + 1] == 'x' ? in.mid(i + 2, 2).toInt(&ok, 16) : 0;
        if (ok) {
            out += char(v);
            i += 3;
        } else {
            out += in[i];
        }
    }
    return QString::fromUtf8(out);
}

QList<Key> parse_keys(const QByteArray &pub, const QByteArray &sec, QStringList *own_ids)
{
    QStringList ids;
    QSet<QString> own;
    QString last;
    for (const QByteArray &raw : sec.split('\n')) {
        QStringList f = QString::fromUtf8(raw).split(':');
        if (f[0] == "fpr" && last == "sec")
            own.insert(f.value(9));
        if (f[0] == "sec" || f[0] == "ssb")
            ids << f.value(4);
        if (f[0] != "fpr")
            last = f[0];
    }
    if (own_ids)
        *own_ids = ids;
    QList<Key> out;
    Key cur;
    bool in_pub = false, usable = false, want_fpr = false;
    auto flush = [&]() {
        if (in_pub && usable && !cur.fingerprint.isEmpty())
            out << cur;
        in_pub = false;
    };
    for (const QByteArray &raw : pub.split('\n')) {
        QStringList f = QString::fromUtf8(raw).split(':');
        if (f[0] == "pub") {
            flush();
            cur = Key();
            in_pub = true;
            QString validity = f.value(1), caps = f.value(11);
            // "E": the key as a whole can encrypt (a subkey can); "D": disabled; i, d, r, e: invalid, disabled,
            // revoked, expired
            usable = caps.contains('E') && !caps.contains('D') && !(validity.size() == 1 && QString("idre").contains(validity));
            cur.verified = validity == "f" || validity == "u";
            want_fpr = true;
        } else if (f[0] == "fpr" && want_fpr) {
            cur.fingerprint = f.value(9);
            want_fpr = false;
        } else if (f[0] == "uid" && in_pub && cur.user_id.isEmpty() && f.value(1) != "r" && f.value(1) != "e") {
            cur.user_id = unescape(f.value(9));
        } else if (f[0] == "sub") {
            want_fpr = false;
        }
    }
    flush();
    for (Key &k : out)
        k.own = own.contains(k.fingerprint);
    std::stable_partition(out.begin(), out.end(), [](const Key &k) { return k.own; });
    return out;
}

static QByteArray secret_listing() { return run_gpg({"--with-colons", "--fixed-list-mode", "--list-secret-keys"}, 30000).out; }

QList<Key> keys(QStringList *own_ids)
{
    QByteArray pub = run_gpg({"--with-colons", "--fixed-list-mode", "--list-keys"}, 30000).out;
    return parse_keys(pub, secret_listing(), own_ids);
}

// ---------------------------------------------------------------- what's in a file

Packets parse_packets(const QByteArray &listing)
{
    Packets p;
    for (const QByteArray &raw : listing.split('\n')) {
        if (raw.startsWith(":pubkey enc packet:")) {
            qsizetype at = raw.indexOf("keyid ");
            if (at >= 0)
                p.to_keys << QString::fromLatin1(raw.mid(at + 6)).trimmed().toUpper();
        } else if (raw.startsWith(":symkey enc packet:")) {
            p.password = true;
        } else if (raw.startsWith(":encrypted data packet:") || raw.startsWith(":aead encrypted packet:")) {
            p.encrypted = true;
        }
    }
    p.encrypted = p.encrypted || p.password || !p.to_keys.isEmpty();
    return p;
}

Packets packets(const QString &path)
{
    // --list-only: the packets, without decrypting; --pinentry-mode cancel: never asked for a password meanwhile
    return parse_packets(run_gpg({"--pinentry-mode", "cancel", "--list-only", "--list-packets", "--", path}, -1).out);
}

Way way_for(const Packets &p, const QStringList &own_ids)
{
    if (!p.encrypted)
        return NOT_ENCRYPTED;
    for (const QString &id : p.to_keys)
        if (id == "0000000000000000" || own_ids.contains(id, Qt::CaseInsensitive))   // (a hidden recipient: maybe you)
            return OWN_KEY;
    return p.password || p.to_keys.isEmpty() ? PASSWORD : OTHERS_KEYS;
}

// ---------------------------------------------------------------- encrypting and decrypting

// Waits for gpg, the job's progress from how far its output has got (an estimate: gpg compresses), stopping it on a
// cancel. Its stderr is read once it has exited: gpg says little there (with --status-fd 2, a few lines).
static void wait_gpg(Task *t, proc::Process *g, int out_fd, qint64 total)
{
    for (;;) {
        t->check();   // (a cancel: g is stopped when it's destroyed)
        if (g->wait_for(200))
            break;
        struct stat st;
        if (total > 0 && t->due() && ::fstat(out_fd, &st) == 0)
            t->report(double(std::min<qint64>(st.st_size, total)), double(total));
    }
}

static QString gpg_said(const QByteArray &err)   // gpg's last words, without its status lines
{
    QStringList lines;
    for (const QString &l : QString::fromUtf8(err).split('\n', Qt::SkipEmptyParts))
        if (!l.startsWith("[GNUPG:]"))
            lines << l.trimmed();
    return lines.isEmpty() ? tx("gpg failed") : lines.mid(std::max<qsizetype>(0, lines.size() - 2)).join(' ');
}

static qint64 size_of(const QString &path)
{
    struct stat st;
    return stat_(path, st) ? qint64(st.st_size) : 0;
}

// gpg writes into a part file beside path (fill), which then takes the next free name from `name`; a failure or a
// cancel deletes the part, so no half-written file is ever left under a real name, and nothing is replaced
static QString write_beside(const QString &path, const QString &name, const std::function<void(int)> &fill)
{
    QString dir = dirname(path), part;
    int out = open_part_at(AT_FDCWD, dir + "/", &part);
    try {
        fill(out);
        int fd = out;
        out = -1;
        if (::close(fd) != 0)
            throw_errno(part);
        QString final = unique_path(dir, name, "num");
        put_new(part, final);
        return final;
    } catch (...) {
        if (out >= 0)
            ::close(out);
        ::unlink(enc(part).constData());
        throw;
    }
}

// name + suffix in dir, or, where that's taken, numbered before the suffix: "notes (2).txt.gpg", not "notes.txt (2).gpg"
static QString free_name(const QString &dir, const QString &name, const QString &suffix)
{
    if (!lexists(join(dir, name + suffix)))
        return name + suffix;
    auto [stem, ext] = split_ext(name);
    for (int i = 2;; ++i) {
        QString n = stem + QString(" (%1)").arg(i) + ext + suffix;
        if (!lexists(join(dir, n)))
            return n;
    }
}

QString encrypt_file(Task *t, const QString &path, const Spec &spec)
{
    QString name = free_name(dirname(path), basename(path), spec.armor ? ".asc" : ".gpg");
    return write_beside(path, name, [&](int out) {
        proc::Options o;
        o.out = out;
        o.err = proc::PIPE;
        o.c_utf8 = true;
        QStringList armor = spec.armor ? QStringList{"--armor"} : QStringList();
        std::unique_ptr<proc::Process> g;
        if (!spec.password.isEmpty()) {
            g = archive::spawn_gpg(armor + QStringList{"--symmetric", "--cipher-algo", "AES256", "-o", "-", "--", path},
                                   spec.password, o);
        } else {
            // --trust-model always: the keys were picked one by one in the window, which says which aren't verified
            QStringList argv = QStringList{"gpg", "--batch", "--no-tty", "--quiet", "--trust-model", "always", "--encrypt"} + armor;
            for (const QString &r : spec.recipients)
                argv << "--recipient" << r;
            argv << "-o" << "-" << "--" << path;
            t->log("$ " + argv.join(' '));
            g = proc::spawn(argv, o);
        }
        wait_gpg(t, g.get(), out, size_of(path));
        if (g->returncode != 0)
            throw Error(tx("gpg couldn't encrypt “%1”: %2").arg(basename(path), gpg_said(g->read_all_err())));
    });
}

QString decrypt_file(Task *t, const QString &path, const QString &password, QString *signed_by)
{
    QByteArray err;
    int rc = -1;
    QString out = write_beside(path, decrypted_name(path), [&](int out_fd) {
        proc::Options o;
        o.out = out_fd;
        o.err = proc::PIPE;
        o.c_utf8 = true;
        QStringList args{"--status-fd", "2", "-d", "-o", "-", "--", path};
        std::unique_ptr<proc::Process> g;
        if (password.isEmpty()) {   // your key: gpg's agent asks for its passphrase itself (its own window)
            QStringList argv = QStringList{"gpg", "--batch", "--no-tty", "--quiet"} + args;
            t->log("$ " + argv.join(' '));
            g = proc::spawn(argv, o);
        } else {
            g = archive::spawn_gpg(args, password, o);
        }
        wait_gpg(t, g.get(), out_fd, size_of(path));
        err = g->read_all_err();
        rc = g->returncode;
        // judged by what gpg says of the decryption: it also fails (exit code 2) for a signature it can't check
        if (err.contains("[GNUPG:] DECRYPTION_OKAY") && !err.contains("[GNUPG:] DECRYPTION_FAILED"))
            return;
        if (!password.isEmpty() && (err.contains("BAD_PASSPHRASE") || err.contains("Bad session key")))
            throw archive::WrongPassword();
        if (err.contains("[GNUPG:] NO_SECKEY"))
            throw Error(tx("“%1” is encrypted to a key whose secret part isn't in your keyring.").arg(basename(path)));
        throw Error(tx("gpg couldn't decrypt “%1”: %2").arg(basename(path), gpg_said(err)));
    });
    if (signed_by && err.contains("[GNUPG:] NEWSIG")) {
        signature::Result r = signature::parse_status(err, rc);
        *signed_by = r.signer.isEmpty() ? signature::headline(r) : tx("%1, by %2").arg(signature::headline(r), r.signer);
    }
    return out;
}

// ---------------------------------------------------------------- the windows

void encrypt_dialog(MainWindow *win, const QStringList &paths)
{
    if (!have_gpg()) {
        warn(win, tx("Encrypt"), tx("Encrypting needs gpg, which isn't installed. Install it with:\n\nsudo apt install gnupg"));
        return;
    }
    show_free(new EncryptWindow(win, paths));
}

static void decrypt_with(QPointer<MainWindow> w, const QString &path, const QString &pw, bool remember, bool from_saved,
                         bool keyring_ok);

static void ask_for_password(QPointer<MainWindow> w, const QString &path, archive_ui::Ask reason, bool keyring_ok)
{
    if (!w)
        return;
    archive_ui::ask_password(
        w, path, reason, keyring_ok,
        [w, path, keyring_ok](const QString &pw, bool remember) { decrypt_with(w, path, pw, remember, false, keyring_ok); },
        nullptr, tx("Decrypt"));
}

static void decrypt_with(QPointer<MainWindow> w, const QString &path, const QString &pw, bool remember, bool from_saved,
                         bool keyring_ok)
{
    if (!w)
        return;
    fileops::queue_job(
        w, tx("Decrypting %1").arg(basename(path)), {path},
        [path, pw, remember](Task *t) -> QVariant {
            QString signed_by;
            try {
                QString out = decrypt_file(t, path, pw, &signed_by);
                t->log(tx("Decrypted into %1").arg(out));
                if (!signed_by.isEmpty())
                    t->log(signed_by);
                if (remember)
                    keyring::store(keyring::fingerprint(path), basename(path), pw);
                return QVariantMap{{"file", out}, {"signed", signed_by}};
            } catch (const archive::WrongPassword &) {
                t->log(tx("The password is wrong."));
                return QVariantMap{{"wrong", true}};
            }
        },
        [w, path, from_saved, keyring_ok](const QVariant &res) {
            if (!w || !res.isValid())
                return;
            QVariantMap m = res.toMap();
            if (m.value("wrong").toBool())   // (a saved one that's wrong: asked as for the first time)
                return ask_for_password(w, path, from_saved ? archive_ui::Ask::First : archive_ui::Ask::Wrong, keyring_ok);
            if (!m.value("signed").toString().isEmpty())
                inform(w, tx("Decrypt"),
                       tx("“%1” is decrypted into “%2”. What was in it was also signed:\n\n%3")
                           .arg(basename(path), basename(m.value("file").toString()), m.value("signed").toString()));
        });
}

void decrypt(MainWindow *win, const QString &path, std::function<void()> not_encrypted)
{
    if (!have_gpg()) {
        if (not_encrypted)
            not_encrypted();
        else
            warn(win, tx("Decrypt"), tx("Decrypting needs gpg, which isn't installed. Install it with:\n\nsudo apt install gnupg"));
        return;
    }
    QPointer<MainWindow> w(win);
    fileops::run_task(
        win, tx("Reading %1").arg(basename(path)),
        [path]() {
            QStringList own;
            parse_keys(QByteArray(), secret_listing(), &own);
            Packets p = packets(path);
            Way way = way_for(p, own);
            bool kr = way == PASSWORD && keyring::available();
            return QVariant(QVariantMap{{"way", int(way)},
                                        {"keys", p.to_keys},
                                        {"keyring", kr},
                                        {"saved", kr ? keyring::lookup(keyring::fingerprint(path)) : QString()}});
        },
        [w, path, not_encrypted](const QVariant &res) {
            if (!w)
                return;
            QVariantMap m = res.toMap();
            bool kr = m.value("keyring").toBool();
            switch (Way(m.value("way").toInt())) {
            case NOT_ENCRYPTED:
                if (not_encrypted)
                    not_encrypted();
                else
                    inform(w, tx("Decrypt"), tx("“%1” isn't encrypted: gpg finds no encrypted data in it (it may be a "
                                                "signature or a key).").arg(basename(path)));
                break;
            case OTHERS_KEYS:
                warn(w, tx("Decrypt"),
                     tx("“%1” is encrypted to someone else's key (%2): your keyring doesn't have its secret part, so "
                        "only they can open it.")
                         .arg(basename(path), m.value("keys").toStringList().join(", ")));
                break;
            case OWN_KEY:
                decrypt_with(w, path, QString(), false, false, false);
                break;
            case PASSWORD:
                if (!m.value("saved").toString().isEmpty())
                    decrypt_with(w, path, m.value("saved").toString(), false, true, kr);
                else
                    ask_for_password(w, path, archive_ui::Ask::First, kr);
                break;
            }
        },
        true);
}

}  // namespace pgp

using namespace pgp;

EncryptWindow::EncryptWindow(MainWindow *win, const QStringList &paths) : QDialog(win), paths(paths), win(win)
{
    setWindowTitle(paths.size() == 1 ? tx("Encrypt “%1”").arg(basename(paths.first())) : txn("Encrypt %Ln File(s)", paths.size()));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(600, 520);
    auto *lay = new QVBoxLayout(this);
    names = new QLabel(paths.size() == 1 ? tx("An encrypted copy is made beside it, as “%1”; the original stays as it is.")
                                               .arg(basename(paths.first()) + ".gpg")
                                         : tx("An encrypted copy of each is made beside it (“name.gpg”); the originals "
                                              "stay as they are."));
    names->setWordWrap(true);
    names->setTextFormat(Qt::PlainText);
    lay->addWidget(names);

    by_password = new QRadioButton(tx("With a password: anyone who has it can open it"));
    lay->addWidget(by_password);
    auto *grid = new QGridLayout;
    grid->setContentsMargins(24, 0, 0, 0);
    password = new QLineEdit;
    password->setEchoMode(QLineEdit::Password);
    password->setAccessibleName(tx("Password"));
    again = new QLineEdit;
    again->setEchoMode(QLineEdit::Password);
    again->setAccessibleName(tx("The password again"));
    auto *show = new QCheckBox(tx("Show"));
    connect(show, &QCheckBox::toggled, this, [this](bool on) {
        password->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        again->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    grid->addWidget(new QLabel(tx("Password:")), 0, 0);
    grid->addWidget(password, 0, 1);
    grid->addWidget(show, 0, 2);
    grid->addWidget(new QLabel(tx("Again:")), 1, 0);
    grid->addWidget(again, 1, 1);
    lay->addLayout(grid);

    by_key = new QRadioButton(tx("To people's keys: only they can open it, with no password to pass on"));
    lay->addWidget(by_key);
    key_list = new QTreeWidget;
    key_list->setAccessibleName(tx("Keys to encrypt to"));
    key_list->setRootIsDecorated(false);
    key_list->setHeaderLabels({tx("Name"), tx("Key"), tx("Verified")});
    key_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    key_list->header()->setStretchLastSection(false);
    lay->addWidget(key_list, 1);
    to_self = new QCheckBox(tx("Also to my own key, so I can open it too"));
    to_self->setChecked(true);
    lay->addWidget(to_self);
    key_note = new QLabel(tx("Looking for keys…"));
    key_note->setWordWrap(true);
    lay->addWidget(key_note);

    armor = new QCheckBox(tx("As text (.asc), to paste into an email or a message"));
    connect(armor, &QCheckBox::toggled, this, [this](bool on) {
        if (this->paths.size() == 1)
            names->setText(tx("An encrypted copy is made beside it, as “%1”; the original stays as it is.")
                               .arg(basename(this->paths.first()) + (on ? ".asc" : ".gpg")));
        else
            names->setText(on ? tx("An encrypted copy of each is made beside it (“name.asc”); the originals stay as they are.")
                              : tx("An encrypted copy of each is made beside it (“name.gpg”); the originals stay as they are."));
    });
    lay->addWidget(armor);
    problem = new QLabel;
    problem->setWordWrap(true);
    problem->hide();
    lay->addWidget(problem);

    auto *group = new QButtonGroup(this);
    group->addButton(by_password);
    group->addButton(by_key);
    by_password->setChecked(true);
    connect(by_password, &QRadioButton::toggled, this, &EncryptWindow::update);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *go = bb->addButton(tx("Encrypt"), QDialogButtonBox::AcceptRole);
    connect(go, &QPushButton::clicked, this, &EncryptWindow::encrypt);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
    lay->addWidget(bb);
    update();
    password->setFocus();

    QPointer<EncryptWindow> self(this);
    fileops::run_task(
        this, tx("Listing your keys"),
        []() {
            QVariantList out;
            for (const Key &k : keys())
                out << QVariantMap{{"fpr", k.fingerprint}, {"uid", k.user_id}, {"own", k.own}, {"verified", k.verified}};
            return QVariant(out);
        },
        [self](const QVariant &res) {
            if (!self)
                return;
            self->found.clear();
            for (const QVariant &v : res.toList()) {
                QVariantMap m = v.toMap();
                Key k;
                k.fingerprint = m.value("fpr").toString();
                k.user_id = m.value("uid").toString();
                k.own = m.value("own").toBool();
                k.verified = m.value("verified").toBool();
                auto *it = new QTreeWidgetItem(self->key_list, {k.own ? tx("%1 (you)").arg(k.user_id) : k.user_id,
                                                                k.fingerprint.right(16),
                                                                k.verified ? tx("Yes") : tx("No")});
                it->setData(0, Qt::UserRole, int(self->found.size()));
                it->setCheckState(0, Qt::Unchecked);
                it->setToolTip(1, k.fingerprint);
                self->found << k;
            }
            self->listed = true;
            self->update();
            Q_EMIT self->keys_listed();
        },
        true);
}

void EncryptWindow::update()
{
    bool pw = by_password->isChecked();
    password->setEnabled(pw);
    again->setEnabled(pw);
    key_list->setEnabled(!pw);
    bool own = std::any_of(found.begin(), found.end(), [](const Key &k) { return k.own; });
    to_self->setEnabled(!pw && own);
    if (found.isEmpty() && listed)
        key_note->setText(tx("Your keyring has no keys to encrypt to. Import the public key of whoever it's for (for "
                             "example in Passwords and Keys, or with gpg --import) and open Encrypt… again."));
    else if (!found.isEmpty())
        key_note->setText(own ? tx("A key you haven't verified may not be theirs: anyone can make a key with any name. "
                                   "Compare its fingerprint (hover over its key) with the one they gave you.")
                              : tx("You have no key of your own, so you won't be able to open what's encrypted to other "
                                   "people's keys. Anyone can make a key with any name: compare a key's fingerprint "
                                   "(hover over it) with the one its owner gave you."));
}

void EncryptWindow::encrypt()
{
    Spec spec;
    spec.armor = armor->isChecked();
    QString why;
    if (by_password->isChecked()) {
        spec.password = password->text();
        if (spec.password.isEmpty())
            why = tx("Type a password, twice.");
        else if (spec.password != again->text())
            why = tx("The two passwords aren't the same.");
    } else {
        for (int i = 0; i < key_list->topLevelItemCount(); ++i) {
            QTreeWidgetItem *it = key_list->topLevelItem(i);
            if (it->checkState(0) == Qt::Checked)
                spec.recipients << found.value(it->data(0, Qt::UserRole).toInt()).fingerprint;
        }
        if (spec.recipients.isEmpty())
            why = tx("Tick the keys of the people it's for.");
        if (to_self->isEnabled() && to_self->isChecked())
            for (const Key &k : std::as_const(found))
                if (k.own && !spec.recipients.contains(k.fingerprint)) {
                    spec.recipients << k.fingerprint;
                    break;
                }
    }
    problem->setText(why);
    problem->setVisible(!why.isEmpty());
    if (!why.isEmpty() || !win)
        return;
    QStringList list = paths, steps;
    for (const QString &p : list)
        steps << basename(p);
    fileops::queue_job(
        win, list.size() == 1 ? tx("Encrypting %1").arg(basename(list.first())) : txn("Encrypting %Ln file(s)", list.size()),
        list,
        [list, spec](Task *t) -> QVariant {
            QStringList failed;
            for (int i = 0; i < list.size(); ++i) {
                t->check();
                t->step = i;
                const QString &p = list[i];
                try {   // (a cancel goes through: Cancelled isn't an Error)
                    t->log(tx("Encrypted %1 into %2").arg(p, encrypt_file(t, p, spec)));
                } catch (const Error &e) {
                    t->log(e.message());
                    failed << e.message();
                }
            }
            if (!failed.isEmpty())
                throw Error(failed.join('\n'));
            return QVariant(true);
        },
        nullptr, true, steps);
    close();
}
