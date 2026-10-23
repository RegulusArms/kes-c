// Built-in image viewer: images decoded at screen size ahead of time (full size when zoomed in), zoom and pan (wheel,
// pinch, double-click), slideshow and animations, a filmstrip, an info panel (EXIF, GPS, generation prompts,
// histogram), comparing two images (side by side or A/B, zoom linked), rotations saved without losing quality, crop
// and resize, and quick file actions (star, copy or move to a recent folder, trash and undo).
#pragma once

#include <QHash>
#include <QMovie>
#include <QPixmap>
#include <QPointF>
#include <QPointer>
#include <QScrollArea>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>

#include "imgedit.h"

#include <functional>
#include <optional>

class QLabel;
class QMenu;
class QVBoxLayout;

// One image on screen with its own zoom, pan, rotation and flip. The pixmap may be a screen-size decode of a bigger
// image: `full` is the image's real size, which zoom is measured against (100% = one image pixel per screen pixel).
class ViewerCanvas : public QWidget {
    Q_OBJECT
public:
    explicit ViewerCanvas(QWidget *parent = nullptr);
    void set_image(const QPixmap &pm, QSize full);   // keeps the view
    void clear_image(const QString &msg);
    void reset_view();                               // fit, centred, not turned
    QSizeF img_size() const;                         // the real size, turned as shown
    double fit_scale() const;
    double scale() const;
    void set_zoom(double z, std::optional<QPointF> anchor = std::nullopt);
    void fit();
    void toggle_zoom_at(QPointF pos);   // double-click: fit ↔ actual size, at the pointer
    void rotate(int deg);
    void flip();
    void show_osd(const QString &text);
    QString osd_text() const;   // what the on-screen message says while it's shown
    // cropping: a selection dragged on the image, in the turned image's real pixels
    void start_crop();
    void stop_crop();
    QRect crop_rect() const;
    QPointF to_image(QPointF widget_pos) const;   // a point on the canvas → the turned image's pixels
    void clamp();

    QPixmap pix;
    QSize full;
    std::optional<double> zoom;   // nullopt = fit to window
    QPointF offset;
    int rotation = 0;
    bool flip_h = false;
    bool upscale = false;
    bool cropping = false;
    QRectF crop;
    QString message;   // instead of an image ("Loading…", an error)
    QString overlay;   // the info line, top left
    QString tag;       // "A" / "B" when comparing

Q_SIGNALS:
    void view_changed();   // zoomed or panned
    void pointer_moved();

protected:
    void paintEvent(QPaintEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;
    void wheelEvent(QWheelEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void mouseDoubleClickEvent(QMouseEvent *ev) override;
    bool event(QEvent *ev) override;

private:
    QPointF center() const;
    bool dragging = false;
    QPointF drag_start, drag_offset, crop_anchor;
    QString osd;
    QTimer osd_timer;
};

// A row of thumbnails along the bottom, centred on the current image; click one to show it.
class Filmstrip : public QWidget {
    Q_OBJECT
public:
    explicit Filmstrip(QWidget *parent = nullptr);
    void set_items(const QStringList &paths, int current);
    int index_at(QPoint pos) const;
    QRect cell_rect(int i) const;
    QStringList paths;
    int current = 0;

Q_SIGNALS:
    void activated(int index);

protected:
    void paintEvent(QPaintEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void wheelEvent(QWheelEvent *ev) override;

private:
    QPixmap thumb(const QString &path);
    QHash<QString, QPair<qint64, qint64>> stats;   // (mtime, size), -1: none
    QSet<QString> asked;
    int wheel_acc = 0;
};

// The red, green and blue histogram of what's shown
class Histogram : public QWidget {
public:
    explicit Histogram(QWidget *parent = nullptr);
    void set_image(const QPixmap &pm);
    QSize sizeHint() const override { return {300, 110}; }
    QVector<int> bins[3];

protected:
    void paintEvent(QPaintEvent *ev) override;
};

// The side panel: the file, its details and GPS position (with a map link), image-generation prompts, all of its
// metadata, and a histogram.
class ViewerInfo : public QScrollArea {
    Q_OBJECT
public:
    explicit ViewerInfo(QWidget *parent = nullptr);
    void show_path(const QString &path);
    void set_histogram(const QPixmap &pm) { histogram->set_image(pm); }
    QString text() const;   // everything shown, as plain text (for tests)
    QString path;
    QString map_url;   // the GPS position's map link, or empty
    bool loaded = false;
    Histogram *histogram;

Q_SIGNALS:
    void details_loaded();

private:
    void row(const QString &k, const QString &v, bool rich = false);
    void clear_rows();
    QLabel *title;
    QWidget *rows_box;
    QVBoxLayout *rows;
    int gen = 0;
};

class ImageViewer : public QWidget {
    Q_OBJECT
public:
    struct Callbacks {
        std::function<bool(const QString &)> on_delete;   // to the trash; true if it went
        std::function<void(const QString &)> on_properties;
        std::function<void(const QString &)> on_close;
        std::function<void(const QString &)> on_open_with;
        std::function<void(const QString &)> on_reveal;   // show it in its folder
        std::function<void(std::function<void()>)> on_undo;   // Ctrl+Z: undo the newest action, then call back
        std::function<void(const QString &, const QString &, const QString &)> on_transfer;   // (file, folder, op)
    };
    ImageViewer(const QStringList &paths, int index, Callbacks callbacks);
    ~ImageViewer() override;

    QString current() const;
    void navigate(int step, bool wrap = false);
    void go_to(int n);
    void delete_current();
    void toggle_fullscreen();
    void toggle_slideshow();
    void fit();
    void toggle_filmstrip();
    void toggle_info();
    void toggle_overlay();
    void toggle_keep_zoom();
    void toggle_compare();
    void toggle_ab();     // compare side by side ↔ one at a time (A/B)
    void flip_ab();       // A/B: show the other one
    void transfer(const QString &folder, const QString &op);   // copy / move the current image there
    void transfer_recent(int n, const QString &op);           // to the n-th recent folder (1 = latest)
    void choose_transfer(const QString &op);
    void undo();
    void toggle_star();
    void toggle_pin();
    void save_changes();    // Ctrl+S: the current image's rotation, or its edits (asked: new file or replace)
    void discard_edits();   // Esc: the crops and resizes not saved go
    void start_crop();
    void finish_crop();
    void resize_image();
    void straighten();
    // what's left to do with an edited image: a new file beside it, or the original replaced
    void add_edit(const QRect &crop, const QSize &size);   // shown at once; the file is only written when saved
    QList<imgedit::Edit> steps;   // the current image's crops and resizes not saved yet (each after its rotation)

    QMenu *build_menu();   // the right-click menu, laid out as Preferences → Menus says (the caller deletes it)
    static QStringList recent_folders();
    QStringList paths;
    int index = 0;
    ViewerCanvas *canvas;   // the current image (B when comparing)
    ViewerCanvas *pinned;   // A when comparing
    QString pinned_path;
    int compare = 0;        // 0 off, 1 side by side, 2 A/B
    bool showing_a = false;
    Filmstrip *filmstrip;
    ViewerInfo *info;
    bool keep_zoom = false;
    QTimer slideshow;
    std::function<void(QWidget *)> on_question;   // tests: each question the viewer asks, as it's shown

Q_SIGNALS:
    void loaded(const QString &path, const QImage &img, QSize full, bool is_full);
    void saved(const QString &path);   // an edit or rotation written

protected:
    void wheelEvent(QWheelEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void contextMenuEvent(QContextMenuEvent *ev) override;
    void closeEvent(QCloseEvent *ev) override;

public:
    struct Entry {
        QPixmap pix;   // null: couldn't be read
        QSize full;
        bool is_full = false;
    };
    QHash<QString, Entry> cache;
    QSet<QString> pending;   // path, or path + FULL

private:
    void request(const QString &path, bool full = false);
    void on_loaded(const QString &path, const QImage &img, QSize full, bool is_full);
    void show_current();
    void show_on(ViewerCanvas *c, const QString &path);
    void want_full(ViewerCanvas *c, const QString &path);
    void link_view(ViewerCanvas *from, ViewerCanvas *to);
    void update_overlay();
    void update_layout();
    void leave_image();   // its rotation saved, or asked about (Preferences → viewer_save_rotation)
    void ask_rotation(const QString &path, int rotation, bool flip, bool closing_viewer);
    // how: 0 Ctrl+S (Cancel keeps the edits), 1 leaving the image (Discard), 2 closing the viewer (Discard, Cancel)
    void ask_edit(const QString &src, const QList<imgedit::Edit> &edits, int how);
    QList<imgedit::Edit> all_steps() const;   // steps, and a rotation made after the last one
    bool write_rotation(const QString &path, int rotation, bool flip);
    void after_write(const QString &path);
    void drop_current();   // it went (trash, move): off the list
    void transfer_file(const QString &path, const QString &folder, const QString &op);
    static void remember_folder(const QString &folder);
    ViewerCanvas *active() const;   // what zoom keys work on: B, or A while A/B shows it
    void stop_movie();
    void movie_frame();
    void add_shortcuts();
    void escape();
    void copy_image();
    int screen_side() const;
    void ask(QWidget *box);

    Callbacks cb;
    QStringList cache_order;
    QThreadPool pool;
    QMovie *movie = nullptr;
    QString shown;   // the path the canvas shows (its rotation belongs to it)
    qint64 shown_bytes = -1;
    QList<QPair<QString, int>> removed;   // (path, index) trashed or moved from here: Ctrl+Z puts them back
    bool show_overlay = true;
    bool closing = false;   // the rotation question is answered: close for real
    QTimer hide_cursor;
    int wheel_acc = 0;
    bool linking = false;
};
