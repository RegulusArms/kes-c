# The test programs (see README.md): part of the root project with -DKES_BUILD_TESTS=ON (run.sh), built into
# <build>/tests. They link the app's objects (kes_objs) as kes does, so the app's code is compiled once for both; each
# compiles only itself and its own version ("test").
foreach(name fileops archives prefs atc_sync atc_tasks atc_undo atc_tabs devices sidebar pinned groups slow jobs theme chooser admin_helper ui parsing search viewer tools overview media i18n)
    add_executable(test_${name} test_${name}.cpp ${PROJECT_SOURCE_DIR}/src/version.cpp)
    target_link_libraries(test_${name} PRIVATE kes_objs)
    target_compile_definitions(test_${name} PRIVATE KES_VERSION="test" TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()
