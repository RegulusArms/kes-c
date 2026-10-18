// Preferences (☰ → Preferences…): every setting Kestrel keeps (prefs.h), also the ones changed elsewhere in the windows,
// in tabs, plus the right-click menus' layout (menus.h). A change applies at once to every open window, here and in
// the other Kestrels; one made elsewhere (the View menu, the zoom slider…) shows here as it happens. A search box
// filters every tab's settings; Reset This Tab / Reset All go back to the defaults.
#pragma once

#include <QDialog>
#include <QHash>

class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QVBoxLayout;

namespace prefs {
struct Pref;
}
namespace menus {
class Editor;
}

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget *parent);
    QTabWidget *tabs;   // (the tests use these)
    QLineEdit *search;
    QWidget *editor(const QString &key) const { return editors.value(key); }
    QWidget *row(const QString &key) const { return rows.value(key); }
    menus::Editor *menu_editor = nullptr;
    void refresh(const QStringList &keys = QStringList());   // show the values in use (empty: all)
    void apply_filter();
    void reset_tab(int index);   // without asking (the buttons ask first)
    void reset_all();

protected:
    void changeEvent(QEvent *ev) override;

private:
    QWidget *make_row(const prefs::Pref &p);
    QWidget *make_custom(const prefs::Pref &p);
    void save(const QString &key, const QVariant &value);
    void load(const QString &key);
    QStringList keys_of(const QString &tab) const;
    QHash<QString, QWidget *> editors, rows;
    QHash<QString, QLabel *> headings;   // tab + "\n" + group
    QHash<QString, std::function<void()>> loaders;   // custom editors: show the value in use
    QStringList tab_names;
    bool loading = false;
};
