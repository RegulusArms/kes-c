#include "viewer.h"

#include "drives.h"
#include "fileops.h"
#include "imgedit.h"
#include "menus.h"
#include "metadata.h"
#include "places.h"
#include "prefs.h"
#include "thumbs.h"
#include "undo.h"
#include "util.h"
#include "uwp.h"
#include "windows.h"

#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include <QWheelEvent>

using namespace util;

static const QSet<QString> ANIMATABLE = {".gif", ".webp", ".avif", ".jxl", ".mng", ".apng"};
static const QString FULL = "\x01full";   // pending: a full-size decode of the path before it

// A rounded dark box with text in it, placed in `area` by `where`
static void bubble(QPainter &p, const QRect &area, const QString &text, Qt::Alignment where, double grow = 1.05)
{
    QFont f = p.font();
    f.setPointSizeF(f.pointSizeF() * grow);
    p.setFont(f);
    QRect r = p.fontMetrics().boundingRect(text).adjusted(-12, -6, 12, 6);
    QRect inner = area.adjusted(12, 12, -12, -12);
    int x = where & Qt::AlignRight ? inner.right() - r.width() : where & Qt::AlignHCenter ? inner.center().x() - r.width() / 2 : inner.left();
    int y = where & Qt::AlignBottom ? inner.bottom() - r.height() : where & Qt::AlignVCenter ? inner.center().y() - r.height() / 2 : inner.top();
    r.moveTopLeft(QPoint(x, y));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 160));
    p.drawRoundedRect(QRectF(r), 8, 8);
    p.setPen(QColor("#eee"));
    p.drawText(r, Qt::AlignCenter, text);
}

// ================================================================ ViewerCanvas

ViewerCanvas::ViewerCanvas(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setMinimumSize(80, 60);
    osd_timer.setSingleShot(true);
    osd_timer.setInterval(1200);
    connect(&osd_timer, &QTimer::timeout, this, [this]() { update(); });
}

void ViewerCanvas::set_image(const QPixmap &pm, QSize f)
{
    pix = pm;
    full = f.isEmpty() ? pm.size() : f;
    message.clear();
    clamp();
    update();
}

void ViewerCanvas::clear_image(const QString &msg)
{
    pix = QPixmap();
    full = QSize();
    message = msg;
    update();
}

void ViewerCanvas::reset_view()
{
    zoom.reset();
    offset = QPointF(0, 0);
    rotation = 0;
    flip_h = false;
    stop_crop();
    update();
}

QSizeF ViewerCanvas::img_size() const
{
    if (full.isEmpty())
        return {1, 1};
    return rotation % 180 ? QSizeF(full.height(), full.width()) : QSizeF(full);
}

double ViewerCanvas::fit_scale() const
{
    QSizeF s = img_size();
    double f = std::min(width() / s.width(), height() / s.height());
    return (upscale || f < 1) ? f : 1.0;
}

double ViewerCanvas::scale() const { return zoom ? *zoom : fit_scale(); }

QPointF ViewerCanvas::center() const { return QPointF(width() / 2.0, height() / 2.0) + offset; }

void ViewerCanvas::set_zoom(double z, std::optional<QPointF> anchor)
{
    double old = scale();
    z = std::max(0.02, std::min(z, 40.0));
    QPointF mid(width() / 2.0, height() / 2.0);
    QPointF a = anchor ? *anchor : mid;
    QPointF img_pt = (a - center()) / old;
    zoom = z;
    offset = a - mid - img_pt * z;
    clamp();
    show_osd(QString("%1%").arg(qRound(z * 100)));
    Q_EMIT view_changed();
}

void ViewerCanvas::fit()
{
    zoom.reset();
    offset = QPointF(0, 0);
    show_osd("Fit");
    Q_EMIT view_changed();
}

void ViewerCanvas::toggle_zoom_at(QPointF pos)
{
    if (zoom && std::fabs(*zoom - fit_scale()) > 1e-3)
        fit();
    else
        set_zoom(fit_scale() < 0.999 ? 1.0 : fit_scale() * 2, pos);
}

void ViewerCanvas::rotate(int deg)
{
    rotation = ((rotation + deg) % 360 + 360) % 360;
    zoom.reset();
    offset = QPointF(0, 0);
    stop_crop();
    update();
}

void ViewerCanvas::flip()
{
    flip_h = !flip_h;
    stop_crop();
    update();
}

void ViewerCanvas::show_osd(const QString &text)
{
    osd = text;
    osd_timer.start();
    update();
}

QString ViewerCanvas::osd_text() const { return osd_timer.isActive() ? osd : QString(); }

void ViewerCanvas::start_crop()
{
    cropping = true;
    crop = QRectF();
    update();
}

void ViewerCanvas::stop_crop()
{
    cropping = false;
    crop = QRectF();
    dragging = false;
    update();
}

QRect ViewerCanvas::crop_rect() const
{
    QRect bounds(QPoint(0, 0), img_size().toSize());
    return crop.normalized().toAlignedRect().intersected(bounds);
}

QPointF ViewerCanvas::to_image(QPointF w) const
{
    QSizeF t = img_size();
    return (w - center()) / scale() + QPointF(t.width() / 2, t.height() / 2);
}

void ViewerCanvas::clamp()
{
    QSizeF s = img_size();
    double sc = scale();
    double mx = std::max(0.0, (s.width() * sc - width()) / 2);
    double my = std::max(0.0, (s.height() * sc - height()) / 2);
    offset = QPointF(std::max(-mx, std::min(mx, offset.x())), std::max(-my, std::min(my, offset.y())));
}

void ViewerCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor("#111"));
    double s = scale();
    if (!pix.isNull() && !full.isEmpty()) {
        double fw = full.width(), fh = full.height();
        p.setRenderHint(QPainter::SmoothPixmapTransform, s * fw / pix.width() < 2.5);
        QPointF c = center();
        QTransform t;
        t.translate(c.x(), c.y());
        t.rotate(rotation);
        t.scale(flip_h ? -s : s, s);
        p.setTransform(t);
        p.drawPixmap(QRectF(-fw / 2, -fh / 2, fw, fh), pix, QRectF(pix.rect()));
        p.resetTransform();
    } else if (!message.isEmpty()) {
        p.setPen(QColor("#aaa"));
        p.drawText(rect(), Qt::AlignCenter, message);
    }
    if (cropping && !pix.isNull()) {
        QSizeF t = img_size();
        QPointF origin = center() - QPointF(t.width() * s / 2, t.height() * s / 2);
        QRectF img_r(origin, t * s);
        QRectF c = crop.normalized();
        QRectF sel = c.isEmpty() ? QRectF() : QRectF(origin + c.topLeft() * s, c.size() * s).intersected(img_r);
        QPainterPath shade;   // odd-even: the selection is a hole in it
        shade.addRect(img_r);
        if (!sel.isEmpty())
            shade.addRect(sel);
        p.fillPath(shade, QColor(0, 0, 0, 150));
        if (!sel.isEmpty()) {
            p.setPen(QPen(Qt::white, 1, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(sel);
        }
        QRect r = crop_rect();
        bubble(p, rect(), sel.isEmpty() ? QString("Drag over the part to keep · Esc: cancel")
                                        : QString("%1 × %2 · Enter: crop · Esc: cancel").arg(r.width()).arg(r.height()),
               Qt::AlignBottom | Qt::AlignHCenter);
    }
    if (!overlay.isEmpty())
        bubble(p, rect(), overlay, Qt::AlignTop | Qt::AlignLeft);
    if (!tag.isEmpty())
        bubble(p, rect(), tag, Qt::AlignTop | Qt::AlignRight, 1.6);
    if (osd_timer.isActive())
        bubble(p, rect(), osd, Qt::AlignCenter, 1.5);
}

void ViewerCanvas::resizeEvent(QResizeEvent *ev)
{
    clamp();
    QWidget::resizeEvent(ev);
}

void ViewerCanvas::wheelEvent(QWheelEvent *ev)
{
    int dy = ev->angleDelta().y();
    if (!(ev->modifiers() & Qt::ControlModifier) || !dy) {
        ev->ignore();   // the viewer: next / previous image
        return;
    }
    set_zoom(scale() * (dy > 0 ? 1.15 : 1 / 1.15), ev->position());
}

void ViewerCanvas::mousePressEvent(QMouseEvent *ev)
{
    if (ev->button() != Qt::LeftButton) {
        ev->ignore();   // back / forward / middle: the viewer's
        return;
    }
    dragging = true;
    if (cropping) {
        QSizeF t = img_size();
        QPointF q = to_image(ev->position());
        crop_anchor = QPointF(std::clamp(q.x(), 0.0, t.width()), std::clamp(q.y(), 0.0, t.height()));
        crop = QRectF(crop_anchor, crop_anchor);
        update();
        return;
    }
    drag_start = ev->position();
    drag_offset = offset;
}

void ViewerCanvas::mouseMoveEvent(QMouseEvent *ev)
{
    Q_EMIT pointer_moved();
    if (!dragging) {
        setCursor(cropping ? Qt::CrossCursor : Qt::ArrowCursor);
        return;
    }
    if (cropping) {
        QSizeF t = img_size();
        QPointF q = to_image(ev->position());
        crop = QRectF(crop_anchor, QPointF(std::clamp(q.x(), 0.0, t.width()), std::clamp(q.y(), 0.0, t.height())));
        update();
        return;
    }
    setCursor(Qt::ClosedHandCursor);
    if (!zoom)
        zoom = fit_scale();
    offset = drag_offset + (ev->position() - drag_start);
    clamp();
    update();
    Q_EMIT view_changed();
}

void ViewerCanvas::mouseReleaseEvent(QMouseEvent *)
{
    dragging = false;
    setCursor(cropping ? Qt::CrossCursor : Qt::ArrowCursor);
}

void ViewerCanvas::mouseDoubleClickEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton && !cropping)
        toggle_zoom_at(ev->position());
    else
        ev->ignore();
}

bool ViewerCanvas::event(QEvent *ev)
{
    if (ev->type() == QEvent::NativeGesture) {   // a touchpad's pinch
        auto *g = static_cast<QNativeGestureEvent *>(ev);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            set_zoom(scale() * (1 + g->value()), g->position());
            return true;
        }
        if (g->gestureType() == Qt::SmartZoomNativeGesture) {
            toggle_zoom_at(g->position());
            return true;
        }
    }
    return QWidget::event(ev);
}

// ================================================================ Filmstrip

static const int CELL = 84;

Filmstrip::Filmstrip(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(88);
    setFocusPolicy(Qt::NoFocus);
    if (auto *tm = ThumbnailManager::instance())
        connect(tm, &ThumbnailManager::updated, this, [this](const QString &p) {
            if (asked.contains(p))
                update();
        });
}

void Filmstrip::set_items(const QStringList &p, int cur)
{
    paths = p;
    current = cur;
    update();
}

QRect Filmstrip::cell_rect(int i) const { return QRect(width() / 2 - CELL / 2 + (i - current) * CELL, 0, CELL, height()); }

int Filmstrip::index_at(QPoint pos) const
{
    int i = current + int(std::floor((pos.x() - (width() / 2 - CELL / 2)) / double(CELL)));
    return i >= 0 && i < paths.size() ? i : -1;
}

QPixmap Filmstrip::thumb(const QString &path)
{
    auto *tm = ThumbnailManager::instance();
    if (!tm)
        return QPixmap();
    auto it = stats.find(path);
    if (it == stats.end()) {
        struct stat st;
        bool ok = stat_(path, st) && (drives::slow_root(path).isEmpty() || drives::previews_allowed(path));
        it = stats.insert(path, ok ? qMakePair(qint64(st.st_mtime), qint64(st.st_size)) : qMakePair(qint64(-1), qint64(-1)));
    }
    if (it->first < 0)
        return QPixmap();
    asked.insert(path);
    return tm->get(path, it->first, false, 128, it->second);
}

void Filmstrip::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor("#181818"));
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    int half = width() / 2 / CELL + 2;
    for (int i = std::max(0, current - half); i < std::min<int>(paths.size(), current + half + 1); ++i) {
        QRect r = cell_rect(i).adjusted(6, 8, -6, -8);
        QPixmap pm = thumb(paths[i]);
        p.setOpacity(i == current ? 1.0 : 0.6);
        if (pm.isNull()) {
            p.fillRect(r, QColor("#2a2a2a"));
        } else {
            QSize s = pm.size().scaled(r.size(), Qt::KeepAspectRatio);
            p.drawPixmap(QRect(r.center() - QPoint(s.width() / 2, s.height() / 2), s), pm);
        }
        p.setOpacity(1);
        if (i == current) {
            p.setPen(QPen(QColor("#d9652f"), 2));
            p.setBrush(Qt::NoBrush);
            p.drawRect(r.adjusted(-3, -3, 3, 3));
        }
    }
}

void Filmstrip::mousePressEvent(QMouseEvent *ev)
{
    int i = index_at(ev->position().toPoint());
    if (ev->button() == Qt::LeftButton && i >= 0)
        Q_EMIT activated(i);
    else
        ev->ignore();
}

void Filmstrip::wheelEvent(QWheelEvent *ev)
{
    QPoint d = ev->angleDelta();
    wheel_acc += std::abs(d.x()) > std::abs(d.y()) ? -d.x() : d.y();
    if (std::abs(wheel_acc) >= 120) {
        int i = std::max(0, std::min<int>(current + (wheel_acc > 0 ? -1 : 1), paths.size() - 1));
        wheel_acc = 0;
        if (i != current)
            Q_EMIT activated(i);
    }
}

// ================================================================ Histogram

Histogram::Histogram(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(90);
    for (auto &b : bins)
        b = QVector<int>(256, 0);
}

void Histogram::set_image(const QPixmap &pm)
{
    for (auto &b : bins)
        b.fill(0);
    if (!pm.isNull()) {
        QImage img = pm.toImage().scaled(256, 256, Qt::KeepAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < img.height(); ++y) {
            auto *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                ++bins[0][qRed(line[x])];
                ++bins[1][qGreen(line[x])];
                ++bins[2][qBlue(line[x])];
            }
        }
    }
    update();
}

void Histogram::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor("#151515"));
    int top = 1;
    for (const auto &b : bins)
        for (int i = 1; i < 255; ++i)   // not the ends: clipped highlights and shadows would flatten the rest
            top = std::max(top, b[i]);
    p.setRenderHint(QPainter::Antialiasing);
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    const QColor colors[3] = {QColor(200, 40, 40), QColor(40, 170, 40), QColor(50, 80, 220)};
    double w = width(), h = height() - 2;
    for (int c = 0; c < 3; ++c) {
        QPainterPath path(QPointF(0, height()));
        for (int i = 0; i < 256; ++i)
            path.lineTo(i * w / 255, height() - std::min(1.0, bins[c][i] / double(top)) * h);
        path.lineTo(w, height());
        path.closeSubpath();
        p.fillPath(path, colors[c]);
    }
}

// ================================================================ ViewerInfo

ViewerInfo::ViewerInfo(QWidget *parent) : QScrollArea(parent)
{
    setObjectName("viewer_info");
    setWidgetResizable(true);
    setFixedWidth(340);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::NoFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *box = new QWidget;
    box->setObjectName("viewer_info_box");
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(14, 14, 14, 14);
    v->setSpacing(10);
    title = new QLabel;
    title->setWordWrap(true);
    QFont f = title->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 1.15);
    title->setFont(f);
    histogram = new Histogram;
    rows_box = new QWidget;
    rows = new QVBoxLayout(rows_box);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(6);
    v->addWidget(title);
    v->addWidget(histogram);
    v->addWidget(rows_box);
    v->addStretch(1);
    setWidget(box);
    setStyleSheet("#viewer_info, #viewer_info_box { background: #1b1b1b; } #viewer_info QLabel { color: #ddd; }");
    QPalette pal = palette();
    pal.setColor(QPalette::Link, QColor("#7ab7ff"));   // the map link, readable on the dark panel
    setPalette(pal);
}

void ViewerInfo::row(const QString &k, const QString &v, bool rich)
{
    auto *l = new QLabel(QString("<span style=\"color:#8a8a8a\">%1</span><br>%2").arg(k.toHtmlEscaped(), rich ? v : v.toHtmlEscaped()));
    l->setWordWrap(true);
    l->setTextInteractionFlags(rich ? Qt::TextBrowserInteraction : Qt::TextSelectableByMouse);
    l->setOpenExternalLinks(rich);
    rows->addWidget(l);
}

void ViewerInfo::clear_rows()
{
    while (QLayoutItem *it = rows->takeAt(0)) {
        delete it->widget();
        delete it;
    }
}

QString ViewerInfo::text() const
{
    QStringList out{title->text()};
    for (QLabel *l : rows_box->findChildren<QLabel *>())
        out << l->text();
    for (QPlainTextEdit *e : rows_box->findChildren<QPlainTextEdit *>())
        out << e->toPlainText();
    return out.join('\n');
}

void ViewerInfo::show_path(const QString &p)
{
    if (p == path)
        return;
    path = p;
    loaded = false;
    map_url.clear();
    int my = ++gen;
    clear_rows();
    title->setText(basename(p));
    struct stat st;
    if (stat_(p, st)) {
        row("Size", human_size(qint64(st.st_size)));
        row("Modified", fmt_time(st.st_mtime, "%Y-%m-%d %H:%M"));
    }
    row("Folder", dirname(p));
    auto *reading = new QLabel("Reading its details…");
    rows->addWidget(reading);
    QPointer<ViewerInfo> self(this);
    fileops::run_task(
        this, "",
        [p]() {
            QVariantMap r;
            QVariantList basic, ai, all;
            for (const auto &[k, v] : metadata::basic_info(p))
                basic << QVariant(QStringList{k, v});
            for (const auto &[k, v] : metadata::ai_info(p))
                ai << QVariant(QStringList{k, v});
            for (const metadata::MetaRow &m : metadata::full_metadata(p))
                all << QVariant(QStringList{m.group, m.tag, m.value});
            double lat, lon;
            if (metadata::gps_position(p, &lat, &lon))
                r["gps"] = QVariantList{lat, lon};
            r["basic"] = basic;
            r["ai"] = ai;
            r["all"] = all;
            return QVariant(r);
        },
        [self, my, reading](const QVariant &res) {
            if (!self || self->gen != my)
                return;
            delete reading;
            QVariantMap r = res.toMap();
            QVariantList gps = r.value("gps").toList();
            for (const QVariant &x : r.value("basic").toList()) {
                QStringList kv = x.toStringList();
                if (kv.value(0) == "GPS" && gps.size() == 2) {
                    QString lat = QString::number(gps[0].toDouble(), 'f', 6), lon = QString::number(gps[1].toDouble(), 'f', 6);
                    self->map_url = QString("https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=16/%1/%2").arg(lat, lon);
                    self->row("GPS", QString("%1 · <a href=\"%2\">Show on a map</a>").arg(kv.value(1).toHtmlEscaped(), self->map_url), true);
                } else {
                    self->row(kv.value(0), kv.value(1));
                }
            }
            for (const QVariant &x : r.value("ai").toList()) {
                QString k = x.toStringList().value(0), v = x.toStringList().value(1);
                if (v.size() <= 60 && !v.contains('\n')) {
                    self->row(k, v);
                    continue;
                }
                auto *lab = new QLabel(QString("<span style=\"color:#8a8a8a\">%1</span>").arg(k.toHtmlEscaped()));
                auto *ed = new QPlainTextEdit(v);
                ed->setReadOnly(true);
                ed->setMaximumHeight(120);
                auto *btn = new QPushButton("Copy");
                btn->setFocusPolicy(Qt::NoFocus);
                QObject::connect(btn, &QPushButton::clicked, self, [v]() { QGuiApplication::clipboard()->setText(v); });
                self->rows->addWidget(lab);
                self->rows->addWidget(ed);
                self->rows->addWidget(btn, 0, Qt::AlignLeft);
            }
            QString html, group;
            for (const QVariant &x : r.value("all").toList()) {
                QStringList m = x.toStringList();
                if (m.value(0) != group) {
                    group = m.value(0);
                    html += QString("%1<b>%2</b><br>").arg(html.isEmpty() ? "" : "<br>", group.toHtmlEscaped());
                }
                QString v = m.value(2);
                if (v.size() > 200)
                    v = v.left(200) + "…";
                html += QString("<span style=\"color:#8a8a8a\">%1</span> %2<br>").arg(m.value(1).toHtmlEscaped(), v.toHtmlEscaped());
            }
            if (!html.isEmpty()) {
                auto *all = new QLabel("<span style=\"color:#8a8a8a\">All metadata</span><br><br>" + html);
                all->setWordWrap(true);
                all->setTextInteractionFlags(Qt::TextSelectableByMouse);
                self->rows->addWidget(all);
            }
            self->loaded = true;
            Q_EMIT self->details_loaded();
        },
        true);
}

// ================================================================ ImageViewer

ImageViewer::ImageViewer(const QStringList &paths_, int idx, Callbacks callbacks)
    : QWidget(nullptr, Qt::Window), paths(paths_), cb(std::move(callbacks))
{
    setAttribute(Qt::WA_DeleteOnClose);
    index = std::max(0, std::min<int>(idx, paths.size() - 1));
    pool.setMaxThreadCount(3);
    connect(this, &ImageViewer::loaded, this, &ImageViewer::on_loaded, Qt::QueuedConnection);
    connect(&slideshow, &QTimer::timeout, this, [this]() { navigate(1, true); });
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor("#111"));
    setPalette(pal);
    setAutoFillBackground(true);

    canvas = new ViewerCanvas;
    pinned = new ViewerCanvas;
    pinned->hide();
    filmstrip = new Filmstrip;
    info = new ViewerInfo;
    auto *canvases = new QHBoxLayout;
    canvases->setContentsMargins(0, 0, 0, 0);
    canvases->setSpacing(2);
    canvases->addWidget(pinned);
    canvases->addWidget(canvas);
    auto *col = new QVBoxLayout;
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    col->addLayout(canvases, 1);
    col->addWidget(filmstrip);
    auto *top = new QHBoxLayout(this);
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(0);
    top->addLayout(col, 1);
    top->addWidget(info);
    filmstrip->setVisible(settings().value("viewer_filmstrip", true).toBool());
    info->setVisible(settings().value("viewer_info_panel", false).toBool());
    keep_zoom = settings().value("viewer_keep_zoom", false).toBool();
    connect(filmstrip, &Filmstrip::activated, this, [this](int i) { go_to(i); });
    for (ViewerCanvas *c : {canvas, pinned}) {
        connect(c, &ViewerCanvas::view_changed, this, [this, c]() {
            link_view(c, c == canvas ? pinned : canvas);
            want_full(c, c == canvas ? shown : pinned_path);
            update_overlay();
        });
        connect(c, &ViewerCanvas::pointer_moved, this, [this]() {
            if (isFullScreen())
                hide_cursor.start();
        });
    }
    hide_cursor.setSingleShot(true);
    hide_cursor.setInterval(2000);
    connect(&hide_cursor, &QTimer::timeout, this, [this]() {
        if (isFullScreen())
            for (ViewerCanvas *c : {canvas, pinned})
                c->setCursor(Qt::BlankCursor);
    });
    QByteArray geo = settings().value("viewer_geometry").toByteArray();
    if (geo.isEmpty() || !restoreGeometry(geo))
        resize(1200, 800);
    add_shortcuts();
    show_current();
}

ImageViewer::~ImageViewer()
{
    pool.clear();
    pool.waitForDone();
}

void ImageViewer::ask(QWidget *box)
{
    if (on_question)
        on_question(box);
}

// ------------------------------------------------------------ loading

int ImageViewer::screen_side() const
{
    QScreen *s = screen() ? screen() : QGuiApplication::primaryScreen();
    if (!s)
        return 4096;
    return std::max(1024, qRound(std::max(s->geometry().width(), s->geometry().height()) * s->devicePixelRatio()));
}

// Decoded at screen size unless `full` (most images are bigger than the screen, and decoding less is faster and
// takes less memory); full size is asked for when zoomed in past that (want_full).
void ImageViewer::request(const QString &path, bool full)
{
    auto it = cache.constFind(path);
    if (it != cache.constEnd() && (it->is_full || !full || it->pix.isNull()))
        return;
    QString key = full ? path + FULL : path;
    if (pending.contains(key) || pending.contains(path + FULL))
        return;
    pending << key;
    int side = screen_side(), gen = generation.value(path);
    pool.start([this, path, full, side, gen]() {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (is_raw(path) && !full)
            reader.setQuality(0);   // RAW: the camera's embedded preview (or a fast half-size decode) first
        QSize stored = reader.size();
        if (!full && stored.isValid() && std::max(stored.width(), stored.height()) > side)
            reader.setScaledSize(stored.scaled(side, side, Qt::KeepAspectRatio));
        QImage img = reader.read();
        QSize real = stored.isValid() ? stored : img.size();
        if (!img.isNull() && (img.width() > img.height()) != (real.width() > real.height()))
            real.transpose();   // turned by its orientation tag
        Q_EMIT loaded(path, img, real, full || img.size() == real, gen);
    });
}

void ImageViewer::on_loaded(const QString &path, const QImage &img, QSize full, bool is_full, int gen)
{
    if (gen != generation.value(path))   // started before the file was deleted, moved or saved over: out of date
        return;
    pending.remove(path);
    pending.remove(path + FULL);
    auto it = cache.constFind(path);
    if (it != cache.constEnd() && it->is_full && !is_full)
        return;
    cache.insert(path, Entry{img.isNull() ? QPixmap() : QPixmap::fromImage(img), full, is_full});
    cache_order.removeAll(path);
    cache_order << path;
    for (int i = 0; cache_order.size() > 7 && i < cache_order.size();) {
        if (cache_order[i] == shown || cache_order[i] == pinned_path) {
            ++i;
            continue;
        }
        cache.remove(cache_order.takeAt(i));
    }
    if (path == shown && !movie)
        show_on(canvas, path);
    if (compare && path == pinned_path)
        show_on(pinned, path);
}

void ImageViewer::show_on(ViewerCanvas *c, const QString &path)
{
    auto it = cache.constFind(path);
    if (it == cache.constEnd())
        return;
    if (it->pix.isNull()) {
        c->clear_image("Cannot display this image");
    } else if (c == canvas && path == shown && !steps.isEmpty()) {   // the edits, previewed on the decoded copy
        QSize real;
        c->set_image(QPixmap::fromImage(imgedit::preview(it->pix.toImage(), it->full, steps, &real)), real);
        want_full(c, path);
    } else {
        c->set_image(it->pix, it->full);
        want_full(c, path);
    }
    if (c == canvas) {
        update_overlay();
        if (!info->isHidden())
            info->set_histogram(it->pix);
    }
}

void ImageViewer::want_full(ViewerCanvas *c, const QString &path)
{
    auto it = cache.constFind(path);
    if (it != cache.constEnd() && !it->is_full && !it->pix.isNull() && !c->pix.isNull() &&
        c->scale() * c->full.width() > c->pix.width() * 1.05)
        request(path, true);
}

QString ImageViewer::current() const { return paths.isEmpty() ? QString() : paths[index]; }

void ImageViewer::show_current()
{
    if (paths.isEmpty()) {
        close();
        return;
    }
    QString path = current();
    std::optional<double> z = canvas->zoom;
    QPointF off = canvas->offset;
    canvas->reset_view();
    steps.clear();   // saved or asked about by leave_image
    if (keep_zoom && z && !shown.isEmpty()) {
        canvas->zoom = z;
        canvas->offset = off;
    }
    shown = path;
    stop_movie();
    struct stat st;
    shown_bytes = stat_(path, st) ? qint64(st.st_size) : -1;
    setWindowTitle(QString("%1 — %2/%3").arg(basename(path), QString::number(index + 1), QString::number(paths.size())));
    // full-size decodes no longer on screen go (they're big); screen-size ones stay, for going back
    for (auto it = cache.begin(); it != cache.end();) {
        if (it->is_full && it.key() != path && it.key() != pinned_path &&
            std::max(it->pix.width(), it->pix.height()) > screen_side()) {
            cache_order.removeAll(it.key());
            it = cache.erase(it);
        } else {
            ++it;
        }
    }
    bool animated = false;
    if (ANIMATABLE.contains(ext_of(path))) {
        QImageReader reader(path);
        animated = reader.supportsAnimation() && reader.imageCount() != 1;
    }
    if (animated) {
        movie = new QMovie(path, QByteArray(), this);
        connect(movie, &QMovie::frameChanged, this, &ImageViewer::movie_frame);
        movie->start();
    } else if (cache.contains(path)) {
        show_on(canvas, path);
    } else {
        // the preview from the thumbnail cache meanwhile, if there's one (embedded previews are what it's made from
        // for RAW photos), then the image itself
        QPixmap thumb;
        if (auto *tm = ThumbnailManager::instance(); tm && shown_bytes >= 0)
            thumb = tm->get(path, qint64(st.st_mtime), false, 256, shown_bytes);
        if (thumb.isNull())
            canvas->clear_image("Loading…");
        else
            canvas->set_image(thumb, thumb.size().scaled(canvas->size(), Qt::KeepAspectRatio));
        request(path);
    }
    for (int d : {1, -1, 2}) {
        int j = index + d;
        if (j >= 0 && j < paths.size())
            request(paths[j]);
    }
    filmstrip->set_items(paths, index);
    if (!info->isHidden())
        info->show_path(path);
    update_overlay();
}

void ImageViewer::movie_frame()
{
    if (movie) {
        QPixmap pm = movie->currentPixmap();
        canvas->set_image(pm, pm.size());
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

void ImageViewer::update_overlay()
{
    QString path = current();
    canvas->overlay.clear();
    pinned->overlay.clear();
    if (show_overlay && !path.isEmpty()) {
        QSize full = cache.contains(path) && steps.isEmpty() ? cache.value(path).full : canvas->full;
        QStringList parts{basename(path), QString("%1 / %2").arg(index + 1).arg(paths.size())};
        if (!full.isEmpty())
            parts << QString("%1 × %2").arg(full.width()).arg(full.height());
        if (shown_bytes >= 0)
            parts << human_size(shown_bytes);
        parts << QString("%1%").arg(qRound(canvas->scale() * 100));
        if (places::is_starred(path))
            parts << "★";
        if (slideshow.isActive())
            parts << "▶ slideshow";
        if (!steps.isEmpty())
            parts << "✎ not saved (Ctrl+S saves, Esc undoes)";
        canvas->overlay = parts.join("    ");
        if (compare)
            pinned->overlay = QString("%1    %2%").arg(basename(pinned_path)).arg(qRound(pinned->scale() * 100));
    }
    canvas->update();
    pinned->update();
}

void ImageViewer::update_layout()
{
    pinned->setVisible(compare == 1 || (compare == 2 && showing_a));
    canvas->setVisible(compare != 2 || !showing_a);
    pinned->tag = compare ? "A" : "";
    canvas->tag = compare ? "B" : "";
    update_overlay();
}

// Comparing: the other image follows the same zoom (relative to fitting the window) and the same part of it
void ImageViewer::link_view(ViewerCanvas *from, ViewerCanvas *to)
{
    if (!compare || linking || to->full.isEmpty() || from->full.isEmpty())
        return;
    linking = true;
    if (!from->zoom) {
        to->zoom.reset();
        to->offset = QPointF(0, 0);
    } else {
        to->zoom = from->scale() / from->fit_scale() * to->fit_scale();
        to->offset = from->offset * (to->scale() * to->img_size().width()) / (from->scale() * from->img_size().width());
    }
    to->clamp();
    to->update();
    want_full(to, to == canvas ? shown : pinned_path);
    linking = false;
}

// ------------------------------------------------------------ navigation

void ImageViewer::navigate(int step, bool wrap)
{
    if (paths.isEmpty())
        return;
    int n = index + step;
    if (wrap)
        n = ((n % int(paths.size())) + int(paths.size())) % int(paths.size());
    go_to(n);
}

void ImageViewer::go_to(int n)
{
    n = std::max(0, std::min<int>(n, paths.size() - 1));
    if (n == index && shown == current())
        return;
    leave_image();
    index = n;
    show_current();
}

// Leaving an image turned with R / L / H: its rotation is saved, or asked about, as Preferences say
void ImageViewer::leave_image()
{
    if (!shown.isEmpty() && !steps.isEmpty()) {
        ask_edit(shown, all_steps(), 1);
        return;
    }
    if (shown.isEmpty() || (canvas->rotation == 0 && !canvas->flip_h))
        return;
    QString mode = settings().value("viewer_save_rotation", "ask").toString();
    if (mode == "never" || !lexists(shown) || imgedit::rotation_method(shown).isEmpty())
        return;
    if (mode == "always")
        write_rotation(shown, canvas->rotation, canvas->flip_h);
    else
        ask_rotation(shown, canvas->rotation, canvas->flip_h, false);
}

void ImageViewer::ask_rotation(const QString &path, int rotation, bool flip, bool closing_viewer)
{
    QString how = imgedit::rotation_method(path);
    auto *box = new QMessageBox(QMessageBox::Question, "Save Rotation",
                                QString("Save the rotation of “%1”?").arg(basename(path)), QMessageBox::NoButton, this);
    box->setInformativeText(how == "pixels" ? "It's written again, turned, in the same lossless format."
                                            : "Only its orientation tag changes: the image itself isn't re-encoded.");
    QAbstractButton *save = box->addButton("Save", QMessageBox::AcceptRole);
    QAbstractButton *always = box->addButton("Always Save", QMessageBox::AcceptRole);
    QAbstractButton *dont = box->addButton("Don't Save", QMessageBox::DestructiveRole);
    QAbstractButton *cancel = closing_viewer ? box->addButton(QMessageBox::Cancel) : nullptr;
    box->setDefaultButton(qobject_cast<QPushButton *>(save));
    QPointer<ImageViewer> self(this);
    ask_box(box, [self, path, rotation, flip, closing_viewer, save, always, dont](QAbstractButton *b) {
        if (!self)
            return;
        if (b == always)
            prefs::set("viewer_save_rotation", "always");
        if (b == save || b == always)
            self->write_rotation(path, rotation, flip);
        if (closing_viewer && (b == save || b == always || b == dont))
            self->close_after_question();
    });
    Q_UNUSED(cancel);
    ask(box);
}

// Closing when the question asked on closing is answered: once its click has been handled (from inside the question's
// own click, a child window of this one, Qt 6.4 leaves the viewer open)
void ImageViewer::close_after_question()
{
    closing = true;
    QTimer::singleShot(0, this, [this]() { close(); });
}

bool ImageViewer::write_rotation(const QString &path, int rotation, bool flip)
{
    try {
        imgedit::save_rotation(path, rotation, flip);
    } catch (const Error &e) {
        warn(this, "Save Rotation", e.message());
        return false;
    }
    after_write(path);
    return true;
}

// A file deleted, moved or changed: its decoded copies go, and decodes of it still running are dropped when they end
// (one that ends after the file has gone would otherwise cache "can't read it", shown if Ctrl+Z puts it back)
void ImageViewer::forget(const QString &path)
{
    cache.remove(path);
    cache_order.removeAll(path);
    pending.remove(path);
    pending.remove(path + FULL);
    ++generation[path];
}

// The file changed under its decoded copies: they go, and what shows it is decoded again
void ImageViewer::after_write(const QString &path)
{
    forget(path);
    if (auto *tm = ThumbnailManager::instance())
        tm->invalidate(path);
    if (path == pinned_path) {
        pinned->reset_view();
        request(path);
    }
    if (path == shown && !movie) {
        steps.clear();
        canvas->reset_view();
        struct stat st;
        shown_bytes = stat_(path, st) ? qint64(st.st_size) : -1;
        request(path);
    }
    Q_EMIT saved(path);
}

void ImageViewer::save_changes()
{
    if (!steps.isEmpty()) {
        ask_edit(shown, all_steps(), 0);
        return;
    }
    if (canvas->rotation == 0 && !canvas->flip_h) {
        canvas->show_osd("It isn't turned (R, L, H turn it)");
        return;
    }
    if (imgedit::rotation_method(shown).isEmpty()) {
        warn(this, "Save Rotation", QString("Kestrel can't save a rotation of “%1” without losing quality.").arg(basename(shown)));
        return;
    }
    if (write_rotation(shown, canvas->rotation, canvas->flip_h))
        canvas->show_osd("Rotation saved");
}

void ImageViewer::delete_current()
{
    QString path = current();
    if (!path.isEmpty() && cb.on_delete && cb.on_delete(path)) {
        removed << qMakePair(path, index);
        drop_current();
    }
}

void ImageViewer::drop_current()
{
    QString path = current();
    canvas->reset_view();   // its rotation went with it
    paths.removeAt(index);
    forget(path);
    if (path == pinned_path) {
        compare = 0;
        pinned_path.clear();
        update_layout();
    }
    index = std::max(0, std::min<int>(index, paths.size() - 1));
    shown.clear();
    show_current();
}

// ------------------------------------------------------------ quick actions

QStringList ImageViewer::recent_folders() { return settings().value("viewer_recent_folders").toStringList(); }

void ImageViewer::remember_folder(const QString &folder)
{
    QStringList l = recent_folders();
    l.removeAll(folder);
    l.prepend(folder);
    settings().setValue("viewer_recent_folders", l.mid(0, 9));
}

void ImageViewer::transfer(const QString &folder, const QString &op) { transfer_file(current(), folder, op); }

void ImageViewer::transfer_file(const QString &path, const QString &folder_in, const QString &op)
{
    QString folder = rstrip(folder_in, '/');
    if (path.isEmpty() || folder.isEmpty() || !cb.on_transfer)
        return;
    if (folder == dirname(path)) {
        canvas->show_osd("It's in that folder already");
        return;
    }
    remember_folder(folder);
    cb.on_transfer(path, folder, op);
    if (op == "move") {
        int i = paths.indexOf(path);
        if (i >= 0) {
            removed << qMakePair(path, i);
            if (i == index) {
                drop_current();
            } else {
                paths.removeAt(i);
                if (i < index)
                    --index;
                filmstrip->set_items(paths, index);
                update_overlay();
            }
        }
    }
    canvas->show_osd(QString("%1 to %2").arg(op == "move" ? "Moving" : "Copying", basename(folder)));
}

void ImageViewer::transfer_recent(int n, const QString &op)
{
    QStringList l = recent_folders();
    if (n < 1 || n > l.size()) {
        canvas->show_osd(QString("No recent folder %1 yet (M or C picks one)").arg(n));
        return;
    }
    transfer(l[n - 1], op);
}

void ImageViewer::choose_transfer(const QString &op)
{
    QString path = current();
    if (path.isEmpty())
        return;
    QStringList l = recent_folders();
    QPointer<ImageViewer> self(this);
    choose_folder(this, op == "move" ? "Move To" : "Copy To", l.isEmpty() ? dirname(path) : l.first(),
                  [self, path, op](const QString &d) {
                      if (self && self->paths.contains(path))   // re-read: it may be gone from the list meanwhile
                          self->transfer_file(path, d, op);
                  });
}

void ImageViewer::undo()
{
    if (!cb.on_undo)
        return;
    QPointer<ImageViewer> self(this);
    cb.on_undo([self]() {
        if (!self)
            return;
        int to = -1;
        for (int k = self->removed.size() - 1; k >= 0; --k) {
            auto [p, i] = self->removed[k];
            if (!lexists(p) || self->paths.contains(p))
                continue;
            int at = std::min<int>(i, self->paths.size());
            self->paths.insert(at, p);
            if (at <= self->index)
                ++self->index;
            if (to >= at)
                ++to;
            to = to < 0 ? at : to;
            self->removed.removeAt(k);
        }
        if (to >= 0) {
            self->go_to(to);
            self->canvas->show_osd("Put back");
        }
    });
}

void ImageViewer::toggle_star()
{
    QString path = current();
    if (path.isEmpty())
        return;
    bool on = !places::is_starred(path);
    places::set_starred({path}, on);
    canvas->show_osd(on ? "★ Starred" : "Not starred");
    update_overlay();
}

void ImageViewer::toggle_pin()
{
    QString path = current();
    if (path.isEmpty())
        return;
    bool on = !places::is_pinned(path);
    places::set_pinned({path}, on);
    canvas->show_osd(on ? "Pinned in its folder" : "Not pinned");
}

// ------------------------------------------------------------ edits

void ImageViewer::start_crop()
{
    if (compare)
        canvas->show_osd("Stop comparing first (X)");
    else if (movie)
        canvas->show_osd("Animations can't be cropped");
    else if (!cache.contains(shown) || canvas->pix.isNull())
        canvas->show_osd("Still loading");
    else
        canvas->start_crop();
}

void ImageViewer::finish_crop()
{
    QRect r = canvas->crop_rect();
    canvas->stop_crop();
    if (r.width() < 2 || r.height() < 2)
        return;
    add_edit(r, QSize());
}

// "50%", "1920x1080" (or ×), "1920" (the longer side); invalid if it isn't one of them
static QSize parse_size(const QString &text, QSize now)
{
    QString t = text.trimmed().toLower().remove(' ');
    bool ok = false;
    QSize out;
    if (t.endsWith('%')) {
        double p = t.chopped(1).toDouble(&ok);
        if (ok && p > 0)
            out = QSize(qRound(now.width() * p / 100), qRound(now.height() * p / 100));
    } else if (t.contains('x') || t.contains(QChar(0xD7))) {
        QStringList wh = t.split(QRegularExpression("[x×]"));
        bool ok2 = false;
        int w = wh.value(0).toInt(&ok), h = wh.value(1).toInt(&ok2);
        if (ok && ok2 && wh.size() == 2)
            out = QSize(w, h);
    } else {
        int n = t.toInt(&ok);
        if (ok && n > 0)
            out = now.scaled(n, n, Qt::KeepAspectRatio);
    }
    if (out.width() < 1 || out.height() < 1 || out.width() > 30000 || out.height() > 30000)
        return QSize();
    return out;
}

void ImageViewer::resize_image()
{
    if (compare || movie || !cache.contains(shown)) {
        canvas->show_osd(compare ? "Stop comparing first (X)" : movie ? "Animations can't be resized" : "Still loading");
        return;
    }
    QString path = shown;
    QSize now = canvas->img_size().toSize();
    QPointer<ImageViewer> self(this);
    ask_text(this, "Resize",
             QString("“%1” is %2 × %3. New size: a percentage (50%), width × height (1920x1080), or the longer side "
                     "in pixels (1920):")
                 .arg(basename(path), QString::number(now.width()), QString::number(now.height())),
             "50%", [self, path, now](const QString &t) {
                 if (!self || self->shown != path)
                     return;
                 QSize s = parse_size(t, now);
                 if (s.isEmpty())
                     warn(self, "Resize", QString("“%1” isn't a size Kestrel understands.").arg(t));
                 else
                     self->add_edit(QRect(), s);
             });
}

void ImageViewer::add_edit(const QRect &crop, const QSize &size)
{
    steps << imgedit::Edit{canvas->rotation, canvas->flip_h, crop, size};
    canvas->reset_view();   // its rotation is in the step now
    show_on(canvas, shown);
    update_overlay();
    canvas->show_osd(QString("%1 × %2").arg(canvas->full.width()).arg(canvas->full.height()));
}

QList<imgedit::Edit> ImageViewer::all_steps() const
{
    QList<imgedit::Edit> l = steps;
    if (canvas->rotation || canvas->flip_h)
        l << imgedit::Edit{canvas->rotation, canvas->flip_h, QRect(), QSize()};
    return l;
}

void ImageViewer::discard_edits()
{
    steps.clear();
    canvas->reset_view();
    show_on(canvas, shown);
    update_overlay();
    canvas->show_osd("Edits undone");
}

void ImageViewer::ask_edit(const QString &src, const QList<imgedit::Edit> &edits, int how)
{
    bool crops = false, sizes = false;
    for (const imgedit::Edit &e : edits) {
        crops |= !e.crop.isEmpty();
        sizes |= !e.size.isEmpty();
    }
    QString what = crops && sizes ? "edited" : crops ? "cropped" : sizes ? "resized" : "turned";
    QString title = crops && !sizes ? "Crop" : sizes && !crops ? "Resize" : "Edit";
    bool can_replace = imgedit::can_write(src);
    auto *box = new QMessageBox(QMessageBox::Question, title,
                                QString("Save the %1 image as a new file, “%2”, or replace “%3” with it?")
                                    .arg(what, basename(imgedit::new_name(src, what)), basename(src)),
                                QMessageBox::NoButton, this);
    box->setInformativeText(can_replace ? "Replacing the original can't be undone."
                                        : "Kestrel can't write this format, so it can only make a new JPEG file.");
    QAbstractButton *as_new = box->addButton("Save as New File", QMessageBox::AcceptRole);
    QAbstractButton *replace = can_replace ? box->addButton("Replace Original", QMessageBox::DestructiveRole) : nullptr;
    QAbstractButton *discard = how ? box->addButton("Discard", QMessageBox::RejectRole) : nullptr;
    if (how != 1)
        box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(qobject_cast<QPushButton *>(as_new));
    QPointer<ImageViewer> self(this);
    ask_box(box, [self, src, what, title, edits, how, as_new, replace, discard](QAbstractButton *b) {
        if (!self)
            return;
        bool save = b && (b == as_new || b == replace);
        if (how == 2 && (save || (b && b == discard)))
            self->close_after_question();   // the save goes on without the viewer
        if (!save)
            return;
        bool over = b == replace;
        QString dst = over ? src : imgedit::new_name(src, what);   // named now: one may have appeared meanwhile
        fileops::run_task(
            nullptr, "Saving " + basename(dst),
            [src, dst, edits]() -> QVariant {
                try {
                    imgedit::save_edit(src, dst, edits);
                    return QString();
                } catch (const Error &x) {
                    return x.message();
                }
            },
            [self, src, dst, over, title](const QVariant &res) {
                if (!res.toString().isEmpty()) {
                    warn(self, title, res.toString());
                    return;
                }
                if (over) {
                    undo::record("none", title, {});   // what was replaced is gone
                    if (self) {
                        self->after_write(src);
                        self->canvas->show_osd("Saved");
                    }
                    return;
                }
                undo::record_paths("create", title, {dst});
                if (!self)
                    return;
                int i = self->paths.indexOf(src);
                if (i < 0)
                    return;
                self->paths.insert(i + 1, dst);
                if (self->shown == src) {   // still on it (Ctrl+S): the new file is shown, the original as it was
                    self->steps.clear();
                    self->canvas->reset_view();
                    self->go_to(i + 1);
                } else {
                    if (self->index > i)
                        ++self->index;
                    self->filmstrip->set_items(self->paths, self->index);
                    self->update_overlay();
                }
                Q_EMIT self->saved(dst);
            },
            true);
    });
    ask(box);
}

void ImageViewer::straighten()
{
    QString path = shown;
    if (path.isEmpty())
        return;
    if (!imgedit::can_straighten(path)) {
        canvas->show_osd("Nothing to straighten: no orientation tag turns it");
        return;
    }
    try {
        imgedit::straighten(path);
    } catch (const Error &e) {
        warn(this, "Straighten", e.message());
        return;
    }
    after_write(path);
    canvas->show_osd("Straightened");
}

// ------------------------------------------------------------ view

ViewerCanvas *ImageViewer::active() const { return compare == 2 && showing_a ? pinned : canvas; }

void ImageViewer::fit() { active()->fit(); }

void ImageViewer::toggle_filmstrip()
{
    bool on = filmstrip->isHidden();
    filmstrip->setVisible(on);
    prefs::set("viewer_filmstrip", on);
}

void ImageViewer::toggle_info()
{
    bool on = info->isHidden();
    info->setVisible(on);
    prefs::set("viewer_info_panel", on);
    if (on && !shown.isEmpty()) {
        info->show_path(shown);
        info->set_histogram(cache.value(shown).pix);
    }
}

void ImageViewer::toggle_overlay()
{
    show_overlay = !show_overlay;
    update_overlay();
}

void ImageViewer::toggle_keep_zoom()
{
    keep_zoom = !keep_zoom;
    prefs::set("viewer_keep_zoom", keep_zoom);
    canvas->show_osd(keep_zoom ? "Zoom kept from image to image" : "Each image fits the window");
}

void ImageViewer::toggle_compare()
{
    if (compare) {
        compare = 0;
        pinned_path.clear();
        pinned->clear_image(QString());
        update_layout();
        return;
    }
    if (paths.size() < 2) {
        canvas->show_osd("Comparing needs two images");
        return;
    }
    if (!steps.isEmpty()) {
        canvas->show_osd("Save (Ctrl+S) or undo (Esc) the edits first");
        return;
    }
    leave_image();
    compare = 1;
    showing_a = false;
    pinned_path = current();
    pinned->reset_view();
    pinned->rotation = canvas->rotation;   // A as it was shown
    pinned->flip_h = canvas->flip_h;
    canvas->rotation = 0;
    canvas->flip_h = false;
    if (cache.contains(pinned_path))
        show_on(pinned, pinned_path);
    else
        request(pinned_path);
    update_layout();
    go_to(index + 1 < paths.size() ? index + 1 : index - 1);   // B: the next one
}

void ImageViewer::toggle_ab()
{
    if (!compare) {
        canvas->show_osd("Not comparing (X compares)");
        return;
    }
    compare = compare == 1 ? 2 : 1;
    showing_a = false;
    update_layout();
}

void ImageViewer::flip_ab()
{
    if (compare != 2)
        return;
    showing_a = !showing_a;
    update_layout();
    link_view(showing_a ? canvas : pinned, active());
}

void ImageViewer::toggle_fullscreen()
{
    if (isFullScreen()) {
        showNormal();
        for (ViewerCanvas *c : {canvas, pinned})
            c->setCursor(Qt::ArrowCursor);
    } else {
        showFullScreen();
        hide_cursor.start();
    }
    prefs::set("viewer_fullscreen", isFullScreen());   // the next viewer opens the same way
}

void ImageViewer::toggle_slideshow()
{
    if (slideshow.isActive())
        slideshow.stop();
    else
        slideshow.start(settings().value("slideshow_secs", 4).toInt() * 1000);
    update_overlay();
}

void ImageViewer::copy_image()
{
    auto it = cache.constFind(shown);
    if (it == cache.constEnd() || it->pix.isNull())
        return;
    if (it->is_full) {
        QGuiApplication::clipboard()->setPixmap(it->pix);
        return;
    }
    QImageReader reader(shown);   // the whole image, not the screen-size copy
    reader.setAutoTransform(true);
    QImage img = reader.read();
    if (!img.isNull())
        QGuiApplication::clipboard()->setImage(img);
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
    add({"F", "F11", "Return", "Enter"}, [this]() {
        if (canvas->cropping)
            finish_crop();
        else
            toggle_fullscreen();
    });
    add({"Escape"}, [this]() { escape(); });
    add({"Q", "Ctrl+W"}, [this]() { close(); });
    add({"+", "=", "Ctrl++"}, [this]() { active()->set_zoom(active()->scale() * 1.25); });
    add({"-", "Ctrl+-"}, [this]() { active()->set_zoom(active()->scale() / 1.25); });
    add({"0"}, [this]() { fit(); });
    add({"1"}, [this]() { active()->set_zoom(1.0); });
    add({"U"}, [this]() {
        for (ViewerCanvas *c : {canvas, pinned})
            c->upscale = !c->upscale;
        active()->fit();
    });
    add({"K"}, [this]() { toggle_keep_zoom(); });
    add({"R"}, [this]() { canvas->rotate(90); update_overlay(); });
    add({"L", "Shift+R"}, [this]() { canvas->rotate(-90); update_overlay(); });
    add({"H"}, [this]() { canvas->flip(); });
    add({"Ctrl+S"}, [this]() { save_changes(); });
    add({"Tab"}, [this]() { toggle_overlay(); });
    add({"I"}, [this]() { toggle_info(); });
    add({"T"}, [this]() { toggle_filmstrip(); });
    add({"X"}, [this]() { toggle_compare(); });
    add({"V"}, [this]() { toggle_ab(); });
    add({"B"}, [this]() { flip_ab(); });
    add({"S"}, [this]() { toggle_slideshow(); });
    add({"Delete"}, [this]() { delete_current(); });
    add({"Ctrl+Z"}, [this]() { undo(); });
    add({"Ctrl+C"}, [this]() { copy_image(); });
    add({"*"}, [this]() { toggle_star(); });
    add({"P"}, [this]() { toggle_pin(); });
    add({"G"}, [this]() {
        if (cb.on_reveal && !current().isEmpty())
            cb.on_reveal(current());
    });
    add({"O"}, [this]() {
        if (cb.on_open_with && !current().isEmpty())
            cb.on_open_with(current());
    });
    add({"M"}, [this]() { choose_transfer("move"); });
    add({"C"}, [this]() { choose_transfer("copy"); });
    for (int n = 1; n <= 9; ++n) {
        add({QString("Ctrl+%1").arg(n)}, [this, n]() { transfer_recent(n, "move"); });
        add({QString("Alt+%1").arg(n)}, [this, n]() { transfer_recent(n, "copy"); });
    }
    add({"Shift+C"}, [this]() { start_crop(); });
    add({"Shift+S"}, [this]() { resize_image(); });
    add({"Shift+O"}, [this]() { straighten(); });
    add({"Alt+Return", "Ctrl+I"}, [this]() {
        if (cb.on_properties)
            cb.on_properties(current());
    });
}

void ImageViewer::escape()
{
    if (canvas->cropping)
        canvas->stop_crop();
    else if (!steps.isEmpty())
        discard_edits();
    else if (slideshow.isActive())
        toggle_slideshow();
    else if (isFullScreen())
        toggle_fullscreen();
    else
        close();
}

void ImageViewer::wheelEvent(QWheelEvent *ev)
{
    int dy = ev->angleDelta().y();
    if (!dy)
        return;
    wheel_acc += dy;   // smooth touchpads send small deltas
    if (std::abs(wheel_acc) >= 120) {
        navigate(wheel_acc > 0 ? -1 : 1);
        wheel_acc = 0;
    }
}

void ImageViewer::mousePressEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::BackButton)
        navigate(-1);
    else if (ev->button() == Qt::ForwardButton)
        navigate(1);
    else if (ev->button() == Qt::MiddleButton)
        close();
}

void ImageViewer::contextMenuEvent(QContextMenuEvent *ev)
{
    if (QMenu *m = build_menu()) {
        m->exec(ev->globalPos());
        delete m;
    }
}

// Every entry has an id (menus.h), so the menu follows the layout chosen in Preferences → Menus
QMenu *ImageViewer::build_menu()
{
    QString path = current();
    if (path.isEmpty())
        return nullptr;
    auto *m = new QMenu(this);
    auto add = [this](QMenu *menu, const QString &id, const QString &text, std::function<void()> fn) {
        QAction *a = menu->addAction(text, this, fn);
        menus::tag(a, id);
        return a;
    };
    auto check = [&](QMenu *menu, const QString &id, const QString &text, bool on, std::function<void()> fn) {
        QAction *a = add(menu, id, text, fn);
        a->setCheckable(true);
        a->setChecked(on);
    };
    auto sub = [&](const QString &id, const QString &title) {
        QMenu *s = m->addMenu(title);
        menus::tag(s->menuAction(), id);
        return s;
    };
    add(m, "next", "Next\tRight", [this]() { navigate(1); });
    add(m, "previous", "Previous\tLeft", [this]() { navigate(-1); });
    m->addSeparator();
    QMenu *view = sub("viewer_view", "View");
    add(view, "fullscreen", isFullScreen() ? "Exit Fullscreen\tF" : "Fullscreen\tF", [this]() { toggle_fullscreen(); });
    add(view, "slideshow", slideshow.isActive() ? "Stop Slideshow\tS" : "Start Slideshow\tS", [this]() { toggle_slideshow(); });
    add(view, "fit", "Fit to Window\t0", [this]() { fit(); });
    add(view, "actual_size", "Actual Size\t1", [this]() { active()->set_zoom(1.0); });
    check(view, "keep_zoom", "Keep Zoom Between Images\tK", keep_zoom, [this]() { toggle_keep_zoom(); });
    view->addSeparator();
    check(view, "filmstrip", "Filmstrip\tT", !filmstrip->isHidden(), [this]() { toggle_filmstrip(); });
    check(view, "info_panel", "Info Panel\tI", !info->isHidden(), [this]() { toggle_info(); });
    check(view, "info_line", "Info Line\tTab", show_overlay, [this]() { toggle_overlay(); });
    view->addSeparator();
    check(view, "compare", "Compare With the Next Image\tX", compare, [this]() { toggle_compare(); });
    if (compare) {
        check(view, "compare_ab", "One at a Time (A/B)\tV", compare == 2, [this]() { toggle_ab(); });
        if (compare == 2)
            add(view, "compare_switch", showing_a ? "Show B\tB" : "Show A\tB", [this]() { flip_ab(); });
    }
    QMenu *edit = sub("viewer_edit", "Rotate and Edit");
    add(edit, "rotate_right", "Rotate Right\tR", [this]() { canvas->rotate(90); });
    add(edit, "rotate_left", "Rotate Left\tL", [this]() { canvas->rotate(-90); });
    add(edit, "flip", "Flip Horizontally\tH", [this]() { canvas->flip(); });
    add(edit, "save_changes", steps.isEmpty() ? "Save Rotation\tCtrl+S" : "Save Changes…\tCtrl+S", [this]() { save_changes(); });
    if (!steps.isEmpty())
        add(edit, "undo_edits", "Undo the Edits\tEsc", [this]() { discard_edits(); });
    edit->addSeparator();
    add(edit, "crop", "Crop…\tShift+C", [this]() { start_crop(); });
    add(edit, "resize", "Resize…\tShift+S", [this]() { resize_image(); });
    add(edit, "straighten", "Straighten (Apply the Orientation Tag)\tShift+O", [this]() { straighten(); });
    m->addSeparator();
    add(m, "star", places::is_starred(path) ? "Unstar\t*" : "Star\t*", [this]() { toggle_star(); });
    add(m, "pin", places::is_pinned(path) ? "Unpin\tP" : "Pin in Its Folder\tP", [this]() { toggle_pin(); });
    QStringList recent = recent_folders();
    for (const QString &op : {QString("copy"), QString("move")}) {
        QMenu *to = sub(op == "copy" ? "viewer_copy_to" : "viewer_move_to", op == "copy" ? "Copy To" : "Move To");
        for (int i = 0; i < recent.size(); ++i) {
            QString f = recent[i];
            to->addAction(QString("%1\t%2+%3").arg(basename(f), op == "copy" ? "Alt" : "Ctrl", QString::number(i + 1)), this,
                          [this, f, op]() { transfer(f, op); })
                ->setToolTip(f);
        }
        if (!recent.isEmpty())
            to->addSeparator();
        to->addAction(QString("Other Folder…\t%1").arg(op == "copy" ? "C" : "M"), this, [this, op]() { choose_transfer(op); });
    }
    if (cb.on_reveal)
        add(m, "show_in_folder", "Show in Folder\tG", [this, path]() { cb.on_reveal(path); });
    m->addSeparator();
    add(m, "open_default", "Open With Default Application", [path]() { open_default(path); });
    if (cb.on_open_with)
        add(m, "open_with", "Open With…\tO", [this, path]() { cb.on_open_with(path); });
    add(m, "copy_image", "Copy Image\tCtrl+C", [this]() { copy_image(); });
    add(m, "copy_full_path", "Copy Path", [path]() { QGuiApplication::clipboard()->setText(path); });
    add(m, "wallpaper", uwp::wallpaper_label(), [path]() { set_wallpaper(path); });
    if (uwp::editor_open())
        add(m, "uwp_add", "Add to Selected UWP Monitor", [path]() { uwp::add_to_selected(path); });
    m->addSeparator();
    if (cb.on_properties)
        add(m, "properties", "Properties\tAlt+Return", [this, path]() { cb.on_properties(path); });
    if (cb.on_delete)
        add(m, "trash", "Move to Trash\tDelete", [this]() { delete_current(); });
    if (cb.on_undo)
        add(m, "undo", "Undo\tCtrl+Z", [this]() { undo(); });
    menus::arrange(m, menus::VIEWER);
    return m;
}

void ImageViewer::closeEvent(QCloseEvent *ev)
{
    if (!closing && !shown.isEmpty() && !steps.isEmpty()) {
        ev->ignore();   // asked first; the answer closes it
        ask_edit(shown, all_steps(), 2);
        return;
    }
    if (!closing && !shown.isEmpty() && (canvas->rotation || canvas->flip_h)) {
        if (settings().value("viewer_save_rotation", "ask").toString() == "ask" && lexists(shown) &&
            !imgedit::rotation_method(shown).isEmpty()) {
            ev->ignore();   // asked first; the answer closes it
            ask_rotation(shown, canvas->rotation, canvas->flip_h, true);
            return;
        }
        leave_image();
    }
    if (!isFullScreen())
        settings().setValue("viewer_geometry", saveGeometry());
    slideshow.stop();
    stop_movie();
    pool.clear();
    if (cb.on_close && !paths.isEmpty())
        cb.on_close(current());
    QWidget::closeEvent(ev);
}
