// Searching: the search bar's Filters (type, size, date) and searching inside files (the index, or reading the files
// with ripgrep or grep: matching lines, case, regular expressions), and opening a result at its matching line.
#include "common.h"

#include "drives.h"

#include <QMessageBox>
#include <QStatusBar>

#include <fcntl.h>
#include <sys/stat.h>

using namespace test;

static void set_mtime(const QString &p, const QDateTime &t)
{
    struct timespec ts[2];
    ts[0].tv_sec = ts[1].tv_sec = t.toSecsSinceEpoch();
    ts[0].tv_nsec = ts[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, p.toUtf8().constData(), ts, 0);
}

// the results shown, relative to root, sorted
static QStringList results(Pane *p, const QString &root)
{
    QStringList out;
    for (const QString &x : p->all_paths())
        out << x.mid(root.size() + 1);
    out.sort();
    return out;
}

// search and wait for the whole result
static QStringList search(Pane *p, const QString &root, const QString &text)
{
    p->search_for(text);
    wait_for([&]() { return !p->searching(); }, 20000);
    spin(100);   // (the last results arrive through the event loop)
    return results(p, root);
}

static QString match_text(Pane *p, const QString &path)   // the Match column of a result
{
    QAbstractItemModel *m = p->tree->model();
    for (int r = 0; r < m->rowCount(); ++r)
        if (m->index(r, 0).data(PathRole).toString() == path)
            return m->index(r, 4).data().toString();
    return QString();
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    QString s = home_path("s");
    makedirs(join(s, "sub"), true);
    write_text(join(s, "photo.jpg"), QByteArray(2048, 'p'));
    write_text(join(s, "clip.mp4"), "v");
    write_text(join(s, "song.mp3"), "a");
    write_text(join(s, "report.pdf"), "d");
    write_text(join(s, "pack.zip"), "z");
    write_text(join(s, "notes.txt"), "hello Needle world\nsecond needle line\n");
    write_text(join(s, "old.txt"), "an old needle\n");
    write_text(join(s, "big.bin"), QByteArray(3 * 1024 * 1024, 'b'));
    write_text(join(s, "sub/deep.txt"), "NEEDLE deep down\n");
    write_text(join(s, "sub/inner.jpg"), "i");
    write_text(join(s, ".hidden.txt"), "a hidden needle\n");
    write_text(join(s, "binary.dat"), QByteArray("needle\0\0\0binary", 16));
    QDateTime old(QDate(QDate::currentDate().year() - 3, 6, 15), QTime(12, 0));
    set_mtime(join(s, "old.txt"), old);

    // ---- types and filters (on any thread, by name)
    check(search_type_of(join(s, "photo.jpg")) == "images" && search_type_of(join(s, "clip.mp4")) == "videos" &&
              search_type_of(join(s, "song.mp3")) == "audio" && search_type_of(join(s, "report.pdf")) == "documents" &&
              search_type_of(join(s, "notes.txt")) == "documents" && search_type_of(join(s, "pack.zip")) == "archives" &&
              search_type_of(join(s, "set.7z.003")) == "archives" && search_type_of(join(s, "big.bin")).isEmpty(),
          "a file's type for the filters comes from its name: images, videos, audio, documents, archives (also split parts)");
    QDate today = QDate::currentDate();
    SearchFilter over;
    over.size_mode = SearchFilter::OVER;
    over.size = 1024 * 1024;
    SearchFilter under = over;
    under.size_mode = SearchFilter::UNDER;
    check(over.matches("x", false, 3 << 20, 0, today) && !over.matches("x", false, 1000, 0, today) &&
              under.matches("x", false, 1000, 0, today) && !over.matches("x", true, 3 << 20, 0, today),
          "the size filter keeps files larger or smaller than the limit (and no folders)");
    SearchFilter recent;
    recent.when = SearchFilter::TODAY;
    qint64 now = QDateTime::currentSecsSinceEpoch(), then = old.toSecsSinceEpoch();
    SearchFilter range;
    range.when = SearchFilter::RANGE;
    range.from = old.date().addDays(-1);
    range.to = old.date().addDays(1);
    SearchFilter year;
    year.when = SearchFilter::THIS_YEAR;
    check(recent.matches("x", false, 0, now, today) && !recent.matches("x", false, 0, then, today) &&
              range.matches("x", false, 0, then, today) && !range.matches("x", false, 0, now, today) &&
              year.matches("x", false, 0, now, today) && !year.matches("x", false, 0, then, today),
          "the date filter: today, this year, and a range of days");
    SearchFilter born = recent;
    born.by_created = true;
    struct statx sx;
    bool births = ::statx(AT_FDCWD, enc(join(s, "old.txt")).constData(), 0, STATX_BTIME, &sx) == 0 && (sx.stx_mask & STATX_BTIME);
    if (births)
        check(born.matches(join(s, "old.txt"), false, 0, then, today),
              "...by the date created when chosen (a file modified years ago, made today)");
    else
        skip("...by the date created when chosen (this file system keeps no creation times)");

    setup_app();
    MainWindow *w = open_window({s});
    Pane *p = w->pane();
    wait_for([&]() { return p->all_paths().size() >= 9; });

    // ---- filters in the search bar
    p->set_search_options(false, false);
    SearchFilter images;
    images.type = SearchFilter::IMAGES;
    p->set_search_filter(images);
    QStringList here = search(p, s, "");
    p->set_search_options(true, false);
    QStringList below = search(p, s, "");
    check(here == QStringList({"photo.jpg"}) && below == QStringList({"photo.jpg", "sub/inner.jpg"}),
          "Filters: a type alone (no name) finds this folder's images, and with subfolders theirs too (" +
              below.join(", ") + ")");
    check(search(p, s, "in") == QStringList({"sub/inner.jpg"}), "...combined with the name");
    p->set_search_filter(over);
    check(search(p, s, "") == QStringList({"big.bin"}), "...a size filter finds only the files over the size");
    SearchFilter docs_today;
    docs_today.type = SearchFilter::DOCUMENTS;
    docs_today.when = SearchFilter::TODAY;
    QStringList found = search(p, s, "");
    p->set_search_filter(docs_today);
    found = search(p, s, "");
    check(found == QStringList({"notes.txt", "report.pdf", "sub/deep.txt"}),
          "...and the date with the type: documents modified today, not the old one (" + found.join(", ") + ")");
    Pane *tab = w->new_tab(s, true);
    check(tab && tab != p && tab->search_filter() == docs_today, "the filters are kept for the session: a new tab has them");
    w->close_tab(w->tabs->indexOf(tab));
    p->set_search_filter(SearchFilter());

    // ---- inside files, by reading them (ripgrep, else grep)
    auto read_with = [&](bool regex, bool match_case) {
        ContentSearch c;
        c.on = true;
        c.engine = ContentSearch::READ;
        c.regex = regex;
        c.match_case = match_case;
        p->set_content_search(c);
    };
    auto content_checks = [&](const QString &engine) {
        read_with(false, false);
        p->set_search_options(true, true);
        QStringList hits = search(p, s, "needle");
        check(hits == QStringList({"notes.txt", "old.txt", "sub/deep.txt"}),
              engine + ": finds the files with the text in them, any case, in subfolders, skipping binary and hidden "
                       "files (" + hits.join(", ") + ")");
        check(p->match_line(join(s, "notes.txt")) == 1 && match_text(p, join(s, "notes.txt")) == "1: hello Needle world",
              engine + ": ...with the first matching line and its number in the Match column (" +
                  match_text(p, join(s, "notes.txt")) + ")");
        p->set_search_options(false, true);
        check(search(p, s, "needle") == QStringList({"notes.txt", "old.txt"}), engine + ": ...not in subfolders unless asked");
        p->set_search_options(true, true);
        read_with(false, true);
        hits = search(p, s, "needle");
        check(hits == QStringList({"notes.txt", "old.txt"}) && p->match_line(join(s, "notes.txt")) == 2,
              engine + ": Match case: only the same case (the first such line of notes.txt is line 2)");
        read_with(true, false);
        hits = search(p, s, "^hello .* world$");
        bool regex_hits = hits == QStringList({"notes.txt"});
        read_with(false, false);
        check(regex_hits && search(p, s, "^hello .* world$").isEmpty(),
              engine + ": a regular expression when chosen, the text as it is otherwise");
        read_with(true, false);
        search(p, s, "(unclosed");
        QString said = p->view_message();
        check(said.startsWith("Nothing found\n") && said.size() > 15 && !shown_window<QMessageBox>(),
              engine + ": a regular expression that doesn't parse says why, in the view, not a window (" +
                  QString(said).replace('\n', " / ") + ")");
        read_with(false, false);
        w->toggle_hidden(true);
        hits = search(p, s, "needle");
        w->toggle_hidden(false);
        check(hits.contains(".hidden.txt"), engine + ": hidden files too while they're shown");
    };
    QString path_before = qEnvironmentVariable("PATH");
    if (which("rg"))
        content_checks("ripgrep");
    else
        skip("ripgrep: the content search's checks (ripgrep isn't installed)");
    {   // without ripgrep: grep (a PATH holding grep but no rg)
        QString bin = home_path("grep-only");
        makedirs(bin, true);
        for (const char *t : {"grep", "gsettings"})
            if (which(t))
                QFile::link(which_path(t), join(bin, t));
        qputenv("PATH", bin.toUtf8());
        content_checks("grep");
        qputenv("PATH", path_before.toUtf8());
    }

    // ---- while a search runs, a progress bar; when it ends with nothing, the view says so (no window opens), over the
    // view shown (switching to the list puts it above everything made before)
    p->set_view_mode("list");
    p->set_content_search(ContentSearch());
    p->set_search_options(true, false);
    QStringList none = search(p, s, "zzz-not-here");
    check(none.isEmpty() && p->view_message() == "Nothing found" && !p->search_busy_shown() && !shown_window<QMessageBox>(),
          "a search that finds nothing says “Nothing found” in the view, with no window, and no progress bar left");
    p->set_search_options(false, false);
    search(p, s, "zzz-not-here");
    check(p->view_message() == "Nothing found",
          "...also filtering this folder by name (not “Folder is empty”)");
    p->set_search_options(true, false);
    search(p, s, "notes");
    check(p->view_message().isEmpty(), "...and nothing is said over results");

    // ✕: the program reading the files is stopped
    QString many = home_path("many");
    makedirs(many, true);
    for (int i = 0; i < 3000; ++i)
        write_text(join(many, QString("f%1.txt").arg(i)), QByteArray(20000, 'x') + "needle\n");
    p->set_path(many);
    read_with(false, false);
    p->search_for("needle");
    bool busy = p->search_busy_shown() && p->searching() && p->view_message() == "Searching…";
    p->stop_search();
    bool stopped = wait_for([&]() { return !p->searching(); }, 5000);
    spin(300);
    auto r = proc::run({"pgrep", "-f", "-e", "needle.*" + many});
    check(busy, "while a search runs, a progress bar shows, and the view says “Searching…” until something is found");
    check(stopped && r.out.trimmed().isEmpty() && !p->search_busy_shown(),
          "stopping a search inside files ends the program reading them, and its progress bar");
    p->set_path(s);

    // ---- the index, when it covers the folder (localsearch's settings, in the throwaway home)
    qputenv("GSETTINGS_BACKEND", "keyfile");
    auto set_index = proc::run({"gsettings", "set", "org.freedesktop.Tracker3.Miner.Files", "index-recursive-directories",
                                "['" + s + "', '&DOCUMENTS']"});
    if (set_index.rc == 0) {
        check(index_covers(s) && index_covers(join(s, "sub")) && !index_covers(home_path("elsewhere")),
              "the index covers a folder under one of localsearch's folders, and not another");
        QStringList thread_found;
        auto engine_for = [&](const QString &root, const ContentSearch &how) {
            SearchThread t(root, "needle", false, nullptr, true, true, SearchFilter(), how);
            thread_found.clear();
            QObject::connect(&t, &SearchThread::found, &t, [&](const QStringList &paths) { thread_found << paths; },
                             Qt::DirectConnection);
            t.start();
            t.wait(30000);
            return t.engine;
        };
        ContentSearch automatic;
        automatic.on = true;
        ContentSearch automatic_regex = automatic;
        automatic_regex.regex = true;
        QString in_index = engine_for(s, automatic);
        QStringList from_index = thread_found;
        QString elsewhere = engine_for(many, automatic), with_regex = engine_for(s, automatic_regex);
        if (can_search_contents()) {
            check(in_index.startsWith("index") && !elsewhere.contains("index") && !with_regex.contains("index"),
                  "Automatic uses the index for plain words in a folder it covers, else reads the files (" + in_index +
                      "; " + elsewhere + "; " + with_regex + ")");
            // here the index isn't running (a private bus): it finds nothing, so the files are read after all
            check(in_index.contains(", then ") && from_index.size() == 3,
                  "...and when the index finds nothing (not running, or not there yet), the files are read after all (" +
                      QString::number(from_index.size()) + " found)");
        }
        else
            check(in_index != "index", "Automatic reads the files when there's no index (localsearch isn't installed)");
    } else {
        skip("the index's folders (localsearch's settings aren't installed)");
    }
    qunsetenv("GSETTINGS_BACKEND");

    // ---- never on a tape or floppy: it would read every file there
    QString tape = home_path("tape");
    makedirs(tape, true);
    write_text(join(tape, "t.txt"), "needle\n");
    qputenv("KESTREL_TEST_SLOW", ("tape=" + tape).toUtf8());
    spin(1100);   // (the mount list is re-read once it's a second old)
    p->set_path(tape);
    QStringList on_tape = search(p, tape, "needle");
    check(on_tape.isEmpty() && w->statusBar()->currentMessage().contains("tape or floppy"),
          "File contents isn't searched on a tape, and says so");
    qunsetenv("KESTREL_TEST_SLOW");
    p->set_path(s);

    // ---- opening a result at its line, with an editor that takes one (a stand-in gedit)
    QString fake = home_path("bin/gedit"), args = home_path("gedit-args");
    makedirs(dirname(fake), true);
    write_text(fake, ("#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + args + "'\n").toUtf8());
    ::chmod(enc(fake).constData(), 0755);
    makedirs(home_path(".local/share/applications"), true);
    write_text(home_path(".local/share/applications/fake-gedit.desktop"),
               ("[Desktop Entry]\nType=Application\nName=Fake gedit\nExec=" + fake + " %U\nMimeType=text/plain;\n").toUtf8());
    write_text(home_path(".config/mimeapps.list"), "[Default Applications]\ntext/plain=fake-gedit.desktop\n");
    read_with(false, true);
    p->set_search_options(true, true);
    search(p, s, "needle");
    w->open_paths(p, {join(s, "notes.txt")});
    bool at_line = wait_for([&]() { return read_file(args) == "+2\n" + join(s, "notes.txt").toUtf8() + "\n"; }, 5000);
    check(at_line,
          "a result opens at its matching line in an editor that takes one (" + QString::fromUtf8(read_file(args)).trimmed() + ")");

    finish();
}
