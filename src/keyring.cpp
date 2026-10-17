#include "keyring.h"

#include "util.h"

#include <QCryptographicHash>
#include <QFile>

#include <libsecret/secret.h>

namespace keyring {

static const SecretSchema *schema()
{
    static const SecretSchema s = {"org.kestrel_explorer.ArchivePassword",
                                   SECRET_SCHEMA_NONE,
                                   {{"fingerprint", SECRET_SCHEMA_ATTRIBUTE_STRING},
                                    {"kind", SECRET_SCHEMA_ATTRIBUTE_STRING},
                                    {nullptr, SecretSchemaAttributeType(0)}},
                                   0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    return &s;
}

QString fingerprint(const QString &archive)
{
    QFile f(archive);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QByteArray start = f.read(64 * 1024);
    return QString("%1:%2").arg(f.size()).arg(QString::fromLatin1(QCryptographicHash::hash(start, QCryptographicHash::Sha256).toHex()));
}

bool available()
{
    GError *err = nullptr;
    // opening a session: the service really answers (not just a name on the bus)
    SecretService *svc = secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, nullptr, &err);
    if (!svc) {
        g_clear_error(&err);
        return false;
    }
    g_object_unref(svc);
    return true;
}

QString lookup(const QString &fp)
{
    if (fp.isEmpty())
        return QString();
    GError *err = nullptr;
    QByteArray f = fp.toUtf8();
    gchar *pw = secret_password_lookup_sync(schema(), nullptr, &err, "fingerprint", f.constData(), "kind", "archive", nullptr);
    g_clear_error(&err);
    if (!pw)
        return QString();
    QString out = QString::fromUtf8(pw);
    secret_password_free(pw);
    return out;
}

QStringList all()
{
    GError *err = nullptr;
    GList *items = secret_password_search_sync(
        schema(), SecretSearchFlags(SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS), nullptr, &err,
        "kind", "archive", nullptr);
    g_clear_error(&err);
    QStringList out;
    for (GList *l = items; l; l = l->next) {
        SecretValue *v = secret_retrievable_retrieve_secret_sync(SECRET_RETRIEVABLE(l->data), nullptr, &err);
        g_clear_error(&err);
        if (!v)
            continue;
        const gchar *text = secret_value_get_text(v);
        if (text && !out.contains(QString::fromUtf8(text)))
            out << QString::fromUtf8(text);
        secret_value_unref(v);
    }
    g_list_free_full(items, g_object_unref);
    return out;
}

bool store(const QString &fp, const QString &name, const QString &password)
{
    if (fp.isEmpty() || password.isEmpty())
        return false;
    GError *err = nullptr;
    QByteArray f = fp.toUtf8(), label = ("Kestrel Explorer: archive password for " + name).toUtf8(), pw = password.toUtf8();
    gboolean ok = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, label.constData(), pw.constData(), nullptr,
                                             &err, "fingerprint", f.constData(), "kind", "archive", nullptr);
    g_clear_error(&err);
    return ok;
}

int forget_all()
{
    GError *err = nullptr;
    GList *items = secret_password_search_sync(schema(), SecretSearchFlags(SECRET_SEARCH_ALL), nullptr, &err, "kind",
                                               "archive", nullptr);
    if (err) {
        g_clear_error(&err);
        return -1;
    }
    int n = int(g_list_length(items));
    g_list_free_full(items, g_object_unref);
    secret_password_clear_sync(schema(), nullptr, &err, "kind", "archive", nullptr);
    if (err) {
        g_clear_error(&err);
        return -1;
    }
    return n;
}

}  // namespace keyring
