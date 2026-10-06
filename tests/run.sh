#!/usr/bin/env bash
# Run the test suite (see tests/README.md).
#   tests/run.sh                    every test
#   tests/run.sh fileops atc_undo   only these (names: fileops atc_sync atc_tasks atc_undo atc_tabs devices sidebar theme chooser admin_helper installer)
# Each test runs with a throwaway HOME on a private D-Bus session bus: your files, settings, dock and running
# Kestrel windows are never touched.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
ALL=(fileops atc_sync atc_tasks atc_undo atc_tabs devices sidebar theme chooser admin_helper installer)
TESTS=("$@")
(( ${#TESTS[@]} )) || TESTS=("${ALL[@]}")
for t in "${TESTS[@]}"; do
    [[ " ${ALL[*]} " == *" $t "* ]] || { echo "Unknown test: $t (tests: ${ALL[*]})" >&2; exit 2; }
done

# build the app (the tab test launches it) and the test programs, with the system cmake, into build/ (or
# KESTREL_BUILD_DIR: another machine sharing this folder keeps its own build)
B="${KESTREL_BUILD_DIR:-build}"
CMAKE=/usr/bin/cmake
[[ -x "$CMAKE" ]] || CMAKE="$(command -v cmake)"
# KESTREL_SANITIZE=address,undefined (or thread): build everything with those sanitizers (CI does; give it its own
# KESTREL_BUILD_DIR). A finding stops the program, so its test fails.
SAN=(-DCMAKE_BUILD_TYPE=Release)
if [[ -n "${KESTREL_SANITIZE:-}" ]]; then
    SAN=(-DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_CXX_FLAGS=-fsanitize=$KESTREL_SANITIZE -fno-omit-frame-pointer"
         "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=$KESTREL_SANITIZE")
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=1}"   # leaks: Qt and GLib keep a lot alive
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"
    export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:second_deadlock_stack=1}"
fi
echo "Building…"
{ "$CMAKE" -S "$ROOT" -B "$ROOT/$B" "${SAN[@]}" && "$CMAKE" --build "$ROOT/$B" -j"$(nproc)" &&
  "$CMAKE" -S "$HERE" -B "$HERE/$B" "${SAN[@]}" && "$CMAKE" --build "$HERE/$B" -j"$(nproc)"; } >"$HERE/build.log" 2>&1 ||
    { tail -30 "$HERE/build.log"; echo "Build failed (full log: tests/build.log)"; exit 1; }

export KES_CXX="$ROOT/$B/kes"
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
        cmd=("$HERE/$B/test_$t")
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
    # (a desktop service started by the test, such as gvfsd-metadata, may still be writing there for a moment)
    case "$work" in */kestrel-test.*) rm -rf "$work" 2>/dev/null || { sleep 1; rm -rf "$work"; } ;; esac
done

echo
if (( ${#failed[@]} )); then
    echo "Failed: ${failed[*]}"
    exit 1
fi
echo "All ${#TESTS[@]} tests passed."
