#!/usr/bin/env bash
# Kestrel's translations (docs/development.md "Translations").
#   translations/update.sh                 refresh every translations/kestrel_<language>.ts from the code (lupdate):
#                                          new texts added, removed ones dropped, translations kept. English holds
#                                          only the plural forms ("%n file(s)" → 1 file / 2 files).
#   translations/update.sh --extract F.ts  every text, untranslated, into F.ts
#   translations/update.sh --pseudo F.qm   the pseudo-translation, [Ǫρéñ ƒöłðéř······]: every text padded and accented,
#                                          to see missed texts and clipped layouts (KESTREL_TEST_TRANSLATION=F.qm kes).
#                                          Not shipped; the tests make one (tests/test_i18n.cpp).
# The code marks its texts with tx(), txn(), TX_NOOP() and TXN_NOOP() (src/i18n.h), all in one context, "Kestrel". lupdate knows
# only Qt's own names, so it reads a copy of the code where those calls are written out as
# QCoreApplication::translate("Kestrel", …), QT_TRANSLATE_NOOP("Kestrel", …) and QT_TRANSLATE_N_NOOP("Kestrel", …).
# Needs lupdate and lrelease (qt6-l10n-tools; LUPDATE= / LRELEASE= to use others) and perl.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"

tool() {   # lupdate / lrelease: $LUPDATE / $LRELEASE, Qt 6's in its libexec or bin folder, or on PATH
    local name=$1 var=${1^^} dir
    if [[ -n "${!var:-}" ]]; then echo "${!var}"; return; fi
    for dir in "$(qtpaths6 --query QT_HOST_LIBEXECS 2>/dev/null)" "$(qtpaths6 --query QT_HOST_BINS 2>/dev/null)" \
               /usr/lib/qt6/libexec /usr/lib/qt6/bin; do
        [[ -n "$dir" && -x "$dir/$name" ]] && { echo "$dir/$name"; return; }
    done
    command -v "$name" || { echo "update.sh: $name not found (install qt6-l10n-tools, or set $var)" >&2; exit 1; }
}
LUPDATE="$(tool lupdate)"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/src" "$tmp/translations"
# The code as lupdate reads it. i18n.h (where tx and friends are defined) is left out.
for f in "$ROOT"/src/*.cpp "$ROOT"/src/*.h; do
    [[ $(basename "$f") == i18n.h ]] && continue
    perl -0777 -pe '
        my $lit = qr/(?:"(?:[^"\\\n]|\\.)*"\s*)+/;
        # txn(text, n): n is any expression, given to lupdate as 1 (it only needs to know it is a count; and Qt 6.4
        # lupdate hangs on some, such as r.value(0).toLongLong())
        my $expr = qr/(?:[^()"]++|"(?:[^"\\]|\\.)*"|(\((?:[^()"]++|"(?:[^"\\]|\\.)*"|(?-1))*\)))*/;
        s/(?<![\w:.>])txn\(\s*($lit),$expr\)/QCoreApplication::translate("Kestrel", $1, "", 1)/g;
        s/(?<![\w:.>])tx\(/QCoreApplication::translate("Kestrel", /g;
        s/(?<![\w:.>])TX_NOOP\(/QT_TRANSLATE_NOOP("Kestrel", /g;
        s/(?<![\w:.>])TXN_NOOP\(/QT_TRANSLATE_N_NOOP("Kestrel", /g;
    ' "$f" >"$tmp/src/$(basename "$f")"
done

lupdate_into() {   # TS-FILE [lupdate options…]: refresh it from the copy (its locations relative to translations/)
    local ts=$1
    shift
    "$LUPDATE" -silent -locations relative -no-obsolete "$@" -extensions cpp,h "$tmp/src" -ts "$ts"
}

case "${1:-}" in
"")
    for ts in "$HERE"/kestrel_*.ts; do
        cp "$ts" "$tmp/translations/"
        t="$tmp/translations/$(basename "$ts")"
        if [[ $(basename "$ts") == kestrel_en.ts ]]; then   # (no locations: they'd change with every edit)
            lupdate_into "$t" -pluralonly -locations none
        else
            lupdate_into "$t"
        fi
        cp "$t" "$ts"
        echo "Updated $(realpath --relative-to="$ROOT" "$ts")"
    done
    ;;
--extract)
    out=$(realpath -m "${2:?--extract needs a file}")
    lupdate_into "$tmp/translations/all.ts" -target-language en
    cp "$tmp/translations/all.ts" "$out"
    ;;
--pseudo)
    out=$(realpath -m "${2:?--pseudo needs a file}")
    lupdate_into "$tmp/translations/all.ts" -target-language en
    # Each text: its letters accented, a third longer (·), in [ ]. Kept as they are: XML entities (&amp; &lt;…), rich
    # text tags (&lt;b&gt;), and %1 %n %L1 %% (and a progress bar's %v %m %p).
    perl -CSD -0777 -Mutf8 -pe '
        my %acc = (a=>"á",b=>"ƀ",c=>"ç",d=>"ð",e=>"é",f=>"ƒ",g=>"ğ",h=>"ĥ",i=>"í",j=>"ĵ",k=>"ķ",l=>"ł",m=>"ɱ",
                   n=>"ñ",o=>"ö",p=>"ρ",q=>"ǫ",r=>"ř",s=>"š",t=>"ţ",u=>"ü",v=>"ṽ",w=>"ŵ",x=>"ẋ",y=>"ý",z=>"ž",
                   A=>"Á",B=>"Ɓ",C=>"Ç",D=>"Ð",E=>"É",F=>"Ƒ",G=>"Ğ",H=>"Ĥ",I=>"Í",J=>"Ĵ",K=>"Ķ",L=>"Ł",M=>"Ḿ",
                   N=>"Ñ",O=>"Ö",P=>"Ƥ",Q=>"Ǫ",R=>"Ř",S=>"Š",T=>"Ţ",U=>"Ü",V=>"Ṽ",W=>"Ŵ",X=>"Ẋ",Y=>"Ý",Z=>"Ž");
        sub pseudo {
            my $s = shift;
            my ($out, $letters) = ("", 0);
            for my $tok (split /(&lt;\/?[A-Za-z][^&]*?&gt;|&#?\w+;|%L?\d+|%L?n|%[vmp]\b|%%)/, $s) {
                if ($tok =~ /^(?:&lt;\/?[A-Za-z][^&]*?&gt;|&#?\w+;|%L?\d+|%L?n|%[vmp]\b|%%)$/) { $out .= $tok; next; }
                $letters += () = $tok =~ /[A-Za-z]/g;
                $tok =~ s/([A-Za-z])/$acc{$1}/g;
                $out .= $tok;
            }
            return "[" . $out . ("·" x int(($letters + 2) / 3)) . "]";
        }
        s{(<message[^>]*>.*?<source>(.*?)</source>.*?)<translation type="unfinished">(.*?)</translation>}{
            my ($head, $src, $tr) = ($1, $2, $3);
            my $p = pseudo($src);
            if ($tr =~ /numerusform/) { $tr =~ s{<numerusform></numerusform>}{<numerusform>$p</numerusform>}g; }
            else { $tr = $p; }
            "$head<translation>$tr</translation>"
        }gse;
    ' "$tmp/translations/all.ts" >"$tmp/translations/pseudo.ts"
    "$(tool lrelease)" -silent "$tmp/translations/pseudo.ts" -qm "$out"
    ;;
*)
    sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
    ;;
esac
