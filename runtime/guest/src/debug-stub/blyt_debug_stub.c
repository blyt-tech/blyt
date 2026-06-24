/*
 * blyt-debug-stub — a minimal riscv32 ELF used as lldb-dap's `program` for
 * native/hybrid cart debug sessions (issue #119, Spike W §5d/§5e).
 *
 * The cart must NOT be lldb-dap's `program`: lldb reads `program` off disk as a
 * permanent *main-executable* module that Unix can never unload, so a hot reload
 * would leave a stale duplicate breakpoint location.  Pointing `program` at this
 * stub instead lets the cart be presented purely as a shared library (the SVR4
 * library list), which IS cleanly unloadable/reloadable.
 *
 * This stub is never executed — the real debuggee is the rv32emu VM reached over
 * the GDB RSP relay.  lldb only reads it to fix the target architecture
 * (riscv:rv32, ilp32d).  It therefore needs no real content; a single empty
 * function is enough to produce a valid ET_DYN riscv32 image.
 */
void blyt_debug_stub(void) {
}
