#!/usr/bin/env bash
# Benchmark Kestrel Explorer against Dolphin on Kubuntu 26.04, in a clean Docker container that holds nothing of this
# machine's but the source (bench/container.sh; see bench/README.md, "On Kubuntu"). Takes bench/run.sh's options;
# measures Kestrel and Dolphin unless --apps says otherwise.
#   bench/kubuntu.sh                      build the image afresh, run everything, write bench/results-kubuntu.md
#   bench/kubuntu.sh --only startup,copy  re-run some measurements and keep the rest from the last run
#   bench/kubuntu.sh --report             rebuild bench/results-kubuntu.md from the last results
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/container.sh"
image=kes-bench-kubuntu
fresh_image "$image" "$bench/kubuntu"
args=("$@")
[[ " $* " == *" --apps"* ]] || args=(--apps kestrel,dolphin "${args[@]}")
name="$image-$(date +%s)"   # a new name each run (one that couldn't be removed is left aside: destroyed next time)
# The data and the throwaway homes are in /tmp, a RAM disk; the move between drives goes to /dev/shm, another one.
# The desktop is KDE's, so Qt apps (Kestrel and Dolphin) load Plasma's integration and Breeze, as on Kubuntu.
send_source results-kubuntu | docker run -i "${RUN_OPTS[@]}" --name "$name" \
    --tmpfs /tmp:rw,exec,size=6g,mode=1777 --shm-size=2g -e LANG=C.UTF-8 -e KESTREL_BENCH_PLATFORM=Kubuntu \
    -e XDG_CURRENT_DESKTOP=KDE -e KDE_FULL_SESSION=true -e KDE_SESSION_VERSION=6 \
    "$image" bash -c "$UNPACK_AND_RUN" bench "${args[@]}" || status=$?
fetch_results "$name" results-kubuntu
exit "${status:-0}"
