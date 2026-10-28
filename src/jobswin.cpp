#include "jobswin.h"

#include "atc.h"
#include "fileops.h"
#include "jobs.h"
#include "prefs.h"
#include "thumbs.h"
#include "util.h"
#include "windows.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace util;

namespace {

enum { KeyRole = Qt::UserRole, FractionRole = Qt::UserRole + 1 };

// the Progress column: a bar (or "…" while it has no percentage)
class ProgressDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(p, option, index);
        QVariant f = index.data(FractionRole);
        if (!f.isValid())
            return;
        double frac = f.toDouble();
        QRect r = option.rect.adjusted(4, option.rect.height() / 2 - 4, -4, -(option.rect.height() / 2 - 4));
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(option.palette.color(QPalette::Mid));
        p->setBrush(option.palette.color(QPalette::Base));
        p->drawRoundedRect(QRectF(r), 3, 3);
        if (frac >= 0) {
            QRectF done(r.x() + 1, r.y() + 1, (r.width() - 2) * std::min(frac, 1.0), r.height() - 2);
            p->setPen(Qt::NoPen);
            p->setBrush(option.palette.color(QPalette::Highlight));
            p->drawRoundedRect(done, 2, 2);
        }
        p->restore();
    }
};

QString clock_text(qint64 ms)
{
    qint64 s = ms / 1000;
    return s >= 3600 ? QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))
                     : QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

QString state_label(const QString &state)
{
    static const QHash<QString, QString> labels = {{"waiting", "Waiting"},     {"running", "Running"},
                                                   {"paused", "Paused"},       {"done", "Done"},
                                                   {"done with errors", "Done, with errors"},
                                                   {"failed", "Failed"},       {"cancelled", "Cancelled"}};
    return labels.value(state, state);
}

}  // namespace

static QPointer<JobsWindow> the_window;

JobsWindow *JobsWindow::open(QWidget *parent)
{
    if (the_window && the_window->isVisible()) {   // (a closed one is about to be deleted)
        the_window->raise();
        the_window->activateWindow();
        return the_window;
    }
    Q_UNUSED(parent);
    auto *w = new JobsWindow(nullptr);
    the_window = w;
    show_free(w);
    return w;
}

bool JobsWindow::is_open() { return the_window && the_window->isVisible(); }

JobsWindow::JobsWindow(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Jobs");
    setAttribute(Qt::WA_DeleteOnClose);
    auto *lay = new QVBoxLayout(this);

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel("Run jobs:"));
    mode = new QComboBox;
    mode->addItem("By drive: jobs on different drives at the same time", "drive");
    mode->addItem("One at a time, in order", "one");
    mode->addItem("All at once: every job starts as it's queued", "all");
    mode->setCurrentIndex(mode->findData(jobs::mode()));
    mode->setAccessibleName("How jobs run");
    connect(mode, &QComboBox::currentIndexChanged, this, [this]() { jobs::set_mode(mode->currentData().toString()); });
    top->addWidget(mode);
    start_paused = new QCheckBox("Start new jobs paused");
    start_paused->setChecked(jobs::Queue::instance()->start_paused());
    connect(start_paused, &QCheckBox::toggled, this, [](bool on) { jobs::Queue::instance()->set_start_paused(on); });
    top->addWidget(start_paused);
    top->addStretch(1);
    auto *pause_all = new QPushButton("Pause All");
    connect(pause_all, &QPushButton::clicked, this, []() { jobs::Queue::instance()->pause_all(true); });
    auto *resume_all = new QPushButton("Resume All");
    connect(resume_all, &QPushButton::clicked, this, []() { jobs::Queue::instance()->pause_all(false); });
    top->addWidget(pause_all);
    top->addWidget(resume_all);
    lay->addLayout(top);

    split = new QSplitter;
    auto *left = new QWidget;
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    list = new QTreeWidget;
    list->setAccessibleName("Jobs");
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setHeaderLabels({"Job", "State", "Progress", "Time"});
    list->header()->setStretchLastSection(false);
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list->setColumnWidth(1, 110);
    list->setColumnWidth(2, 130);
    list->setColumnWidth(3, 150);
    list->setItemDelegateForColumn(2, new ProgressDelegate(list));
    connect(list, &QTreeWidget::currentItemChanged, this, &JobsWindow::show_details);
    lists = new QSplitter(Qt::Vertical);
    lists->setChildrenCollapsible(false);
    auto *jbox = new QWidget;   // the jobs and their buttons
    auto *jl = new QVBoxLayout(jbox);
    jl->setContentsMargins(0, 0, 0, 0);
    jl->addWidget(list, 1);
    auto *controls = new QHBoxLayout;
    pause_btn = new QPushButton("Pause");
    connect(pause_btn, &QPushButton::clicked, this, [this]() {
        const Row *r = selected_row();
        act(r && r->state == "paused" ? "resume" : "pause");
    });
    stop_btn = new QPushButton("Stop");
    stop_btn->setToolTip("Cancel the job (Delete)");
    connect(stop_btn, &QPushButton::clicked, this, [this]() { act("cancel"); });
    now_btn = new QPushButton("Start Now");
    now_btn->setToolTip("Start it without waiting for its turn");
    connect(now_btn, &QPushButton::clicked, this, [this]() { act("start_now"); });
    up_btn = new QPushButton("Up");
    connect(up_btn, &QPushButton::clicked, this, [this]() { act("up"); });
    down_btn = new QPushButton("Down");
    connect(down_btn, &QPushButton::clicked, this, [this]() { act("down"); });
    auto *clear = new QPushButton("Clear Finished");
    connect(clear, &QPushButton::clicked, this, []() { TaskBoard::instance()->clear_finished(); });
    for (QPushButton *b : {pause_btn, stop_btn, now_btn, up_btn, down_btn, clear})
        controls->addWidget(b);
    controls->addStretch(1);
    jl->addLayout(controls);
    lists->addWidget(jbox);
    auto *tbox = new QWidget;
    auto *tl = new QVBoxLayout(tbox);
    tl->setContentsMargins(0, 0, 0, 0);
    thumb_list = new QTreeWidget;
    thumb_list->setAccessibleName("Thumbnail jobs");
    thumb_list->setRootIsDecorated(false);
    thumb_list->setUniformRowHeights(true);
    thumb_list->setHeaderLabels({"Thumbnails and folder previews", "For", "State", "Progress"});
    thumb_list->header()->setStretchLastSection(false);
    thumb_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    thumb_list->setColumnWidth(1, 150);
    thumb_list->setColumnWidth(2, 190);
    thumb_list->setColumnWidth(3, 130);
    thumb_list->setItemDelegateForColumn(3, new ProgressDelegate(thumb_list));
    tl->addWidget(thumb_list, 1);
    auto *tc = new QHBoxLayout;
    thumb_pause = new QPushButton("Pause");
    thumb_stop = new QPushButton("Stop");
    thumb_stop->setToolTip("Stop making this folder's previews (those made stay)");
    auto thumb_act = [this](bool stop) {
        ThumbnailManager *t = ThumbnailManager::instance();
        QTreeWidgetItem *it = thumb_list->currentItem();
        if (!t || !it)
            return;
        quint64 id = it->data(0, Qt::UserRole).toULongLong();
        if (stop)
            t->stop_job(id);
        else
            t->pause_job(id, it->text(2) != "Paused");
    };
    connect(thumb_pause, &QPushButton::clicked, this, [thumb_act]() { thumb_act(false); });
    connect(thumb_stop, &QPushButton::clicked, this, [thumb_act]() { thumb_act(true); });
    connect(thumb_list, &QTreeWidget::currentItemChanged, this, &JobsWindow::refresh_thumbs);
    tc->addWidget(thumb_pause);
    tc->addWidget(thumb_stop);
    tc->addSpacing(12);
    tc->addWidget(new QLabel("Thumbnailing:"));
    thumb_mode = new QComboBox;
    thumb_mode->setAccessibleName("How thumbnailing runs");
    for (const auto &[label, value] : prefs::find("thumb_mode")->choices)
        thumb_mode->addItem(label, value);
    thumb_mode->setToolTip(prefs::find("thumb_mode")->tip + "\nFor every Kestrel window (also Preferences → Images & Videos).");
    auto show_mode = [this]() {
        QSignalBlocker quiet(thumb_mode);
        thumb_mode->setCurrentIndex(thumb_mode->findData(ThumbnailManager::known_mode(prefs::get("thumb_mode").toString())));
    };
    show_mode();
    connect(thumb_mode, &QComboBox::currentIndexChanged, this,
            [this]() { prefs::set("thumb_mode", thumb_mode->currentData()); });
    connect(prefs::Notifier::instance(), &prefs::Notifier::changed, this, [show_mode](const QStringList &keys) {
        if (keys.contains("thumb_mode"))
            show_mode();
    });
    tc->addWidget(thumb_mode);
    tc->addStretch(1);
    tl->addLayout(tc);
    lists->addWidget(tbox);
    lists->setStretchFactor(0, 3);   // three quarters: the jobs; a quarter: the thumbnails
    lists->setStretchFactor(1, 1);
    ll->addWidget(lists, 1);
    split->addWidget(left);

    auto *right = new QWidget;
    auto *rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    head = new QLabel("Choose a job to see what it's doing.");
    head->setWordWrap(true);
    head->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rl->addWidget(head);
    rl->addWidget(new QLabel("<b>Steps</b>"));
    steps = new QListWidget;
    steps->setAccessibleName("Steps");
    steps->setMaximumHeight(140);
    rl->addWidget(steps);
    rl->addWidget(new QLabel("<b>Log and output</b>"));
    log = new QPlainTextEdit;
    log->setAccessibleName("Log");
    log->setReadOnly(true);
    log->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono("monospace");
    mono.setStyleHint(QFont::Monospace);
    log->setFont(mono);
    rl->addWidget(log, 1);
    auto *lb = new QHBoxLayout;
    auto *copy = new QPushButton("Copy");
    connect(copy, &QPushButton::clicked, this, [this]() { QGuiApplication::clipboard()->setText(log->toPlainText()); });
    auto *save = new QPushButton("Save Log…");
    connect(save, &QPushButton::clicked, this, &JobsWindow::save_log);
    lb->addStretch(1);
    lb->addWidget(copy);
    lb->addWidget(save);
    rl->addLayout(lb);
    split->addWidget(right);
    lay->addWidget(split, 1);

    QSettings &s = settings();
    if (!restoreGeometry(s.value("jobs_window_geometry").toByteArray()))
        resize(1000, 560);
    if (!split->restoreState(s.value("jobs_window_split").toByteArray()))
        split->setSizes({560, 440});
    if (!lists->restoreState(s.value("jobs_window_lists").toByteArray()))
        lists->setSizes({300, 100});
    if (ThumbnailManager *t = ThumbnailManager::instance())
        connect(t, &ThumbnailManager::jobs_changed, this, &JobsWindow::refresh_thumbs);
    refresh_thumbs();

    connect(TaskBoard::instance(), &TaskBoard::changed, this, [this]() {
        if (!timer.isActive())
            timer.start();
    });
    connect(jobs::Queue::instance(), &jobs::Queue::changed, this, [this]() {
        if (!timer.isActive())
            timer.start();
    });
    timer.setInterval(300);   // also while nothing is said: the log and the times move on
    connect(&timer, &QTimer::timeout, this, &JobsWindow::refresh);
    timer.start();
    refresh();
}

void JobsWindow::closeEvent(QCloseEvent *ev)
{
    settings().setValue("jobs_window_geometry", saveGeometry());
    settings().setValue("jobs_window_split", split->saveState());
    settings().setValue("jobs_window_lists", lists->saveState());
    QDialog::closeEvent(ev);
}

void JobsWindow::refresh_thumbs()
{
    ThumbnailManager *t = ThumbnailManager::instance();
    QList<ThumbJob> now = t ? t->thumb_jobs() : QList<ThumbJob>();
    quint64 current = thumb_list->currentItem() ? thumb_list->currentItem()->data(0, Qt::UserRole).toULongLong() : 0;
    bool same = thumb_list->topLevelItemCount() == now.size();
    for (int i = 0; same && i < now.size(); ++i)
        same = thumb_list->topLevelItem(i)->data(0, Qt::UserRole).toULongLong() == now[i].id;
    if (!same) {   // came, went or moved: made again
        thumb_list->blockSignals(true);
        thumb_list->clear();
        for (const ThumbJob &j : now) {
            auto *it = new QTreeWidgetItem(thumb_list);
            it->setData(0, Qt::UserRole, j.id);
            if (j.id == current)
                thumb_list->setCurrentItem(it);
        }
        thumb_list->blockSignals(false);
    }
    for (int i = 0; i < now.size(); ++i) {
        const ThumbJob &j = now[i];
        QTreeWidgetItem *it = thumb_list->topLevelItem(i);
        it->setText(0, j.title() + QString(", %1 of %2").arg(j.done).arg(j.total));
        it->setToolTip(0, j.folder);
        it->setText(1, j.window);
        it->setText(2, j.state());
        it->setData(3, Qt::UserRole + 1, j.total ? double(j.done) / j.total : -1.0);
    }
    QTreeWidgetItem *cur = thumb_list->currentItem();
    thumb_pause->setEnabled(cur);
    thumb_stop->setEnabled(cur);
    thumb_pause->setText(cur && cur->text(2) == "Paused" ? "Resume" : "Pause");
}

void JobsWindow::keyPressEvent(QKeyEvent *ev)
{
    if (ev->key() == Qt::Key_Delete) {
        act("cancel");
        return;
    }
    QDialog::keyPressEvent(ev);
}

QList<JobsWindow::Row> JobsWindow::rows() const
{
    QList<Row> out;
    QHash<QString, TaskInfo> running;
    for (const TaskInfo &t : TaskBoard::instance()->all())
        running.insert(t.flight + "/" + t.id, t);
    QSet<QString> seen;
    // the queue, in its order (every Kestrel's through the tower), then the jobs that aren't queued
    for (const jobs::Job &j : jobs::Queue::instance()->jobs()) {
        Row r;
        r.key = j.flight + "/" + j.id;
        r.flight = j.flight;
        r.id = j.id;
        r.title = j.title;
        r.state = j.state;
        r.admin = j.admin;
        r.local = j.local != nullptr;
        if (running.contains(r.key)) {
            r.text = running[r.key].text;
            r.fraction = running[r.key].fraction;
        }
        if (j.local) {
            r.text = j.local->text;
            r.fraction = j.local->fraction;
            r.elapsed = j.local->elapsed_ms();
        }
        seen << r.key;
        out << r;
    }
    for (auto it = running.begin(); it != running.end(); ++it) {
        if (seen.contains(it.key()))
            continue;
        const TaskInfo &t = it.value();
        Row r{it.key(), t.flight, t.id, t.title, t.state.isEmpty() ? QString("running") : t.state, t.text, t.fraction,
              t.local ? t.local->elapsed_ms() : 0, t.admin, t.local != nullptr};
        out << r;
    }
    const QList<FinishedTask> &done = TaskBoard::instance()->finished;
    for (int i = done.size() - 1; i >= 0; --i) {
        Row r;
        r.key = QString("done:%1:%2").arg(done[i].ended.toMSecsSinceEpoch()).arg(i);
        r.title = done[i].title;
        r.state = done[i].state;
        r.elapsed = done[i].took_ms;
        r.admin = done[i].admin;
        r.local = true;
        r.finished = true;
        r.finished_index = i;
        out << r;
    }
    return out;
}

void JobsWindow::refresh()
{
    QString current = list->currentItem() ? list->currentItem()->data(0, KeyRole).toString() : QString();
    QList<Row> now = rows();
    QStringList keys, old;
    for (const Row &r : now)
        keys << r.key;
    for (const Row &r : shown)
        old << r.key;
    if (keys != old) {   // jobs came, went or moved: the list is made again
        list->blockSignals(true);
        list->clear();
        for (const Row &r : now) {
            auto *it = new QTreeWidgetItem(list);
            it->setData(0, KeyRole, r.key);
        }
        list->blockSignals(false);
    }
    shown = now;
    for (int i = 0; i < now.size(); ++i) {
        const Row &r = now[i];
        QTreeWidgetItem *it = list->topLevelItem(i);
        QString where = r.local || r.finished ? QString() : QString("  (another Kestrel)");
        it->setText(0, (r.admin ? "🛡 " : "") + r.title + where);
        it->setToolTip(0, r.title + (r.text.isEmpty() ? QString() : "\n" + r.text));
        it->setText(1, state_label(r.state));
        bool active = r.state == "running" || r.state == "paused";
        it->setData(2, FractionRole, active ? QVariant(r.fraction) : QVariant());
        it->setText(2, r.state == "waiting" ? QString("…") : QString());
        QString time;
        if (r.elapsed > 0) {
            time = clock_text(r.elapsed);
            if (r.state == "running" && r.fraction > 0.02 && r.fraction < 1)
                time += QString(" · %1 left").arg(clock_text(qint64(r.elapsed * (1 - r.fraction) / r.fraction)));
        }
        it->setText(3, time);
        if (r.key == current && list->currentItem() != it)
            list->setCurrentItem(it);
    }
    show_details();
}

const JobsWindow::Row *JobsWindow::selected_row() const
{
    QString key = list->currentItem() ? list->currentItem()->data(0, KeyRole).toString() : QString();
    for (const Row &r : shown)
        if (r.key == key)
            return &r;
    return nullptr;
}

void JobsWindow::show_details()
{
    const Row *r = selected_row();
    bool queued = r && !r->finished && !r->flight.isEmpty();
    pause_btn->setText(r && r->state == "paused" ? "Resume" : "Pause");
    pause_btn->setEnabled(r && !r->finished && !(r->admin && r->state != "waiting"));
    pause_btn->setToolTip(r && r->admin && r->state != "waiting" ? "An administrator job can't be paused" : QString());
    stop_btn->setEnabled(r && !r->finished);
    now_btn->setEnabled(queued && r->state == "waiting");
    up_btn->setEnabled(queued && r->state == "waiting");
    down_btn->setEnabled(queued && r->state == "waiting");
    if (!r) {
        head->setText("Choose a job to see what it's doing.");
        steps->clear();
        log->clear();
        log_key.clear();
        return;
    }
    Task *task = nullptr;
    for (const jobs::Job &j : jobs::Queue::instance()->jobs())
        if (j.flight + "/" + j.id == r->key)
            task = j.local;
    if (!task && !r->finished)
        for (const TaskInfo &t : TaskBoard::instance()->all())
            if (t.flight + "/" + t.id == r->key)
                task = t.local;
    QStringList drives;
    for (const jobs::Job &j : jobs::Queue::instance()->jobs())
        if (j.flight + "/" + j.id == r->key)
            drives = j.drives;
    QString text = "<b>" + r->title.toHtmlEscaped() + "</b> — " + state_label(r->state);
    if (!r->text.isEmpty())
        text += "<br>" + r->text.toHtmlEscaped();
    if (!drives.isEmpty())
        text += "<br><small>Drives: " + drives.join(", ").toHtmlEscaped() + "</small>";
    head->setText(text);
    // steps: done ✔, on it ▶, still to do •
    QStringList st = r->finished ? TaskBoard::instance()->finished.value(r->finished_index).steps
                                 : task ? task->steps : QStringList();
    int at = task ? task->step.load() : (r->finished ? int(st.size()) : -1);
    QStringList shown_steps;
    int from = std::max(0, at - 200), to = std::min<int>(st.size(), from + 500);   // a window around where it is
    for (int i = from; i < to; ++i)
        shown_steps << (i < at ? "✔ " : i == at ? "▶ " : "• ") + st[i];
    if (to < st.size())
        shown_steps << QString("… and %1 more").arg(st.size() - to);
    QStringList cur;
    for (int i = 0; i < steps->count(); ++i)
        cur << steps->item(i)->text();
    if (cur != shown_steps) {
        steps->clear();
        steps->addItems(shown_steps);
        if (at >= from && at - from < steps->count())
            steps->scrollToItem(steps->item(at - from));
    }
    QString body = r->finished      ? TaskBoard::instance()->finished.value(r->finished_index).log
                   : task           ? task->log_text()
                   : r->local       ? QString()
                                    : QString("This job runs in another Kestrel window; its log is in that window's Jobs window.");
    if (body.isEmpty() && !r->finished)
        body = r->state == "waiting" ? QString("Waiting for its turn: nothing done yet.") : QString();
    if (r->key != log_key || body != log->toPlainText()) {
        QScrollBar *sb = log->verticalScrollBar();
        bool at_end = r->key != log_key || sb->value() >= sb->maximum() - 4;
        log->setPlainText(body);
        if (at_end)
            sb->setValue(sb->maximum());
        log_key = r->key;
    }
}

void JobsWindow::act(const QString &action)
{
    const Row *r = selected_row();
    if (!r || r->finished)
        return;
    bool in_queue = false;
    for (const jobs::Job &j : jobs::Queue::instance()->jobs())
        in_queue = in_queue || j.flight + "/" + j.id == r->key;
    if (in_queue) {
        jobs::Queue::instance()->control(r->flight, r->id, action);
    } else {   // a job outside the queue (it uses no drive lanes): only these
        for (const TaskInfo &t : TaskBoard::instance()->all()) {
            if (t.flight + "/" + t.id != r->key)
                continue;
            if (action == "cancel")
                TaskBoard::instance()->cancel(t);
            else if (t.local && (action == "pause" || action == "resume"))
                t.local->set_paused(action == "pause");
        }
    }
    refresh();
}

void JobsWindow::save_log()
{
    const Row *r = selected_row();
    if (!r)
        return;
    QString text = log->toPlainText(), title = r->title;
    QPointer<JobsWindow> self(this);
    choose_folder(this, "Save Log In", HOME(), [self, text, title](const QString &dir) {
        QString name = QString(title).replace('/', '-') + " log.txt";
        QString path = unique_path(dir, name, "num");
        try {
            write_text(path, text.toUtf8());
            if (self)
                inform(self, "Save Log", "Saved as " + path);
        } catch (const OSError &e) {
            if (self)
                warn(self, "Save Log", e.message());
        }
    });
}
