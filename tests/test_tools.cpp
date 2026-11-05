// The file tools: Find Duplicates (dupes.h), Disk Usage (diskusage.h), Compare With… and Sync To… (compare.h), and
// Verify Signature (signature.h).
#include "common.h"

#include "compare.h"
#include "diskimage.h"
#include "drives.h"
#include "diskusage.h"
#include "dupes.h"
#include "overview.h"
#include "pgp.h"
#include "prefs.h"
#include "ramdisk.h"
#include "ramdiskwin.h"
#include "hashcheck.h"
#include "signature.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QToolButton>
#include <QTreeWidget>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <signal.h>
#include <unistd.h>

#include <thread>

using namespace test;

static void set_mtime(const QString &path, qint64 secs)
{
    struct timespec ts[2] = {{secs, 0}, {secs, 0}};
    ::utimensat(AT_FDCWD, enc(path).constData(), ts, AT_SYMLINK_NOFOLLOW);
}

static ino_t inode(const QString &path)
{
    struct stat st;
    return lstat_(path, st) ? st.st_ino : 0;
}

static QStringList names_of(const QList<dupes::Group> &groups, const QString &root)
{
    QStringList out;
    for (const dupes::Group &g : groups) {
        QStringList files;
        for (const dupes::File &f : g.files)   // (a.bin and a-link.bin are one file: either name may be listed)
            files << relpath(f.path, root).replace("a-link.bin", "a.bin");
        files.sort();
        out << files.join(" = ");
    }
    out.sort();
    return out;
}

static QStringList menu_texts(MainWindow *w, const QStringList &paths)
{
    QMenu *m = w->build_menu(w->pane(), paths);
    QStringList out;
    for (QAction *a : m->actions())
        out << a->text();
    delete m;
    return out;
}

static QAction *menu_action(MainWindow *w, const QStringList &paths, const QString &text, QMenu **keep)
{
    *keep = w->build_menu(w->pane(), paths);
    for (QAction *a : (*keep)->actions())
        if (a->text() == text)
            return a;
    return nullptr;
}

static bool click_answer(QMessageBox::StandardButton which)
{
    QMessageBox *box = nullptr;
    if (!wait_for([&]() { return (box = shown_window<QMessageBox>()) != nullptr; }, 5000))
        return false;
    QAbstractButton *b = box->button(which);
    if (!b)
        return false;
    b->click();
    spin(50);
    return true;
}

static proc::Result gpg(const QStringList &args, const QString &home = QString())
{
    proc::Options o;
    o.c_utf8 = true;
    if (!home.isEmpty())
        o.env << "GNUPGHOME=" + home;
    return proc::run(QStringList{"gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase", ""} + args, 120000, o);
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    setup_app();
    QString home = HOME();
    MainWindow *w = open_window({home});
    wait_for([&]() { return w->pane() && w->pane()->path == home; });

    // ================================================================ Find Duplicates
    {
        QString d = home_path("dupes");
        makedirs(join(d, "sub"), true);
        makedirs(join(d, ".hid"), true);
        QByteArray big(200 * 1024, 'x');
        big[150000] = 'A';
        QByteArray big2 = big;
        big2[150000] = 'B';   // the same size and beginning, other contents further on
        write_text(join(d, "a.bin"), big);
        write_text(join(d, "sub/b.bin"), big);
        write_text(join(d, "c.bin"), big);
        write_text(join(d, "d.bin"), big2);
        write_text(join(d, "small1.txt"), "same");
        write_text(join(d, "small2.txt"), "same");
        write_text(join(d, ".hidden.bin"), big);
        write_text(join(d, ".hid/e.bin"), big);
        ::link(enc(join(d, "a.bin")).constData(), enc(join(d, "a-link.bin")).constData());
        ::symlink("a.bin", enc(join(d, "a-sym.bin")).constData());
        set_mtime(join(d, "a.bin"), 1600000000);   // (a-link.bin too: the same file)
        set_mtime(join(d, "sub/b.bin"), 1500000000);
        set_mtime(join(d, "c.bin"), 1700000000);

        dupes::Options o;
        QStringList got = names_of(dupes::find({d}, o), d);
        check(got == QStringList({"a.bin = c.bin = sub/b.bin", "small1.txt = small2.txt"}),
              "Find Duplicates: files with the same contents in groups, through subfolders; not a file of the same size "
              "and beginning that differs later, a symlink, hidden files, or a hard link to one already listed (" +
                  got.join("; ") + ")");
        o.min_size = 1024;
        o.hidden = true;
        got = names_of(dupes::find({d}, o), d);
        check(got == QStringList({".hid/e.bin = .hidden.bin = a.bin = c.bin = sub/b.bin"}),
              "...a minimum size leaves small files out, and hidden files and folders can be included (" + got.join("; ") + ")");
        o.hidden = false;
        o.skip_hardlinks = false;
        o.recursive = false;
        QList<dupes::Group> groups = dupes::find({d}, o);
        got = names_of(groups, d);
        check(got == QStringList({"a.bin = a.bin = c.bin"}) && dupes::freed(groups) == big.size(),
              "...hard links listed when asked (freeing nothing: one file), and not into subfolders when asked (" +
                  got.join("; ") + ", " + QString::number(dupes::freed(groups)) + " bytes freed)");

        o = dupes::Options();
        groups = dupes::find({d}, o);
        dupes::Group g;
        for (const dupes::Group &x : groups)
            if (x.size == big.size())
                g = x;
        auto kept = [&](const QList<bool> &k) {
            QStringList out;
            for (int i = 0; i < g.files.size(); ++i)
                if (k.value(i))
                    out << relpath(g.files[i].path, d).replace("a-link.bin", "a.bin");
            return out.join(" ");
        };
        check(g.files.size() == 3 && relpath(g.files.first().path, d) == "sub/b.bin" &&
                  kept(dupes::keep_oldest(g)) == "sub/b.bin" && kept(dupes::keep_newest(g)) == "c.bin" &&
                  kept(dupes::keep_in(g, join(d, "sub"))) == "sub/b.bin" &&
                  kept(dupes::keep_in(g, home_path("elsewhere"))) == "sub/b.bin a.bin c.bin" &&
                  dupes::freed(g, dupes::keep_oldest(g)) == 2 * big.size(),
              "...a group lists the oldest first; keeping the oldest, the newest, or those in a folder (none there: all)");

        // hard links: each replaced by a link to the one kept, whole; one that can't be is left as it was
        QString l = home_path("dupes-link");
        makedirs(join(l, "locked"), true);
        for (const QString &n : {"k.bin", "x.bin", "locked/y.bin"})
            write_text(join(l, n), big);
        QList<dupes::Group> lg = dupes::find({l}, dupes::Options());
        dupes::File keep, x, y;
        for (const dupes::File &f : lg.value(0).files)
            (f.path.endsWith("k.bin") ? keep : f.path.endsWith("x.bin") ? x : y) = f;
        ::chmod(enc(join(l, "locked")).constData(), 0555);   // its folder takes no new name: the link can't be made
        QStringList replaced;
        QStringList errors = dupes::link_over(keep, {x, y}, &replaced);
        ::chmod(enc(join(l, "locked")).constData(), 0755);
        QStringList left_in_locked = listdir(join(l, "locked"));
        check(lg.size() == 1 && inode(join(l, "x.bin")) == inode(join(l, "k.bin")) && replaced == QStringList{x.path} &&
                  errors.size() == 1 && errors.first().startsWith(y.path) && inode(y.path) != inode(keep.path) &&
                  read_file(y.path) == big && left_in_locked == QStringList{"y.bin"},
              "Replace with Hard Links: a duplicate becomes a link to the copy kept; one whose folder takes no new name "
              "fails part-way and is left whole, with no part file (" + errors.join("; ") + ")");
        write_text(join(l, "k.bin"), big2);   // changed since the search: nothing is done
        x = dupes::File{join(l, "z.bin"), big.size(), 0, 0, 0};
        errors = dupes::link_over(keep, {x});
        check(errors.size() == 1 && errors.first().contains("changed since"),
              "...nothing is replaced when the copy kept has changed since the search");

        // the window, from a folder's right-click menu: search, keep the oldest, move the others to the trash
        QString t = home_path("dupes-win");
        makedirs(join(t, "in"), true);
        write_text(join(t, "one.bin"), big);
        write_text(join(t, "in/two.bin"), big);
        write_text(join(t, "three.bin"), big);
        set_mtime(join(t, "in/two.bin"), 1500000000);
        makedirs(join(TRASH_DIR(), "files"), true);
        makedirs(join(TRASH_DIR(), "info"), true);
        QMenu *keep_menu = nullptr;
        QAction *find_act = menu_action(w, {t}, "Find Duplicates…", &keep_menu);
        if (find_act)
            find_act->trigger();
        DupesWindow *dw = nullptr;
        bool shown = wait_for([&]() { return (dw = shown_window<DupesWindow>()) != nullptr; });
        bool found = shown && wait_for([&]() { return !dw->searching() && dw->groups.size() == 1; }, 20000);
        check(find_act && shown && is_free_window(dw) && found && dw->tree->topLevelItemCount() == 1 &&
                  dw->tree->topLevelItem(0)->childCount() == 3 && dw->summary->text().contains("can be freed"),
              "Find Duplicates… in a folder's menu opens a free window that searches and lists the group of 3 identical files");
        delete keep_menu;
        if (found) {
            dw->set_keep(dupes::keep_oldest);
            bool counted = dw->summary->text().contains("Not kept: 2 files");
            dw->trash_others();
            bool trashed = wait_for([&]() {
                return exists(join(t, "in/two.bin")) && !exists(join(t, "one.bin")) && !exists(join(t, "three.bin")) &&
                       exists(join(TRASH_DIR(), "files/one.bin"));
            }, 10000);
            check(counted && trashed && dw->groups.isEmpty() && wait_for([]() { return undo::label() == "Move to Trash"; }),
                  "...keeping the oldest and moving the others to the trash leaves the oldest, and can be undone");
            dw->close();
        }
        check(menu_texts(w, {join(t, "in/two.bin")}).contains("Find Duplicates…") == false &&
                  menu_texts(w, {join(t, "in"), d}).contains("Find Duplicates…"),
              "Find Duplicates… is in the menu of a folder or several folders, not of a file");
    }

    // ================================================================ Disk Usage
    {
        using diskusage::Node;
        QString d = home_path("du");
        makedirs(join(d, "sub/deep"), true);
        makedirs(join(d, "tiny"), true);
        makedirs(join(d, "locked"), true);
        write_text(join(d, "big.bin"), QByteArray(300 * 1024, 'b'));
        write_text(join(d, "sub/a.bin"), QByteArray(200 * 1024, 'a'));
        write_text(join(d, "sub/deep/c.bin"), QByteArray(150 * 1024, 'c'));
        write_text(join(d, "tiny/t.txt"), "tiny");
        write_text(join(d, "locked/secret.bin"), QByteArray(1000, 's'));
        for (int i = 0; i < diskusage::FILES_KEPT + 10; ++i)   // more than are kept one by one, and too small anyway
            write_text(join(d, QString("small%1.txt").arg(i)), QByteArray(100, 'x'));
        ::symlink("big.bin", enc(join(d, "link")).constData());
        ::chmod(enc(join(d, "locked")).constData(), 0);
        QStringList unreadable;
        diskusage::Tree t = diskusage::count(d, nullptr, 0, &unreadable);
        fileops::DirStats want = fileops::dir_stats(d);
        ::chmod(enc(join(d, "locked")).constData(), 0755);
        QStringList order;
        for (const auto &k : t->kids)
            order << (k->rest ? QString("rest:%1").arg(k->files) : k->name);
        Node *sub = diskusage::find(t.get(), join(d, "sub")), *tiny = diskusage::find(t.get(), join(d, "tiny"));
        Node *deep = diskusage::find(t.get(), join(d, "sub/deep/c.bin"));
        check(t->size == want.size && t->files == want.files && t->dirs == want.dirs && t->complete,
              QString("Disk Usage: a folder's size, files and folders counted as Properties counts them, a folder that "
                      "can't be read as empty (%1 bytes, %2 files, %3 folders; Properties: %4, %5, %6)")
                  .arg(t->size).arg(t->files).arg(t->dirs).arg(want.size).arg(want.files).arg(want.dirs));
        check(order.mid(0, 2) == QStringList({"sub", "big.bin"}) && order.contains("rest:31") && sub &&
                  sub->size == 350 * 1024 && deep && deep->path() == join(d, "sub/deep/c.bin") &&
                  unreadable == QStringList{join(d, "locked")},
              "...biggest first, the small files of a folder together as one block, the folder that couldn't be read "
              "named (" + order.join(" ") + "; " + unreadable.join(", ") + ")");
        check(tiny && tiny->pruned && tiny->kids.empty() && tiny->size == 4 && !tiny->can_enter(),
              "...a small folder keeps only its totals (a block, not gone into)");
        diskusage::remove(diskusage::find(t.get(), join(d, "sub/deep")));
        check(t->size == want.size - 150 * 1024 && sub->size == 200 * 1024 && sub->dirs == 0 &&
                  !diskusage::find(t.get(), join(d, "sub/deep/c.bin")),
              "...taking a folder out (moved to the trash) makes the folders above it smaller");

        // the treemap: every block in the area, as big as its share, none overlapping
        QList<double> sizes{6, 6, 4, 3, 2, 2, 1};
        QRectF area(10, 20, 600, 400);
        QList<QRectF> rects = diskusage::squarify(sizes, area);
        bool in = rects.size() == sizes.size(), sized = true, apart = true;
        double worst_aspect = 0;
        for (int i = 0; i < rects.size(); ++i) {
            in = in && area.adjusted(-1e-6, -1e-6, 1e-6, 1e-6).contains(rects[i]);
            sized = sized && std::abs(rects[i].width() * rects[i].height() - sizes[i] / 24 * 600 * 400) < 1e-3;
            worst_aspect = std::max(worst_aspect, std::max(rects[i].width() / rects[i].height(), rects[i].height() / rects[i].width()));
            for (int j = 0; j < i; ++j)
                apart = apart && rects[i].intersected(rects[j]).width() * rects[i].intersected(rects[j]).height() < 1e-6;
        }
        check(in && sized && apart && worst_aspect < 3,
              QString("...the treemap: each block inside the map, sized by its share, none overlapping, none long and "
                      "thin (the longest side %1 times the shortest)").arg(worst_aspect));

        // the window, from a folder's right-click menu: it counts, then remembers what it counted for Properties
        check(menu_texts(w, {d}).contains("Disk Usage…") && !menu_texts(w, {join(d, "big.bin")}).contains("Disk Usage…") &&
                  !menu_texts(w, {d, join(d, "sub")}).contains("Disk Usage…"),
              "Disk Usage… is in a folder's right-click menu, not a file's or several folders'");
        QMenu *keep_menu = nullptr;
        QAction *du_act = menu_action(w, {d}, "Disk Usage…", &keep_menu);
        if (du_act)
            du_act->trigger();
        DiskUsageWindow *dw = nullptr;
        bool shown = wait_for([&]() { return (dw = shown_window<DiskUsageWindow>()) != nullptr; });
        bool counted = shown && wait_for([&]() { return !dw->counting() && dw->tree && !dw->tree->remembered; }, 20000);
        delete keep_menu;
        auto sub_size = fileops::dirsizes::get(join(d, "sub"));
        auto all_size = fileops::dirsizes::get(d);
        check(du_act && shown && is_free_window(dw) && counted && dw->map->shown == dw->tree.get() &&
                  dw->list->topLevelItemCount() == int(dw->tree->kids.size()) && sub_size &&
                  sub_size->stats.size == fileops::dir_stats(join(d, "sub")).size && all_size &&
                  all_size->stats.files == fileops::dir_stats(d).files,
              "...it opens a free window that counts the folder, maps it, lists it, and remembers its folders' sizes "
              "for Properties");
        if (counted) {
            Node *s = diskusage::find(dw->tree.get(), join(d, "sub"));
            dw->go(s);
            bool into = dw->map->shown == s && dw->back_btn->isEnabled() && dw->where->text().contains("href");
            dw->up();
            check(into && dw->map->shown == dw->tree.get() && !dw->back_btn->isEnabled(),
                  "...going into a folder and Back up again");
            QStringList menu;
            if (QMenu *m = dw->item_menu(diskusage::find(dw->tree.get(), join(d, "big.bin")))) {
                for (QAction *a : m->actions())
                    if (!a->isSeparator())
                        menu << a->text();
                delete m;
            }
            check(menu == QStringList({"Open", "Show in Folder", "Move to Trash", "Properties"}),
                  "...a file's right-click menu there: Open, Show in Folder, Move to Trash, Properties (" + menu.join(", ") + ")");
            makedirs(join(TRASH_DIR(), "files"), true);
            makedirs(join(TRASH_DIR(), "info"), true);
            qint64 before = dw->tree->size;
            dw->trash(diskusage::find(dw->tree.get(), join(d, "big.bin")));
            bool trashed = wait_for([&]() {
                return !exists(join(d, "big.bin")) && !diskusage::find(dw->tree.get(), join(d, "big.bin"));
            }, 10000);
            check(trashed && dw->tree->size == before - 300 * 1024 && wait_for([]() { return undo::label() == "Move to Trash"; }),
                  "...Move to Trash takes it off the map (the folder's size with it), and can be undone");
            dw->close();
        }

        // opened again: what was counted shows at once, from the sizes remembered, while it counts again (not the
        // folder the trashed file was in: its size changed, so it's forgotten)
        diskusage::Tree mem = diskusage::remembered(join(d, "sub"));
        Node *mdeep = mem ? diskusage::find(mem.get(), join(d, "sub/deep")) : nullptr;
        bool has_rest = false;
        if (mem)
            for (const auto &k : mem->kids)
                has_rest = has_rest || (k->rest && k->size == 200 * 1024);
        check(mem && mem->remembered && mem->size == 350 * 1024 && mdeep && mdeep->size == 150 * 1024 && has_rest &&
                  !fileops::dirsizes::get(d),
              "...the sizes remembered make a map at once (the files not remembered one by one as one block); the "
              "folder a file was trashed from is forgotten");

        // on a tape, it counts only when asked
        QString tape = home_path("du-tape");
        makedirs(tape, true);
        write_text(join(tape, "f.bin"), "on tape");
        qputenv("KESTREL_TEST_SLOW", QString("tape=%1").arg(tape).toUtf8());
        drives::reread_mounts();   // (the mount list read under a second ago, without the tape, would be used)
        diskusage::open_window(w, tape);
        DiskUsageWindow *tw = nullptr;
        bool tshown = wait_for([&]() {
            for (QWidget *x : QApplication::topLevelWidgets())
                if (auto *y = qobject_cast<DiskUsageWindow *>(x); y && y->isVisible() && y->windowTitle().contains("du-tape"))
                    tw = y;
            return tw != nullptr;
        });
        spin(300);
        bool waited = tshown && !tw->counting() && !tw->tree && tw->recount_btn->isEnabled();
        if (tshown)
            tw->recount();
        bool counted_when_asked = tshown && wait_for([&]() { return !tw->counting() && tw->tree; }, 20000);
        qunsetenv("KESTREL_TEST_SLOW");
        drives::reread_mounts();
        check(waited && counted_when_asked, "...on a tape it doesn't count until asked (Count Now), then counts");
        if (tw)
            tw->close();

        // a drive's Overview card: its button beside the usage bar opens Disk Usage
        DriveInfo info;
        info.name = "Drive";
        info.root = "/";
        info.total = 1000;
        info.free = 400;
        DriveCard card(info);
        QString asked;
        QObject::connect(&card, &DriveCard::disk_usage, [&]() { asked = "yes"; });
        if (card.usage_button)
            card.usage_button->click();
        DriveInfo phone = info;
        phone.kind = "phone";
        DriveCard phone_card(phone);
        check(card.usage_button && asked == "yes" && !phone_card.usage_button,
              "a drive's Overview card has “What's using this space?” beside its usage, which opens Disk Usage (not "
              "a phone's)");
    }

    // ================================================================ Compare
    {
        QString L = home_path("cmp/left"), R = home_path("cmp/right");
        makedirs(join(L, "both"), true);
        makedirs(join(R, "both"), true);
        makedirs(join(L, "onlydir/inner"), true);
        write_text(join(L, "onlydir/inner/f.txt"), "f");
        write_text(join(L, "only-left.txt"), "l");
        write_text(join(R, "only-right.txt"), "r");
        write_text(join(L, "both/same.txt"), "same");
        write_text(join(R, "both/same.txt"), "same");
        write_text(join(L, "newer.txt"), "new");
        write_text(join(R, "newer.txt"), "old");
        write_text(join(L, "older.txt"), "old");
        write_text(join(R, "older.txt"), "new!");
        write_text(join(L, "size.txt"), "abc");
        write_text(join(R, "size.txt"), "abcd");
        write_text(join(L, "sneaky.txt"), "aaaa");   // same size and date, other bytes: only a deep compare sees it
        write_text(join(R, "sneaky.txt"), "bbbb");
        write_text(join(L, "type"), "a file");
        makedirs(join(R, "type"), true);
        write_text(join(L, ".kes-abc.part"), "half");   // Kestrel's unfinished piece: not part of the folder
        for (const QString &n : {"both/same.txt", "size.txt", "sneaky.txt"}) {
            set_mtime(join(L, n), 1600000000);
            set_mtime(join(R, n), 1600000000);
        }
        set_mtime(join(L, "newer.txt"), 1700000000);
        set_mtime(join(R, "newer.txt"), 1600000000);
        set_mtime(join(L, "older.txt"), 1600000000);
        set_mtime(join(R, "older.txt"), 1700000000);
        auto kinds = [](const QList<compare::Item> &items) {
            QStringList out;
            for (const compare::Item &it : items)
                out << it.rel + ":" + QString::number(it.kind);
            return out.join(" ");
        };
        using namespace compare;
        QString want = QString("both/same.txt:%1 newer.txt:%2 older.txt:%3 only-left.txt:%4 only-right.txt:%5 onlydir:%4 "
                               "size.txt:%6 sneaky.txt:%1 type:%6")
                           .arg(SAME).arg(LEFT_NEWER).arg(RIGHT_NEWER).arg(ONLY_LEFT).arg(ONLY_RIGHT).arg(DIFFERENT);
        QString got = kinds(diff(L, R));
        check(got == want, "Compare: only on one side (a folder as one item), newer on either side, other contents, and "
                           "the same, through subfolders (" + got + ")");
        got = kinds(diff(L, R, true));
        check(got.contains(QString("sneaky.txt:%1").arg(DIFFERENT)) && got.contains(QString("both/same.txt:%1").arg(SAME)),
              "...a deep compare reads the files: the same size and date with other bytes differ");
        check(same_time(100, 101) && !same_time(100, 102), "...dates a second apart count as the same (FAT's rounding)");

        // the window: the right folder chosen from the folder's menu, then Copy to Right replaces after asking
        QMenu *keep_menu = nullptr;
        QAction *cmp = menu_action(w, {L}, "Compare With…", &keep_menu);   // (it asks for the other folder first)
        auto *cw = new CompareWindow(w, L, R);
        show_free(cw);
        cw->run();
        bool listed = wait_for([&]() { return !cw->running() && !cw->items.isEmpty(); }, 10000);
        int rows = cw->tree->topLevelItemCount();
        check(cmp && listed && is_free_window(cw) && rows == 7 && cw->summary->text().contains("difference"),
              QString("Compare With… is in a folder's menu; the window lists the differences, not what's the same (%1 rows)").arg(rows));
        delete keep_menu;
        for (int i = 0; i < cw->tree->topLevelItemCount(); ++i)
            cw->tree->topLevelItem(i)->setSelected(cw->tree->topLevelItem(i)->text(0) == "newer.txt" ||
                                                    cw->tree->topLevelItem(i)->text(0) == "only-left.txt");
        cw->copy_across(true);
        bool asked = click_answer(QMessageBox::Yes);
        bool copied = wait_for([&]() { return read_file(join(R, "newer.txt")) == "new" && exists(join(R, "only-left.txt")); }, 10000);
        bool again = wait_for([&]() { return !cw->running() && cw->tree->topLevelItemCount() == 5; }, 10000);
        check(asked && copied && again, "...Copy to Right copies the selected items across (asking before it replaces one), "
                                        "and the list is compared again");
        cw->close();
    }

    // ================================================================ Sync
    {
        QString S = home_path("sync/src"), D = home_path("sync/dst");
        makedirs(join(S, "sub"), true);
        makedirs(join(D, "sub"), true);
        write_text(join(S, "new.txt"), "new");
        write_text(join(S, "sub/changed.txt"), "version 2");
        write_text(join(D, "sub/changed.txt"), "version 1");
        write_text(join(S, "same.txt"), "same");
        write_text(join(D, "same.txt"), "same");
        write_text(join(D, "extra.txt"), "only here");
        set_mtime(join(S, "same.txt"), 1600000000);
        set_mtime(join(D, "same.txt"), 1600000000);
        set_mtime(join(S, "sub/changed.txt"), 1700000000);
        set_mtime(join(D, "sub/changed.txt"), 1600000000);
        QList<compare::Step> p = compare::plan(compare::diff(S, D), true);
        QStringList steps;
        for (const compare::Step &s : p)
            steps << QString::number(s.op) + ":" + s.rel;
        check(steps == QStringList({"0:new.txt", "1:sub/changed.txt", "2:extra.txt"}) &&
                  compare::plan(compare::diff(S, D), false).size() == 2,
              "Sync's plan: copy what's new, replace what changed, and delete what's only on the destination only when "
              "asked, after the copies (" + steps.join(" ") + ")");

        QMenu *keep_menu = nullptr;
        QAction *sync_act = menu_action(w, {S}, "Sync To…", &keep_menu);
        if (sync_act)
            sync_act->trigger();
        SyncWindow *sw = nullptr;
        bool opened = wait_for([&]() { return (sw = shown_window<SyncWindow>()) != nullptr; });
        delete keep_menu;
        check(opened && is_free_window(sw) && sw->source_edit->text() == S && !sw->deletes->isChecked(),
              "Sync To… in a folder's menu opens a free window from that folder, deleting off");
        if (opened) {
            sw->dest_edit->setText(D);
            sw->use_kestrel->setChecked(true);
            bool previewed = false;
            QObject::connect(sw, &SyncWindow::previewed, [&]() { previewed = true; });
            sw->preview();
            wait_for([&]() { return previewed; }, 10000);
            check(previewed && sw->list->topLevelItemCount() == 2 && sw->summary->text().contains("1 to copy, 1 to replace"),
                  "...the preview lists exactly what will be copied and replaced (" + sw->summary->text() + ")");
            bool synced = false;
            QObject::connect(sw, &SyncWindow::synced, [&]() { synced = true; });
            sw->sync_now();
            wait_for([&]() { return synced; }, 15000);
            check(synced && read_file(join(D, "new.txt")) == "new" && read_file(join(D, "sub/changed.txt")) == "version 2" &&
                      exists(join(D, "extra.txt")),
                  "...with Kestrel's copy, the sync copies and replaces, and keeps what's only on the destination");
            // deleting: asked again, and nothing goes until it's answered
            previewed = synced = false;
            sw->deletes->setChecked(true);
            wait_for([&]() { return previewed; }, 10000);
            sw->sync_now();
            bool question = wait_for([]() { return shown_window<QMessageBox>() != nullptr; }, 5000);
            spin(300);
            bool still = exists(join(D, "extra.txt"));
            click_answer(QMessageBox::Yes);
            wait_for([&]() { return synced; }, 15000);
            QList<compare::Item> left = compare::diff(S, D);
            bool all_same = std::all_of(left.begin(), left.end(), [](const compare::Item &i) { return i.kind == compare::SAME; });
            check(question && still && synced && !exists(join(D, "extra.txt")) && all_same,
                  "...deleting on the destination asks again, deletes nothing before the answer, and then the folders match");

            // saved pairs: kept in sync-pairs.json, and run again in one click
            sw->save_pair("Test pair");
            QList<compare::Pair> saved = compare::pairs();
            write_text(join(S, "later.txt"), "later");
            synced = false;
            sw->run_pair(0);
            bool ran = wait_for([&]() { return synced; }, 15000);
            check(saved.size() == 1 && saved.first().name == "Test pair" && saved.first().source == S &&
                      saved.first().dest == D && saved.first().deletes && exists(compare::pairs_file()) && ran &&
                      read_file(join(D, "later.txt")) == "later",
                  "...a saved sync pair is kept (sync-pairs.json) and runs again in one click");
            check(compare::resolve(S, QString(), QString()) == S && compare::resolve(home_path("gone"), "1234-ABCD", "x") ==
                                                                        home_path("gone"),
                  "...a saved folder without a file system UUID is used as saved, as is one whose drive isn't there");
            sw->close();
        }

        // remembered syncs: each sync that ran is kept (sync-history.json), and Sync To… on either folder fills itself in
        spin(100);
        QList<compare::Pair> past = compare::history();
        check(opened && past.size() == 1 && past.first().source == S && past.first().dest == D && past.first().deletes &&
                  past.first().when > 0 && compare::history_for(S).size() == 1 && compare::history_for(D + "/").size() == 1 &&
                  compare::history_for(home_path("sync")).isEmpty() && exists(compare::history_file()),
              "Sync To… remembers each sync that ran, unnamed, once per pair (sync-history.json), found from either "
              "folder and only from them");
        // opened on folder: the window once it has looked up its past syncs
        auto open_on = [&](const QString &folder) -> SyncWindow * {
            compare::open_sync(w, folder);
            SyncWindow *x = nullptr;
            if (!wait_for([&]() { return (x = shown_window<SyncWindow>()) != nullptr; }))
                return nullptr;
            bool found = false;
            QObject::connect(x, &SyncWindow::past_found, [&]() { found = true; });
            return wait_for([&]() { return found; }, 10000) ? x : nullptr;
        };
        auto close_it = [](SyncWindow *x) {
            if (x)
                x->close();
            spin(100);
        };
        SyncWindow *from_src = open_on(S);
        check(from_src && from_src->source_edit->text() == S && from_src->dest_edit->text() == D &&
                  from_src->deletes->isChecked() && from_src->use_kestrel->isChecked() && !from_src->past_note->isHidden() &&
                  !from_src->recent_row->isHidden() && from_src->recent->count() == 1,
              "...opened on the source again, it's filled in as last run: the destination, deleting, Kestrel's copy, and "
              "says it was filled in");
        close_it(from_src);
        SyncWindow *from_dst = open_on(D);
        check(from_dst && from_dst->source_edit->text() == S && from_dst->dest_edit->text() == D &&
                  from_dst->past_note->text().contains("isn't there") == false,
              "...opened on the destination, the right way round: that folder as To:, the source as From: (not swapped)");
        close_it(from_dst);
        QString other = home_path("sync/other"), gone = home_path("sync/gone");
        makedirs(other, true);
        SyncWindow *never = open_on(other);
        check(never && never->source_edit->text() == other && never->dest_edit->text().isEmpty() &&
                  never->recent_row->isHidden() && never->past_note->isHidden(),
              "...a folder never synced opens as before: it as the source, the destination to choose");
        close_it(never);
        // the latest partner is gone: filled in, but said; the older ones a click away
        makedirs(gone, true);
        compare::remember(S, gone, false, true);
        rmtree(gone);
        SyncWindow *missing = open_on(S);
        bool said = missing && missing->dest_edit->text() == gone && missing->past_note->text().contains(gone) &&
                    missing->past_note->text().contains("isn't there now") && !missing->deletes->isChecked() &&
                    missing->recent->count() == 2;
        if (missing)
            missing->use_past(1);
        check(said && missing->dest_edit->text() == D && missing->deletes->isChecked() &&
                  !missing->past_note->text().contains("isn't there now"),
              "...a remembered folder that isn't there now is filled in with a warning naming it, and the sync before "
              "it is under Last synced, a click away");
        close_it(missing);
        // folders changed by hand before the lookup ends are kept (changed at once: the lookup's answer needs the event loop)
        compare::open_sync(w, S);
        SyncWindow *typed = shown_window<SyncWindow>();
        bool typed_found = false;
        if (typed) {
            QObject::connect(typed, &SyncWindow::past_found, [&]() { typed_found = true; });
            typed->dest_edit->setText(other);
            wait_for([&]() { return typed_found; }, 10000);
        }
        check(typed && typed_found && typed->dest_edit->text() == other && typed->recent->count() == 2,
              "...a destination chosen before the past syncs are found is kept, not overwritten");
        close_it(typed);
        // the last few per folder; the same pair run again moves to the top, not listed twice
        QString many = home_path("sync/many");
        for (int i = 0; i < compare::HISTORY_PER_FOLDER + 2; ++i)
            compare::remember(many, join(many + "-to", QString::number(i)), false, true, 1700000000 + i);
        compare::remember(many, join(many + "-to", "4"), true, true, 1800000000);
        QList<compare::Pair> kept = compare::history_for(many);
        check(kept.size() == compare::HISTORY_PER_FOLDER && kept.first().dest == join(many + "-to", "4") &&
                  kept.first().deletes && kept.value(1).dest == join(many + "-to", "6") &&
                  std::count_if(kept.begin(), kept.end(), [&](const compare::Pair &x) { return x.dest.endsWith("-to/4"); }) == 1 &&
                  compare::history_for(S).size() == 2 && compare::pairs().size() == 1,
              "...only the last " + QString::number(compare::HISTORY_PER_FOLDER) + " per folder are kept, newest first; the same "
              "pair run again moves to the top; the saved pairs stay as they were");

        // rsync: the same, and a write that fails part-way leaves the destination's old file as it was
        if (compare::have_rsync()) {
            QString S2 = home_path("rsync/src"), D2 = home_path("rsync/dst");
            makedirs(join(S2, "sub"), true);
            makedirs(D2, true);
            write_text(join(S2, "sub/a.txt"), "a");
            QByteArray payload(3 * 1024 * 1024, 'p');
            for (int i = 0; i < payload.size(); i += 4096)
                payload[i] = char(i / 4096);
            write_text(join(S2, "big.bin"), payload);
            write_text(join(D2, "big.bin"), "the old version");
            struct rlimit old_lim, lim;
            ::getrlimit(RLIMIT_FSIZE, &old_lim);
            lim = old_lim;
            lim.rlim_cur = 1024 * 1024;
            auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
            ::setrlimit(RLIMIT_FSIZE, &lim);   // (rsync inherits it)
            QString error;
            QPointer<Task> t = fileops::queue_job(w, "rsync test", {S2, D2}, [&](Task *task) {
                try {
                    compare::run_rsync(task, S2, D2, false);
                } catch (const Error &e) {
                    error = e.message();
                }
                return QVariant(true);
            });
            wait_for([&]() { return !t; }, 30000);
            ::setrlimit(RLIMIT_FSIZE, &old_lim);
            ::signal(SIGXFSZ, old_handler);
            QStringList in_dst = listdir(D2);
            in_dst.removeAll(".kes-rsync-partial");
            in_dst.removeAll("sub");
            check(!error.isEmpty() && read_file(join(D2, "big.bin")) == "the old version" && in_dst == QStringList{"big.bin"},
                  "rsync: a sync that fails part-way (a file-size limit) leaves the destination's file as it was, and "
                  "nothing half-written under a real name (" + error + ")");
            t = fileops::queue_job(w, "rsync test 2", {S2, D2}, [&](Task *task) {
                compare::run_rsync(task, S2, D2, false);
                return QVariant(true);
            });
            wait_for([&]() { return !t; }, 30000);
            check(read_file(join(D2, "big.bin")) == payload && read_file(join(D2, "sub/a.txt")) == "a",
                  "...run again, it completes the sync");
        } else {
            skip("syncing with rsync (rsync isn't installed)");
        }
    }

    // ================================================================ Verify Signature
    {
        using signature::Result;
        Result r = signature::parse_status("[GNUPG:] NEWSIG\n[GNUPG:] GOODSIG 0123456789ABCDEF Kestrel Test <t@example.com>\n"
                                           "[GNUPG:] VALIDSIG AAAABBBBCCCCDDDDEEEEFFFF0000111122223333 2026-10-26 1793000000 0 4 0 22 10 00 "
                                           "AAAABBBBCCCCDDDDEEEEFFFF0000111122223333\n[GNUPG:] TRUST_UNDEFINED 0 pgp\n",
                                           0);
        Result missing = signature::parse_status("[GNUPG:] NEWSIG\n[GNUPG:] ERRSIG 0123456789ABCDEF 22 10 00 1793000000 9 "
                                                 "AAAABBBBCCCCDDDDEEEEFFFF0000111122223333\n[GNUPG:] NO_PUBKEY 0123456789ABCDEF\n",
                                                 2);
        Result bad = signature::parse_status("[GNUPG:] BADSIG 0123456789ABCDEF Kestrel Test\n", 1);
        check(r.status == Result::GOOD && !r.trusted && r.signer == "Kestrel Test <t@example.com>" && r.when == 1793000000 &&
                  r.fingerprint.size() == 40 && missing.status == Result::NO_KEY && missing.key_id == "0123456789ABCDEF" &&
                  missing.fingerprint.size() == 40 && bad.status == Result::BAD,
              "Verify Signature: gpg's status lines read as valid (signer, date, fingerprint, not trusted), a missing key "
              "(its ID and fingerprint) and a bad signature");
        r.file = "/x/disk.iso";
        check(signature::headline(r).contains("haven't verified") && signature::plain(r).contains("isn't") == false &&
                  signature::plain(r).contains("haven't verified that this key is really theirs") &&
                  signature::plain(missing).contains("Fetch Key"),
              "...said plainly: a valid signature from a key you haven't verified says so, a missing key offers to fetch it");

        if (signature::have_gpg()) {
            QString g1 = home_path(".gnupg"), g2 = home_path("gnupg2"), g3 = home_path("gnupg3");
            for (const QString &g : {g1, g2, g3}) {
                makedirs(g, true);
                ::chmod(enc(g).constData(), 0700);
            }
            proc::Result made = gpg({"--quick-gen-key", "Kestrel Test <test@example.com>", "ed25519", "sign", "never"});
            QString s = home_path("sig");
            makedirs(s, true);
            write_text(join(s, "release.tar"), "the release");
            gpg({"--detach-sign", "--armor", "-o", join(s, "release.tar.asc"), join(s, "release.tar")});
            write_text(join(s, "note.txt"), "a signed note\n");
            gpg({"--clearsign", "-o", join(s, "note.signed.asc"), join(s, "note.txt")});
            gpg({"--armor", "--export", "-o", join(s, "key.asc"), "test@example.com"});
            gpg({"--import", join(s, "key.asc")}, g2);
            Result own = signature::verify(join(s, "release.tar.asc"));
            Result clear = signature::verify(join(s, "note.signed.asc"));
            check(made.rc == 0 && own.status == Result::GOOD && own.trusted && own.signer.contains("Kestrel Test") &&
                      own.file == join(s, "release.tar") && own.when > 0 && clear.status == Result::GOOD &&
                      clear.file == join(s, "note.signed.asc"),
                  "...a detached signature is checked against the file it signs, a clearsigned file against itself: "
                  "valid, by whom, and your own key is trusted (" + signature::headline(own) + ")");
            qputenv("GNUPGHOME", g2.toUtf8());
            Result other = signature::verify(join(s, "release.tar.asc"));
            qputenv("GNUPGHOME", g3.toUtf8());
            Result none = signature::verify(join(s, "release.tar.asc"));
            qunsetenv("GNUPGHOME");
            check(other.status == Result::GOOD && !other.trusted && none.status == Result::NO_KEY && !none.key_id.isEmpty(),
                  "...with the key imported but not verified: valid, but not trusted; without the key: the key is "
                  "missing, with its ID (" + signature::headline(other) + "; " + signature::headline(none) + ")");
            write_text(join(s, "release.tar"), "the release, changed");
            Result changed = signature::verify(join(s, "release.tar.asc"));
            unlink(join(s, "release.tar"));
            Result gone = signature::verify(join(s, "release.tar.asc"));
            Result key = signature::verify(join(s, "key.asc"));
            check(changed.status == Result::BAD && gone.status == Result::MISSING_FILE && key.status == Result::NO_SIGNATURE,
                  "...a changed file is a bad signature; a missing one is said; a key isn't a signature");

            // opening: a signature shows the window; a signed checksum file is checked, then the files it lists
            w->open_paths(w->pane(), {join(s, "release.tar.asc")});
            SignatureWindow *sgw = nullptr;
            bool sig_shown = wait_for([&]() { return (sgw = shown_window<SignatureWindow>()) != nullptr; }, 10000);
            check(sig_shown && is_free_window(sgw) && sgw->head->text().contains("Bad signature") == false &&
                      sgw->head->text().contains("isn't here"),
                  "...opening a signature opens Verify Signature, in a free window (" + (sgw ? sgw->head->text() : QString()) + ")");
            if (sgw)
                sgw->close();
            spin(100);
            write_text(join(s, "data.bin"), "data");
            write_text(join(s, "SHA256SUMS"), (hashcheck::hash_file(join(s, "data.bin"), "sha256") + "  data.bin\n").toUtf8());
            gpg({"--detach-sign", "-o", join(s, "SHA256SUMS.gpg"), join(s, "SHA256SUMS")});
            w->open_paths(w->pane(), {join(s, "SHA256SUMS")});
            hashcheck::VerifyDialog *vd = nullptr;
            sig_shown = wait_for([&]() { return (sgw = shown_window<SignatureWindow>()) != nullptr; }, 10000);
            bool listed = wait_for([&]() { return (vd = shown_window<hashcheck::VerifyDialog>()) && vd->complete(); }, 10000);
            check(sig_shown && sgw->head->text().startsWith("✔") && listed && vd->results.value(0).status == "ok",
                  "...opening a signed SHA256SUMS checks its signature, then the files it lists");
            if (sgw)
                sgw->close();
            if (vd)
                vd->close();
            QStringList sig_menu = menu_texts(w, {join(s, "SHA256SUMS.gpg")});
            check(sig_menu.contains("Verify Signature") && !menu_texts(w, {join(s, "data.bin")}).contains("Verify Signature"),
                  "...Verify Signature is in a signature's right-click menu, not other files'");
        } else {
            skip("checking signatures with gpg (gpg isn't installed)");
        }
    }
    // ================================================================ Encrypt… / Decrypt (pgp.h)
    {
        // gpg's key listing: the keys that can encrypt (not a sign-only, revoked or disabled one), your own first, a
        // user ID's escaped ":" read back
        QByteArray pub = "tru:o:1:1793724092:1:3:1:5\n"
                         "pub:-:255:22:1111111111111111:1793724092:::-:::scESC:::::ed25519:::0:\n"
                         "fpr:::::::::AAAA0000000000000000000000001111111111111111:\n"
                         "uid:-::::1793724092::H1::Someone Else <else@example.com>::::::::::0:\n"
                         "sub:-:255:18:2222222222222222:1793724092::::::e:::::cv25519::\n"
                         "fpr:::::::::BBBB0000000000000000000000002222222222222222:\n"
                         "pub:u:255:22:3333333333333333:1793724092:::u:::scESC:::::ed25519:::0:\n"
                         "fpr:::::::::CCCC0000000000000000000000003333333333333333:\n"
                         "uid:r::::1793724092::H2::Old Name <old@example.com>::::::::::0:\n"
                         "uid:u::::1793724092::H3::Me\\x3a Myself <me@example.com>::::::::::0:\n"
                         "sub:u:255:18:4444444444444444:1793724092::::::e:::::cv25519::\n"
                         "pub:u:255:22:5555555555555555:1793724092:::u:::scSC:::::ed25519:::0:\n"
                         "fpr:::::::::DDDD0000000000000000000000005555555555555555:\n"
                         "uid:u::::1793724092::H4::Signing Only <s@example.com>::::::::::0:\n"
                         "pub:r:255:22:6666666666666666:1793724092:::-:::sc:::::ed25519:::0:\n"
                         "fpr:::::::::EEEE0000000000000000000000006666666666666666:\n"
                         "uid:r::::1793724092::H5::Revoked <r@example.com>::::::::::0:\n";
        QByteArray sec = "sec:u:255:22:3333333333333333:1793724092:::u:::scESC:::+::ed25519:::0:\n"
                         "fpr:::::::::CCCC0000000000000000000000003333333333333333:\n"
                         "ssb:u:255:18:4444444444444444:1793724092::::::e:::+::cv25519::\n"
                         "fpr:::::::::FFFF0000000000000000000000004444444444444444:\n";
        QStringList own_ids;
        QList<pgp::Key> ks = pgp::parse_keys(pub, sec, &own_ids);
        check(ks.size() == 2 && ks[0].own && ks[0].verified && ks[0].user_id == "Me: Myself <me@example.com>" &&
                  ks[0].fingerprint.endsWith("3333333333333333") && !ks[1].own && !ks[1].verified &&
                  ks[1].user_id == "Someone Else <else@example.com>" && own_ids.contains("4444444444444444"),
              "Encrypt…: gpg's key list read as the keys that can encrypt, yours first and verified, another's not; a "
              "sign-only and a revoked key left out");
        pgp::Packets sym = pgp::parse_packets(":symkey enc packet: version 4, cipher 9, aead 0, s2k 3, hash 10\n"
                                              "\tsalt 7EF82EF6CAF1B739, count 65011712 (255)\n:encrypted data packet:\n");
        pgp::Packets to = pgp::parse_packets(":pubkey enc packet: version 3, algo 18, keyid 4444444444444444\n"
                                             "\tdata: [263 bits]\n:encrypted data packet:\n\tlength: 70\n");
        pgp::Packets others = pgp::parse_packets(":pubkey enc packet: version 3, algo 18, keyid 2222222222222222\n");
        pgp::Packets signature_only = pgp::parse_packets(":signature packet: algo 22, keyid 1289F2A90176A555\n");
        check(pgp::way_for(sym, own_ids) == pgp::PASSWORD && pgp::way_for(to, own_ids) == pgp::OWN_KEY &&
                  pgp::way_for(others, own_ids) == pgp::OTHERS_KEYS && others.to_keys == QStringList{"2222222222222222"} &&
                  pgp::way_for(signature_only, own_ids) == pgp::NOT_ENCRYPTED,
              "Decrypt: what gpg lists in a file says how it opens: with a password, with your key, only with someone "
              "else's, or not encrypted (a signature)");
        check(pgp::decrypted_name("/x/notes.txt.gpg") == "notes.txt" && pgp::decrypted_name("/x/msg.asc") == "msg" &&
                  pgp::is_encrypted_name("/x/a.PGP") && !pgp::is_encrypted_name("/x/a.txt"),
              "...a decrypted copy is named without the .gpg / .pgp / .asc");

        if (pgp::have_gpg()) {
            QString g4 = home_path("gnupg4"), e = home_path("crypt");
            makedirs(g4, true);
            ::chmod(enc(g4).constData(), 0700);
            makedirs(e, true);
            proc::Result mine = gpg({"--quick-gen-key", "Kestrel Recipient <recipient@example.com>", "default", "default", "never"});
            gpg({"--quick-gen-key", "Someone Else <else@example.com>", "default", "default", "never"}, g4);
            gpg({"--armor", "--export", "-o", join(e, "else.key"), "else@example.com"}, g4);
            gpg({"--import", join(e, "else.key")});
            ::unlink(enc(join(e, "else.key")).constData());
            QList<pgp::Key> found = pgp::keys();
            QString my_fpr, else_fpr;
            for (const pgp::Key &k : found) {
                if (k.user_id.contains("recipient@"))
                    my_fpr = k.fingerprint;
                else if (k.user_id.contains("else@"))
                    else_fpr = k.fingerprint;
            }
            bool listed_ok = mine.rc == 0 && !my_fpr.isEmpty() && !else_fpr.isEmpty() &&
                             std::none_of(found.begin(), found.end(), [](const pgp::Key &k) { return k.user_id.contains("Kestrel Test"); }) &&
                             found.value(0).fingerprint == my_fpr && found.value(0).own;
            check(listed_ok, "...with gpg: your own key and an imported one are listed to encrypt to, yours first; a "
                             "sign-only key isn't");

            // each in a job, as the windows run them: what it returned, or the error
            auto in_job = [&](std::function<QString(Task *)> fn) {
                auto out = std::make_shared<QString>();
                QPointer<Task> t = fileops::queue_job(w, "pgp test", {e}, [fn, out](Task *task) {
                    try {
                        *out = fn(task);
                    } catch (const archive::WrongPassword &) {
                        *out = "WRONG";
                    } catch (const Error &err) {
                        *out = "ERROR " + err.message();
                    }
                    return QVariant(true);
                });
                wait_for([&]() { return !t; }, 60000);
                return *out;
            };
            auto no_parts = [&]() {
                QStringList l = listdir(e);
                return std::none_of(l.begin(), l.end(), [](const QString &n) { return n.endsWith(".part"); });
            };
            QByteArray secret = "the secret plans\n";
            write_text(join(e, "plans.txt"), secret);
            pgp::Spec by_pw;
            by_pw.password = "correct horse";
            QString made = in_job([&](Task *t) { return pgp::encrypt_file(t, join(e, "plans.txt"), by_pw); });
            pgp::Packets p = pgp::packets(made);
            check(made == join(e, "plans.txt.gpg") && exists(made) && !read_file(made).contains("secret plans") &&
                      read_file(join(e, "plans.txt")) == secret && pgp::way_for(p, {}) == pgp::PASSWORD,
                  "...Encrypt with a password makes plans.txt.gpg beside it (not readable), the original left as it is (" + made + ")");
            ::rename(enc(join(e, "plans.txt")).constData(), enc(join(e, "plans.orig")).constData());
            QString wrong = in_job([&](Task *t) { return pgp::decrypt_file(t, made, "wrong horse"); });
            bool nothing = !exists(join(e, "plans.txt")) && no_parts();
            QString right = in_job([&](Task *t) { return pgp::decrypt_file(t, made, "correct horse"); });
            check(wrong == "WRONG" && nothing && right == join(e, "plans.txt") && read_file(right) == secret,
                  "...Decrypt: a wrong password is said as such and leaves nothing; the right one makes plans.txt (" + wrong + ")");
            QString again = in_job([&](Task *t) { return pgp::decrypt_file(t, made, "correct horse"); });
            check(again == join(e, "plans (2).txt") && read_file(join(e, "plans.txt")) == secret,
                  "...decrypted again, it takes the next free name: nothing is replaced (" + again + ")");

            pgp::Spec to_me;
            to_me.recipients = {my_fpr};
            to_me.armor = true;
            QString asc = in_job([&](Task *t) { return pgp::encrypt_file(t, join(e, "plans.txt"), to_me); });
            QStringList own;
            pgp::parse_keys(QByteArray(), proc::run({"gpg", "--batch", "--with-colons", "--list-secret-keys"}).out, &own);
            bool to_own = pgp::way_for(pgp::packets(asc), own) == pgp::OWN_KEY && read_file(asc).startsWith("-----BEGIN PGP MESSAGE");
            ::unlink(enc(join(e, "plans.txt")).constData());
            QString opened = in_job([&](Task *t) { return pgp::decrypt_file(t, asc, QString()); });
            check(asc == join(e, "plans.txt.asc") && to_own && opened == join(e, "plans.txt") && read_file(opened) == secret,
                  "...Encrypt to your key as text makes plans.txt.asc, which Decrypt opens with your key (" + opened + ")");
            pgp::Spec to_else;
            to_else.recipients = {else_fpr};
            write_text(join(e, "for-else.txt"), "for someone else");
            QString theirs = in_job([&](Task *t) { return pgp::encrypt_file(t, join(e, "for-else.txt"), to_else); });
            QString cant = in_job([&](Task *t) { return pgp::decrypt_file(t, theirs, QString()); });
            check(pgp::way_for(pgp::packets(theirs), own) == pgp::OTHERS_KEYS && cant.startsWith("ERROR") &&
                      !exists(join(e, "for-else (2).txt")) && no_parts(),
                  "...encrypted to someone else's key: it's seen as theirs, and decrypting it fails leaving nothing (" + cant + ")");

            // failing part-way (a file-size limit): nothing under a real name, no part left
            QByteArray noise(3 * 1024 * 1024, 0);
            quint32 x = 12345;
            for (char &c : noise)
                c = char((x = x * 1103515245 + 12345) >> 24);   // (gpg can't compress it below the limit)
            write_text(join(e, "big.bin"), noise);
            struct rlimit old_lim, lim;
            ::getrlimit(RLIMIT_FSIZE, &old_lim);
            lim = old_lim;
            lim.rlim_cur = 1024 * 1024;
            auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
            ::setrlimit(RLIMIT_FSIZE, &lim);   // (gpg inherits it)
            QString failed = in_job([&](Task *t) { return pgp::encrypt_file(t, join(e, "big.bin"), by_pw); });
            ::setrlimit(RLIMIT_FSIZE, &old_lim);
            ::signal(SIGXFSZ, old_handler);
            check(failed.startsWith("ERROR") && !exists(join(e, "big.bin.gpg")) && no_parts() && read_file(join(e, "big.bin")) == noise,
                  "...an encryption that fails part-way (a file-size limit) leaves no file and no part behind (" + failed + ")");
            // a cancel: the same
            QPointer<Task> ct = fileops::queue_job(w, "pgp cancel", {e}, [&](Task *task) {
                task->cancel();
                return QVariant(pgp::encrypt_file(task, join(e, "big.bin"), by_pw));
            });
            wait_for([&]() { return !ct; }, 60000);
            check(!exists(join(e, "big.bin.gpg")) && no_parts(), "...and so does a cancelled one");

            // the menu, and opening an encrypted file: its password asked for, then decrypted beside it
            QStringList file_menu = menu_texts(w, {join(e, "for-else.txt")}), gpg_menu = menu_texts(w, {made});
            check(file_menu.contains("Encrypt…") && !file_menu.contains("Decrypt") && gpg_menu.contains("Decrypt") &&
                      !menu_texts(w, {e}).contains("Encrypt…") &&
                      menu_texts(w, {join(e, "for-else.txt"), join(e, "big.bin")}).contains("Encrypt…"),
                  "...Encrypt… is in files' right-click menus (one or several, not folders), Decrypt in encrypted files'");
            for (const QString &n : listdir(e))
                if (n.startsWith("plans") && n != "plans.txt.gpg" && n != "plans.orig")
                    ::unlink(enc(join(e, n)).constData());
            w->open_paths(w->pane(), {made});
            QDialog *ask_pw = nullptr;
            bool asked = wait_for([&]() {
                for (QWidget *x : QApplication::topLevelWidgets())
                    if (x->objectName() == "archive_password" && x->isVisible())
                        ask_pw = qobject_cast<QDialog *>(x);
                return ask_pw != nullptr;
            }, 20000);
            bool free_pw = asked && is_free_window(ask_pw);
            if (asked) {
                ask_pw->findChild<QLineEdit *>()->setText("correct horse");
                ask_pw->accept();
            }
            bool decrypted = wait_for([&]() { return read_file(join(e, "plans.txt")) == secret; }, 20000);
            check(asked && free_pw && decrypted && shown_window<SignatureWindow>() == nullptr,
                  "...double-clicking an encrypted .gpg asks for its password (not Verify Signature) and decrypts it beside it");

            // the window: the keys listed, the passwords must match, then a job makes the copy
            pgp::encrypt_dialog(w, {join(e, "for-else.txt")});
            EncryptWindow *ew = shown_window<EncryptWindow>();
            bool keys_in = false;
            if (ew) {
                QObject::connect(ew, &EncryptWindow::keys_listed, [&]() { keys_in = true; });
                wait_for([&]() { return keys_in; }, 20000);
            }
            check(ew && is_free_window(ew) && keys_in && ew->key_list->topLevelItemCount() == 2 &&
                      ew->key_list->topLevelItem(0)->text(0).contains("(you)"),
                  "...the Encrypt window opens free and lists the keys to encrypt to, yours marked");
            if (ew) {
                QStringList before = listdir(e);
                ew->password->setText("one");
                ew->again->setText("two");
                ew->encrypt();
                wait_for([&]() { return !ew->problem->isHidden(); });
                spin(300);   // (a file made anyway would be started by now: encrypt() starts it at once)
                bool refused = ew->isVisible() && !ew->problem->isHidden() && listdir(e) == before;
                ew->by_key->setChecked(true);
                for (int i = 0; i < ew->key_list->topLevelItemCount(); ++i)
                    if (!ew->key_list->topLevelItem(i)->text(0).contains("(you)"))
                        ew->key_list->topLevelItem(i)->setCheckState(0, Qt::Checked);
                ew->encrypt();
                QString out = join(e, "for-else (2).txt.gpg");   // (for-else.txt.gpg is there: numbered before the .gpg)
                bool made2 = wait_for([&]() { return exists(out); }, 20000);
                spin(300);
                pgp::Packets two = pgp::packets(out);
                check(refused && made2 && two.to_keys.size() == 2,
                      "...two passwords that differ are refused; to a key it's encrypted to it and to your own, under the "
                      "next free name");
            }
        } else {
            skip("encrypting and decrypting with gpg (gpg isn't installed)");
        }
    }
    // ================================================================ disk images (diskimage.h)
    {
        QString d = home_path("images");
        makedirs(d, true);
        QByteArray data;
        for (quint32 i = 0; i < 3 * 1024 * 1024; ++i)
            data += char((i * 7919 + i / 4096) & 0xff);
        QString src = join(d, "device.bin");
        write_text(src, data);
        // write_image in a job, fed by fd (a file standing for the device, or a pipe): what it raised, or ""
        auto make = [&](int fd, qint64 total, const QString &dest, bool compress, const QString &pw, bool whole,
                        Task **task = nullptr) {
            auto err = std::make_shared<QString>("(not finished)");
            Task *t = fileops::run_job(w, "image", [=](Task *t) {
                try {
                    diskimage::write_image(t, fd, total, dest, compress, pw, [whole]() { return whole; });
                    *err = QString();
                } catch (const Cancelled &) {
                    *err = "cancelled";
                } catch (const Error &e) {
                    *err = e.message();
                }
                return QVariant(true);
            }, nullptr, true, true);
            if (task)
                *task = t;
            QPointer<Task> tp(t);
            wait_for([&]() { return !tp; }, 30000);
            return *err;
        };
        auto parts_in = [&]() {
            QStringList out;
            for (const QString &n : QDir(d).entryList(QDir::Files | QDir::Hidden))
                if (n.startsWith(".kes-"))
                    out << n;
            return out;
        };
        int fd = ::open(enc(src).constData(), O_RDONLY);
        QString plain = join(d, "plain.img");
        QString e = make(fd, data.size(), plain, false, QString(), true);
        ::close(fd);
        struct stat st;
        check(e.isEmpty() && read_file(plain) == data && stat_(plain, st) && (st.st_mode & 0777) == 0600 &&
                  parts_in().isEmpty(),
              "Back Up Drive to Image: the stream becomes the image file, byte for byte, for the user alone, with no "
              "part left (" + e + ")");

        fd = ::open(enc(src).constData(), O_RDONLY);
        e = make(fd, data.size(), join(d, "short.img"), false, QString(), false);
        ::close(fd);
        check(!e.isEmpty() && !lexists(join(d, "short.img")) && parts_in().isEmpty(),
              "an image whose drive wasn't read to its end (the helper failed) isn't made, and no part is left");

        fd = ::open(enc(src).constData(), O_RDONLY);
        e = make(fd, data.size() + 1, join(d, "fewer.img"), false, QString(), true);
        ::close(fd);
        check(!e.isEmpty() && !lexists(join(d, "fewer.img")), "...nor one with fewer bytes than the drive holds");

        // a write that fails part-way (a file size limit, as a full disk): no image, no part
        struct rlimit old_lim, lim;
        ::getrlimit(RLIMIT_FSIZE, &old_lim);
        lim = old_lim;
        lim.rlim_cur = 1 << 20;
        auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
        ::setrlimit(RLIMIT_FSIZE, &lim);
        fd = ::open(enc(src).constData(), O_RDONLY);
        e = make(fd, data.size(), join(d, "full.img"), false, QString(), true);
        ::close(fd);
        ::setrlimit(RLIMIT_FSIZE, &old_lim);
        ::signal(SIGXFSZ, old_handler);
        check(!e.isEmpty() && !lexists(join(d, "full.img")) && parts_in().isEmpty(),
              "a write that fails part-way leaves no image and no part (" + e + ")");

        // cancelled part-way: the stream still coming (a pipe fed slowly)
        int p[2];
        (void)!::pipe(p);
        // never blocking: once the image stops reading (cancelled), a full pipe would hold the feeder, and the test with it
        ::fcntl(p[1], F_SETFL, O_NONBLOCK);
        std::atomic<bool> stop{false};
        std::thread feeder([&]() {
            for (int i = 0; i < 300 && !stop; ++i) {
                (void)!::write(p[1], data.constData(), 64 * 1024);   // (EAGAIN while it's full: skipped)
                ::usleep(20000);
            }
            ::close(p[1]);
        });
        auto cerr = std::make_shared<QString>();
        QPointer<Task> ctp = fileops::run_job(w, "image", [&, cerr](Task *t) {
            try {
                diskimage::write_image(t, p[0], -1, join(d, "cancelled.img"), false, QString(), []() { return true; });
            } catch (const Cancelled &) {
                *cerr = "cancelled";
            } catch (const Error &x) {
                *cerr = x.message();
            }
            return QVariant(true);
        }, nullptr, true, true);
        bool writing = wait_for([&]() { return !parts_in().isEmpty(); }, 10000);
        spin(200);
        if (ctp)
            ctp->cancel();
        wait_for([&]() { return !ctp; }, 10000);
        stop = true;
        feeder.join();
        ::close(p[0]);
        check(writing && *cerr == "cancelled" && !lexists(join(d, "cancelled.img")) && parts_in().isEmpty(),
              "cancelled part-way, it leaves no image and no part");

        if (which("zstd") && which("gpg")) {
            fd = ::open(enc(src).constData(), O_RDONLY);
            QString packed = join(d, "packed.img.zst.gpg");
            e = make(fd, data.size(), packed, true, "pass word", true);
            ::close(fd);
            proc::Options o;
            o.c_utf8 = true;
            auto r = proc::run({"sh", "-c", "gpg --batch --quiet --pinentry-mode loopback --passphrase-fd 0 -d -o - \"$1\" | zstd -d -q -c",
                                "sh", packed},
                               60000, o, "pass word\n");
            bool raw_inside = read_file(packed).contains(data.mid(100000, 4096));
            check(e.isEmpty() && r.out == data && !raw_inside && parts_in().isEmpty(),
                  "compressed and encrypted, it opens with its password to the drive's bytes, and holds none of them "
                  "as they were (" + e + ")");
        } else {
            skip("compressed and encrypted images (zstd or gpg isn't installed)");
        }

        // which files are disk images, and which can be mounted for writing
        write_text(join(d, "disk.img"), "x");
        check(diskimage::is_image(join(d, "disk.img")) && diskimage::is_image(join(d, "cd.ISO")) &&
                  diskimage::is_image(join(d, "b.img.zst.gpg")) && diskimage::is_image(join(d, "b.raw.xz")) &&
                  !diskimage::is_image(join(d, "notes.txt")) && !diskimage::is_image(join(d, ".img")) &&
                  !diskimage::is_image(join(d, "a.tar.gz")),
              "disk images are told by their names: .img, .iso, .raw, also compressed or encrypted");
        check(diskimage::needs_expanding(join(d, "b.img.gpg")) && diskimage::needs_expanding(join(d, "b.img.xz")) &&
                  !diskimage::needs_expanding(join(d, "disk.img")) && diskimage::can_write(join(d, "disk.img")) &&
                  !diskimage::can_write(join(d, "b.img.zst")) && !diskimage::can_write(join(d, "cd.iso")),
              "compressed or encrypted ones are mounted from a copy (read-only); only a plain one can be mounted for writing");
        QStringList img_menu = menu_texts(w, {join(d, "disk.img")});
        check(img_menu.contains("Mount Image") && img_menu.contains("Mount Image for Writing") &&
                  !menu_texts(w, {src}).contains("Mount Image"),
              "Mount Image (and for Writing) are in a disk image's right-click menu, not other files'");

        // the image is never saved onto the drive being read
        QString source;
        for (const drives::Mount &m : drives::mounts())
            if (m.root == drives::mount_root(d))
                source = m.source;
        if (source.startsWith("/dev/") && exists(source))
            check(!diskimage::refuse_reason(source, d).isEmpty(),
                  "an image of a drive can't be saved on that same drive (" + source + ")");
        else
            skip("refusing to save an image on the drive being read (the test's folder isn't on a block device)");
    }

    // ================================================================ RAM disks and private mode (ramdisk.h)
    {
        QString shm = ramdisk::shm_root();
        check(!shm.isEmpty() && shm.startsWith(qEnvironmentVariable("KESTREL_SHM_DIR") + "/") && isdir(shm),
              "RAM disks and private mode have a folder of Kestrel's own in /dev/shm (the test's own here: "
              "KESTREL_SHM_DIR)");
        struct stat sst;
        check(stat_(shm, sst) && (sst.st_mode & 0777) == 0700, "...which only you can open");

        // -- the mirror (a persistent RAM disk's loading and saving)
        QString r = home_path("rd/ram"), disk = home_path("rd/disk");
        makedirs(join(r, "sub"), true);
        makedirs(join(disk, "gone"), true);
        write_text(join(r, "a.txt"), "new a");
        write_text(join(r, "sub/b.txt"), "b");
        write_text(join(r, ".kes-00000000.part"), "half-made");   // a copy into the RAM disk still running
        (void)!::symlink("a.txt", enc(join(r, "link")).constData());
        write_text(join(disk, "a.txt"), "old a");
        set_mtime(join(disk, "a.txt"), time(nullptr) - 3600);   // (the same size: written in the same tick, it'd look saved)
        write_text(join(disk, "only-on-disk.txt"), "x");
        write_text(join(disk, "gone/c.txt"), "c");
        QStringList errors;
        ramdisk::MirrorStats st = ramdisk::mirror(r, disk, true, &errors);
        check(errors.isEmpty() && read_file(join(disk, "a.txt")) == "new a" && read_file(join(disk, "sub/b.txt")) == "b" &&
                  islink(join(disk, "link")) && readlink(join(disk, "link")) == "a.txt" &&
                  !exists(join(disk, "only-on-disk.txt")) && !exists(join(disk, "gone")) &&
                  !exists(join(disk, ".kes-00000000.part")) && st.copied == 3,
              "saving makes the folder on disk match the RAM disk: changed files replaced, links as links, what's gone "
              "deleted, and a copy still being made in the RAM disk left out (" + errors.join("; ") + ")");
        st = ramdisk::mirror(r, disk, true, &errors);
        check(errors.isEmpty() && st.copied == 0 && st.deleted == 0, "saving again copies only what changed (nothing)");
        write_text(join(disk, "extra.txt"), "kept");
        ramdisk::mirror(r, disk, false, &errors);
        check(exists(join(disk, "extra.txt")), "without deleting (a load, a partly loaded RAM disk's save), nothing "
                                               "only in the destination is deleted");

        // a save cut off part-way (a full disk: RLIMIT_FSIZE): the saved file is still the old one, whole, and no part
        // file is left
        write_text(join(r, "big.bin"), QByteArray(3 << 20, 'n'));
        write_text(join(disk, "big.bin"), "old big");
        errors.clear();
        {
            struct rlimit old_lim, lim;
            ::getrlimit(RLIMIT_FSIZE, &old_lim);
            lim = old_lim;
            lim.rlim_cur = 1 << 20;
            auto old_handler = ::signal(SIGXFSZ, SIG_IGN);
            ::setrlimit(RLIMIT_FSIZE, &lim);
            ramdisk::mirror(r, disk, true, &errors);
            ::setrlimit(RLIMIT_FSIZE, &old_lim);
            ::signal(SIGXFSZ, old_handler);
        }
        QStringList parts;
        for (const QString &n : listdir(disk))
            if (n.startsWith(".kes-"))
                parts << n;
        check(!errors.isEmpty() && read_file(join(disk, "big.bin")) == "old big" && parts.isEmpty(),
              "a save that fails part-way keeps the saved file as it was, whole, and leaves no part file");
        // ...and one cancelled after its first file: what's saved is whole, the rest still the old one
        write_text(join(r, "a.txt"), "newer a");
        write_text(join(r, "sub/b.txt"), "newer b");
        int asked = 0;
        bool stopped = false;
        try {
            ramdisk::mirror(r, disk, true, &errors, [&]() { return ++asked > 3; });
        } catch (const Cancelled &) {
            stopped = true;
        }
        QByteArray a = read_file(join(disk, "a.txt")), b = read_file(join(disk, "sub/b.txt"));
        parts.clear();
        for (const QString &n : listdir(disk) + listdir(join(disk, "sub")))
            if (n.startsWith(".kes-"))
                parts << n;
        check(stopped && (a == "new a" || a == "newer a") && (b == "b" || b == "newer b") && parts.isEmpty(),
              "a cancelled save leaves each file whole, old or new, and no part file");
        // ...and one cancelled in the middle of a file (asked once its part is there): that part goes too
        write_text(join(r, "big.bin"), QByteArray((3 << 20) + 1, 'm'));
        auto part_in_disk = [&]() {
            for (const QString &n : listdir(disk) + listdir(join(disk, "sub")))
                if (n.startsWith(".kes-"))
                    return true;
            return false;
        };
        stopped = false;
        try {
            ramdisk::mirror(r, disk, true, &errors, part_in_disk);
        } catch (const Cancelled &) {
            stopped = true;
        }
        check(stopped && !part_in_disk() && read_file(join(disk, "big.bin")).size() != (3 << 20) + 1,
              "a save cancelled in the middle of a file leaves no part file, and the saved one as it was");
        ::unlink(enc(join(r, "big.bin")).constData());

        // -- a persistent RAM disk is saved only once it has been loaded
        ramdisk::Disk d;
        d.id = "0badf00d";
        d.name = "Test";
        d.kind = ramdisk::FOLDER;
        d.path = home_path("rd/fresh");
        d.life = ramdisk::PERSISTENT;
        d.save_to = disk;
        makedirs(d.path, true);
        QStringList before = listdir(disk);
        before.sort();
        errors.clear();
        bool saved = ramdisk::save(d, &errors);
        QStringList after = listdir(disk);
        after.sort();
        check(!saved && !errors.isEmpty() && after == before,
              "a persistent RAM disk that came up without its files isn't saved (it would empty the saved copy)");
        if (::setxattr(enc(d.path).constData(), "user.kestrel-test", "1", 1, 0) != 0 &&
            qEnvironmentVariableIsEmpty("XDG_RUNTIME_DIR")) {
            skip("loading a persistent RAM disk (no user.* attributes here, and no XDG_RUNTIME_DIR)");
        } else {
            write_text(join(disk, "extra.txt"), "kept");   // (the saves above deleted it: it isn't in the RAM disk)
            errors.clear();
            bool loaded = ramdisk::load(d, &errors);
            check(loaded && ramdisk::loaded(d.path, d.id) && read_file(join(d.path, "a.txt")) == read_file(join(disk, "a.txt")) &&
                      read_file(join(d.path, "extra.txt")) == "kept",
                  "loading copies the saved files into it and marks it loaded");
            write_text(join(d.path, "made-in-ram.txt"), "r");
            ::unlink(enc(join(d.path, "extra.txt")).constData());
            errors.clear();
            check(ramdisk::save(d, &errors) && read_file(join(disk, "made-in-ram.txt")) == "r" &&
                      !exists(join(disk, "extra.txt")),
                  "...then it's saved: what's new goes to disk, what was deleted in it goes from there too");
        }

        // -- its systemd units: nothing in a name or path is expanded or splits the command
        d.name = "50% off";
        d.interval = 10;
        QString svc = ramdisk::service_text(d, "/opt/my kes/kes$x");
        check(svc.contains("ExecStart=\"/opt/my kes/kes$$x\" --ramdisk-start 0badf00d\n") &&
                  svc.contains("ExecStop=\"/opt/my kes/kes$$x\" --ramdisk-stop 0badf00d\n") &&
                  svc.contains("Description=Kestrel Explorer RAM disk 50%% off\n") &&
                  ramdisk::timer_text(d).contains("OnUnitActiveSec=10min\n") &&
                  ramdisk::unit_names(d) == QStringList({"kestrel-ramdisk-0badf00d.service",
                                                         "kestrel-ramdisk-0badf00d-save.service",
                                                         "kestrel-ramdisk-0badf00d.timer"}),
              "a persistent RAM disk's units run kes with its path quoted and nothing in it expanded, and save on a timer");
        d.life = ramdisk::SESSION;
        check(ramdisk::unit_names(d).isEmpty(), "one that lasts until logout has no units");

        // -- the list, and the New RAM Disk window
        check(!ramdisk::name_problem("").isEmpty() && !ramdisk::name_problem("a/b").isEmpty() &&
                  !ramdisk::name_problem(".hidden").isEmpty() && ramdisk::name_problem("Scratch").isEmpty(),
              "a RAM disk's name can't be empty, have a / or start with a dot");
        auto *win = new RamDiskWindow(w);
        show_free(win);
        spin(50);
        win->name->setText("Scratch");
        check(is_free_window(win) && win->problem_text().isEmpty() && win->chosen().path == join(shm, "Scratch") &&
                  win->chosen().kind == ramdisk::FOLDER,
              "New RAM Disk is a free window; without a size limit it's a folder in Kestrel's /dev/shm folder");
        win->lives[ramdisk::PERSISTENT]->setChecked(true);
        win->save_to->setText(join(shm, "inside"));
        check(!win->problem_text().isEmpty(), "...a persistent one can't be saved to a folder in memory");
        win->limit->setChecked(true);
        win->location->setText(disk);   // not empty
        win->lives[ramdisk::SESSION]->setChecked(true);
        check(!win->problem_text().isEmpty() && win->chosen().kind == ramdisk::MOUNT,
              "...with a size limit it's mounted on a folder, which must be new or empty");
        win->close();
        QList<ramdisk::Disk> list;
        ramdisk::Disk e;
        e.id = "00000001", e.name = "Gone", e.path = join(shm, "Gone"), e.life = ramdisk::SESSION;
        ramdisk::Disk k = e;
        k.id = "00000002", k.name = "Kept", k.path = join(shm, "Kept"), k.life = ramdisk::PERMANENT;
        list << e << k;
        ramdisk::save_disks(list);
        ramdisk::prune();
        list = ramdisk::disks();
        check(list.size() == 1 && list[0].id == "00000002",
              "at start, a RAM disk whose time is over (until logout, and no longer there) is forgotten; a permanent "
              "one is kept to bring back");
        makedirs(join(shm, "Kept"), true);
        w->sidebar->refresh();
        check(wait_for([&]() { return w->sidebar->entry_keys("ramdisks").contains("ramdisk:00000002"); }),
              "the sidebar's RAM Disks section lists it");

        // -- private mode: caches and histories in memory, Save windows start in Private, all of it deleted when
        // it's left (or the last Kestrel closes)
        QString cache_before = APP_CACHE(), thumbs_before = THUMB_DIR(), history_before = HISTORY_DIR();
        prefs::set("private_mode", true);
        QString priv = ramdisk::private_dir();
        check(APP_CACHE().startsWith(priv + "/") && THUMB_DIR().startsWith(priv + "/") && HISTORY_DIR().startsWith(priv + "/") &&
                  SHARED_THUMB_DIR() == thumbs_before && isdir(ramdisk::private_files()),
              "in private mode the caches, thumbnails and histories are in the private RAM disk (thumbnails made "
              "before are still read from the shared cache)");
        check(wait_for([&]() { return w->sidebar->entry_keys("ramdisks").contains("private") && w->private_btn->isVisible(); }) &&
                  w->windowTitle().contains("(Private)"),
              "...and the window says so: Private in the sidebar and the status bar, and in its title");
        history_settings().setValue("recent_servers", QStringList{"sftp://private.example"});
        check(!settings().value("recent_servers").toStringList().contains("sftp://private.example"),
              "a server connected to in private mode isn't remembered on disk");
        prefs::set("ramdisk_wipe_warning", false);
        write_text(join(ramdisk::private_files(), "saved.txt"), "s");
        ramdisk::set_private(w, false);
        check(APP_CACHE() == cache_before && HISTORY_DIR() == history_before && THUMB_DIR() == thumbs_before,
              "leaving private mode puts the caches and histories back where they were");
        check(wait_for([&]() { return !lexists(priv); }, 5000),
              "...and deletes the private RAM disk, with the files saved in Private");
        prefs::set("private_mode", true);
        prefs::set("ramdisk_wipe_warning", true);
        write_text(join(ramdisk::private_files(), "saved.txt"), "s");
        check(ramdisk::wipe_needs_warning(true), "with files in Private, leaving says first that they'll be deleted");
        ramdisk::last_kestrel_gone();
        check(!lexists(ramdisk::private_dir()) && lexists(join(shm, "Kept")),
              "when the last Kestrel has gone, the private RAM disk is deleted (and a permanent RAM disk isn't)");
        prefs::set("private_mode", false);
    }

    finish();
}
