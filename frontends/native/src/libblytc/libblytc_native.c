/* libblytc_native.c — Native-path libblytc.so.
 *
 * On the native RISC-V execution path, the C library comes from the system
 * musl interpreter (ld-blyt.so.1) via a DT_NEEDED entry.  This file contains
 * only a version identifier; all stdlib implementations (malloc, snprintf,
 * memcpy, getenv, ...) are provided by ld-blyt.so.1 at runtime.
 *
 * The DT_NEEDED entry for ld-blyt.so.1 is injected at link time via
 * -Wl,--no-as-needed applied to the ld-blyt.so.1 linker stub.
 */

const int blytc_native_version = 1;
