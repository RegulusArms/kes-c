// The image viewer and saving its changes: lossless rotations, Straighten, crop and resize, GPS and the info panel,
// screen-size decoding, the filmstrip, zoom, compare, quick actions, remembered state, and the order it shows a
// selection in.
#include "common.h"
#include "imgedit.h"
#include "menus.h"
#include "metadata.h"
#include "places.h"
#include "viewer.h"

#include <QBuffer>
#include <QComboBox>
#include <QMenu>
#include <QTreeWidget>
#include <QImageWriter>
#include <QMessageBox>
#include <QPushButton>

#include <cmath>
#include <sys/resource.h>

using namespace test;

// ---------------------------------------------------------------- test images

static QByteArray le16(quint32 v)
{
    QByteArray b(2, '\0');
    b[0] = char(v & 0xff);
    b[1] = char(v >> 8 & 0xff);
    return b;
}
static QByteArray le32(quint32 v) { return le16(v & 0xffff) + le16(v >> 16); }

struct Entry {
    quint16 tag, type;
    quint32 count;
    QByteArray data;
};

// An IFD starting at `base` in the TIFF block, its values after it
static QByteArray build_ifd(const QList<Entry> &es, quint32 base)
{
    QByteArray head = le16(es.size()), data;
    quint32 data_at = base + 2 + es.size() * 12 + 4;
    for (const Entry &e : es) {
        head += le16(e.tag) + le16(e.type) + le32(e.count);
        if (e.data.size() <= 4) {
            head += e.data + QByteArray(4 - e.data.size(), '\0');
        } else {
            head += le32(data_at + data.size());
            data += e.data;
            if (data.size() & 1)
                data += '\0';
        }
    }
    return head + le32(0) + data;
}

static QByteArray ascii(const QString &s) { return s.toLatin1() + '\0'; }

static QByteArray dms(double v)
{
    v = std::fabs(v);
    double d = std::floor(v), m = std::floor((v - d) * 60), s = ((v - d) * 60 - m) * 60;
    return le32(quint32(d)) + le32(1) + le32(quint32(m)) + le32(1) + le32(quint32(std::lround(s * 1000))) + le32(1000);
}

// A little-endian EXIF block: orientation (0: no tag), make and model (empty: none), a GPS position (NaN: none)
static QByteArray exif_block(int orientation, const QString &make, const QString &model, double lat = NAN, double lon = NAN)
{
    QList<Entry> ifd0;
    if (!make.isEmpty())
        ifd0 << Entry{0x010F, 2, quint32(ascii(make).size()), ascii(make)};
    if (!model.isEmpty())
        ifd0 << Entry{0x0110, 2, quint32(ascii(model).size()), ascii(model)};
    if (orientation)
        ifd0 << Entry{0x0112, 3, 1, le16(orientation)};
    bool gps = !std::isnan(lat);
    if (gps)
        ifd0 << Entry{0x8825, 4, 1, le32(0)};   // filled in below
    QByteArray first = build_ifd(ifd0, 8);
    if (gps) {
        quint32 gps_at = 8 + first.size();
        ifd0.last().data = le32(gps_at);
        first = build_ifd(ifd0, 8);
        QList<Entry> g{{1, 2, 2, ascii(lat < 0 ? "S" : "N")}, {2, 5, 3, dms(lat)},
                       {3, 2, 2, ascii(lon < 0 ? "W" : "E")}, {4, 5, 3, dms(lon)}};
        first += build_ifd(g, gps_at);
    }
    return QByteArray("II*\0", 4) + le32(8) + first;
}

// 64 × 48: red, green / blue, white quarters
static QImage quarters(int w = 64, int h = 48)
{
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.setPixel(x, y, y < h / 2 ? (x < w / 2 ? qRgb(255, 0, 0) : qRgb(0, 255, 0))
                                         : (x < w / 2 ? qRgb(0, 0, 255) : qRgb(255, 255, 255)));
    return img;
}

static void write_bytes(const QString &path, const QByteArray &b)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(b);
}

static QByteArray jpeg_of(const QImage &img, const QByteArray &exif = QByteArray())
{
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    QImageWriter w(&buf, "jpeg");
    w.setQuality(98);
    w.write(img);
    QByteArray j = buf.data();
    if (exif.isEmpty())
        return j;
    QByteArray app1 = QByteArray("Exif\0\0", 6) + exif;
    QByteArray seg = QByteArray("\xff\xe1", 2) + char((app1.size() + 2) >> 8) + char((app1.size() + 2) & 0xff) + app1;
    return j.left(2) + seg + j.mid(2);
}

static QImage shown(const QString &path)   // as a viewer shows it
{
    QImageReader r(path);
    r.setAutoTransform(true);
    return r.read();
}

// The colours of an image's four corners: R G B W (K: none of them)
static QString corners(const QImage &img)
{
    if (img.isNull())
        return "null";
    auto name = [&](int x, int y) {
        QColor c = img.pixelColor(x, y);
        int r = c.red() > 128, g = c.green() > 128, b = c.blue() > 128;
        return r && g && b ? 'W' : r ? 'R' : g ? 'G' : b ? 'B' : 'K';
    };
    int w = img.width() - 3, h = img.height() - 3;
    return QString(QChar(name(2, 2))) + QChar(name(w, 2)) + QChar(name(2, h)) + QChar(name(w, h));
}

static QStringList parts_in(const QString &dir)
{
    QStringList out;
    for (const QString &n : listdir(dir))
        if (n.startsWith(".kes-") || n.contains("_exiftool_tmp"))
            out << n;
    return out;
}

template <typename F> static QString error_of(F f)
{
    try {
        f();
        return QString();
    } catch (const Error &e) {
        return e.message().isEmpty() ? QString("error") : e.message();
    }
}

// The button of a question box with this text
static QAbstractButton *button(QWidget *w, const QString &text)
{
    auto *box = qobject_cast<QMessageBox *>(w);
    if (!box)
        return nullptr;
    for (QAbstractButton *b : box->buttons())
        if (b->text() == text)
            return b;
    return nullptr;
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    setup_app();
    QString dir = home_path("pics");
    makedirs(dir);
    QImage q = quarters();

    // ---- orientations
    check(imgedit::compose(1, 90, false) == 6 && imgedit::compose(1, 180, false) == 3 &&
              imgedit::compose(1, 270, false) == 8 && imgedit::compose(1, -90, false) == 8 &&
              imgedit::compose(1, 0, true) == 2 && imgedit::compose(1, 90, true) == 7 &&
              imgedit::compose(6, 90, false) == 3 && imgedit::compose(8, 90, false) == 1 &&
              imgedit::compose(2, 0, true) == 1,
          "a viewer rotation and flip on top of an orientation tag give the EXIF orientation that shows the same");

    // ---- saving a rotation: only the orientation tag changes
    QString bad;
    bool lossless = true;
    QString t = join(dir, "turn.jpg");
    for (int o = 1; o <= 8; ++o)
        for (int rot : {0, 90, 180, 270})
            for (bool flip : {false, true}) {
                write_bytes(t, jpeg_of(q, exif_block(o, "Cam", "One")));
                QByteArray before = read_file(t);
                QString want = corners(imgedit::apply(shown(t), imgedit::Edit{rot, flip, QRect(), QSize()}));
                QString err = error_of([&]() { imgedit::save_rotation(t, rot, flip); });
                QByteArray after = read_file(t);
                int differ = 0;
                for (int i = 0; i < std::min(before.size(), after.size()); ++i)
                    differ += before[i] != after[i];
                if (after.size() != before.size() || differ > 2)
                    lossless = false;
                QString got = corners(shown(t));
                if (!err.isEmpty() || got != want)
                    bad += QString(" %1/%2/%3: %4 not %5 %6").arg(o).arg(rot).arg(flip).arg(got, want, err);
            }
    check(bad.isEmpty(), "a saved rotation (every orientation, turned 0–270°, flipped or not) shows the image as the "
                         "viewer showed it" + bad.left(300));
    check(lossless && imgedit::rotation_method(t) == "tag",
          "...by changing only the orientation tag's two bytes (the image isn't re-encoded)");

    // a JPEG without a tag: exiftool adds one
    QString nt = join(dir, "notag.jpg");
    write_bytes(nt, jpeg_of(q));
    if (which("exiftool")) {
        QString want = corners(imgedit::apply(q, imgedit::Edit{90, false, QRect(), QSize()}));
        QString err = error_of([&]() { imgedit::save_rotation(nt, 90, false); });
        check(err.isEmpty() && metadata::orientation_tag(nt).value == 6 && corners(shown(nt)) == want && parts_in(dir).isEmpty(),
              "a JPEG without an orientation tag gets one from exiftool (" + err + ")");
    } else {
        skip("a JPEG without an orientation tag gets one from exiftool (exiftool isn't installed)");
    }

    // a PNG is written again, turned, keeping its text (generation prompts)
    QString png = join(dir, "gen.png");
    {
        QImage g = q;
        g.setText("parameters", "a kestrel over a field, Steps: 20");
        g.save(png);
    }
    QImage png_want = imgedit::apply(shown(png), imgedit::Edit{270, true, QRect(), QSize()});
    QString err = error_of([&]() { imgedit::save_rotation(png, 270, true); });
    QImage png_got = shown(png);
    check(err.isEmpty() && imgedit::rotation_method(png) == "pixels" && png_got.convertToFormat(QImage::Format_RGB32) == png_want.convertToFormat(QImage::Format_RGB32) &&
              png_got.text("parameters") == "a kestrel over a field, Steps: 20",
          "a PNG's rotation is saved by writing it again turned, exactly, with its text kept (" + err + ")");
    check(imgedit::rotation_method(join(TESTS_DIR, "fixtures")).isEmpty() && imgedit::rotation_method(join(dir, "x.webp")).isEmpty(),
          "a format that can't be turned without losing quality has no way to save a rotation");

    // a write that fails part-way (a full disk) leaves the file as it was, and no part file
    {
        write_bytes(t, jpeg_of(q, exif_block(1, "Cam", "One")));
        QString png2 = join(dir, "full.png");
        q.save(png2);
        QByteArray jb = read_file(t), pb = read_file(png2);
        struct rlimit old_lim, lim{64, 64};
        ::getrlimit(RLIMIT_FSIZE, &old_lim);
        auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
        lim.rlim_max = old_lim.rlim_max;
        ::setrlimit(RLIMIT_FSIZE, &lim);
        QString e1 = error_of([&]() { imgedit::save_rotation(t, 90, false); });
        QString e2 = error_of([&]() { imgedit::save_rotation(png2, 90, false); });
        QString e3 = error_of([&]() { imgedit::save_edit(png2, png2, imgedit::Edit{0, false, QRect(0, 0, 10, 10), QSize()}); });
        QString e4 = error_of([&]() { imgedit::save_edit(t, join(dir, "new.jpg"), imgedit::Edit{0, false, QRect(0, 0, 10, 10), QSize()}); });
        ::setrlimit(RLIMIT_FSIZE, &old_lim);
        ::signal(SIGXFSZ, old_handler);
        check(!e1.isEmpty() && !e2.isEmpty() && !e3.isEmpty() && !e4.isEmpty() && read_file(t) == jb &&
                  read_file(png2) == pb && !lexists(join(dir, "new.jpg")) && parts_in(dir).isEmpty(),
              "saving a rotation, a crop over the original or a new crop that fails part-way (a file-size limit) keeps "
              "the original as it was, and leaves no part file or half-made image");
    }

    // ---- Straighten: the tag's turn into the pixels (jpegtran), the tag set to 1
    QString st = join(dir, "straight.jpg");
    write_bytes(st, jpeg_of(q, exif_block(6, "Cam", "One")));
    if (which("jpegtran")) {
        QString before = corners(shown(st));
        QString e = error_of([&]() { imgedit::straighten(st); });
        check(e.isEmpty() && metadata::orientation_tag(st).value == 1 && QImageReader(st).size() == QSize(48, 64) &&
                  corners(shown(st)) == before && !imgedit::can_straighten(st),
              "Straighten turns a JPEG's pixels as its orientation tag said and sets the tag to 1: it looks the same (" + e + ")");
    } else {
        skip("Straighten turns a JPEG's pixels as its orientation tag said (jpegtran isn't installed)");
    }

    // ---- crop and resize
    QString cam = join(dir, "photo.jpg");
    write_bytes(cam, jpeg_of(q, exif_block(6, "TestCam", "X1")));   // shown 48 × 64
    QByteArray cam_bytes = read_file(cam);
    QString dst = imgedit::new_name(cam, "cropped");
    err = error_of([&]() { imgedit::save_edit(cam, dst, imgedit::Edit{0, false, QRect(0, 0, 24, 32), QSize()}); });
    QString camera;
    for (const auto &[k, v] : metadata::basic_info(dst))
        if (k == "Camera")
            camera = v;
    check(err.isEmpty() && basename(dst) == "photo (cropped).jpg" && shown(dst).size() == QSize(24, 32) &&
              corners(shown(dst)) == QString(4, corners(shown(cam))[0]) && read_file(cam) == cam_bytes,
          "a crop is saved as a new file beside the original, as shown (turned by its tag), the original untouched (" + err + ")");
    check(camera == "TestCam X1" && metadata::orientation_tag(dst).value == 1,
          "...keeping the camera details, with the orientation tag set to 1 (its pixels are turned already)");
    QSize real;
    QImage pv = imgedit::preview(q.scaled(32, 24), q.size(), {imgedit::Edit{0, false, QRect(0, 0, 32, 24), QSize()},
                                                              imgedit::Edit{90, false, QRect(), QSize(6, 8)}}, &real);
    QSize real1;
    QImage pv1 = imgedit::preview(q.scaled(32, 24), q.size(), {imgedit::Edit{0, false, QRect(0, 0, 32, 24), QSize()}}, &real1);
    check(real == QSize(6, 8) && pv.size() == QSize(3, 4) && real1 == QSize(32, 24) && pv1.size() == QSize(16, 12) &&
              corners(pv1) == "RRRR",
          "the viewer's preview of edits on a half-size copy crops, turns and scales it as the real image would be");
    check(basename(imgedit::new_name(cam, "cropped")) == "photo (cropped 2).jpg" &&
              !error_of([&]() { imgedit::save_edit(cam, dst, imgedit::Edit{}); }).isEmpty() && shown(dst).size() == QSize(24, 32),
          "the next crop gets another name, and a new file never replaces one that's there");
    QString gp = join(dir, "gen2.png");
    {
        QImage g = q;
        g.setText("prompt", "{\"3\": {}}");
        g.save(gp);
    }
    err = error_of([&]() { imgedit::save_edit(gp, gp, imgedit::Edit{90, false, QRect(), QSize(24, 32)}); });
    check(err.isEmpty() && shown(gp).size() == QSize(24, 32) && shown(gp).text("prompt") == "{\"3\": {}}" && parts_in(dir).isEmpty(),
          "a resize over the original replaces it, turned as shown, with a PNG's text kept (" + err + ")");

    // ---- GPS
    QString geo = join(dir, "geo.jpg");
    write_bytes(geo, jpeg_of(q, exif_block(1, "TestCam", "X1", 51.5, -0.125)));
    double lat = 0, lon = 0;
    QString gps_row;
    for (const auto &[k, v] : metadata::basic_info(geo))
        if (k == "GPS")
            gps_row = v;
    check(metadata::gps_position(geo, &lat, &lon) && std::fabs(lat - 51.5) < 1e-6 && std::fabs(lon + 0.125) < 1e-6 &&
              gps_row == "51.500000, -0.125000" && !metadata::gps_position(cam, &lat, &lon),
          "a photo's GPS position is read from its EXIF (south and west negative) and shown as coordinates (" + gps_row + ")");

    // ---- the viewer
    QString vdir = home_path("view");
    makedirs(vdir);
    QString a = join(vdir, "a.jpg"), b = join(vdir, "b.png"), c = join(vdir, "c.jpg"), d = join(vdir, "d.png");
    write_bytes(a, jpeg_of(q, exif_block(1, "TestCam", "X1", 51.5, -0.125)));
    quarters(3000, 2000).save(b);
    write_bytes(c, jpeg_of(q, exif_block(1, "Cam", "One")));
    q.save(d);
    QString moved_dir = home_path("keep"), bin = home_path("bin");
    makedirs(moved_dir);
    makedirs(bin);
    QList<QStringList> transfers;
    QString last_gone;   // what the stubs moved away, for the undo stub to put back
    ImageViewer::Callbacks cb;
    cb.on_transfer = [&](const QString &p, const QString &folder, const QString &op) {
        transfers << QStringList{p, folder, op};
        if (op == "move") {
            util::rename(p, join(folder, basename(p)));
            last_gone = p;
        }
    };
    cb.on_delete = [&](const QString &p) {
        util::rename(p, join(bin, basename(p)));
        last_gone = p;
        return true;
    };
    cb.on_undo = [&](std::function<void()> after) {
        if (!last_gone.isEmpty()) {
            QString from = lexists(join(bin, basename(last_gone))) ? join(bin, basename(last_gone)) : join(moved_dir, basename(last_gone));
            util::rename(from, last_gone);
            last_gone.clear();
        }
        after();
    };
    QList<QWidget *> questions;
    auto open = [&](const QStringList &list, int i) {
        auto *v = new ImageViewer(list, i, cb);
        v->on_question = [&](QWidget *box) { questions << box; };
        v->show();
        return v;
    };
    QPointer<ImageViewer> v = open({a, b, c, d}, 0);
    check(wait_for([&]() { return v->cache.contains(a) && !v->canvas->pix.isNull(); }), "the viewer shows the first image");

    // screen-size decodes, full size when zoomed in
    check(wait_for([&]() { return v->cache.contains(b); }) && !v->cache.value(b).is_full &&
              std::max(v->cache.value(b).pix.width(), v->cache.value(b).pix.height()) < 3000 &&
              v->cache.value(b).full == QSize(3000, 2000),
          "the next image is decoded ahead of time at screen size, knowing its real size (" +
              QString("%1×%2").arg(v->cache.value(b).pix.width()).arg(v->cache.value(b).pix.height()) + ")");
    v->go_to(1);
    check(v->canvas->full == QSize(3000, 2000) && v->canvas->fit_scale() < 1, "it's shown fitted, as its real size");
    QPointF mid(v->canvas->width() / 2.0, v->canvas->height() / 2.0);
    QMouseEvent dbl(QEvent::MouseButtonDblClick, mid, v->canvas->mapToGlobal(mid), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(v->canvas, &dbl);
    check(v->canvas->zoom && std::fabs(*v->canvas->zoom - 1.0) < 1e-9 && v->canvas->osd_text() == "100%",
          "double-clicking zooms to actual size at the pointer, saying so on screen");
    check(wait_for([&]() { return v->cache.value(b).is_full && v->cache.value(b).pix.size() == QSize(3000, 2000); }),
          "...and at actual size the image is decoded at full size");
    QApplication::sendEvent(v->canvas, &dbl);
    check(!v->canvas->zoom, "double-clicking again fits it to the window");
    QNativeGestureEvent pinch(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2, mid, mid,
                              v->canvas->mapToGlobal(mid), 0.5, QPointF());
    double before_pinch = v->canvas->scale();
    QApplication::sendEvent(v->canvas, &pinch);
    check(v->canvas->zoom && std::fabs(*v->canvas->zoom - before_pinch * 1.5) < 1e-6, "a touchpad pinch zooms");

    // keep zoom
    v->canvas->set_zoom(1.0);
    v->toggle_keep_zoom();
    v->navigate(1);
    bool kept = v->canvas->zoom && *v->canvas->zoom == 1.0;
    v->toggle_keep_zoom();
    v->navigate(-1);
    check(kept && !v->canvas->zoom && !settings().value("viewer_keep_zoom").toBool(),
          "with Keep Zoom (K) the next image opens at the same zoom; without it, fitted");

    // filmstrip
    check(!v->filmstrip->isHidden() && v->filmstrip->paths.size() == 4 && v->filmstrip->current == 1,
          "the filmstrip shows every image, centred on the current one");
    QPoint cell = v->filmstrip->cell_rect(3).center();
    QMouseEvent press(QEvent::MouseButtonPress, cell, v->filmstrip->mapToGlobal(cell), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(v->filmstrip, &press);
    check(v->index == 3 && v->current() == d, "clicking a thumbnail in the filmstrip shows that image");
    v->toggle_filmstrip();
    check(v->filmstrip->isHidden() && !settings().value("viewer_filmstrip").toBool(), "T hides the filmstrip, and it's remembered");

    // info panel
    v->go_to(0);
    v->toggle_info();
    check(wait_for([&]() { return v->info->loaded; }) && v->info->text().contains("TestCam X1") &&
              v->info->map_url.contains("mlat=51.500000&mlon=-0.125000") && settings().value("viewer_info_panel").toBool(),
          "the info panel (I) shows the photo's details and a map link for its GPS position, and is remembered");
    int counted = 0, reds = 0;
    for (int i = 0; i < 256; ++i)
        counted += v->info->histogram->bins[0][i];
    for (int i = 129; i < 256; ++i)
        reds += v->info->histogram->bins[0][i];
    check(counted > 0 && reds * 2 > counted * 0.9 && reds * 2 < counted * 1.1,
          "...and a histogram of it (half of this image is bright red or white)");

    // compare
    v->toggle_compare();
    check(v->compare == 1 && v->pinned_path == a && v->current() == b && v->pinned->isVisible() && v->canvas->isVisible(),
          "X compares the image with the next one, side by side");
    check(wait_for([&]() { return !v->pinned->pix.isNull() && !v->canvas->full.isEmpty(); }), "...both shown");
    v->canvas->set_zoom(v->canvas->fit_scale() * 2);
    check(v->pinned->zoom && std::fabs(*v->pinned->zoom / v->pinned->fit_scale() - 2) < 1e-6,
          "zooming one zooms the other the same (relative to fitting)");
    v->toggle_ab();
    bool b_only = v->canvas->isVisible() && !v->pinned->isVisible();
    v->flip_ab();
    check(b_only && v->pinned->isVisible() && !v->canvas->isVisible() && v->pinned->tag == "A",
          "V shows them one at a time, and B switches between A and B");
    v->toggle_compare();
    check(!v->compare && !v->pinned->isVisible() && v->canvas->isVisible(), "X again stops comparing");

    // quick actions
    v->go_to(0);
    v->toggle_star();
    bool starred = places::is_starred(a) && v->canvas->overlay.contains("★");
    v->toggle_star();
    check(starred && !places::is_starred(a), "* stars and unstars the image");
    v->go_to(2);
    v->transfer(moved_dir, "move");
    check(transfers.size() == 1 && transfers[0] == QStringList({c, moved_dir, "move"}) && !v->paths.contains(c) &&
              v->current() != c && !v->windowTitle().startsWith("c.jpg") && v->canvas->osd_text() == "Moving to keep" &&
              ImageViewer::recent_folders().value(0) == moved_dir,
          "moving the image to a folder hands it to the file operations, takes it off the list and remembers the folder");
    v->undo();
    check(wait_for([&]() { return v->paths.contains(c) && v->current() == c; }), "Ctrl+Z puts it back, in the list too");
    v->transfer_recent(1, "copy");
    v->transfer_recent(4, "copy");
    check(transfers.size() == 2 && transfers[1] == QStringList({c, moved_dir, "copy"}) && v->canvas->osd_text().contains("No recent folder 4"),
          "Alt+1 copies it to the latest folder in one key; one that isn't there yet says so");
    v->delete_current();
    check(!v->paths.contains(c) && lexists(join(bin, "c.jpg")), "Delete moves it to the trash and off the list");
    v->undo();
    check(wait_for([&]() { return v->paths.contains(c) && v->current() == c && lexists(c); }), "...and Ctrl+Z puts it back");

    // rotations, saved when leaving the image
    settings().setValue("viewer_save_rotation", "ask");
    v->go_to(0);
    v->canvas->rotate(90);
    v->navigate(1);
    QWidget *qbox = questions.value(0);
    check(qbox && is_free_window(qbox) && v->index == 1 && metadata::orientation_tag(a).value == 1,
          "leaving a turned image asks (a free window) whether to save its rotation, and moves on meanwhile");
    if (QAbstractButton *save = button(qbox, "Save"))
        save->click();
    check(metadata::orientation_tag(a).value == 6, "Save saves it: the orientation tag now turns it");
    v->go_to(0);
    check(wait_for([&]() { return v->canvas->full == QSize(48, 64); }) && v->canvas->rotation == 0,
          "...and it's shown turned by its tag, not turned again");
    v->canvas->rotate(90);
    questions.clear();
    v->navigate(1);
    if (QAbstractButton *always = button(questions.value(0), "Always Save"))
        always->click();
    check(metadata::orientation_tag(a).value == 3 && settings().value("viewer_save_rotation").toString() == "always",
          "Always Save saves it and makes that the setting");
    v->go_to(0);
    v->canvas->rotate(90);
    questions.clear();
    v->navigate(1);
    check(questions.isEmpty() && metadata::orientation_tag(a).value == 8, "then leaving a turned image saves it without asking");
    settings().setValue("viewer_save_rotation", "never");
    v->go_to(0);
    v->canvas->rotate(90);
    v->navigate(1);
    check(questions.isEmpty() && metadata::orientation_tag(a).value == 8, "with Never, rotations are only for viewing");
    v->go_to(0);
    v->canvas->rotate(90);   // 8 (a quarter turn left) and a quarter turn right
    v->save_changes();
    check(metadata::orientation_tag(a).value == 1 && v->canvas->rotation == 0, "Ctrl+S saves the rotation at once");

    // crop and resize in the viewer: shown at once, written only when saved
    settings().setValue("viewer_save_rotation", "ask");
    v->go_to(v->paths.indexOf(c));
    wait_for([&]() { return v->cache.contains(c) && !v->canvas->pix.isNull(); });
    QByteArray c_bytes = read_file(c);
    questions.clear();
    v->canvas->rotate(90);
    v->add_edit(QRect(), QSize(24, 32));
    check(questions.isEmpty() && v->canvas->full == QSize(24, 32) && corners(v->canvas->pix.toImage()) == "BRWG" &&
              v->canvas->rotation == 0 && read_file(c) == c_bytes && v->canvas->overlay.contains("not saved"),
          "Resize (Shift+S) after turning the image shows it resized and turned at once, and writes nothing yet");
    v->discard_edits();
    check(v->steps.isEmpty() && v->canvas->full == QSize(64, 48) && corners(v->canvas->pix.toImage()) == "RGBW" &&
              read_file(c) == c_bytes,
          "Esc undoes the edits: the image is shown as it is in the file");
    v->start_crop();
    v->canvas->crop = QRectF(0, 0, 32, 24);
    v->finish_crop();
    check(questions.isEmpty() && v->canvas->full == QSize(32, 24) && corners(v->canvas->pix.toImage()) == "RRRR" &&
              read_file(c) == c_bytes,
          "Crop (Shift+C) shows the cropped image at once, and writes nothing yet");
    questions.clear();
    v->save_changes();
    if (QAbstractButton *as_new = button(questions.value(0), "Save as New File"))
        as_new->click();
    QString cropped = join(vdir, "c (cropped).jpg");
    check(wait_for([&]() { return v->current() == cropped; }) && shown(cropped).size() == QSize(32, 24) &&
              read_file(c) == c_bytes && undo::label() == "Crop",
          "Ctrl+S saves it as a new file (asked), shown next, the original untouched, and Ctrl+Z would trash it");
    v->go_to(v->paths.indexOf(d));
    wait_for([&]() { return v->cache.contains(d) && !v->canvas->pix.isNull(); });
    v->add_edit(QRect(), QSize(32, 24));
    questions.clear();
    v->navigate(-1);
    QAbstractButton *replace = button(questions.value(0), "Replace Original");
    bool unwritten = shown(d).size() == QSize(64, 48) && button(questions.value(0), "Discard");
    if (replace)
        replace->click();
    check(unwritten && replace && wait_for([&]() { return shown(d).size() == QSize(32, 24); }),
          "leaving an edited image asks (Save as New File, Replace Original, Discard); Replace writes it over the original");
    v->go_to(v->paths.indexOf(d));
    check(wait_for([&]() { return v->canvas->full == QSize(32, 24); }) && v->steps.isEmpty(),
          "...and the viewer shows the new one, not edited again");

    // the right-click menu, laid out in Preferences → Menus
    {
        v->go_to(0);
        wait_for([&]() { return v->cache.contains(a) && !v->canvas->pix.isNull(); });
        v->toggle_compare();
        v->toggle_ab();
        v->add_edit(QRect(), QSize(32, 24));   // (not allowed while comparing: refused, so no Undo the Edits here)
        QStringList order;
        std::function<void(const QList<menus::Entry> &)> flat = [&](const QList<menus::Entry> &l) {
            for (const menus::Entry &e : l)
                if (e.type != menus::Entry::SEP) {
                    order << e.id;
                    flat(e.children);
                }
        };
        flat(menus::defaults(menus::VIEWER));
        QStringList problems, seen;
        int last = -1;
        std::function<void(QMenu *)> walk = [&](QMenu *menu) {
            for (QAction *x : menu->actions()) {
                if (x->isSeparator())
                    continue;
                QString id = menus::id_of(x);
                int at = order.indexOf(id, std::max(last, 0));
                if (id.isEmpty())
                    problems << "no id for " + x->text();
                else if (at < 0)
                    problems << id + " not in the defaults (or out of order)";
                else
                    last = at;
                seen << id;
                if (x->menu() && (id == "viewer_view" || id == "viewer_edit"))
                    walk(x->menu());
            }
        };
        QMenu *m = v->build_menu();
        walk(m);
        delete m;
        v->toggle_compare();
        v->add_edit(QRect(), QSize(32, 24));
        m = v->build_menu();
        last = -1;
        walk(m);
        delete m;
        v->discard_edits();
        check(problems.isEmpty() && seen.contains("compare_switch") && seen.contains("undo_edits") && seen.contains("crop"),
              "every entry of the viewer's right-click menu has an id, and its default layout lists them in the menu's "
              "order" + (problems.isEmpty() ? QString() : " (" + problems.mid(0, 4).join("; ") + ")"));

        auto shape = [&](QMenu *menu) {
            QStringList out;
            for (QAction *x : menu->actions()) {
                if (x->isSeparator())
                    out << "-";
                else if (x->menu() && menus::id_of(x) == "viewer_edit") {
                    QStringList kids;
                    for (QAction *k : x->menu()->actions())
                        kids << (k->isSeparator() ? QString("-") : menus::id_of(k));
                    out << "viewer_edit[" + kids.join(",") + "]";
                } else
                    out << menus::id_of(x);
            }
            return out;
        };
        QList<menus::Entry> lay = menus::defaults(menus::VIEWER);
        menus::Entry props, crop;
        for (int i = 0; i < lay.size(); ++i)
            if (lay[i].id == "trash")
                props = lay.takeAt(i--);
            else if (lay[i].id == "star")
                lay[i].off = true;
            else if (lay[i].id == "viewer_edit")
                for (int j = 0; j < lay[i].children.size(); ++j)
                    if (lay[i].children[j].id == "crop")
                        crop = lay[i].children.takeAt(j--);
        lay.insert(0, props);
        lay.insert(2, crop);
        menus::save(menus::VIEWER, lay);
        m = v->build_menu();
        QStringList got = shape(m);
        delete m;
        menus::reset(menus::VIEWER);
        m = v->build_menu();
        QStringList back = shape(m);
        delete m;
        check(got.mid(0, 4) == QStringList({"trash", "next", "crop", "previous"}) && !got.contains("star") &&
                  got.contains("pin") && got.filter("viewer_edit[").value(0).contains("resize") &&
                  !got.filter("viewer_edit[").value(0).contains("crop") && back.value(0) == "next" && back.contains("star"),
              "the viewer's menu follows its saved layout (an entry hidden, one moved to the top, one taken out of Rotate "
              "and Edit), and Reset puts it back (" + got.mid(0, 6).join(" ") + ")");

        menus::Editor ed;
        int at = ed.kind_box->findText("The image viewer");
        ed.kind_box->setCurrentIndex(at);
        QStringList tops;
        for (int i = 0; i < ed.tree->topLevelItemCount(); ++i)
            tops << ed.tree->topLevelItem(i)->text(0);
        check(at >= 0 && ed.kind() == menus::VIEWER && tops.value(0).startsWith("Next") &&
                  tops.filter("Rotate and Edit").size() == 1,
              "Preferences → Menus offers the image viewer's menu to lay out (" + tops.mid(0, 3).join(", ") + ")");
    }

    // closing with an unsaved rotation asks first
    v->go_to(0);
    v->resize(700, 500);   // the offscreen screen is 800 × 600
    v->canvas->rotate(90);
    questions.clear();
    v->close();
    QWidget *cbox = questions.value(0);
    bool still_open = v && v->isVisible();
    if (QAbstractButton *cancel = button(cbox, "Cancel"))
        cancel->click();
    check(still_open && v && v->isVisible(), "closing with an unsaved rotation asks first; Cancel keeps the viewer open");
    questions.clear();
    v->close();
    QAbstractButton *dont = button(questions.value(0), "Don't Save");
    if (dont)
        dont->click();
    bool gone = wait_for([&]() { return !v; });
    int tag = metadata::orientation_tag(a).value;
    check(dont && gone && tag == 1, QString("Don't Save closes it without saving (asked: %1, closed: %2, tag: %3)")
                                        .arg(dont ? "yes" : "no", gone ? "yes" : "no", QString::number(tag)));

    // remembered state
    QPointer<ImageViewer> v2 = open({a, d}, 0);
    check(v2->size() == QSize(700, 500) && v2->filmstrip->isHidden() && !v2->info->isHidden(),
          QString("the next viewer opens at the size the last one was left at, with its filmstrip and info panel as they "
                  "were (%1×%2, filmstrip %3, info %4)")
              .arg(v2->width()).arg(v2->height()).arg(!v2->filmstrip->isHidden()).arg(!v2->info->isHidden()));
    v2->toggle_filmstrip();
    v2->toggle_info();
    v2->close();
    wait_for([&]() { return !v2; });

    // the selection's order, from a window
    settings().setValue("image_opener", "builtin");
    MainWindow *w = open_window({vdir});
    wait_for([&]() { return w->pane()->all_paths().contains(d); });
    w->open_paths(w->pane(), {d, a}, false);
    ImageViewer *v3 = shown_window<ImageViewer>();
    check(v3 && v3->paths == QStringList({a, d}), "several images selected open in the order the folder shows them, not the order they were picked in");
    if (v3) {
        v3->go_to(1);
        v3->delete_current();
        check(!lexists(d) && undo::label() == "Move to Trash", "the viewer's Move to Trash can be undone with Ctrl+Z");
        v3->undo();
        check(wait_for([&]() { return lexists(d) && v3->paths.contains(d) && v3->current() == d; }),
              "...and Ctrl+Z in the viewer puts it back where it was");
        v3->close();
    }
    finish();
}
