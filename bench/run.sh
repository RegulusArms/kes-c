#!/usr/bin/env bash
# Benchmark Kestrel Explorer against GNOME Files, Dolphin, Thunar, Krusader and PCManFM on Ubuntu 26.04, in a clean
# Docker container that holds nothing of this machine's but the source (bench/container.sh; see bench/README.md).
#   bench/run.sh                      build the image afresh, run everything, print the tables, write bench/results.md
#   bench/run.sh --runs 5             more runs per measurement (medians are reported)
#   bench/run.sh --only startup,copy  only some measurements (the rest are kept from the last run)
#   bench/run.sh --apps thunar        only some apps (the others' results are kept from the last run)
#   bench/run.sh --report             rebuild the tables from the last results without running anything
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/container.sh"
image=kes-bench-ubuntu
fresh_image "$image" "$bench/ubuntu"
base=$(docker run --rm --network none "$image" bash -c '. /etc/os-release; echo "$PRETTY_NAME"')
name="$image-$(date +%s)"   # a new name each run (one that couldn't be removed is left aside: destroyed next time)
# The data and the throwaway homes are in /tmp, a RAM disk; the move between drives goes to /dev/shm, another one.
# The desktop is Ubuntu's GNOME, as in an Ubuntu session: Qt (Kestrel) loads its GTK integration, GTK apps Yaru.
send_source results | docker run -i "${RUN_OPTS[@]}" --name "$name" \
    --tmpfs /tmp:rw,exec,size=6g,mode=1777 --shm-size=2g -e LANG=C.UTF-8 \
    -e KESTREL_BENCH_DISTRO="$base, in a Docker container (Ubuntu's image with the parts of Ubuntu's desktop the apps use)" \
    -e XDG_CURRENT_DESKTOP=ubuntu:GNOME -e DESKTOP_SESSION=ubuntu \
    "$image" bash -c "$UNPACK_AND_RUN" bench "$@" || status=$?
fetch_results "$name" results
exit "${status:-0}"
