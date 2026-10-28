// The desktop's theme: colours derived from its palette, and following a change (a light/dark switch, another theme);
// and the desktop's own settings (Cinnamon's on Linux Mint).
#include "common.h"

#include <QComboBox>
#include <QLabel>
#include <QMenu>
#include <QSplitter>
#include <QTreeWidget>
#include <QStatusBar>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace test;

// Stand-ins for Cinnamon's settings (Linux Mint), compiled before GLib first reads the schemas. False if the schema
// compiler is missing.
static const char *CINNAMON_SCHEMAS = R"(<schemalist>
  <schema id="org.cinnamon.desktop.background" path="/org/cinnamon/desktop/background/">
    <key name="picture-uri" type="s"><default>''</default></key>
  </schema>
  <schema id="org.cinnamon.desktop.privacy" path="/org/cinnamon/desktop/privacy/">
    <key name="remember-recent-files" type="b"><default>true</default></key>
  </schema>
  <schema id="org.cinnamon.desktop.interface" path="/org/cinnamon/desktop/interface/">
    <key name="icon-theme" type="s"><default>'Mint-Y'</default></key>
  </schema>
</schemalist>
)";

static bool cinnamon_schemas()
{
    QString dir = home_path("schemas");
    makedirs(dir, true);
    write_text(join(dir, "org.cinnamon.test.gschema.xml"), CINNAMON_SCHEMAS);
    QString compiler = "/usr/lib/x86_64-linux-gnu/glib-2.0/glib-compile-schemas";
    if (!isfile(compiler))
        compiler = which_path("glib-compile-schemas");
    if (compiler.isEmpty() || std::system(QString("'%1' '%2'").arg(compiler, dir).toLocal8Bit().constData()) != 0)
        return false;
    qputenv("GSETTINGS_SCHEMA_DIR", dir.toLocal8Bit());
    return true;
}

// light and dark palettes, with different highlight colours
static QPalette palette_of(const char *window, const char *highlight)
{
    QPalette p{QColor(window)};
    p.setColor(QPalette::Highlight, QColor(highlight));
    return p;
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    bool schemas = cinnamon_schemas();
    QApplication app(argc, argv);
    setup_app();

    // -- the desktop's own settings
    QByteArray desktop = qgetenv("XDG_CURRENT_DESKTOP");
    if (schemas) {
        qputenv("XDG_CURRENT_DESKTOP", "X-Cinnamon");
        check(desktop_schema("org.gnome.desktop.background") == "org.cinnamon.desktop.background" &&
                  desktop_schema("org.gnome.desktop.privacy") == "org.cinnamon.desktop.privacy" &&
                  desktop_schema("org.gnome.desktop.interface") == "org.cinnamon.desktop.interface",
              "on Cinnamon, its own settings are used (wallpaper, file history, icon theme)");
        check(has_schema_key("org.cinnamon.desktop.background", "picture-uri") &&
                  !has_schema_key("org.cinnamon.desktop.background", "picture-uri-dark"),
              "Cinnamon's wallpaper has no dark picture, so only the one is set");
        check(desktop_schema("org.gnome.desktop.a11y") == "org.gnome.desktop.a11y",
              "a setting Cinnamon has no copy of stays GNOME's");
        qputenv("XDG_CURRENT_DESKTOP", "ubuntu:GNOME");
        check(desktop_schema("org.gnome.desktop.background") == "org.gnome.desktop.background",
              "other desktops use GNOME's settings");
    } else {
        skip("the desktop's own settings (no glib-compile-schemas)");
    }
    qputenv("XDG_CURRENT_DESKTOP", desktop);

    // -- the GTK theme's colours (Qt before 6.5)
    check(needs_gtk_palette("6.4.2") && needs_gtk_palette("6.4.0") && !needs_gtk_palette("6.5.0") &&
              !needs_gtk_palette("6.10.2"),
          "Qt before 6.5 needs Kestrel to read the GTK theme's colours; 6.5 and newer do it themselves");
    bool ok = false;
    auto gp_colors = []() {
        return QJsonObject{{"theme_bg_color", "#2b2b2b"}, {"theme_fg_color", "#dadada"}, {"theme_base_color", "#323232"},
                           {"theme_text_color", "#ffffff"}, {"theme_selected_bg_color", "#35a854"},
                           {"theme_selected_fg_color", "#ffffff"}, {"insensitive_fg_color", "#888888"}};
    };
    QPalette gp = gtk_palette_from(gp_colors(), &ok);
    check(ok && gp.color(QPalette::Window) == QColor("#2b2b2b") && gp.color(QPalette::WindowText) == QColor("#dadada") &&
              gp.color(QPalette::Base) == QColor("#323232") && gp.color(QPalette::Text) == QColor("#ffffff") &&
              gp.color(QPalette::Highlight) == QColor("#35a854") &&
              gp.color(QPalette::Disabled, QPalette::Text) == QColor("#888888"),
          "a GTK theme's colours become the palette (window, text, selection, disabled text)");
    gtk_palette_from(QJsonObject{{"theme_fg_color", "#dadada"}}, &ok);
    check(!ok, "a theme without the basic colours changes nothing");
    QJsonObject yaru{{"theme_bg_color", "#fafafa"}, {"theme_fg_color", "#3d3d3d"}, {"theme_selected_bg_color", "#e95420"}};
    QPalette lp = theme_palette(yaru, false, &ok);
    check(ok && lp.color(QPalette::Window) == QColor("#fafafa"), "with the light style, a light theme's colours are used");
    QPalette dp = theme_palette(yaru, true, &ok);
    check(ok && dp.color(QPalette::Window).lightness() < 128 && dp.color(QPalette::Text).lightness() > 200 &&
              dp.color(QPalette::WindowText).lightness() > 200,
          "with the dark style, a theme that stays light gives a dark background and light text");
    QPalette none = theme_palette(QJsonObject(), true, &ok);
    check(ok && none.color(QPalette::Window).lightness() < 128,
          "with the dark style and no theme colours (no python3 or GTK), the background is still dark");
    check(theme_palette(gp_colors(), true, &ok).color(QPalette::Window) == QColor("#2b2b2b"),
          "with the dark style, a dark theme's own colours are kept");

    const QPalette LIGHT = palette_of("#fafafa", "#e95420"), DARK = palette_of("#2a2a2a", "#2a7fd4");
    QApplication::setPalette(LIGHT);
    MainWindow *w = open_window({HOME()});
    w->navigate(OVERVIEW);
    spin(300);

    // -- colours from the palette
    bool light_ok = !dark_theme();
    QColor light_card = card_color(), light_error = error_color();
    QApplication::setPalette(DARK);
    check(light_ok && dark_theme(), "a light palette is recognised as light and a dark one as dark");
    check(light_card != LIGHT.color(QPalette::Window) && card_color() != DARK.color(QPalette::Window),
          "cards stand out from the window background in light and dark themes");
    check(error_color().lightness() > light_error.lightness(), "error text is a lighter red on a dark background");
    QApplication::setPalette(LIGHT);
    spin(100);

    // -- following a change
    int calls = 0;
    auto *owner = new QObject;
    on_palette_change(owner, [&]() { ++calls; });
    auto *label = new QLabel("x");
    label->setStyleSheet("QLabel { color: palette(highlight); }");
    label->show();
    spin(100);
    QPalette dark2 = DARK;   // a theme switch can change the palette more than once
    dark2.setColor(QPalette::Link, QColor("#2a7fd4"));
    QApplication::setPalette(DARK);
    QApplication::setPalette(dark2);
    spin(300);
    check(calls == 1, "a palette change runs the registered callbacks once");
    check(label->palette().color(QPalette::WindowText) == QColor("#2a7fd4"),
          "stylesheets that use palette() take the new colours");
    QColor header = QColor();
    for (int i = 0; i < w->sidebar->count(); ++i)
        if (w->sidebar->item(i)->data(Qt::UserRole).toList().value(0) == "header")
            header = w->sidebar->item(i)->foreground().color();
    check(header == DARK.color(QPalette::PlaceholderText), "the sidebar's headers take the new colours");
    QList<Card *> cards = w->findChildren<Card *>();
    check(!cards.isEmpty() && cards.first()->styleSheet().contains(card_color().name()),
          "the Overview's cards take the new colours");
    check(g_thumbs->folder_color == "#2a7fd4" && g_thumbs->color_for(home_path("any")) == "#2a7fd4",
          "the default folder colour follows the accent, also in folder previews");
    QString empty_dir = home_path("no-images");
    makedirs(empty_dir, true);
    qint64 empty_mtime = QFileInfo(empty_dir).lastModified().toSecsSinceEpoch();
    QImage plain = g_thumbs->plain_folder(g_thumbs->folder_color, 128).toImage();
    bool filled = wait_for([&]() { return g_thumbs->folder_pixmap(empty_dir, empty_mtime, 128).toImage() == plain; });
    check(filled && g_thumbs->folder_pixmap(empty_dir, empty_mtime, 128, false).isNull(),
          "a folder without images is drawn in the default folder colour like the others (the theme's icon only with "
          "folder previews off)");
    // a special folder (Downloads, Home…) without images shows its symbol on the plain folder, in a shade of the
    // folder's colour that stands out from it, whatever the colour
    setup_icon_theme();
    if (QIcon::hasThemeIcon("folder-download-symbolic") && QIcon::hasThemeIcon("user-home-symbolic")) {
        auto shows_symbol = [](const QString &color) {
            QImage plain = g_thumbs->plain_folder(color, 128).toImage();
            QImage sym = g_thumbs->plain_folder(color, 128, "folder-download").toImage();
            int front = QColor(plain.pixel(64, 80)).lightness(), stands_out = 0;
            for (int y = 0; y < sym.height(); ++y)
                for (int x = 0; x < sym.width(); ++x)
                    if (sym.pixel(x, y) != plain.pixel(x, y) && std::abs(QColor(sym.pixel(x, y)).lightness() - front) > 70)
                        stands_out += 1;
            return stands_out > 100;
        };
        QStringList faint;
        for (const QString &c : {QString("#3584e4"), QString("#f6d32d"), QString("#ffffff"), QString("#000000")})
            if (!shows_symbol(c))
                faint << c;
        check(faint.isEmpty(), "a special folder's symbol stands out on a plain folder of any colour (light, dark, "
                               "white, black)" + (faint.isEmpty() ? QString() : " (not on " + faint.join(", ") + ")"));
        QString home = HOME();
        qint64 home_mtime = QFileInfo(home).lastModified().toSecsSinceEpoch();
        QImage want = g_thumbs->plain_folder(g_thumbs->color_for(home), 128, "user-home").toImage();
        bool symbol = wait_for([&]() { return g_thumbs->folder_pixmap(home, home_mtime, 128).toImage() == want; }) &&
                      want != g_thumbs->plain_folder(g_thumbs->color_for(home), 128).toImage();
        check(symbol, "a special folder without images (the home folder) is drawn with its symbol");
    } else {
        skip("special folder symbols (the icon theme has no folder-download-symbolic)");
    }
    // a bookmark that isn't there (a drive not connected, a folder that's gone) is a faded folder in the default
    // colour, a network one a plain folder in it: not the theme's folder icon
    {
        auto card_pic = [](const QString &target) {
            BookmarkCard card(target, "x", g_thumbs);
            QList<QLabel *> labels = card.findChildren<QLabel *>();
            return labels.isEmpty() ? QImage() : labels.first()->pixmap().toImage();
        };
        QImage drive = card_pic("/media/kes-test-unplugged/folder"), gone = card_pic(home_path("gone")),
               net = card_pic("smb://nas/share");
        // a point on the front panel, beside the symbol
        int fh = QColor(g_thumbs->folder_color).lighter(118).hsvHue();
        auto colored = [&](const QImage &img, bool faded) {
            if (img.isNull())
                return false;
            QColor c = img.pixelColor(img.width() / 5, img.height() * 3 / 4);
            return std::abs(c.hsvHue() - fh) <= 6 && (faded ? c.alpha() > 60 && c.alpha() < 160 : c.alpha() == 255);
        };
        check(colored(drive, true) && colored(gone, true) && colored(net, false),
              "a bookmark that isn't there (a drive not connected, a folder that's gone) is a faded folder in the default "
              "colour, a network one a folder in it");
    }
    // -- which pictures a folder preview shows (Preferences → Folder preview picks)
    {
        QString pics = home_path("picks"), nested = home_path("picks-nested");
        makedirs(pics, true);
        const int when[] = {5, 9, 1, 7, 3, 8, 2, 6, 4};   // p1.jpg's age, p2.jpg's…: not in name order
        for (int i = 1; i <= 9; ++i) {
            QString f = join(pics, QString("p%1.jpg").arg(i));
            write_text(f, "x");
            struct timespec t[2] = {{1700000000 + when[i - 1] * 100, 0}, {1700000000 + when[i - 1] * 100, 0}};
            utimensat(AT_FDCWD, f.toLocal8Bit().constData(), t, 0);
        }
        for (const char *d : {"a", "b", "c", "d", "e"}) {
            makedirs(join(nested, d), true);
            write_text(join(join(nested, d), QString(d) + ".png"), "x");
        }
        auto names = [](const QStringList &paths) {
            QStringList out;
            for (const QString &p : paths)
                out << basename(p);
            return out.join(' ');
        };
        auto picks = [&](int count, const QString &order) { return names(thumbs::pick_folder_images(pics, count, order)); };
        check(picks(3, "name") == "p1.jpg p2.jpg p3.jpg", "first by name: " + picks(3, "name"));
        check(picks(3, "last") == "p9.jpg p8.jpg p7.jpg", "last by name: " + picks(3, "last"));
        check(picks(3, "newest") == "p2.jpg p6.jpg p4.jpg", "newest first: " + picks(3, "newest"));
        check(picks(3, "oldest") == "p3.jpg p7.jpg p5.jpg", "oldest first: " + picks(3, "oldest"));
        check(picks(3, "spread") == "p1.jpg p5.jpg p9.jpg" && picks(4, "spread") == "p1.jpg p4.jpg p6.jpg p9.jpg" &&
                  picks(1, "spread") == "p1.jpg",
              "beginning, middle and end: evenly spaced by name (" + picks(3, "spread") + " / " + picks(4, "spread") + ")");
        check(names(thumbs::pick_folder_images(pics, 3, "spread", join(pics, "p2.jpg"))) == "p2.jpg p1.jpg p9.jpg",
              "beginning, middle and end: a chosen cover comes first, the rest spread over the others");
        check(picks(2, "something-newer") == "p1.jpg p2.jpg", "an unknown setting picks first by name");
        check(names(thumbs::pick_folder_images(nested, 3, "spread")) == "a.png c.png e.png" &&
                  names(thumbs::pick_folder_images(nested, 2, "last")) == "e.png d.png",
              "a folder of galleries takes its subfolders in the same order");
    }
    check(thumbs::follows_accent("accent") && thumbs::follows_accent("") && thumbs::follows_accent("#D9652F") &&
              !thumbs::follows_accent("#33d17a"),
          "“accent”, no setting and the old fixed default follow the accent; a chosen colour doesn't");
    check(named_accent("orange") == QColor("#ed5b00") && named_accent("slate") == QColor("#6f8396") &&
              !named_accent("olive").isValid(),
          "GNOME's accent colour names give libadwaita's colours");
    QString iface = "org.gnome.desktop.interface";
    if (desktop_schema(iface) == iface && has_schema_key(iface, "accent-color")) {
        GSettings *gs = g_settings_new("org.gnome.desktop.interface");
        g_settings_set_string(gs, "accent-color", "orange");
        g_settings_sync();
        check(wait_for([]() { return g_thumbs->folder_color == "#ed5b00"; }, 3000),
              "a chosen GNOME accent colour comes before the theme's, and folders follow it when it changes");
        g_settings_reset(gs, "accent-color");
        g_settings_sync();
        check(wait_for([]() { return g_thumbs->folder_color == "#2a7fd4"; }, 3000),
              "with no GNOME accent colour chosen, folders take the theme's");
        g_object_unref(gs);
    } else {
        skip("GNOME's accent colour setting (GNOME 47+)");
    }
    settings().setValue("folder_color", "#33d17a");
    apply_thumb_settings(g_thumbs);
    int before = calls;
    delete owner;
    QApplication::setPalette(LIGHT);
    spin(300);
    check(calls == before, "a deleted owner's callback is dropped");
    check(g_thumbs->folder_color == "#33d17a", "a chosen folder colour stays when the accent changes");

    // -- Generate Folder Previews (this folder and the ones directly in it) and its recursive form make folder
    // previews only: no large thumbnail for every image, as Generate Previews Recursively makes
    {
        QString root = home_path("gen"), s1 = join(root, "s1"), s2 = join(s1, "s2");
        makedirs(s2, true);
        int n = 0;
        for (const QString &d : {root, s1, s2})
            for (int i = 0; i < 2; ++i) {   // two each, so a preview's tiles are the small (normal) size; big
                QImage im(600, 400, QImage::Format_RGB32);   // enough to be cached at the large size
                im.fill(QColor::fromHsv(n * 40, 200, 200));
                im.save(join(d, QString("p%1.png").arg(n++)), "PNG");
            }
        auto preview = [](const QString &d) {
            return exists(join(join(APP_CACHE(), "folders"), md5(d) + "-256.png"));
        };
        auto large = []() { return isdir(join(THUMB_DIR(), "large")) ? listdir(join(THUMB_DIR(), "large")).size() : 0; };
        auto build = [&](bool folders_only, int depth) {
            RecursiveBuilder b(root, 256, g_thumbs, nullptr, 3, folders_only, depth);
            b.run();
        };
        build(true, 1);
        check(preview(root) && preview(s1) && !preview(s2) && large() == 0,
              "Generate Folder Previews makes previews for the folder and the folders directly in it, not deeper, "
              "and no thumbnail for every file");
        build(true, -1);
        check(preview(s2) && large() == 0, "Generate Folder Previews Recursively makes previews for every folder below, "
                                           "and no thumbnail for every file");
        build(false, -1);
        check(large() == 6, "Generate Previews Recursively makes every file's thumbnail too");
    }
    {   // the walk doesn't wait for the previews: it's done ("scanning" over, the total known) long before they are
        QString root = home_path("gen-big");
        for (int d = 0; d < 20; ++d) {
            QString dir = join(root, QString("d%1").arg(d));
            makedirs(dir, true);
            for (int i = 0; i < 15; ++i) {
                QImage im(600, 400, QImage::Format_RGB32);
                im.fill(QColor::fromHsv((d * 15 + i) % 360, 200, 200));
                im.save(join(dir, QString("p%1.png").arg(i)), "PNG");
            }
        }
        RecursiveBuilder b(root, 256, g_thumbs);
        QMutex m;
        int done_at_scan_end = -1, total_at_scan_end = 0;
        QObject::connect(&b, &RecursiveBuilder::progress, &b, [&](int done, int total, bool scanning, const QString &) {
            QMutexLocker g(&m);
            if (!scanning && done_at_scan_end < 0)
                done_at_scan_end = done, total_at_scan_end = total;
        }, Qt::DirectConnection);
        b.run();
        check(total_at_scan_end == 321 && done_at_scan_end >= 0 && done_at_scan_end < total_at_scan_end / 2,
              QString("a preview build finishes scanning the tree before most previews are made (%1 of %2 done then)")
                  .arg(done_at_scan_end).arg(total_at_scan_end));
    }

    // -- nothing in Kestrel's own cache (~/.cache/kestrel-explorer) gets a thumbnail or folder preview: they'd only be
    // previews of its previews, written back into it and into ~/.cache/thumbnails
    {
        QString inside = join(APP_CACHE(), "thumb-test"), sub = join(inside, "sub");
        makedirs(sub, true);
        for (int i = 0; i < 2; ++i) {
            QImage im(600, 400, QImage::Format_RGB32);
            im.fill(QColor::fromHsv(i * 90, 200, 200));
            im.save(join(sub, QString("c%1.png").arg(i)), "PNG");
        }
        auto large = []() { return isdir(join(THUMB_DIR(), "large")) ? listdir(join(THUMB_DIR(), "large")).size() : 0; };
        auto preview = [](const QString &d) { return exists(join(join(APP_CACHE(), "folders"), md5(d) + "-256.png")); };
        int before = large();
        RecursiveBuilder(inside, 256, g_thumbs).run();
        RecursiveBuilder(CACHE_ROOT(), 256, g_thumbs).run();   // the walk from above it leaves it out
        check(large() == before && !preview(inside) && !preview(sub),
              "Generate Previews Recursively makes nothing inside Kestrel's own cache folder");
        QString img = join(sub, "c0.png");
        g_thumbs->get(img, QFileInfo(img).lastModified().toSecsSinceEpoch(), false, 256, QFileInfo(img).size());
        g_thumbs->folder_pixmap(sub, QFileInfo(sub).lastModified().toSecsSinceEpoch(), 256, true);
        spin(1500);
        check(large() == before && !preview(sub) &&
                  g_thumbs->get(img, QFileInfo(img).lastModified().toSecsSinceEpoch(), false, 256,
                                QFileInfo(img).size()).isNull(),
              "a file or folder inside Kestrel's own cache folder gets no thumbnail or preview");
        rmtree(inside);
    }

    // -- thumbnailing is a job per pane and folder (the Jobs window's lower list): the pane leaving the folder lowers
    // its job (what's newer comes first; back, it's first again; left long enough, it stops), Pause and Stop work,
    // it waits while another job uses its drive, and its window closing stops it, no other window's
    {
        QImage im(800, 600, QImage::Format_RGB32);
        im.fill(QColor("#3070b0"));
        QString src = home_path("own-src.png");
        im.save(src, "PNG");
        // folders of 4 images each (links to one picture: quick to make, and still read one by one)
        auto make_tree = [&](const QString &root, int n) {
            QStringList dirs;
            for (int d = 0; d < n; ++d) {
                QString dir = join(root, QString("d%1").arg(d));
                makedirs(dir, true);
                for (int i = 0; i < 4; ++i)
                    ::link(enc(src).constData(), enc(join(dir, QString("p%1.png").arg(i))).constData());
                dirs << dir;
            }
            return dirs;
        };
        int b = thumbs::bucket_for(256);
        auto made = [&](const QStringList &dirs) {
            int n = 0;
            for (const QString &d : dirs)
                n += exists(join(join(APP_CACHE(), "folders"), QString("%1-%2.png").arg(md5(d)).arg(b)));
            return n;
        };
        auto ask = [&](const QStringList &dirs, const QObject *owner) {
            for (const QString &d : dirs)
                g_thumbs->folder_pixmap(d, QFileInfo(d).lastModified().toSecsSinceEpoch(), 256, true, owner);
        };
        QString shown = home_path("own/shown");   // what the windows show: nothing of their own to preview
        makedirs(shown, true);
        MainWindow *w1 = open_window({shown}), *w2 = open_window({shown});
        spin(200);
        QStringList few = make_tree(home_path("own/few"), 3), many = make_tree(home_path("own/many"), 300);
        // the other window's first: the newest requests go first, so these come last unless w1's are dropped
        ask(few, w2->pane()->thumb_owner());
        ask(many, w1->pane()->thumb_owner());
        auto job_of = [&](const QString &folder) {
            for (const ThumbJob &j : g_thumbs->thumb_jobs())
                if (j.folder == folder)
                    return j;
            return ThumbJob();
        };
        QString many_dir = home_path("own/many"), few_dir = home_path("own/few");
        spin(300);
        ThumbJob mj = job_of(many_dir);
        check(mj.id && mj.title() == "Thumbnails: own/many" && mj.total == 300 && mj.window == w1->windowTitle() &&
                  mj.state() == "Running" && job_of(few_dir).id,
              "a pane's thumbnails are a job of their own, for its folder and window (" + mj.title() + ", " +
                  QString::number(mj.total) + ", " + mj.window + ")");
        // the Jobs window lists them below the other jobs, in a quarter of its height
        JobsWindow *jw = JobsWindow::open(w1);
        jw->refresh_thumbs();
        QString row = jw->thumb_list->topLevelItemCount() ? jw->thumb_list->topLevelItem(0)->text(0) : QString();
        QList<int> sizes = jw->lists->sizes();
        bool listed = false;
        for (int i = 0; i < jw->thumb_list->topLevelItemCount(); ++i)
            listed = listed || jw->thumb_list->topLevelItem(i)->text(0).startsWith("Thumbnails: own/many, ");
        check(listed && sizes.size() == 2 && sizes[0] > sizes[1] * 2,
              "the Jobs window lists the thumbnail jobs in a part of its own below the other jobs (" + row + ")");
        g_thumbs->pause_job(mj.id, true);
        spin(800);
        int paused_at = made(many);
        spin(800);
        bool held = made(many) <= paused_at + 2 && job_of(many_dir).state() == "Paused";
        g_thumbs->pause_job(mj.id, false);
        check(held && wait_for([&]() { return made(many) > paused_at + 2; }, 10000),
              "a paused thumbnail job makes nothing more until it's resumed");

        w1->pane()->set_path(home_path("own"));   // w1 moves on: its job is lowered, w2's comes first
        spin(100);
        int many_then = made(many);
        bool came = wait_for([&]() { return made(few) == 3; }, 20000);
        int many_when_few = made(many);
        check(came && job_of(many_dir).lowered && job_of(many_dir).state() == "Lowered (folder left)" &&
                  many_when_few <= many_then + 4,
              QString("leaving a folder lowers its thumbnail job: another window's newer ones come first (%1 more of the "
                      "lowered job's made meanwhile)").arg(many_when_few - many_then));
        check(wait_for([&]() { return made(many) > many_when_few + 2; }, 10000),
              "...and the lowered job goes on once nothing newer waits");
        ask(many, w1->pane()->thumb_owner());   // back in it (as its view draws it again)
        check(!job_of(many_dir).lowered && job_of(many_dir).state() == "Running", "going back to the folder raises it again");

        // another job on its drive: it waits, and goes on after
        auto hold = std::make_shared<std::atomic<bool>>(true);
        QPointer<Task> holder = fileops::queue_job(w1, "Holding the drive", {many_dir}, [hold](Task *t) {
            while (*hold) {
                t->check();
                QThread::msleep(20);
            }
            return QVariant();
        });
        bool waits = wait_for([&]() { return job_of(many_dir).state() == "Waiting for the drive"; }, 5000);
        spin(500);
        int busy_at = made(many);
        spin(800);
        bool idle = made(many) <= busy_at + 2;
        *hold = false;
        wait_for([&]() { return !holder; });
        check(waits && idle && wait_for([&]() { return made(many) > busy_at + 2; }, 10000),
              "a thumbnail job waits while another job uses its drive, and goes on after");

        // left behind long enough, it stops (what it made stays)
        w1->pane()->set_path(home_path("own"));
        g_thumbs->job_minutes = 0;
        bool timed_out = wait_for([&]() { return !job_of(many_dir).id; }, 6000);
        g_thumbs->job_minutes = 3;
        int stopped_at = made(many);
        spin(800);
        check(timed_out && made(many) <= stopped_at + 2 && stopped_at > 0,
              "a lowered thumbnail job stops after the time set in Preferences, keeping what it made");
        // Stop: nothing more, and the view drawing the folder again doesn't start it again
        QStringList many3 = make_tree(home_path("own/many3"), 300);
        QString many3_dir = home_path("own/many3");
        ask(many3, w1->pane()->thumb_owner());
        spin(200);
        g_thumbs->stop_job(job_of(many3_dir).id);
        spin(300);
        int stop_at = made(many3);
        ask(many3, w1->pane()->thumb_owner());
        spin(800);
        check(stop_at < 250 && !job_of(many3_dir).id && made(many3) <= stop_at + 2,
              QString("Stop ends a thumbnail job: the view drawing the folder again doesn't start it again (%1 made at "
                      "the stop, %2 now)").arg(stop_at).arg(made(many3)));

        // the thumbnailing modes (thumb_mode, under the thumbnail list and in Preferences), with a job holding the
        // drive: paused, nothing is made; "wait", it waits for the drive; "drive", it runs beside that job, and a
        // second folder's job on the drive waits for the first
        {
            QStringList m1 = make_tree(home_path("own/mode1"), 300), m2 = make_tree(home_path("own/mode2"), 300);
            QString m1_dir = home_path("own/mode1"), m2_dir = home_path("own/mode2");
            auto hold2 = std::make_shared<std::atomic<bool>>(true);
            QPointer<Task> holder2 = fileops::queue_job(w1, "Holding the drive", {m1_dir}, [hold2](Task *t) {
                while (*hold2) {
                    t->check();
                    QThread::msleep(20);
                }
                return QVariant();
            });
            wait_for([&]() { return jobs::Queue::instance()->busy(m1_dir); }, 5000);
            prefs::set("thumb_mode", "paused");
            bool shown = jw->thumb_mode->currentData().toString() == "paused";
            ask(m1, w1->pane()->thumb_owner());
            spin(1000);
            check(shown && made(m1) == 0 && job_of(m1_dir).state() == "Paused (all thumbnailing)",
                  "thumbnailing paused: a thumbnail job makes nothing and is listed as paused (" +
                      job_of(m1_dir).state() + ", " + QString::number(made(m1)) + " made)");
            prefs::set("thumb_mode", "wait");
            spin(1000);
            check(made(m1) == 0 && job_of(m1_dir).state() == "Waiting for the drive",
                  "thumbnailing waits for the drive: a job on a drive another job uses waits (" + job_of(m1_dir).state() + ")");
            prefs::set("thumb_mode", "drive");
            check(wait_for([&]() { return made(m1) > 2; }, 10000) && job_of(m1_dir).state() == "Running" &&
                      jw->thumb_mode->currentData().toString() == "drive",
                  "one thumbnail job per drive: it runs beside the job using its drive");
            ask(m2, w2->pane()->thumb_owner());
            spin(800);
            bool behind = job_of(m2_dir).state() == "Waiting for another thumbnail job on its drive" && made(m2) == 0 &&
                          job_of(m1_dir).id;
            g_thumbs->stop_job(job_of(m1_dir).id);
            check(behind && wait_for([&]() { return made(m2) > 2; }, 10000),
                  "...and a second folder's thumbnail job on that drive waits for the first (" + job_of(m2_dir).state() + ")");
            g_thumbs->stop_job(job_of(m2_dir).id);
            prefs::set("thumb_mode", QVariant());
            *hold2 = false;
            wait_for([&]() { return !holder2; });
        }
        jw->close();

        QStringList few2 = make_tree(home_path("own/few2"), 3), many2 = make_tree(home_path("own/many2"), 300);
        ask(few2, w2->pane()->thumb_owner());
        ask(many2, w1->pane()->thumb_owner());
        int queued = g_thumbs->pending_count();
        w1->close();
        int at_close = made(many2);
        int left = g_thumbs->pending_count();
        check(queued > 250 && at_close < 100 && left <= 3 + 2,
              QString("closing a window cancels the folder previews it asked for (%1 queued, %2 made, %3 left)")
                  .arg(queued).arg(at_close).arg(left));
        check(wait_for([&]() { return made(few2) == 3; }, 20000), "another window's folder previews still come");
        spin(500);
        check(made(many2) <= at_close + 2,
              QString("nothing more is made for the closed window (%1 at the close, %2 now)").arg(at_close).arg(made(many2)));
        w2->close();
        spin(100);
    }


    // -- each Generate / Regenerate is a job of its own; Regenerate makes previews again where Generate keeps them
    {
        QString root = home_path("regen"), s1 = join(root, "s1"), s2 = join(s1, "s2");
        makedirs(s2, true);
        int n = 0;
        for (const QString &d : {root, s1, s2})
            for (int i = 0; i < 2; ++i) {
                QImage im(600, 400, QImage::Format_RGB32);
                im.fill(QColor::fromHsv(n * 40, 200, 200));
                im.save(join(d, QString("q%1.png").arg(n++)), "PNG");
            }
        int size = w->pane() ? w->pane()->grid_size : 160;
        int b = thumbs::bucket_for(size);
        auto folder_cache = [&](const QString &d) { return join(join(APP_CACHE(), "folders"), QString("%1-%2.png").arg(md5(d)).arg(b)); };
        QString flavor = b <= 128 ? "normal" : b <= 256 ? "large" : b <= 512 ? "x-large" : "xx-large";
        auto file_cache = [&](const QString &f) { return join(join(THUMB_DIR(), flavor), md5(file_uri(f)) + ".png"); };
        // stands in for an old or wrong preview: a grey picture with the same tags, so only remaking it replaces it
        auto tamper = [](const QString &cache) {
            QImage old(cache);
            if (old.isNull())
                return false;
            QImage fake(old.size(), QImage::Format_ARGB32);
            fake.fill(Qt::gray);
            for (const QString &k : old.textKeys())
                fake.setText(k, old.text(k));
            return fake.save(cache, "PNG");
        };
        auto tampered = [](const QString &cache) {
            QImage im(cache);
            return !im.isNull() && im.pixelColor(im.width() / 2, im.height() / 2) == QColor(Qt::gray);
        };
        // a job's Task is deleted once it ends
        auto run = [&](QPointer<Task> t) { return wait_for([&]() { return !t || t->isFinished(); }, 60000); };

        // two builds at once: both queued as jobs (the second isn't refused), both in the status bar's task list
        QPointer<Task> t1 = w->build_previews(root, false, -1);
        QPointer<Task> t2 = w->build_previews(home_path("gen"), true, -1);
        bool both = t1 && t2 && t1 != t2 && w->task_panel->tasks.contains(t1.data()) &&
                    w->task_panel->tasks.contains(t2.data()) && t1->title == "Generating previews recursively: regen";
        check(both && run(t1) && run(t2), "each preview build is a job of its own: two at once are both queued, shown "
                                          "in the status bar, and run");
        QString f_root = join(root, "q0.png"), f_s1 = join(s1, "q2.png");
        bool made = exists(folder_cache(root)) && exists(folder_cache(s1)) && exists(file_cache(f_root)) &&
                    exists(file_cache(f_s1));
        bool marked = made && tamper(folder_cache(root)) && tamper(folder_cache(s1)) && tamper(folder_cache(s2)) &&
                      tamper(file_cache(f_root)) && tamper(file_cache(f_s1));
        run(w->build_previews(root, false, -1));
        check(marked && tampered(folder_cache(root)) && tampered(file_cache(f_root)),
              "Generate Previews keeps the previews already made");
        run(w->build_previews(root, false, 1, true));
        check(!tampered(folder_cache(root)) && !tampered(folder_cache(s1)) && !tampered(file_cache(f_root)) &&
                  tampered(file_cache(f_s1)) && tampered(folder_cache(s2)),
              "Regenerate Previews makes this folder's file previews and its and its folders' previews again, and "
              "nothing deeper");
        run(w->build_previews(root, true, -1, true));
        check(!tampered(folder_cache(s2)) && tampered(file_cache(f_s1)),
              "Regenerate Folder Previews Recursively makes every folder's preview below again, not every file's");
        tamper(folder_cache(root));
        tamper(file_cache(f_root));
        run(w->build_previews(root, true, 1, true));
        check(!tampered(folder_cache(root)) && (b == 128 || tampered(file_cache(f_root))),
              "Regenerate Folder Previews makes this folder's preview again, not its files' (only the few its preview "
              "shows, at their small size)");
        run(w->build_previews(root, false, -1, true));
        check(!tampered(file_cache(f_s1)), "Regenerate Previews Recursively makes every file's preview again");

        // ✕: the job stops
        QString big = home_path("gen-big");
        w->statusBar()->clearMessage();
        QPointer<Task> t3 = w->build_previews(big, false, -1, true);
        // after the walk (the previews still being made): the workers stop too, not just the walk
        bool started = wait_for([&]() { return t3 && t3->isRunning() && t3->text.contains(" of ") && !t3->text.contains("scanning"); }, 30000);
        if (t3)
            t3->cancel();
        bool ended = run(t3);
        QString said = w->statusBar()->currentMessage();
        check(started && ended && said.startsWith("Stopped regenerating previews recursively in gen-big"),
              "✕ on a preview job stops it, also once its folders are all found (" + said + ")");

        // the menu
        QMenu *m = w->build_menu(w->pane(), {root});
        QStringList entries;
        for (QAction *a : m->actions())
            if (a->menu() && a->text() == "Previews")
                for (QAction *x : a->menu()->actions())
                    if (!x->isSeparator())
                        entries << x->text();
        delete m;
        check(entries == QStringList({"Generate Previews Recursively", "Generate Folder Previews",
                                      "Generate Folder Previews Recursively", "Regenerate Previews",
                                      "Regenerate Previews Recursively", "Regenerate Folder Previews",
                                      "Regenerate Folder Previews Recursively"}),
              "a folder's menu has a Previews submenu to generate and regenerate them, here or recursively");
    }
    finish();
}
