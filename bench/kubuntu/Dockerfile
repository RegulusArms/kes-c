# Kubuntu 26.04 for bench/kubuntu.sh: Ubuntu 26.04 with the KDE parts of Kubuntu's desktop that Dolphin uses (KIO and
# its thumbnailers, the Plasma Qt integration and Breeze), Kestrel's build dependencies, and what bench.py needs.
# Pulled through Google's mirror of Docker Hub, as in CI (Docker Hub rate-limits).
FROM mirror.gcr.io/library/ubuntu:26.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
      g++ cmake pkg-config qt6-base-dev qt6-tools-dev qt6-l10n-tools libglib2.0-dev libxcb1-dev libsecret-1-dev \
      dolphin kio6 kio-extras kdegraphics-thumbnailers ffmpegthumbs kimageformat6-plugins qt6-image-formats-plugins \
      plasma-integration breeze breeze-icon-theme frameworkintegration6 \
      xvfb xdotool x11-apps dbus dbus-x11 libglib2.0-bin gvfs python3 python3-gi gir1.2-glib-2.0 python3-pyqt6 \
      ffmpeg rsync \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -m tester
USER tester
WORKDIR /home/tester
