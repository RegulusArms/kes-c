// The window's actions, as a list: every menu entry, submenu and separator, with its shortcuts, in the main menu, the
// toolbar's menus and the context menus. Compared with tests/ui_actions.txt, which is the same in the Python version,
// so a menu entry or shortcut lost when code moves (or added in one version only) shows up as a difference.
// KESTREL_UI_DUMP=file writes the list there instead of comparing (to update ui_actions.txt after a deliberate change).
#include "common.h"

#include <QAbstractSlider>
#include <QAccessible>
#include <QComboBox>
#include <QDropEvent>
#include <QFile>
#include <QHeaderView>
#include <QMimeData>

using namespace test;

// what the computer has installed decides whether these appear (an email client or Bluetooth; Samba; code editors,
// "Open in Zed" and the like): not listed
static bool depends_on_computer(const QString &text)
{
    static const QStringList ALWAYS = {"Open in Terminal", "Open in New Tab", "Open in New Window"};
    return text == "Send To" || text == "Network Sharing…" || (text.startsWith("Open in ") && !ALWAYS.contains(text));
}

static QString label(const QAction *a)   // the text without its mnemonic ("&&" is a real "&")
{
    return QString(a->text()).replace("&&", "\x01").remove('&').replace('\x01', '&');
}

static QString keys(const QAction *a)
{
    QStringList out;
    for (const QKeySequence &k : a->shortcuts())
        out << k.toString(QKeySequence::PortableText);
    return out.isEmpty() ? QString() : " [" + out.join(", ") + "]";
}

static void walk(const QMenu *m, const QString &where, QStringList &out)
{
    for (const QAction *a : m->actions()) {
        if (a->isSeparator()) {
            out << where + " > ---";
            continue;
        }
        QString text = label(a);
        if (depends_on_computer(text))
            continue;
        out << where + " > " + text + keys(a) + (a->isCheckable() ? " (toggle)" : "");
        // the apps offered depend on the computer: only the submenu itself is listed
        if (a->menu() && text != "Open With")
            walk(a->menu(), where + " > " + text, out);
    }
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    setup_app();
    QString home = HOME();
    makedirs(home_path("folder"), true);
    write_text(home_path("file.txt"), "x");
    write_text(home_path("other.txt"), "y");
    MainWindow *w = open_window({home});
    wait_for([&]() { return w->pane() && w->pane()->path == home; });

    QStringList out;
    QList<QToolButton *> buttons = w->findChildren<QToolButton *>();
    for (QToolButton *b : buttons) {
        if (!b->menu())
            continue;
        // named by its tooltip's first line (without a shortcut hint), else its text
        QString name = b->toolTip().section('\n', 0, 0).section(" (", 0, 0).trimmed();
        if (name.isEmpty())
            name = b->text().isEmpty() ? QString("menu") : b->text();
        walk(b->menu(), "button \"" + name + "\"", out);
    }
    QStringList shortcuts;   // every window shortcut, menu or not
    for (const QAction *a : w->actions())
        if (!a->shortcuts().isEmpty())
            shortcuts << "shortcut > " + label(a) + keys(a);
    shortcuts.sort();
    out << shortcuts;
    const QList<QPair<QString, QStringList>> cases = {
        {"empty space", {}},
        {"a file", {home_path("file.txt")}},
        {"a folder", {home_path("folder")}},
        {"two files", {home_path("file.txt"), home_path("other.txt")}},
    };
    for (const auto &[name, paths] : cases) {
        QMenu *m = w->build_menu(w->pane(), paths);
        walk(m, "context menu, " + name, out);
        delete m;
    }

    // -- accessibility: every control has a name a screen reader can say (Qt's own accessibility layer, which screen
    // readers use), as the window opens and with the search bar, the list view and the info panel open
    auto unnamed = [&]() {
        QStringList out;
        for (QWidget *wd : w->findChildren<QWidget *>()) {
            bool control = qobject_cast<QAbstractButton *>(wd) || qobject_cast<QLineEdit *>(wd) ||
                           qobject_cast<QAbstractSlider *>(wd) || qobject_cast<QComboBox *>(wd) ||
                           qobject_cast<QAbstractItemView *>(wd) || qobject_cast<QHeaderView *>(wd);
            if (!control || !wd->isVisibleTo(w))
                continue;
            QAccessibleInterface *ai = QAccessible::queryAccessibleInterface(wd);
            if (!ai || ai->text(QAccessible::Name).trimmed().isEmpty())
                out << QString(wd->metaObject()->className()) + " \"" + wd->toolTip().section('\n', 0, 0) + "\"";
        }
        return out;
    };
    QStringList missing = unnamed();
    w->pane()->start_search();
    w->set_view("list");
    for (QAction *a : w->actions())
        if (a->text() == "Info Panel" && !a->isChecked())
            a->trigger();
    spin(200);
    missing << unnamed();
    missing.removeDuplicates();
    check(missing.isEmpty(), "every control in the window has a name a screen reader can say (also with the search bar, "
                             "the list view and the info panel open)" +
                                 (missing.isEmpty() ? QString() : " (no name: " + missing.join(", ") + ")"));

    // -- drag and drop: a file dropped onto a folder goes into it, in both views, and the folder is highlighted while the
    // file is over it. The events go where a real drag's go: to the innermost widget under the pointer that accepts drops
    w->pane()->close_search();
    auto drop_onto_folder = [&](const QString &mode, const QString &name) {
        w->set_view(mode);
        write_text(home_path(name), "z");
        QAbstractItemView *v = w->pane()->view();
        QModelIndex folder;
        wait_for([&]() {
            for (int r = 0; r < v->model()->rowCount(v->rootIndex()); r++)
                if (v->model()->index(r, 0, v->rootIndex()).data().toString() == "folder")
                    folder = v->model()->index(r, 0, v->rootIndex());
            return folder.isValid() && !v->visualRect(folder).isEmpty();
        });
        QWidget *target = v->viewport();
        while (target && !target->acceptDrops())
            target = target->parentWidget();
        if (!folder.isValid() || !target)
            return false;
        QPointF pos = target->mapFrom(v->viewport(), v->visualRect(folder).center());
        QMimeData data;
        data.setUrls({QUrl::fromLocalFile(home_path(name))});
        QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        QDragMoveEvent move(pos.toPoint(), Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &move);
        bool lit = v->property("drop_target").toString() == home_path("folder");
        QDropEvent drop(pos, Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &drop);
        bool cleared = v->property("drop_target").toString().isEmpty();
        return lit && cleared && wait_for([&]() { return exists(join(home_path("folder"), name)) && !exists(home_path(name)); });
    };
    bool in_grid = drop_onto_folder("grid", "dropped-in-grid.txt"), in_list = drop_onto_folder("list", "dropped-in-list.txt");
    check(in_grid && in_list, "a file dropped onto a folder goes into it, in the grid and the list view, and the folder is highlighted "
                                  "while the file is over it" +
                                  QString(in_grid ? "" : " (not in the grid view)") + QString(in_list ? "" : " (not in the list view)"));

    QString dump = qEnvironmentVariable("KESTREL_UI_DUMP");
    if (!dump.isEmpty()) {
        write_text(dump, (out.join('\n') + '\n').toUtf8());
        std::printf("wrote %lld lines to %s\n", qlonglong(out.size()), dump.toUtf8().constData());
    }
    QStringList expected = QString::fromUtf8(read_file(join(QString(TESTS_DIR), "ui_actions.txt"))).split('\n', Qt::SkipEmptyParts);
    // the window's own menus and shortcuts, then the context menus: each compared in order, naming what differs
    for (bool context : {false, true}) {
        auto part = [context](const QStringList &l) {
            QStringList p;
            for (const QString &x : l)
                if (x.startsWith("context menu") == context)
                    p << x;
            return p;
        };
        QStringList want = part(expected), got = part(out), missing, extra;
        for (const QString &x : want)
            if (!got.contains(x))
                missing << x;
        for (const QString &x : got)
            if (!want.contains(x))
                extra << x;
        QString what = context ? "the context menus are as listed in tests/ui_actions.txt"
                               : "the menus, toolbar menus and shortcuts are as listed in tests/ui_actions.txt";
        if (got != want)
            what += QString(" (missing: %1; extra: %2%3)")
                        .arg(missing.mid(0, 3).join(" | "), extra.mid(0, 3).join(" | "),
                             missing.isEmpty() && extra.isEmpty() ? QString("; the order differs") : QString());
        check(!want.isEmpty() && got == want, what);
    }
    finish();
}
