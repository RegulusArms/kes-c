#!/usr/bin/env bash
# The .deb, installed on a clean system: it installs with its dependencies, kes, the admin helper and kes-setup run,
# its files are in place, the menu entry is valid, and removing it leaves nothing behind. Needs root and a throwaway
# system: CI runs it in a fresh ubuntu:24.04 container. Locally:
#   docker run --rm -v "$PWD":/src:ro ubuntu:24.04 /src/tests/check_package.sh /src/kestrel-explorer_….deb
set -uo pipefail
DEB="$(realpath "${1:?usage: check_package.sh kestrel-explorer_VERSION_amd64.deb}")"
fails=0
check() { if eval "$1"; then echo "PASS $2"; else echo "FAIL $2"; fails=$((fails + 1)); fi; }
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq desktop-file-utils >/dev/null 2>&1

check 'apt-get install -y -qq --no-install-recommends "$DEB" >/tmp/install.log 2>&1 || { tail -20 /tmp/install.log; false; }' \
    "the package installs, with its dependencies"
check '! ldd /usr/bin/kes /usr/bin/kes-admin-helper | grep -q "not found"' "every library kes and the admin helper need is there"
check '[[ "$(kes --version)" == "Kestrel Explorer "* ]]' "kes starts and reports its version"
check '[[ "$(kes-admin-helper </dev/null | head -1)" == *"\"hello\":true"* ]]' "the admin helper starts"
check 'kes-setup --help >/dev/null' "kes-setup runs"
# the admin helper's rules that only apply as root (the test suite runs it as a normal user)
useradd -m kestrel-pkg-test 2>/dev/null
U="$(id -u kestrel-pkg-test)"
D="$(mktemp -d)"
chown kestrel-pkg-test "$D"
su kestrel-pkg-test -c "cp /bin/true '$D/prog' && chmod 4755 '$D/prog'"
OUT="$({ printf '%s\n' "{\"id\":1,\"op\":\"copy\",\"src\":\"$D/prog\",\"dst\":\"$D/prog-copy\"}" \
                      "{\"id\":2,\"op\":\"hardlink\",\"target\":\"/etc/shadow\",\"link\":\"$D/shadow\"}"; sleep 1; } |
        PKEXEC_UID="$U" kes-admin-helper 2>&1)"
check '[[ "$(stat -c %A "$D/prog-copy")" == -rwxr-xr-x && "$(stat -c %U "$D/prog-copy")" == root ]]' \
    "as root, the admin helper's copy of a user's set-user-ID program isn't set-user-ID"
check '[[ "$OUT" == *"isn'"'"'t yours"* && ! -e "$D/shadow" ]]' "as root, the admin helper won't hard-link a system file for the user"
check 'desktop-file-validate /usr/share/applications/kestrel-explorer.desktop' "the menu entry is valid"
check '[[ "$(ls /usr/share/icons/hicolor/*/apps/kestrel-explorer.png | wc -l)" == 8 ]]' "the app icon is installed in every size"
check '[[ -f /usr/share/xdg-desktop-portal/portals/kestrel.portal &&
       -f /usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.kestrel.service &&
       -f /usr/share/gnome-shell/extensions/kestrel-focus@regulusarms.com/metadata.json ]]' \
    "the file chooser's portal files and the drop focus extension are installed"
apt-get remove -y -qq kestrel-explorer >/dev/null 2>&1
check '! dpkg -l kestrel-explorer 2>/dev/null | grep -q "^ii" && [[ ! -e /usr/bin/kes && ! -e /usr/bin/kes-admin-helper &&
       ! -e /usr/share/applications/kestrel-explorer.desktop && ! -e /usr/share/icons/hicolor/256x256/apps/kestrel-explorer.png ]]' \
    "removing it leaves nothing behind"

if (( fails )); then echo "FAILED ($fails failed)"; exit 1; fi
echo "ALL PASSED (0 failed)"
