// Saving the image viewer's changes to image files: a rotation or flip without re-encoding the image (the EXIF
// orientation tag), turning that tag into real pixels (Straighten), and crops and resizes, as a new file or over the
// original. Every write is a part file renamed into place (util write_parts); the original is untouched on failure.
#pragma once

#include <QImage>
#include <QList>
#include <QRect>
#include <QSize>
#include <QString>

namespace imgedit {

// The EXIF orientation (1–8) that shows a file stored with `orientation` the way the viewer shows it after turning it
// `rotation` degrees clockwise (0, 90, 180, 270), mirrored left to right first when `flip`.
int compose(int orientation, int rotation, bool flip);

// How save_rotation() would save this file, without losing quality: "tag" (change its orientation tag in place),
// "exiftool" (a JPEG without one: exiftool adds it), "pixels" (a lossless format, written again turned), or empty
// when it can't be saved without losing quality.
QString rotation_method(const QString &path);
void save_rotation(const QString &path, int rotation, bool flip);   // raises Error

// Straighten: a JPEG whose orientation tag turns it gets its pixels turned instead (jpegtran, lossless) and the tag
// set to 1, for programs that ignore the tag.
bool can_straighten(const QString &path);
void straighten(const QString &path);   // raises Error

// What the viewer shows, changed: turned by `rotation` / `flip` (as compose()), then cropped to `crop` (in the turned
// image's pixels; empty: all of it), then scaled to `size` (empty: as it is).
struct Edit {
    int rotation = 0;
    bool flip = false;
    QRect crop;
    QSize size;
};
QImage apply(const QImage &img, const Edit &e);
// The viewer's preview of edits not saved yet: the steps applied in turn to `img`, a smaller copy (a screen-size decode)
// of an image whose real size is `full`; *real gets the edited image's real size.
QImage preview(const QImage &img, QSize full, const QList<Edit> &steps, QSize *real);
// Reads src at full size, applies e and writes it to dst: a new file (it must still be free), or src itself
// replaced when dst == src. In src's format when Kestrel can write it, else as JPEG; a JPEG keeps its EXIF and XMP
// (camera details, with the orientation tag set to 1), a PNG its text (image-generation prompts). Raises Error.
void save_edit(const QString &src, const QString &dst, const QList<Edit> &steps);   // the steps in turn
inline void save_edit(const QString &src, const QString &dst, const Edit &e) { save_edit(src, dst, QList<Edit>{e}); }
bool can_write(const QString &path);   // Kestrel can write files in this one's format
// a free name for an edited copy: "photo (cropped).jpg", "photo (cropped 2).jpg"… (.jpg when src's format can't be
// written)
QString new_name(const QString &src, const QString &what);

}  // namespace imgedit
