// Phones and cameras: recognising their paths, where the Overview lists them, the hints, and thumbnails on them.
#include "common.h"

#include <QImage>

using namespace test;

int main(int argc, char **argv)
{
    int rc;
    if (is_tower(argc, argv, &rc))
        return rc;
    QApplication app(argc, argv);
    setup_app();

    // a uid that has no /run/user folder, so nothing here reaches a real gvfs mount
    const QString gvfs = "/run/user/99999/gvfs/";

    // -- paths on a phone
    check(is_device_path(gvfs + "afc:host=00008140-000201913640801C,port=3/Documents/a.pdf"),
          "a file on an iPhone (afc) is on a device");
    check(is_device_path(gvfs + "gphoto2:host=Apple_Inc._iPhone_00008140000201913640801C/DCIM/100APPLE/IMG_0001.HEIC"),
          "a photo on a camera or iPhone (gphoto2) is on a device");
    check(is_device_path(gvfs + "mtp:host=Google_Pixel_8_1A2B3C/Internal shared storage/DCIM"),
          "a file on an Android phone (mtp) is on a device");
    check(!is_device_path(gvfs + "smb-share:server=nas,share=media/film.mkv"), "a network share (smb) is not a device");
    check(!is_device_path(HOME()) && !is_device_path("/run/user/99999/gvfs-not/afc:host=x/a"),
          "a local folder is not a device");
    check(needs_local_copy(gvfs + "gphoto2:host=Apple_Inc._iPhone_00008140000201913640801C/202608_a/IMG_2183.MOV"),
          "a video from an iPhone's photos (gphoto2) is copied before it plays");
    check(!needs_local_copy(gvfs + "afc:host=00008140-000201913640801C,port=3/VLC/clip.mov") &&
              !needs_local_copy(gvfs + "mtp:host=Google_Pixel_8_1A2B3C/DCIM/Camera/clip.mp4") && !needs_local_copy(HOME()),
          "videos from an iPhone's app files (afc), Android (mtp) or a local folder play in place");

    // -- where the Overview lists mounts
    using overview::MountGroup;
    check(overview::is_phone_scheme("afc") && overview::is_phone_scheme("gphoto2") && overview::is_phone_scheme("mtp") &&
              !overview::is_phone_scheme("smb") && !overview::is_phone_scheme("file"),
          "afc, gphoto2 and mtp are phones; smb and file aren't");
    check(overview::phone_kind("gphoto2") == "Photos and videos" && overview::phone_kind("afc") == "Files",
          "an iPhone's photos are labelled Photos and videos, its apps' files Files");
    check(overview::mount_group("gphoto2", gvfs + "gphoto2:host=X", true, false) == MountGroup::Skip,
          "a shadowed mount is skipped (no duplicate cards)");
    check(overview::mount_group("afc", gvfs + "afc:host=X,port=3", false, false) == MountGroup::Phone &&
              overview::mount_group("mtp", gvfs + "mtp:host=X", false, false) == MountGroup::Phone,
          "a phone goes under Phones & Cameras, not Network");
    check(overview::mount_group("smb", gvfs + "smb-share:server=nas,share=media", false, false) == MountGroup::Network,
          "an smb share goes under Network");
    check(overview::mount_group("file", "/media/usb", false, true) == MountGroup::Local,
          "a scanned local drive stays under Drives");

    // -- hints
    check(overview::phone_hint("afc", "Documents on Sam’s iPhone").contains("Trust") &&
              overview::phone_hint("gphoto2", "iPhone").contains("Trust"),
          "an iPhone that won't mount asks to tap Trust");
    check(overview::phone_hint("mtp", "Pixel 8").contains("File transfer"), "an Android phone asks for File transfer");
    check(overview::phone_hint("gphoto2", "Canon EOS R6").contains("PTP"), "a camera asks for PTP mode");
    check(overview::NO_PHONES_HINT.contains("Trust") && overview::NO_PHONES_HINT.contains("File transfer"),
          "the Overview explains how to connect a phone when none is connected");

    // -- thumbnails on a phone
    ThumbnailManager *t = g_thumbs;
    QString dir = gvfs + "gphoto2:host=Test/DCIM/100APPLE";
    qsizetype before = t->failed.size();
    check(t->folder_pixmap(dir, 1, 128).isNull() && t->failed.size() == before + 1,
          "no folder previews on a phone (they would download every file)");
    check(device_uri(dir + "/IMG_0001.JPG").isEmpty() && thumbs::device_preview(QString(), 128).isNull(),
          "a file outside any mount has no device preview");
    before = t->failed.size();
    QString photo = dir + "/IMG_0001.JPG";
    check(t->get(photo, 1, false, 128, 2000000).isNull() && t->failed.size() == before,
          "a photo on a phone is queued for a thumbnail");
    check(wait_for([&]() { return t->failed.size() == before + 1; }, 10000),
          "a photo that can't be read from the phone fails without hanging");
    QString local = home_path("local.jpg");
    QImage img(64, 48, QImage::Format_RGB32);
    img.fill(Qt::darkCyan);
    img.save(local, "JPG");
    qint64 mtime = QFileInfo(local).lastModified().toSecsSinceEpoch() - 60;   // not "still being written"
    t->get(local, mtime, false, 128, QFileInfo(local).size());
    check(wait_for([&]() { return !t->get(local, mtime, false, 128, QFileInfo(local).size()).isNull(); }, 10000),
          "local photos still get thumbnails");

    finish();
}
