// The file tools: Find Duplicates (dupes.h), Compare With… and Sync To… (compare.h), and Verify Signature
// (signature.h).
#include "common.h"

#include "compare.h"
#include "dupes.h"
#include "hashcheck.h"
#include "signature.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QTreeWidget>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>

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
            bool counted = dw->summary->text().contains("Not kept: 2 file(s)");
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
    finish();
}
