// The system's file chooser (chooser.h): the portal backend answers Open and Save requests with Kestrel windows.
#include "common.h"

#include <QMessageBox>
#include <QPushButton>

using namespace test;

static const char *NAME = "org.freedesktop.impl.portal.desktop.kestrel";

struct Reply {
    bool got = false;
    guint32 response = 99;
    QStringList uris;
    QString filter;
};

static GDBusConnection *client()   // a second connection: the portal calling the backend
{
    static GDBusConnection *conn = nullptr;
    if (!conn) {
        gchar *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
        conn = g_dbus_connection_new_for_address_sync(
            addr, GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, nullptr, nullptr);
        g_free(addr);
    }
    return conn;
}

static void call(const char *method, GVariant *options, Reply *r, const char *handle = "/org/freedesktop/portal/desktop/request/1_1/t")
{
    *r = Reply();
    g_dbus_connection_call(
        client(), NAME, "/org/freedesktop/portal/desktop", "org.freedesktop.impl.portal.FileChooser", method,
        g_variant_new("(osss@a{sv})", handle, "com.example.App", "", "Test Dialog", options), G_VARIANT_TYPE("(ua{sv})"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
            auto *r = static_cast<Reply *>(data);
            GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
            r->got = true;
            if (!v)
                return;
            GVariant *results = nullptr;
            g_variant_get(v, "(u@a{sv})", &r->response, &results);
            const gchar **uris = nullptr;
            if (g_variant_lookup(results, "uris", "^a&s", &uris)) {
                for (const gchar **u = uris; *u; ++u)
                    r->uris << QString::fromUtf8(*u);
                g_free(uris);
            }
            const gchar *fname = nullptr;
            GVariant *f = g_variant_lookup_value(results, "current_filter", G_VARIANT_TYPE("(sa(us))"));
            if (f) {
                g_variant_get_child(f, 0, "&s", &fname);
                r->filter = QString::fromUtf8(fname);
                g_variant_unref(f);
            }
            g_variant_unref(results);
            g_variant_unref(v);
        },
        r);
}

static GVariant *options(std::initializer_list<std::pair<const char *, GVariant *>> items)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    for (const auto &[k, v] : items)
        g_variant_builder_add(&b, "{sv}", k, v);
    return g_variant_builder_end(&b);
}

static GVariant *folder(const QString &p) { return g_variant_new_bytestring(p.toLocal8Bit().constData()); }

static GVariant *png_filter()   // [("Images", [(1, "image/png")]), ("Everything", [(0, "*")])]
{
    return g_variant_new_parsed("[('Images', [(uint32 1, 'image/png')]), ('Everything', [(uint32 0, '*')])]");
}

static MainWindow *chooser_window()   // the open chooser window, if any
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *m = qobject_cast<MainWindow *>(w); m && m->chooser && m->isVisible())
            return m;
    return nullptr;
}

static MainWindow *wait_window()
{
    MainWindow *w = nullptr;
    wait_for([&]() { return (w = chooser_window()) != nullptr; });
    return w;
}

static QString answer_box = "Cancel";   // how to answer a question box (Replace / Cancel)
static bool box_seen = false;

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    setup_app();
    QString dir = home_path("pics");
    makedirs(join(dir, "sub"), true);
    write_text(join(dir, "a.png"), "x");
    write_text(join(dir, "b.png"), "x");
    write_text(join(dir, "c.txt"), "x");
    chooser::serve([](const chooser::Request &req, std::function<void(const chooser::Result &)> done) {
        QPointer<MainWindow> w = open_chooser(req, std::move(done));
        return std::function<void()>([w]() {
            if (w)
                w->close();
        });
    });
    QTimer boxes;   // question boxes are modal: answer them
    QObject::connect(&boxes, &QTimer::timeout, []() {
        if (auto *m = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            box_seen = true;
            for (QAbstractButton *b : m->buttons())
                if (b->text() == answer_box)
                    b->click();
        }
    });
    boxes.start(50);
    spin(500);   // own the name
    Reply r;

    // -- the request
    chooser::Request req = chooser::parse("OpenFile", "t", options({{"filters", png_filter()}}));
    check(req.filters.size() == 2 && req.filters[0].globs.contains("*.png"), "a MIME type filter becomes its file patterns");
    chooser::Request any = chooser::parse("OpenFile", "t", options({{"filters", g_variant_new_parsed("[('Pictures', [(uint32 1, 'image/*')])]")}}));
    check(any.filters.value(0).globs.contains("*.jpg") && any.filters.value(0).globs.contains("*.png"),
          "image/* stands for every image type");
    req.accept_label = "_Save";
    check(chooser::button_text(req) == "Save", "a GTK mnemonic is dropped from the button label");
    check(chooser::x11_parent("x11:1a2b") == 0x1a2b && chooser::x11_parent("wayland:abc") == 0 &&
              chooser::x11_parent("") == 0 && chooser::x11_parent("x11:zz") == 0,
          "an X11 app's window id is read from the portal's handle (the chooser becomes its dialog)");

    // -- saving
    call("SaveFile", options({{"current_folder", folder(dir)}, {"current_name", g_variant_new_string("new.png")}}), &r);
    MainWindow *w = wait_window();
    check(w && w->cur_dir() == dir && w->chooser->name->text() == "new.png",
          "Save opens a Kestrel window in the requested folder with the suggested name");
    check(w && w->windowTitle() == "Test Dialog", "the window has the app's title");
    if (w)
        w->chooser->accept();
    check(wait_for([&]() { return r.got; }) && r.response == 0 && r.uris == QStringList{file_uri(join(dir, "new.png"))},
          "the chosen file goes back to the app as a file:// URI");
    check(wait_for([]() { return chooser_window() == nullptr; }), "the window closes after choosing");

    call("SaveFile", options({{"current_folder", folder(dir)}, {"current_name", g_variant_new_string("a.png")}}), &r);
    w = wait_window();
    answer_box = "Cancel";
    box_seen = false;
    if (w)
        w->chooser->accept();
    spin(300);
    check(box_seen && !r.got && chooser_window(), "saving over a file asks first, and Cancel keeps the dialog open");
    answer_box = "Replace";
    if (w)
        w->chooser->accept();
    check(wait_for([&]() { return r.got; }) && r.response == 0 && r.uris == QStringList{file_uri(join(dir, "a.png"))},
          "...and Replace saves over it");

    call("SaveFile", options({{"current_folder", folder(dir)}}), &r);
    w = wait_window();
    if (w) {
        w->open_paths(w->pane(), {join(dir, "sub")});
        spin(200);
    }
    check(w && w->cur_dir() == join(dir, "sub"), "double-clicking a folder goes into it");
    if (w) {
        w->chooser->name->setText("x.png");
        w->chooser->accept();
    }
    check(wait_for([&]() { return r.got; }) && r.uris == QStringList{file_uri(join(dir, "sub/x.png"))},
          "...and the file is saved there");
    check(settings().value("chooser_folder").toString() == join(dir, "sub"), "the next chooser starts where the last one saved");

    // -- opening
    call("OpenFile", options({{"current_folder", folder(dir)}, {"filters", png_filter()}}), &r);
    w = wait_window();
    QStringList shown;
    wait_for([&]() { return w && (shown = w->pane()->all_paths()).size() == 3; });
    shown.sort();
    check(shown == QStringList({join(dir, "a.png"), join(dir, "b.png"), join(dir, "sub")}),
          "only files of the chosen type are shown, and folders stay");
    if (w)
        w->open_paths(w->pane(), {join(dir, "b.png")});
    check(wait_for([&]() { return r.got; }) && r.response == 0 && r.uris == QStringList{file_uri(join(dir, "b.png"))} &&
              r.filter == "Images",
          "double-clicking a file chooses it, and the chosen type goes back too");

    call("OpenFile", options({{"current_folder", folder(dir)}, {"multiple", g_variant_new_boolean(TRUE)}}), &r);
    w = wait_window();
    wait_for([&]() { return w && w->pane()->all_paths().size() == 4; });
    if (w) {
        w->pane()->select_paths({join(dir, "a.png"), join(dir, "c.txt")});
        w->chooser->accept();
    }
    check(wait_for([&]() { return r.got; }) && r.uris.size() == 2, "with multiple, every selected file is chosen");

    call("OpenFile", options({{"current_folder", folder(dir)}, {"directory", g_variant_new_boolean(TRUE)}}), &r);
    w = wait_window();
    bool select = false;
    if (w)
        for (QPushButton *b : w->chooser->findChildren<QPushButton *>())
            select = select || b->text() == "Select";
    check(select, "a folder chooser's button says Select");
    if (w)
        w->chooser->accept();
    check(wait_for([&]() { return r.got; }) && r.uris == QStringList{file_uri(dir)}, "choosing a folder: the current folder");

    call("SaveFiles", options({{"current_folder", folder(dir)},
                               {"files", g_variant_new_parsed("[b'one.txt', b'two.txt']")}}), &r);
    w = wait_window();
    if (w)
        w->chooser->accept();
    check(wait_for([&]() { return r.got; }) &&
              r.uris == QStringList({file_uri(join(dir, "one.txt")), file_uri(join(dir, "two.txt"))}),
          "saving several files: each goes into the chosen folder");

    // -- its own settings
    QWidget big;   // a main window's saved size
    big.resize(1500, 1000);
    settings().setValue("geometry", big.saveGeometry());
    settings().setValue("grid_size", 200);
    call("OpenFile", options({{"current_folder", folder(dir)}}), &r);
    w = wait_window();
    check(w && w->width() < 1500 && w->pane() && w->pane()->grid_size == 96,
          "a chooser opens smaller than a main window, with smaller icons");
    if (w) {
        w->pane()->zoom(0, 120);
        w->toggle_hidden(true);
        w->close();
    }
    wait_for([&]() { return r.got; });
    check(settings().value("grid_size").toInt() == 200 && !settings().value("show_hidden", false).toBool() &&
              settings().value("chooser/grid_size").toInt() == 120,
          "zooming and showing hidden files in a chooser leave the main windows' settings alone");
    call("OpenFile", options({{"current_folder", folder(dir)}}), &r);
    w = wait_window();
    check(w && w->pane() && w->pane()->grid_size == 120 && w->show_hidden,
          "...and the next chooser starts from its own settings");
    if (w)
        w->close();
    wait_for([&]() { return r.got; });

    // -- cancelling
    call("OpenFile", options({{"current_folder", folder(dir)}}), &r);
    w = wait_window();
    if (w)
        w->chooser->finish(false);
    check(wait_for([&]() { return r.got; }) && r.response == 1 && r.uris.isEmpty(), "Cancel answers “cancelled”");
    call("OpenFile", options({{"current_folder", folder(dir)}}), &r);
    w = wait_window();
    if (w)
        w->close();
    check(wait_for([&]() { return r.got; }) && r.response == 1, "closing the window answers “cancelled”");
    const char *handle = "/org/freedesktop/portal/desktop/request/1_1/closeme";
    call("OpenFile", options({{"current_folder", folder(dir)}}), &r, handle);
    wait_window();
    g_dbus_connection_call(client(), NAME, handle, "org.freedesktop.impl.portal.Request", "Close", nullptr, nullptr,
                           G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr, nullptr);   // not call_sync: this thread answers it
    check(wait_for([&]() { return r.got; }) && r.response == 1 && wait_for([]() { return chooser_window() == nullptr; }),
          "the app can close the dialog (Request.Close)");

    finish();
}
