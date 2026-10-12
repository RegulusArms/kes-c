// The desktop's theme: colours derived from its palette, and following a change (a light/dark switch, another theme);
// and the desktop's own settings (Cinnamon's on Linux Mint).
#include "common.h"

#include <QLabel>

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
    QPalette gp = gtk_palette_from(
        QJsonObject{{"theme_bg_color", "#2b2b2b"}, {"theme_fg_color", "#dadada"}, {"theme_base_color", "#323232"},
                    {"theme_text_color", "#ffffff"}, {"theme_selected_bg_color", "#35a854"},
                    {"theme_selected_fg_color", "#ffffff"}, {"insensitive_fg_color", "#888888"}},
        &ok);
    check(ok && gp.color(QPalette::Window) == QColor("#2b2b2b") && gp.color(QPalette::WindowText) == QColor("#dadada") &&
              gp.color(QPalette::Base) == QColor("#323232") && gp.color(QPalette::Text) == QColor("#ffffff") &&
              gp.color(QPalette::Highlight) == QColor("#35a854") &&
              gp.color(QPalette::Disabled, QPalette::Text) == QColor("#888888"),
          "a GTK theme's colours become the palette (window, text, selection, disabled text)");
    gtk_palette_from(QJsonObject{{"theme_fg_color", "#dadada"}}, &ok);
    check(!ok, "a theme without the basic colours changes nothing");

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
            b.start();
            b.wait();
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
        b.start();
        b.wait();
        check(total_at_scan_end == 321 && done_at_scan_end >= 0 && done_at_scan_end < total_at_scan_end / 2,
              QString("a preview build finishes scanning the tree before most previews are made (%1 of %2 done then)")
                  .arg(done_at_scan_end).arg(total_at_scan_end));
    }

    finish();
}
