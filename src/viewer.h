// Built-in image viewer with prefetching, zoom/pan, slideshow and animation support.
#pragma once

#include <QHash>
#include <QMovie>
#include <QPixmap>
#include <QPointF>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <optional>

class ImageViewer : public QWidget {
    Q_OBJECT
public:
    struct Callbacks {
        std::function<bool(const QString &)> on_delete;
        std::function<void(const QString &)> on_properties;
        std::function<void(const QString &)> on_close;
        std::function<void(const QString &)> on_open_with;
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

Q_SIGNALS:
    void loaded(const QString &path, const QImage &img);

protected:
    void paintEvent(QPaintEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;
    void wheelEvent(QWheelEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void mouseDoubleClickEvent(QMouseEvent *ev) override;
    void contextMenuEvent(QContextMenuEvent *ev) override;
    void closeEvent(QCloseEvent *ev) override;

private:
    void request(const QString &path);
    void on_loaded(const QString &path, const QImage &img);
    void show_current();
    void display(const QString &path);
    void movie_frame();
    void stop_movie();
    QSizeF img_size() const;
    double fit_scale() const;
    double scale() const;
    void set_zoom(double z, std::optional<QPointF> anchor = std::nullopt);
    void clamp();
    void add_shortcuts();
    void escape();
    void toggle_upscale();
    void rotate(int deg);
    void flip();
    void toggle_info();
    void copy_image();

    QStringList paths;
    int index;
    Callbacks cb;
    QHash<QString, QPixmap> cache;   // null pixmap = failed to load
    QStringList cache_order;
    QSet<QString> pending;
    QThreadPool pool;
    QPixmap pix;
    QMovie *movie = nullptr;
    std::optional<double> zoom;   // nullopt = fit to window
    QPointF offset;
    int rotation = 0;
    bool flip_h = false;
    bool show_info = true;
    bool upscale = false;
    bool dragging = false;
    QPointF drag_start, drag_offset;
    QString error;
    QTimer slideshow;
    QTimer hide_cursor;
    int wheel_acc = 0;
};
