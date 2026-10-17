// The desktop's theme: colours derived from its palette, and following a change (a light/dark switch, another theme);
// and the desktop's own settings (Cinnamon's on Linux Mint).
#include "common.h"

#include <QLabel>
#include <QMenu>
#include <QStatusBar>

#include <fcntl.h>
#include <sys/stat.h>

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
