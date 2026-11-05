# Sourced by bench/run.sh, mint.sh, kubuntu.sh and fs.sh: each benchmark runs in a clean new container that's
# destroyed once it's done, and that holds nothing of this machine's but the project's source.
#   - Clean: any container left from an earlier run is destroyed first (stopped, or still running: --init's tini ends
#     at a TERM sent from inside, where the snap's docker can refuse a stop), and the image is built again from a
#     freshly pulled base with nothing cached, so no state or old package carries over.
#   - Sterile: nothing of the machine is mounted. The source goes in as a tar on the container's stdin (send_source):
#     only the files git has (tracked, and new ones not ignored), never ignored ones (local notes, settings, builds,
#     logs) nor .git (its history, names and emails), with no owner names, xattrs or ACLs; plus the last results
#     (measurements only), so --only and --apps can keep the others. Runs have no network (--network none: loopback
#     only), and get no variables but the ones given.
bench=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname "$bench")

destroy() {   # IMAGE: every container of that image's runs
    local c
    for c in $(docker ps -aq --filter "name=^$1-"); do
        [[ "$(docker inspect -f '{{.State.Running}}' "$c")" == true ]] && docker exec -u root "$c" kill -TERM 1 2>/dev/null || true
        for _ in $(seq 50); do [[ "$(docker inspect -f '{{.State.Running}}' "$c" 2>/dev/null)" == true ]] || break; sleep 0.2; done
        docker rm -f "$c" >/dev/null 2>&1 || echo "Couldn't remove the container $c (sudo snap restart docker, then docker rm $c)" >&2
    done
}

fresh_image() {   # IMAGE DIR [docker build options]: leftovers destroyed, the image built afresh, runs destroyed at exit
    local image=$1 dir=$2
    shift 2
    destroy "$image"
    docker build -q --pull --no-cache "$@" -t "$image" "$dir" >/dev/null
    trap "destroy $image" EXIT
    apparmor_profile "$image"
}

send_source() {   # RESULTS: a tar of the source to stdout, with bench/RESULTS.json (if any) as bench/results.json
    local results=$1 f new=()
    {
        while IFS= read -r -d '' f; do
            [[ -e "$root/$f" || -L "$root/$f" ]] || continue   # (deleted, not yet committed)
            printf '%s\0' "$f"
        done < <(git -C "$root" ls-files -z --cached --others --exclude-standard)
        [[ -f "$bench/$results.json" ]] && printf '%s\0' "bench/$results.json"
    } | tar -C "$root" -c --null -T - --owner=0 --group=0 --numeric-owner --no-xattrs --no-acls --no-selinux \
            --transform "s|^bench/$results\.json\$|bench/results.json|" -f -
    mapfile -d '' new < <(git -C "$root" ls-files -z --others --exclude-standard)
    ((${#new[@]} == 0)) || echo "Also sent, not in git yet: ${new[*]}" >&2
}

fetch_results() {   # CONTAINER RESULTS: its results.json and .md, as bench/RESULTS.json and .md
    local f
    for f in json md; do
        docker cp "$1:/home/tester/kes-c/bench/results.$f" "$bench/$2.$f" 2>/dev/null || true
    done
    [[ -f "$bench/$2.md" ]] && echo "Results: $bench/$2.md (raw: $2.json)"
    return 0
}

# How every run's container starts: --init (so it can be stopped from inside, above), no network, and room for the
# sandboxes desktop apps make for their helpers: GNOME's thumbnailers and glycin's image loaders run under bwrap,
# which needs user namespaces (blocked by Docker's seccomp profile) and to mount its own /proc (masked by Docker, and
# mounts are denied by Docker's AppArmor profile), as an Ubuntu session lets it. These loosen what the container may
# do inside itself, not what it sees: still no mount of the machine's files and no network. AppArmor: our own
# profile (bench/apparmor/kes-bench), not unconfined, which would let the host's profiles for the same programs
# attach inside the container (and fail there: Papers' can't start); used where the host has AppArmor at all.
RUN_OPTS=(--init --network none --security-opt seccomp=unconfined --security-opt systempaths=unconfined)

apparmor_profile() {   # IMAGE: kes-bench loaded (with sudo when it isn't, or has changed) and added to RUN_OPTS
    [[ -d /sys/kernel/security/apparmor ]] || return 0
    local src="$bench/apparmor/kes-bench" dst=/etc/apparmor.d/kes-bench
    if ! cmp -s "$src" "$dst" || ! docker run --rm --network none --security-opt apparmor=kes-bench "$1" true >/dev/null 2>&1; then
        echo "Loading the benchmark's AppArmor profile ($dst, from bench/apparmor/kes-bench; needs sudo)…" >&2
        sudo install -m 644 "$src" "$dst"
        sudo apparmor_parser -r -W "$dst"
    fi
    RUN_OPTS+=(--security-opt apparmor=kes-bench)
}

# What a container runs as its user (tester) to unpack the source from stdin into ~/kes-c and run bench.py there.
UNPACK_AND_RUN='mkdir kes-c && tar -x -C kes-c -f - && KESTREL_BENCH_CONTAINER=1 exec /usr/bin/python3 kes-c/bench/bench.py "$@"'
