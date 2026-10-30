// Loading the translation at startup, and the system's formats for numbers and dates (see i18n.h)
#include "i18n.h"

#include <QLibraryInfo>
#include <QTranslator>

namespace i18n {

static QString g_language = "en";

QString language() { return g_language; }

// Every program built with this file (kes, the tower, the tests) gets the translation as its QCoreApplication starts.
// English always (its plural forms; and what a translation leaves out falls back to it), then the UI language's on
// top when there is one; Qt's own texts (standard buttons…) in that language too. KESTREL_TEST_TRANSLATION names a
// .qm to use instead of the UI language's (the pseudo-translation: translations/update.sh --pseudo).
static void install()
{
    QCoreApplication *app = QCoreApplication::instance();
    auto *en = new QTranslator(app);
    if (en->load(":/i18n/kestrel_en.qm"))
        QCoreApplication::installTranslator(en);
    else
        delete en;
    auto *t = new QTranslator(app);
    QString test = qEnvironmentVariable("KESTREL_TEST_TRANSLATION");
    bool found = test.isEmpty() ? t->load(QLocale::system(), "kestrel", "_", ":/i18n") : t->load(test);
    if (!found || t->filePath().endsWith("/kestrel_en.qm")) {
        delete t;
        return;
    }
    QCoreApplication::installTranslator(t);
    g_language = t->language().isEmpty() ? QString("en") : t->language();
    auto *qt = new QTranslator(app);
    if (test.isEmpty() && qt->load(QLocale::system(), "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(qt);
    else
        delete qt;
}

QString number(qint64 n) { return QLocale().toString(n); }

QString decimal(double v, int decimals) { return QLocale().toString(v, 'f', decimals); }

QString date(const QDate &d)
{
    QString f = QLocale().dateFormat(QLocale::ShortFormat);
    if (!f.contains("yyyy"))
        f.replace("yy", "yyyy");   // (some short formats have a two-digit year: 10/28/26)
    return QLocale().toString(d, f);
}

QString time(const QTime &t) { return QLocale().toString(t, QLocale::ShortFormat); }

QString date_time(const QDateTime &dt) { return date(dt.date()) + " " + time(dt.time()); }

QString date_time_secs(const QDateTime &dt)
{
    QString f = QLocale().timeFormat(QLocale::ShortFormat);
    if (!f.contains("ss"))
        f.replace("mm", "mm:ss");
    return date(dt.date()) + " " + QLocale().toString(dt.time(), f);
}

QString long_date(const QDate &d) { return QLocale().toString(d, QLocale::LongFormat); }

QString month_year(const QDate &d)
{
    //: The format of a month and its year (QDate's: MMMM the month's name, yyyy the year)
    return QLocale().toString(d, tx("MMMM yyyy"));
}

Q_COREAPP_STARTUP_FUNCTION(install)

}  // namespace i18n
