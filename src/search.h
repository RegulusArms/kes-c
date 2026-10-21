// The search bar's Filters and how it searches inside files (widgets.h: SearchThread does the searching).
#pragma once

#include <QDate>
#include <QString>

// What a search keeps besides the name (the search bar's Filters, what `find` does): the type, the size, and when an
// item was modified or created. Checked on the search's thread; types by the file's name (nothing is read).
struct SearchFilter {
    enum Type { ANY_TYPE, IMAGES, VIDEOS, AUDIO, DOCUMENTS, ARCHIVES, FOLDERS };
    enum Size { ANY_SIZE, OVER, UNDER };
    enum When { ANY_TIME, TODAY, THIS_WEEK, THIS_MONTH, THIS_YEAR, RANGE };
    int type = ANY_TYPE;
    int size_mode = ANY_SIZE;
    qint64 size = 0;            // bytes
    int when = ANY_TIME;
    QDate from, to;             // RANGE: both days included
    bool by_created = false;    // the date: created (birth time; modified where the file system keeps none)
    bool active() const { return type != ANY_TYPE || size_mode != ANY_SIZE || when != ANY_TIME; }
    bool matches(const QString &path, bool is_dir, qint64 size, qint64 mtime, const QDate &today) const;
    bool operator==(const SearchFilter &o) const;   // the same search: fields not in use don't count
};
QString search_type_of(const QString &path);   // "images", "videos", "audio", "documents", "archives" or empty

// How a search looks inside files: the desktop's search index (localsearch: instant, but only for the folders it
// indexes, words only), or reading the files (ripgrep, else grep: any folder, regular expressions, case).
struct ContentSearch {
    enum Engine { AUTOMATIC, INDEX, READ };
    bool on = false;
    int engine = AUTOMATIC;
    bool regex = false, match_case = false;
};
// Whether the index covers a folder: under one of localsearch's recursive folders (its settings)
bool index_covers(const QString &folder);
