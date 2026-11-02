#include "links.h"

#include "fileops.h"
#include "i18n.h"
#include "util.h"

#include <QRegularExpression>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <functional>
#include <memory>

using namespace util;

namespace links {

QStringList kinds() { return {"sym", "rel", "hard", "desktop"}; }

QString kind_label(const QString &kind)
{
    if (kind == "sym")
        return tx("Symbolic link");
    if (kind == "rel")
        return tx("Relative symbolic link");
    if (kind == "hard")
        return tx("Hard link");
    return tx("Shortcut (.desktop)");
}

QString Link::path() const { return join(folder, name); }

QString Link::text() const
{
    // a link being changed that still points where it did, the same way, keeps what it held (relpath() could spell a
    // relative path another way)
    if (is_edit() && kind == old_kind && points_to == old_points_to && folder == dirname(existing))
        return old_text;
    return kind == "rel" ? relpath(points_to, folder) : points_to;
}

bool Link::unchanged() const { return is_edit() && path() == existing && text() == old_text; }

Link from_existing(const QString &path)
{
    Link l;
    l.existing = abspath(path);
    l.folder = dirname(l.existing);
    l.name = basename(l.existing);
    l.old_text = util::readlink(l.existing);
    l.kind = l.old_text.startsWith('/') ? "sym" : "rel";
    l.points_to = l.old_text.startsWith('/') ? normpath(l.old_text) : normpath(join(l.folder, l.old_text));
    l.old_kind = l.kind;
    l.old_points_to = l.points_to;
    l.renamed = true;
    return l;
}

QString free_name(const QString &kind, const QString &points_to, const QString &folder, const QSet<QString> &taken)
{
    return basename(unique_path(folder, fileops::link_name(kind, points_to, folder), "num", taken));
}

QList<Problem> check(const Link &l, const QList<Link> &others)
{
    QList<Problem> out;
    auto block = [&out](const QString &t) { out << Problem{t, true}; };
    if (l.name.isEmpty() || l.name.contains('/') || l.name.contains(QChar(0)) || l.name == "." || l.name == "..") {
        block(tx("That name can't be used"));
    } else if (!isdir(l.folder)) {
        block(tx("The folder it goes in isn't there"));
    } else {
        if (l.path() != l.existing && lexists(l.path()))
            block(tx("The name is taken in that folder"));
        for (const Link &o : others)
            if (o.id != l.id && o.path() == l.path()) {
                block(tx("Another link in the list has this name in this folder"));
                break;
            }
    }
    struct stat st;
    if (l.is_edit()) {
        if (!lstat_(l.existing, st) || !S_ISLNK(st.st_mode)) {
            block(tx("The link isn't there any more"));
        } else {
            QString now;
            try {
                now = util::readlink(l.existing);
            } catch (const OSError &) {
            }
            if (now != l.old_text)
                block(tx("The link was changed since it was put in the list"));
        }
        if (l.kind != "sym" && l.kind != "rel")
            block(tx("A symbolic link can only be changed into another symbolic link"));
    }
    if (l.kind == "sym" || l.kind == "rel") {
        if (!lexists(l.points_to))
            out << Problem{tx("What it points to isn't there: the link will be broken"), false};
    } else if (!lstat_(l.points_to, st)) {
        block(tx("What it points to isn't there"));
    } else if (l.kind == "hard") {
        struct stat dir;
        if (!S_ISREG(st.st_mode))
            block(tx("Hard links are for files only (not folders or links)"));
        else if (stat_(l.folder, dir) && dir.st_dev != st.st_dev)
            block(tx("A hard link has to be on the same drive as the file"));
    } else {
        try {
            fileops::desktop_entry(l.points_to);
        } catch (const Error &e) {
            block(e.message());
        }
    }
    return out;
}

// A new symbolic link holding text, at a name that must still be free: made under a part name beside it, then given
// the name (put_new: never over anything)
static void make_new_symlink(const QString &text, const QString &to)
{
    QString part = join(dirname(to), part_name());
    util::symlink(text, part);
    try {
        put_new(part, to);
    } catch (...) {
        ::unlink(enc(part).constData());
        throw;
    }
}

void replace_symlink(const QString &path, const QString &text, const QString &expected)
{
    QString dir = dirname(path), part = join(dir, part_name());
    util::symlink(text, part);
    auto drop = [&part]() { ::unlink(enc(part).constData()); };
    // the new link on the disk before it replaces the old one (a symbolic link can't be fsynced itself: its folder,
    // which holds it, is), so a crash leaves the old link or the new one
    int dfd = ::open(enc(dir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    struct stat st;
    if (!lstat_(path, st) || !S_ISLNK(st.st_mode)) {
        drop();
        throw Error(tx("“%1” isn't a symbolic link any more").arg(path));
    }
    if (!expected.isNull()) {
        QString now;
        try {
            now = util::readlink(path);
        } catch (const OSError &) {
        }
        if (now != expected) {
            drop();
            throw Error(tx("“%1” was changed meanwhile, so it was left as it is").arg(path));
        }
    }
    if (::rename(enc(part).constData(), enc(path).constData()) != 0) {
        int e = errno;
        drop();
        throw_errno(path, e);
    }
}

void move_symlink(const QString &from, const QString &to)
{
    try {
        put_new(from, to);   // a rename that never replaces anything
        return;
    } catch (const OSError &e) {
        if (e.code != EXDEV)
            throw;
    }
    // another drive: a copy of it there first, then the old one goes (not atomic across drives; nothing is lost)
    make_new_symlink(util::readlink(from), to);
    try {
        util::unlink(from);
    } catch (const OSError &e) {
        throw Error(tx("Made “%1”, but couldn't remove “%2”: %3").arg(to, from, e.message()));
    }
}

void commit(const Link &l, UndoItems *undo)
{
    QString to = l.path();
    if (l.is_edit()) {
        if (l.unchanged())
            return;
        struct stat st;
        if (!lstat_(l.existing, st) || !S_ISLNK(st.st_mode))
            throw Error(tx("“%1” isn't a symbolic link any more").arg(l.existing));
        if (util::readlink(l.existing) != l.old_text)
            throw Error(tx("“%1” was changed meanwhile, so it was left as it is").arg(l.existing));
        QString text = l.text();
        if (to == l.existing) {   // new contents, same name: replaced in one step
            replace_symlink(to, text, l.old_text);
            *undo << qMakePair(to, "->" + l.old_text);
            return;
        }
        if (lexists(to))
            throw Error(tx("“%1” is taken").arg(to));
        if (text == l.old_text) {   // only renamed (or moved)
            move_symlink(l.existing, to);
            *undo << qMakePair(l.existing, to);
            return;
        }
        // new contents under a new name: the new link is made first, and the old one goes after
        make_new_symlink(text, to);
        try {
            util::unlink(l.existing);
        } catch (const OSError &e) {
            *undo << qMakePair(to, QString());
            throw Error(tx("Made “%1”, but couldn't remove the old link “%2”: %3").arg(to, l.existing, e.message()));
        }
        *undo << qMakePair(l.existing, to) << qMakePair(to, "->" + l.old_text);
        return;
    }
    if (!isdir(l.folder))
        throw Error(tx("The folder “%1” isn't there").arg(l.folder));
    if (lexists(to))
        throw Error(tx("“%1” is taken").arg(to));
    if (l.kind == "sym" || l.kind == "rel") {
        make_new_symlink(l.text(), to);
    } else if (l.kind == "hard") {
        struct stat st;
        if (!lstat_(l.points_to, st) || !S_ISREG(st.st_mode))
            throw Error(tx("“%1” isn't a file").arg(l.points_to));
        util::link(l.points_to, to);   // link() claims the name itself: it never replaces anything
    } else {
        QByteArray body = fileops::desktop_entry(l.points_to).toUtf8();
        write_parts(to, [&](int fd) {
            write_all(fd, body.constData(), body.size(), to);
            ::fchmod(fd, 0755);
        }, true);
        mark_trusted(to);
    }
    *undo << qMakePair(to, QString());
}

QStringList other_names(const QString &path, Task *task)
{
    struct stat want;
    if (!lstat_(path, want))
        throw_errno(path);
    // the top of its drive: up while the folder above is on the same one
    QString top = dirname(abspath(path));
    while (top != "/") {
        struct stat up;
        if (!stat_(dirname(top), up) || up.st_dev != want.st_dev)
            break;
        top = dirname(top);
    }
    QStringList found;
    QString self = abspath(path);
    qint64 folders = 0;
    std::function<bool(int, const QString &)> walk = [&](int dfd, const QString &dir) {
        std::unique_ptr<DIR, int (*)(DIR *)> d(::fdopendir(dfd), ::closedir);   // (closed also on a cancel)
        if (!d) {
            ::close(dfd);
            return true;
        }
        if (task && ++folders % 64 == 0) {
            task->check();
            task->report(0, 0, dir);
        }
        while (struct dirent *e = ::readdir(d.get())) {
            if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
                continue;
            struct stat st;
            if (::fstatat(::dirfd(d.get()), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || st.st_dev != want.st_dev)
                continue;   // (another drive mounted inside: not this one)
            QString p = join(dir, dec(e->d_name));
            if (S_ISDIR(st.st_mode)) {
                int sub = ::openat(::dirfd(d.get()), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (sub >= 0 && !walk(sub, p))
                    return false;
            } else if (st.st_ino == want.st_ino && p != self) {
                found << p;
                if (found.size() + 1 >= qint64(want.st_nlink))   // all of them
                    return false;
            }
        }
        return true;
    };
    int fd = ::open(enc(top).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        throw_errno(top);
    walk(fd, top);
    return found;
}

}  // namespace links
