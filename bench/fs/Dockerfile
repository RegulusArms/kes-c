# Ubuntu 26.04 for bench/fs.sh: Kestrel's build dependencies, GNOME Files, what bench.py needs, and the file systems
# the copy tests use (Btrfs and XFS tools; Samba and the kernel's SMB client's mount helper).
# Pulled through Google's mirror of Docker Hub, as in CI (Docker Hub rate-limits).
FROM mirror.gcr.io/library/ubuntu:26.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
      g++ cmake pkg-config qt6-base-dev qt6-tools-dev qt6-l10n-tools libglib2.0-dev libxcb1-dev libsecret-1-dev \
      nautilus xvfb xdotool x11-apps dbus dbus-x11 libglib2.0-bin gvfs python3 python3-gi gir1.2-glib-2.0 python3-pyqt6 \
      ffmpeg rsync btrfs-progs xfsprogs samba cifs-utils \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -m tester
WORKDIR /home/tester
