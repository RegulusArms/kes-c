#include "places.h"

#include "atc.h"
#include "util.h"

#include <QBuffer>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <gio/gio.h>

#include <algorithm>
#include <optional>

using namespace util;

namespace places {

const QString STARRED = "starred://";
const QString RECENT = "recent://";

static const QString NS_BOOKMARK = "http://www.freedesktop.org/standards/desktop-bookmarks";
static const QString NS_MIME = "http://www.freedesktop.org/standards/shared-mime-info";

bool is_virtual(const QString &place) { return place == STARRED || place == RECENT; }
QString title(const QString &place) { return place == STARRED ? "Starred" : "Recent"; }
QString icon_name(const QString &place) { return place == STARRED ? "starred" : "document-open-recent"; }

QString empty_text(const QString &place)
{
    return place == STARRED ? "No starred items — right-click a file or folder and choose Star" : "No recent files";
}

Signals *signals_()
{
    static Signals *s = new Signals;
    return s;
}

// ---------------------------------------------------------------- starred

static QString starred_file() { return join(CONFIG_DIR(), "starred.json"); }

static std::optional<QStringList> starred_list;
static QSet<QString> starred_set;

QStringList starred()
{
    if (!starred_list) {
        bool ok = false;
        QStringList l;
        for (const QJsonValue &v : QJsonDocument::fromJson(read_file(starred_file(), &ok)).array())
            if (v.isString())
                l << v.toString();
        starred_list = l;
        starred_set = QSet<QString>(l.begin(), l.end());
    }
    return *starred_list;
}

bool is_starred(const QString &path)
{
    starred();
    return starred_set.contains(path);
}

void set_starred(const QStringList &paths, bool on)
{
    starred_list.reset();   // another Kestrel may have changed it since
    QStringList current = starred();
    if (on) {
        for (const QString &p : paths)
            if (!current.contains(p))
                current << p;
    } else {
        QSet<QString> drop(paths.begin(), paths.end());
        current.erase(std::remove_if(current.begin(), current.end(), [&](const QString &p) { return drop.contains(p); }),
                      current.end());
    }
    starred_list = current;
    starred_set = QSet<QString>(current.begin(), current.end());
    try {
        makedirs(dirname(starred_file()), true);
        write_text(starred_file(), QJsonDocument(QJsonArray::fromStringList(current)).toJson(QJsonDocument::Indented));
    } catch (const OSError &) {
    }
    Q_EMIT signals_()->starred_changed();
    atc::announce("starred");
}

void reload_starred()
{
    starred_list.reset();
    starred();
    Q_EMIT signals_()->starred_changed();
}

QList<QPair<QString, QString>> items(const QString &place)
{
    QStringList paths;
    if (place == STARRED) {
        for (const QString &p : starred())
            if (lexists(p))
                paths << p;
    } else {
        paths = recent_files();
    }
    QList<QPair<QString, QString>> out;
    for (const QString &p : paths)
        out << qMakePair(p, p);
    return out;
}

// ---------------------------------------------------------------- recent

static QString recent_file()
{
    QByteArray data_home = qgetenv("XDG_DATA_HOME");
    return join(data_home.isEmpty() ? HOME() + "/.local/share" : QString::fromLocal8Bit(data_home),
                "recently-used.xbel");
}

QStringList recent_files(int limit)
{
    bool ok = false;
    QByteArray data = read_file(recent_file(), &ok);
    QList<QPair<QString, QString>> entries;   // (stamp, path)
    QXmlStreamReader r(data);
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement() || r.name() != u"bookmark" || !r.namespaceUri().isEmpty())
            continue;
        QXmlStreamAttributes a = r.attributes();
        QString path = uri_to_path(a.value("href").toString());
        if (path.isEmpty() || !lexists(path))
            continue;
        QString stamp = std::max({a.value("visited").toString(), a.value("modified").toString(),
                                  a.value("added").toString()});
        entries << qMakePair(stamp, path);
    }
    std::stable_sort(entries.begin(), entries.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
    QStringList out;
    QSet<QString> seen;
    for (const auto &[stamp, p] : entries) {
        if (seen.contains(p))
            continue;
        seen << p;
        out << p;
        if (out.size() >= limit)
            break;
    }
    return out;
}

static bool remember_recent()
{
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, "org.gnome.desktop.privacy", TRUE) : nullptr;
    if (!schema)
        return true;
    g_settings_schema_unref(schema);
    GSettings *s = g_settings_new("org.gnome.desktop.privacy");
    bool on = g_settings_get_boolean(s, "remember-recent-files");
    g_object_unref(s);
    return on;
}

static void write_new_bookmark(QXmlStreamWriter &w, const QString &href, const QString &mime, const QString &now)
{
    w.writeStartElement("bookmark");
    w.writeAttribute("href", href);
    w.writeAttribute("added", now);
    w.writeAttribute("modified", now);
    w.writeAttribute("visited", now);
    w.writeStartElement("info");
    w.writeStartElement("metadata");
    w.writeAttribute("owner", "http://freedesktop.org");
    w.writeEmptyElement(NS_MIME, "mime-type");
    w.writeAttribute("type", mime);
    w.writeStartElement(NS_BOOKMARK, "applications");
    w.writeEmptyElement(NS_BOOKMARK, "application");
    w.writeAttribute("name", APP_NAME);
    w.writeAttribute("exec", QString("'%1 %u'").arg(APP_COMMAND));
    w.writeAttribute("modified", now);
    w.writeAttribute("count", "1");
    w.writeEndElement();   // applications
    w.writeEndElement();   // metadata
    w.writeEndElement();   // info
    w.writeEndElement();   // bookmark
}

void add_recent(const QStringList &paths_in)
{
    QHash<QString, QString> wanted;   // href -> path
    for (const QString &p : paths_in)
        if (isfile(p))
            wanted.insert(file_uri(p), p);
    if (wanted.isEmpty() || !remember_recent())
        return;
    QString now = QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ss.zzz000Z");
    bool ok = false;
    QByteArray old = read_file(recent_file(), &ok);
    QByteArray out;
    QXmlStreamWriter w(&out);
    w.setAutoFormatting(true);
    w.setAutoFormattingIndent(2);
    QSet<QString> seen;
    bool have_root = false;
    if (ok && !old.trimmed().isEmpty()) {
        // copy the file, refreshing the dates (and our app entry) of bookmarks we're adding again
        QXmlStreamReader r(old);
        QString in_target;   // href of the bookmark being copied, if it's one of ours
        bool app_found = false;
        while (!r.atEnd()) {
            r.readNext();
            if (r.hasError())
                break;
            if (r.isStartDocument()) {
                w.writeStartDocument();
            } else if (r.isStartElement() && r.name() == u"xbel") {
                have_root = true;
                w.writeCurrentToken(r);
            } else if (r.isStartElement() && r.name() == u"bookmark" && wanted.contains(r.attributes().value("href").toString())) {
                in_target = r.attributes().value("href").toString();
                app_found = false;
                seen << in_target;
                w.writeStartElement("bookmark");
                for (const QXmlStreamAttribute &a : r.attributes())
                    if (a.name() != u"modified" && a.name() != u"visited")
                        w.writeAttribute(a);
                w.writeAttribute("modified", now);
                w.writeAttribute("visited", now);
            } else if (!in_target.isEmpty() && r.isStartElement() && r.name() == u"application" &&
                       r.namespaceUri() == NS_BOOKMARK && r.attributes().value("name") == QString(APP_NAME)) {
                app_found = true;
                w.writeStartElement(NS_BOOKMARK, "application");
                for (const QXmlStreamAttribute &a : r.attributes())
                    if (a.name() != u"modified" && a.name() != u"count")
                        w.writeAttribute(a);
                w.writeAttribute("modified", now);
                w.writeAttribute("count", QString::number(r.attributes().value("count").toInt() + 1));
            } else if (!in_target.isEmpty() && r.isEndElement() && r.name() == u"applications" && !app_found) {
                w.writeEmptyElement(NS_BOOKMARK, "application");
                w.writeAttribute("name", APP_NAME);
                w.writeAttribute("exec", QString("'%1 %u'").arg(APP_COMMAND));
                w.writeAttribute("modified", now);
                w.writeAttribute("count", "1");
                w.writeEndElement();
                app_found = true;
            } else if (r.isEndElement() && r.name() == u"bookmark" && r.namespaceUri().isEmpty()) {
                in_target.clear();
                w.writeEndElement();
            } else if (r.isEndElement() && r.name() == u"xbel") {
                for (auto it = wanted.begin(); it != wanted.end(); ++it)
                    if (!seen.contains(it.key()))
                        write_new_bookmark(w, it.key(), content_type(it.value()), now);
                w.writeEndElement();
            } else if (r.isCharacters() && r.isWhitespace()) {
                // auto-formatting re-indents
            } else if (!r.isEndDocument()) {
                w.writeCurrentToken(r);
            }
        }
        if (r.hasError())
            return;   // never overwrite a file we couldn't read
    }
    if (!have_root) {
        out.clear();
        QXmlStreamWriter fresh(&out);
        fresh.setAutoFormatting(true);
        fresh.setAutoFormattingIndent(2);
        fresh.writeStartDocument();
        fresh.writeStartElement("xbel");
        fresh.writeNamespace(NS_BOOKMARK, "bookmark");
        fresh.writeNamespace(NS_MIME, "mime");
        fresh.writeAttribute("version", "1.0");
        for (auto it = wanted.begin(); it != wanted.end(); ++it)
            write_new_bookmark(fresh, it.key(), content_type(it.value()), now);
        fresh.writeEndElement();
        fresh.writeEndDocument();
    } else {
        w.writeEndDocument();
    }
    QString tmp = recent_file() + ".kestrel-tmp";
    try {
        makedirs(dirname(recent_file()), true);
        write_text(tmp, out);
        util::chmod(tmp, 0600);
        util::rename(tmp, recent_file());
    } catch (const OSError &) {
        ::unlink(enc(tmp).constData());
    }
}

}  // namespace places
