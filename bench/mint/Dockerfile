# Linux Mint for bench/mint.sh: Linux Mint's own image (Ubuntu's base with Mint's repository: its Nemo, Cinnamon and
# XApp packages) with the parts of Mint's Cinnamon desktop that Nemo and Kestrel use (Nemo, the thumbnailers Mint
# installs, Qt's GTK integration, the Mint-Y theme and icons), Kestrel's build dependencies, and what bench.py needs.
# RELEASE is Mint's version (22.3: Ubuntu 24.04 and Qt 6.4; 23: Ubuntu 26.04). Pulled through Google's mirror of
# Docker Hub, as in CI (Docker Hub rate-limits).
# Mint's look comes from its gsettings defaults (mint-artwork), which Cinnamon's settings daemon hands to every app; on
# the bare X server there's no such daemon, so GTK (Nemo, and Qt's GTK integration in Kestrel) reads them from
# /etc/gtk-3.0/settings.ini instead, which still names Ubuntu's Yaru: it gets Mint's defaults.
ARG RELEASE=22.3
FROM mirror.gcr.io/linuxmintd/mint${RELEASE}-amd64
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
      g++ cmake pkg-config qt6-base-dev qt6-tools-dev qt6-l10n-tools libglib2.0-dev libxcb1-dev libsecret-1-dev \
      nemo nemo-fileroller ffmpegthumbnailer heif-thumbnailer xreader qt6-gtk-platformtheme qt6-image-formats-plugins \
      mint-artwork mint-themes mint-y-icons \
      xvfb xdotool x11-apps dbus dbus-x11 libglib2.0-bin gvfs python3 python3-gi gir1.2-glib-2.0 python3-pyqt6 \
      ffmpeg rsync \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -m tester \
    && printf '[Settings]\ngtk-theme-name = Mint-Y-Aqua\ngtk-icon-theme-name = Mint-Y-Sand\n' > /etc/gtk-3.0/settings.ini
USER tester
WORKDIR /home/tester
