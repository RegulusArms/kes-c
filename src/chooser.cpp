#include "chooser.h"

#include "app.h"
#include "util.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPushButton>
#include <QTimer>

#include <gio/gio.h>

using namespace util;

namespace chooser {

static QString str_option(GVariant *options, const char *key)
{
    const gchar *s = nullptr;
    return g_variant_lookup(options, key, "&s", &s) ? QString::fromUtf8(s) : QString();
}

static QString path_option(GVariant *options, const char *key)
{
    // paths come as byte strings (ay, with a trailing NUL)
    GVariant *v = g_variant_lookup_value(options, key, G_VARIANT_TYPE_BYTESTRING);
    if (!v)
        return QString();
    QString p = QString::fromLocal8Bit(g_variant_get_bytestring(v));
    g_variant_unref(v);
    return p;
}

static QStringList mime_globs(const QString &mime)
{
    static QMimeDatabase db;
    QStringList out;
    if (mime.endsWith("/*")) {   // image/* and the like
        QString top = mime.left(mime.size() - 1);
        for (const QMimeType &t : db.allMimeTypes())
            if (t.name().startsWith(top))
                out << t.globPatterns();
    } else {
        out = db.mimeTypeForName(mime).globPatterns();
    }
    return out;
}

static Filter parse_filter(GVariant *f)   // (sa(us))
{
    Filter out;
    const gchar *name = nullptr;
    GVariantIter *iter = nullptr;
    g_variant_get(f, "(&sa(us))", &name, &iter);
    out.name = QString::fromUtf8(name);
    guint32 kind;
    const gchar *pattern;
    while (g_variant_iter_next(iter, "(u&s)", &kind, &pattern)) {
        QString p = QString::fromUtf8(pattern);
        out.patterns << qMakePair(uint(kind), p);
        out.globs << (kind == 0 ? QStringList{p} : mime_globs(p));
    }
    g_variant_iter_free(iter);
    out.globs.removeDuplicates();
    return out;
}

Request parse(const QString &method, const QString &title, GVariant *options)
{
    Request r;
    r.method = method;
    r.title = title;
    r.accept_label = str_option(options, "accept_label");
    g_variant_lookup(options, "multiple", "b", &r.multiple);
    g_variant_lookup(options, "directory", "b", &r.directory);
    r.current_name = str_option(options, "current_name");
    r.current_folder = path_option(options, "current_folder");
    QString current_file = path_option(options, "current_file");   // saving over an existing file
    if (!current_file.isEmpty()) {
        r.current_folder = dirname(current_file);
        r.current_name = basename(current_file);
    }
    if (GVariant *fs = g_variant_lookup_value(options, "filters", G_VARIANT_TYPE("a(sa(us))"))) {
        for (gsize i = 0; i < g_variant_n_children(fs); ++i) {
            GVariant *f = g_variant_get_child_value(fs, i);
            r.filters << parse_filter(f);
            g_variant_unref(f);
        }
        g_variant_unref(fs);
    }
    if (GVariant *cf = g_variant_lookup_value(options, "current_filter", G_VARIANT_TYPE("(sa(us))"))) {
        Filter cur = parse_filter(cf);
        g_variant_unref(cf);
        for (int i = 0; i < r.filters.size() && r.current_filter < 0; ++i)
            if (r.filters[i].name == cur.name && r.filters[i].patterns == cur.patterns)
                r.current_filter = i;
        if (r.current_filter < 0) {   // not one of the list: it is the only filter
            r.filters << cur;
            r.current_filter = r.filters.size() - 1;
        }
    }
    if (r.current_filter < 0 && !r.filters.isEmpty())
        r.current_filter = 0;
    if (GVariant *cs = g_variant_lookup_value(options, "choices", G_VARIANT_TYPE("a(ssa(ss)s)"))) {
        GVariantIter iter;
        g_variant_iter_init(&iter, cs);
        const gchar *id, *label, *def;
        GVariantIter *opts;
        while (g_variant_iter_next(&iter, "(&s&sa(ss)&s)", &id, &label, &opts, &def)) {
            r.choices << qMakePair(QString::fromUtf8(id), QString::fromUtf8(def));
            g_variant_iter_free(opts);
        }
        g_variant_unref(cs);
    }
    if (GVariant *files = g_variant_lookup_value(options, "files", G_VARIANT_TYPE("aay"))) {
        for (gsize i = 0; i < g_variant_n_children(files); ++i) {
            GVariant *f = g_variant_get_child_value(files, i);
            r.files << basename(QString::fromLocal8Bit(g_variant_get_bytestring(f)));
            g_variant_unref(f);
        }
        g_variant_unref(files);
    }
    return r;
}

static GVariant *filter_variant(const Filter &f)
{
    GVariantBuilder pats;
    g_variant_builder_init(&pats, G_VARIANT_TYPE("a(us)"));
    for (const auto &[kind, p] : f.patterns)
        g_variant_builder_add(&pats, "(us)", guint32(kind), p.toUtf8().constData());
    return g_variant_new("(sa(us))", f.name.toUtf8().constData(), &pats);
}

GVariant *results(const Request &req, const Result &res)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    if (res.ok) {
        GVariantBuilder uris;
        g_variant_builder_init(&uris, G_VARIANT_TYPE_STRING_ARRAY);
        for (const QString &p : res.paths)
            g_variant_builder_add(&uris, "s", file_uri(p).toUtf8().constData());
        g_variant_builder_add(&b, "{sv}", "uris", g_variant_builder_end(&uris));
        if (res.filter >= 0 && res.filter < req.filters.size())
            g_variant_builder_add(&b, "{sv}", "current_filter", filter_variant(req.filters[res.filter]));
        if (!req.choices.isEmpty()) {
            GVariantBuilder cs;
            g_variant_builder_init(&cs, G_VARIANT_TYPE("a(ss)"));
            for (const auto &[id, value] : req.choices)
                g_variant_builder_add(&cs, "(ss)", id.toUtf8().constData(), value.toUtf8().constData());
            g_variant_builder_add(&b, "{sv}", "choices", g_variant_builder_end(&cs));
        }
        if (!req.saving())
            g_variant_builder_add(&b, "{sv}", "writable", g_variant_new_boolean(TRUE));
    }
    return g_variant_builder_end(&b);
}

QString button_text(const Request &req)
{
    if (!req.accept_label.isEmpty()) {
        QString t = req.accept_label;
        return t.replace("__", "\x01").remove('_').replace('\x01', '_');   // GTK mnemonics: _Save
    }
    if (req.method == "SaveFiles" || req.directory)
        return "Select";
    return req.saving() ? "Save" : "Open";
}

// ---------------------------------------------------------------- the D-Bus service

static const char *NAME = "org.freedesktop.impl.portal.desktop.kestrel";
static const char *PATH = "/org/freedesktop/portal/desktop";
static const char *XML = R"(
<node>
  <interface name="org.freedesktop.impl.portal.FileChooser">
    <method name="OpenFile">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="SaveFile">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="SaveFiles">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
  </interface>
  <interface name="org.freedesktop.impl.portal.Request">
    <method name="Close"/>
  </interface>
</node>
)";

struct Service {
    Opener open;
    GDBusNodeInfo *node = nullptr;
    int open_dialogs = 0;
    QTimer idle;
};
static Service *service = nullptr;

struct Pending {
    GDBusMethodInvocation *invocation;
    GDBusConnection *conn;
    guint request_id = 0;   // the Request object at the handle (Close)
    Request req;
    std::function<void()> close;
    bool answered = false;
};

static void answer(Pending *p, guint32 response, const Result &res)
{
    if (p->answered)
        return;
    p->answered = true;
    g_dbus_method_invocation_return_value(p->invocation, g_variant_new("(u@a{sv})", response, results(p->req, res)));
    if (p->request_id)
        g_dbus_connection_unregister_object(p->conn, p->request_id);
    service->open_dialogs -= 1;
    if (service->open_dialogs == 0)
        service->idle.start();
}

static void on_request_call(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *,
                            GVariant *, GDBusMethodInvocation *invocation, gpointer data)
{
    // Request.Close: the app gave up waiting; close the dialog
    auto *p = static_cast<Pending *>(data);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    if (p->close)
        QTimer::singleShot(0, qApp, [close = p->close]() { close(); });
}

static const GDBusInterfaceVTable request_vtable = {on_request_call, nullptr, nullptr, {nullptr}};

static void on_call(GDBusConnection *conn, const gchar *, const gchar *, const gchar *, const gchar *method,
                    GVariant *params, GDBusMethodInvocation *invocation, gpointer)
{
    const gchar *handle, *app_id, *parent, *title;
    GVariant *options = nullptr;
    g_variant_get(params, "(&o&s&s&s@a{sv})", &handle, &app_id, &parent, &title, &options);
    auto *p = new Pending{invocation, conn, 0, parse(QString::fromUtf8(method), QString::fromUtf8(title), options), {}};
    g_variant_unref(options);
    p->request_id = g_dbus_connection_register_object(conn, handle, service->node->interfaces[1], &request_vtable, p,
                                                      nullptr, nullptr);
    service->open_dialogs += 1;
    service->idle.stop();
    QTimer::singleShot(0, qApp, [p]() {
        p->close = service->open(p->req, [p](const Result &res) {
            answer(p, res.ok ? 0 : 1, res);   // 0: chosen, 1: cancelled
            QTimer::singleShot(0, qApp, [p]() { delete p; });
        });
    });
}

static const GDBusInterfaceVTable vtable = {on_call, nullptr, nullptr, {nullptr}};

void serve(Opener open)
{
    if (service)
        return;
    service = new Service;
    service->open = std::move(open);
    service->node = g_dbus_node_info_new_for_xml(XML, nullptr);
    service->idle.setSingleShot(true);
    service->idle.setInterval(60000);
    QObject::connect(&service->idle, &QTimer::timeout, qApp, &QCoreApplication::quit);
    service->idle.start();   // started by D-Bus for a request that may never come
    g_bus_own_name(
        G_BUS_TYPE_SESSION, NAME, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
        [](GDBusConnection *conn, const gchar *, gpointer) {
            g_dbus_connection_register_object(conn, PATH, service->node->interfaces[0], &vtable, nullptr, nullptr,
                                              nullptr);
        },
        nullptr,
        [](GDBusConnection *, const gchar *, gpointer) {   // another Kestrel already serves it
            if (service->open_dialogs == 0)
                QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        },
        nullptr, nullptr);
}

}  // namespace chooser

// ---------------------------------------------------------------- the chooser window's bar

ChooserBar::ChooserBar(MainWindow *win, const chooser::Request &req, std::function<void(const chooser::Result &)> done)
    : win(win), req(req), done(std::move(done))
{
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(8, 6, 8, 6);
    if (req.method == "SaveFile") {
        lay->addWidget(new QLabel("Name:"));
        name = new QLineEdit(req.current_name);
        name->setMinimumWidth(320);
        connect(name, &QLineEdit::returnPressed, this, &ChooserBar::accept);
        connect(name, &QLineEdit::textChanged, this, &ChooserBar::update_button);
        lay->addWidget(name, 1);
        // select the name without its extension, ready to type over
        int dot = req.current_name.lastIndexOf('.');
        name->setSelection(0, dot > 0 ? dot : req.current_name.size());
    } else if (req.method == "SaveFiles") {
        lay->addWidget(new QLabel(QString("Choose a folder for %1 file%2")
                                      .arg(QString::number(req.files.size()), req.files.size() == 1 ? "" : "s")),
                       1);
    } else {
        lay->addStretch(1);
    }
    if (!req.filters.isEmpty()) {
        filter = new QComboBox;
        for (const chooser::Filter &f : req.filters)
            filter->addItem(f.name);
        filter->setCurrentIndex(req.current_filter);
        connect(filter, &QComboBox::currentIndexChanged, this, [this, win]() {
            for (Pane *p : win->panes())
                p->set_type_filter(type_filter());
        });
        lay->addWidget(filter);
    }
    auto *cancel = new QPushButton("Cancel");
    connect(cancel, &QPushButton::clicked, this, [this]() { finish(false); });
    ok_btn = new QPushButton(chooser::button_text(req));
    ok_btn->setDefault(true);
    connect(ok_btn, &QPushButton::clicked, this, &ChooserBar::accept);
    lay->addWidget(cancel);
    lay->addWidget(ok_btn);
    update_button();
}

QStringList ChooserBar::type_filter() const
{
    int i = filter ? filter->currentIndex() : -1;
    return i >= 0 && i < req.filters.size() ? req.filters[i].globs : QStringList();
}

void ChooserBar::update_button()
{
    bool in_folder = !win->cur_dir().isEmpty();
    if (req.method == "SaveFile")
        ok_btn->setEnabled(in_folder && !name->text().trimmed().isEmpty());
    else if (req.saving() || req.directory)
        ok_btn->setEnabled(in_folder || (win->pane() && !win->pane()->selected_paths().isEmpty()));
    else
        ok_btn->setEnabled(win->pane() && !win->pane()->selected_paths().isEmpty());
}

void ChooserBar::selection_changed()
{
    // saving: picking a file puts its name in the box (to save over it, or to start from it)
    if (name && win->pane()) {
        QStringList sel = win->pane()->selected_paths();
        if (sel.size() == 1 && !isdir(sel[0]))
            name->setText(basename(sel[0]));
    }
    update_button();
}

void ChooserBar::activated(const QStringList &files)
{
    if (name) {
        name->setText(basename(files.value(0)));
        accept();
    } else if (!req.saving() && !req.directory) {
        finish(true, req.multiple ? files : files.mid(0, 1));
    }
}

static bool confirm_replace(QWidget *parent, const QStringList &existing)
{
    QString text = existing.size() == 1
                       ? QString("A file named “%1” already exists. Do you want to replace it?").arg(basename(existing[0]))
                       : QString("%1 of these files already exist in this folder. Do you want to replace them?")
                             .arg(existing.size());
    QMessageBox box(QMessageBox::Question, "Replace File", text, QMessageBox::NoButton, parent);
    QPushButton *replace = box.addButton("Replace", QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == replace;
}

void ChooserBar::accept()
{
    if (!ok_btn->isEnabled())
        return;
    QString dir = win->cur_dir();
    QStringList sel = win->pane() ? win->pane()->selected_paths() : QStringList();
    if (req.method == "SaveFile") {
        QString n = name->text().trimmed();
        QString target = n.startsWith('/') || n.startsWith('~') ? expanduser(n) : join(dir, n);
        if (isdir(target)) {   // a folder name: go into it
            win->navigate(target);
            name->clear();
            return;
        }
        if (!isdir(dirname(target))) {
            QMessageBox::warning(this, "Save", QString("The folder “%1” doesn't exist.").arg(dirname(target)));
            return;
        }
        if (lexists(target) && !confirm_replace(this, {target}))
            return;
        finish(true, {target});
    } else if (req.method == "SaveFiles") {
        QString folder = sel.size() == 1 && isdir(sel[0]) ? sel[0] : dir;
        QStringList targets, existing;
        for (const QString &f : req.files) {
            targets << join(folder, f);
            if (lexists(targets.last()))
                existing << targets.last();
        }
        if (!existing.isEmpty() && !confirm_replace(this, existing))
            return;
        finish(true, targets);
    } else if (req.directory) {
        QStringList dirs;
        for (const QString &p : sel)
            if (isdir(p))
                dirs << p;
        if (dirs.isEmpty())
            dirs << dir;
        finish(true, req.multiple ? dirs : dirs.mid(0, 1));
    } else {
        QStringList files;
        for (const QString &p : sel)
            if (!isdir(p))
                files << p;
        if (files.isEmpty()) {   // only folders selected: go into the first
            win->navigate(sel.value(0));
            return;
        }
        finish(true, req.multiple ? files : files.mid(0, 1));
    }
}

void ChooserBar::finish(bool ok, const QStringList &paths)
{
    if (finished)
        return;
    finished = true;
    chooser::Result res;
    res.ok = ok;
    res.paths = paths;
    res.filter = filter ? filter->currentIndex() : -1;
    if (ok && !paths.isEmpty()) {
        QString folder = req.directory && !req.saving() ? paths[0] : dirname(paths[0]);
        settings().setValue("chooser_folder", folder);   // the next chooser starts here
    }
    done(res);
    win->close();
}
