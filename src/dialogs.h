// Dialogs: properties (with the metadata editor and Add Tag picker), open-with, rename, batch rename,
// preferences, edit bookmark.
#pragma once

#include "metadata.h"
#include "util.h"

#include <QDialog>
#include <QHash>
#include <QPointer>

#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;

class TextDialog : public QDialog {
    Q_OBJECT
public:
    TextDialog(QWidget *parent, const QString &title, const QString &text);
};

// Edit one metadata tag, or (with an empty key) pick a tag to add from those writable for `path`. List-type tags
// are edited one item per line. `existing` maps "Group:Tag" -> value for tags already in the file.
class MetaTagDialog : public QDialog {
    Q_OBJECT
public:
    MetaTagDialog(QWidget *parent, const QString &key, const QString &value, const QString &path = QString(),
                  const QHash<QString, QString> &existing = {});
    static std::optional<metadata::Change> ask(QWidget *parent, const QString &key, const QString &value,
                                               const QString &path = QString(),
                                               const QHash<QString, QString> &existing = {});
    QString tag() const;
    metadata::Change result_change() const;

private:
    void fill(const QList<metadata::Section> &sections);
    QTreeView *tag_view();
    void size_columns(QTreeView *view);
    QString catalog_key(const QString &key) const;
    void key_changed(const QString &key);
    bool is_list(const QString &key) const;
    void update_note(const QString &key);
    void ok();

    bool adding;
    bool value_is_list = false;
    QStringList items;
    QHash<QPair<QString, QString>, QString> existing;
    QHash<QPair<QString, QString>, QPair<QString, QStringList>> kinds;   // tag_identity -> (kind, allowed values)
    QHash<QString, QString> help;
    QComboBox *key_combo = nullptr;
    QLineEdit *key_edit = nullptr;
    QLabel *blurb;
    QPlainTextEdit *value;
    QLabel *note;
};

class PropertiesDialog : public QDialog {
    Q_OBJECT
public:
    PropertiesDialog(QWidget *parent, const QStringList &paths);

private:
    void run(std::function<QVariant()> fn, std::function<void(const QVariant &)> cb, const QString &title = QString());
    QWidget *general_tab();
    void show_dir_size(const QVariant &res);
    void change_app();
    void folder_style_rows(QFormLayout *form);
    void apply_folder_style();
    QWidget *perm_tab();
    void toggle_exec(bool on);
    mode_t mode() const;
    void update_octal();
    QWidget *image_tab();
    QWidget *meta_tab();
    bool meta_row_editable(QTreeWidgetItem *it) const;
    QList<QTreeWidgetItem *> meta_selected() const;
    void meta_buttons();
    QString meta_key(QTreeWidgetItem *it) const;
    void meta_write(std::function<QStringList()> fn);
    void reload_meta();
    void meta_edit(QTreeWidgetItem *it);
    void meta_add();
    void meta_remove();
    void meta_clear();
    void maybe_load_meta(int idx);
    void show_meta(const QVariant &rows);
    void filter_meta(const QString &text);
    void meta_menu(const QPoint &pos);
    QWidget *checksum_tab();
    void apply();

public:
    void verify_against(const QString &hash_path);   // the Checksums tab's checksum file (Browse…)
    QLabel *hash_result = nullptr;                   // what it said

private:

    QStringList paths;
    bool single;
    QString path;
    QTabWidget *tabs;
    QLineEdit *name_edit = nullptr;
    QLabel *size_label = nullptr;
    QLabel *app_label = nullptr;
    QComboBox *style_color = nullptr;
    QCheckBox *style_previews = nullptr;
    QHash<mode_t, QCheckBox *> perm_boxes;
    mode_t orig_mode = 0;
    QLabel *octal = nullptr;
    QLineEdit *hash_edit = nullptr;
    QWidget *meta_tab_widget = nullptr;
    QLineEdit *meta_filter = nullptr;
    QTreeWidget *meta_tree = nullptr;
    bool meta_editable = false;
    bool meta_loaded = false;
    QPushButton *meta_add_btn = nullptr, *meta_edit_btn = nullptr, *meta_del_btn = nullptr,
                *meta_clear_btn = nullptr;
};

class OpenWithDialog : public QDialog {
    Q_OBJECT
public:
    OpenWithDialog(QWidget *parent, const QStringList &paths, bool launch = true);
    util::AppRef selected_app() const;
    QCheckBox *default_box;

private:
    void filter(const QString &t);
    void ok();
    QStringList paths;
    bool launch;
    QLineEdit *search;
    QListWidget *list;
};

class BatchRenameDialog : public QDialog {
    Q_OBJECT
public:
    BatchRenameDialog(QWidget *parent, const QStringList &paths);

private:
    QStringList new_names() const;
    void preview();
    void apply();
    QStringList paths;
    QRadioButton *r_tmpl, *r_repl;
    QLineEdit *tmpl, *find, *repl;
    QSpinBox *start;
    QCheckBox *regex, *keep_ext;
    QTableWidget *table;
    QLabel *status;
    QPushButton *ok_btn;
};

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget *parent);

private:
    QComboBox *opener_combo(const QString &ct, const QString &current, bool builtin = false);
    void browse_home();
    void paint_color();
    void pick_color();
    void save();
    QComboBox *hp_mode;
    QLineEdit *hp_edit;
    QWidget *hp_custom;
    QSpinBox *count, *max_mb, *slide;
    QComboBox *order, *img_opener, *vid_opener;
    QColor color;
    QPushButton *color_btn;
    QCheckBox *color_accent;
    QCheckBox *single_click, *list_previews, *play_gifs, *play_webm, *shared_undo, *open_in_tabs;
};

namespace dialogs {

struct RenameResult {
    QString error;        // shown to the user
    bool denied = false;  // permission denied: callers offer to retry as administrator
};
RenameResult do_rename(const QString &path, const QString &new_name);
QString ask_rename(QWidget *parent, const QString &path);   // null if cancelled or unchanged
QString choose_dir(QWidget *parent, const QString &title, const QString &start);
// Edit the name and location of the bookmark pointing at `target`. Returns true if saved.
bool edit_bookmark(QWidget *parent, const QString &target);

}  // namespace dialogs
