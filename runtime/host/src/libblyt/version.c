/* The one TU compiled with -DBLYT_VERSION (see CMakeLists.txt): the version
 * string carries a per-run timestamp in CI dev builds, so keeping it out of
 * every other TU keeps them compiler-cacheable across runs. */

#include "blyt_runtime.h"

#ifndef BLYT_VERSION
#define BLYT_VERSION "dev"
#endif

const char *blyt_runtime_version(void) {
    return BLYT_VERSION;
}
