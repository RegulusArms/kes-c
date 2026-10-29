#pragma once
// Translating Kestrel's texts. Every text the user sees goes through tx() (or txn() for a count), all in one context,
// "Kestrel"; translations/update.sh collects them into translations/kestrel_<language>.ts (docs/development.md
// "Translations"). Only English ships: its file holds just the plural forms ("%n file(s)" → "1 file" / "2 files").
//
// - tx("Open"): a text. tx("Open", "verb") when the same English needs two translations (the second is a note for
//   translators, not shown).
// - txn("%n file(s)", n): a count; the translation picks the form for n. %Ln instead of %n groups its digits (1,234).
// - TX_NOOP("Name"): marks a text kept in a table (a list of labels) that is passed through tx() where it's shown;
//   TXN_NOOP("%n file(s)") one with a count, passed through txn().
// - A sentence with parts: one text with %1 %2… filled by one .arg(a, b) call, so a translation can reorder them.
//
// Not for settings keys, ids, file names, command lines, or what's passed to other programs.
//
// Numbers, sizes and dates follow the system's formats (LC_NUMERIC, LC_TIME, as GNOME does), through QLocale() (the
// system locale unless a test sets another).

#include <QCoreApplication>
#include <climits>
#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QString>

// (the names are what update.sh looks for: keep them, and keep each one's first argument a string literal)
inline QString tx(const char *text, const char *disambiguation = nullptr)
{
    return QCoreApplication::translate("Kestrel", text, disambiguation);
}
// a text kept in a table (marked with TX_NOOP where it's written)
inline QString tx(const QString &text) { return tx(text.toUtf8().constData()); }
inline QString txn(const char *text, qint64 n)
{
    return QCoreApplication::translate("Kestrel", text, nullptr, int(qBound(qint64(INT_MIN), n, qint64(INT_MAX))));
}
#define TX_NOOP(text) text
#define TXN_NOOP(text) text   // …a count's text kept in a table, passed through txn() where it's shown

namespace i18n {

QString number(qint64 n);                       // 1,234 (the system's digit grouping)
QString decimal(double v, int decimals);        // 1.5 (the system's decimal mark)
QString date(const QDate &d);                   // the system's short date, with a four-digit year
QString date_time(const QDateTime &dt);         // that date and the time (hours and minutes)
QString date_time_secs(const QDateTime &dt);    // …and seconds
QString time(const QTime &t);                   // hours and minutes, in the system's 12 or 24 hours
QString long_date(const QDate &d);              // with the weekday and the month's name: Wednesday, 28 October 2026
QString month_year(const QDate &d);             // October 2026

// The UI language's translation ("en" when there's none for it), as loaded at startup
QString language();

}  // namespace i18n
