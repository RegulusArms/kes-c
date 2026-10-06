cmake_minimum_required(VERSION 3.20)
project(kestrel-explorer VERSION 0.2.1 LANGUAGES CXX)

set(KES_VERSION "0.2.1-alpha.3")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)   # focus.cpp: activating a drop target on X11

# GLib headers use "signals" as an identifier, so Qt's lowercase keywords are off (Q_SIGNALS, Q_EMIT, ...)
add_compile_definitions(QT_NO_KEYWORDS KES_VERSION="${KES_VERSION}")

add_executable(kes
    src/main.cpp
    src/util.cpp
    src/proc.cpp
    src/uwp.cpp
    src/fileops.cpp
    src/fm1.cpp
    src/focus.cpp
    src/chooser.cpp
    src/places.cpp
    src/sharing.cpp
    src/undo.cpp
    src/admin.cpp
    src/animate.cpp
    src/atc.cpp
    src/thumbs.cpp
    src/metadata.cpp
    src/viewer.cpp
    src/widgets.cpp
    src/overview.cpp
    src/dialogs.cpp
    src/archive.cpp
    src/archive_ui.cpp
    src/app.cpp
    src/actions.cpp
    data/kestrel.qrc
)
target_link_libraries(kes PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets PkgConfig::GIO PkgConfig::XCB)
target_compile_options(kes PRIVATE -Wall -Wextra -Wno-unused-parameter)

# The small root helper for the admin session (started through pkexec). Qt Core only, for JSON.
add_executable(kes-admin-helper src/admin_helper.cpp)
target_link_libraries(kes-admin-helper PRIVATE Qt6::Core)
target_compile_options(kes-admin-helper PRIVATE -Wall -Wextra)

install(TARGETS kes kes-admin-helper RUNTIME DESTINATION bin)
install(PROGRAMS kes-setup DESTINATION bin)   # kes-setup --default: what install.sh --default does, for the .deb
# For the .deb (cpack): the app menu entry, and the file chooser's portal backend (used only once portals.conf picks it,
# which `install.sh --default` does). The FileManager1 service isn't installed: GNOME Files ships that file.
install(FILES data/kestrel-explorer.desktop DESTINATION share/applications)
install(DIRECTORY data/icons/hicolor DESTINATION share/icons)   # the app icon, "kestrel-explorer"
install(FILES data/kestrel.portal DESTINATION share/xdg-desktop-portal/portals)
install(FILES data/org.freedesktop.impl.portal.desktop.kestrel.service DESTINATION share/dbus-1/services)
# The GNOME Shell extension that focuses a drop target on Wayland (src/focus.h); kes-setup --default enables it
install(DIRECTORY data/gnome-shell/kestrel-focus@regulusarms.com DESTINATION share/gnome-shell/extensions)

# .deb package: cmake -DCMAKE_INSTALL_PREFIX=/usr ... && cd build && cpack
# KES_DEB_VERSION overrides the package version (CI's dev builds); by default it's KES_VERSION with "-" → "~",
# so 0.2.0~alpha sorts before 0.2.0
string(REPLACE "-" "~" KES_DEB_VERSION_DEFAULT "${KES_VERSION}")
set(KES_DEB_VERSION "${KES_DEB_VERSION_DEFAULT}" CACHE STRING "Debian package version")
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME kestrel-explorer)
set(CPACK_PACKAGE_VERSION "${KES_DEB_VERSION}")
set(CPACK_PACKAGE_CONTACT "RegulusAlpha <ross@regulusarms.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "File manager with archive, admin, permission and metadata tools built in")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/RegulusArms/kes-c")
set(CPACK_DEBIAN_PACKAGE_SECTION utils)
# Depends written out rather than from dpkg-shlibdeps: the libraries' package names differ between releases
# (Ubuntu 24.04 has libqt6gui6t64, 26.04 libqt6gui6 again), and one .deb is built for all of them
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.34), libgcc-s1, libstdc++6 (>= 13.1), libglib2.0-0t64 (>= 2.40) | libglib2.0-0 (>= 2.40), libxcb1, libqt6core6t64 (>= 6.4) | libqt6core6 (>= 6.4), libqt6gui6t64 (>= 6.4) | libqt6gui6 (>= 6.4), libqt6widgets6t64 (>= 6.4) | libqt6widgets6 (>= 6.4), qt6-qpa-plugins")
# install.sh's RECOMMENDED list
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pkexec, libglib2.0-bin, xdg-utils, gvfs, gvfs-backends, udisks2, qt6-image-formats-plugins, qt6-svg-plugins | libqt6svg6, kimageformat6-plugins, adwaita-icon-theme, qt6-gtk-platformtheme, 7zip | p7zip-full, unrar, zip, unzip, pigz, zpaq, zstd, xz-utils, bzip2, lzip")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "libimage-exiftool-perl, ffmpeg")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_STRIP_FILES ON)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
include(CPack)
