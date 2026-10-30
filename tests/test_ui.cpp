// The window's actions, as a list: every menu entry, submenu and separator, with its shortcuts, in the main menu, the
// toolbar's menus and the context menus. Compared with tests/ui_actions.txt, so a menu entry or shortcut lost when code
// moves shows up as a difference.
// KESTREL_UI_DUMP=file writes the list there instead of comparing (to update ui_actions.txt after a deliberate change).
#include "common.h"

#include "groups.h"

#include <QAbstractSlider>
#include <QAccessible>
#include <QComboBox>
#include <QDir>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QScrollBar>
#include <QFile>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabBar>
#include <QTreeWidget>

using namespace test;

// what the computer has installed decides whether these appear (an email client or Bluetooth; Samba; BleachBit; code
// editors, "Open in Zed" and the like): not listed
static bool depends_on_computer(const QString &text)
{
    static const QStringList ALWAYS = {"Open in Terminal", "Open in New Tab", "Open in New Window"};
    return text == "Send To" || text == "Network Sharing…" || text.endsWith("with BleachBit…") ||
           (text.startsWith("Open in ") && !ALWAYS.contains(text));
}

// Calls in the source that are given a bare string literal holding a word ("…", QString("…"), {"…", …}) as one of
// their arguments, where it would be shown: "file:line: the call". A call spread over lines is read whole; a literal
// in a nested call (icon({"go-up"}), settings().value("key")) isn't one of its arguments. "notr:" in a comment on
// the call's first line excuses it.
static QStringList untranslated_literals(const QString &dir)
{
    static const QRegularExpression call(
        R"((?<![\w.>:])(?:\w+(?:\.|->))?\b(setText|setToolTip|setWindowTitle|setPlaceholderText|setStatusTip|setWhatsThis|)"
        R"(showMessage|setTitle|setLabelText|setAccessibleName|setAccessibleDescription|addAction|addMenu|addTab|)"
        R"(insertTab|setTabText|setTabToolTip|setHeaderLabels|setHeaderLabel|addItem|addItems|insertItem|setItemText|)"
        R"(setSpecialValueText|setSuffix|setPrefix|setInformativeText|setDetailedText|setButtonText|addButton|addRow|)"
        R"(QAction|QLabel|QPushButton|QCheckBox|QRadioButton|QGroupBox|QMenu|QTreeWidgetItem|QListWidgetItem|)"
        R"(QStandardItem|QMessageBox|warn|inform|ask|ask_yes|ask_box|ask_text|tell|choose_folder|choose_file|show_osd|clear_image|)"
        R"(small|row)\()");
    static const QRegularExpression word("[A-Za-z]{2}");
    QStringList out;
    QDir src(dir);
    for (const QString &f : src.entryList({"*.cpp", "*.h"}, QDir::Files)) {
        if (f == "admin_helper.cpp" || f == "i18n.h")   // (the root helper shows nothing; i18n.h documents tx())
            continue;
        const QString text = QString::fromUtf8(read_file(src.filePath(f)));
        auto it = call.globalMatch(text);
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            int line_start = text.lastIndexOf('\n', m.capturedStart()) + 1;
            QString line = text.mid(line_start, text.indexOf('\n', m.capturedStart()) - line_start);
            if (line.trimmed().startsWith("//") || line.trimmed().startsWith("*") || line.contains("notr:"))
                continue;
            // its arguments, at the top level: split on commas outside brackets, strings and characters
            QStringList args;
            QString cur;
            int depth = 0;
            for (int i = m.capturedEnd(); i < text.size(); ++i) {
                QChar c = text[i];
                if (c == '"' || c == '\'') {   // a literal, whole
                    int j = i + 1;
                    while (j < text.size() && text[j] != c)
                        j += text[j] == '\\' ? 2 : 1;
                    cur += text.mid(i, j - i + 1);
                    i = j;
                    continue;
                }
                if (depth == 0 && (c == ',' || c == ')')) {
                    args << cur.trimmed();
                    cur.clear();
                    if (c == ')')
                        break;
                    continue;
                }
                if (c == '(' || c == '{' || c == '[')
                    ++depth;
                else if (c == ')' || c == '}' || c == ']')
                    --depth;
                cur += c;
            }
            // which arguments are shown: a message's all (title, text, buttons); a text window's title and label; a
            // chooser's title and filter; otherwise the first one holding a literal (an icon's names aside): after it
            // come data (a combo box item's value, a key)
            const QString fn = m.captured(1);
            static const QStringList all = {"warn", "inform", "tell", "ask", "ask_yes", "QMessageBox"};
            QList<int> shown;
            if (all.contains(fn)) {
                for (int k = 0; k < args.size(); ++k)
                    shown << k;
            } else if (fn == "ask_text") {
                shown = {1, 2};
            } else if (fn == "choose_folder" || fn == "choose_file") {
                shown = {1, 3};
            } else {
                for (int k = 0; k < args.size(); ++k)
                    if (args[k].contains('"') && !args[k].startsWith("icon(") && !args[k].startsWith("theme_icon(") &&
                        !args[k].startsWith("QIcon")) {
                        shown << k;
                        break;
                    }
            }
            for (int k : shown) {
                QString a = args.value(k);
                for (const char *wrap : {"QString(", "QStringLiteral(", "QLatin1String(", "{"})
                    if (a.startsWith(wrap))
                        a = a.mid(int(strlen(wrap))).trimmed();
                if (!a.startsWith('"'))
                    continue;
                int end = 1;
                while (end < a.size() && a[end] != '"')
                    end += a[end] == '\\' ? 2 : 1;
                if (word.match(a.mid(1, end - 1)).hasMatch()) {
                    out << f + ":" + QString::number(text.left(m.capturedStart()).count('\n') + 1) + ": " +
                               line.trimmed().left(140);
                    break;
                }
            }
        }
    }
    return out;
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

    // -- files dragged onto another tab: resting on the tab shows it (after a moment), so the drag goes on into a
    // folder there; dropped onto the tab itself, they go into its folder (the Trash's: Move to Trash). The tab is
    // highlighted while it would take them; a tab with no one folder (the Overview) is still shown, but takes none
    {
        w->set_view("grid");
        QString dst = home_path("tab-dst");
        makedirs(join(dst, "sub"), true);
        makedirs(join(TRASH_DIR(), "files"), true);
        makedirs(join(TRASH_DIR(), "info"), true);
        Pane *from = w->pane();
        Pane *to = w->new_tab(dst, false), *ov = w->new_tab(OVERVIEW, false), *trash = w->new_tab(join(TRASH_DIR(), "files"), false);
        QTabBar *bar = w->tabs->tabBar();
        spin(200);
        auto send = [&](QWidget *target, QEvent::Type type, const QPoint &pos, const QMimeData &data) {
            if (type == QEvent::Drop) {
                QDropEvent e(pos, Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(target, &e);
            } else if (type == QEvent::DragLeave) {
                QDragLeaveEvent e;
                QApplication::sendEvent(target, &e);
            } else {
                QDragMoveEvent e(pos, Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier, type);
                QApplication::sendEvent(target, &e);
            }
        };
        auto over_tab = [&](Pane *p, const QMimeData &data) {   // the drag comes onto p's tab and rests there
            QPoint at = bar->tabRect(w->tabs->indexOf(p)).center();
            send(bar, QEvent::DragEnter, at, data);
            send(bar, QEvent::DragMove, at, data);
            return at;
        };
        QStringList res;
        // onto a tab, dropped there: into its folder
        write_text(home_path("tab-1.txt"), "1");
        QMimeData d1;
        d1.setUrls({QUrl::fromLocalFile(home_path("tab-1.txt"))});
        QPoint at = over_tab(to, d1);
        bool lit = bar->property("drop_tab").toInt() == w->tabs->indexOf(to);
        bool shown = wait_for([&]() { return w->pane() == to; }, 3000);
        send(bar, QEvent::Drop, at, d1);
        bool unlit = bar->property("drop_tab").toInt() == -1;
        if (!lit)
            res << "the tab isn't highlighted";
        if (!shown)
            res << "resting on the tab doesn't show it";
        if (!unlit)
            res << "the highlight stays after the drop";
        if (!wait_for([&]() { return exists(join(dst, "tab-1.txt")) && !exists(home_path("tab-1.txt")); }))
            res << "the file dropped onto the tab isn't in its folder";
        // onto a tab, then on into a subfolder shown there
        w->tabs->setCurrentWidget(from);
        write_text(home_path("tab-2.txt"), "2");
        QMimeData d2;
        d2.setUrls({QUrl::fromLocalFile(home_path("tab-2.txt"))});
        at = over_tab(to, d2);
        wait_for([&]() { return w->pane() == to; }, 3000);
        send(bar, QEvent::DragLeave, at, d2);
        QAbstractItemView *v = to->view();
        QModelIndex sub;
        wait_for([&]() {
            for (int r = 0; r < v->model()->rowCount(v->rootIndex()); r++)
                if (v->model()->index(r, 0, v->rootIndex()).data(PathRole).toString() == join(dst, "sub"))
                    sub = v->model()->index(r, 0, v->rootIndex());
            return sub.isValid() && !v->visualRect(sub).isEmpty();
        });
        if (sub.isValid()) {
            QPoint p = v->visualRect(sub).center();
            send(v->viewport(), QEvent::DragEnter, p, d2);
            send(v->viewport(), QEvent::DragMove, p, d2);
            send(v->viewport(), QEvent::Drop, p, d2);
        }
        if (!wait_for([&]() { return exists(join(dst, "sub/tab-2.txt")) && !exists(home_path("tab-2.txt")); }))
            res << "the drag can't go on into a subfolder in the tab it showed";
        // the Overview's tab: shown, not highlighted, and a drop onto it does nothing
        w->tabs->setCurrentWidget(from);
        write_text(home_path("tab-3.txt"), "3");
        QMimeData d3;
        d3.setUrls({QUrl::fromLocalFile(home_path("tab-3.txt"))});
        at = over_tab(ov, d3);
        bool ov_lit = bar->property("drop_tab").toInt() != -1;
        bool ov_shown = wait_for([&]() { return w->pane() == ov; }, 3000);
        send(bar, QEvent::Drop, at, d3);
        spin(300);
        if (ov_lit || !ov_shown || !exists(home_path("tab-3.txt")))
            res << "the Overview's tab takes the drop or isn't shown";
        // the Trash's tab: Move to Trash
        w->tabs->setCurrentWidget(from);
        at = over_tab(trash, d3);
        send(bar, QEvent::Drop, at, d3);
        if (!wait_for([&]() {
                return !exists(home_path("tab-3.txt")) && exists(join(TRASH_DIR(), "files/tab-3.txt")) &&
                       exists(join(TRASH_DIR(), "info/tab-3.txt.trashinfo"));
            }))
            res << "a drop onto the Trash's tab doesn't move it to the trash";
        check(res.isEmpty(), "files dragged onto a tab: resting there shows it, highlighted, and the drag goes on into a "
                             "folder there; dropped onto it, into its folder (the Trash's: Move to Trash); the "
                             "Overview's tab is shown but takes none" +
                                 (res.isEmpty() ? QString() : " (" + res.join("; ") + ")"));
        for (Pane *p : {to, ov, trash})
            w->close_tab(w->tabs->indexOf(p));
        w->tabs->setCurrentWidget(from);
    }

    // -- in the grid, an item is its icon and name, not its whole cell: a right-click elsewhere in the cell gives the
    // folder's own menu (empty space's: New Folder…), on the icon or the name the item's (Open); the rest of what asks
    // what's under the pointer (clicks, hover, drops) goes through the same indexAt
    {
        QString dir = home_path("cells");
        makedirs(dir, true);
        write_text(join(dir, "a.txt"), "a");   // a short name: one line, narrower than the cell
        w->navigate(dir);
        w->set_view("grid");
        Pane *p = w->pane();
        p->set_sizes(128, p->list_size);
        QAbstractItemView *v = p->view();
        QModelIndex item;
        wait_for([&]() {
            for (int r = 0; r < v->model()->rowCount(v->rootIndex()); r++)
                if (v->model()->index(r, 0, v->rootIndex()).data(PathRole).toString() == join(dir, "a.txt"))
                    item = v->model()->index(r, 0, v->rootIndex());
            return item.isValid() && !v->visualRect(item).isEmpty();
        });
        auto menu_has = [&](const QPoint &pos, const QString &text) {
            v->clearSelection();
            QMenu *m = w->menu_at(p, v, pos);
            bool has = false;
            for (QAction *a : m->actions())
                has = has || label(a) == text;
            delete m;
            return has;
        };
        QRect cell = v->visualRect(item);
        int fh = v->fontMetrics().height();
        QPoint on_icon(cell.center().x(), cell.top() + 3 + 6 + 128 / 2);
        QPoint on_name(cell.center().x(), cell.top() + 3 + 6 + 128 + 6 + fh / 2);
        QPoint corner(cell.left() + 6, cell.bottom() - 4), beside_name(cell.left() + 6, on_name.y());
        QStringList res;
        if (!item.isValid())
            res << "not shown";
        if (!menu_has(on_icon, "Open"))
            res << "the icon gives no item menu";
        if (!menu_has(on_name, "Open"))
            res << "the name gives no item menu";
        if (!menu_has(corner, "New Folder…") || menu_has(corner, "Open"))
            res << "the cell's empty corner gives the item's menu";
        if (!menu_has(beside_name, "New Folder…") || menu_has(beside_name, "Open"))
            res << "the space beside a short name gives the item's menu";
        if (v->indexAt(beside_name).isValid() || !v->indexAt(on_icon).isValid())
            res << "indexAt doesn't follow the box";
        check(res.isEmpty(), "in the grid, a right-click outside an item's icon and name (in its cell) gives the folder's "
                             "menu, on the icon or the name the item's" +
                                 (res.isEmpty() ? QString() : " (" + res.join("; ") + ")"));
        w->navigate(home);
        wait_for([&]() { return w->pane()->path == home; });
    }

    // -- a band dragged from empty space selects the items it touches, whichever way it goes: grid and list, plain and
    // grouped by date. Up-left, the grid's band (a rect with a negative width and height) was taken for a press on
    // empty space and selected nothing.
    {
        QString dir = home_path("band");
        makedirs(dir, true);
        QStringList files;
        for (int i = 0; i < 4; i++) {
            files << join(dir, QString("b%1.txt").arg(i));
            write_text(files.last(), "b");
        }
        // in home's listing first: a folder opened before then is added to it afterwards, and the view loses its root
        wait_for([&]() {
            QAbstractItemView *v = w->pane()->view();
            for (int r = 0; r < v->model()->rowCount(v->rootIndex()); r++)
                if (v->model()->index(r, 0, v->rootIndex()).data(PathRole).toString() == dir)
                    return true;
            return false;
        });
        w->navigate(dir);
        wait_for([&]() { return w->pane()->path == dir; });
        QStringList res;
        for (int group : {groups::NONE, groups::PERIOD}) {
            for (const QString mode : {"grid", "list"}) {
                w->set_view(mode);
                Pane *p = w->pane();
                p->set_grouping(group, groups::MODIFIED);
                QAbstractItemView *v = p->view();
                QWidget *vp = v->viewport();
                QRect items;   // where the four are, in the viewport
                wait_for([&]() {
                    QApplication::processEvents();
                    items = QRect();
                    int n = 0;
                    if (group == groups::NONE && v->rootIndex().data(PathRole).toString() != dir)   // (grouped: the groups')
                        return false;
                    for (int r = 0; r < v->model()->rowCount(v->rootIndex()); r++) {
                        QModelIndex i = v->model()->index(r, 0, v->rootIndex());
                        QRect cell = v->visualRect(i);
                        if (files.contains(i.data(PathRole).toString()) && !cell.isEmpty()) {
                            items |= cell;
                            n++;
                        }
                    }
                    return n == 4;
                });
                // from below and right of them all (empty space) to above and left of the first, and back the other way;
                // then from the top right and bottom left corners (the list: the two from below)
                QPoint tl(1, items.top() + 1), br(std::min(vp->width() - 2, items.right() + 40), items.bottom() + 40);
                const QList<QPair<QPoint, QPoint>> drags = {
                    {br, tl}, {tl, br}, {QPoint(br.x(), tl.y()), QPoint(tl.x(), br.y())}, {QPoint(tl.x(), br.y()), QPoint(br.x(), tl.y())}};
                for (const auto &[from, to] : drags) {
                    QString what = QString("%1%2, from (%3,%4) to (%5,%6)")
                                       .arg(mode, group ? QString(" grouped") : QString())
                                       .arg(from.x()).arg(from.y()).arg(to.x()).arg(to.y());
                    v->clearSelection();
                    if (v->indexAt(from).isValid()) {   // the list's rows span its width: its empty space is below them
                        if (mode == "grid")
                            res << what + ": the start isn't empty space";
                        continue;
                    }
                    auto send = [&](QEvent::Type t, QPoint pos, Qt::MouseButton b, Qt::MouseButtons held) {
                        QMouseEvent ev(t, QPointF(pos), QPointF(vp->mapToGlobal(pos)), b, held, Qt::NoModifier);
                        QApplication::sendEvent(vp, &ev);
                    };
                    send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
                    for (int k = 1; k <= 10; k++)
                        send(QEvent::MouseMove, from + (to - from) * k / 10, Qt::NoButton, Qt::LeftButton);
                    send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
                    QStringList got = p->selected_paths();
                    got.sort();
                    if (got != files)
                        res << QString("%1: %2 of 4 selected").arg(what).arg(got.size());
                }
            }
            w->pane()->set_grouping(groups::NONE, groups::MODIFIED);
        }
        check(res.isEmpty(), "a band dragged from empty space selects the items it touches, in any direction, in the grid "
                             "and the list, plain and grouped" +
                                 (res.isEmpty() ? QString() : " (" + res.join("; ") + ")"));
        w->set_view("grid");
        w->navigate(home);
        wait_for([&]() { return w->pane()->path == home; });
    }

    // -- a folder whose name holds "&": its tab doubles it, which the tab bar shows as one "&" (a lone one underlined
    // the next letter instead), and the window's title has it as it is
    {
        QString dir = home_path("Tom & Jerry");
        makedirs(dir, true);
        Pane *own = w->new_tab(dir);
        spin(100);
        int i = w->tabs->indexOf(own);
        check(w->tabs->tabText(i) == "Tom && Jerry" && w->windowTitle().startsWith("Tom & Jerry — "),
              "a folder named “Tom & Jerry” shows as that on its tab and in the window's title (tab: " + w->tabs->tabText(i) +
                  ", title: " + w->windowTitle() + ")");
        w->close_tab(i);
    }

    // -- a drag that starts in the view itself, grouped by period or by day: what it carries is what the view's own
    // model gives for the selected items (as startDrag asks it), with the drag actions it offers. Grouped, that model
    // gave no files (the drop onto a folder did nothing) and offered no move.
    auto own_drag = [&](const QString &mode, int group) {
        QString dir = home_path(QString("own-%1-%2").arg(mode).arg(group));
        makedirs(join(dir, "into"), true);
        write_text(join(dir, "moving.txt"), "m");
        Pane *own = w->new_tab(dir);
        struct Closer {
            MainWindow *w;
            Pane *p;
            ~Closer() { w->close_tab(w->tabs->indexOf(p)); }
        } closer{w, own};
        w->set_view(mode);
        w->pane()->set_grouping(group, groups::MODIFIED);
        QAbstractItemView *v = w->pane()->view();
        QModelIndex file, folder;
        wait_for([&]() {
            for (int r = 0; r < v->model()->rowCount(v->rootIndex()); r++) {
                QModelIndex i = v->model()->index(r, 0, v->rootIndex());
                if (i.data(PathRole).toString() == join(dir, "moving.txt"))
                    file = i;
                if (i.data(PathRole).toString() == join(dir, "into"))
                    folder = i;
            }
            return file.isValid() && folder.isValid() && !v->visualRect(folder).isEmpty();
        });
        if (!file.isValid() || !folder.isValid())
            return QString("not shown");
        std::unique_ptr<QMimeData> data(v->model()->mimeData({file}));
        if (!data || data->urls() != QList<QUrl>{QUrl::fromLocalFile(join(dir, "moving.txt"))})
            return QString("the drag carries no file");
        if (!(v->model()->supportedDragActions() & Qt::MoveAction))
            return QString("the drag offers no move");
        QPointF pos = v->visualRect(folder).center();
        QWidget *dropw = v->viewport();
        QDragEnterEvent enter(pos.toPoint(), Qt::MoveAction, data.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(dropw, &enter);
        QDragMoveEvent move(pos.toPoint(), Qt::MoveAction, data.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(dropw, &move);
        QDropEvent drop(pos, Qt::MoveAction, data.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(dropw, &drop);
        if (!wait_for([&]() { return exists(join(dir, "into/moving.txt")) && !exists(join(dir, "moving.txt")); }))
            return QString("not moved");
        return QString();
    };
    QStringList own_results;
    for (const QString &mode : {QString("grid"), QString("list")})
        for (int group : {int(groups::PERIOD), int(groups::DAY)}) {
            QString r = own_drag(mode, group);
            if (!r.isEmpty())
                own_results << QString("%1, %2: %3").arg(mode, group == groups::PERIOD ? "by period" : "by day", r);
        }
    check(own_results.isEmpty(), "files dragged from a view grouped by period or by day onto a folder in it move into "
                                 "that folder, in the grid and the list" +
                                     (own_results.isEmpty() ? QString() : " (" + own_results.join("; ") + ")"));

    // -- a drop into a subfolder doesn't move the view: the dragged items (the view's current item among them) leave the
    // folder, and the view stays where it was instead of scrolling to where they were. Here the subfolder is at the top
    // (folders first) and the dragged files near the bottom, with the view scrolled to the top.
    auto view_stays = [&](const QString &mode, int group) {
        QString dir = home_path("long-" + mode + QString::number(group));
        makedirs(join(dir, "aa-target"), true);
        for (int i = 0; i < 300; ++i)
            write_text(join(dir, QString("f%1.txt").arg(i, 3, 10, QChar('0'))), "x");
        Pane *own = w->new_tab(dir);   // a tab of its own (the tab menu checks below look at the first tab's history)
        struct Closer {
            MainWindow *w;
            Pane *p;
            ~Closer() { w->close_tab(w->tabs->indexOf(p)); }
        } closer{w, own};
        w->set_view(mode);
        w->pane()->set_grouping(group, groups::MODIFIED);
        QAbstractItemView *v = w->pane()->view();
        if (!wait_for([&]() { return w->pane()->all_paths().size() == 301 && v->verticalScrollBar()->maximum() > 0; }))
            return QString("folder not shown");
        QStringList moving = {join(dir, "f297.txt"), join(dir, "f298.txt"), join(dir, "f299.txt")};
        w->pane()->select_paths(moving);   // also makes the first one current
        spin(800);   // past select_paths' own scrolls to it again (at up to 700 ms), which would undo the one to the top
        v->verticalScrollBar()->setValue(0);
        spin(100);
        if (v->verticalScrollBar()->value() != 0)
            return QString("not at the top before the drop");
        QModelIndex target;
        for (int r = 0; r < v->model()->rowCount(v->rootIndex()); r++) {
            QModelIndex i = v->model()->index(r, 0, v->rootIndex());
            if (i.data(PathRole).toString() == join(dir, "aa-target"))
                target = i;
        }
        if (!target.isValid())
            return QString("no target");
        QWidget *dropw = v->viewport();
        QPointF pos = v->visualRect(target).center();
        QMimeData data;
        QList<QUrl> urls;
        for (const QString &m : moving)
            urls << QUrl::fromLocalFile(m);
        data.setUrls(urls);
        QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(dropw, &enter);
        QDragMoveEvent move(pos.toPoint(), Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(dropw, &move);
        QDropEvent drop(pos, Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(dropw, &drop);
        if (!wait_for([&]() { return w->pane()->all_paths().size() == 298 && exists(join(dir, "aa-target/f299.txt")); }))
            return QString("not moved");
        spin(1000);   // re-selection and late scrolls (they come at up to 700 ms)
        int at = v->verticalScrollBar()->value();
        return at == 0 ? QString() : QString("scrolled to %1 of %2").arg(at).arg(v->verticalScrollBar()->maximum());
    };
    QString grid_r = view_stays("grid", groups::NONE), list_r = view_stays("list", groups::NONE),
            grouped_r = view_stays("grid", groups::PERIOD);
    check(grid_r.isEmpty() && list_r.isEmpty() && grouped_r.isEmpty(),
          "dropping items into a subfolder leaves the view where it was (grid: " + (grid_r.isEmpty() ? "ok" : grid_r) +
              "; list: " + (list_r.isEmpty() ? "ok" : list_r) + "; grouped: " + (grouped_r.isEmpty() ? "ok" : grouped_r) + ")");

    // -- many selected folders dropped into a sibling don't hold the window. Each one leaving the view deselects it, and
    // the status line counted the whole selection again for each (with a list search for duplicates in it): 800
    // folders froze the window for seconds. A timer ticking every 10 ms measures the longest the window was held.
    {
        QString dir = home_path("many");
        const int n = 1500;
        makedirs(join(dir, "aa-target"), true);
        QStringList moving;
        for (int i = 0; i < n; ++i) {
            moving << join(dir, QString("d%1").arg(i, 4, 10, QChar('0')));
            makedirs(moving.last(), true);
        }
        Pane *own = w->new_tab(dir);
        struct Closer {
            MainWindow *w;
            Pane *p;
            ~Closer() { w->close_tab(w->tabs->indexOf(p)); }
        } closer{w, own};
        w->set_view("grid");
        w->pane()->set_grouping(groups::NONE, groups::MODIFIED);
        QAbstractItemView *v = w->pane()->view();
        QString r;
        if (!wait_for([&]() { return w->pane()->all_paths().size() == n + 1; }, 15000))
            r = "folder not shown";
        QModelIndex target;
        if (r.isEmpty()) {
            w->pane()->select_paths(moving);
            spin(100);
            for (int row = 0; row < v->model()->rowCount(v->rootIndex()); row++) {
                QModelIndex i = v->model()->index(row, 0, v->rootIndex());
                if (i.data(PathRole).toString() == join(dir, "aa-target"))
                    target = i;
            }
            if (w->pane()->selected_paths().size() != n || !target.isValid())
                r = "not selected";
        }
        if (r.isEmpty()) {
            QElapsedTimer clock;
            qint64 last = 0, longest = 0;
            QTimer tick;
            QObject::connect(&tick, &QTimer::timeout, [&]() {
                longest = std::max(longest, clock.elapsed() - last);
                last = clock.elapsed();
            });
            QWidget *dropw = v->viewport();
            QPointF pos = v->visualRect(target).center();
            QMimeData data;
            QList<QUrl> urls;
            for (const QString &m : moving)
                urls << QUrl::fromLocalFile(m);
            data.setUrls(urls);
            QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(dropw, &enter);
            QDragMoveEvent move(pos.toPoint(), Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(dropw, &move);
            QDropEvent drop(pos, Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
            clock.start();
            tick.start(10);
            QApplication::sendEvent(dropw, &drop);
            bool moved = wait_for([&]() { return w->pane()->all_paths().size() == 1; }, 60000);
            spin(300);   // the status line's update after the burst
            tick.stop();
            if (!moved || !exists(join(dir, "aa-target/d1499")))
                r = "not moved";
            else if (longest > 2500)
                r = QString("the window was held for %1 ms").arg(longest);
        }
        check(r.isEmpty(), QString::number(n) + " selected folders dropped into a sibling move without holding the window" +
                               (r.isEmpty() ? QString() : " (" + r + ")"));
    }

    // -- going back with the mouse's back button: the hand moving a little while the button is still down mustn't drag
    // anything. It used to drag the folder just left (selected again in the folder above), and the drag going nowhere
    // showed its picture sliding back into place. A drag's picture moving in the view is a QLabel with an opacity effect.
    {
        QString parent = home_path("flyp"), child = join(parent, "child");
        makedirs(child, true);
        for (int i = 0; i < 6; ++i)
            write_text(join(child, QString("c%1.txt").arg(i)), "x");
        Pane *own = w->new_tab(parent);
        w->set_view("grid");
        QAbstractItemView *v = own->view();
        QWidget *vp = v->viewport();
        auto flying = [&]() {
            int n = 0;
            for (QLabel *l : vp->findChildren<QLabel *>())
                if (l->graphicsEffect())
                    ++n;
            return n;
        };
        auto rect_of = [&](const QString &p) {
            for (int r = 0; r < v->model()->rowCount(v->rootIndex()); r++) {
                QModelIndex i = v->model()->index(r, 0, v->rootIndex());
                if (i.data(PathRole).toString() == p)
                    return v->visualRect(i);
            }
            return QRect();
        };
        auto send = [&](QEvent::Type t, QPoint pos, Qt::MouseButton b, Qt::MouseButtons bs) {
            QMouseEvent e(t, QPointF(pos), vp->mapToGlobal(QPointF(pos)), b, bs, Qt::NoModifier);
            QApplication::sendEvent(vp, &e);
        };
        wait_for([&]() { return !rect_of(child).isEmpty(); });
        QPoint c = rect_of(child).center();
        send(QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);   // double-click into the folder
        send(QEvent::MouseButtonRelease, c, Qt::LeftButton, Qt::NoButton);
        send(QEvent::MouseButtonDblClick, c, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, c, Qt::LeftButton, Qt::NoButton);
        wait_for([&]() { return own->path == child && !rect_of(join(child, "c1.txt")).isEmpty(); });
        QPoint f = rect_of(join(child, "c1.txt")).center();
        send(QEvent::MouseButtonPress, f, Qt::LeftButton, Qt::LeftButton);   // select something there
        send(QEvent::MouseButtonRelease, f, Qt::LeftButton, Qt::NoButton);
        send(QEvent::MouseButtonPress, f, Qt::BackButton, Qt::BackButton);   // back, the button held...
        wait_for([&]() { return own->path == parent; });
        spin(100);
        for (int i = 1; i < 10; ++i)   // ...while the hand moves a little
            send(QEvent::MouseMove, f + QPoint(i * 2, i), Qt::NoButton, Qt::BackButton);
        send(QEvent::MouseButtonRelease, f + QPoint(20, 10), Qt::BackButton, Qt::NoButton);
        spin(50);
        check(own->path == parent && flying() == 0,
              "going back with the mouse's back button, moving the mouse while it's down drags nothing (no picture of "
              "the folder just left slides into place)");
        w->close_tab(w->tabs->indexOf(own));
    }

    // -- the grid packs its items: item n in the n-th cell, as many columns as fit, no empty cell between items. A
    // folder of 850 (more than the 400 a batched layout used to lay out at a time, which started the next batch on a
    // new row after a re-layout), re-laid out by zooming and by window widths one pixel apart (some make the cells add
    // up to exactly the width, which used to push the last column onto the next row)
    {
        QString dir = home_path("packed");
        makedirs(dir, true);
        for (int i = 0; i < 850; ++i)
            write_text(join(dir, QString("p%1.txt").arg(i, 4, 10, QChar('0'))), "x");
        Pane *own = w->new_tab(dir);
        w->set_view("grid");
        QListView *g = own->grid;
        wait_for([&]() { return own->all_paths().size() == 850; });
        spin(300);
        auto unpacked = [&]() {   // "" if packed, else where it isn't
            QAbstractItemModel *m = g->model();
            QSize cell = g->gridSize();
            int n = m->rowCount(g->rootIndex());
            if (!cell.isValid() || n != 850)
                return QString("not shown");
            // how many columns fit: unstretched cells (the delegate's) in the width, which an item may not end exactly at
            auto *del = qobject_cast<GridDelegate *>(g->itemDelegate());
            int sb = g->verticalScrollBar()->isVisible() ? 0 : g->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
            int cols = del ? std::max(1, (g->viewport()->width() - sb - 1) / del->cell_size().width()) : -1;
            int off = g->verticalScrollBar()->value();
            for (int r = 0; r < n; ++r) {
                QRect vr = g->visualRect(m->index(r, 0, g->rootIndex()));
                int col = vr.x() / cell.width(), row = (vr.y() + off) / cell.height();
                if (row * cols + col != r)
                    return QString("item %1 in column %2, row %3 of %4 columns (width %5, cell %6)")
                        .arg(r).arg(col).arg(row).arg(cols).arg(g->viewport()->width()).arg(cell.width());
            }
            return QString();
        };
        QString bad;
        for (int z : {60, 200, 128}) {
            own->zoom(0, z);
            spin(100);
            if (bad.isEmpty() && !unpacked().isEmpty())
                bad = QString("zoom %1: ").arg(z) + unpacked();
        }
        QSize old = w->size();
        for (int width = 800; width < 870 && bad.isEmpty(); ++width) {
            w->resize(width, 700);
            QCoreApplication::processEvents();
            own->update_grid_size();
            QCoreApplication::processEvents();
            if (!unpacked().isEmpty())
                bad = QString("window %1 wide: ").arg(width) + unpacked();
        }
        w->resize(old);
        check(bad.isEmpty(), "the grid packs its items (no empty cell, as many columns as fit), also after zooming and at "
                             "every window width" + (bad.isEmpty() ? QString() : " (" + bad + ")"));
        w->close_tab(w->tabs->indexOf(own));
    }

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

    // -- the right-click menu's shortcuts into Properties: Calculate Folder Size counts afresh (not the size remembered),
    // Open Metadata opens it on the Metadata tab, and shows for images, not for a plain text file
    {
        auto entry = [&](const QStringList &paths, const QString &id) -> QAction * {
            QMenu *m = w->build_menu(w->pane(), paths);
            m->deleteLater();
            for (QAction *a : m->actions())
                if (menus::id_of(a) == id)
                    return a;
            return nullptr;
        };
        auto newest_props = [&](const QSet<QWidget *> &before) -> PropertiesDialog * {
            for (QWidget *t : QApplication::topLevelWidgets())
                if (auto *d = qobject_cast<PropertiesDialog *>(t); d && d->isVisible() && !before.contains(d))
                    return d;
            return nullptr;
        };
        auto tops = []() {
            const QWidgetList l = QApplication::topLevelWidgets();
            return QSet<QWidget *>(l.begin(), l.end());
        };
        QString dir = home_path("sizeme");
        makedirs(dir, true);
        write_text(join(dir, "a.bin"), QByteArray(1000, 'x'));
        fileops::dirsizes::put(dir, fileops::DirStats{5, 1, 0});   // remembered, and wrong
        QSet<QWidget *> before = tops();
        QAction *calc = entry({dir}, "calculate_size");
        if (calc)
            calc->trigger();
        bool counted = calc && wait_for([&]() {
            auto e = fileops::dirsizes::get(dir);
            return e && e->stats.size == 1000;
        }, 5000);
        PropertiesDialog *pd = newest_props(before);
        check(calc && pd && counted, "Calculate Folder Size opens Properties and counts the folder again, not the size "
                                     "remembered");
        if (pd)
            pd->close();
        QString img = home_path("meta.png");
        QImage(8, 8, QImage::Format_RGB32).save(img);
        before = tops();
        QAction *meta = entry({img}, "open_metadata");
        if (meta)
            meta->trigger();
        spin(100);
        pd = newest_props(before);
        QTabWidget *tw = pd ? pd->findChild<QTabWidget *>() : nullptr;
        check(meta && tw && tw->tabText(tw->currentIndex()) == "Metadata",
              "Open Metadata opens Properties on its Metadata tab");
        if (pd)
            pd->close();
        check(!entry({home_path("file.txt")}, "open_metadata") && !entry({home_path("file.txt")}, "calculate_size"),
              "...and neither shows for a plain text file");
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

        // the source: no text the user sees passed straight to a widget or a message (setText("…"), QAction("…"),
        // warn(…, "…")…) without tx() or txn() (src/i18n.h), so it can be translated. A line can say why one isn't,
        // with "notr:" in a comment.
        QStringList bare = untranslated_literals(QString(TESTS_DIR) + "/../src");
        check(bare.isEmpty(), "every text given to a widget or a message goes through tx() (" +
                                  QString::number(bare.size()) + " don't" +
                                  (bare.isEmpty() ? QString(")") : "):\n  " + bare.mid(0, 40).join("\n  ")));
    }

    // -- QStorageInfo asks drives for their size (statfs), which waits on a busy tape or a floppy: it's used only in the
    // Overview's drive scan, which runs on a worker; everything else goes through drives::Sizes
    {
        QStringList found;
        QDir src(QString(TESTS_DIR) + "/../src");
        for (const QString &f : src.entryList({"*.cpp", "*.h"}, QDir::Files)) {
            if (f == "overview.cpp")
                continue;
            int n = 0;
            for (const QString &line : QString::fromUtf8(read_file(src.filePath(f))).split('\n')) {
                ++n;
                QString code = line.section("//", 0, 0);
                if (code.contains("QStorageInfo"))
                    found << QString("%1:%2: %3").arg(f).arg(n).arg(line.trimmed());
            }
        }
        check(found.isEmpty(), "no drive is asked for its size on the UI thread (QStorageInfo only in the Overview's "
                               "background scan)" + (found.isEmpty() ? QString() : ":\n  " + found.join("\n  ")));
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

    // -- the Jobs window: each list with its own controls. The job buttons right under the jobs list, above the
    // thumbnail jobs; the thumbnail ones (and how thumbnailing runs) under the thumbnail list
    {
        JobsWindow *jw = JobsWindow::open(w);
        jw->resize(1000, 600);
        spin(200);
        auto top = [jw](QWidget *x) { return x->mapTo(jw, QPoint(0, 0)).y(); };
        auto bottom = [jw, top](QWidget *x) { return top(x) + x->height(); };
        bool jobs_ok = true, thumbs_ok = true;
        for (QWidget *b : std::initializer_list<QWidget *>{jw->pause_btn, jw->stop_btn, jw->now_btn, jw->up_btn, jw->down_btn})
            jobs_ok = jobs_ok && top(b) >= bottom(jw->list) && bottom(b) <= top(jw->thumb_list);
        for (QWidget *b : std::initializer_list<QWidget *>{jw->thumb_pause, jw->thumb_stop, jw->thumb_mode})
            thumbs_ok = thumbs_ok && top(b) >= bottom(jw->thumb_list);
        check(jobs_ok && thumbs_ok, QString("the Jobs window's job buttons are right under the jobs list, the thumbnail "
                                            "jobs' controls under the thumbnail list") +
                                        (jobs_ok ? "" : " (the job buttons aren't)") + (thumbs_ok ? "" : " (the thumbnail ones aren't)"));
        jw->close();
    }

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
