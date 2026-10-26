#include "towerwin.h"

#include "atc.h"
#include "util.h"
#include "windiag.h"
#include "windows.h"

#include <QCheckBox>
#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <csignal>

using namespace util;

static QPointer<TowerWindow> the_window;

TowerWindow *TowerWindow::open(QWidget *parent)
{
    if (the_window) {
        the_window->raise();
        the_window->activateWindow();
        return the_window;
    }
    auto *w = new TowerWindow(parent ? parent->window() : nullptr);
    the_window = w;
    show_free(w);
    return w;
}

static QTreeWidget *table(const QStringList &headers, const QString &name)
{
    auto *t = new QTreeWidget;
    t->setRootIsDecorated(false);
    t->setUniformRowHeights(true);
    t->setHeaderLabels(headers);
    t->setAccessibleName(name);
    return t;
}

TowerWindow::TowerWindow(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Tower");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 560);
    auto *lay = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;

    auto *st = new QWidget;
    auto *sl = new QVBoxLayout(st);
    status = new QLabel;
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    sl->addWidget(status, 1);
    restart = new QPushButton("Restart Tower");
    restart->setToolTip("Stop the tower; the next Kestrel to notice starts a new one and every Kestrel checks in again");
    connect(restart, &QPushButton::clicked, this, [this]() {
        qint64 pid = tower_pid;
        if (pid <= 0)
            return;
        ask_yes(this, "Restart Tower",
                "Stop the tower? Kestrel starts a new one at once, and every Kestrel window checks in with it again.",
                [pid]() { ::kill(pid_t(pid), SIGTERM); });
    });
    auto *rl = new QHBoxLayout;
    rl->addWidget(restart);
    rl->addStretch(1);
    sl->addLayout(rl);
    tabs->addTab(st, "Status");

    flights = table({"Kestrel", "Process", "Version", "Protocol", "Windows", "Jobs"}, "Kestrels checked in");
    tabs->addTab(flights, "Kestrels");

    auto *mw = new QWidget;
    auto *ml = new QVBoxLayout(mw);
    auto *mtop = new QHBoxLayout;
    mtop->addWidget(new QLabel("Show:"));
    filter = new QComboBox;
    filter->addItem("All messages", "");
    filter->setAccessibleName("Message type");
    mtop->addWidget(filter);
    paused = new QCheckBox("Pause");
    mtop->addWidget(paused);
    auto *clear = new QPushButton("Clear");
    mtop->addWidget(clear);
    mtop->addStretch(1);
    ml->addLayout(mtop);
    messages = table({"Time", "From", "Type", "Delay", "Fields"}, "Messages");
    messages->setColumnWidth(0, 100);
    messages->setColumnWidth(1, 110);
    messages->setColumnWidth(2, 110);
    messages->setColumnWidth(3, 60);
    connect(clear, &QPushButton::clicked, messages, &QTreeWidget::clear);
    connect(filter, &QComboBox::currentIndexChanged, this, [this]() {
        QString type = filter->currentData().toString();
        for (int i = 0; i < messages->topLevelItemCount(); ++i)
            messages->topLevelItem(i)->setHidden(!type.isEmpty() && messages->topLevelItem(i)->text(2) != type);
    });
    ml->addWidget(messages, 1);
    tabs->addTab(mw, "Messages");

    queue = table({"Job", "Kestrel", "State", "Drives"}, "Job queue");
    tabs->addTab(queue, "Queue && Lanes");
    undo = table({"Undo", "What", "Items"}, "Shared undo");
    tabs->addTab(undo, "Shared Undo");

    // this Kestrel's windows as Qt sees them (windiag.h): for a window that misbehaves, compared with a fresh one
    auto *ww = new QWidget;
    auto *wl = new QVBoxLayout(ww);
    auto *wnote = new QLabel("This Kestrel's windows (another Kestrel's show in the Tower opened from one of them): their "
                             "size, the minimum and maximum they ask for, their state and screen, and their recent "
                             "changes. When a window misbehaves, open a new one (Ctrl+N) and compare the two.");
    wnote->setWordWrap(true);
    wl->addWidget(wnote);
    windows = new QPlainTextEdit;
    windows->setObjectName("windows_report");
    windows->setReadOnly(true);
    windows->setLineWrapMode(QPlainTextEdit::NoWrap);
    windows->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    wl->addWidget(windows, 1);
    auto *wb = new QHBoxLayout;
    auto *wrefresh = new QPushButton("Refresh");
    auto *wcopy = new QPushButton("Copy");
    auto *wsave = new QPushButton("Save to Home Folder");
    wsave->setObjectName("save_windows");
    wb->addWidget(wrefresh);
    wb->addWidget(wcopy);
    wb->addWidget(wsave);
    wb->addStretch(1);
    wl->addLayout(wb);
    connect(wrefresh, &QPushButton::clicked, this, &TowerWindow::show_windows);
    connect(wcopy, &QPushButton::clicked, this, [this]() {
        show_windows();
        QGuiApplication::clipboard()->setText(windows->toPlainText());
    });
    connect(wsave, &QPushButton::clicked, this, [this]() {
        show_windows();
        QString path = join(HOME(), "kestrel-windows-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".txt");
        try {
            write_text(path, windows->toPlainText().toUtf8());
            inform(this, "Windows", "Saved as " + path);
        } catch (const OSError &e) {
            warn(this, "Windows", e.message());
        }
    });
    int windows_tab = tabs->addTab(ww, "Windows");
    connect(tabs, &QTabWidget::currentChanged, this, [this, windows_tab](int i) {
        if (i == windows_tab)
            show_windows();
    });
    lay->addWidget(tabs);

    connect(atc::radio(), &atc::Radio::broadcast, this, &TowerWindow::heard);
    timer.setInterval(1000);
    connect(&timer, &QTimer::timeout, this, &TowerWindow::refresh);
    timer.start();
    refresh();
}

void TowerWindow::show_windows()
{
    int at = windows->verticalScrollBar()->value();
    windows->setPlainText(windiag::report());
    windows->verticalScrollBar()->setValue(at);
}

void TowerWindow::refresh()
{
    QString st = atc::radio()->request("Status");
    QJsonObject s = QJsonDocument::fromJson(st.toUtf8()).object();
    if (st.isNull() || s.isEmpty()) {
        tower_pid = 0;
        status->setText("<b>The tower isn't running.</b><br>Kestrel starts it when a Kestrel window opens; it lands about "
                        "10 seconds after the last one closes.");
        restart->setEnabled(false);
        flights->clear();
        queue->clear();
        undo->clear();
        return;
    }
    tower_pid = s.value("pid").toVariant().toLongLong();
    restart->setEnabled(true);
    qint64 up = QDateTime::currentMSecsSinceEpoch() - qint64(s.value("started").toDouble());
    int landing = s.value("landing_in").toInt(-1);
    QStringList lines;
    lines << "<b>The tower is running.</b>";
    lines << QString("Process: %1").arg(tower_pid);
    lines << QString("Up for: %1").arg(QString("%1:%2:%3").arg(up / 3600000).arg(up / 60000 % 60, 2, 10, QChar('0')).arg(up / 1000 % 60, 2, 10, QChar('0')));
    lines << QString("Protocol: %1 (this Kestrel speaks %2)").arg(s.value("protocol").toInt()).arg(atc::PROTOCOL);
    lines << QString("Kestrels checked in: %1").arg(s.value("flights").toInt());
    lines << (landing >= 0 ? QString("Lands in %1 s (no Kestrel left)").arg((landing + 999) / 1000)
                           : QString("Stays up while a Kestrel is checked in"));
    lines << QString("Messages refused (malformed, unknown, another protocol): %1").arg(s.value("refused").toInt());
    lines << QString("Job queue: %1 job(s), %2").arg(s.value("queued").toInt()).arg(s.value("mode").toString() == "one" ? "one at a time" : s.value("mode").toString() == "all" ? "all at once" : "by drive");
    status->setText(lines.join("<br>"));

    // the Kestrels, with their windows and jobs (their kept state)
    QJsonArray fl = QJsonDocument::fromJson(atc::radio()->request("Flights").toUtf8()).array();
    QJsonArray kept = QJsonDocument::fromJson(atc::radio()->request("Kept").toUtf8()).array();
    QHash<QString, QJsonObject> windows, tasks;
    QJsonObject queue_msg;
    for (const QJsonValue &v : kept) {
        QString from = v.toObject().value("flight").toString();
        QJsonObject m = v.toObject().value("message").toObject();
        if (m.value("type").toString() == "windows")
            windows[from] = m;
        else if (m.value("type").toString() == "tasks")
            tasks[from] = m;
        else if (m.value("type").toString() == "queue")
            queue_msg = m;
    }
    flights->clear();
    QString me = atc::radio()->flight();
    for (const QJsonValue &v : fl) {
        QJsonObject f = v.toObject();
        QString name = f.value("flight").toString();
        names[name] = QString("pid %1").arg(f.value("pid").toVariant().toLongLong());
        QJsonObject w = windows.value(name);
        QString wins = w.isEmpty() ? QString("—")
                                   : QString("%1, last used %2").arg(w.value("count").toInt()).arg(
                                         QDateTime::fromMSecsSinceEpoch(qint64(w.value("active").toDouble())).toString("HH:mm:ss"));
        auto *it = new QTreeWidgetItem(flights, {name + (name == me ? " (this one)" : ""), names[name],
                                                 f.value("version").toString(), QString::number(f.value("protocol").toInt(1)),
                                                 wins, QString::number(tasks.value(name).value("tasks").toArray().size())});
        Q_UNUSED(it);
    }
    // the queue, and which drives are in use (its lanes)
    queue->clear();
    QStringList busy;
    for (const QJsonValue &v : queue_msg.value("jobs").toArray()) {
        QJsonObject j = v.toObject();
        QStringList drives;
        for (const QJsonValue &d : j.value("drives").toArray())
            drives << d.toString();
        QString state = j.value("paused").toBool() ? "paused" : j.value("state").toString();
        new QTreeWidgetItem(queue, {j.value("title").toString(), names.value(j.value("flight").toString(), j.value("flight").toString()),
                                    state, drives.join(", ")});
        if (j.value("state").toString() == "running")
            busy << drives;
    }
    busy.removeDuplicates();
    queue->setToolTip(busy.isEmpty() ? QString("No drive in use") : "Drives in use: " + busy.join(", "));
    // what Ctrl+Z would undo (shared undo), newest first
    undo->clear();
    QJsonArray ops = QJsonDocument::fromJson(atc::radio()->request("UndoList").toUtf8()).array();
    for (int i = ops.size() - 1; i >= 0; --i) {
        QJsonObject op = ops[i].toObject();
        new QTreeWidgetItem(undo, {op.value("label").toString(), op.value("kind").toString(),
                                   QString::number(op.value("items").toArray().size())});
    }
}

void TowerWindow::heard(const QString &flight, const QString &message)
{
    if (paused->isChecked())
        return;
    QJsonObject m = QJsonDocument::fromJson(message.toUtf8()).object();
    QString type = m.value("type").toString();
    QString delay;
    if (m.value("sent").isDouble())
        delay = QString("%1 ms").arg(qint64(QDateTime::currentMSecsSinceEpoch() - m.value("sent").toDouble()));
    QJsonObject fields = m;
    fields.remove("type");
    fields.remove("sent");
    QString text = QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact));
    if (text.size() > 300)
        text = text.left(300) + "…";
    QString from = flight == atc::radio()->flight() ? QString("this Kestrel") : names.value(flight, flight);
    auto *it = new QTreeWidgetItem(messages, {QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), from,
                                              type.isEmpty() ? QString("(not a message)") : type, delay, text});
    it->setToolTip(4, message);
    if (filter->findData(type) < 0 && !type.isEmpty())
        filter->addItem(type, type);
    QString shown = filter->currentData().toString();
    it->setHidden(!shown.isEmpty() && type != shown);
    while (messages->topLevelItemCount() > 2000)
        delete messages->takeTopLevelItem(0);
    messages->scrollToBottom();
}
