// Kestrel as the system's file chooser: the Open and Save dialogs other apps get through xdg-desktop-portal (a
// browser's "Save image as", Flatpak and Snap apps, GTK 4 and Qt apps that use the portal).
//
// install.sh --default registers it: a D-Bus activation file starts `kes --file-chooser`, which owns
// org.freedesktop.impl.portal.desktop.kestrel and serves org.freedesktop.impl.portal.FileChooser, and the user's
// portals.conf picks it for FileChooser (GNOME's chooser stays next in line). Each request opens a Kestrel window in
// chooser mode: a normal window with a ChooserBar (file name, file type, Cancel and Save/Open) at the bottom.
#pragma once

#include <QPair>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

class MainWindow;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
typedef struct _GVariant GVariant;

namespace chooser {

struct Filter {
    QString name;
    QList<QPair<uint, QString>> patterns;   // as the app sent them: (0, glob) or (1, MIME type)
    QStringList globs;                      // what to show: the globs, and each MIME type's globs
};

struct Request {
    QString method;   // OpenFile, SaveFile or SaveFiles
    QString title, accept_label;
    QString current_name, current_folder;   // SaveFile: the suggested name; any: the folder to start in
    bool multiple = false, directory = false;
    QList<Filter> filters;
    int current_filter = -1;
    QStringList files;                            // SaveFiles: the names to save into the chosen folder
    QList<QPair<QString, QString>> choices;   // (id, default): returned as given
    bool saving() const { return method != "OpenFile"; }
};

struct Result {
    bool ok = false;
    QStringList paths;
    int filter = -1;
};

Request parse(const QString &method, const QString &title, GVariant *options);   // options: a{sv}
GVariant *results(const Request &req, const Result &res);                          // a{sv}, floating
QString button_text(const Request &req);   // accept_label without its GTK mnemonic, or Open / Save / Select

// Own the portal backend's name and answer each request with open(request, done); done(result) may be called once.
// The returned function closes the dialog (the app cancelled). Quits after a minute without dialogs.
using Opener = std::function<std::function<void()>(const Request &, std::function<void(const Result &)>)>;
void serve(Opener open);

}  // namespace chooser

// The bottom of a chooser window: the file name (saving), the file type, Cancel and Save / Open.
class ChooserBar : public QWidget {
    Q_OBJECT
public:
    ChooserBar(MainWindow *win, const chooser::Request &req, std::function<void(const chooser::Result &)> done);
    QStringList type_filter() const;              // the chosen file type's globs (empty: all files)
    void activated(const QStringList &files);     // files double-clicked or opened with Enter
    void selection_changed();
    void finish(bool ok, const QStringList &paths = {});   // once; closes the window
    void accept();
    QLineEdit *name = nullptr;

private:
    void update_button();
    MainWindow *win;
    chooser::Request req;
    std::function<void(const chooser::Result &)> done;
    bool finished = false;
    QComboBox *filter = nullptr;
    QPushButton *ok_btn;
};
