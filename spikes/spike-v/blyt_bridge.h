// Bridging header: exposes the blyt guest C API to Embedded Swift.
// Embedded Swift imports C headers natively via -import-bridging-header.
// We expose a minimal subset: lifecycle functions, blyt_quit, blyt_console_debug.
// Full blyt.h is included so that all types are available if needed.
#include "../../runtime/guest/include/blyt.h"
