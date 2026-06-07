/* ld_blyt_stub.c — Build-time stub for ld-blyt.so.1.
 *
 * Compiled with -Wl,-soname,ld-blyt.so.1 to produce a stub shared library
 * that satisfies the linker when building native libblytc.so.  This file is
 * never staged to the QEMU VM; at runtime the real /lib/ld-blyt.so.1 (system
 * musl) is loaded by the dynamic linker.
 */
