#include "signature.h"

#include "app.h"
#include "fileops.h"
#include "hashcheck.h"
#include "proc.h"
#include "util.h"
#include "windows.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

using namespace util;

namespace signature {

static const QStringList EXTS = {".sig", ".asc", ".sign", ".gpg"};

bool is_signature_name(const QString &path) { return EXTS.contains(ext_of(path)); }

QString signed_file(const QString &sig) { return sig.left(sig.size() - splitext_ext(sig).size()); }

QString signature_for(const QString &file)
{
    for (const QString &e : EXTS)
        if (isfile(file + e))
            return file + e;
    return QString();
}

Kind kind_of(const QString &sig)
{
    QFile f(sig);
    if (!f.open(QIODevice::ReadOnly))
        return NOT_SIGNATURE;
    QByteArray head = f.read(4096);
    QByteArray text = head.trimmed();
    if (text.startsWith("-----BEGIN PGP SIGNED MESSAGE-----"))
        return CLEARSIGNED;
    if (text.startsWith("-----BEGIN PGP SIGNATURE-----"))
        return DETACHED;
    if (text.startsWith("-----BEGIN PGP MESSAGE-----"))
        return INLINE;   // (or encrypted: gpg then finds no signature)
    if (text.startsWith("-----BEGIN") || head.isEmpty())   // a key, a certificate, other text
        return NOT_SIGNATURE;
    if (!(uchar(head.at(0)) & 0x80))   // an OpenPGP packet's first byte has its top bit set
        return NOT_SIGNATURE;
    return isfile(signed_file(sig)) ? DETACHED : INLINE;
}

Result parse_status(const QByteArray &status, int rc)
{
    Result r;
    bool decided = false, no_key = false, no_data = false;
    for (const QByteArray &raw : status.split('\n')) {
        if (!raw.startsWith("[GNUPG:] "))
            continue;
        QString line = QString::fromUtf8(raw.mid(9)).trimmed();
        QStringList f = line.split(' ');
        QString kw = f.value(0);
        auto rest = [&](int from) { return QStringList(f.mid(from)).join(' '); };
        if (!decided && (kw == "GOODSIG" || kw == "BADSIG" || kw == "EXPKEYSIG" || kw == "REVKEYSIG")) {
            r.status = kw == "GOODSIG" ? Result::GOOD : kw == "BADSIG" ? Result::BAD : kw == "EXPKEYSIG" ? Result::EXPIRED_KEY
                                                                                                        : Result::REVOKED_KEY;
            r.key_id = f.value(1);
            r.signer = rest(2);
            decided = true;
        } else if (kw == "ERRSIG" && !decided) {
            r.key_id = f.value(1);
            r.when = f.value(5).toLongLong();
            if (f.value(7).size() > 8 && f.value(7) != "-")
                r.fingerprint = f.value(7);
            no_key = no_key || f.value(6) == "9";
        } else if (kw == "NO_PUBKEY") {
            no_key = true;
            if (r.key_id.isEmpty())
                r.key_id = f.value(1);
        } else if (kw == "VALIDSIG") {
            r.fingerprint = f.size() > 10 ? f.value(10) : f.value(1);   // the primary key's, else the signing key's
            bool ok = false;
            qint64 ts = f.value(3).toLongLong(&ok);
            if (ok)
                r.when = ts;
        } else if (kw == "TRUST_FULLY" || kw == "TRUST_ULTIMATE") {
            r.trusted = true;
        } else if (kw == "NODATA") {
            no_data = true;
        }
    }
    if (!decided)
        r.status = no_key ? Result::NO_KEY : no_data ? Result::NO_SIGNATURE : Result::ERROR;
    if (r.status == Result::GOOD && rc != 0)   // (gpg says good but fails: trust it not)
        r.status = Result::ERROR;
    return r;
}

bool have_gpg() { return which("gpg"); }

QString keyserver() { return "hkps://keyserver.ubuntu.com"; }

static proc::Result run_gpg(const QStringList &args, int timeout_ms)
{
    proc::Options o;
    o.c_utf8 = true;   // its messages in English
    return proc::run(QStringList{"gpg", "--batch"} + args, timeout_ms, o);
}

Result verify(const QString &sig)
{
    Result r;
    r.file = sig;
    if (!have_gpg()) {
        r.output = tx("gpg isn't installed (the gnupg package).");
        return r;
    }
    Kind k = kind_of(sig);
    if (k == NOT_SIGNATURE) {
        r.status = Result::NO_SIGNATURE;
        return r;
    }
    QStringList args{"--status-fd", "1", "--verify", sig};
    if (k == DETACHED) {
        r.file = signed_file(sig);
        if (!isfile(r.file)) {
            r.status = Result::MISSING_FILE;
            return r;
        }
        args << r.file;
    }
    proc::Result p = run_gpg(args, -1);
    QString file = r.file;
    r = parse_status(p.out, p.rc);
    r.file = file;
    r.output = QString::fromUtf8(p.err).trimmed();
    if (p.failed)
        r.output = tx("gpg couldn't be started.");
    return r;
}

QString fetch_key(const QString &id)
{
    proc::Result p = run_gpg({"--keyserver", keyserver(), "--recv-keys", id}, 90000);
    if (p.rc == 0)
        return QString();
    QStringList lines = QString::fromUtf8(p.err).split('\n', Qt::SkipEmptyParts);
    return p.timed_out ? tx("The keyserver didn't answer.") : lines.isEmpty() ? tx("gpg failed") : lines.last();
}

static QString grouped(const QString &hex)   // a fingerprint in fours, as gpg shows it
{
    QString out;
    for (int i = 0; i < hex.size(); i += 4)
        out += (i ? (i == hex.size() / 2 ? "  " : " ") : "") + hex.mid(i, 4);
    return out;
}

QString headline(const Result &r)
{
    switch (r.status) {
    case Result::GOOD:
        return r.trusted ? tx("✔ Valid signature") : tx("✔ Valid signature, from a key you haven't verified");
    case Result::EXPIRED_KEY:
        return tx("✔ Valid signature, from a key that has expired");
    case Result::BAD:
        return tx("✘ Bad signature");
    case Result::REVOKED_KEY:
        return tx("⚠ Signed with a revoked key");
    case Result::NO_KEY:
        return tx("? Can't check: the signer's key isn't in your keyring");
    case Result::NO_SIGNATURE:
        return tx("No signature in this file");
    case Result::MISSING_FILE:
        return tx("The file it signs isn't here");
    case Result::ERROR:
        break;
    }
    return tx("The signature couldn't be checked");
}

QString plain(const Result &r)
{
    QString who = r.signer.isEmpty() ? tx("an unknown signer") : "“" + r.signer + "”";
    QDateTime at = QDateTime::fromSecsSinceEpoch(r.when);
    QString when = r.when ? tx(" on %1", "when it was signed, after “signed”")
                                .arg(tx("%1 at %2", "a date and a time").arg(i18n::long_date(at.date()), i18n::time(at.time())))
                          : QString();
    QString what = "“" + basename(r.file) + "”";
    switch (r.status) {
    case Result::GOOD:
        return r.trusted ? tx("%1 is exactly what %2 signed%3. You've marked this key as trusted.").arg(what, who, when)
                         : tx("%1 is exactly what the key named %2 signed%3, and hasn't changed since. But anyone "
                              "can make a key with any name: you haven't verified that this key is really theirs. "
                              "Compare its fingerprint (below) with the one they publish, for example on their "
                              "website, before trusting it.")
                               .arg(what, who, when);
    case Result::EXPIRED_KEY:
        return tx("%1 is exactly what %2 signed%3, but their key has expired since. A signature made before it "
                  "expired is still sound; a new release should come with a new key.")
                   .arg(what, who, when);
    case Result::BAD:
        return tx("%1 isn't what %2 signed: it was changed after it was signed, damaged in the download, or the "
                  "signature is for another file. Don't trust it.")
            .arg(what, who);
    case Result::REVOKED_KEY:
        return tx("%1 was signed with %2's key, which they have withdrawn (revoked), for example because it was "
                  "stolen. Don't trust it.")
            .arg(what, who);
    case Result::NO_KEY:
        return tx("To check it, gpg needs the signer's public key, which isn't in your keyring. Fetch Key… gets it "
                  "from %1; once you have it, compare its fingerprint with the one the signer publishes.")
            .arg(keyserver());
    case Result::NO_SIGNATURE:
        return tx("gpg found no signature in it: it may be encrypted, or a key.");
    case Result::MISSING_FILE:
        return tx("This is a signature for %1, which should be in the same folder. Put it there and check again.").arg(what);
    case Result::ERROR:
        break;
    }
    return r.output.isEmpty() ? tx("gpg failed.") : r.output.split('\n').last();
}

static QVariantMap to_map(const Result &r)
{
    return {{"status", int(r.status)}, {"signer", r.signer}, {"key_id", r.key_id}, {"fingerprint", r.fingerprint},
            {"when", r.when},          {"trusted", r.trusted}, {"file", r.file},   {"output", r.output}};
}

static Result from_map(const QVariantMap &m)
{
    Result r;
    r.status = Result::Status(m.value("status", int(Result::ERROR)).toInt());
    r.signer = m.value("signer").toString();
    r.key_id = m.value("key_id").toString();
    r.fingerprint = m.value("fingerprint").toString();
    r.when = m.value("when").toLongLong();
    r.trusted = m.value("trusted").toBool();
    r.file = m.value("file").toString();
    r.output = m.value("output").toString();
    return r;
}

static bool sound(const Result &r) { return r.status == Result::GOOD || r.status == Result::EXPIRED_KEY; }

// check sig on a job (it reads the signed file: maybe a big download on a slow drive), then done(result)
static void check(QWidget *parent, const QString &sig, std::function<void(const Result &)> done)
{
    QStringList paths{sig};
    if (isfile(signed_file(sig)))
        paths << signed_file(sig);
    fileops::queue_job(
        parent, tx("Checking the signature %1").arg(basename(sig)), paths,
        [sig](Task *) { return QVariant(to_map(verify(sig))); },
        [done](const QVariant &res) {
            if (res.isValid())
                done(from_map(res.toMap()));
        },
        false, {}, true);
}

void open_window(MainWindow *win, const QString &sig, std::function<void()> not_signature)
{
    QPointer<MainWindow> w(win);
    check(win, sig, [w, sig, not_signature](const Result &r) {
        if (!w)
            return;
        if (r.status == Result::NO_SIGNATURE && not_signature) {
            not_signature();
            return;
        }
        show_free(new SignatureWindow(w, sig, r));
        if (sound(r) && hashcheck::is_hash_file(r.file))   // a signed checksum file: then the files it lists
            hashcheck::open_dialog(w, r.file, nullptr);
    });
}

}  // namespace signature

using namespace signature;

SignatureWindow::SignatureWindow(MainWindow *win, const QString &sig, const Result &r) : QDialog(win), win(win), sig(sig)
{
    setWindowTitle(tx("Verify Signature — %1").arg(basename(sig)));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(620, 360);
    auto *lay = new QVBoxLayout(this);
    head = new QLabel;
    QFont f = head->font();
    f.setPointSizeF(f.pointSizeF() * 1.3);
    f.setBold(true);
    head->setFont(f);
    head->setWordWrap(true);
    lay->addWidget(head);
    body = new QLabel;
    body->setWordWrap(true);
    body->setTextFormat(Qt::PlainText);
    lay->addWidget(body);
    details = new QLabel;
    details->setWordWrap(true);
    details->setTextFormat(Qt::PlainText);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(details);
    output = new QPlainTextEdit;
    output->setReadOnly(true);
    output->setAccessibleName(tx("gpg's output"));
    output->hide();
    lay->addWidget(output, 1);
    lay->addStretch(0);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    fetch_btn = bb->addButton(tx("Fetch Key…"), QDialogButtonBox::ActionRole);
    connect(fetch_btn, &QPushButton::clicked, this, &SignatureWindow::fetch);
    files_btn = bb->addButton(tx("Verify the Listed Files"), QDialogButtonBox::ActionRole);
    files_btn->setToolTip(tx("Check every file this checksum file lists"));
    connect(files_btn, &QPushButton::clicked, this, [this]() {
        if (this->win)
            hashcheck::open_dialog(this->win, result.file, nullptr);
    });
    auto *again = bb->addButton(tx("Check Again"), QDialogButtonBox::ActionRole);
    connect(again, &QPushButton::clicked, this, &SignatureWindow::check_again);
    auto *raw = bb->addButton(tx("Show gpg's Output"), QDialogButtonBox::ActionRole);
    connect(raw, &QPushButton::clicked, this, [this, raw]() {
        output->setVisible(!output->isVisible());
        raw->setText(output->isVisible() ? tx("Hide gpg's Output") : tx("Show gpg's Output"));
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);
    lay->addWidget(bb);
    show_result(r);
}

void SignatureWindow::show_result(const Result &r)
{
    result = r;
    head->setText(headline(r));
    body->setText(plain(r));
    QStringList d{tx("Signature: %1").arg(sig)};
    if (r.file != sig)
        d << tx("Signed file: %1").arg(r.file);
    if (!r.key_id.isEmpty())
        d << tx("Key ID: %1").arg(r.key_id);
    if (!r.fingerprint.isEmpty())
        d << tx("Fingerprint: %1").arg(grouped(r.fingerprint));
    details->setText(d.join('\n'));
    output->setPlainText(r.output);
    fetch_btn->setVisible(r.status == Result::NO_KEY && !(r.fingerprint.isEmpty() && r.key_id.isEmpty()));
    files_btn->setVisible((r.status == Result::GOOD || r.status == Result::EXPIRED_KEY) && hashcheck::is_hash_file(r.file));
    Q_EMIT checked();
}

void SignatureWindow::check_again()
{
    QPointer<SignatureWindow> self(this);
    head->setText(tx("Checking…"));
    signature::check(this, sig, [self](const Result &r) {
        if (self)
            self->show_result(r);
    });
}

void SignatureWindow::fetch()
{
    QString id = result.fingerprint.isEmpty() ? result.key_id : result.fingerprint;
    QPointer<SignatureWindow> self(this);
    ask_yes(this, tx("Fetch Key"),
            tx("Fetch the key %1 from %2 and add it to your keyring?\n\nThis connects to the internet: the "
                    "keyserver sees which key is asked for. Having the key lets gpg check the signature; it doesn't make "
                    "the key trusted.")
                .arg(id, keyserver()),
            [self, id]() {
                if (!self)
                    return;
                self->head->setText(tx("Fetching the key…"));
                fileops::run_task(self, tx("Fetching key %1").arg(id), [id]() { return QVariant(fetch_key(id)); },
                                  [self](const QVariant &err) {
                                      if (!self)
                                          return;
                                      if (!err.toString().isEmpty()) {
                                          self->show_result(self->result);
                                          warn(self, tx("Fetch Key"), tx("The key couldn't be fetched: %1").arg(err.toString()));
                                          return;
                                      }
                                      self->check_again();
                                  });
            });
}
