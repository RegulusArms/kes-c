// The version, compiled into each program on its own (kes: KES_VERSION from CMakeLists.txt; the tests: "test"), so the
// rest of the code is compiled once and shared by kes and the tests.
#include "util.h"

namespace util {
const char *VERSION = KES_VERSION;
}
