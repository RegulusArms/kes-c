#include "windows.h"

#include "app.h"
#include "chooser.h"

#include <QAbstractButton>
#include <QApplication>
#include <QInputDialog>
#include <QPointer>
#include <QRegularExpression>
#include <QWindow>

#include <memory>

void show_free(QWidget *w)
{
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->setWindowModality(Qt::NonModal);
    // a normal window, not a dialog: X11 keeps a dialog without a parent above all of Kestrel's windows
    w->setWindowFlags((w->windowFlags() & ~Qt::WindowType_Mask) | Qt::Window);
    // Qt makes it transient for the parent's window (kept above it), which can only be undone before it's shown, so
    // its native window is made first. Without AA_DontCreateNativeWidgetSiblings that also gives every widget in the
    // parent window a native window of its own, which glitch on Wayland.
    bool siblings = !QCoreApplication::testAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);
    w->winId();
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, !siblings);
    if (QWindow *win = w->windowHandle())
        win->setTransientParent(nullptr);
    w->show();
}

QMessageBox *tell(QWidget *parent, QMessageBox::Icon icon, const QString &title, const QString &text)
{
    auto *box = new QMessageBox(icon, title, text, QMessageBox::Ok, parent);
    show_free(box);
    return box;
}

QMessageBox *ask(QWidget *parent, QMessageBox::Icon icon, const QString &title, const QString &text,
                 QMessageBox::StandardButtons buttons, std::function<void(QMessageBox::StandardButton)> answered,
                 QMessageBox::StandardButton default_button)
{
    auto *box = new QMessageBox(icon, title, text, buttons, parent);
    if (default_button != QMessageBox::NoButton)
        box->setDefaultButton(default_button);
    ask_box(box, [box, answered](QAbstractButton *b) {
        if (answered)
            answered(b ? box->standardButton(b) : QMessageBox::NoButton);
    });
    return box;
}

QMessageBox *ask_yes(QWidget *parent, const QString &title, const QString &text, std::function<void()> yes)
{
    return ask(parent, QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::No,
               [yes](QMessageBox::StandardButton b) {
                   if (b == QMessageBox::Yes)
                       yes();
               });
}

void ask_box(QMessageBox *box, std::function<void(QAbstractButton *)> clicked)
{
    // a click comes before the box closes; closed without one (the window's close button), it's answered with none
    auto answered = std::make_shared<bool>(false);
    QObject::connect(box, &QMessageBox::buttonClicked, box, [clicked, answered](QAbstractButton *b) {
        *answered = true;
        if (clicked)
            clicked(b);
    });
    QObject::connect(box, &QDialog::finished, box, [clicked, answered]() {
        if (!*answered && clicked)
            clicked(nullptr);
    });
    show_free(box);
}

void ask_text(QWidget *parent, const QString &title, const QString &label, const QString &text,
              std::function<void(const QString &)> ok)
{
    auto *d = new QInputDialog(parent);
    d->setWindowTitle(title);
    d->setLabelText(label);
    d->setTextValue(text);
    QObject::connect(d, &QInputDialog::textValueSelected, d, [ok](const QString &t) { ok(t); });
    show_free(d);
}

// "Name (*.a *.b);;All files (*)" -> the chooser's filters
static QList<chooser::Filter> parse_filters(const QString &filter)
{
    QList<chooser::Filter> out;
    static const QRegularExpression re("^(.*)\\(([^)]*)\\)\\s*$");
    for (const QString &part : filter.split(";;", Qt::SkipEmptyParts)) {
        auto m = re.match(part.trimmed());
        chooser::Filter f;
        f.name = m.hasMatch() ? m.captured(1).trimmed() : part.trimmed();
        for (const QString &g : (m.hasMatch() ? m.captured(2) : part).split(' ', Qt::SkipEmptyParts)) {
            f.patterns << qMakePair(0u, g);
            f.globs << g;
        }
        out << f;
    }
    return out;
}

static void choose(QWidget *parent, const QString &title, const QString &start, bool directory, const QString &filter,
                   std::function<void(const QString &)> chosen)
{
    chooser::Request req;
    req.method = "OpenFile";
    req.title = title;
    req.directory = directory;
    req.current_folder = start;
    req.filters = parse_filters(filter);
    req.current_filter = req.filters.isEmpty() ? -1 : 0;
    QPointer<QWidget> owner(parent);
    QPointer<MainWindow> w = open_chooser(req, [owner, chosen, has_owner = parent != nullptr](const chooser::Result &r) {
        if (r.ok && !r.paths.isEmpty() && (owner || !has_owner))
            chosen(r.paths.first());
    });
    if (parent && w)   // it goes with the window it was opened for
        QObject::connect(parent, &QObject::destroyed, w, [w]() {
            if (w)
                w->close();
        });
}

void choose_folder(QWidget *parent, const QString &title, const QString &start, std::function<void(const QString &)> chosen)
{
    choose(parent, title, start, true, QString(), std::move(chosen));
}

void choose_file(QWidget *parent, const QString &title, const QString &start, const QString &filter,
                 std::function<void(const QString &)> chosen)
{
    choose(parent, title, start, false, filter, std::move(chosen));
}
