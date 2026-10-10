// Parsing modelled on Python's: shlex_split (util.h) splits as shlex.split does, and metadata's ordered JSON reader
// reads as json.loads does. The expected results are Python's.
#include "common.h"

#include "metadata.h"
#include "stats.h"

using namespace test;

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QCoreApplication app(argc, argv);

    // -- shlex_split: (input, Python's shlex.split result)
    const QList<QPair<QString, QStringList>> words = {
        {"a b  c", {"a", "b", "c"}},
        {"'two words' x", {"two words", "x"}},
        {"\"dq \\\"inner\\\" x\"", {"dq \"inner\" x"}},
        {"a\\ b", {"a b"}},
        {"\"a\\$b\" \"c\\`d\"", {"a\\$b", "c\\`d"}},
        {"''", {""}},
        {"a '' b", {"a", "", "b"}},
        {"x\"y\"z", {"xyz"}},
        {"tab\there", {"tab", "here"}},
        {QString("nb") + QChar(0xa0) + "sp", {QString("nb") + QChar(0xa0) + "sp"}},
        {"a\\\nb", {"a\nb"}},
        {"\"back\\\\slash\"", {"back\\slash"}},
        {"'single \\ kept'", {"single \\ kept"}},
        {"-o 'x y' --flag=\"a b\"", {"-o", "x y", "--flag=a b"}},
    };
    QStringList wrong;
    for (const auto &[in, want] : words) {
        bool ok = false;
        if (shlex_split(in, &ok) != want || !ok)
            wrong << in;
    }
    check(wrong.isEmpty(), "shlex_split splits as Python's shlex.split does (quotes, escapes, empty words, spaces)" +
                               (wrong.isEmpty() ? QString() : " (differs for: " + wrong.join(" | ") + ")"));
    bool refused = true;
    for (const QString &in : {QString("unclosed 'quote"), QString("unclosed \"dq"), QString("trailing\\")}) {
        bool ok = true;
        shlex_split(in, &ok);
        refused = refused && !ok;
    }
    check(refused, "...and refuses an unclosed quote or a trailing backslash");

    // -- ordered JSON: (keys in order, as json.loads keeps them)
    QByteArray tricky = R"({"z": 1, "a\"q": "x", "été": [1, {"k": "}"}], "m:n": {"in": "]\\"}, )"
                        R"("n": -1.5e3, "t": true, "nul": null, "a\"q": "last"})";
    QList<QPair<QString, QJsonValue>> got = metadata::ordered_object(tricky);
    QStringList keys;
    for (const auto &[k, v] : got)
        keys << k;
    check(keys == QStringList({"z", "a\"q", QString::fromUtf8("été"), "m:n", "n", "t", "nul"}) &&
              got.value(4).second.toDouble() == -1500,
          "metadata JSON keeps the writer's key order (escaped quotes, \\u escapes, braces in strings, nesting)");
    QStringList exif;
    for (const auto &[k, v] : metadata::ordered_object(R"([{"SourceFile": "f", "EXIF:Make": "C", "XMP:Title": "T"}])"))
        exif << k;
    check(got.value(1).second.toString() == "last" &&
              exif == QStringList({"SourceFile", "EXIF:Make", "XMP:Title"}) &&
              metadata::ordered_object("{not json").isEmpty(),
          "...each key once with its last value (as Python), exiftool's [{…}] form, and nothing from invalid JSON");

    // -- KESTREL_STATS's report
    stats::enable();
    stats::sample("b timing (ms)", 1.0);
    stats::sample("b timing (ms)", 4.5);
    stats::count("a count", 3);
    stats::peak("c peak", 2);
    stats::peak("c peak", 7);
    stats::peak("c peak", 5);
    check(stats::summary() == "Kestrel stats (KESTREL_STATS):\n  a count: 3\n  b timing (ms): 2×, average 2.8, largest 4.5\n"
                              "  c peak: most 7\n",
          "the KESTREL_STATS report lists sorted names, counts, averages and peaks");
    finish();
}
