#!/usr/bin/env bash
# Run the test suite (see tests/README.md).
#   tests/run.sh                    every test
#   tests/run.sh fileops atc_undo   only these (names: fileops atc_sync atc_tasks atc_undo atc_tabs devices sidebar installer)
# Each test runs with a throwaway HOME on a private D-Bus session bus: your files, settings, dock and running
# Kestrel windows are never touched.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
ALL=(fileops atc_sync atc_tasks atc_undo atc_tabs devices sidebar installer)
TESTS=("$@")
(( ${#TESTS[@]} )) || TESTS=("${ALL[@]}")
for t in "${TESTS[@]}"; do
    [[ " ${ALL[*]} " == *" $t "* ]] || { echo "Unknown test: $t (tests: ${ALL[*]})" >&2; exit 2; }
done

# build the app (the tab test launches it) and the test programs, with the system cmake
CMAKE=/usr/bin/cmake
[[ -x "$CMAKE" ]] || CMAKE="$(command -v cmake)"
echo "Building…"
{ "$CMAKE" -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release && "$CMAKE" --build "$ROOT/build" -j"$(nproc)" &&
  "$CMAKE" -S "$HERE" -B "$HERE/build" && "$CMAKE" --build "$HERE/build" -j"$(nproc)"; } >"$HERE/build.log" 2>&1 ||
    { tail -30 "$HERE/build.log"; echo "Build failed (full log: tests/build.log)"; exit 1; }

export KES_CXX="$ROOT/build/kes"
KES_PY=""
[[ -x "$ROOT/../kestrel-explorer/kes" ]] && KES_PY="$(cd "$ROOT/../kestrel-explorer" && pwd)/kes"
export KES_PY

failed=()
for t in "${TESTS[@]}"; do
    echo
    echo "== $t"
    work="$(mktemp -d "${TMPDIR:-/tmp}/kestrel-test.XXXXXX")"
    mkdir -p "$work/home"
    if [[ $t == installer ]]; then
        cmd=("$HERE/test_installer.sh")
    else
        cmd=("$HERE/build/test_$t")
    fi
    env -u XDG_CONFIG_HOME -u XDG_CACHE_HOME -u XDG_DATA_HOME -u CONDA_DEFAULT_ENV \
        HOME="$work/home" QT_QPA_PLATFORM=offscreen \
        timeout 600 dbus-run-session -- "${cmd[@]}" >"$work/log" 2>&1
    rc=$?
    grep -E '^(PASS|FAIL|SKIP) ' "$work/log"
    if grep -q '^ALL PASSED' "$work/log" && (( rc == 0 )); then
        grep '^ALL PASSED' "$work/log"
    else
        failed+=("$t")
        grep -q -E '^FAILED' "$work/log" && grep -E '^FAILED' "$work/log" ||
            { echo "CRASHED or timed out (exit $rc). Last lines:"; tail -15 "$work/log"; }
    fi
    case "$work" in */kestrel-test.*) rm -rf "$work" ;; esac
done

echo
if (( ${#failed[@]} )); then
    echo "Failed: ${failed[*]}"
    exit 1
fi
echo "All ${#TESTS[@]} tests passed."
