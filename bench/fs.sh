#!/usr/bin/env bash
# Benchmark copies within one drive that can clone (Btrfs, XFS) and within one SMB share (server-side copy): Kestrel
# with Preferences → Files → Copying off and on, and GNOME Files, in a Docker container (see bench/README.md, "Copying
# on Btrfs, XFS and SMB"). The container is privileged: it makes the two file systems on loop images in its RAM disk,
# and a Samba share on the same machine mounted with the kernel's SMB client; all of it goes with the container.
# Like the other runs, the container holds nothing of this machine's but the source (bench/container.sh). Takes
# bench/run.sh's options.
#   bench/fs.sh                 build the image afresh, run the three copy tests, write bench/results-fs.md
#   bench/fs.sh --report        rebuild bench/results-fs.md from the last results
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/container.sh"
image=kes-bench-fs
fresh_image "$image" "$bench/fs"

args=("$@")
[[ " $* " == *" --apps"* ]] || args=(--apps kestrel,nautilus "${args[@]}")
[[ " $* " == *" --only"* || " $* " == *" --report"* ]] || args=(--only copy_btrfs,copy_xfs,copy_smb "${args[@]}")
name="$image-$(date +%s)"   # a new name each run (one that couldn't be removed is left aside: destroyed next time)
# (no network: the SMB share is reached on loopback, which the container keeps)
send_source results-fs | docker run -i "${RUN_OPTS[@]}" --privileged --name "$name" \
    --tmpfs /tmp:rw,exec,size=20g,mode=1777 --shm-size=2g -e LANG=C.UTF-8 -e UNPACK_AND_RUN="$UNPACK_AND_RUN" \
    "$image" bash -c '
        set -e
        cat > /tmp/source.tar   # (first, so that nothing before the benchmark reads the source off stdin)
        mkdir -p /fs/btrfs /fs/xfs /fs/smb /tmp/share
        truncate -s 4G /tmp/btrfs.img /tmp/xfs.img
        attach() {   # a loop device for the image: a node made for it when the container has none (/dev is a copy)
            local dev; dev=$(losetup -f)
            [ -e "$dev" ] || mknod "$dev" b 7 "${dev#/dev/loop}"
            losetup "$dev" "$1" && echo "$dev"
        }
        cleanup() {   # (the loop devices are the host'"'"'s: detached here, not left behind)
            umount /fs/smb /fs/btrfs /fs/xfs 2>/dev/null || true
            for img in /tmp/btrfs.img /tmp/xfs.img; do
                for d in $(losetup -j "$img" | cut -d: -f1); do losetup -d "$d"; done
            done
        }
        trap cleanup EXIT
        mkfs.btrfs -q /tmp/btrfs.img && dev=$(attach /tmp/btrfs.img) && mount "$dev" /fs/btrfs
        mkfs.xfs -q /tmp/xfs.img && dev=$(attach /tmp/xfs.img) && mount "$dev" /fs/xfs
        chown tester:tester /fs/btrfs /fs/xfs /tmp/share
        printf "[global]\n  server min protocol = SMB3\n  smb ports = 445\n[bench]\n  path = /tmp/share\n  writable = yes\n  valid users = tester\n" \
            > /etc/samba/smb.conf
        printf "bench\nbench\n" | smbpasswd -s -a tester >/dev/null
        smbd -D
        for i in $(seq 50); do mount -t cifs //127.0.0.1/bench /fs/smb \
            -o username=tester,password=bench,uid=tester,gid=tester,vers=3.1.1 2>/dev/null && break; sleep 0.2; done
        mountpoint -q /fs/smb
        cd /home/tester
        runuser -u tester -- env KESTREL_BENCH_BTRFS=/fs/btrfs KESTREL_BENCH_XFS=/fs/xfs KESTREL_BENCH_SMB=/fs/smb \
            HOME=/home/tester bash -c "$UNPACK_AND_RUN" bench "$@" < /tmp/source.tar
        ' bench "${args[@]}" || status=$?
fetch_results "$name" results-fs
exit "${status:-0}"
