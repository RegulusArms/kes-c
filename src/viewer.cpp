#include "viewer.h"

#include "util.h"
#include "uwp.h"

#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QImageReader>
#include <QKeySequence>
#include <QMenu>
#include <QPainter>
#include <QWheelEvent>

using namespace util;

static const QSet<QString> ANIMATABLE = {".gif", ".webp", ".avif", ".jxl", ".mng", ".apng"};

ImageViewer::ImageViewer(const QStringList &paths_, int idx, Callbacks callbacks)
    : QWidget(nullptr, Qt::Window), paths(paths_), cb(std::move(callbacks))
{
    setAttribute(Qt::WA_DeleteOnClose);
    index = std::max(0, std::min<int>(idx, paths.size() - 1));
    pool.setMaxThreadCount(3);
    connect(this, &ImageViewer::loaded, this, &ImageViewer::on_loaded, Qt::QueuedConnection);
    connect(&slideshow, &QTimer::timeout, this, [this]() { navigate(1, true); });
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
    resize(1200, 800);
    setStyleSheet("background:#111");
    hide_cursor.setSingleShot(true);
    hide_cursor.setInterval(2000);
    connect(&hide_cursor, &QTimer::timeout, this, [this]() {
        if (isFullScreen())
            setCursor(Qt::BlankCursor);
    });
    add_shortcuts();
    show_current();
}

ImageViewer::~ImageViewer()
{
    pool.clear();
    pool.waitForDone();
}

// ------------------------------------------------------------ loading

void ImageViewer::request(const QString &path)
{
    if (cache.contains(path) || pending.contains(path))
        return;
    pending << path;
    pool.start([this, path]() {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (is_raw(path))
            reader.setQuality(0);
        Q_EMIT loaded(path, reader.read());
    });
}

void ImageViewer::on_loaded(const QString &path, const QImage &img)
{
    pending.remove(path);
    cache.insert(path, img.isNull() ? QPixmap() : QPixmap::fromImage(img));
    cache_order.removeAll(path);
    cache_order << path;
    while (cache_order.size() > 7)
        cache.remove(cache_order.takeFirst());
    if (!paths.isEmpty() && path == paths[index])
        display(path);
}

QString ImageViewer::current() const { return paths.isEmpty() ? QString() : paths[index]; }

void ImageViewer::show_current()
{
    if (paths.isEmpty()) {
        close();
        return;
    }
    QString path = current();
    zoom.reset();
    offset = QPointF(0, 0);
    rotation = 0;
    flip_h = false;
    stop_movie();
    setWindowTitle(QString("%1 — %2/%3").arg(basename(path), QString::number(index + 1), QString::number(paths.size())));
    bool animated = false;
    if (ANIMATABLE.contains(ext_of(path))) {
        QImageReader reader(path);
        animated = reader.supportsAnimation() && reader.imageCount() != 1;
    }
    if (animated) {
        movie = new QMovie(path, QByteArray(), this);
        connect(movie, &QMovie::frameChanged, this, &ImageViewer::movie_frame);
        movie->start();
        error.clear();
    } else if (cache.contains(path)) {
        display(path);
    } else {
        pix = QPixmap();
        error.clear();
        request(path);
    }
    for (int d : {1, -1, 2}) {
        int j = index + d;
        if (j >= 0 && j < paths.size())
            request(paths[j]);
    }
    update();
}

void ImageViewer::display(const QString &path)
{
    pix = cache.value(path);
    error = pix.isNull() ? "Cannot display this image" : "";
    update();
}

void ImageViewer::movie_frame()
{
    if (movie) {
        pix = movie->currentPixmap();
        update();
    }
}

void ImageViewer::stop_movie()
{
    if (movie) {
        movie->stop();
        movie->deleteLater();
        movie = nullptr;
    }
}

// ------------------------------------------------------------ navigation

void ImageViewer::navigate(int step, bool wrap)
{
    if (paths.isEmpty())
        return;
    int n = index + step;
    if (wrap)
        n = ((n % int(paths.size())) + int(paths.size())) % int(paths.size());
    n = std::max(0, std::min<int>(n, paths.size() - 1));
    if (n != index) {
        index = n;
        show_current();
    }
}

void ImageViewer::go_to(int n)
{
    index = std::max(0, std::min<int>(n, paths.size() - 1));
    show_current();
}

void ImageViewer::delete_current()
{
    QString path = current();
    if (!path.isEmpty() && cb.on_delete && cb.on_delete(path)) {
        paths.removeAt(index);
        cache.remove(path);
        if (index >= paths.size())
            index = paths.size() - 1;
        if (index < 0)
            index = 0;
        show_current();
    }
}

// ------------------------------------------------------------ geometry

QSizeF ImageViewer::img_size() const
{
    if (pix.isNull())
        return {1, 1};
    double w = pix.width() / pix.devicePixelRatio(), h = pix.height() / pix.devicePixelRatio();
    return rotation % 180 ? QSizeF(h, w) : QSizeF(w, h);
}

double ImageViewer::fit_scale() const
{
    QSizeF s = img_size();
    double f = std::min(width() / s.width(), height() / s.height());
    return (upscale || f < 1) ? f : 1.0;
}

double ImageViewer::scale() const { return zoom ? *zoom : fit_scale(); }

void ImageViewer::set_zoom(double z, std::optional<QPointF> anchor)
{
    double old = scale();
    z = std::max(0.02, std::min(z, 40.0));
    QPointF mid(width() / 2.0, height() / 2.0);
    QPointF a = anchor ? *anchor : mid;
    QPointF center = mid + offset;
    QPointF img_pt = (a - center) / old;
    zoom = z;
    offset = a - mid - img_pt * z;
    clamp();
    update();
}

void ImageViewer::clamp()
{
    QSizeF s = img_size();
    double sc = scale();
    double mx = std::max(0.0, (s.width() * sc - width()) / 2);
    double my = std::max(0.0, (s.height() * sc - height()) / 2);
    offset = QPointF(std::max(-mx, std::min(mx, offset.x())), std::max(-my, std::min(my, offset.y())));
}

// ------------------------------------------------------------ painting

void ImageViewer::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor("#111"));
    p.setRenderHint(QPainter::SmoothPixmapTransform, scale() < 2.5);
    if (!pix.isNull()) {
        double s = scale();
        double pw = pix.width() / pix.devicePixelRatio(), ph = pix.height() / pix.devicePixelRatio();
        QPointF c = QPointF(width() / 2.0, height() / 2.0) + offset;
        QTransform t;
        t.translate(c.x(), c.y());
        t.rotate(rotation);
        t.scale(flip_h ? -s : s, s);
        p.setTransform(t);
        p.drawPixmap(QRectF(-pw / 2, -ph / 2, pw, ph), pix, QRectF(pix.rect()));
        p.resetTransform();
    } else if (!paths.isEmpty()) {
        p.setPen(QColor("#aaa"));
        p.drawText(rect(), Qt::AlignCenter, error.isEmpty() ? "Loading…" : error);
    }
    if (show_info && !paths.isEmpty()) {
        QString path = current();
        QString dims = pix.isNull() ? QString() : QString("%1 × %2").arg(pix.width()).arg(pix.height());
        struct stat st;
        QString size = stat_(path, st) ? human_size(qint64(st.st_size)) : QString();
        QString extra = slideshow.isActive() ? "   ▶ slideshow" : "";
        QString text = QString("%1    %2 / %3    %4    %5    %6%%7")
                           .arg(basename(path), QString::number(index + 1), QString::number(paths.size()), dims, size,
                                QString::number(qRound(scale() * 100)), extra);
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 1.05);
        p.setFont(f);
        QRect r = p.fontMetrics().boundingRect(text).adjusted(-12, -6, 12, 6);
        r.moveTopLeft(rect().topLeft() + QPoint(12, 12));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(QRectF(r), 8, 8);
        p.setPen(QColor("#eee"));
        p.drawText(r, Qt::AlignCenter, text);
    }
}

void ImageViewer::resizeEvent(QResizeEvent *ev)
{
    clamp();
    QWidget::resizeEvent(ev);
}

// ------------------------------------------------------------ input

void ImageViewer::add_shortcuts()
{
    auto add = [this](const QStringList &keys, std::function<void()> fn) {
        auto *a = new QAction(this);
        QList<QKeySequence> seqs;
        for (const QString &k : keys)
            seqs << QKeySequence(k);
        a->setShortcuts(seqs);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
    };
    add({"Right", "Space", "PgDown", "Down"}, [this]() { navigate(1); });
    add({"Left", "Backspace", "PgUp", "Up"}, [this]() { navigate(-1); });
    add({"Home"}, [this]() { go_to(0); });
    add({"End"}, [this]() { go_to(paths.size() - 1); });
    add({"F", "F11", "Return"}, [this]() { toggle_fullscreen(); });
    add({"Escape"}, [this]() { escape(); });
    add({"Q", "Ctrl+W"}, [this]() { close(); });
    add({"+", "=", "Ctrl++"}, [this]() { set_zoom(scale() * 1.25); });
    add({"-", "Ctrl+-"}, [this]() { set_zoom(scale() / 1.25); });
    add({"0"}, [this]() { fit(); });
    add({"1"}, [this]() { set_zoom(1.0); });
    add({"U"}, [this]() { toggle_upscale(); });
    add({"R"}, [this]() { rotate(90); });
    add({"L", "Shift+R"}, [this]() { rotate(-90); });
    add({"H"}, [this]() { flip(); });
    add({"I", "Tab"}, [this]() { toggle_info(); });
    add({"S"}, [this]() { toggle_slideshow(); });
    add({"Delete"}, [this]() { delete_current(); });
    add({"Ctrl+C"}, [this]() { copy_image(); });
    add({"Alt+Return", "Ctrl+I"}, [this]() {
        if (cb.on_properties)
            cb.on_properties(current());
    });
}

void ImageViewer::escape()
{
    if (slideshow.isActive())
        toggle_slideshow();
    else if (isFullScreen())
        toggle_fullscreen();
    else
        close();
}

void ImageViewer::fit()
{
    zoom.reset();
    offset = QPointF(0, 0);
    update();
}

void ImageViewer::toggle_upscale()
{
    upscale = !upscale;
    fit();
}

void ImageViewer::rotate(int deg)
{
    rotation = ((rotation + deg) % 360 + 360) % 360;
    fit();
}

void ImageViewer::flip()
{
    flip_h = !flip_h;
    update();
}

void ImageViewer::toggle_info()
{
    show_info = !show_info;
    update();
}

void ImageViewer::toggle_fullscreen()
{
    if (isFullScreen()) {
        showNormal();
        setCursor(Qt::ArrowCursor);
    } else {
        showFullScreen();
        hide_cursor.start();
    }
}

void ImageViewer::toggle_slideshow()
{
    if (slideshow.isActive())
        slideshow.stop();
    else
        slideshow.start(settings().value("slideshow_secs", 4).toInt() * 1000);
    update();
}

void ImageViewer::copy_image()
{
    if (!pix.isNull())
        QGuiApplication::clipboard()->setPixmap(pix);
}

void ImageViewer::wheelEvent(QWheelEvent *ev)
{
    int dy = ev->angleDelta().y();
    if (!dy)
        return;
    if (ev->modifiers() & Qt::ControlModifier) {
        set_zoom(scale() * (dy > 0 ? 1.15 : 1 / 1.15), ev->position());
        return;
    }
    wheel_acc += dy;   // smooth touchpads send small deltas
    if (std::abs(wheel_acc) >= 120) {
        navigate(wheel_acc > 0 ? -1 : 1);
        wheel_acc = 0;
    }
}

void ImageViewer::mousePressEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton) {
        dragging = true;
        drag_start = ev->position();
        drag_offset = offset;
    } else if (ev->button() == Qt::BackButton) {
        navigate(-1);
    } else if (ev->button() == Qt::ForwardButton) {
        navigate(1);
    } else if (ev->button() == Qt::MiddleButton) {
        close();
    }
}

void ImageViewer::mouseMoveEvent(QMouseEvent *ev)
{
    setCursor(dragging ? Qt::ClosedHandCursor : Qt::ArrowCursor);
    if (isFullScreen())
        hide_cursor.start();
    if (dragging) {
        if (!zoom)
            zoom = fit_scale();
        offset = drag_offset + (ev->position() - drag_start);
        clamp();
        update();
    }
}

void ImageViewer::mouseReleaseEvent(QMouseEvent *)
{
    dragging = false;
    setCursor(Qt::ArrowCursor);
}

void ImageViewer::mouseDoubleClickEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton)
        toggle_fullscreen();
}

void ImageViewer::contextMenuEvent(QContextMenuEvent *ev)
{
    QString path = current();
    if (path.isEmpty())
        return;
    QMenu m(this);
    m.addAction("Next\tRight", this, [this]() { navigate(1); });
    m.addAction("Previous\tLeft", this, [this]() { navigate(-1); });
    m.addSeparator();
    m.addAction(isFullScreen() ? "Exit Fullscreen\tF" : "Fullscreen\tF", this, [this]() { toggle_fullscreen(); });
    m.addAction(slideshow.isActive() ? "Stop Slideshow\tS" : "Start Slideshow\tS", this,
                [this]() { toggle_slideshow(); });
    m.addAction("Fit to Window\t0", this, [this]() { fit(); });
    m.addAction("Actual Size\t1", this, [this]() { set_zoom(1.0); });
    m.addAction("Rotate Right\tR", this, [this]() { rotate(90); });
    m.addAction("Rotate Left\tL", this, [this]() { rotate(-90); });
    m.addAction("Flip Horizontally\tH", this, [this]() { flip(); });
    m.addSeparator();
    m.addAction("Open With Default Application", this, [path]() { open_default(path); });
    if (cb.on_open_with)
        m.addAction("Open With…", this, [this, path]() { cb.on_open_with(path); });
    m.addAction("Copy Image\tCtrl+C", this, [this]() { copy_image(); });
    m.addAction("Copy Path", this, [path]() { QGuiApplication::clipboard()->setText(path); });
    m.addAction(uwp::wallpaper_label(), this, [path]() { set_wallpaper(path); });
    if (uwp::editor_open())
        m.addAction("Add to Selected UWP Monitor", this, [path]() { uwp::add_to_selected(path); });
    m.addSeparator();
    if (cb.on_properties)
        m.addAction("Properties\tAlt+Return", this, [this, path]() { cb.on_properties(path); });
    if (cb.on_delete)
        m.addAction("Move to Trash\tDelete", this, [this]() { delete_current(); });
    m.exec(ev->globalPos());
}

void ImageViewer::closeEvent(QCloseEvent *ev)
{
    slideshow.stop();
    stop_movie();
    pool.clear();
    if (cb.on_close && !paths.isEmpty())
        cb.on_close(current());
    QWidget::closeEvent(ev);
}
