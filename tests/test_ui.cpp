// The window's actions, as a list: every menu entry, submenu and separator, with its shortcuts, in the main menu, the
// toolbar's menus and the context menus. Compared with tests/ui_actions.txt, so a menu entry or shortcut lost when code
// moves shows up as a difference.
// KESTREL_UI_DUMP=file writes the list there instead of comparing (to update ui_actions.txt after a deliberate change).
#include "common.h"

#include <QAbstractSlider>
#include <QAccessible>
#include <QComboBox>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QMimeData>
#include <QRegularExpression>

using namespace test;

// what the computer has installed decides whether these appear (an email client or Bluetooth; Samba; BleachBit; code
// editors, "Open in Zed" and the like): not listed
static bool depends_on_computer(const QString &text)
{
    static const QStringList ALWAYS = {"Open in Terminal", "Open in New Tab", "Open in New Window"};
    return text == "Send To" || text == "Network Sharing…" || text.endsWith("with BleachBit…") ||
           (text.startsWith("Open in ") && !ALWAYS.contains(text));
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
    QMenu *tab_menu = w->build_tab_menu(0);
    walk(tab_menu, "context menu, a tab", out);
    delete tab_menu;

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

    // -- Properties windows: each a free window of its own (not modal, not kept above or attached to Kestrel), several
    // at once, and the windows they open the same
    {
        auto props_open = []() {
            QList<QPointer<PropertiesDialog>> out;
            for (QWidget *t : QApplication::topLevelWidgets())
                if (auto *d = qobject_cast<PropertiesDialog *>(t); d && d->isVisible())
                    out << d;
            return out;
        };
        auto free_window = is_free_window;
        w->properties({home_path("file.txt")});
        w->properties({home_path("other.txt")});
        spin(100);
        auto open = props_open();
        bool both = open.size() == 2 && free_window(open.value(0)) && free_window(open.value(1)) &&
                    !QApplication::activeModalWidget();
        check(both, "Properties opens as a free window, not modal or kept above Kestrel, and several can be open at once");
        auto *value = open.isEmpty() ? nullptr : new TextDialog(open.first(), "Tag", "value");
        if (value)
            show_free(value);
        spin(50);
        check(free_window(value), "a window Properties opens (View Value…) is a free window too");
        QStringList native;   // a native window inside Kestrel's or Properties' window glitches on Wayland
        for (QWidget *x : QApplication::allWidgets())
            if (x->internalWinId() && !x->isWindow())
                native << QString(x->metaObject()->className()) + " in " + x->window()->metaObject()->className();
        check(native.isEmpty(), "opening those windows doesn't give the widgets in Kestrel's or Properties' window native "
                                "windows of their own" + (native.isEmpty() ? QString() : " (" + native.join(", ") + ")"));
        QPointer<PropertiesDialog> first = open.value(0);
        QPointer<TextDialog> opened = value;
        if (first)
            first->close();
        // a deleteLater() is carried out by the app's event loop; this test runs outside it, and Qt 6.4 doesn't carry
        // one out in the nested loop spin() runs, so do it here (nothing is pending if the window isn't deleted on close)
        bool gone = wait_for([&]() {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return !first && !opened;
        }, 3000);
        int left = int(props_open().size());
        check(gone && left == 1, QString("closing a Properties window deletes it (and what it opened) and leaves the "
                                         "others open (deleted: %1, open: %2)")
                                     .arg(gone ? "yes" : "no")
                                     .arg(left));
        for (auto &d : props_open())
            d->close();
        spin(50);
    }

    // -- every window Kestrel opens is free (windows.h): messages, questions, text, pickers, and the conflict question
    // of a copy; none is opened modally anywhere in the source
    {
        QMessageBox *msg = warn(w, "Test", "a message");
        spin(50);
        check(is_free_window(msg), "a message is a free window");
        msg->close();
        QMessageBox::StandardButton got = QMessageBox::Cancel;
        int answers = 0;
        QMessageBox *q = ask(w, QMessageBox::Question, "Test", "yes?", QMessageBox::Yes | QMessageBox::No,
                             [&](QMessageBox::StandardButton b) { got = b, ++answers; });
        spin(50);
        bool free_q = is_free_window(q);
        if (q)
            q->button(QMessageBox::Yes)->click();
        spin(50);
        check(free_q && got == QMessageBox::Yes && answers == 1, "a question is a free window, and its answer comes back");
        q = ask(w, QMessageBox::Question, "Test", "yes?", QMessageBox::Yes | QMessageBox::No,
                [&](QMessageBox::StandardButton b) { got = b, ++answers; });
        spin(50);
        q->close();
        spin(50);
        check(got == QMessageBox::NoButton && answers == 2, "a question closed without an answer answers nothing, once");
        QString typed;
        ask_text(w, "Test", "Name:", "start", [&](const QString &t) { typed = t; });
        spin(50);
        auto *input = shown_window<QInputDialog>();
        bool free_input = is_free_window(input);
        if (input) {
            input->setTextValue("typed");
            input->accept();
        }
        spin(50);
        check(free_input && typed == "typed", "asking for text is a free window, and the text comes back");

        QString picked;
        choose_folder(w, "Pick", home, [&](const QString &p) { picked = p; });
        spin(200);
        MainWindow *picker = nullptr;
        for (QWidget *t : QApplication::topLevelWidgets())
            if (auto *m = qobject_cast<MainWindow *>(t); m && m->isVisible() && m->chooser)
                picker = m;
        bool free_picker = is_free_window(picker) && !WINDOWS.contains(picker);
        if (picker)
            picker->chooser->finish(true, {home_path("folder")});
        spin(100);
        check(free_picker && picked == home_path("folder"),
              "choosing a folder opens Kestrel's own chooser as a free window, and the folder comes back");

        makedirs(home_path("conflict-from"), true);
        makedirs(home_path("conflict-to"), true);
        write_text(home_path("conflict-from/same.txt"), "new");
        write_text(home_path("conflict-to/same.txt"), "old");
        QList<fileops::Job> planned;
        bool done = false;
        fileops::plan_transfer(w, {home_path("conflict-from/same.txt")}, home_path("conflict-to"), "copy",
                               [&](const QList<fileops::Job> &jobs) { planned = jobs, done = true; });
        spin(50);
        auto *conflict = shown_window<ConflictDialog>();
        bool free_conflict = is_free_window(conflict) && !done;
        if (conflict)
            for (QPushButton *b : conflict->findChildren<QPushButton *>())
                if (b->text() == "Keep Both")
                    b->click();
        spin(50);
        check(free_conflict && done && planned.size() == 1 && planned.value(0).dst != home_path("conflict-to/same.txt"),
              "a copy's name conflict asks in a free window, and the copy is planned with the answer");

        // the source: nothing opened modally (exec(), open(), the static dialog functions, a modality)
        static const QRegularExpression modal(
            R"(\bexec\(\)|\bopen\(\)|QMessageBox::(warning|question|information|critical|about|aboutQt)\(|)"
            R"(QInputDialog::get|QFileDialog::get|QColorDialog::get|QFontDialog::get|setModal\(true\)|)"
            R"(Qt::ApplicationModal|Qt::WindowModal)");
        QStringList found;
        QDir src(QString(TESTS_DIR) + "/../src");
        for (const QString &f : src.entryList({"*.cpp", "*.h"}, QDir::Files)) {
            int n = 0;
            for (const QString &line : QString::fromUtf8(read_file(src.filePath(f))).split('\n')) {
                ++n;
                if (modal.match(line).hasMatch() && !line.contains("app.exec()") && !line.trimmed().startsWith("//"))
                    found << f + ":" + QString::number(n) + ": " + line.trimmed();
            }
        }
        check(found.isEmpty(), "no window is opened modally anywhere in the source" +
                                   (found.isEmpty() ? QString() : ":\n  " + found.join("\n  ")));
    }

    // -- the tab menu: Duplicate Tab puts a copy (same folder and history) beside the tab, Open in New Window moves the
    // tab to a new window, Close Tab closes it
    auto tab_action = [&](int i, const QString &text) {
        QMenu *m = w->build_tab_menu(i);
        for (QAction *a : m->actions())
            if (label(a) == text)
                a->trigger();
        delete m;
    };
    w->navigate(home_path("folder"));
    wait_for([&]() { return w->pane()->path == home_path("folder"); });
    w->new_tab(home);   // the tab to the right of the one duplicated
    int before = w->tabs->count();
    tab_action(0, "Duplicate Tab");
    auto *copy = qobject_cast<Pane *>(w->tabs->widget(1));
    auto *orig = qobject_cast<Pane *>(w->tabs->widget(0));
    bool dup = w->tabs->count() == before + 1 && copy && copy != orig && copy->path == home_path("folder") &&
               w->pane() == copy && copy->back_stack.size() == orig->back_stack.size() && !copy->back_stack.isEmpty() &&
               copy->back_stack.last().path == home;
    check(dup, "Duplicate Tab opens a copy of the tab (its folder and back history) right after it, and shows it");
    int windows = WINDOWS.size(), tabs_before = w->tabs->count();
    tab_action(1, "Open in New Window");
    spin(100);
    MainWindow *nw = WINDOWS.size() == windows + 1 ? WINDOWS.last() : nullptr;
    bool moved = nw && nw != w && wait_for([&]() { return nw->pane() && nw->pane()->path == home_path("folder"); }) &&
                 w->tabs->count() == tabs_before - 1;
    check(moved, "Open in New Window opens the tab's folder in a new window and closes the tab here");
    if (nw)
        nw->close();
    tabs_before = w->tabs->count();
    QWidget *last = w->tabs->widget(tabs_before - 1);
    tab_action(tabs_before - 1, "Close Tab");
    check(w->tabs->count() == tabs_before - 1 && w->tabs->indexOf(last) < 0, "Close Tab in the tab menu closes that tab");

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
