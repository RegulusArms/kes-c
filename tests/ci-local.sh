#!/usr/bin/env bash
# Run CI's test jobs on this machine the way a GitHub runner runs them (see tests/README.md, "Running CI's tests here").
#   tests/ci-local.sh                      every job of .github/workflows/deb.yml that runs tests/run.sh, in parallel
#   tests/ci-local.sh test-noble           only these jobs
#   tests/ci-local.sh --head               test the last commit, as CI checks it out (default: the working tree)
#   tests/ci-local.sh --cpus 2 --repeat 3  fewer CPUs and three runs of the tests, to bring out timing races
#   tests/ci-local.sh --print              show what each job would run, and stop
#
# Each job's own steps are read from deb.yml and run as they are there (its `run:` blocks, its shell, its user), so a
# change to the workflow's packages or commands applies here too. What a runner is, and this imitates:
#   - a GitHub runner for a public repository has 4 vCPUs and 16 GB: each job gets 4 CPUs of its own (whole cores
#     with their second threads, as Azure's vCPUs are) and 16 GB, so nproc, the build's -j and the timing of the
#     tests' threads are a runner's, not this machine's;
#   - a job without `container:` runs on the runner's VM: here in a fresh ubuntu:<runs-on> container with what the
#     runner image has that the tests use, as the user "runner" (uid 1001, passwordless sudo), its workspace and /tmp
#     on Docker volumes (this machine's disk, as the VM's ext4) rather than the container's overlayfs;
#   - a job with `container:` runs in that image as root, with its workspace (/__w) on a volume, as Actions mounts it
#     from the VM's disk, and /tmp in the container;
#   - `uses:` steps are left out: actions/checkout is the copy of the tree, actions/cache only saves compile time;
#   - `sudo sysctl -w KEY=VALUE` can't be done in a container: it becomes a check that this machine has KEY at VALUE.
# What it can't imitate: the runner's kernel (Azure's; a container has this machine's) and the VM's block devices.
# Every run starts from freshly pulled base images and removes its containers and volumes when it ends.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
WORKFLOW="$ROOT/.github/workflows/deb.yml"
LABEL=kes-ci-local          # on every container and volume this makes (named kes-ci-<job>)
RUNNER_CPUS=4 RUNNER_MEM=16g
# What the ubuntu-24.04 runner image has installed that the tests (or the workflow's steps) use, and a plain
# ubuntu image doesn't: sudo for the steps, setfacl (the ACL copy), zstd/xz/bzip2/zip/unzip (archives, disk images),
# gnupg, git, curl, python3, file, sysctl (procps); tzdata, so that no install stops to ask for a time zone.
RUNNER_IMAGE_PACKAGES="acl bzip2 ca-certificates curl file git gnupg procps python3 sudo tzdata unzip xz-utils zip zstd"

usage() { sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
TREE=worktree CPUS=$RUNNER_CPUS REPEAT=1 PRINT=0 LOGS="" JOBS=()
while (( $# )); do
    case "$1" in
        --head) TREE=head ;;
        --cpus) CPUS="$2"; shift ;;
        --repeat) REPEAT="$2"; shift ;;
        --print) PRINT=1 ;;
        --logs) LOGS="$2"; shift ;;
        -h|--help) usage ;;
        -*) echo "Unknown option: $1" >&2; usage 2 ;;
        *) JOBS+=("$1") ;;
    esac
    shift
done
[[ "$CPUS" =~ ^[1-9][0-9]*$ && "$REPEAT" =~ ^[1-9][0-9]*$ ]] || { echo "--cpus and --repeat take a number" >&2; exit 2; }
command -v docker >/dev/null || { echo "docker isn't installed" >&2; exit 1; }
python3 -c 'import yaml' 2>/dev/null || { echo "needs python3-yaml (sudo apt install python3-yaml)" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/kes-ci-local.XXXXXX")"
[[ -n "$LOGS" ]] || LOGS="$WORK/logs"
mkdir -p "$LOGS"

# --- the jobs, from deb.yml: every job with a step that runs tests/run.sh, one folder each with its steps
python3 -I - "$WORKFLOW" "$WORK/jobs" "${JOBS[@]}" <<'EOF' || exit 2
import os, re, sys, yaml
wf, out, wanted = sys.argv[1], sys.argv[2], sys.argv[3:]
jobs = yaml.safe_load(open(wf))["jobs"]
testing = [n for n, j in jobs.items() if any("tests/run.sh" in (s.get("run") or "") for s in j.get("steps", []))]
for n in wanted:
    if n not in testing:
        sys.exit(f"Not one of deb.yml's test jobs: {n} (they are: {' '.join(testing)})")
for n in wanted or testing:
    j = jobs[n]
    if "strategy" in j:
        sys.exit(f"{n}: a matrix job; this script doesn't expand matrices")
    c = j.get("container")
    image = c if isinstance(c, str) else (c or {}).get("image")
    if image is None:
        m = re.fullmatch(r"ubuntu-(\d\d\.\d\d)", str(j["runs-on"]))
        if not m:
            sys.exit(f"{n}: runs on {j['runs-on']}, which this script doesn't know")
        image, kind = f"mirror.gcr.io/library/ubuntu:{m.group(1)}", "vm"
    else:
        kind = "container"
    d = os.path.join(out, n)
    os.makedirs(os.path.join(d, "steps"))
    open(os.path.join(d, "image"), "w").write(image + "\n")
    open(os.path.join(d, "kind"), "w").write(kind + "\n")
    i = 0
    for s in j["steps"]:
        name = s.get("name") or s.get("uses") or "step"
        if "uses" in s:
            print(f"{n}: leaving out {s['uses']}", file=sys.stderr)
            continue
        run = s["run"].replace("${{ github.job }}", n)
        if "${{" in run:
            sys.exit(f"{n}, step \"{name}\": an expression this script can't fill in")
        i += 1
        base = os.path.join(d, "steps", f"{i:02d}")
        open(base + ".sh", "w").write(run)
        open(base + ".name", "w").write(name)
        # GitHub's shells: none given → bash -e; "bash" → bash --noprofile --norc -eo pipefail
        sh = s.get("shell")
        if sh not in (None, "bash"):
            sys.exit(f"{n}, step \"{name}\": shell {sh} isn't handled")
        open(base + ".shell", "w").write("bash --noprofile --norc -eo pipefail" if sh == "bash" else "bash -e")
    if i == 0:
        sys.exit(f"{n}: no steps to run")
EOF
mapfile -t JOBS < <(ls "$WORK/jobs")

if (( PRINT )); then
    for j in "${JOBS[@]}"; do
        echo "=== $j: $(cat "$WORK/jobs/$j/image") ($(cat "$WORK/jobs/$j/kind"))"
        for s in "$WORK/jobs/$j"/steps/*.sh; do
            echo "--- $(cat "${s%.sh}.name")  [$(cat "${s%.sh}.shell")]"
            cat "$s"; echo
        done
    done
    rm -rf "$WORK"
    exit 0
fi

# --- this machine's CPUs, whole cores first (each core's threads together), to hand out $CPUS to each job
mapfile -t CORES < <(for c in /sys/devices/system/cpu/cpu[0-9]*/topology/thread_siblings_list; do cat "$c"; done | sort -un -t, -k1,1)
CPULIST=()
for c in "${CORES[@]}"; do
    IFS=, read -ra parts <<<"${c//-/,}"
    if [[ "$c" == *-* ]]; then for ((k = parts[0]; k <= parts[1]; k++)); do CPULIST+=("$k"); done
    else CPULIST+=("${parts[@]}"); fi
done
need=$(( CPUS * ${#JOBS[@]} ))
(( need <= ${#CPULIST[@]} )) || echo "Note: ${#JOBS[@]} jobs × $CPUS CPUs is more than this machine's ${#CPULIST[@]}: some share CPUs (slower than a runner)."
cpus_for() {   # cpus_for INDEX → a cpuset list
    local i=$1 k out=()
    for ((k = 0; k < CPUS; k++)); do out+=("${CPULIST[$(( (i * CPUS + k) % ${#CPULIST[@]} ))]}"); done
    local IFS=,; echo "${out[*]}"
}

# --- the tree, as CI's checkout would have it
cd "$ROOT" || exit 1
if [[ $TREE == head ]]; then
    git archive --format=tar HEAD >"$WORK/tree.tar" || exit 1
    echo "Testing HEAD ($(git rev-parse --short HEAD)), as committed."
else
    : >"$WORK/files"
    while IFS= read -r -d '' f; do
        [[ -e "$f" || -L "$f" ]] && printf '%s\0' "$f" >>"$WORK/files"
    done < <(git ls-files -z --cached --others --exclude-standard)
    tar --null -T "$WORK/files" --owner=0 --group=0 --numeric-owner -cf "$WORK/tree.tar" || exit 1
    UNCOMMITTED="$(git status --porcelain --untracked-files=all)"
fi

# --- containers and volumes (kes-ci-<job>, labelled): a job's leftovers from an earlier run go first, and this run's
# go when it ends. Only the jobs being run: another run of other jobs may be going on.
cleanup() {
    local j c v
    for j in "${JOBS[@]}"; do
        c=kes-ci-$j
        if docker container inspect "$c" >/dev/null 2>&1; then
            # (Docker from the snap can't stop a container: its init is told to end it)
            docker exec -u root "$c" kill -TERM 1 >/dev/null 2>&1 && sleep 1
            docker rm -f "$c" >/dev/null 2>&1 || echo "Couldn't remove container $c (try: sudo snap restart docker)" >&2
        fi
        for v in "$c-work" "$c-tmp"; do
            docker volume inspect "$v" >/dev/null 2>&1 && docker volume rm "$v" >/dev/null
        done
    done
}
cleanup
trap 'cleanup; rm -rf "$WORK/jobs" "$WORK/tree.tar" "$WORK/files"' EXIT
trap 'exit 130' INT TERM

for img in $(cat "$WORK"/jobs/*/image | sort -u); do
    docker pull -q "$img" >/dev/null || { echo "Couldn't pull $img" >&2; exit 1; }
done

# run_job NAME INDEX: the job in its own container; its output in $LOGS/NAME.log, its result in $LOGS/NAME.result
run_job() {
    local job=$1 idx=$2 d="$WORK/jobs/$1" C="kes-ci-$1"
    local image kind ws user envs=() vols=()
    image=$(cat "$d/image") kind=$(cat "$d/kind")
    if [[ $kind == vm ]]; then
        ws=/home/runner/work/kes-c/kes-c user=runner
        vols=(-v "$C-work:/home/runner/work" -v "$C-tmp:/tmp")
        envs=(-e HOME=/home/runner -e LANG=C.UTF-8 -e USER=runner)
    else
        ws=/__w/kes-c/kes-c user=root
        vols=(-v "$C-work:/__w")
        envs=(-e HOME=/github/home)
    fi
    envs+=(-e CI=true -e GITHUB_ACTIONS=true -e "GITHUB_JOB=$job" -e "GITHUB_WORKSPACE=$ws" -e RUNNER_OS=Linux)
    local v
    for v in "$C-work" "$C-tmp"; do docker volume create --label "$LABEL" "$v" >/dev/null; done
    echo "== $job: $image ($kind), CPUs $(cpus_for "$idx"), $RUNNER_MEM"
    docker run -d --init --label "$LABEL" --name "$C" --cpuset-cpus "$(cpus_for "$idx")" --memory "$RUNNER_MEM" \
        "${vols[@]}" "$image" sleep infinity >/dev/null || return 1
    docker cp "$WORK/tree.tar" "$C:/tree.tar" >/dev/null && docker cp "$d/steps" "$C:/ci-steps" >/dev/null || return 1
    if [[ $kind == vm ]]; then
        # the runner image: its packages, its user, the checkout in its workspace
        docker exec -u root "$C" bash -c "
            umask 022; export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq && apt-get install -y -qq $RUNNER_IMAGE_PACKAGES >/dev/null &&
            useradd -m -u 1001 -G adm,users -s /bin/bash runner &&
            echo 'runner ALL=(ALL) NOPASSWD:ALL' >/etc/sudoers.d/runner && chmod 0440 /etc/sudoers.d/runner &&
            mkdir -p '$ws' && tar -xf /tree.tar -C '$ws' && chown -R runner: /home/runner &&
            chmod 1777 /tmp && chmod -R a+rX /ci-steps" || { echo "== $job: setting up the runner failed"; return 1; }
    else
        docker exec -u root "$C" bash -c "umask 022; mkdir -p '$ws' /github/home && tar -xf /tree.tar -C '$ws'" || return 1
    fi
    local s name sh run_n rc last
    last=$(grep -l 'tests/run.sh' "$d"/steps/*.sh | tail -1)
    for s in "$d"/steps/*.sh; do
        name=$(cat "${s%.sh}.name") sh=$(cat "${s%.sh}.shell")
        # a sysctl the job sets on its VM: a container can't, so this machine must have it already
        sed -i -E 's#sudo sysctl -w ([a-z0-9_.]+)=([0-9]+)#[ "$(sudo sysctl -n \1)" = \2 ] || { echo "This machine needs \1=\2 (CI sets it on the runner): sudo sysctl -w \1=\2" >\&2; exit 1; }#' "$s"
        docker cp "$s" "$C:/ci-steps/$(basename "$s")" >/dev/null
        run_n=1
        [[ $s == "$last" ]] && run_n=$REPEAT
        for ((r = 1; r <= run_n; r++)); do
            echo "--- $job: $name$( (( run_n > 1 )) && echo " (run $r of $run_n)")"
            docker exec -u "$user" -w "$ws" "${envs[@]}" "$C" \
                bash -c "umask 022; exec $sh /ci-steps/$(basename "$s")"
            rc=$?
            (( rc == 0 )) || { echo "--- $job: \"$name\" failed (exit $rc)"; return 1; }
        done
    done
}

echo "Logs: $LOGS"
pids=()
for i in "${!JOBS[@]}"; do
    j=${JOBS[$i]}
    { run_job "$j" "$i"; echo $? >"$LOGS/$j.result"; } >"$LOGS/$j.log" 2>&1 &
    pids+=($!)
    echo "Started $j"
done
wait "${pids[@]}"

echo
failed=0
for j in "${JOBS[@]}"; do
    rc=$(cat "$LOGS/$j.result" 2>/dev/null || echo 1)
    if (( rc == 0 )); then echo "PASSED  $j"; else echo "FAILED  $j"; failed=1; fi
    grep -E '^(FAIL |CRASHED|Failed:|Build failed)|" failed \(exit' "$LOGS/$j.log" | sed 's/^/        /'
    grep '^SKIP ' "$LOGS/$j.log" | sort -u | sed 's/^/        /'
done
echo "Logs: $LOGS"
if [[ -n "${UNCOMMITTED:-}" ]]; then
    echo
    echo "Tested the working tree, which has changes not committed yet. CI only gets what's pushed: commit these"
    echo "(new files too) or CI won't test what passed here. (--head tests the last commit.)"
    sed 's/^/  /' <<<"$UNCOMMITTED"
fi
exit "$failed"
