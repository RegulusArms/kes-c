// Grouping by date: the groups a date falls in (by period, by day), a folder (or a place) shown under date headers
// keeping the chosen sort inside each group, and the dates used: modified, created, or taken (from the metadata).
#include "common.h"

#include "groups.h"
#include "metadata.h"

#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QUrl>

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

// what the view shows, top to bottom: "# <header>" or an item's name (with footers: "^ <footer>")
static QStringList shown(Pane *p, bool footers = false)
{
    QAbstractItemModel *m = p->view()->model();
    QStringList out;
    for (int r = 0; r < m->rowCount(p->view()->rootIndex()); ++r) {
        QModelIndex i = m->index(r, 0, p->view()->rootIndex());
        if (i.data(GroupFooterRole).toBool())
            out << (footers ? QStringList{"^ " + i.data().toString()} : QStringList());
        else
            out << (i.data(GroupRole).isValid() ? "# " + i.data().toString() : basename(i.data(PathRole).toString()));
    }
    return out;
}

static QModelIndex footer(Pane *p, const QString &title)   // the footer of the group titled so
{
    QAbstractItemModel *m = p->view()->model();
    for (int r = 0; r < m->rowCount(); ++r)
        if (m->index(r, 0).data(GroupFooterRole).toBool() && m->index(r, 0).data().toString() == "Collapse " + title)
            return m->index(r, 0);
    return QModelIndex();
}

static QModelIndex item_of(Pane *p, const QString &name)
{
    QAbstractItemModel *m = p->view()->model();
    for (int r = 0; r < m->rowCount(); ++r)
        if (basename(m->index(r, 0).data(PathRole).toString()) == name)
            return m->index(r, 0);
    return QModelIndex();
}

static QAction *action_in(QMenu *m, const QString &text)
{
    for (QAction *a : m->actions())
        if (a->text() == text)
            return a;
    return nullptr;
}

static QModelIndex header(Pane *p, const QString &title)
{
    QAbstractItemModel *m = p->view()->model();
    for (int r = 0; r < m->rowCount(); ++r)
        if (m->index(r, 0).data(GroupRole).isValid() && m->index(r, 0).data().toString().startsWith(title))
            return m->index(r, 0);
    return QModelIndex();
}

static void write_file(const QString &p, const QByteArray &data)
{
    QFile f(p);
    f.open(QIODevice::WriteOnly);
    f.write(data);
}

static void le16(QByteArray &b, int v) { b.append(char(v & 0xff)).append(char((v >> 8) & 0xff)); }
static void le32(QByteArray &b, quint32 v) { le16(b, int(v & 0xffff)); le16(b, int(v >> 16)); }
static void be32(QByteArray &b, quint32 v)
{
    b.append(char(v >> 24)).append(char((v >> 16) & 0xff)).append(char((v >> 8) & 0xff)).append(char(v & 0xff));
}

// a JPEG holding only EXIF with DateTimeOriginal (what metadata::taken reads; no picture needed)
static QByteArray jpeg_taken(const QString &when)   // "YYYY:MM:DD HH:MM:SS"
{
    QByteArray tiff("II*\0", 4);
    le32(tiff, 8);                        // IFD0
    le16(tiff, 1);                        // one entry: the Exif IFD's offset
    le16(tiff, 0x8769), le16(tiff, 4), le32(tiff, 1), le32(tiff, 26);
    le32(tiff, 0);
    le16(tiff, 1);                        // Exif IFD at 26: DateTimeOriginal, ASCII, 20 bytes at 44
    le16(tiff, 0x9003), le16(tiff, 2), le32(tiff, 20), le32(tiff, 44);
    le32(tiff, 0);
    tiff += when.toLatin1() + QByteArray(1, '\0');
    QByteArray app1 = QByteArray("Exif\0\0", 6) + tiff;
    QByteArray out("\xff\xd8\xff\xe1", 4);
    out.append(char((app1.size() + 2) >> 8)).append(char((app1.size() + 2) & 0xff));
    return out + app1 + QByteArray("\xff\xd9", 2);
}

// an MP4 with only a movie header recorded at `when`
static QByteArray mp4_taken(const QDateTime &when)
{
    QByteArray mvhd;
    be32(mvhd, 8 + 100), mvhd += "mvhd";
    QByteArray body(100, '\0');
    quint32 secs = quint32(when.toSecsSinceEpoch() + 2082844800LL);
    body[4] = char(secs >> 24), body[5] = char((secs >> 16) & 0xff), body[6] = char((secs >> 8) & 0xff),
    body[7] = char(secs & 0xff);
    mvhd += body;
    QByteArray out;
    be32(out, 16), out += "ftypisom", be32(out, 0);
    be32(out, 8 + mvhd.size()), out += "moov", out += mvhd;
    return out;
}

static QString titles(const QDate &today, Qt::DayOfWeek first, const QList<QDate> &dates)
{
    QStringList out;
    for (const QDate &d : dates)
        out << groups::title(groups::key(d, today, first), today);
    return out.join(" | ");
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);

    // ---- the groups (pure): Wednesday 14 October 2026, weeks starting on Monday
    QDate wed(2026, 10, 14);
    QString got = titles(wed, Qt::Monday,
                         {wed.addDays(1), wed, QDate(2026, 10, 13), QDate(2026, 10, 12), QDate(2026, 10, 11),
                          QDate(2026, 10, 5), QDate(2026, 10, 4), QDate(2026, 10, 1), QDate(2026, 9, 30),
                          QDate(2026, 9, 1), QDate(2026, 8, 31), QDate(2026, 1, 1), QDate(2025, 12, 31),
                          QDate(1999, 3, 3), QDate()});
    check(got == "In the future | Today | Yesterday | Earlier this week | Last week | Last week | Earlier this month | "
                 "Earlier this month | Last month | Last month | Earlier this year | Earlier this year | 2025 | 1999 | "
                 "No date",
          "each date falls in its group (got: " + got + ")");
    check(titles(wed, Qt::Sunday, {QDate(2026, 10, 11), QDate(2026, 10, 10)}) == "Earlier this week | Last week",
          "the week starts on the locale's first day");
    // Friday 2 October: this week began in September, and last week is all September
    QDate fri(2026, 10, 2);
    got = titles(fri, Qt::Monday, {QDate(2026, 9, 29), QDate(2026, 9, 21), QDate(2026, 9, 20), QDate(2026, 8, 31)});
    check(got == "Earlier this week | Last week | Last month | Earlier this year",
          "a week across two months stays a week; the month before takes what's left (got: " + got + ")");
    QDate jan(2026, 1, 5);
    got = titles(jan, Qt::Monday, {QDate(2026, 1, 1), QDate(2025, 12, 20), QDate(2025, 11, 30)});
    check(got == "Last week | Last month | 2025", "in January, last month is December and the rest of last year is its "
                                                  "year (got: " + got + ")");
    bool ordered = true;
    for (QDate today : {wed, fri, jan, QDate(2026, 3, 1), QDate(2028, 2, 29)}) {
        int last = -1;
        for (QDate d = today.addDays(3); d > today.addDays(-800); d = d.addDays(-1)) {
            int k = groups::key(d, today, Qt::Monday);
            ordered = ordered && k >= last;
            last = k;
        }
    }
    check(ordered, "going back in time, the groups only ever get older (no group comes back after a later one)");

    QDate tue(2026, 10, 13);
    check(groups::day_key(tue) < groups::day_key(tue.addDays(-1)) && groups::day_key(tue.addDays(-1)) < groups::day_key(QDate()) &&
              groups::day_title(groups::day_key(tue), tue) == "Today, " + QLocale().toString(tue, QLocale::LongFormat) &&
              groups::day_title(groups::day_key(tue.addDays(-1)), tue).startsWith("Yesterday, ") &&
              groups::day_title(groups::day_key(QDate(2020, 2, 3)), tue) == QLocale().toString(QDate(2020, 2, 3), QLocale::LongFormat) &&
              groups::day_title(groups::day_key(QDate()), tue) == "No date",
          "Group by Day: a key per date, newer first, titled with the date (Today and Yesterday named)");

    // ---- the metadata's dates
    QString media = home_path("media");
    makedirs(media, true);
    QString jpg = join(media, "photo.jpg"), mp4 = join(media, "clip.mp4"), png = join(media, "shot.png"),
            txt = join(media, "notes.txt"), bare = join(media, "bare.jpg");
    write_file(jpg, jpeg_taken("2019:05:04 10:20:30"));
    write_file(mp4, mp4_taken(QDateTime(QDate(2018, 7, 8), QTime(12, 0))));
    QByteArray png_data("\x89PNG\r\n\x1a\n", 8), text = QByteArray("Creation Time") + '\0' + "2017-03-02T09:00:00";
    be32(png_data, quint32(text.size())), png_data += "tEXt" + text, be32(png_data, 0);
    be32(png_data, 0), png_data += "IEND", be32(png_data, 0);
    write_file(png, png_data);
    write_text(txt, "x");
    write_file(bare, QByteArray("\xff\xd8\xff\xd9", 4));
    check(metadata::taken(jpg) == QDateTime(QDate(2019, 5, 4), QTime(10, 20, 30)),
          "a photo's date taken is its EXIF DateTimeOriginal");
    check(metadata::taken(mp4) == QDateTime(QDate(2018, 7, 8), QTime(12, 0)), "a video's is its movie header's creation time");
    check(metadata::taken(png).date() == QDate(2017, 3, 2), "a PNG's is its Creation Time text");
    check(!metadata::taken(txt).isValid() && !metadata::taken(bare).isValid(), "a file without one has none");

    // ---- a folder grouped by date
    QString fs = home_path("fs"), other = home_path("other");
    makedirs(fs, true);
    makedirs(other, true);
    QDateTime now = QDateTime::currentDateTime().addSecs(-1);
    QDateTime yesterday(QDate::currentDate().addDays(-1), QTime(12, 0));
    QDateTime old(QDate(QDate::currentDate().year() - 3, 6, 15), QTime(12, 0));
    QString old_year = QString::number(old.date().year());
    write_text(join(fs, "a.txt"), "xxx");
    write_text(join(fs, "b.txt"), "x");
    write_text(join(fs, "c.txt"), "x");
    write_text(join(fs, "d.txt"), "x");
    write_text(join(other, "x.txt"), "x");
    set_mtime(join(fs, "a.txt"), now);
    set_mtime(join(fs, "b.txt"), now);
    set_mtime(join(fs, "c.txt"), yesterday);
    set_mtime(join(fs, "d.txt"), old);
    setup_app();
    MainWindow *w = open_window({fs});
    Pane *p = w->pane();
    p->sort_by(0, Qt::AscendingOrder);
    wait_for([&]() { return p->all_paths().size() == 4; });
    check(!p->grouping() && shown(p) == QStringList({"a.txt", "b.txt", "c.txt", "d.txt"}),
          "(before) the folder isn't grouped");
    p->set_grouping(groups::PERIOD, groups::MODIFIED);
    QStringList want = {"# Today (2)", "a.txt", "b.txt", "# Yesterday (1)", "c.txt", "# " + old_year + " (1)", "d.txt"};
    check(wait_for([&]() { return shown(p) == want; }), "Group by Period shows the items under date headers, newest first");
    p->sort_by(0, Qt::DescendingOrder);
    want = {"# Today (2)", "b.txt", "a.txt", "# Yesterday (1)", "c.txt", "# " + old_year + " (1)", "d.txt"};
    check(wait_for([&]() { return shown(p) == want; }), "sorting by name the other way sorts each group, the groups stay");
    p->sort_by(1, Qt::AscendingOrder);   // by size: b (1 byte) before a (3), unlike by name
    want = {"# Today (2)", "b.txt", "a.txt", "# Yesterday (1)", "c.txt", "# " + old_year + " (1)", "d.txt"};
    check(wait_for([&]() { return shown(p) == want; }), "sorting by size sorts each group by size");
    p->sort_by(3, Qt::AscendingOrder);
    check(wait_for([&]() { return shown(p).value(0) == "# " + old_year + " (1)" && shown(p).value(4) == "# Today (2)"; }),
          "sorting by Modified, oldest first, puts the oldest group first");
    p->sort_by(0, Qt::AscendingOrder);
    want = {"# Today (2)", "a.txt", "b.txt", "# Yesterday (1)", "c.txt", "# " + old_year + " (1)", "d.txt"};
    check(wait_for([&]() { return shown(p) == want; }), "...and any other sort puts the newest first again");
    check(p->all_paths() == QStringList({join(fs, "a.txt"), join(fs, "b.txt"), join(fs, "c.txt"), join(fs, "d.txt")}),
          "the folder's items are listed in the order shown, without the headers");

    // Group by Day: a header per date
    QLocale loc;
    auto day = [&](const QDate &d, const QString &prefix = QString()) { return "# " + prefix + loc.toString(d, QLocale::LongFormat); };
    p->set_grouping(groups::DAY, groups::MODIFIED);
    want = {day(QDate::currentDate(), "Today, ") + " (2)", "a.txt", "b.txt", day(yesterday.date(), "Yesterday, ") + " (1)",
            "c.txt", day(old.date()) + " (1)", "d.txt"};
    check(wait_for([&]() { return shown(p) == want; }), "Group by Day shows a header per date, newest first");
    // Date Created: these files were all made just now, whatever their modification times
    struct statx sx;
    bool births = ::statx(AT_FDCWD, enc(join(fs, "d.txt")).constData(), 0, STATX_BTIME, &sx) == 0 && (sx.stx_mask & STATX_BTIME);
    if (births) {
        p->set_grouping(groups::PERIOD, groups::CREATED);
        check(wait_for([&]() { return shown(p) == QStringList({"# Today (4)", "a.txt", "b.txt", "c.txt", "d.txt"}); }),
              "grouping by Date Created uses when the files were made, not modified");
    } else {
        skip("grouping by Date Created (this file system keeps no creation times)");
    }
    p->set_grouping(groups::PERIOD, groups::MODIFIED);
    want = {"# Today (2)", "a.txt", "b.txt", "# Yesterday (1)", "c.txt", "# " + old_year + " (1)", "d.txt"};
    check(wait_for([&]() { return shown(p) == want; }), "...and back to the date modified");

    // headers: neither selected nor opened
    p->view()->selectAll();
    check(p->selected_paths().size() == 4 && !p->view()->selectionModel()->isSelected(header(p, "Today")),
          "Select All selects the items, not the headers");
    p->view()->clearSelection();

    // collapsing
    Q_EMIT p->view()->clicked(header(p, "Today"));
    want = {"# Today (2)", "# Yesterday (1)", "c.txt", "# " + old_year + " (1)", "d.txt"};
    check(shown(p) == want, "clicking a header collapses its group");
    check(header(p, "Today").data(GroupFoldedRole).toBool() && p->all_paths().size() == 4,
          "...the header says so, and the folder still counts its items");
    p->select_paths({join(fs, "b.txt")});
    check(shown(p).size() == 7 && p->selected_paths() == QStringList({join(fs, "b.txt")}),
          "selecting an item of a collapsed group opens the group");
    p->view()->clearSelection();
    Q_EMIT p->view()->clicked(header(p, "Yesterday"));
    Q_EMIT p->view()->clicked(header(p, "Yesterday"));
    check(shown(p).size() == 7, "clicking it again expands it");
    p->view()->setCurrentIndex(header(p, "Yesterday"));
    p->view()->clearSelection();
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(p->view(), &enter);
    check(shown(p).size() == 6, "Enter on a header collapses its group");
    QApplication::sendEvent(p->view(), &enter);
    check(shown(p).size() == 7, "...and expands it");

    // collapsing from anywhere in a group: its footer, Ctrl+Left on an item, the menus
    QString y = "Yesterday (1)", o = old_year + " (1)";
    want = {"# Today (2)", "a.txt", "b.txt", "^ Collapse Today (2)", "# " + y, "c.txt", "^ Collapse " + y, "# " + o, "d.txt",
            "^ Collapse " + o};
    check(shown(p, true) == want, "each open group ends with a footer that names it and counts its items");
    p->view()->selectAll();
    QModelIndex foot = footer(p, y);
    check(foot.isValid() && !(foot.flags() & Qt::ItemIsSelectable) && !p->view()->selectionModel()->isSelected(foot) &&
              p->selected_paths().size() == 4,
          "...and isn't selectable (Select All leaves it out)");
    p->view()->clearSelection();
    Q_EMIT p->view()->clicked(foot);
    want = {"# Today (2)", "a.txt", "b.txt", "^ Collapse Today (2)", "# " + y, "# " + o, "d.txt", "^ Collapse " + o};
    check(shown(p, true) == want && p->view()->currentIndex() == header(p, "Yesterday"),
          "clicking a footer collapses its group (without a footer then), and its header becomes the current row");
    Q_EMIT p->view()->clicked(header(p, "Yesterday"));
    p->view()->setCurrentIndex(item_of(p, "b.txt"));
    QKeyEvent ctrl_left(QEvent::KeyPress, Qt::Key_Left, Qt::ControlModifier);
    QApplication::sendEvent(p->view(), &ctrl_left);
    bool left_folded = header(p, "Today").data(GroupFoldedRole).toBool() && p->view()->currentIndex() == header(p, "Today");
    QKeyEvent ctrl_right(QEvent::KeyPress, Qt::Key_Right, Qt::ControlModifier);
    QApplication::sendEvent(p->view(), &ctrl_right);
    {   // Ctrl let go: Qt keeps the modifiers of the last key event (a drop or a click would think it still held)
        QWidget elsewhere;
        QKeyEvent plain(QEvent::KeyPress, Qt::Key_F35, Qt::NoModifier);
        QApplication::sendEvent(&elsewhere, &plain);
    }
    check(left_folded && !header(p, "Today").data(GroupFoldedRole).toBool() && shown(p).size() == 7,
          "Ctrl+Left on an item collapses its group (its header current), Ctrl+Right expands it again");
    QMenu *im = w->build_menu(p, {join(fs, "c.txt")});
    QAction *this_group = action_in(im, "Collapse This Group");
    if (this_group)
        this_group->trigger();
    delete im;
    check(this_group && header(p, "Yesterday").data(GroupFoldedRole).toBool() &&
              !header(p, "Today").data(GroupFoldedRole).toBool(),
          "an item's right-click menu has Collapse This Group, for its own group");
    QMenu *bg = w->build_menu(p, {});
    QAction *all = action_in(bg, "Collapse All Groups"), *none = action_in(bg, "Expand All Groups");
    if (all)
        all->trigger();
    QStringList all_shut = shown(p, true);
    if (none)
        none->trigger();
    delete bg;
    check(all && none && all_shut == QStringList({"# Today (2)", "# " + y, "# " + o}) && shown(p).size() == 7,
          "empty space's menu collapses all groups (only their headers) and expands them all");
    QMenu *bg2 = w->build_menu(p, {});
    QAction *sort_by = action_in(bg2, "Sort By");
    QMenu *sm = sort_by ? sort_by->menu() : nullptr;   // the Sort menu's items (MainWindow::add_sort_items)
    check(sm && action_in(sm, "Collapse All Groups") && action_in(sm, "Expand All Groups"),
          "...and so does the Sort menu");
    delete bg2;

    // the grid: a header takes a line of its own
    p->set_view_mode("grid");
    QListView *g = p->grid;
    p->resize(900, 600);
    auto rect_of = [&](const QString &name) {
        for (int r = 0; r < g->model()->rowCount(); ++r) {
            QModelIndex i = g->model()->index(r, 0);
            if (basename(i.data(PathRole).toString()) == name)
                return g->visualRect(i);
        }
        return QRect();
    };
    auto grid_ok = [&]() {
        QRect h = g->visualRect(header(p, "Yesterday")), a = rect_of("a.txt"), b = rect_of("b.txt"), c = rect_of("c.txt");
        return h.width() >= g->viewport()->width() - 30 && a.top() == b.top() && b.left() > a.left() &&
               h.top() >= a.bottom() && c.top() >= h.bottom() && c.left() == a.left();
    };
    check(wait_for(grid_ok), "in the grid, a header spans the width and its group starts on a new line");
    p->set_view_mode("list");
    check(wait_for([&]() { return p->tree->isFirstColumnSpanned(header(p, "Today").row(), QModelIndex()); }) &&
              !p->tree->isFirstColumnSpanned(header(p, "Today").row() + 1, QModelIndex()),
          "in the list, a header spans the columns and the items don't");

    // changes in the folder
    write_text(join(fs, "e.txt"), "x");
    check(wait_for([&]() { return shown(p).mid(0, 4) == QStringList({"# Today (3)", "a.txt", "b.txt", "e.txt"}); }),
          "a new file appears in Today");
    ::unlink(join(fs, "d.txt").toUtf8().constData());
    check(wait_for([&]() { return !shown(p).contains("d.txt") && !shown(p).join(",").contains(old_year); }),
          "a group whose last item goes goes too");
    p->select_paths({join(fs, "c.txt")});
    write_text(join(fs, "c.txt"), "changed");
    check(wait_for([&]() { return shown(p).mid(0, 5) == QStringList({"# Today (4)", "a.txt", "b.txt", "c.txt", "e.txt"}); },
                   8000),
          "a file changed today moves to Today");
    check(p->selected_paths() == QStringList({join(fs, "c.txt")}), "...and stays selected");

    // a drop on a header goes into the folder
    QString outside = home_path("outside.txt");
    write_text(outside, "x");
    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(outside)});
    p->view()->model()->dropMimeData(mime, Qt::MoveAction, -1, -1, header(p, "Today"));
    delete mime;
    check(wait_for([&]() { return isfile(join(fs, "outside.txt")) && !lexists(outside); }),
          "a file dropped on a header goes into the folder");

    // per folder, like the sort
    check(QJsonDocument::fromJson(read_file(join(CONFIG_DIR(), "sorts.json"))).object().value(fs).toArray() ==
              QJsonArray({0, 0, groups::PERIOD, groups::MODIFIED}),
          "grouping is saved with the folder's sort");
    p->set_path(other);
    check(!p->grouping() && wait_for([&]() { return shown(p) == QStringList({"x.txt"}); }),
          "another folder isn't grouped");
    p->set_path(fs);
    check(p->grouping() == groups::PERIOD && wait_for([&]() { return shown(p).value(0).startsWith("# Today"); }),
          "coming back, the folder is grouped again");
    w->set_default_sort(p);
    p->set_path(other);
    check(p->grouping() == groups::PERIOD, "Set as Default makes folders without their own sort grouped");

    // Date Taken: read from the files on a worker, then regrouped; a file without one goes by its date created
    p->set_path(media);
    p->set_grouping(groups::PERIOD, groups::TAKEN);
    want = {"# Today (2)", "bare.jpg", "notes.txt", "# 2019 (1)", "photo.jpg", "# 2018 (1)", "clip.mp4", "# 2017 (1)", "shot.png"};
    check(wait_for([&]() { return shown(p) == want; }),
          "grouping by Date Taken puts photos and videos under when they were taken, the rest under when they were made");
    check(p->grouping_by() == groups::TAKEN &&
              QJsonDocument::fromJson(read_file(join(CONFIG_DIR(), "sorts.json"))).object().value(media).toArray().at(3).toInt() ==
                  groups::TAKEN,
          "...which is saved with the folder's sort");

    // a place: Starred lists files from anywhere (the results model)
    places::set_starred({join(fs, "a.txt"), join(other, "x.txt")}, true);
    set_mtime(join(other, "x.txt"), yesterday);
    p->set_path(places::STARRED);
    p->set_grouping(groups::PERIOD, groups::MODIFIED);
    check(wait_for([&]() { return shown(p) == QStringList({"# Today (1)", "a.txt", "# Yesterday (1)", "x.txt"}); }),
          "Starred is grouped by date too");
    p->set_grouping(groups::NONE, groups::MODIFIED);
    check(wait_for([&]() { return shown(p).size() == 2 && !shown(p).join(",").contains("#"); }),
          "turning it off shows the plain list again");

    // collapsing a big group from its footer keeps the view on its header (above, out of view, before)
    {
        QString big = home_path("big");
        makedirs(big, true);
        for (int i = 0; i < 450; ++i) {
            QString f = join(big, QString("f%1.txt").arg(i, 3, 10, QChar('0')));
            write_text(f, "x");
            set_mtime(f, i < 150 ? now : i < 300 ? yesterday : old);
        }
        p->set_path(big);
        p->set_view_mode("list");
        p->set_grouping(groups::PERIOD, groups::MODIFIED);
        wait_for([&]() { return footer(p, "Yesterday (150)").isValid(); }, 10000);
        QModelIndex end = footer(p, "Yesterday (150)");
        p->view()->scrollTo(end, QAbstractItemView::PositionAtBottom);
        QApplication::processEvents();
        bool header_hidden = !p->view()->viewport()->rect().intersects(p->view()->visualRect(header(p, "Yesterday")));
        Q_EMIT p->view()->clicked(end);
        QApplication::processEvents();
        QRect h = p->view()->visualRect(header(p, "Yesterday"));
        check(header_hidden && header(p, "Yesterday").data(GroupFoldedRole).toBool() &&
                  p->view()->viewport()->rect().contains(h) && p->view()->currentIndex() == header(p, "Yesterday"),
              "collapsing a long group from its footer brings its header into view, as the current row");
    }
    finish();
}
