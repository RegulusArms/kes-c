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

    const QPalette LIGHT = palette_of("#fafafa", "#e95420"), DARK = palette_of("#2a2a2a", "#3584e4");
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
    dark2.setColor(QPalette::Link, QColor("#3584e4"));
    QApplication::setPalette(DARK);
    QApplication::setPalette(dark2);
    spin(300);
    check(calls == 1, "a palette change runs the registered callbacks once");
    check(label->palette().color(QPalette::WindowText) == QColor("#3584e4"),
          "stylesheets that use palette() take the new colours");
    QColor header = QColor();
    for (int i = 0; i < w->sidebar->count(); ++i)
        if (w->sidebar->item(i)->data(Qt::UserRole).toList().value(0) == "header")
            header = w->sidebar->item(i)->foreground().color();
    check(header == DARK.color(QPalette::PlaceholderText), "the sidebar's headers take the new colours");
    QList<Card *> cards = w->findChildren<Card *>();
    check(!cards.isEmpty() && cards.first()->styleSheet().contains(card_color().name()),
          "the Overview's cards take the new colours");
    check(g_thumbs->folder_color == "#3584e4" && g_thumbs->color_for(home_path("any")) == "#3584e4",
          "the default folder colour follows the accent, also in folder previews");
    check(thumbs::follows_accent("accent") && thumbs::follows_accent("") && thumbs::follows_accent("#D9652F") &&
              !thumbs::follows_accent("#33d17a"),
          "“accent”, no setting and the old fixed default follow the accent; a chosen colour doesn't");
    settings().setValue("folder_color", "#33d17a");
    apply_thumb_settings(g_thumbs);
    delete owner;
    QApplication::setPalette(LIGHT);
    spin(300);
    check(calls == 1, "a deleted owner's callback is dropped");
    check(g_thumbs->folder_color == "#33d17a", "a chosen folder colour stays when the accent changes");

    finish();
}
