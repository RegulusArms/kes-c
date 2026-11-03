// Copying a file's or folder's extended attributes (Preferences → Files → Copying, "copy_xattrs"): what a browser saved
// a download from (user.xdg.origin.url), other apps' tags, ACLs (system.posix_acl_*), security labels, attributes
// backup tools rely on. Header-only: Kestrel's own copy (fileops.cpp) and the admin helper's (admin_helper.cpp) both use
// it, on descriptors (never through a symlink).
#pragma once

#include <cerrno>
#include <string>
#include <sys/xattr.h>
#include <vector>

namespace xattrs {

// Copies every extended attribute of `from` onto `to`. Returns the names the destination couldn't hold (a file system
// without them, such as FAT, exFAT, some FUSE mounts: not supported, too big, no room). Names the caller may not set
// (security.*, trusted.* without root: EPERM / EACCES) are skipped quietly, as is one that can't be read or went away
// meanwhile. A source that has none, or can't list them, copies nothing.
inline std::vector<std::string> copy(int from, int to)
{
    std::vector<std::string> left_out;
    std::vector<char> names;
    for (;;) {   // (the list may grow between asking its size and reading it: ERANGE, asked again)
        ssize_t n = flistxattr(from, nullptr, 0);
        if (n <= 0)
            return left_out;
        names.resize(size_t(n));
        n = flistxattr(from, names.data(), names.size());
        if (n >= 0) {
            names.resize(size_t(n));
            break;
        }
        if (errno != ERANGE)
            return left_out;
    }
    std::vector<char> value;
    for (size_t i = 0; i < names.size();) {
        std::string name(names.data() + i);
        i += name.size() + 1;
        if (name.empty())
            continue;
        ssize_t n;
        for (;;) {
            n = fgetxattr(from, name.c_str(), nullptr, 0);
            if (n < 0)
                break;
            value.resize(size_t(n) + 1);
            n = fgetxattr(from, name.c_str(), value.data(), value.size());
            if (n >= 0 || errno != ERANGE)
                break;
        }
        if (n < 0)
            continue;
        if (fsetxattr(to, name.c_str(), value.data(), size_t(n), 0) == 0)
            continue;
        if (errno == EPERM || errno == EACCES)   // not this user's to set (security.*, trusted.*)
            continue;
        left_out.push_back(name);
    }
    return left_out;
}

}  // namespace xattrs
