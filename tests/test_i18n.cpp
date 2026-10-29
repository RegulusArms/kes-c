// Translations (src/i18n.h, translations/): every .ts file is built into kes and keeps the placeholders of what it
// translates; English has the plural forms of every count in the code; and with the pseudo-translation installed, the
// windows and menus show no text that didn't go through tx().
#include "common.h"

#include "drives.h"
#include "jobswin.h"
#include "overview.h"
#include "prefswin.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDirIterator>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTabWidget>
#include <QTextDocument>
#include <QToolButton>
#include <QTranslator>
#include <QTreeWidget>
#include <QXmlStreamReader>

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

using namespace test;

static const QString TRANSLATIONS = QString(TESTS_DIR) + "/../translations";

struct Message {
    QString source;
    bool numerus = false, finished = false;
    QStringList forms;   // the translation, or each plural form
};

static QList<Message> read_ts(const QString &path, QString *language = nullptr)
{
    QList<Message> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QXmlStreamReader r(&f);
    Message m;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            if (r.name() == u"TS" && language)
                *language = r.attributes().value("language").toString();
            else if (r.name() == u"message")
                m = Message{QString(), r.attributes().value("numerus") == u"yes", false, {}};
            else if (r.name() == u"source")
                m.source = r.readElementText();
            else if (r.name() == u"translation") {
                QString type = r.attributes().value("type").toString();
                m.finished = type.isEmpty();
                if (!m.numerus)
                    m.forms << r.readElementText();
            } else if (r.name() == u"numerusform")
                m.forms << r.readElementText();
        } else if (r.isEndElement() && r.name() == u"message") {
            out << m;
        }
    }
    return out;
}

// the placeholders a text holds: %1…%99 and %n (as %Ln or %n), sorted
static QStringList placeholders(const QString &text)
{
    static const QRegularExpression re("%%|%L?(\\d{1,2}|n)");
    QStringList out;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        auto m = it.next();
        if (m.captured(0) != "%%")
            out << m.captured(1);
    }
    out.sort();
    return out;
}

// what a window shows: its labels, buttons, tabs, combo box items, placeholders, tooltips, header labels and the rows
// of its lists (plain text, markup taken out)
static void texts_of(QWidget *top, QStringList &out)
{
    auto plain = [](const QString &t) {
        if (!Qt::mightBeRichText(t))
            return t;
        QTextDocument d;
        d.setHtml(t);
        return d.toPlainText();
    };
    QList<QWidget *> all = top->findChildren<QWidget *>();
    all.prepend(top);
    for (QWidget *w : all) {
        if (!w->isVisibleTo(top))
            continue;
        out << plain(w->toolTip());
        if (auto *l = qobject_cast<QLabel *>(w))
            out << plain(l->text());
        else if (auto *b = qobject_cast<QAbstractButton *>(w))
            out << b->text();
        else if (auto *g = qobject_cast<QGroupBox *>(w))
            out << g->title();
        else if (auto *e = qobject_cast<QLineEdit *>(w))
            out << e->placeholderText();
        else if (auto *c = qobject_cast<QComboBox *>(w)) {
            for (int i = 0; i < c->count(); ++i)
                out << c->itemText(i);
        } else if (auto *tw = qobject_cast<QTabWidget *>(w)) {
            for (int i = 0; i < tw->count(); ++i)
                out << tw->tabText(i);
        } else if (auto *tree = qobject_cast<QTreeWidget *>(w)) {
            for (int c = 0; c < tree->columnCount(); ++c)
                out << tree->headerItem()->text(c);
            for (int i = 0; i < std::min(tree->topLevelItemCount(), 50); ++i)
                for (int c = 0; c < tree->columnCount(); ++c)
                    out << tree->topLevelItem(i)->text(c);
        }
    }
}

static void texts_of(const QMenu *m, QStringList &out)
{
    for (QAction *a : m->actions()) {
        out << a->text() << a->toolTip();
        if (a->menu())
            texts_of(a->menu(), out);
    }
}

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    setup_app();

    // ---- every .ts file is built into kes, and no translation leaves out a placeholder of what it translates
    QStringList ts_files, unbuilt, dropped;
    for (const QString &f : QDir(TRANSLATIONS).entryList({"kestrel_*.ts"}, QDir::Files))
        ts_files << f;
    for (const QString &f : ts_files) {
        QString lang = QFileInfo(f).completeBaseName().mid(8);
        QTranslator t;
        if (!t.load(":/i18n/kestrel_" + lang + ".qm"))
            unbuilt << f;
        for (const Message &m : read_ts(join(TRANSLATIONS, f))) {
            if (!m.finished)
                continue;
            for (const QString &form : m.forms)
                if (placeholders(form) != placeholders(m.source))
                    dropped << f + ": “" + m.source.left(60) + "” → “" + form.left(60) + "”";
        }
    }
    check(ts_files.contains("kestrel_en.ts") && unbuilt.isEmpty(),
          "every translation (" + ts_files.join(", ") + ") is built into kes as :/i18n/kestrel_<language>.qm and loads" +
              (unbuilt.isEmpty() ? QString() : " (not: " + unbuilt.join(", ") + ")"));
    check(dropped.isEmpty(), "no translation drops or adds a %1 or %n of the text it translates" +
                                 (dropped.isEmpty() ? QString() : ":\n  " + dropped.mid(0, 20).join("\n  ")));

    // ---- English is loaded at startup, and turns a count's text back into English ("%n file(s)" → 1 file, 2 files)
    check(txn("%Ln file(s)", 1) == "1 file" && txn("%Ln file(s)", 2) == "2 files" && i18n::language() == "en",
          "the English translation is loaded at startup and gives a count's text its plural forms (" +
              txn("%Ln file(s)", 1) + ", " + txn("%Ln file(s)", 2) + ")");

    // ---- the code's texts as they are now (lupdate, through translations/update.sh): every count has English forms
    QString work = home_path("i18n");
    makedirs(work, true);
    QProcess up;
    up.start("bash", {join(TRANSLATIONS, "update.sh"), "--extract", join(work, "all.ts")});
    bool have_tools = up.waitForFinished(120000) && up.exitCode() == 0 && exists(join(work, "all.ts"));
    if (!have_tools) {
        skip("English plural forms and the pseudo-translation (lupdate and lrelease aren't installed: qt6-l10n-tools)");
        finish();
    }
    QSet<QString> english;
    for (const Message &m : read_ts(join(TRANSLATIONS, "kestrel_en.ts")))
        if (m.numerus && m.finished && m.forms.size() == 2 && !m.forms.contains(QString()))
            english << m.source;
    QStringList no_forms;
    int counts = 0, texts = 0;
    for (const Message &m : read_ts(join(work, "all.ts"))) {
        ++texts;
        if (!m.numerus)
            continue;
        ++counts;
        if (!english.contains(m.source))
            no_forms << m.source;
    }
    check(counts > 50 && no_forms.isEmpty(),
          QString("every count's text in the code (%1 of %2 texts) has its two English forms in kestrel_en.ts (run "
                  "translations/update.sh, then write them)")
                  .arg(counts)
                  .arg(texts) +
              (no_forms.isEmpty() ? QString() : ":\n  " + no_forms.mid(0, 20).join("\n  ")));

    // ---- the pseudo-translation: every window and menu shown with it installed says nothing in plain English
    up.start("bash", {join(TRANSLATIONS, "update.sh"), "--pseudo", join(work, "pseudo.qm")});
    QTranslator pseudo;
    bool made = up.waitForFinished(120000) && up.exitCode() == 0 && pseudo.load(join(work, "pseudo.qm"));
    QCoreApplication::installTranslator(&pseudo);
    check(made && tx("Open") == "[Öρéñ··]" && txn("%Ln file(s)", 3).startsWith("[3 "),
          "the pseudo-translation is made, and with it installed texts come from it, with their counts (" + tx("Open") +
              ", " + txn("%Ln file(s)", 3) + ")");

    QString dir = home_path("place");
    makedirs(join(dir, "sub"), true);
    write_text(join(dir, "note.txt"), "x");
    QStringList shown;
    MainWindow *w = open_window({dir});
    wait_for([&]() { return w->pane()->all_paths().size() == 2; });
    spin(300);
    texts_of(w, shown);
    for (QToolButton *b : w->findChildren<QToolButton *>())   // the main menu and the Sort menu (filled as it opens)
        if (b->menu()) {
            Q_EMIT b->menu()->aboutToShow();
            texts_of(b->menu(), shown);
        }
    for (const QStringList &sel : {QStringList{join(dir, "note.txt")}, QStringList{join(dir, "sub")},
                                   QStringList{join(dir, "note.txt"), join(dir, "sub")}, QStringList()}) {
        QMenu *m = w->build_menu(w->pane(), sel);
        texts_of(m, shown);
        delete m;
    }
    w->navigate(OVERVIEW);
    spin(500);
    texts_of(w, shown);
    w->preferences();
    auto *pd = shown_window<PreferencesDialog>();
    if (pd) {
        for (int i = 0; i < pd->tabs->count(); ++i) {
            pd->tabs->setCurrentIndex(i);
            spin(50);
            texts_of(pd, shown);
        }
        pd->close();
    }
    JobsWindow::open(w);
    if (auto *jw = shown_window<JobsWindow>()) {
        texts_of(jw, shown);
        jw->close();
    }
    for (const QString &p : {join(dir, "note.txt"), join(dir, "sub")}) {
        auto *d = new PropertiesDialog(w, {p});
        show_free(d);
        spin(300);
        for (auto *tw : d->findChildren<QTabWidget *>())
            for (int i = 0; i < tw->count(); ++i) {
                tw->setCurrentIndex(i);
                spin(50);
                texts_of(d, shown);
            }
        d->close();
    }
    // A text in plain English: a word of three letters or more left once the pseudo-translated parts ([…]), the test's
    // own names and the names that stay as they are (Kestrel, formats, programs) are taken out
    static const QRegularExpression pseudo_part("\\[[^\\[\\]]*\\]"), word("[A-Za-z]{3,}");
    static const QStringList kept = {"Kestrel", "Explorer", "place", "note", "txt", "sub", "home", "tmp", "kestrel", "test",
                                     "GNOME", "LTFS", "UWP", "BleachBit", "SHA", "CRC", "SFV", "BLAKE", "MD5", "Ctrl",
                                     "Shift", "Alt", "Del", "zip", "rar", "tar", "zpaq", "gzip", "bzip", "XZ", "LZ",
                                     "Zstandard", "lzip", "LZMA", "PPMd", "BZip", "Deflate", "Copy", "MP", "WebM", "MKV",
                                     "MOV", "GIF", "AAC", "Opus", "OGG", "Vorbis", "FLAC", "WAV", "ALAC", "dd", "exiftool",
                                     "zst", "lz4", "bz2", "gz"};
    // …and what the computer names: the mounts and their drives' labels (a runner's "cloudimg-rootfs"), the user and
    // their group (a runner's "docker"), file types (the system translates those), Qt's own buttons
    const struct passwd *pw = getpwuid(getuid());
    const struct group *gr = getgrgid(getgid());
    QStringList theirs = {pw ? QString::fromLocal8Bit(pw->pw_name) : QString(), gr ? QString::fromLocal8Bit(gr->gr_name) : QString(),
                          QMimeDatabase().mimeTypeForName("text/plain").comment(),
                          QMimeDatabase().mimeTypeForName("inode/directory").comment()};
    for (const drives::Mount &mt : drives::mounts())
        theirs << basename(mt.root) << mt.fstype << drives::device_label(mt.source) << mt.source.section('/', 0, 0)
               << QStorageInfo(mt.root).displayName();
    static const QStringList qt_buttons = {"Cancel", "OK", "Close", "Yes", "No", "Apply", "Reset", "Help"};
    QStringList english_left;
    for (QString t : shown) {
        if (qt_buttons.contains(t.remove('&')))
            continue;
        QString rest = t;
        rest.remove(pseudo_part);
        rest.remove(QRegularExpression("\\S*/\\S*"));   // paths and MIME types
        for (const QString &k : theirs)
            if (!k.isEmpty())
                rest.remove(k);
        for (const QString &k : kept)
            rest.remove(QRegularExpression("\\b" + QRegularExpression::escape(k) + "\\w*", QRegularExpression::CaseInsensitiveOption));
        if (word.match(rest).hasMatch() && !english_left.contains(t.trimmed()))
            english_left << t.trimmed();
    }
    check(shown.size() > 200 && english_left.isEmpty(),
          QString("the main window, its menus, the Overview, Preferences, the Jobs window and Properties show no "
                  "text that isn't translated (%1 texts looked at)")
                  .arg(shown.size()) +
              (english_left.isEmpty() ? QString() : ":\n  " + english_left.mid(0, 40).join("\n  ")));

    // ---- what's written or removed is the tag the row stands for, not what it shows (a translation may name its
    // group differently)
    if (!which("exiftool")) {
        skip("Properties → Metadata removes the tag a row stands for (exiftool isn't installed)");
        finish();
    }
    QString img = join(dir, "tagged.jpg");
    QImage(8, 8, QImage::Format_RGB32).save(img);
    QProcess tag;
    tag.start("exiftool", {"-q", "-overwrite_original", "-Artist=Kestrel Test", img});
    tag.waitForFinished(30000);
    auto artist = [&]() {
        QProcess r;
        r.start("exiftool", {"-s3", "-Artist", img});
        r.waitForFinished(30000);
        return QString::fromUtf8(r.readAllStandardOutput()).trimmed();
    };
    bool tagged = artist() == "Kestrel Test";
    auto *pd2 = new PropertiesDialog(w, {img});
    show_free(pd2);
    for (auto *tw : pd2->findChildren<QTabWidget *>())
        for (int i = 0; i < tw->count(); ++i)
            if (tw->tabText(i) == tx("Metadata"))
                tw->setCurrentIndex(i);
    QTreeWidgetItem *row = nullptr;
    wait_for([&]() {
        for (auto *t : pd2->findChildren<QTreeWidget *>())
            for (int i = 0; i < t->topLevelItemCount(); ++i)
                if (t->topLevelItem(i)->text(1) == "Artist")
                    row = t->topLevelItem(i);
        return row != nullptr;
    }, 30000);
    QAbstractButton *remove = nullptr;
    if (row) {
        row->setText(0, "[Ğŕöûƥ]");   // as a translation could show it
        row->treeWidget()->setCurrentItem(row);
        row->setSelected(true);
        for (auto *b : pd2->findChildren<QAbstractButton *>())
            if (b->text() == tx("Remove"))
                remove = b;
    }
    if (remove)
        remove->click();
    if (auto *box = shown_window<QMessageBox>())
        box->button(QMessageBox::Yes)->click();
    bool removed = wait_for([&]() { return artist().isEmpty(); }, 30000);
    check(tagged && row && remove && removed,
          "Properties → Metadata removes the tag a row stands for, whatever its group shows (" + artist() + ")");
    pd2->close();
    finish();
}
