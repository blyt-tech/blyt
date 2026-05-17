/*
 * libblyt32 — Blyt32 variant shared library.
 *
 * Cart code calls blyt_* API functions via PLT entries that the runtime
 * dynamic loader resolves.  Shared-API functions (declared in blyt.h) live
 * in libblytcommon.so, which this library lists as a DT_NEEDED dependency;
 * the cart linker follows that chain to resolve the symbols.  Blyt32-specific
 * functions (declared in blyt32.h) are implemented here as they are added.
 */

#include "blyt.h"

/* Blyt32-specific API stubs will be added here as the surface grows. */
