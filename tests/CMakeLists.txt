# The test programs (see README.md). run.sh builds them into tests/build.
cmake_minimum_required(VERSION 3.16)
project(kestrel_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
file(GLOB APP_SOURCES ${SRC}/*.cpp ${SRC}/*.h)
# main.cpp and the admin helper are separate programs; app.cpp is included by each test (common.h)
list(REMOVE_ITEM APP_SOURCES ${SRC}/main.cpp ${SRC}/admin_helper.cpp ${SRC}/app.cpp)

add_library(kes_core OBJECT ${APP_SOURCES})
target_include_directories(kes_core PUBLIC ${SRC})
target_compile_definitions(kes_core PUBLIC QT_NO_KEYWORDS KES_VERSION="test")
target_link_libraries(kes_core PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets PkgConfig::GIO)

foreach(name fileops atc_sync atc_tasks atc_undo atc_tabs devices sidebar theme)
    add_executable(test_${name} test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE kes_core)
endforeach()
