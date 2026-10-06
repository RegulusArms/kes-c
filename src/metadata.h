// Image metadata: EXIF summary, AI-generation parameters, full exiftool dump, exiftool editing and the tag catalog
// for the Add Tag picker.
#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace metadata {

using Rows = QList<QPair<QString, QString>>;   // ordered (label, value)

struct MetaRow {
    QString group, tag, value;
};

Rows basic_info(const QString &path);   // fast summary for the info panel
Rows ai_info(const QString &path);      // Stable Diffusion / ComfyUI generation info
QList<MetaRow> full_metadata(const QString &path);
// An object's keys in the order they were written, with their values (each key once, as Python's json.loads): the
// object `json` is, or the first in an array of them (exiftool's -j output); empty if it isn't valid JSON
QList<QPair<QString, QJsonValue>> ordered_object(const QByteArray &json);

// ---------------------------------------------------------------- editing (exiftool)

bool can_edit();
bool is_editable(const QString &group, const QString &value = QString(), const QString &tag = QString());
// the items of a list-type value as shown by full_metadata(); ok=false if it isn't a list
QStringList as_list(const QString &value, bool *ok);

struct Change {
    QString key;          // "Group:Tag" or "Tag"
    QStringList values;   // one value, several for list-type tags
    bool is_list = false;
    bool remove = false;
};
QStringList set_tags(const QString &path, const QList<Change> &changes);   // warnings; raises Error
QStringList clear_all(const QString &path, bool keep_basic = true);

// ---------------------------------------------------------------- tag catalog

struct CatalogEntry {
    QString family, key, blurb;
};
extern const QList<CatalogEntry> TAG_CATALOG;
QString tag_help(const QString &key);   // the hand-written description of a catalog key, or empty
bool is_list_tag(const QString &key);
QPair<QString, QString> tag_identity(const QString &key);
QStringList tag_families(const QString &path);

struct DbRow {
    QString name, desc, kind;
    QStringList values;
    QString category;
};
using TagDb = QHash<QString, QList<DbRow>>;
const TagDb *tag_db_if_loaded();   // nullptr until tag_db() has run
const TagDb &tag_db();             // slow the first time (~6 s); call from a worker thread

struct TagChoice {
    QString key, blurb, kind;
    QStringList values;
};
using Section = QPair<QString, QList<TagChoice>>;
QList<Section> tag_choices(const QString &path, const TagDb *db);

}  // namespace metadata
