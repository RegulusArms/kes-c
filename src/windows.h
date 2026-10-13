// Kestrel's windows are free: every window it opens (a dialog, a question, a message, a file or folder picker) can be
// moved anywhere and left open until it's needed. None is modal, which would block the window it came from and, on
// GNOME, drag that window along when it's moved; none is kept above another window. Several can be open at once.
//
// So a window is never opened with exec(), open(), setModal() / setWindowModality(), or the static functions of
// QMessageBox, QInputDialog and QFileDialog (which are all modal): it's made on the heap and shown with show_free()
// or one of the functions below, and what happens after it's answered goes in a callback. A callback runs only while
// the window's parent still exists (the window is deleted with it).
#pragma once

#include <QMessageBox>
#include <QString>

#include <functional>

class QAbstractButton;
class QWidget;

// Shows w as a window of its own: not modal and not kept above its parent's window. Deleted when closed, or with its
// parent (so it still goes with the Kestrel window it belongs to).
void show_free(QWidget *w);

// A message. Returns the box, already shown.
QMessageBox *tell(QWidget *parent, QMessageBox::Icon icon, const QString &title, const QString &text);
inline QMessageBox *warn(QWidget *parent, const QString &title, const QString &text)
{
    return tell(parent, QMessageBox::Warning, title, text);
}
inline QMessageBox *inform(QWidget *parent, const QString &title, const QString &text)
{
    return tell(parent, QMessageBox::Information, title, text);
}

// A question with standard buttons: answered(button) runs once, with the button clicked (Escape clicks the cancelling
// one), or NoButton when the window is closed without one. Returns the box, already shown.
QMessageBox *ask(QWidget *parent, QMessageBox::Icon icon, const QString &title, const QString &text,
                 QMessageBox::StandardButtons buttons, std::function<void(QMessageBox::StandardButton)> answered,
                 QMessageBox::StandardButton default_button = QMessageBox::NoButton);
// Yes / No: yes() runs on Yes
QMessageBox *ask_yes(QWidget *parent, const QString &title, const QString &text, std::function<void()> yes);
// A box made by the caller (its own buttons, a checkbox…), shown free: clicked(button) runs once, with the button
// clicked, or nullptr when it's closed without one.
void ask_box(QMessageBox *box, std::function<void(QAbstractButton *)> clicked);

// A line of text: ok(text) runs with what was typed when OK is pressed.
void ask_text(QWidget *parent, const QString &title, const QString &label, const QString &text,
              std::function<void(const QString &)> ok);

// Kestrel's own chooser window (a Kestrel window with a file-name bar, as other apps get it; chooser.h): chosen(path)
// runs with the folder or file picked. filter: Qt's format, "Checksum files (*.sha256 *.md5);;All files (*)".
void choose_folder(QWidget *parent, const QString &title, const QString &start, std::function<void(const QString &)> chosen);
void choose_file(QWidget *parent, const QString &title, const QString &start, const QString &filter,
                 std::function<void(const QString &)> chosen);
