#include "blyt.h"

BLYT_LUA_MODULE_EXPORT_VOID(greeting, hello) {
    blyt_console_debug("hello from lua+c");
}
