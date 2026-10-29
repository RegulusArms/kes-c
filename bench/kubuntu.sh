#!/usr/bin/env bash
# Benchmark Kestrel Explorer against Dolphin on Kubuntu 26.04, in a Docker container (see bench/README.md, "On
# Kubuntu"). Takes bench/run.sh's options; measures Kestrel and Dolphin unless --apps says otherwise.
#   bench/kubuntu.sh                      build the image (first time), run everything, write bench/results-kubuntu.md
#   bench/kubuntu.sh --only startup,copy  re-run some measurements and keep the rest from the last run
#   bench/kubuntu.sh --report             rebuild bench/results-kubuntu.md from the last results
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname "$here")
image=kes-bench-kubuntu
docker build -q -t "$image" "$here/kubuntu" >/dev/null

args=("$@")
[[ " $* " == *" --apps"* ]] || args=(--apps kestrel,dolphin "${args[@]}")
# a new name each run: containers can't always be removed here (docker rm is refused on some hosts)
name="kes-bench-kubuntu-$(date +%s)"
# The data and the throwaway homes are in /tmp, a RAM disk; the move between drives goes to /dev/shm, another one.
# The desktop is KDE's, so Qt apps (Kestrel and Dolphin) load Plasma's integration and Breeze, as on Kubuntu.
docker run --init --name "$name" --tmpfs /tmp:rw,exec,size=6g,mode=1777 --shm-size=2g \
    -v "$root:/src:ro" -e LANG=C.UTF-8 -e KESTREL_BENCH_PLATFORM=Kubuntu \
    -e XDG_CURRENT_DESKTOP=KDE -e KDE_FULL_SESSION=true -e KDE_SESSION_VERSION=6 \
    "$image" bash -c '
        set -e
        rsync -a --exclude=.git --exclude=/build --exclude=/build-* --exclude=/tests/build --exclude=/bench/build \
            --exclude=/bench/results.json --exclude=/bench/results.md /src/ kes-c/
        if [ -f kes-c/bench/results-kubuntu.json ]; then cp kes-c/bench/results-kubuntu.json kes-c/bench/results.json; fi
        kes-c/bench/run.sh "$@"' bench "${args[@]}" || status=$?
for f in json md; do
    docker cp "$name:/home/tester/kes-c/bench/results.$f" "$here/results-kubuntu.$f" 2>/dev/null || true
done
[ -f "$here/results-kubuntu.md" ] && echo "Results: $here/results-kubuntu.md (raw: results-kubuntu.json)"
exit "${status:-0}"
