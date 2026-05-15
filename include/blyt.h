#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* --- Debug output -------------------------------------------------------- */

/*
 * Write a message to the frontend's log/console.
 * Called via ECALL from inside the emulated cart; implemented by the runtime.
 * The string must be NUL-terminated and readable from the cart's address space.
 */
void blyt_console_debug(const char *s);

#ifdef __cplusplus
}
#endif
