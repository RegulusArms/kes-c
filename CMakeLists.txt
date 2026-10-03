cmake_minimum_required(VERSION 3.20)
project(kestrel-explorer VERSION 0.1.2 LANGUAGES CXX)

set(KES_VERSION "0.1.2-alpha")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)

# GLib headers use "signals" as an identifier, so Qt's lowercase keywords are off (Q_SIGNALS, Q_EMIT, ...)
add_compile_definitions(QT_NO_KEYWORDS KES_VERSION="${KES_VERSION}")

add_executable(kes
    src/main.cpp
    src/util.cpp
    src/proc.cpp
    src/uwp.cpp
    src/fileops.cpp
    src/admin.cpp
    src/thumbs.cpp
    src/metadata.cpp
    src/viewer.cpp
    src/widgets.cpp
    src/overview.cpp
    src/dialogs.cpp
    src/archive.cpp
    src/archive_ui.cpp
    src/app.cpp
)
target_link_libraries(kes PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets PkgConfig::GIO)
target_compile_options(kes PRIVATE -Wall -Wextra -Wno-unused-parameter)

# The small root helper for the admin session (started through pkexec). Qt Core only, for JSON.
add_executable(kes-admin-helper src/admin_helper.cpp)
target_link_libraries(kes-admin-helper PRIVATE Qt6::Core)
target_compile_options(kes-admin-helper PRIVATE -Wall -Wextra)

install(TARGETS kes kes-admin-helper RUNTIME DESTINATION bin)
