// MainWindow's opening of files and folders: activating items (folders navigate, archives offer Extract, checksum
// files check the files they list, images open in the viewer, videos and everything else in their apps, as Preferences
// says), Quick View, and the image viewer.
// The window itself (tabs, menus, context menus) is in app.cpp.
#include "app.h"

#include "archive.h"
#include "archive_ui.h"
#include "chooser.h"
#include "dialogs.h"
#include "fileops.h"
#include "hashcheck.h"
#include "places.h"
#include "proc.h"
#include "util.h"
#include "viewer.h"
#include "widgets.h"

#include <QMessageBox>
#include <QPointer>

using namespace util;

void MainWindow::open_paths(Pane *p, const QStringList &paths_in, bool new_tab_)
{
    QStringList paths;
    for (const QString &x : paths_in)
        if (!x.isEmpty())
            paths << x;
    if (paths.isEmpty())
        return;
    QStringList dirs, files;
    for (const QString &x : paths)
        (isdir(x) ? dirs : files) << x;
    if (chooser) {   // a file chooser: folders open as usual, files are the choice
        if (!dirs.isEmpty())
            p->set_path(dirs.first());
        else if (!files.isEmpty())
            chooser->activated(files);
        return;
    }
    places::add_recent(files);
    if (!dirs.isEmpty()) {
        if (dirs.size() == 1 && !new_tab_ && files.isEmpty()) {
            p->set_path(dirs.first());
        } else {
            for (const QString &d : dirs)
                new_tab(d, false);
        }
    }
    QStringList images, videos, fetch, others;
    for (const QString &f : files) {
        if (archive::opens_as_archive(f))
            archive_ui::extract_dialog(this, f);   // Kestrel's own extraction, not the system's archive app
        else if (hashcheck::is_hash_file(f) && hashcheck::open_dialog(this, f))
            ;   // Verify Checksums (one that lists nothing opens as text)
        else if (is_image(f))
            images << f;
        else if (is_video(f))
            (needs_local_copy(f) ? fetch : videos) << f;   // a player would download it again on every open/seek
        else
            others << f;
    }
    if (!fetch.isEmpty()) {
        QPointer<MainWindow> self(this);
        fileops::fetch_local(this, fetch, [self](const QStringList &local) {
            if (self)
                self->open_videos(local);
        });
    }
    QString img_choice = settings().value("image_opener", "system").toString();
    if (!images.isEmpty() && img_choice == "builtin") {
        if (images.size() == 1) {
            QStringList all_imgs;
            for (const QString &x : p->all_paths())
                if (is_image(x))
                    all_imgs << x;
            int idx = std::max<int>(0, all_imgs.indexOf(images.first()));
            open_viewer(p, all_imgs.isEmpty() ? images : all_imgs, all_imgs.contains(images.first()) ? idx : 0);
        } else {
            open_viewer(p, images, 0);
        }
    } else if (!images.isEmpty()) {
        others += open_with_choice(images, img_choice);
    }
    if (!videos.isEmpty())
        open_videos(videos);
    for (const QString &f : others)
        if (!open_file(f))
            QMessageBox::warning(this, "Open", "Could not open " + f);
}

void MainWindow::open_videos(const QStringList &videos)
{
    for (const QString &f : open_with_choice(videos, settings().value("video_opener", "system").toString()))
        if (!open_file(f))
            QMessageBox::warning(this, "Open", "Could not open " + f);
}

QStringList MainWindow::open_with_choice(const QStringList &files, const QString &choice)
{
    // launch `files` with the app chosen in Preferences; returns files left for the system default
    AppRef app = choice != "system" ? app_by_id(choice) : AppRef();
    if (!app)
        return files;
    try {
        launch_app(app, files);
        return {};
    } catch (const Error &e) {
        QMessageBox::warning(this, "Open", QString("Could not open with %1: %2").arg(app_name(app), e.message()));
        return files;
    }
}

bool MainWindow::open_file(const QString &path)
{
    if (path.endsWith(".desktop") && which("gio")) {
        QString text = QString::fromUtf8(read_file(path));
        if (text.contains("Type=Link")) {
            for (const QString &line : text.split('\n')) {
                if (line.startsWith("URL=")) {
                    QString url = line.mid(4).trimmed();
                    QString target = uri_to_path(url);
                    if (target.isEmpty())
                        target = url;
                    if (isdir(target)) {
                        navigate(target);
                        return true;
                    }
                    return open_default(target);
                }
            }
        }
        proc::start_detached({"gio", "launch", path});
        return true;
    }
    return open_default(path);
}

void MainWindow::quick_view(Pane *p, const QStringList &paths)
{
    QStringList imgs;
    for (const QString &x : paths)
        if (is_image(x))
            imgs << x;
    if (!imgs.isEmpty()) {
        if (paths.size() == 1) {
            QStringList all_imgs;
            for (const QString &x : p->all_paths())
                if (is_image(x))
                    all_imgs << x;
            int i = all_imgs.indexOf(imgs.first());
            open_viewer(p, all_imgs.isEmpty() ? imgs : all_imgs, std::max(i, 0));
        } else {
            open_viewer(p, imgs, 0);
        }
    } else if (!paths.isEmpty()) {
        properties(paths);
    }
}

void MainWindow::open_viewer(Pane *p, const QStringList &images, int idx)
{
    QPointer<Pane> pp(p);
    auto viewer_ptr = std::make_shared<QPointer<ImageViewer>>();
    ImageViewer::Callbacks cb;
    cb.on_delete = [this](const QString &path) {
        try {
            util::trash(path);
            sidebar->refresh();
            return true;
        } catch (const OSError &e) {
            QMessageBox::warning(this, "Move to Trash", e.message());
            return false;
        }
    };
    cb.on_close = [this, pp](const QString &path) {
        if (pp && panes().contains(pp.data()) && dirname(path) == pp->path)
            pp->select_paths({path});
        QList<QPointer<ImageViewer>> keep;
        for (const auto &v : viewers)
            if (v && v->isVisible())
                keep << v;
        viewers = keep;
    };
    cb.on_properties = [this, viewer_ptr](const QString &path) { properties({path}, viewer_ptr->data()); };
    cb.on_open_with = [viewer_ptr](const QString &path) { OpenWithDialog(viewer_ptr->data(), {path}).exec(); };
    auto *v = new ImageViewer(images, idx, cb);
    *viewer_ptr = v;
    viewers << v;
    if (settings().value("viewer_fullscreen", false).toBool())
        v->showFullScreen();
    else
        v->show();
    v->activateWindow();
}
