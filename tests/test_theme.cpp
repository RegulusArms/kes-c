// The desktop's theme: colours derived from its palette, and following a change (a light/dark switch, another theme).
#include "common.h"

#include <QLabel>

using namespace test;

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
    QApplication app(argc, argv);
    setup_app();
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
