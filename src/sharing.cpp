#include "sharing.h"

#include "proc.h"
#include "util.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include <unistd.h>

using namespace util;

namespace sharing {

// code editors that open a folder as a project: (command, name, icon)
static const QList<QStringList> EDITORS = {{"code", "Visual Studio Code", "vscode"}, {"codium", "VSCodium", "vscodium"},
                                           {"cursor", "Cursor", "cursor"},           {"zed", "Zed", "zed"},
                                           {"subl", "Sublime Text", "sublime-text"}};
static const QStringList FILE_MANAGERS = {"kestrel-explorer.desktop", "folder-explorer.desktop"};

QString scripts_dir()
{
    QByteArray data_home = qgetenv("XDG_DATA_HOME");
    return join(data_home.isEmpty() ? HOME() + "/.local/share" : QString::fromLocal8Bit(data_home), "nautilus/scripts");
}

static bool start(const QStringList &argv, const QString &cwd = QString(), const QProcessEnvironment &env = {})
{
    if (argv.isEmpty())
        return false;
    QProcess p;
    p.setProgram(argv[0]);
    p.setArguments(argv.mid(1));
    if (!cwd.isEmpty())
        p.setWorkingDirectory(cwd);
    if (!env.isEmpty())
        p.setProcessEnvironment(env);
    p.setStandardInputFile(QProcess::nullDevice());
    p.setStandardOutputFile(QProcess::nullDevice());
    p.setStandardErrorFile(QProcess::nullDevice());
    return p.startDetached();
}

// ---------------------------------------------------------------- open folders in other apps

// apps that open folders (e.g. Disk Usage Analyzer), without other file managers
static QList<AppRef> folder_apps(const QString &folder)
{
    QList<AppRef> out;
    for (const AppRef &app : apps_for(folder).first) {
        const char *cats = G_IS_DESKTOP_APP_INFO(app.get()) ? g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(app.get()))
                                                            : nullptr;
        QStringList categories = QString::fromUtf8(cats ? cats : "").split(';');
        if (FILE_MANAGERS.contains(app_id(app)) || categories.contains("FileManager"))
            continue;
        out << app;
    }
    return out;
}

void add_open_folder_menu(QMenu *menu, const QString &folder, std::function<void()> on_other)
{
    for (const QStringList &e : EDITORS) {
        if (!which(e[0]))
            continue;
        QString cmd = e[0];
        menu->addAction(theme_icon({e[2], "text-editor", "accessories-text-editor"}), "Open in " + e[1], menu,
                        [cmd, folder]() { start({cmd, folder}); });
    }
    QMenu *ow = menu->addMenu("Open With");
    for (const AppRef &app : folder_apps(folder))
        ow->addAction(app_icon(app), app_name(app), menu, [app, folder]() {
            try {
                launch_app(app, {folder});
            } catch (const Error &) {
            }
        });
    ow->addSeparator();
    ow->addAction("Other Application…", menu, on_other);
}

// ---------------------------------------------------------------- Nautilus scripts

struct Script {
    QString name, path;
    QList<Script> children;
    bool is_folder = false;
};

static QList<Script> scripts(const QString &folder)
{
    QStringList names;
    try {
        names = listdir(folder);
    } catch (const OSError &) {
        return {};
    }
    natural_sort(names);
    QList<Script> out;
    for (const QString &n : names) {
        if (n.startsWith('.'))
            continue;
        QString p = join(folder, n);
        if (isdir(p)) {
            QList<Script> sub = scripts(p);
            if (!sub.isEmpty())
                out << Script{n, p, sub, true};
        } else if (util::access(p, X_OK)) {
            out << Script{n, p, {}, false};
        }
    }
    return out;
}

bool run_script(const QString &script, const QStringList &paths, const QString &cur_dir)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString file_paths, uris;
    for (const QString &p : paths) {
        file_paths += p + "\n";
        uris += file_uri(p) + "\n";
    }
    env.insert("NAUTILUS_SCRIPT_SELECTED_FILE_PATHS", file_paths);
    env.insert("NAUTILUS_SCRIPT_SELECTED_URIS", uris);
    env.insert("NAUTILUS_SCRIPT_CURRENT_URI", cur_dir.isEmpty() ? QString() : file_uri(cur_dir));
    env.insert("NAUTILUS_SCRIPT_WINDOW_GEOMETRY", "");
    QStringList argv{script};
    for (const QString &p : paths)
        argv << (!cur_dir.isEmpty() && dirname(p) == cur_dir ? basename(p) : p);
    return start(argv, cur_dir.isEmpty() ? HOME() : cur_dir, env);
}

static void fill_scripts(QMenu *m, const QList<Script> &items, const QStringList &paths, const QString &cur_dir)
{
    for (const Script &s : items) {
        if (s.is_folder) {
            fill_scripts(m->addMenu(s.name), s.children, paths, cur_dir);
        } else {
            QString p = s.path;
            m->addAction(s.name, m, [p, paths, cur_dir]() { run_script(p, paths, cur_dir); });
        }
    }
}

void add_scripts_menu(QMenu *menu, const QStringList &paths, const QString &cur_dir,
                      std::function<void(const QString &)> open_folder)
{
    QList<Script> entries = scripts(scripts_dir());
    if (entries.isEmpty())
        return;
    QMenu *sm = menu->addMenu(theme_icon({"text-x-script", "application-x-executable"}), "Scripts");
    fill_scripts(sm, entries, paths, cur_dir);
    sm->addSeparator();
    sm->addAction("Open Scripts Folder", menu, [open_folder]() { open_folder(scripts_dir()); });
}

// ---------------------------------------------------------------- Send To

void add_send_to_menu(QMenu *menu, const QStringList &paths)
{
    if (paths.isEmpty() || std::any_of(paths.begin(), paths.end(), [](const QString &p) { return isdir(p); }))
        return;
    QList<QPair<QStringList, QStringList>> targets;   // (icon + label, argv)
    if (which("xdg-email")) {
        QStringList argv{"xdg-email"};
        for (const QString &p : paths)
            argv << "--attach" << p;
        targets << qMakePair(QStringList{"mail-send", "Email…"}, argv);
    }
    if (which("bluetooth-sendto"))
        targets << qMakePair(QStringList{"bluetooth", "Bluetooth Device…"}, QStringList{"bluetooth-sendto"} + paths);
    if (targets.isEmpty())
        return;
    QMenu *sm = menu->addMenu(theme_icon({"document-send", "mail-send"}), "Send To");
    for (const auto &[label, argv] : targets) {
        QStringList a = argv;
        sm->addAction(theme_icon({label[0], "document-send"}), label[1], menu, [a]() { start(a); });
    }
}

// ---------------------------------------------------------------- network sharing (Samba usershares)

bool can_share() { return which("net"); }

// {folder path: {"name", "comment", "usershare_acl", "guest_ok"}} for this user's network shares
static QHash<QString, QHash<QString, QString>> usershares()
{
    QHash<QString, QHash<QString, QString>> shares;
    auto r = proc::run({"net", "usershare", "info"}, 10000);
    QHash<QString, QString> cur;
    bool in_share = false;
    for (QString line : QString::fromUtf8(r.out).split('\n')) {
        line = line.trimmed();
        if (line.startsWith('[') && line.endsWith(']')) {
            cur = {{"name", line.mid(1, line.size() - 2)}};
            in_share = true;
        } else if (in_share && line.contains('=')) {
            QString k = line.section('=', 0, 0), v = line.section('=', 1);
            cur[k] = v;
            if (k == "path")
                shares[v] = cur;
            else if (shares.contains(cur.value("path")))
                shares[cur.value("path")] = cur;
        }
    }
    return shares;
}

void share_dialog(QWidget *parent, const QString &folder)
{
    auto shares = usershares();
    bool existing = shares.contains(folder);
    QHash<QString, QString> ex = shares.value(folder);
    QDialog d(parent);
    d.setWindowTitle("Network Sharing");
    auto *form = new QFormLayout(&d);
    auto *on = new QCheckBox("Share this folder");
    on->setChecked(existing);
    form->addRow(on);
    QString def = basename(rstrip(folder, '/'));
    auto *name = new QLineEdit(ex.value("name", def.isEmpty() ? QString("share") : def));
    auto *comment = new QLineEdit(ex.value("comment"));
    auto *write = new QCheckBox("Allow others to create and delete files in this folder");
    write->setChecked(ex.value("usershare_acl").contains(":F"));
    auto *guest = new QCheckBox("Guest access (for people without a user account)");
    guest->setChecked(ex.value("guest_ok") == "y");
    form->addRow("Share name:", name);
    form->addRow("Comment:", comment);
    form->addRow(write);
    form->addRow(guest);
    auto *note = new QLabel("<small>Others on your network can then open it as smb://this-computer/share-name. "
                            "Needs the Samba server (sudo apt install samba).</small>");
    note->setWordWrap(true);
    form->addRow(note);
    for (QWidget *w : std::initializer_list<QWidget *>{name, comment, write, guest}) {
        w->setEnabled(on->isChecked());
        QObject::connect(on, &QCheckBox::toggled, w, &QWidget::setEnabled);
    }
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    form->addRow(bb);
    d.resize(460, d.sizeHint().height());
    if (!d.exec())
        return;
    if (existing && (!on->isChecked() || ex.value("name") != name->text().trimmed()))
        proc::run({"net", "usershare", "delete", ex.value("name")}, 10000);
    if (!on->isChecked())
        return;
    QString acl = write->isChecked() ? "Everyone:F" : "Everyone:R";
    auto r = proc::run({"net", "usershare", "add", name->text().trimmed(), folder, comment->text(), acl,
                        QString("guest_ok=%1").arg(guest->isChecked() ? "y" : "n")},
                       10000);
    if (r.rc != 0) {
        QString err = QString::fromUtf8(r.err + r.out).trimmed();
        QMessageBox::warning(parent, "Network Sharing", err.isEmpty() ? QString("Sharing failed.") : err);
    } else {
        struct stat st;
        if (write->isChecked() && stat_(folder, st) && !(st.st_mode & 0002))
            QMessageBox::information(parent, "Network Sharing",
                                     "The folder is shared. For others to create files in it, it must also be writable "
                                     "by others (Properties → Permissions).");
    }
}

}  // namespace sharing
