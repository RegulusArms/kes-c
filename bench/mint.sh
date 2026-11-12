#!/usr/bin/env bash
# Benchmark Kestrel Explorer against Nemo on Linux Mint, in a clean Docker container that holds nothing of this
# machine's but the source (bench/container.sh; see bench/README.md, "On Linux Mint"). Takes bench/run.sh's options,
# and --release (Mint's version, default 22.3); measures Kestrel and Nemo unless --apps says otherwise.
#   bench/mint.sh                      build the image afresh, run everything, write bench/results-mint.md
#   bench/mint.sh --release 23         on Linux Mint 23 instead
#   bench/mint.sh --only startup,copy  re-run some measurements and keep the rest from the last run
#   bench/mint.sh --report             rebuild bench/results-mint.md from the last results
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/container.sh"
image=kes-bench-mint
release=22.3
args=()
while (($#)); do
    case "$1" in
        --release) release=${2:?"--release needs a version, such as 22.3"}; shift 2 ;;
        --release=*) release=${1#--release=}; shift ;;
        *) args+=("$1"); shift ;;
    esac
done
[[ "$release" =~ ^[0-9]+(\.[0-9]+)?$ ]] || { echo "--release: a version such as 22.3 or 23, not $release" >&2; exit 2; }
fresh_image "$image" "$bench/mint" --build-arg "RELEASE=$release"
[[ " ${args[*]-} " == *" --apps"* ]] || args=(--apps kestrel,nemo "${args[@]}")
# Mint's image keeps Ubuntu's /etc/os-release, so the report is told which Mint it is, and on which Ubuntu
base=$(docker run --rm --network none "$image" bash -c '. /etc/os-release; echo "$VERSION_ID"')
name="$image-$(date +%s)"   # a new name each run (one that couldn't be removed is left aside: destroyed next time)
# The data and the throwaway homes are in /tmp, a RAM disk; the move between drives goes to /dev/shm, another one.
# The desktop is Cinnamon's, so Qt (Kestrel) loads its GTK integration and both apps use Mint's theme, as on Mint.
send_source results-mint | docker run -i "${RUN_OPTS[@]}" --name "$name" \
    --tmpfs /tmp:rw,exec,size=6g,mode=1777 --shm-size=2g -e LANG=C.UTF-8 -e KESTREL_BENCH_PLATFORM=Mint \
    -e KESTREL_BENCH_DISTRO="Linux Mint $release, in a Docker container (Linux Mint's image: Ubuntu $base with Mint's own packages, and the parts of its Cinnamon desktop the apps use)" \
    -e XDG_CURRENT_DESKTOP=X-Cinnamon -e DESKTOP_SESSION=cinnamon \
    "$image" bash -c "$UNPACK_AND_RUN" bench "${args[@]}" || status=$?
fetch_results "$name" results-mint
exit "${status:-0}"
