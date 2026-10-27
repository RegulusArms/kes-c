// Image metadata: EXIF summary, image-generation prompts and settings (read from the file), full exiftool dump, exiftool editing and the tag catalog
// for the Add Tag picker.
#pragma once

#include <QByteArray>
#include <QDateTime>
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
// A photo's GPS position from its EXIF (degrees, south and west negative); false if it has none
bool gps_position(const QString &path, double *lat, double *lon);
// The EXIF Orientation tag of a JPEG or TIFF file, found so its value can be changed without re-encoding the image
struct OrientationTag {
    qint64 offset = -1;   // where its 2-byte value is in the file; -1: the file has none (or isn't a JPEG or TIFF)
    bool little_endian = true;
    int value = 1;        // 1–8, as EXIF numbers them
};
OrientationTag orientation_tag(const QString &path);
OrientationTag orientation_tag_in(const QByteArray &file);   // the same for a file's contents
QList<MetaRow> full_metadata(const QString &path);
// whether full_metadata has something to show for a file (by its name): images, and with exiftool also videos, audio,
// PDFs, e-books and office documents (the right-click menu's Open Metadata)
bool viewable(const QString &path);
// When a photo or video was taken, from its metadata (EXIF DateTimeOriginal / DateTimeDigitized, a PNG's Creation Time,
// an MP4 / MOV movie header's creation time); invalid when it has none. Reads the file: not on the UI thread.
QDateTime taken(const QString &path);
bool may_have_taken(const QString &path);   // a type taken() reads (by its name)
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
// Wipe Metadata…: every tag taken out (EXIF, XMP, IPTC, GPS, comments, a PNG's text…), the pixels untouched
// (exiftool). For images Kestrel can wipe: not camera raw files, whose maker notes are needed to develop them.
bool can_wipe(const QString &path);
// path without its metadata (keep_basic: its orientation and colour profile stay), written over path itself (fsynced
// first) or, with copy, as a new file beside it ("photo (no metadata).jpg", the next free name, with path's
// permissions). Either way a part file renamed into place: a failure leaves path and the name as they were. Returns
// the file written; raises Error.
QString wipe(const QString &path, bool copy, bool keep_basic = true);

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
