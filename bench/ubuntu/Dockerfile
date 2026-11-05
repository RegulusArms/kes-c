# Ubuntu 26.04 for bench/run.sh: what a standard Ubuntu desktop (ubuntu-desktop) gives the apps measured (GNOME Files,
# its thumbnailers: Papers for PDFs, GStreamer's for videos, glycin's for images; the Yaru theme and icons), the other
# file managers compared with (Dolphin, Thunar, Krusader, PCManFM, each with what it recommends, as `apt install`
# gives: KIO and its thumbnailers, tumbler…), the packages Kestrel recommends that change what it shows (Qt's image
# formats and GTK integration), the extra media formats Ubuntu's installer offers (ubuntu-restricted-addons: GStreamer's
# plugins and libav, without which GNOME's video thumbnailer can't read MP4/H.264), Kestrel's build dependencies, and
# what bench.py needs.
# Pulled through Google's mirror of Docker Hub, as in CI (Docker Hub rate-limits).
FROM mirror.gcr.io/library/ubuntu:26.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
      g++ cmake pkg-config qt6-base-dev qt6-tools-dev qt6-l10n-tools libglib2.0-dev libxcb1-dev libsecret-1-dev \
      nautilus papers gst-video-thumbnailer glycin-thumbnailers yaru-theme-gtk yaru-theme-icon \
      dolphin thunar krusader pcmanfm \
      qt6-image-formats-plugins kimageformat6-plugins qt6-svg-plugins qt6-gtk-platformtheme adwaita-icon-theme \
      xvfb xdotool x11-apps dbus dbus-x11 libglib2.0-bin gvfs python3 python3-gi gir1.2-glib-2.0 python3-pyqt6 \
      gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav \
      ffmpeg rsync \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -m tester
USER tester
WORKDIR /home/tester
