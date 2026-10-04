#include "widgets.h"

#include "animate.h"
#include "atc.h"
#include "dialogs.h"
#include "metadata.h"
#include "overview.h"
#include "places.h"
#include "proc.h"
#include "thumbs.h"
#include "util.h"

#include <QApplication>
#include <QClipboard>
#include <QCompleter>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QVBoxLayout>

#include <cmath>
#include <fnmatch.h>
#include <gio/gio.h>

using namespace util;

// ---------------------------------------------------------------- models

QString FastIconProvider::type(const QFileInfo &fi) const
{
    if (fi.isDir())
        return "Folder";
    QString suf = fi.suffix().toLower();
    auto it = types.constFind(suf);
    if (it != types.constEnd())
        return *it;
    QString t = mime_for(fi.fileName(), 0).comment();
    if (t.isEmpty())
        t = "File";
    types.insert(suf, t);
    return t;
}

FSModel::FSModel(ThumbnailManager *thumbs, QObject *parent) : QFileSystemModel(parent), thumbs(thumbs)
{
    setReadOnly(false);
    icon_provider = new FastIconProvider;   // the model doesn't take ownership
    setIconProvider(icon_provider);
    connect(thumbs, &ThumbnailManager::updated, this, &FSModel::thumb_ready);
}

FSModel::~FSModel()
{
    setIconProvider(nullptr);
    delete icon_provider;
}

QPixmap FSModel::thumb(const QModelIndex &index) const
{
    QFileInfo fi = fileInfo(index);
    QString path = fi.absoluteFilePath();
    qint64 mtime = fi.lastModified().toSecsSinceEpoch();
    if (fi.isDir())
        return thumbs->folder_pixmap(path, mtime, thumb_size, folder_previews);
    if (!thumbs::can_thumbnail(path))
        return QPixmap();
    return thumbs->get(path, mtime, false, thumb_size, fi.size());
}

QIcon FSModel::plain_icon(const QModelIndex &index) const
{
    QFileInfo fi = fileInfo(index);
    if (fi.isDir())
        return icon_for_path(fi.absoluteFilePath(), 1);
    QString suf = fi.suffix().toLower();
    auto it = suffix_icons.constFind(suf);
    if (it != suffix_icons.constEnd())
        return *it;
    QIcon icon = icon_for_path(fi.absoluteFilePath(), 0);
    suffix_icons.insert(suf, icon);
    return icon;
}

QVariant FSModel::data(const QModelIndex &index, int role) const
{
    if (index.column() == 0) {
        if (role == ThumbRole) {
            QPixmap pm = thumb(index);
            return pm.isNull() ? QVariant() : QVariant(pm);
        }
        if (role == Qt::DecorationRole) {
            QPixmap pm = thumb(index);
            return pm.isNull() ? QVariant(plain_icon(index)) : QVariant(QIcon(pm));
        }
        if (role == PathRole)
            return filePath(index);
        if (role == KindRole) {
            QFileInfo fi = fileInfo(index);
            return (fi.isDir() ? KIND_DIR : 0) | (fi.isSymLink() ? KIND_LINK : 0);
        }
    }
    return QFileSystemModel::data(index, role);
}

void FSModel::thumb_ready(const QString &path)
{
    if (dirname(path) != rootPath())
        return;
    QModelIndex idx = index(path);
    if (idx.isValid())
        Q_EMIT dataChanged(idx, idx, {Qt::DecorationRole, ThumbRole});
}

bool FSModel::dropMimeData(const QMimeData *data, Qt::DropAction, int, int, const QModelIndex &parent)
{
    if (!data->hasUrls() || !drop_handler)
        return false;
    QString target = parent.isValid() ? filePath(parent) : rootPath();
    if (!isdir(target))
        target = dirname(target);
    QStringList paths;
    for (const QUrl &u : data->urls())
        if (u.isLocalFile())
            paths << u.toLocalFile();
    auto handler = drop_handler;
    QTimer::singleShot(0, this, [handler, paths, target]() { handler(paths, target); });
    return true;
}

SearchModel::SearchModel(ThumbnailManager *thumbs, QObject *parent) : QStandardItemModel(parent), thumbs(thumbs)
{
    setHorizontalHeaderLabels({"Name", "Location", "Size", "Modified"});
    connect(thumbs, &ThumbnailManager::updated, this, &SearchModel::thumb_ready);
}

void SearchModel::add_paths(const QStringList &paths, const QHash<QString, QString> &locations)
{
    for (const QString &p : paths) {
        struct stat st;
        if (!stat_(p, st))
            continue;
        bool is_dir = S_ISDIR(st.st_mode);
        auto *name = new QStandardItem(basename(p));
        name->setData(p, PathRole);
        name->setData(QVariantList{is_dir, qint64(st.st_mtime), qint64(st.st_size)}, StatRole);
        name->setData((is_dir ? KIND_DIR : 0) | (islink(p) ? KIND_LINK : 0), KindRole);
        name->setToolTip(p);
        name->setEditable(false);
        auto *loc = new QStandardItem(locations.isEmpty() ? dirname(p) : locations.value(p, dirname(p)));
        auto *size = new QStandardItem(is_dir ? QString() : human_size(qint64(st.st_size)));
        size->setData(qint64(st.st_size), Qt::UserRole);
        auto *mod = new QStandardItem(fmt_time(st.st_mtime, "%Y-%m-%d %H:%M"));
        for (QStandardItem *it : {loc, size, mod})
            it->setEditable(false);
        appendRow({name, loc, size, mod});
        rows.insert(p, name);
    }
}

void SearchModel::clear_results()
{
    removeRows(0, rowCount());
    rows.clear();
}

QVariant SearchModel::data(const QModelIndex &index, int role) const
{
    if (index.column() == 0 && (role == ThumbRole || role == Qt::DecorationRole)) {
        QStandardItem *item = itemFromIndex(index);
        QString path = item->data(PathRole).toString();
        QVariantList st = item->data(StatRole).toList();
        bool is_dir = st.value(0).toBool();
        QPixmap pm;
        if (is_dir)
            pm = thumbs->folder_pixmap(path, st.value(1).toLongLong(), thumb_size, folder_previews);
        else if (thumbs::can_thumbnail(path))
            pm = thumbs->get(path, st.value(1).toLongLong(), false, thumb_size, st.value(2).toLongLong());
        if (role == ThumbRole)
            return pm.isNull() ? QVariant() : QVariant(pm);
        return pm.isNull() ? QVariant(icon_for_path(path, is_dir)) : QVariant(QIcon(pm));
    }
    return QStandardItemModel::data(index, role);
}

void SearchModel::thumb_ready(const QString &path)
{
    QStandardItem *item = rows.value(path);
    if (item) {
        QModelIndex idx = item->index();
        Q_EMIT dataChanged(idx, idx, {Qt::DecorationRole, ThumbRole});
    }
}

SearchThread::SearchThread(const QString &root, const QString &query, bool hidden, QObject *parent, bool contents)
    : QThread(parent), root(root), query(query.toLower()), raw_query(query), hidden(hidden), contents(contents)
{
    wildcard = query.contains('*') || query.contains('?') || query.contains('[');
}

bool SearchThread::match(const QString &name) const
{
    if (wildcard)
        return ::fnmatch(query.toUtf8().constData(), name.toLower().toUtf8().constData(), 0) == 0;
    return name.toLower().contains(query);
}

bool can_search_contents() { return which("localsearch"); }

void SearchThread::run_contents()
{
    // search file contents (and names) with the desktop's search index (localsearch), within root
    static const QRegularExpression sep("[\\s*?\\[\\]]+");
    QStringList terms = raw_query.split(sep, Qt::SkipEmptyParts);
    if (terms.isEmpty())
        return;
    auto r = proc::run(QStringList{"localsearch", "search", "-f", "--limit", "20000"} + terms, 60000);
    QString prefix = rstrip(root, '/') + "/";
    QStringList batch;
    for (const QString &line : QString::fromUtf8(r.out).split('\n')) {
        if (stop)
            return;
        QString p = uri_to_path(line.trimmed());
        if (p.isEmpty() || !p.startsWith(prefix) || !lexists(p))
            continue;
        if (!hidden) {
            bool dot = false;
            for (const QString &part : p.mid(prefix.size()).split('/'))
                dot = dot || part.startsWith('.');
            if (dot)
                continue;
        }
        batch << p;
        if (batch.size() >= 50) {
            Q_EMIT found(batch);
            batch.clear();
        }
    }
    if (!batch.isEmpty())
        Q_EMIT found(batch);
}

void SearchThread::run()
{
    if (contents) {
        run_contents();
        return;
    }
    QStringList batch;
    int count = 0;
    walk(root, [&](const QString &dir, QStringList &dirs, QStringList &files) {
        if (stop)
            return false;
        if (!hidden) {
            QStringList d, f;
            for (const QString &n : dirs)
                if (!n.startsWith('.'))
                    d << n;
            for (const QString &n : files)
                if (!n.startsWith('.'))
                    f << n;
            dirs = d;
            files = f;
        }
        natural_sort(dirs);
        QStringList sorted_files = files;
        natural_sort(sorted_files);
        for (const QString &n : dirs + sorted_files) {
            if (match(n)) {
                batch << join(dir, n);
                count += 1;
            }
        }
        if (batch.size() >= 50) {
            Q_EMIT found(batch);
            batch.clear();
        }
        return count < 10000;
    });
    if (stop)
        return;
    if (!batch.isEmpty())
        Q_EMIT found(batch);
}

// ---------------------------------------------------------------- grid delegate

GridDelegate::GridDelegate(QWidget *pane, ThumbnailManager *thumbs, std::function<bool(const QString &)> is_cut,
                           QObject *parent)
    : QStyledItemDelegate(parent), pane(pane), thumbs(thumbs), is_cut(std::move(is_cut))
{
}

QSize GridDelegate::cell_size() const
{
    QFontMetrics fm = pane->fontMetrics();
    int w = std::max(icon_size + 16, 96);
    return QSize(w, icon_size + 14 + fm.height() * 2 + 6);
}

QSize GridDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const { return cell_size(); }

QStringList GridDelegate::lines(const QFontMetrics &fm, const QString &text, int width) const
{
    if (fm.horizontalAdvance(text) <= width)
        return {text};
    // break the first line at a natural boundary if possible
    int cut = text.size();
    while (cut > 1 && fm.horizontalAdvance(text.left(cut)) > width)
        --cut;
    int brk = -1;
    for (QChar c : {QChar(' '), QChar('_'), QChar('-'), QChar('.')})
        brk = std::max<int>(brk, text.left(cut).lastIndexOf(c));
    if (brk > cut / 2)
        cut = brk + 1;
    return {text.left(cut), fm.elidedText(text.mid(cut), Qt::ElideMiddle, width)};
}

void GridDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    QRect r = option.rect.adjusted(3, 3, -3, -3);
    const QPalette &pal = option.palette;
    bool selected = option.state & QStyle::State_Selected;
    bool hover = option.state & QStyle::State_MouseOver;
    if (selected || hover) {
        QColor c = pal.color(QPalette::Highlight);
        c.setAlpha(selected ? 110 : 40);
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawRoundedRect(QRectF(r), 8, 8);
    }
    QString path = index.data(PathRole).toString();
    QString name = index.data(Qt::DisplayRole).toString();
    if (name.isEmpty())
        name = basename(path);
    int kind = index.data(KindRole).toInt();
    bool is_dir = kind & KIND_DIR, is_link = kind & KIND_LINK;
    bool dim = name.startsWith('.') || (is_cut && is_cut(path));
    if (dim)
        p->setOpacity(0.5);
    int s = icon_size;
    QRect icon_rect(r.x() + (r.width() - s) / 2, r.y() + 6, s, s);
    QVariant thumb = index.data(ThumbRole);
    QPixmap anim = animator && !is_dir ? animator->frame(path, index, s) : QPixmap();   // GIF / WebM playing
    QRectF badge_box(icon_rect);   // where the star goes: the corner of what's drawn
    if (!anim.isNull() || thumb.isValid()) {
        QPixmap spm = !anim.isNull() ? anim : thumbs->scaled(thumb.value<QPixmap>(), s);
        double dpr = spm.devicePixelRatio();
        double w = spm.width() / dpr, h = spm.height() / dpr;
        QRectF target(icon_rect.x() + (s - w) / 2, icon_rect.y() + (s - h), w, h);
        badge_box = target;
        if (!is_dir) {
            QPainterPath clip;
            clip.addRoundedRect(target, 4, 4);
            p->save();
            p->setClipPath(clip);
            p->drawPixmap(target, spm, QRectF(spm.rect()));
            p->restore();
            p->setPen(QColor(0, 0, 0, 50));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(target, 4, 4);
            if (is_video(path) && anim.isNull())
                play_badge(p, target);
        } else {
            p->drawPixmap(target, spm, QRectF(spm.rect()));
        }
    } else {
        QVariant icon = index.data(Qt::DecorationRole);
        if (icon.canConvert<QIcon>())
            icon.value<QIcon>().paint(p, icon_rect);
    }
    if (is_link) {
        QIcon em = theme_icon({"emblem-symbolic-link", "emblem-link"});
        int es = std::max(16, s / 5);
        em.paint(p, QRect(icon_rect.right() - es, icon_rect.bottom() - es, es, es));
    }
    if (places::is_starred(path)) {
        double bs = std::max(16, s / 6);
        star_badge(p, QRectF(badge_box.right() - bs * 0.75, badge_box.top() - bs * 0.25, bs, bs));
    }
    // text
    p->setPen(pal.color(QPalette::Text));
    if (selected) {
        QFont f = option.font;
        f.setWeight(QFont::DemiBold);
        p->setFont(f);
    } else {
        p->setFont(option.font);
    }
    QFontMetrics fm = p->fontMetrics();
    QRect text_rect(r.x() + 4, icon_rect.bottom() + 6, r.width() - 8, fm.height() * 2 + 2);
    QStringList ls = lines(fm, name, text_rect.width());
    for (int i = 0; i < ls.size(); ++i) {
        QRect lr(text_rect.x(), text_rect.y() + i * fm.height(), text_rect.width(), fm.height());
        p->drawText(lr, Qt::AlignHCenter | Qt::AlignVCenter, ls[i]);
    }
    p->restore();
}

void GridDelegate::star_badge(QPainter *p, const QRectF &rect)
{
    // a small gold star (starred items)
    QPointF c = rect.center();
    double r = rect.width() / 2;
    QPainterPath star;
    for (int i = 0; i < 10; ++i) {
        double radius = i % 2 == 0 ? r : r * 0.45;
        double a = M_PI / 2 + i * M_PI / 5;
        QPointF pt(c.x() + radius * std::cos(a), c.y() - radius * std::sin(a));
        if (i == 0)
            star.moveTo(pt);
        else
            star.lineTo(pt);
    }
    star.closeSubpath();
    p->save();
    p->setOpacity(1.0);
    p->setPen(QColor(120, 80, 0, 220));
    p->setBrush(QColor("#f6c02d"));
    p->drawPath(star);
    p->restore();
}

void GridDelegate::play_badge(QPainter *p, const QRectF &rect)
{
    double d = std::max(14.0, std::min(rect.width(), rect.height()) * 0.28);
    QPointF c = rect.center();
    p->setPen(Qt::NoPen);
    p->setBrush(QColor(0, 0, 0, 140));
    p->drawEllipse(QRectF(c.x() - d / 2, c.y() - d / 2, d, d));
    QPainterPath tri;
    tri.moveTo(c.x() - d * 0.15, c.y() - d * 0.22);
    tri.lineTo(c.x() + d * 0.25, c.y());
    tri.lineTo(c.x() - d * 0.15, c.y() + d * 0.22);
    tri.closeSubpath();
    p->setBrush(QColor(255, 255, 255, 230));
    p->drawPath(tri);
}

// ---------------------------------------------------------------- path bar

PathBar::PathBar(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);
    crumb_box = new Crumbs;
    // Ignored: the crumbs' width must not depend on which buttons are visible, otherwise hiding a button resizes
    // the bar, which re-runs fit -> jitter.
    crumb_box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMinimumWidth(200);
    crumb_lay = new QHBoxLayout(crumb_box);
    crumb_lay->setSizeConstraint(QLayout::SetNoConstraint);
    crumb_lay->setContentsMargins(0, 0, 0, 0);
    crumb_lay->setSpacing(0);
    connect(crumb_box, &Crumbs::clicked, this, &PathBar::start_edit);
    edit = new QLineEdit;
    edit->setClearButtonEnabled(true);
    edit->hide();
    auto *comp_model = new QFileSystemModel(this);
    comp_model->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot);
    comp_model->setRootPath("/");
    auto *comp = new QCompleter(comp_model, this);
    comp->setCaseSensitivity(Qt::CaseInsensitive);
    edit->setCompleter(comp);
    connect(edit, &QLineEdit::returnPressed, this, &PathBar::commit);
    edit->installEventFilter(this);
    edit_btn = new QToolButton;
    edit_btn->setIcon(theme_icon({"document-edit-symbolic", "edit-symbolic", "document-edit"}));
    edit_btn->setToolTip("Type a location (Ctrl+L)");
    edit_btn->setAutoRaise(true);
    connect(edit_btn, &QToolButton::clicked, this, [this]() {
        if (edit->isVisible())
            cancel_edit();
        else
            start_edit();
    });
    lay->addWidget(crumb_box, 1);
    lay->addWidget(edit, 1);
    lay->addWidget(edit_btn);
    overflow = new QToolButton;
    overflow->setText("…");
    overflow->setAutoRaise(true);
    overflow->setPopupMode(QToolButton::InstantPopup);
}

bool PathBar::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == edit && ev->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(ev)->key() == Qt::Key_Escape) {
        cancel_edit();
        return true;
    }
    // a popup (the right-click menu, the completer) takes focus while still editing
    if (obj == edit && ev->type() == QEvent::FocusOut && static_cast<QFocusEvent *>(ev)->reason() != Qt::PopupFocusReason
        && !edit->completer()->popup()->isVisible())
        QTimer::singleShot(0, this, &PathBar::cancel_edit);
    return false;
}

void PathBar::start_edit()
{
    crumb_box->hide();
    edit->setText(path.contains("://") ? QString() : path);
    edit->show();
    edit->setFocus();
    edit->selectAll();
}

void PathBar::cancel_edit()
{
    edit->hide();
    crumb_box->show();
}

void PathBar::commit()
{
    QString t = expanduser(edit->text().trimmed());
    if (t.startsWith("file://"))
        t = uri_to_path(t);
    cancel_edit();
    if (!t.isEmpty())
        Q_EMIT navigate(t);
}

void PathBar::set_path(const QString &p)
{
    path = p;
    while (QLayoutItem *it = crumb_lay->takeAt(0)) {
        QWidget *w = it->widget();
        if (w && w != overflow)
            w->deleteLater();
        delete it;
    }
    buttons.clear();
    struct Part {
        QString label, path, icon;
    };
    QList<Part> parts;
    QString rest, base;
    if (p == OVERVIEW) {
        parts << Part{OVERVIEW_TITLE, OVERVIEW, "computer"};
    } else if (places::is_virtual(p)) {
        parts << Part{places::title(p), p, places::icon_name(p)};
    } else if (p == HOME() || p.startsWith(HOME() + "/")) {
        parts << Part{"Home", HOME(), "user-home"};
        rest = strip(p.mid(HOME().size()), "/");
        base = HOME();
    } else {
        parts << Part{"/", "/", "drive-harddisk"};
        rest = strip(p, "/");
    }
    for (const QString &seg : rest.split('/', Qt::SkipEmptyParts)) {
        base = base + "/" + seg;
        parts << Part{seg, base, QString()};
    }
    crumb_lay->addWidget(overflow);
    for (const Part &part : parts) {
        auto *b = new QToolButton;
        b->setText(part.label);
        b->setAutoRaise(true);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        if (!part.icon.isEmpty())
            b->setIcon(theme_icon(part.icon));
        if (part.path == p) {
            QFont f = b->font();
            f.setBold(true);
            b->setFont(f);
        }
        QString target = part.path;
        connect(b, &QToolButton::clicked, this, [this, target]() { Q_EMIT navigate(target); });
        b->setProperty("crumb_path", target);
        crumb_lay->addWidget(b);
        buttons << b;
    }
    crumb_lay->addStretch(1);
    QTimer::singleShot(0, this, [this]() { fit(true); });
}

void PathBar::resizeEvent(QResizeEvent *ev)
{
    QWidget::resizeEvent(ev);
    fit();
}

void PathBar::fit(bool force)
{
    int avail = crumb_box->width() - 30;
    if (fitting || (avail == fit_width && !force))
        return;
    fitting = true;
    fit_width = avail;
    // sizeHint() is valid for hidden widgets, so decide first, then apply once
    QList<int> widths;
    int total = 0;
    for (QToolButton *b : buttons) {
        widths << b->sizeHint().width();
        total += widths.last();
    }
    int n_hide = 0;
    while (n_hide < buttons.size() - 1 && total > avail) {
        total -= widths[n_hide];
        ++n_hide;
    }
    for (int i = 0; i < buttons.size(); ++i)
        if (buttons[i]->isHidden() != (i < n_hide))
            buttons[i]->setVisible(i >= n_hide);
    overflow->setVisible(n_hide > 0);
    if (n_hide) {
        auto *m = new QMenu(overflow);
        for (int i = 0; i < n_hide; ++i) {
            QString target = buttons[i]->property("crumb_path").toString();
            m->addAction(buttons[i]->text(), this, [this, target]() { Q_EMIT navigate(target); });
        }
        if (QMenu *old = overflow->menu())
            old->deleteLater();
        overflow->setMenu(m);
    }
    fitting = false;
}

// ---------------------------------------------------------------- sidebar

Sidebar::Sidebar(QWidget *parent) : QListWidget(parent)
{
    // phones and cameras come and go through GIO's volume monitor (QStorageInfo doesn't see them)
    monitor = g_volume_monitor_get();
    for (const char *sig : {"volume-added", "volume-removed", "volume-changed", "mount-added", "mount-removed",
                            "mount-changed"})
        g_signal_connect(monitor, sig, G_CALLBACK(&Sidebar::monitor_changed), this);
    phone_timer.setSingleShot(true);
    phone_timer.setInterval(300);
    connect(&phone_timer, &QTimer::timeout, this, &Sidebar::refresh);
    setIconSize(QSize(18, 18));
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTextElideMode(Qt::ElideRight);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DropOnly);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, &Sidebar::menu);
    connect(this, &QListWidget::itemClicked, this, &Sidebar::clicked);
    setStyleSheet("QListWidget { background: palette(window); } QListWidget::item { padding: 3px; }");
    refresh();
    timer.setInterval(4000);
    connect(&timer, &QTimer::timeout, this, &Sidebar::check_mounts);
    timer.start();
}

Sidebar::~Sidebar()
{
    g_signal_handlers_disconnect_by_data(monitor, this);
    g_object_unref(monitor);
}

void Sidebar::monitor_changed(void *, void *, void *self) { static_cast<Sidebar *>(self)->phone_timer.start(); }

// ---------------------------------------------------------------- sidebar order (shared with the Python version)

namespace sidebar {

const QStringList SECTIONS = {"places", "bookmarks", "devices"};

QStringList section_order(const QStringList &saved)
{
    QStringList out;
    for (const QString &id : saved)
        if (SECTIONS.contains(id) && !out.contains(id))
            out << id;
    for (const QString &id : SECTIONS)
        if (!out.contains(id))
            out << id;
    return out;
}

QStringList ordered(const QStringList &keys, const QStringList &saved)
{
    QStringList out;
    for (const QString &k : saved)
        if (keys.contains(k) && !out.contains(k))
            out << k;
    for (const QString &k : keys)
        if (!out.contains(k))
            out << k;
    return out;
}

QStringList moved(QStringList keys, const QString &key, int index)
{
    int from = keys.indexOf(key);
    if (from < 0)
        return keys;
    int to = std::clamp(index, 0, int(keys.size()));
    if (to > from)
        to -= 1;
    keys.move(from, to);
    return keys;
}

}  // namespace sidebar

static const int SectionRole = Qt::UserRole + 1;
static const int KeyRole = Qt::UserRole + 2;
static const char *SIDEBAR_MIME = "application/x-kestrel-sidebar";

void Sidebar::header(const QString &section, const QString &title, bool collapsed)
{
    auto *it = new QListWidgetItem(QString(collapsed ? "▸  " : "▾  ") + title);
    it->setFlags(Qt::ItemIsEnabled);   // clickable (collapse), not selectable
    QFont f = it->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 0.85);
    it->setFont(f);
    it->setForeground(palette().color(QPalette::PlaceholderText));
    it->setData(Qt::UserRole, QVariantList{"header", section});
    it->setData(SectionRole, section);
    it->setToolTip(collapsed ? "Click to expand · drag to move the section" : "Click to collapse · drag to move the section");
    addItem(it);
}

void Sidebar::add(const Entry &e, const QString &section)
{
    auto *it = new QListWidgetItem(e.icon, e.label);
    it->setData(PathRole, e.path);
    it->setData(Qt::UserRole, QVariantList{e.kind, e.extra});
    it->setData(SectionRole, section);
    it->setData(KeyRole, e.key.isEmpty() ? e.path : e.key);
    it->setToolTip(e.tip.isEmpty() ? e.path : e.tip);
    addItem(it);
}

QList<Sidebar::Entry> Sidebar::in_order(const QList<Entry> &entries, const QString &setting) const
{
    QStringList keys;
    for (const Entry &e : entries)
        keys << (e.key.isEmpty() ? e.path : e.key);
    QList<Entry> out;
    for (const QString &k : sidebar::ordered(keys, settings().value(setting).toStringList()))
        out << entries[keys.indexOf(k)];
    return out;
}

QList<Sidebar::Mount> Sidebar::mount_list() const
{
    QList<Mount> out;
    for (const QStorageInfo &v : QStorageInfo::mountedVolumes()) {
        QString dev = QString::fromUtf8(v.device());
        QString root = v.rootPath();
        if (!v.isValid() || !v.isReady() || root == "/")
            continue;
        bool sys_root = false;
        for (const char *pre : {"/boot", "/snap", "/var/snap", "/run/", "/sys"})
            sys_root = sys_root || root.startsWith(pre);
        if (!dev.startsWith("/dev/") || sys_root) {
            if (!(root.startsWith("/media/") || root.startsWith("/run/media/") || root.startsWith("/mnt")))
                continue;
        }
        if (dev.startsWith("/dev/loop"))
            continue;
        QString name = v.displayName();
        if (name.isEmpty())
            name = basename(root);
        out << Mount{name, root, dev, v.bytesTotal()};
    }
    std::sort(out.begin(), out.end(), [](const Mount &a, const Mount &b) { return a.root < b.root; });
    return out;
}

void Sidebar::check_mounts()
{
    if (!(mount_list() == mounts))
        refresh();
}

void Sidebar::refresh()
{
    QString current = currentItem() ? currentItem()->data(PathRole).toString() : QString();
    clear();
    QList<Entry> place_list;
    place_list << Entry{OVERVIEW_TITLE, OVERVIEW, theme_icon({"computer", "folder"}), "overview"};
    place_list << Entry{"Home", HOME(), theme_icon({"user-home", "folder"})};
    place_list << Entry{"Recent", places::RECENT, theme_icon({"document-open-recent", "folder-recent", "folder"}), "recent"};
    place_list << Entry{"Starred", places::STARRED, theme_icon({"starred", "starred-symbolic", "folder"}), "starred"};
    const QList<QStringList> xdg = {{"DESKTOP", "Desktop", "user-desktop"},
                                    {"DOCUMENTS", "Documents", "folder-documents"},
                                    {"DOWNLOAD", "Downloads", "folder-download"},
                                    {"MUSIC", "Music", "folder-music"},
                                    {"PICTURES", "Pictures", "folder-pictures"},
                                    {"VIDEOS", "Videos", "folder-videos"}};
    for (const QStringList &pl : xdg) {
        QString p = xdg_user_dir(pl[0]);
        if (isdir(p))
            place_list << Entry{pl[1], p, theme_icon({pl[2], "folder"})};
    }
    bool empty = trash_is_empty();   // home trash and every drive's trash
    place_list << Entry{"Trash", join(TRASH_DIR(), "files"),
                        theme_icon({empty ? "user-trash" : "user-trash-full", "folder"}), "trash"};
    QList<Entry> bookmark_list;   // in the bookmarks file's order (moving one rewrites the file)
    auto bms = read_bookmarks();
    for (int i = 0; i < bms.size(); ++i) {
        const auto &[path, label] = bms[i];
        bool remote = !path.startsWith('/') || path.startsWith("/run/user/");
        bookmark_list << Entry{label, path, theme_icon({remote ? "folder-remote" : "folder", "folder"}), "bookmark", i};
    }
    QList<Entry> device_list;
    device_list << Entry{"Computer", "/", theme_icon({"drive-harddisk", "folder"}), "root"};
    mounts = mount_list();
    for (const Mount &m : mounts) {
        QString label = m.total ? QString("%1 (%2)").arg(m.name, human_size(m.total)) : m.name;
        bool removable = m.root.startsWith("/media/") || m.root.startsWith("/run/media/");
        device_list << Entry{label, m.root, theme_icon({removable ? "drive-removable-media" : "drive-harddisk", "folder"}),
                             "mount", m.dev};
    }
    phones = overview::phone_infos(monitor);
    for (int i = 0; i < phones.size(); ++i) {
        const DriveInfo &ph = phones[i];
        // a phone that isn't mounted yet opens its URI: open_location mounts it
        QIcon icon = ph.icon.isNull() ? theme_icon({"phone", "drive-removable-media"}) : ph.icon;
        device_list << Entry{ph.name, ph.mounted ? ph.root : ph.uri, icon, "phone", i, "phone:" + ph.name,
                             ph.mounted ? ph.fs : ph.fs + " — click to connect"};
    }
    QStringList collapsed = settings().value("sidebar_collapsed").toStringList();
    for (const QString &id : sidebar::section_order(settings().value("sidebar_sections").toStringList())) {
        QList<Entry> entries = id == "places"      ? in_order(place_list, "sidebar_places_order")
                               : id == "devices" ? in_order(device_list, "sidebar_devices_order")
                                                 : bookmark_list;
        if (entries.isEmpty())
            continue;
        bool shut = collapsed.contains(id);
        header(id, id == "places" ? "PLACES" : id == "devices" ? "DEVICES" : "BOOKMARKS", shut);
        if (!shut)
            for (const Entry &e : entries)
                add(e, id);
    }
    if (!current.isEmpty())
        select_path(current);
}

QStringList Sidebar::shown_sections() const
{
    QStringList out;
    for (int i = 0; i < count(); ++i)
        if (item(i)->data(Qt::UserRole).toList().value(0) == "header")
            out << item(i)->data(SectionRole).toString();
    return out;
}

QStringList Sidebar::entry_keys(const QString &section) const
{
    QStringList out;
    for (int i = 0; i < count(); ++i)
        if (item(i)->data(SectionRole) == section && item(i)->data(Qt::UserRole).toList().value(0) != "header")
            out << item(i)->data(KeyRole).toString();
    return out;
}

int Sidebar::section_row(const QString &section) const
{
    for (int i = 0; i < count(); ++i)
        if (item(i)->data(SectionRole) == section)
            return i;
    return -1;
}

int Sidebar::section_end(const QString &section) const
{
    int end = section_row(section);
    if (end < 0)
        return -1;
    while (end < count() && item(end)->data(SectionRole) == section)
        ++end;
    return end;
}

void Sidebar::save_order(const QString &setting, const QStringList &keys)
{
    // keep the places of entries that aren't shown now (a drive that isn't plugged in)
    QStringList all = keys;
    for (const QString &k : settings().value(setting).toStringList())
        if (!all.contains(k))
            all << k;
    settings().setValue(setting, all);
    settings().sync();   // other Kestrels read the file as soon as they hear the report
    atc::announce("sidebar");   // refreshes every sidebar, here and in other Kestrels
}

void Sidebar::move_entry(const QString &section, const QString &key, int index)
{
    QStringList keys = entry_keys(section);
    if (!keys.contains(key))
        return;
    if (section == "bookmarks") {
        auto bms = read_bookmarks();
        int from = -1;
        for (int i = 0; i < bms.size() && from < 0; ++i)
            if (bms[i].first == key)
                from = i;
        if (from < 0)
            return;
        int to = std::clamp(index, 0, int(bms.size()));
        if (to > from)
            to -= 1;
        if (to == from)
            return;
        bms.move(from, to);
        write_bookmarks(bms);   // announces "bookmarks": every sidebar refreshes
        return;
    }
    QStringList now = sidebar::moved(keys, key, index);
    if (now != keys)
        save_order(section == "places" ? "sidebar_places_order" : "sidebar_devices_order", now);
}

void Sidebar::move_section(const QString &section, int index)
{
    QStringList order = sidebar::section_order(settings().value("sidebar_sections").toStringList());
    // index counts the sections shown; hidden ones (no bookmarks) keep their place relative to the next shown one
    QStringList shown = shown_sections();
    QStringList now_shown = sidebar::moved(shown, section, index);
    if (now_shown == shown)
        return;
    QStringList now;
    for (const QString &id : now_shown) {
        int at = order.indexOf(id);
        for (int i = 0; i < at; ++i)   // hidden sections that came before it
            if (!shown.contains(order[i]) && !now.contains(order[i]))
                now << order[i];
        now << id;
    }
    for (const QString &id : order)
        if (!now.contains(id))
            now << id;
    settings().setValue("sidebar_sections", now);
    settings().sync();
    atc::announce("sidebar");
}

void Sidebar::toggle_section(const QString &section)
{
    QStringList collapsed = settings().value("sidebar_collapsed").toStringList();
    if (collapsed.contains(section))
        collapsed.removeAll(section);
    else
        collapsed << section;
    settings().setValue("sidebar_collapsed", collapsed);
    settings().sync();
    atc::announce("sidebar");
}

void Sidebar::select_path(const QString &path)
{
    blockSignals(true);
    setCurrentItem(nullptr);
    for (int i = 0; i < count(); ++i) {
        if (item(i)->data(PathRole).toString() == path) {
            setCurrentRow(i);
            break;
        }
    }
    blockSignals(false);
}

void Sidebar::clicked(QListWidgetItem *it)
{
    if (it->data(Qt::UserRole).toList().value(0) == "header") {
        toggle_section(it->data(SectionRole).toString());
        return;
    }
    QString p = it->data(PathRole).toString();
    if (!p.isEmpty())
        Q_EMIT open_path(p, QGuiApplication::keyboardModifiers() & Qt::ControlModifier);
}

void Sidebar::mousePressEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton) {
        press_pos = ev->position().toPoint();
        press_index = indexAt(press_pos);
    }
    QListWidget::mousePressEvent(ev);
}

void Sidebar::mouseMoveEvent(QMouseEvent *ev)
{
    // press and drag an entry (within its section) or a header (the whole section) to rearrange
    if ((ev->buttons() & Qt::LeftButton) && press_index.isValid() &&
        (ev->position().toPoint() - press_pos).manhattanLength() >= QApplication::startDragDistance()) {
        QListWidgetItem *it = item(press_index.row());
        press_index = QPersistentModelIndex();
        if (!it)
            return;
        bool is_header = it->data(Qt::UserRole).toList().value(0) == "header";
        QString section = it->data(SectionRole).toString();
        QString key = is_header ? QString() : it->data(KeyRole).toString();
        auto *mime = new QMimeData;
        mime->setData(SIDEBAR_MIME, (section + "\n" + key).toUtf8());
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        QRect r = visualItemRect(it);
        drag->setPixmap(viewport()->grab(r));
        drag->setHotSpot(press_pos - r.topLeft());
        drag->exec(Qt::MoveAction);
        drop_line = -1;
        viewport()->update();
        return;
    }
    QListWidget::mouseMoveEvent(ev);
}

void Sidebar::mouseReleaseEvent(QMouseEvent *ev)
{
    press_index = QPersistentModelIndex();
    if (ev->button() == Qt::MiddleButton) {
        QListWidgetItem *it = itemAt(ev->position().toPoint());
        if (it && !it->data(PathRole).toString().isEmpty()) {
            Q_EMIT open_path(it->data(PathRole).toString(), true);
            return;
        }
    }
    QListWidget::mouseReleaseEvent(ev);
}

void Sidebar::menu(const QPoint &pos)
{
    QListWidgetItem *it = itemAt(pos);
    if (!it || it->data(PathRole).toString().isEmpty())
        return;
    QString path = it->data(PathRole).toString();
    QVariantList ke = it->data(Qt::UserRole).toList();
    QString kind = ke.value(0).toString();
    QVariant extra = ke.value(1);
    QMenu m(this);
    m.addAction("Open", this, [this, path]() { Q_EMIT open_path(path, false); });
    m.addAction("Open in New Tab", this, [this, path]() { Q_EMIT open_path(path, true); });
    if (kind != "phone" && isdir(path))   // a stat on a phone can wait behind its transfers
        m.addAction("Open in Terminal", this, [path]() { open_terminal(path); });
    if (kind == "bookmark") {
        int i = extra.toInt();
        m.addSeparator();
        m.addAction("Edit Bookmark…", this, [this, path]() { edit_bookmark(path); });
        m.addAction("Remove Bookmark", this, [this, i]() { remove_bookmark(i); });
        m.addAction("Move Up", this, [this, i]() { move_bookmark(i, -1); });
        m.addAction("Move Down", this, [this, i]() { move_bookmark(i, 1); });
    } else if (kind == "trash") {
        m.addSeparator();
        m.addAction("Empty Trash", this, [this]() { Q_EMIT empty_trash_requested(); });
    } else if (kind == "mount") {
        m.addSeparator();
        m.addAction("Unmount", this, [this, path]() { unmount(path); });
    } else if (kind == "phone" && phones.value(extra.toInt()).mount) {
        int i = extra.toInt();
        m.addSeparator();
        QString label = g_mount_can_eject(phones[i].mount.get()) ? "Eject" : "Unmount";
        m.addAction(label, this, [this, i]() { eject_phone(i); });
    }
    m.exec(viewport()->mapToGlobal(pos));
}

void Sidebar::unmount(const QString &path)
{
    auto r = proc::run({"gio", "mount", "-u", path});
    if (r.rc != 0)
        r = proc::run({"umount", path});
    if (r.rc != 0) {
        QString err = QString::fromUtf8(r.err);
        QMessageBox::warning(this, "Unmount", err.isEmpty() ? "Unmount failed" : err);
    }
    refresh();
}

void Sidebar::eject_phone(int i)
{
    if (i < 0 || i >= phones.size() || !phones[i].mount)
        return;
    QPointer<Sidebar> self(this);
    overview::unmount(this, phones[i].mount, [self](const QString &err) {
        if (!self)
            return;
        if (!err.isEmpty())
            QMessageBox::warning(self, "Unmount", err);
        self->refresh();
    });
}

void Sidebar::edit_bookmark(const QString &path)
{
    if (dialogs::edit_bookmark(this, path))
        refresh();
}

void Sidebar::remove_bookmark(int i)
{
    auto bms = read_bookmarks();
    if (i < 0 || i >= bms.size())
        return;
    bms.removeAt(i);
    write_bookmarks(bms);
    refresh();
}

void Sidebar::move_bookmark(int i, int d)
{
    auto bms = read_bookmarks();
    int j = i + d;
    if (i >= 0 && i < bms.size() && j >= 0 && j < bms.size()) {
        bms.swapItemsAt(i, j);
        write_bookmarks(bms);
        refresh();
    }
}

void Sidebar::add_bookmark(const QString &path)
{
    auto bms = read_bookmarks();
    bool known = false;
    for (const auto &b : bms)
        known = known || b.first == path;
    if (!known) {
        QString label = basename(rstrip(path, '/'));
        bms << qMakePair(path, label.isEmpty() ? path : label);
        write_bookmarks(bms);
    }
    refresh();
}

// drag & drop onto places
Sidebar::Drop Sidebar::drop_for(const QPoint &pos, const QString &section, const QString &key) const
{
    // the insertion point under pos: between the section's entries, or (for a section) between sections
    Drop d;
    d.section = section;
    d.key = key;
    int n = count();
    int row = n;
    if (QListWidgetItem *it = itemAt(pos)) {
        QRect r = visualItemRect(it);
        row = this->row(it) + (pos.y() >= r.center().y() ? 1 : 0);
    } else if (n && pos.y() < visualItemRect(item(0)).top()) {
        row = 0;
    }
    if (!key.isEmpty()) {
        int first = section_row(section) + 1, end = section_end(section);
        if (first <= 0)
            return Drop();
        d.row = std::clamp(row, first, end);
        d.index = d.row - first;
        return d;
    }
    // a whole section: snap to the nearer edge of the section under the pointer
    QStringList shown = shown_sections();
    int target = shown.size();
    if (row < n) {
        QString over = item(row)->data(SectionRole).toString();
        int i = shown.indexOf(over);
        if (i >= 0) {
            int top = section_row(over), end = section_end(over);
            target = (row - top) * 2 <= end - top ? i : i + 1;
        }
    }
    d.index = target;
    d.row = target < shown.size() ? section_row(shown[target]) : n;
    return d;
}

void Sidebar::dragEnterEvent(QDragEnterEvent *ev)
{
    if (ev->mimeData()->hasFormat(SIDEBAR_MIME) && ev->source() == this)
        ev->acceptProposedAction();
    else if (ev->mimeData()->hasUrls())
        ev->acceptProposedAction();
}

void Sidebar::dragMoveEvent(QDragMoveEvent *ev)
{
    if (ev->mimeData()->hasFormat(SIDEBAR_MIME)) {
        QStringList sk = QString::fromUtf8(ev->mimeData()->data(SIDEBAR_MIME)).split('\n');
        Drop d = ev->source() == this ? drop_for(ev->position().toPoint(), sk.value(0), sk.value(1)) : Drop();
        if (d.row < 0) {
            drop_line = -1;
            ev->ignore();
        } else {
            drop_line = d.row < count() ? visualItemRect(item(d.row)).top() : visualItemRect(item(count() - 1)).bottom() + 1;
            ev->acceptProposedAction();
        }
        viewport()->update();
        return;
    }
    QListWidgetItem *it = itemAt(ev->position().toPoint());
    QString p = it ? it->data(PathRole).toString() : QString();
    if (p.startsWith('/') && isdir(p))
        ev->acceptProposedAction();
    else
        ev->ignore();
}

void Sidebar::dragLeaveEvent(QDragLeaveEvent *ev)
{
    drop_line = -1;
    viewport()->update();
    QListWidget::dragLeaveEvent(ev);
}

void Sidebar::dropEvent(QDropEvent *ev)
{
    if (ev->mimeData()->hasFormat(SIDEBAR_MIME)) {
        drop_line = -1;
        viewport()->update();
        QStringList sk = QString::fromUtf8(ev->mimeData()->data(SIDEBAR_MIME)).split('\n');
        Drop d = ev->source() == this ? drop_for(ev->position().toPoint(), sk.value(0), sk.value(1)) : Drop();
        if (d.row < 0)
            return;
        ev->acceptProposedAction();
        // after the drag has finished: moving rebuilds the list
        QTimer::singleShot(0, this, [this, d]() {
            if (d.key.isEmpty())
                move_section(d.section, d.index);
            else
                move_entry(d.section, d.key, d.index);
        });
        return;
    }
    QListWidgetItem *it = itemAt(ev->position().toPoint());
    if (!it || it->data(PathRole).toString().isEmpty())
        return;
    QStringList paths;
    for (const QUrl &u : ev->mimeData()->urls())
        if (u.isLocalFile())
            paths << u.toLocalFile();
    QString target = it->data(PathRole).toString();
    ev->acceptProposedAction();
    QTimer::singleShot(0, this, [this, paths, target]() { Q_EMIT dropped(paths, target); });
}

void Sidebar::paintEvent(QPaintEvent *ev)
{
    QListWidget::paintEvent(ev);
    if (drop_line < 0)
        return;
    QPainter p(viewport());
    p.setPen(QPen(palette().color(QPalette::Highlight), 2));
    p.drawLine(4, drop_line, viewport()->width() - 4, drop_line);
}

// ---------------------------------------------------------------- info panel

InfoPanel::InfoPanel(ThumbnailManager *thumbs, QWidget *parent) : QScrollArea(parent), thumbs(thumbs)
{
    setWidgetResizable(true);
    setMinimumWidth(220);
    auto *w = new QWidget;
    setWidget(w);
    auto *lay = new QVBoxLayout(w);
    preview = new QLabel;
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumHeight(180);
    lay->addWidget(preview);
    title = new QLabel;
    title->setWordWrap(true);
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont f = font();
    f.setBold(true);
    title->setFont(f);
    lay->addWidget(title);
    form = new QFormLayout;
    lay->addLayout(form);
    extra = new QVBoxLayout;
    lay->addLayout(extra);
    lay->addStretch(1);
    timer.setSingleShot(true);
    timer.setInterval(120);
    connect(&timer, &QTimer::timeout, this, &InfoPanel::refresh);
    connect(thumbs, &ThumbnailManager::updated, this, [this](const QString &p) {
        if (p == path)
            set_preview();
    });
}

void InfoPanel::show_path(const QString &p)
{
    path = p;
    timer.start();
}

void InfoPanel::set_preview()
{
    if (path.isEmpty() || !exists(path)) {
        preview->clear();
        return;
    }
    QFileInfo fi(path);
    bool is_dir = fi.isDir();
    int size = std::max(160, std::min(viewport()->width() - 24, 512));
    QPixmap pm;
    if (is_dir)
        pm = thumbs->folder_pixmap(path, fi.lastModified().toSecsSinceEpoch(), 512, folder_previews);
    else if (thumbs::can_thumbnail(path))
        pm = thumbs->get(path, fi.lastModified().toSecsSinceEpoch(), false, 512, fi.size());
    if (!pm.isNull())
        preview->setPixmap(thumbs->scaled(pm, size));
    else
        preview->setPixmap(icon_for_path(path, is_dir).pixmap(128, 128));
}

void InfoPanel::clear()
{
    while (form->rowCount())
        form->removeRow(0);
    while (QLayoutItem *it = extra->takeAt(0)) {
        if (it->widget())
            it->widget()->deleteLater();
        delete it;
    }
}

void InfoPanel::row(const QString &k, const QString &v)
{
    auto *lab = new QLabel(v);
    lab->setWordWrap(true);
    lab->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(k + ":", lab);
}

void InfoPanel::refresh()
{
    clear();
    if (path.isEmpty() || !lexists(path)) {
        title->setText("");
        preview->clear();
        return;
    }
    QString t = basename(rstrip(path, '/'));
    title->setText(t.isEmpty() ? path : t);
    set_preview();
    struct stat st;
    if (!stat_(path, st))
        lstat_(path, st);
    if (isdir(path)) {
        try {
            int n = listdir(path).size();
            row("Contents", QString("%1 item%2").arg(n).arg(n != 1 ? "s" : ""));
        } catch (const OSError &) {
            row("Contents", "unreadable");
        }
    } else {
        row("Size", human_size(qint64(st.st_size)));
        row("Type", mime_for(path, 0).comment());
    }
    row("Modified", fmt_time(st.st_mtime, "%Y-%m-%d %H:%M"));
    if (islink(path)) {
        try {
            row("Link to", util::readlink(path));
        } catch (const OSError &) {
        }
    }
    if (is_image(path)) {
        for (const auto &[k, v] : metadata::basic_info(path))
            row(k, v);
        for (const auto &[k, v] : metadata::ai_info(path)) {
            auto *lab = new QLabel("<b>" + k.toHtmlEscaped() + "</b>");
            extra->addWidget(lab);
            if (v.size() > 60 || v.contains('\n')) {
                auto *ed = new QPlainTextEdit(v);
                ed->setReadOnly(true);
                ed->setMaximumHeight(120);
                extra->addWidget(ed);
                auto *btn = new QPushButton("Copy");
                QString text = v;
                connect(btn, &QPushButton::clicked, this, [text]() { QGuiApplication::clipboard()->setText(text); });
                extra->addWidget(btn);
            } else {
                auto *tl = new QLabel(v);
                tl->setWordWrap(true);
                tl->setTextInteractionFlags(Qt::TextSelectableByMouse);
                extra->addWidget(tl);
            }
        }
    }
}

void InfoPanel::resizeEvent(QResizeEvent *ev)
{
    QScrollArea::resizeEvent(ev);
    if (!path.isEmpty())
        set_preview();
}
