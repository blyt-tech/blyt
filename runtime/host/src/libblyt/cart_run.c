#include "cart_run.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "blyt_handle.h" /* runtime/shared: console-wide resource-constant encoding (ADR-0134) */
#include "blyt_resource_codec.h" /* runtime/shared: BLYT_RES_ALGO_NONE (#157) */
#include "blyt_runtime.h"
#include "blyt_trace.h"
#include "cart_load.h"
#include "ecall.h"
#include "resource.h"

#include "blyt_mem_budget.h" /* runtime/shared: unified-budget predicate (#158) */
#include "save.h"
#include "state_buffer.h"
#ifdef BLYT_DAP
#include "dap_server.h"
#endif
#ifdef BLYT_GDB
#include "gdb_stub.h"
#include <pthread.h>
#ifdef __EMSCRIPTEN__
#include "gdb_transport_wasm.h"
#else
#include "gdb_transport_tcp.h"
#endif
#endif
#include "blyt_frame_hash.h"
#include "blyt_phase.h" /* runtime/shared: lifecycle phase (draw()-only, #205) */
#include "blyt_raster.h"
#include "elf32.h"
#include "testcard.h"

#ifdef BLYT_LUA
/* ECALL-bridged Lua C API (ADR-0130).  Only the WASM frontend builds libblyt
 * sources with BLYT_LUA; the host-side Lua state lives in the frontend and is
 * attached via blyt_session_lua_bridge_attach(). */
#include <lauxlib.h>
#include <lua.h>
#endif

/*
 * rv32emu headers — common.h must come first; ${RV32EMU_DIR} is on the
 * include path so these can be referenced by name rather than relative path.
 */
#include "common.h"
#include "io.h"
#include "riscv.h"
#include "riscv_private.h"

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

/* 256 MiB guest memory: libraries live at 128 MiB, validated by Spike C. */
#define BLYT_EMU_MEM_SIZE (256u * 1024u * 1024u)
#define BLYT_STACK_SIZE (1u * 1024u * 1024u)
#define BLYT_CYCLE_PER_STEP 500

/* Base guest address for the first runtime library.  Subsequent libraries
 * are placed at GUEST_LIB_BASE + n * GUEST_LIB_STRIDE (2 MiB each). */
#define GUEST_LIB_BASE 0x08000000u
#define GUEST_LIB_STRIDE 0x00200000u /* 2 MiB between libraries */

/* Maximum number of runtime libraries loaded per cart execution. */
#define MAX_RUNTIME_LIBS 8

/* Maximum PT_LOAD BSS regions tracked from the cart ELF (for --reset-every-frame). */
#define MAX_BSS_REGIONS 16

/* Cart heap arena (ADR-0120): 16 MiB region in guest address space.
 * Placed at 64 MiB — well above the cart/trampoline region (~64 KiB) and
 * below the runtime library region (128 MiB). */
#define BLYT_ARENA_BASE 0x04000000u /* 64 MiB */
#define BLYT_ARENA_SIZE (16u * 1024u * 1024u) /* 16 MiB */

/* Resource scratch region (issue #91): host copies resource bytes here and
 * returns a guest pointer into it from blyt_resource_text_get.  Sits in the
 * free gap between the arena (ends at 80 MiB) and the library base (128 MiB).
 * A per-frame bump allocator; reset at each frame boundary so a pointer stays
 * valid for the whole frame it was fetched in. */
#define BLYT_RESOURCE_SCRATCH_BASE 0x06000000u /* 96 MiB */
#define BLYT_RESOURCE_SCRATCH_SIZE (16u * 1024u * 1024u) /* 16 MiB */

/* Raw framebuffer region (issue #188 / Spike X, Q1): blyt_gfx_acquire() returns
 * this guest VA so the cart can write palette indices directly into a
 * runtime-reserved region, and blyt_gfx_present() copies it into
 * session->pixels[].  Sits in the free 32 MiB gap between the exit trampoline
 * (16 MiB) and the arena (64 MiB); the cart image itself is capped at 16 MiB and
 * the guest stack lives near the 256 MiB top, so nothing else maps here.  Only
 * the framebuffer's 320x240 = 75 KiB is touched. */
#define BLYT_GFX_FB_BASE 0x02000000u /* 32 MiB */

/* Debug hot-reload cart bases (issue #119).  On a reload-while-debugging the
 * cart is re-mapped at a FRESH base each time so lldb sees a relocated module
 * (combined with a unique reported path, this makes it re-read the rebuilt
 * DWARF — Spike W §5e).  Two 16 MiB slots in the free gaps either side of the
 * resource scratch region (80–96 MiB and 112–128 MiB); ping-ponging between
 * them guarantees each reload's base differs from the live one, so the swap
 * zeroes the whole previous image (no overlap, no stale bytes). */
#define BLYT_RELOAD_BASE_A 0x05000000u /* 80 MiB: arena-end .. resource scratch */
#define BLYT_RELOAD_BASE_B 0x07000000u /* 112 MiB: resource scratch-end .. libs */

/* Maximum exported symbols tracked across all loaded runtime libraries. */
#define MAX_SYMS 2048

/* -------------------------------------------------------------------------
 * In-memory library registry
 *
 * Frontends (e.g. libretro) that embed guest libraries as compiled-in data
 * call blyt_register_lib() before creating a session.  dynlink checks here
 * first before falling back to the BLYT_LIB_DIR filesystem path.
 * ------------------------------------------------------------------------- */

#define MAX_REGISTERED_LIBS 8

typedef struct {
    char name[64];
    const void *data;
    size_t size;
} blyt_registered_lib_t;

static blyt_registered_lib_t g_registered_libs[MAX_REGISTERED_LIBS];
static int g_registered_lib_count = 0;

void blyt_register_lib(const char *name, const void *data, size_t size) {
    if (g_registered_lib_count >= MAX_REGISTERED_LIBS)
        return;
    /* Ignore duplicate registrations */
    for (int i = 0; i < g_registered_lib_count; i++) {
        if (strcmp(g_registered_libs[i].name, name) == 0)
            return;
    }
    blyt_registered_lib_t *r = &g_registered_libs[g_registered_lib_count++];
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';
    r->data = data;
    r->size = size;
}

void blyt_clear_libs(void) {
    g_registered_lib_count = 0;
}

/* -------------------------------------------------------------------------
 * Lua export entry — mirrors blyt_lua_export_entry_t from blyt.h (guest SDK).
 * Must have the same binary layout; used to parse .lua_exports ELF sections.
 * ------------------------------------------------------------------------- */

#ifdef BLYT_LUA
typedef struct {
    char lua_name[32];
    char fn_sym[64];
    char wrap_sym[64];
    uint8_t nargs;
    uint8_t arg_types[4];
    uint8_t ret_type;
    uint8_t _pad[2];
} blyt_lua_export_entry_t; /* 168 bytes — must match guest SDK definition */
#endif

/* -------------------------------------------------------------------------
 * Surface registry (#195/#205) — runtime-managed paletted buffers.
 *
 * Slot 0 is always BLYT_SCREEN (session->pixels[], never reaped or
 * budget-counted); slots 1.. are off-screen surfaces created within draw() and
 * auto-reaped at the frame boundary.  A surface handle (blyt_handle.h) is
 * kind SURFACE | gen | index; a resolver rejects a wrong kind or a stale
 * generation.  The canonical buffer is the single source of truth for coherence
 * across the emulated / host-Lua / native execution models (decision 1).
 * ------------------------------------------------------------------------- */

#define BLYT_SURFACE_MAX 64 /* registry slots incl. slot 0 = screen */

typedef struct {
    uint8_t *pixels; /* canonical buffer; slot 0 aliases session->pixels */
    int32_t w, h; /* dimensions; stride == w (tightly packed) */
    uint16_t gen; /* surface generation — bumped on reap/destroy */
    bool in_use;
    bool is_screen; /* slot 0: draws flip cart_has_drawn; never freed/reaped */
    bool owned; /* pixels was malloc'd by the runtime (off-screen surfaces) */
    /* Tier-2 lock state (#205): while locked, the canonical buffer is
     * materialized into the guest VA region [lock_vaddr, lock_vaddr+lock_len). */
    bool locked;
    bool lock_phantom; /* acquired outside draw() (release no-op): reads-as-cleared,
                        * never flushes back to the canonical buffer (#205) */
    uint16_t lock_gen; /* bumped on release — a released token goes stale */
    uint32_t lock_vaddr; /* guest VA of the materialized region */
    uint32_t lock_len; /* == w*h */
} blyt_surface_slot_t;

/* The tier-2 lock materialization arena: the free 32 MiB VA gap Spike X verified
 * (BLYT_GFX_FB_BASE .. arena at 64 MiB).  A bump allocator carves per-lock
 * regions from it and resets when the last lock releases; since concurrent locks
 * sum to at most the 16 MB surface budget and the gap is 32 MiB, it can never
 * exhaust (#205). */
#define BLYT_SURFACE_LOCK_ARENA_BASE BLYT_GFX_FB_BASE
#define BLYT_SURFACE_LOCK_ARENA_SIZE 0x02000000u /* 32 MiB */

typedef struct {
    blyt_surface_slot_t slots[BLYT_SURFACE_MAX];
    /* Total bytes of off-screen surface buffers (screen excluded).  Folded into
     * the non-evictable footprint so surface creation shares the unified 16 MB
     * budget with the guest heap and resource cache (#158/#205). */
    uint32_t surface_bytes;
    uint32_t lock_bump; /* bump offset into the lock arena */
    int active_locks; /* live locks; the bump resets to 0 when this hits 0 */
} blyt_surface_registry_t;

/* -------------------------------------------------------------------------
 * ECALL handler context
 * ------------------------------------------------------------------------- */

typedef enum {
    BLYT_DEBUG_RUNNING = 0,
    BLYT_DEBUG_PAUSED_GDB = 1,
    BLYT_DEBUG_PAUSED_DAP = 2,
} blyt_debug_state_t;

typedef struct {
    blyt_log_fn log_fn;
    bool ecall_trapped; /* cart issued a non-permitted ecall */
    bool ecall_aborted; /* cart called abort() — ECALL_EXIT with a0 != 0 */
    bool frame_done; /* set by BLYT_ECALL_FRAME_DONE; cleared by run_frame */
    bool fn_return_done; /* set by BLYT_ECALL_HOST_FN_RETURN; cleared by run_frame */
    bool dap_enabled; /* set by blyt_session_dap_listen(); enables ECALL_DAP_HOOK */
    blyt_debug_state_t debug_state;
    bool gdb_enabled; /* set by blyt_session_gdb_listen() */
    bool gdb_single_step; /* set when vCont;s received; cleared after one step */
    bool gdb_ebreak_pending; /* ebreak fired inside rv_step; cleared by post-step handler */
    uint32_t gdb_bp_resume_addr; /* WASM: bp addr we paused at; cleared when stepping over */
#ifdef BLYT_LUA
    /* ECALL-bridged Lua C API (ADR-0130). */
    struct lua_State *lua_exch; /* exchange thread; set by lua_bridge_attach */
    bool lua_bridge_active; /* a bridged Lua→native call is in flight */
    uint32_t lua_bridge_token; /* nonce passed to the wrapper as its lua_State* */
    bool lua_bridge_error; /* a bridge op raised; run_frame returns FN_ERROR */
#endif
    /* State buffer context — pointer into the owning blyt_session_t. */
    blyt_state_ctx_t *state_ctx;
    /* Save directory (from BLYT_SAVE_DIR or default).  Heap-allocated. */
    char *save_dir;
    /* Cart name derived from the cart path (used as save subdirectory). */
    char cart_name[64];
    /* .cart.config save_version (ADR-0125): stamped into the save header on
     * write; reported to on_load_state from the *save header* on read. */
    uint32_t save_version;
    /* Resource table (issue #91) and the per-frame bump offset into the guest
     * resource scratch region.  scratch_off resets each frame so a pointer
     * returned by blyt_resource_text_get stays valid for that whole frame. */
    blyt_resource_table_t resources;
    uint32_t resource_scratch_off;
    /* Graphics back buffer (issue #188) — pointers into the owning
     * blyt_session_t so the gfx ECALL handlers can draw into session->pixels[]
     * and flip cart_has_drawn (displacing the test card) without a session
     * handle.  Mirrors the state_ctx "pointer into the owning session" idiom. */
    uint8_t *fb; /* = session->pixels */
    bool *cart_has_drawn; /* = &session->cart_has_drawn */
    /* Surface registry (#205): slot 0 = screen (aliases fb), slots 1.. are
     * off-screen surfaces.  Draw-scoped — reaped at each frame boundary. */
    blyt_surface_registry_t surfaces;
    /* Lifecycle phase (#205, blyt_phase.h): set by BLYT_ECALL_PHASE from the
     * guest blyt_main.  Surface access is permitted only while it is
     * BLYT_PHASE_DRAW; outside draw() a debug cart faults (dev_fault) and a
     * release cart gets a defined no-op. */
    int32_t phase;
    bool cart_is_debug; /* .cart.info debug flag — selects dev-trap vs no-op */
    bool dev_fault; /* a draw()-only rule was violated in a debug cart */
    /* Guest vaddr of libblytc's exported blyt_mem_acct (blyt_mem_accounting_t),
     * resolved at cart load (#158). field 0 = guest_heap_used (guest-written,
     * host-read); field 4 = non_evictable_footprint (host-written, guest-read).
     * 0 if the cart has no libblytc (then the budget is unenforced host-side). */
    uint32_t mem_acct_vaddr;
} blyt_run_ctx_t;

static blyt_run_ctx_t *g_run_ctx = NULL;

/* -------------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------------- */

#define MAX_GDB_LIBS 8
#define MAX_GDB_BREAKS 128

typedef struct {
    char path[256]; /* host name or filesystem path of the .so */
    uint32_t l_addr; /* load base in guest memory */
    uint32_t l_ld; /* runtime address of .dynamic section */
} blyt_gdb_lib_t;

typedef struct {
    uint32_t addr;
    uint32_t original_word;
} blyt_gdb_bp_t;

/* Combined symbol table (defined below); the session retains the runtime-lib
 * subset so a cart can be re-linked against the persistent libs on a hot swap. */
struct blyt_symtab;

/* Per-library state during dynamic loading; retained on the session (issue #119)
 * so a hot swap can re-resolve the libs' references back into the cart. */
typedef struct {
    const uint8_t *map; /* host-side mapping */
    size_t size;
    uint32_t bias; /* guest load bias */
    bool mmapped; /* true if map was mmap'd and must be munmap'd on cleanup */
} blyt_lib_t;

struct blyt_session {
    riscv_t *rv;
    /* vm_attr_t must outlive rv: rv_create stores &attr in rv->data (rv->priv),
     * so it must not be stack-allocated in blyt_session_create. */
    vm_attr_t attr;
    blyt_run_ctx_t ctx;
    blyt_state_ctx_t state_ctx;
    /* Palette-indexed framebuffer: filled by the runtime until the cart draws.
     * Frontends call blyt_session_expand_frame() to convert to XRGB8888. */
    uint8_t pixels[BLYT_FRAME_W * BLYT_FRAME_H];
    uint32_t palette[256]; /* XRGB8888 — set at session create, updated by cart */
    uint32_t frame_count;
    bool cart_has_drawn;

#ifdef BLYT_LUA
    /* Resolved export table for WASM hybrid trampolines.  Populated by dynlink
     * when the cart has a .lua_exports section (hybrid Lua+C carts only). */
    struct {
        char lua_name[32];
        uint32_t fn_guest_addr;
        uint32_t wrap_guest_addr; /* ADR-0130: resolved for bridged exports */
        uint8_t nargs;
        uint8_t arg_types[4];
        uint8_t ret_type;
        uint8_t flags; /* BLYT_LUA_EXPORT_FLAG_* */
    } lua_exports[32];
    int lua_nexports;
    /* ADR-0130 bridged-call state: register snapshot taken at
     * begin_bridged_call, restored when a wrapper raises a Lua error so
     * repeated errors do not leak guest stack.  The FP register file is
     * snapshotted too (Spike U): with hardware doubles (RV32D) a bridged
     * wrapper may hold live values in callee-saved FP registers (fs0–fs11),
     * which the error unwind must restore alongside the integer regs. Raw
     * 64-bit (FLEN) words; fcsr is the rounding/exception state. */
    uint32_t bridge_saved_regs[32];
    uint32_t bridge_saved_fcsr;
    uint64_t bridge_saved_fregs[32];
    uint32_t lua_bridge_next_token;
#endif

#ifdef BLYT_GDB
    /* Library layout for GDB qXfer:libraries-svr4:read. */
    blyt_gdb_lib_t gdb_libs[MAX_GDB_LIBS];
    fc_gdb_library_t gdb_libs_ffi[MAX_GDB_LIBS]; /* stable pointers into gdb_libs */
    int gdb_nlibs;
    /* Index of the cart's OWN library entry in gdb_libs (issue #119), or -1.
     * The cart is presented purely as a shared library (never the main exe), so
     * blyt_session_swap_cart can re-report it at a fresh base + unique path on a
     * debug reload and lldb re-reads its DWARF.  See blyt_session_gdb_notify_cart_reloaded. */
    int gdb_cart_lib_idx;
    char gdb_exec_path[4096]; /* cart path for qXfer:exec-file:read */
    /* Software breakpoints: saved original words. */
    blyt_gdb_bp_t gdb_bps[MAX_GDB_BREAKS];
    int gdb_nbp;
    /* Non-blocking two-phase reload solib swap state (issue #170, WASM).  Driven
     * by blyt_session_gdb_reload_notify_begin/_pump across async ticks; 0 when
     * no swap is in flight, 1 after phase 1 published, 2 after phase 2 published.
     * reload_notify_cont is the continue_gen snapshot at the last publish (the
     * swap advances when the client re-resolves and continues past it);
     * reload_notify_old_idx is the stale cart slot dropped in phase 2;
     * reload_notify_ticks bounds the wait so a clientless session can't wedge. */
    int reload_notify_phase;
    unsigned reload_notify_cont;
    int reload_notify_old_idx;
    int reload_notify_ticks;
#endif

    /* Cart BSS regions (recorded at load time for blyt_reset_every_frame_cycle). */
    struct {
        uint32_t start; /* guest vaddr = p_vaddr + p_filesz */
        uint32_t size; /* = p_memsz - p_filesz */
    } cart_bss[MAX_BSS_REGIONS];
    int n_cart_bss;

    /* Cart image placement (issue #127).  cart_base is the guest load base of
     * the cart image (0 for the native bias today); cart_span is the highest
     * mapped offset (max p_vaddr + p_memsz) so [cart_base, cart_base + cart_span)
     * is the cart's region.  Both are updated by blyt_session_swap_cart and used
     * by session_cart_fn to tell cart-native code from runtime-lib code. */
    uint32_t cart_base;
    uint32_t cart_span;

    /* Runtime-lib symbol table retained from the initial dynlink: the cart's
     * own symbols are seeded fresh on top of these to re-link a swapped cart
     * against the persistent libs (issue #127) without reloading them. */
    struct blyt_symtab *lib_syms;

    /* Runtime-lib ELF images retained from the initial dynlink (issue #119).
     * A hot swap must re-resolve the libs' OWN references back into the cart —
     * libblytcommon's blyt_main calls blyt_cart_init/update/draw through GOT
     * entries that were resolved to the first cart's addresses; after a swap
     * (especially at a fresh base) those entries are stale and would jump into
     * freed/relocated memory.  Re-running resolve_elf_plt needs each lib's host
     * image, so they are kept mapped for the session (freed in destroy). */
    blyt_lib_t retained_libs[MAX_RUNTIME_LIBS];
    int n_retained_libs;

    /* Cached guest addresses of cart lifecycle callbacks (0 = not found). */
    uint32_t fn_on_save_state;
    uint32_t fn_init;
    uint32_t fn_on_load_state;
    uint32_t fn_on_new_state;
    uint32_t fn_update;
    uint32_t fn_draw;
    uint32_t fn_on_quit;
    uint32_t fn_cleanup;
    uint32_t fn_on_assets_reloaded; /* dev-only asset hot-swap hook (issue #122) */
    /* blyt_is_quit_requested() from libblytcommon.so: called by the WASM
     * frontend after each lifecycle trampoline to propagate blyt_quit(). */
    uint32_t fn_is_quit_requested;

    /* BLYT_TRACE bookkeeping: open frame ("start" emitted, "end" pending),
     * per-session frame counter for blyt_trace_frame_mark, and the guest
     * address of the in-flight host-initiated fn call (0 = none) so the
     * lifecycle "ret <name>" line can name the function that returned. */
    bool trace_frame_open;
    uint32_t trace_frame_no;
    uint32_t trace_fn_addr;
};

#ifdef BLYT_GDB
static blyt_session_t *g_gdb_session = NULL;
static pthread_mutex_t g_bp_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Issue #146: set by gdb_set_bp/gdb_clear_bp (which run on the GDB transport
 * reader thread) to ask the CPU thread to discard rv32emu's translated-block
 * cache before the next instruction executes.  Inserting or removing a software
 * breakpoint patches guest memory, but a block already translated for that PC
 * range still holds the pre-patch instructions: on insert the ebreak is never
 * decoded (so the breakpoint is silently skipped — the core symptom of #146);
 * on remove a stale ebreak op lingers and blyt_ebreak_handler skips it as
 * "unregistered", advancing PC past — and so skipping — the original
 * instruction.  block_map_clear frees blocks the dispatch loop may be walking
 * and must run on the thread that owns rv, so the write side only raises this
 * flag and run_frame performs the clear at a safe point. */
static volatile bool g_gdb_block_flush_pending = false;

static void gdb_read_regs(uint8_t out[33 * 4]) {
    if (!g_gdb_session)
        return;
    for (int i = 0; i < 32; i++) {
        uint32_t v = rv_get_reg(g_gdb_session->rv, (uint32_t)i);
        out[i * 4 + 0] = (uint8_t)(v);
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
    uint32_t pc = rv_get_pc(g_gdb_session->rv);
    out[32 * 4 + 0] = (uint8_t)(pc);
    out[32 * 4 + 1] = (uint8_t)(pc >> 8);
    out[32 * 4 + 2] = (uint8_t)(pc >> 16);
    out[32 * 4 + 3] = (uint8_t)(pc >> 24);
}

static void gdb_write_regs(const uint8_t in[33 * 4]) {
    if (!g_gdb_session)
        return;
    for (int i = 0; i < 32; i++) {
        uint32_t v = (uint32_t)in[i * 4] | ((uint32_t)in[i * 4 + 1] << 8) |
                     ((uint32_t)in[i * 4 + 2] << 16) | ((uint32_t)in[i * 4 + 3] << 24);
        rv_set_reg(g_gdb_session->rv, (uint32_t)i, v);
    }
    /* PC is register index 32 */
    uint32_t pc = (uint32_t)in[32 * 4] | ((uint32_t)in[32 * 4 + 1] << 8) |
                  ((uint32_t)in[32 * 4 + 2] << 16) | ((uint32_t)in[32 * 4 + 3] << 24);
    g_gdb_session->rv->PC = pc;
}

static uint32_t gdb_read_mem(uint32_t addr, uint8_t *dst, uint32_t n) {
    if (!g_gdb_session)
        return 0;
    vm_attr_t *attr = PRIV(g_gdb_session->rv);
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (!GUEST_RAM_CONTAINS(attr->mem, addr + i, 1))
            break;
        memory_read(attr->mem, &dst[i], addr + i, 1);
    }
    return i;
}

static uint32_t gdb_write_mem(uint32_t addr, const uint8_t *src, uint32_t n) {
    if (!g_gdb_session)
        return 0;
    vm_attr_t *attr = PRIV(g_gdb_session->rv);
    if (!memory_write(attr->mem, addr, src, n))
        return 0;
    return n;
}

static int gdb_set_bp(uint32_t addr) {
    if (!g_gdb_session)
        return -1;
    blyt_session_t *s = g_gdb_session;
    pthread_mutex_lock(&g_bp_mutex);
    if (s->gdb_nbp >= MAX_GDB_BREAKS) {
        pthread_mutex_unlock(&g_bp_mutex);
        return -1;
    }
    /* Read original word. */
    uint8_t orig[4];
    if (gdb_read_mem(addr, orig, 4) != 4) {
        pthread_mutex_unlock(&g_bp_mutex);
        return -1;
    }
    uint32_t orig_word = (uint32_t)orig[0] | ((uint32_t)orig[1] << 8) | ((uint32_t)orig[2] << 16) |
                         ((uint32_t)orig[3] << 24);
    /* Use C.EBREAK (2-byte) for compressed instructions to avoid overwriting adjacent bytes. */
    bool is_compressed = (orig_word & 0x3) != 0x3;
    if (is_compressed) {
        static const uint8_t cebreak[2] = {0x02, 0x90}; /* C.EBREAK = 0x9002 */
        if (gdb_write_mem(addr, cebreak, 2) != 2) {
            pthread_mutex_unlock(&g_bp_mutex);
            return -1;
        }
    } else {
        static const uint8_t ebreak[4] = {0x73, 0x00, 0x10, 0x00};
        if (gdb_write_mem(addr, ebreak, 4) != 4) {
            pthread_mutex_unlock(&g_bp_mutex);
            return -1;
        }
    }
    s->gdb_bps[s->gdb_nbp].addr = addr;
    s->gdb_bps[s->gdb_nbp].original_word = orig_word;
    s->gdb_nbp++;
    g_gdb_block_flush_pending = true; /* issue #146: re-translate with the ebreak */
    pthread_mutex_unlock(&g_bp_mutex);
    return 0;
}

static int gdb_clear_bp(uint32_t addr) {
    if (!g_gdb_session)
        return -1;
    blyt_session_t *s = g_gdb_session;
    pthread_mutex_lock(&g_bp_mutex);
    for (int i = 0; i < s->gdb_nbp; i++) {
        if (s->gdb_bps[i].addr == addr) {
            uint32_t w = s->gdb_bps[i].original_word;
            uint8_t bytes[4] = {(uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16),
                                (uint8_t)(w >> 24)};
            bool compressed = (w & 0x3) != 0x3;
            gdb_write_mem(addr, bytes, compressed ? 2 : 4);
            /* Remove entry by shifting. */
            for (int j = i; j + 1 < s->gdb_nbp; j++)
                s->gdb_bps[j] = s->gdb_bps[j + 1];
            s->gdb_nbp--;
            g_gdb_block_flush_pending = true; /* issue #146: drop the stale ebreak block */
            pthread_mutex_unlock(&g_bp_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_bp_mutex);
    return -1;
}

static const fc_gdb_cpu_ops_t gdb_cpu_ops = {
    .read_regs = gdb_read_regs,
    .write_regs = gdb_write_regs,
    .read_mem = gdb_read_mem,
    .write_mem = gdb_write_mem,
    .set_breakpoint = gdb_set_bp,
    .clear_breakpoint = gdb_clear_bp,
};
#endif /* BLYT_GDB */

/* -------------------------------------------------------------------------
 * EXIT trampoline
 * ------------------------------------------------------------------------- */

static void write_u32_le(uint8_t *dst, uint32_t val) {
    dst[0] = (uint8_t)val;
    dst[1] = (uint8_t)(val >> 8);
    dst[2] = (uint8_t)(val >> 16);
    dst[3] = (uint8_t)(val >> 24);
}

/* ── Unified-budget shared accounting block bridge (ADR-0008 #158) ───────────
 * libblytc exports blyt_mem_acct: field 0 = guest_heap_used (the guest arena
 * writes it), field 4 = non_evictable_footprint (the host writes it). The guest
 * arena's malloc reads the footprint to cap the heap; the host reads the heap
 * total when deciding whether a resource load/pin fits the unified 16 MB budget,
 * and publishes the footprint after every refcount change. */
static uint32_t mem_acct_read_u32(memory_t *mem, uint32_t vaddr) {
    uint8_t b[4];
    memory_read(mem, b, vaddr, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint32_t mem_acct_guest_heap_used(const blyt_run_ctx_t *ctx, memory_t *mem) {
    if (!ctx->mem_acct_vaddr)
        return 0;
    return mem_acct_read_u32(mem, ctx->mem_acct_vaddr);
}

/* Recompute the non-evictable footprint from the resource table, publish it into
 * the guest-visible accounting block, and bound the resident evictable cache to
 * the room the new footprint leaves (the housekeeping eviction site, #158). Call
 * after any load/pin/unpin/release and at the frame boundary. */
/* The non-evictable footprint the budget predicate charges against: resident
 * loaded/pinned/persistent resource bytes plus live off-screen surface buffers
 * (#205).  Both are irreclaimable within a frame, so both sit in the predicate's
 * footprint term alongside the guest heap. */
static uint32_t ctx_non_evictable_footprint(const blyt_run_ctx_t *ctx) {
    return blyt_resource_table_footprint(&ctx->resources) + ctx->surfaces.surface_bytes;
}

static void mem_acct_publish_footprint(blyt_run_ctx_t *ctx, memory_t *mem) {
    if (!ctx->mem_acct_vaddr)
        return;
    uint32_t footprint = ctx_non_evictable_footprint(ctx);
    uint8_t b[4];
    write_u32_le(b, footprint);
    memory_write(mem, ctx->mem_acct_vaddr + 4u, b, 4);
    uint32_t heap_used = mem_acct_guest_heap_used(ctx, mem);
    blyt_resource_table_evict_to_fit(&ctx->resources, blyt_mem_cache_room(heap_used, footprint));
    /* Publish the advisory resident-decompressed cache total for the
     * introspection API (#159) — after eviction, so it reflects what is actually
     * resident. The guest reads it from blyt_mem_acct without an ECALL. */
    write_u32_le(b, blyt_resource_table_resident_decompressed(&ctx->resources));
    memory_write(mem, ctx->mem_acct_vaddr + 8u, b, 4);
}

/* Would making `id` non-evictable (a load/pin that adds e->len to the footprint)
 * still fit the unified budget? incoming is e->len only when this op newly
 * references a so-far-evictable entry; an already-referenced entry adds nothing.
 * Eviction is NOT attempted here — evictable cache is not in the predicate, so
 * evicting it cannot change the decision (ADR-0027 #158). */
static int mem_acct_reference_fits(blyt_run_ctx_t *ctx, memory_t *mem,
                                   const blyt_resource_entry_t *e, int was_evictable) {
    if (!ctx->mem_acct_vaddr)
        return 1; /* no accounting block: budget unenforced host-side */
    uint32_t incoming = was_evictable ? (uint32_t)e->len : 0u;
    uint32_t heap_used = mem_acct_guest_heap_used(ctx, mem);
    uint32_t footprint = ctx_non_evictable_footprint(ctx);
    return blyt_mem_alloc_fits(heap_used, footprint, incoming);
}

static bool inject_exit_trampoline(memory_t *mem) {
    uint8_t stub[16];
    write_u32_le(stub + 0, RV32_LI_A0_0); /* a0 = 0 (clean exit code) */
    write_u32_le(stub + 4, RV32_LI_A7_0); /* a7 = BLYT_ECALL_EXIT */
    write_u32_le(stub + 8, RV32_ECALL);
    write_u32_le(stub + 12, RV32_UNIMP);
    if (!memory_write(mem, BLYT_TRAMPOLINE_EXIT_ADDR, stub, sizeof(stub)))
        return false;

    /* FN_RETURN stub: a0 already holds the C function's return value;
     * set a7 = BLYT_ECALL_HOST_FN_RETURN and fire.  Do NOT clobber a0. */
    write_u32_le(stub + 0, RV32_LI_A7_9); /* a7 = BLYT_ECALL_HOST_FN_RETURN */
    write_u32_le(stub + 4, RV32_ECALL);
    write_u32_le(stub + 8, RV32_UNIMP);
    write_u32_le(stub + 12, RV32_UNIMP);
    return memory_write(mem, BLYT_TRAMPOLINE_FN_RETURN_ADDR, stub, sizeof(stub));
}

/* -------------------------------------------------------------------------
 * ECALL-bridged Lua C API dispatcher (ADR-0130)
 *
 * Executes one Lua C API operation against the exchange thread on behalf of
 * a guest wrapper.  The exchange thread's "outer" stack (no active frames)
 * is the wrapper-visible Lua stack; ops that can run Lua code (metamethods)
 * or allocate are executed inside lua_pcall so errors never reach the panic
 * handler.  On a Lua error the error value is left on the exchange stack,
 * lua_bridge_error is set, and emulation halts WITHOUT advancing the PC —
 * the guest stub never resumes; the frontend raises from the trampoline
 * continuation inside the game coroutine.
 * ------------------------------------------------------------------------- */
#ifdef BLYT_LUA

#define BLYT_BRIDGE_STR_MAX (16u * 1024u * 1024u) /* host-OOM guard */

/* Context for protected ops: filled by bridge_lua_op, read by the pcall'd
 * helper.  A host global is fine — ops are strictly synchronous. */
typedef struct {
    uint32_t opcode;
    int idx; /* absolute exchange-stack index of the op's target */
    const char *str; /* host copy of guest string (key/name/data) */
    size_t str_len;
    lua_Integer i; /* GETI/SETI index */
    int narr, nrec; /* CREATETABLE */
    int out_type; /* GETFIELD/GETI/GETGLOBAL result type */
    int out_more; /* NEXT: 1 = produced key/value */
} bridge_opctx_t;

static bridge_opctx_t g_bridge_opctx;

/* Protected helper: runs the metamethod/allocation-capable part of one op.
 * Arguments (copies of outer-stack values) arrive at indices 1..n; results
 * are returned (LUA_MULTRET) and land appended to the outer stack. */
static int bridge_protected(lua_State *L) {
    bridge_opctx_t *c = &g_bridge_opctx;
    switch (c->opcode) {
    case BLYT_LUA_OP_PUSHLSTRING:
        lua_pushlstring(L, c->str, c->str_len);
        return 1;
    case BLYT_LUA_OP_CREATETABLE:
        lua_createtable(L, c->narr, c->nrec);
        return 1;
    case BLYT_LUA_OP_GETFIELD: /* arg1 = table */
        c->out_type = lua_getfield(L, 1, c->str);
        return 1;
    case BLYT_LUA_OP_SETFIELD: /* arg1 = table, arg2 = value */
        lua_setfield(L, 1, c->str);
        return 0;
    case BLYT_LUA_OP_GETI: /* arg1 = table */
        c->out_type = lua_geti(L, 1, c->i);
        return 1;
    case BLYT_LUA_OP_SETI: /* arg1 = table, arg2 = value */
        lua_seti(L, 1, c->i);
        return 0;
    case BLYT_LUA_OP_NEXT: /* arg1 = table, arg2 = key */
        c->out_more = lua_next(L, 1);
        return c->out_more ? 2 : 0;
    case BLYT_LUA_OP_GETGLOBAL:
        c->out_type = lua_getglobal(L, c->str);
        return 1;
    case BLYT_LUA_OP_SETGLOBAL: /* arg1 = value */
        lua_pushvalue(L, 1);
        lua_setglobal(L, c->str);
        return 0;
    case BLYT_LUA_OP_TOLSTRING: /* arg1 = value (string or number) */
        /* Number→string conversion allocates; do it on the copy so the
         * outer slot keeps its type (the real API converts in place; the
         * difference is unobservable through the bridge). */
        c->str = lua_tolstring(L, 1, &c->str_len);
        return 1; /* keep the (converted) copy alive on the outer stack */
    default:
        return luaL_error(L, "blyt bridge: bad protected opcode %d", (int)c->opcode);
    }
}

/* Read a guest string into a malloc'd NUL-terminated host buffer. */
static char *bridge_read_guest_str(riscv_t *rv, uint32_t vaddr, uint32_t len, bool allow_nul,
                                   size_t *out_len) {
    vm_attr_t *attr = PRIV(rv);
    memory_t *mem = attr->mem;
    if (len > BLYT_BRIDGE_STR_MAX)
        return NULL;
    if (len > 0 && !GUEST_RAM_CONTAINS(mem, vaddr, len))
        return NULL;
    char *buf = malloc((size_t)len + 1);
    if (!buf)
        return NULL;
    if (len > 0)
        memory_read(mem, (uint8_t *)buf, vaddr, len);
    buf[len] = '\0';
    if (!allow_nul && memchr(buf, '\0', len) != NULL) {
        free(buf);
        return NULL;
    }
    *out_len = len;
    return buf;
}

/* Convert a wrapper-supplied stack index to an absolute outer-stack index.
 * Pseudo-indices (registry, upvalues) are rejected (ADR-0130 v1). */
static bool bridge_abs_index(lua_State *EX, int32_t idx, int *out) {
    int top = lua_gettop(EX);
    if (idx >= 1 && idx <= top) {
        *out = (int)idx;
        return true;
    }
    if (idx <= -1 && idx >= -top) {
        *out = top + (int)idx + 1;
        return true;
    }
    return false;
}

static const char *bridge_op_name(uint32_t op) {
    switch (op) {
    case BLYT_LUA_OP_GETTOP:
        return "GETTOP";
    case BLYT_LUA_OP_SETTOP:
        return "SETTOP";
    case BLYT_LUA_OP_PUSHVALUE:
        return "PUSHVALUE";
    case BLYT_LUA_OP_TYPE:
        return "TYPE";
    case BLYT_LUA_OP_PUSHNIL:
        return "PUSHNIL";
    case BLYT_LUA_OP_PUSHBOOLEAN:
        return "PUSHBOOLEAN";
    case BLYT_LUA_OP_PUSHINTEGER:
        return "PUSHINTEGER";
    case BLYT_LUA_OP_PUSHNUMBER:
        return "PUSHNUMBER";
    case BLYT_LUA_OP_PUSHLSTRING:
        return "PUSHLSTRING";
    case BLYT_LUA_OP_TOINTEGERX:
        return "TOINTEGERX";
    case BLYT_LUA_OP_TONUMBERX:
        return "TONUMBERX";
    case BLYT_LUA_OP_TOBOOLEAN:
        return "TOBOOLEAN";
    case BLYT_LUA_OP_TOLSTRING:
        return "TOLSTRING";
    case BLYT_LUA_OP_CREATETABLE:
        return "CREATETABLE";
    case BLYT_LUA_OP_GETFIELD:
        return "GETFIELD";
    case BLYT_LUA_OP_SETFIELD:
        return "SETFIELD";
    case BLYT_LUA_OP_GETI:
        return "GETI";
    case BLYT_LUA_OP_SETI:
        return "SETI";
    case BLYT_LUA_OP_RAWLEN:
        return "RAWLEN";
    case BLYT_LUA_OP_NEXT:
        return "NEXT";
    case BLYT_LUA_OP_GETGLOBAL:
        return "GETGLOBAL";
    case BLYT_LUA_OP_SETGLOBAL:
        return "SETGLOBAL";
    case BLYT_LUA_OP_ERROR:
        return "ERROR";
    case BLYT_LUA_OP_ERRMSG:
        return "ERRMSG";
    default:
        return "?";
    }
}

/* One typed api-channel line per bridge op, formatted with the args that
 * matter for that op (per-API "most useful thing" policy). */
static void bridge_trace_op(uint32_t opcode, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t st,
                            uint32_t val, uint32_t aux) {
    if (!blyt_trace_enabled(BLYT_TRACE_API))
        return;
    char args[64];
    switch (opcode) {
    case BLYT_LUA_OP_GETTOP:
    case BLYT_LUA_OP_PUSHNIL:
    case BLYT_LUA_OP_ERROR:
        args[0] = '\0';
        break;
    case BLYT_LUA_OP_SETTOP:
    case BLYT_LUA_OP_PUSHVALUE:
    case BLYT_LUA_OP_TYPE:
    case BLYT_LUA_OP_TOINTEGERX:
    case BLYT_LUA_OP_TONUMBERX:
    case BLYT_LUA_OP_TOBOOLEAN:
    case BLYT_LUA_OP_RAWLEN:
    case BLYT_LUA_OP_NEXT:
        snprintf(args, sizeof args, "idx=%d", (int32_t)a2);
        break;
    case BLYT_LUA_OP_PUSHBOOLEAN:
        snprintf(args, sizeof args, "b=%u", a2 ? 1u : 0u);
        break;
    case BLYT_LUA_OP_PUSHINTEGER:
        snprintf(args, sizeof args, "n=%d", (int32_t)a2);
        break;
    case BLYT_LUA_OP_PUSHNUMBER: {
        float f;
        memcpy(&f, &a2, 4);
        snprintf(args, sizeof args, "n=%g", (double)f);
        break;
    }
    case BLYT_LUA_OP_PUSHLSTRING:
        snprintf(args, sizeof args, "len=%u", a3);
        break;
    case BLYT_LUA_OP_TOLSTRING:
        snprintf(args, sizeof args, "idx=%d, cap=%u", (int32_t)a2, a4);
        break;
    case BLYT_LUA_OP_CREATETABLE:
        snprintf(args, sizeof args, "narr=%d, nrec=%d", (int32_t)a2, (int32_t)a3);
        break;
    case BLYT_LUA_OP_GETFIELD:
    case BLYT_LUA_OP_SETFIELD:
        snprintf(args, sizeof args, "idx=%d, klen=%u", (int32_t)a2, a4);
        break;
    case BLYT_LUA_OP_GETI:
    case BLYT_LUA_OP_SETI:
        snprintf(args, sizeof args, "idx=%d, i=%d", (int32_t)a2, (int32_t)a3);
        break;
    case BLYT_LUA_OP_GETGLOBAL:
    case BLYT_LUA_OP_SETGLOBAL:
    case BLYT_LUA_OP_ERRMSG:
        snprintf(args, sizeof args, "len=%u", a3);
        break;
    default:
        snprintf(args, sizeof args, "a2=0x%x, a3=0x%x, a4=0x%x", a2, a3, a4);
        break;
    }
    blyt_tracef(BLYT_TRACE_API, "lua_op %s(%s) -> st=%u val=0x%x aux=%u", bridge_op_name(opcode),
                args, st, val, aux);
}

/* Raise: leave `err` (already on top of EX) as the pending error, halt. */
static void bridge_fail(riscv_t *rv) {
    blyt_tracef(BLYT_TRACE_API, "lua_op %s -> error raised", bridge_op_name(g_bridge_opctx.opcode));
    g_run_ctx->lua_bridge_error = true;
    rv_halt(rv); /* PC NOT advanced: the guest stub never resumes */
}

/* Push a formatted error message on EX and fail.  Never raises (pushfstring
 * can OOM-panic in theory; acceptable for the spike, noted in ADR-0130). */
static void bridge_fail_msg(riscv_t *rv, lua_State *EX, const char *msg) {
    lua_checkstack(EX, 2);
    lua_pushstring(EX, msg);
    bridge_fail(rv);
}

/* Run bridge_protected via lua_pcall with `nargs` copies of outer-stack
 * values (absolute indices in `argidx`).  On success results are appended
 * to the outer stack (MULTRET).  Returns true on success; on failure the
 * error value is on top of EX and the call is failed. */
static bool bridge_pcall(riscv_t *rv, lua_State *EX, const int *argidx, int nargs) {
    if (!lua_checkstack(EX, nargs + 2)) {
        bridge_fail_msg(rv, EX, "blyt bridge: exchange stack overflow");
        return false;
    }
    lua_pushcfunction(EX, bridge_protected);
    for (int i = 0; i < nargs; i++)
        lua_pushvalue(EX, argidx[i]);
    if (lua_pcall(EX, nargs, LUA_MULTRET, 0) != LUA_OK) {
        bridge_fail(rv); /* error value already on top */
        return false;
    }
    return true;
}

/* Dispatch one BLYT_ECALL_LUA_OP.  Register conventions per ecall.h. */
static void bridge_lua_op(riscv_t *rv) {
    /* Validity window and token check: anything wrong here is a protocol
     * violation (fatal trap), not a recoverable Lua error. */
    if (!g_run_ctx || !g_run_ctx->lua_bridge_active || !g_run_ctx->lua_exch) {
        g_run_ctx ? (g_run_ctx->ecall_trapped = true) : (void)0;
        rv_halt(rv);
        return;
    }
    lua_State *EX = g_run_ctx->lua_exch;
    uint32_t opcode = rv_get_reg(rv, rv_reg_a0);
    uint32_t token = rv_get_reg(rv, rv_reg_a1);
    if (token != g_run_ctx->lua_bridge_token) {
        g_run_ctx->ecall_trapped = true;
        rv_halt(rv);
        return;
    }
    uint32_t a2 = rv_get_reg(rv, rv_reg_a2);
    uint32_t a3 = rv_get_reg(rv, rv_reg_a3);
    uint32_t a4 = rv_get_reg(rv, rv_reg_a4);
    uint32_t st = BLYT_LUA_ST_OK, val = 0, aux = 0;
    char *hstr = NULL;
    g_bridge_opctx.opcode = opcode;

    switch (opcode) {
    case BLYT_LUA_OP_GETTOP:
        val = (uint32_t)lua_gettop(EX);
        break;

    case BLYT_LUA_OP_SETTOP: {
        int32_t idx = (int32_t)a2;
        int top = lua_gettop(EX);
        /* idx in [0, top] (shrink/keep) or [-top, -1] (pop) — growth with
         * nils is allowed by the real API; permit a bounded version. */
        if (idx >= 0 && idx <= top + 64) {
            if (idx > top && !lua_checkstack(EX, idx - top)) {
                bridge_fail_msg(rv, EX, "blyt bridge: settop overflow");
                return;
            }
            lua_settop(EX, idx);
        } else if (idx < 0 && -idx <= top) {
            lua_settop(EX, idx);
        } else {
            bridge_fail_msg(rv, EX, "blyt bridge: settop index out of range");
            return;
        }
        break;
    }

    case BLYT_LUA_OP_PUSHVALUE: {
        int ai;
        if (!bridge_abs_index(EX, (int32_t)a2, &ai)) {
            bridge_fail_msg(rv, EX, "blyt bridge: invalid stack index");
            return;
        }
        if (!lua_checkstack(EX, 1)) {
            bridge_fail_msg(rv, EX, "blyt bridge: exchange stack overflow");
            return;
        }
        lua_pushvalue(EX, ai);
        break;
    }

    case BLYT_LUA_OP_TYPE: {
        int ai;
        if (!bridge_abs_index(EX, (int32_t)a2, &ai)) {
            val = (uint32_t)LUA_TNONE;
        } else {
            val = (uint32_t)lua_type(EX, ai);
        }
        break;
    }

    case BLYT_LUA_OP_PUSHNIL:
    case BLYT_LUA_OP_PUSHBOOLEAN:
    case BLYT_LUA_OP_PUSHINTEGER:
    case BLYT_LUA_OP_PUSHNUMBER: {
        if (!lua_checkstack(EX, 1)) {
            bridge_fail_msg(rv, EX, "blyt bridge: exchange stack overflow");
            return;
        }
        if (opcode == BLYT_LUA_OP_PUSHNIL) {
            lua_pushnil(EX);
        } else if (opcode == BLYT_LUA_OP_PUSHBOOLEAN) {
            lua_pushboolean(EX, a2 ? 1 : 0);
        } else if (opcode == BLYT_LUA_OP_PUSHINTEGER) {
            lua_pushinteger(EX, (lua_Integer)(int32_t)a2);
        } else {
            uint64_t bits = (uint64_t)a3 << 32 | (uint64_t)a2;
            double d;
            memcpy(&d, &bits, 8);
            lua_pushnumber(EX, (lua_Number)d);
        }
        break;
    }

    case BLYT_LUA_OP_TOINTEGERX:
    case BLYT_LUA_OP_TONUMBERX:
    case BLYT_LUA_OP_TOBOOLEAN: {
        int ai;
        if (!bridge_abs_index(EX, (int32_t)a2, &ai)) {
            bridge_fail_msg(rv, EX, "blyt bridge: invalid stack index");
            return;
        }
        if (opcode == BLYT_LUA_OP_TOINTEGERX) {
            int isnum = 0;
            lua_Integer n = lua_tointegerx(EX, ai, &isnum);
            val = (uint32_t)(int32_t)n;
            aux = (uint32_t)isnum;
        } else if (opcode == BLYT_LUA_OP_TONUMBERX) {
            int isnum = 0;
            double d = (double)lua_tonumberx(EX, ai, &isnum);
            if (isnum) {
                uint64_t bits;
                memcpy(&bits, &d, 8);
                val = (uint32_t)bits;
                aux = (uint32_t)(bits >> 32);
                /* st remains BLYT_LUA_ST_OK */
            } else {
                st = BLYT_LUA_ST_NIL;
                val = 0;
                aux = 0;
            }
        } else {
            val = (uint32_t)lua_toboolean(EX, ai);
        }
        break;
    }

    case BLYT_LUA_OP_TOLSTRING: {
        int ai;
        if (!bridge_abs_index(EX, (int32_t)a2, &ai)) {
            bridge_fail_msg(rv, EX, "blyt bridge: invalid stack index");
            return;
        }
        int t = lua_type(EX, ai);
        if (t != LUA_TSTRING && t != LUA_TNUMBER) {
            st = BLYT_LUA_ST_NIL; /* real lua_tolstring returns NULL */
            break;
        }
        g_bridge_opctx.str = NULL;
        g_bridge_opctx.str_len = 0;
        int args[1] = {ai};
        if (!bridge_pcall(rv, EX, args, 1))
            return;
        /* bridge_protected left the (possibly converted) copy on top; the
         * interned string pointer in opctx.str is valid while it stays. */
        const char *sp = g_bridge_opctx.str;
        size_t slen = g_bridge_opctx.str_len;
        uint32_t buf = a3, cap = a4;
        aux = (uint32_t)slen;
        if (slen + 1 > cap) {
            st = BLYT_LUA_ST_RETRY;
            lua_pop(EX, 1);
            break;
        }
        vm_attr_t *attr = PRIV(rv);
        memory_t *mem = attr->mem;
        if (!GUEST_RAM_CONTAINS(mem, buf, (uint32_t)slen + 1)) {
            lua_pop(EX, 1);
            bridge_fail_msg(rv, EX, "blyt bridge: tolstring buffer out of bounds");
            return;
        }
        memory_write(mem, buf, (const uint8_t *)sp, (uint32_t)slen);
        const uint8_t nul = 0;
        memory_write(mem, buf + (uint32_t)slen, &nul, 1);
        val = (uint32_t)slen;
        lua_pop(EX, 1); /* drop the copy */
        break;
    }

    case BLYT_LUA_OP_PUSHLSTRING: {
        size_t slen = 0;
        hstr = bridge_read_guest_str(rv, a2, a3, true, &slen);
        if (!hstr) {
            bridge_fail_msg(rv, EX, "blyt bridge: bad string pointer/length");
            return;
        }
        g_bridge_opctx.str = hstr;
        g_bridge_opctx.str_len = slen;
        if (!bridge_pcall(rv, EX, NULL, 0)) {
            free(hstr);
            return;
        }
        free(hstr);
        break;
    }

    case BLYT_LUA_OP_CREATETABLE: {
        g_bridge_opctx.narr = (int)(int32_t)a2;
        g_bridge_opctx.nrec = (int)(int32_t)a3;
        if (g_bridge_opctx.narr < 0 || g_bridge_opctx.narr > (1 << 20) || g_bridge_opctx.nrec < 0 ||
            g_bridge_opctx.nrec > (1 << 20)) {
            bridge_fail_msg(rv, EX, "blyt bridge: createtable size out of range");
            return;
        }
        if (!bridge_pcall(rv, EX, NULL, 0))
            return;
        break;
    }

    case BLYT_LUA_OP_GETFIELD:
    case BLYT_LUA_OP_SETFIELD: {
        int ai;
        if (!bridge_abs_index(EX, (int32_t)a2, &ai)) {
            bridge_fail_msg(rv, EX, "blyt bridge: invalid stack index");
            return;
        }
        size_t klen = 0;
        hstr = bridge_read_guest_str(rv, a3, a4, false, &klen);
        if (!hstr) {
            bridge_fail_msg(rv, EX, "blyt bridge: bad field key");
            return;
        }
        g_bridge_opctx.str = hstr;
        if (opcode == BLYT_LUA_OP_GETFIELD) {
            int args[1] = {ai};
            if (!bridge_pcall(rv, EX, args, 1)) {
                free(hstr);
                return;
            }
            val = (uint32_t)g_bridge_opctx.out_type;
        } else {
            /* setfield pops the value (top of outer stack). */
            int top = lua_gettop(EX);
            if (top < 1 || ai == top) {
                free(hstr);
                bridge_fail_msg(rv, EX, "blyt bridge: setfield needs a value on the stack");
                return;
            }
            int args[2] = {ai, top};
            if (!bridge_pcall(rv, EX, args, 2)) {
                free(hstr);
                return;
            }
            lua_pop(EX, 1); /* consume the original value */
        }
        free(hstr);
        hstr = NULL;
        break;
    }

    case BLYT_LUA_OP_GETI:
    case BLYT_LUA_OP_SETI: {
        int ai;
        if (!bridge_abs_index(EX, (int32_t)a2, &ai)) {
            bridge_fail_msg(rv, EX, "blyt bridge: invalid stack index");
            return;
        }
        g_bridge_opctx.i = (lua_Integer)(int32_t)a3;
        if (opcode == BLYT_LUA_OP_GETI) {
            int args[1] = {ai};
            if (!bridge_pcall(rv, EX, args, 1))
                return;
            val = (uint32_t)g_bridge_opctx.out_type;
        } else {
            int top = lua_gettop(EX);
            if (top < 1 || ai == top) {
                bridge_fail_msg(rv, EX, "blyt bridge: seti needs a value on the stack");
                return;
            }
            int args[2] = {ai, top};
            if (!bridge_pcall(rv, EX, args, 2))
                return;
            lua_pop(EX, 1);
        }
        break;
    }

    case BLYT_LUA_OP_RAWLEN: {
        int ai;
        if (!bridge_abs_index(EX, (int32_t)a2, &ai)) {
            bridge_fail_msg(rv, EX, "blyt bridge: invalid stack index");
            return;
        }
        int t = lua_type(EX, ai);
        if (t != LUA_TTABLE && t != LUA_TSTRING) {
            bridge_fail_msg(rv, EX, "blyt bridge: rawlen on non-table/string");
            return;
        }
        val = (uint32_t)lua_rawlen(EX, ai);
        break;
    }

    case BLYT_LUA_OP_NEXT: {
        int ai;
        if (!bridge_abs_index(EX, (int32_t)a2, &ai)) {
            bridge_fail_msg(rv, EX, "blyt bridge: invalid stack index");
            return;
        }
        if (lua_type(EX, ai) != LUA_TTABLE) {
            bridge_fail_msg(rv, EX, "blyt bridge: next on non-table");
            return;
        }
        int top = lua_gettop(EX);
        if (top < 1 || ai == top) {
            bridge_fail_msg(rv, EX, "blyt bridge: next needs a key on the stack");
            return;
        }
        g_bridge_opctx.out_more = 0;
        int args[2] = {ai, top};
        if (!bridge_pcall(rv, EX, args, 2))
            return;
        /* Outer stack: [.. tbl .. key] (+ key' value if more).  Remove the
         * consumed original key to match real lua_next semantics. */
        if (g_bridge_opctx.out_more) {
            lua_remove(EX, top);
            val = 1;
        } else {
            lua_pop(EX, 1);
            val = 0;
        }
        break;
    }

    case BLYT_LUA_OP_GETGLOBAL:
    case BLYT_LUA_OP_SETGLOBAL: {
        size_t nlen = 0;
        hstr = bridge_read_guest_str(rv, a2, a3, false, &nlen);
        if (!hstr) {
            bridge_fail_msg(rv, EX, "blyt bridge: bad global name");
            return;
        }
        g_bridge_opctx.str = hstr;
        if (opcode == BLYT_LUA_OP_GETGLOBAL) {
            if (!bridge_pcall(rv, EX, NULL, 0)) {
                free(hstr);
                return;
            }
            val = (uint32_t)g_bridge_opctx.out_type;
        } else {
            int top = lua_gettop(EX);
            if (top < 1) {
                free(hstr);
                bridge_fail_msg(rv, EX, "blyt bridge: setglobal needs a value on the stack");
                return;
            }
            int args[1] = {top};
            if (!bridge_pcall(rv, EX, args, 1)) {
                free(hstr);
                return;
            }
            lua_pop(EX, 1);
        }
        free(hstr);
        hstr = NULL;
        break;
    }

    case BLYT_LUA_OP_ERROR: {
        /* Error value = top of the exchange stack (wrapper pushed it). */
        if (lua_gettop(EX) < 1) {
            bridge_fail_msg(rv, EX, "blyt bridge: error with empty stack");
            return;
        }
        bridge_fail(rv);
        return;
    }

    case BLYT_LUA_OP_ERRMSG: {
        size_t mlen = 0;
        hstr = bridge_read_guest_str(rv, a2, a3, false, &mlen);
        lua_checkstack(EX, 1);
        if (hstr) {
            lua_pushlstring(EX, hstr, mlen);
            free(hstr);
        } else {
            lua_pushstring(EX, "blyt bridge: (unreadable error message)");
        }
        bridge_fail(rv);
        return;
    }

    default:
        blyt_tracef(BLYT_TRACE_API, "lua_op unknown opcode %u (trap)", opcode);
        g_run_ctx->ecall_trapped = true;
        rv_halt(rv);
        return;
    }

    bridge_trace_op(opcode, a2, a3, a4, st, val, aux);
    rv_set_reg(rv, rv_reg_a0, st);
    rv_set_reg(rv, rv_reg_a1, val);
    rv_set_reg(rv, rv_reg_a2, aux);
    rv->PC += 4;
}
#endif /* BLYT_LUA */

/* -------------------------------------------------------------------------
 * ECALL handler (ADR-0085: a0=ptr, a1=len, a7=ecall_number)
 * ------------------------------------------------------------------------- */

static const char *buf_op_name(uint32_t op) {
    switch (op) {
    case BUF_OP_GET_F32:
        return "buf_get_f32";
    case BUF_OP_SET_F32:
        return "buf_set_f32";
    case BUF_OP_GET_I32:
        return "buf_get_i32";
    case BUF_OP_SET_I32:
        return "buf_set_i32";
    case BUF_OP_GET_U32:
        return "buf_get_u32";
    case BUF_OP_SET_U32:
        return "buf_set_u32";
    case BUF_OP_GET_I16:
        return "buf_get_i16";
    case BUF_OP_SET_I16:
        return "buf_set_i16";
    case BUF_OP_GET_U16:
        return "buf_get_u16";
    case BUF_OP_SET_U16:
        return "buf_set_u16";
    case BUF_OP_GET_I8:
        return "buf_get_i8";
    case BUF_OP_SET_I8:
        return "buf_set_i8";
    case BUF_OP_GET_U8:
        return "buf_get_u8";
    case BUF_OP_SET_U8:
        return "buf_set_u8";
    case BUF_OP_GET_BOOL:
        return "buf_get_bool";
    case BUF_OP_SET_BOOL:
        return "buf_set_bool";
    case BUF_OP_ALLOC_SLOT:
        return "buf_alloc_slot";
    case BUF_OP_FREE_SLOT:
        return "buf_free_slot";
    case BUF_OP_REF:
        return "buf_ref";
    case BUF_OP_REF_VALID:
        return "buf_ref_valid";
    case BUF_OP_GET_F64:
        return "buf_get_f64";
    case BUF_OP_SET_F64:
        return "buf_set_f64";
    default:
        return "buf_op?";
    }
}

/* Typed value formatting for buf get/set traces: f32 as a float, u32
 * unsigned, everything narrower sign-/zero-extended as the int it is. */
static void buf_op_trace(uint32_t op, uint32_t buf_id, int32_t slot, uint32_t field,
                         uint32_t bits) {
    if (!blyt_trace_enabled(BLYT_TRACE_API))
        return;
    int is_set = (op & 1u) == 0 && op <= BUF_OP_SET_BOOL;
    char vstr[32];
    if (op == BUF_OP_GET_F32 || op == BUF_OP_SET_F32) {
        float f;
        memcpy(&f, &bits, 4);
        snprintf(vstr, sizeof vstr, "%g", (double)f);
    } else if (op == BUF_OP_GET_U32 || op == BUF_OP_SET_U32) {
        snprintf(vstr, sizeof vstr, "%u", bits);
    } else {
        snprintf(vstr, sizeof vstr, "%d", (int32_t)bits);
    }
    if (is_set)
        blyt_tracef(BLYT_TRACE_API, "%s(buf=%u, slot=%d, field=%u, v=%s)", buf_op_name(op), buf_id,
                    slot, field, vstr);
    else
        blyt_tracef(BLYT_TRACE_API, "%s(buf=%u, slot=%d, field=%u) -> %s", buf_op_name(op), buf_id,
                    slot, field, vstr);
}

/* Resolve an incoming resource constant (the baked console-wide handle,
 * ADR-0134) to its table entry: classify it as a cart-bundled RESOURCE and look
 * up the decoded 24-bit id.  Returns NULL for a non-resource kind, a
 * runtime-shipped provenance (no built-in registry yet — ADR-0134 defers it), or
 * an absent id.  The table is keyed by the raw 24-bit id (the `.cart.resource.<id>`
 * section id); only cart-facing values carry the kind/provenance tag. */
static blyt_resource_entry_t *blyt_resolve_resource(blyt_resource_table_t *t, uint32_t handle) {
    if (!blyt_handle_is_resource(handle) ||
        blyt_resource_decode_provenance(handle) != BLYT_RESOURCE_PROV_CART)
        return NULL;
    return blyt_resource_table_find_mut(t, blyt_resource_decode_id(handle));
}

/* Resolve a surface handle to its live registry slot, mirroring the
 * resolve-at-entry pattern (#196): classify the tagged u32, then validate the
 * slot index and generation.  Returns NULL for a non-surface kind, an
 * out-of-range or free slot, or a stale generation (use-after-reap/destroy) —
 * every gfx entry point classifies its handle this way, so a lock-view token or
 * a resource constant can never reach a surface buffer (#205). */
static blyt_surface_slot_t *blyt_resolve_surface(blyt_surface_registry_t *reg, uint32_t handle) {
    if (!blyt_handle_is_surface(handle))
        return NULL;
    uint32_t idx = blyt_dyn_decode_index(handle);
    if (idx >= BLYT_SURFACE_MAX)
        return NULL;
    blyt_surface_slot_t *s = &reg->slots[idx];
    if (!s->in_use || (uint16_t)blyt_dyn_decode_gen(handle) != s->gen)
        return NULL;
    return s;
}

/* Largest off-screen surface edge — an overflow guard, not the size limit: it
 * bounds w*h so the uint32 byte count and the i64-clipped rasterizer
 * intermediates never overflow (8192*8192 = 64 MiB fits both).  The 16 MB budget
 * (#158) is the real size limit and rejects anything that does not fit. */
#define BLYT_SURFACE_MAX_DIM 8192

/* Free one off-screen surface slot: release its buffer, drop its bytes from the
 * budget footprint, and bump the generation so any outstanding handle goes
 * stale.  A no-op on the screen slot (never owned).  Caller republishes the
 * footprint. */
static void blyt_surface_free_slot(blyt_surface_registry_t *reg, blyt_surface_slot_t *s) {
    if (!s->in_use || s->is_screen)
        return;
    if (s->owned && s->pixels) {
        reg->surface_bytes -= (uint32_t)((uint32_t)s->w * (uint32_t)s->h);
        free(s->pixels);
    }
    s->pixels = NULL;
    s->in_use = false;
    s->owned = false;
    s->gen++; /* invalidate handles referencing this slot */
}

/* Flush a locked surface's materialized guest region back into its canonical
 * buffer and clear the lock, bumping the lock generation so the outstanding
 * token goes stale.  A no-op if the slot is not locked. */
static void blyt_surface_flush_lock(blyt_surface_registry_t *reg, blyt_surface_slot_t *s,
                                    memory_t *mem) {
    if (!s->locked)
        return;
    /* A phantom lock (acquired outside draw() in a release cart) never writes
     * back to the canonical buffer — an out-of-phase op is a defined no-op. */
    if (!s->lock_phantom && mem && s->pixels)
        memory_read(mem, s->pixels, s->lock_vaddr, s->lock_len);
    s->locked = false;
    s->lock_phantom = false;
    s->lock_gen++;
    if (reg->active_locks > 0)
        reg->active_locks--;
    if (reg->active_locks == 0)
        reg->lock_bump = 0; /* arena empty — reclaim it */
}

/* Reap every off-screen surface at the frame boundary (draw-scoped lifetime,
 * #205): surfaces are derived per-frame artifacts, so none survive into the next
 * frame.  Any lock the cart forgot to release is force-flushed first (frame
 * boundary force-release).  Screen (slot 0) is force-released but not freed. */
static void blyt_surface_reap_all(blyt_run_ctx_t *ctx, memory_t *mem) {
    blyt_surface_registry_t *reg = &ctx->surfaces;
    for (uint32_t i = 0; i < BLYT_SURFACE_MAX; i++)
        blyt_surface_flush_lock(reg, &reg->slots[i], mem);
    for (uint32_t i = 1; i < BLYT_SURFACE_MAX; i++)
        blyt_surface_free_slot(reg, &reg->slots[i]);
    /* surface_bytes has been decremented per slot; it is now 0. */
    reg->lock_bump = 0;
    reg->active_locks = 0;
    if (mem)
        mem_acct_publish_footprint(ctx, mem);
}

/* Create a blank off-screen surface of w x h, charging w*h against the unified
 * budget.  Returns a fresh SURFACE handle, or BLYT_HANDLE_NONE on invalid size,
 * an exhausted registry, an allocation failure, or an over-budget request. */
static uint32_t blyt_surface_create_slot(blyt_run_ctx_t *ctx, memory_t *mem, int32_t w, int32_t h) {
    blyt_surface_registry_t *reg = &ctx->surfaces;
    if (w <= 0 || h <= 0 || w > BLYT_SURFACE_MAX_DIM || h > BLYT_SURFACE_MAX_DIM)
        return BLYT_HANDLE_NONE;
    uint32_t bytes = (uint32_t)w * (uint32_t)h;

    /* Budget: a surface is non-evictable for the frame, so it charges against the
     * same predicate as the heap + non-evictable resources (#158). */
    uint32_t heap_used = mem_acct_guest_heap_used(ctx, mem);
    if (!blyt_mem_alloc_fits(heap_used, ctx_non_evictable_footprint(ctx), bytes))
        return BLYT_HANDLE_NONE;

    uint32_t idx = 0;
    for (uint32_t i = 1; i < BLYT_SURFACE_MAX; i++) {
        if (!reg->slots[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx == 0) /* registry full (slot 0 is the screen) */
        return BLYT_HANDLE_NONE;

    uint8_t *pixels = (uint8_t *)calloc(bytes, 1); /* blank = palette index 0 */
    if (!pixels)
        return BLYT_HANDLE_NONE;

    blyt_surface_slot_t *s = &reg->slots[idx];
    s->pixels = pixels;
    s->w = w;
    s->h = h;
    s->in_use = true;
    s->is_screen = false;
    s->owned = true;
    /* s->gen carries over from the previous occupant so a stale handle to that
     * occupant does not alias this new surface. */
    reg->surface_bytes += bytes;
    mem_acct_publish_footprint(ctx, mem);
    return blyt_surface_encode(s->gen, idx);
}

/* Resolve a lock-view token to the surface slot it locks: classify kind
 * LOCKVIEW, validate the slot index, that it is actually locked, and that the
 * token's generation matches (a released token has a bumped lock_gen → stale).
 * Returns NULL otherwise. */
static blyt_surface_slot_t *blyt_resolve_lockview(blyt_surface_registry_t *reg, uint32_t token) {
    if (!blyt_handle_is_lockview(token))
        return NULL;
    uint32_t idx = blyt_dyn_decode_index(token);
    if (idx >= BLYT_SURFACE_MAX)
        return NULL;
    blyt_surface_slot_t *s = &reg->slots[idx];
    if (!s->locked || (uint16_t)blyt_dyn_decode_gen(token) != s->lock_gen)
        return NULL;
    return s;
}

/* Zero `len` bytes of guest memory at `vaddr` (read-as-cleared materialization,
 * #205).  Chunked from a small static zero buffer so an arbitrarily large region
 * needs no host allocation. */
static void guest_memzero(memory_t *mem, uint32_t vaddr, uint32_t len) {
    static const uint8_t zeros[4096] = {0};
    uint32_t off = 0;
    while (off < len) {
        uint32_t n = len - off;
        if (n > sizeof(zeros))
            n = sizeof(zeros);
        memory_write(mem, vaddr + off, zeros, n);
        off += n;
    }
}

/* Acquire an exclusive tier-2 lock on the surface `handle`: carve a per-lock
 * region from the VA-gap arena, materialize the buffer into it, and write the
 * guest blyt_lock_t (pixels/stride/w/h/token) to out_vaddr.  Normally the
 * canonical buffer is copied in; when `clear_only` (an out-of-phase acquire in a
 * release cart) the region is materialized as cleared instead and the lock is
 * marked phantom so it never flushes back — the draw()-only read-as-cleared
 * semantics.  Returns 1 on success, 0 on failure (unresolvable surface, already
 * locked, or arena exhausted — the latter impossible given gap > budget). */
static int blyt_surface_acquire_lock(blyt_run_ctx_t *ctx, memory_t *mem, uint32_t handle,
                                     uint32_t out_vaddr, bool clear_only) {
    blyt_surface_registry_t *reg = &ctx->surfaces;
    blyt_surface_slot_t *s = blyt_resolve_surface(reg, handle);
    uint8_t lockbuf[20]; /* blyt_lock_t on ilp32: pixels,stride,w,h,token */
    if (!s || s->locked) {
        memset(lockbuf, 0, sizeof(lockbuf)); /* token = NONE, pixels = 0 */
        memory_write(mem, out_vaddr, lockbuf, sizeof(lockbuf));
        return 0;
    }
    uint32_t len = (uint32_t)((uint32_t)s->w * (uint32_t)s->h);
    uint32_t aligned = (len + 3u) & ~3u;
    if ((uint64_t)reg->lock_bump + aligned > BLYT_SURFACE_LOCK_ARENA_SIZE) {
        memset(lockbuf, 0, sizeof(lockbuf));
        memory_write(mem, out_vaddr, lockbuf, sizeof(lockbuf));
        return 0;
    }
    uint32_t vaddr = BLYT_SURFACE_LOCK_ARENA_BASE + reg->lock_bump;
    reg->lock_bump += aligned;
    reg->active_locks++;

    /* Copy-in: the canonical buffer is the single source of truth, so every
     * acquire reads it (coherence claim, #205).  A phantom (out-of-phase) lock
     * reads as cleared instead — it must not leak the real surface contents. */
    if (clear_only)
        guest_memzero(mem, vaddr, len);
    else
        memory_write(mem, vaddr, s->pixels, len);

    s->locked = true;
    s->lock_phantom = clear_only;
    s->lock_vaddr = vaddr;
    s->lock_len = len;
    uint32_t token = blyt_lockview_encode(s->lock_gen, blyt_dyn_decode_index(handle));

    write_u32_le(lockbuf + 0, vaddr); /* pixels */
    write_u32_le(lockbuf + 4, (uint32_t)s->w); /* stride */
    write_u32_le(lockbuf + 8, (uint32_t)s->w); /* w */
    write_u32_le(lockbuf + 12, (uint32_t)s->h); /* h */
    write_u32_le(lockbuf + 16, token); /* token */
    memory_write(mem, out_vaddr, lockbuf, sizeof(lockbuf));
    return 1;
}

/* Release a tier-2 lock by its token: flush (copy-out) the materialized region
 * back into the canonical buffer and invalidate the token.  A no-op on a stale
 * or foreign token.  Returns the released slot (for cart_has_drawn on screen),
 * or NULL. */
static blyt_surface_slot_t *blyt_surface_release_lock(blyt_run_ctx_t *ctx, memory_t *mem,
                                                      uint32_t token) {
    blyt_surface_slot_t *s = blyt_resolve_lockview(&ctx->surfaces, token);
    if (!s)
        return NULL;
    blyt_surface_flush_lock(&ctx->surfaces, s, mem);
    return s;
}

/* Draw()-only phase gate for a surface access op (#205).  Returns true when the
 * op may proceed (phase is DRAW).  Outside draw() it returns false and:
 *   - in a debug cart, raises a dev fault and halts the emulator (*halted set);
 *     the caller must return immediately without advancing PC.
 *   - in a release cart, leaves *halted false; the caller performs its defined
 *     no-op (writes drop, acquire reads as cleared) and advances PC normally.
 * A NULL ctx behaves like the release no-op. */
static bool blyt_surface_phase_gate(riscv_t *rv, bool *halted) {
    *halted = false;
    blyt_run_ctx_t *ctx = g_run_ctx;
    if (!ctx || ctx->phase == BLYT_PHASE_DRAW)
        return ctx != NULL;
    if (ctx->cart_is_debug) {
        if (ctx->log_fn)
            ctx->log_fn("blyt: surface access outside draw() — debug build trap\n");
        ctx->dev_fault = true;
        rv_halt(rv);
        *halted = true;
    }
    return false;
}

static void blyt_ecall_handler(riscv_t *rv) {
    uint32_t num = rv_get_reg(rv, rv_reg_a7);

    switch (num) {
    case BLYT_ECALL_EXIT: {
        uint32_t code = rv_get_reg(rv, rv_reg_a0);
        blyt_tracef(BLYT_TRACE_API, "exit(code=%u)", code);
        rv_halt(rv);
        if (code != 0 && g_run_ctx)
            g_run_ctx->ecall_aborted = true;
        return;
    }

    case BLYT_ECALL_CONSOLE_DEBUG: {
        uint32_t vaddr = rv_get_reg(rv, rv_reg_a0);
        uint32_t len = rv_get_reg(rv, rv_reg_a1);
        vm_attr_t *attr = PRIV(rv);
        memory_t *mem = attr->mem;

        char buf[4096];
        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        uint32_t i;
        for (i = 0; i < len; i++) {
            if (!GUEST_RAM_CONTAINS(mem, vaddr + i, 1))
                break;
            uint8_t c;
            memory_read(mem, &c, vaddr + i, 1);
            buf[i] = (char)c;
        }
        buf[i] = '\0';

        if (blyt_trace_enabled(BLYT_TRACE_API)) {
            uint32_t tl = i;
            while (tl > 0 && (buf[tl - 1] == '\n' || buf[tl - 1] == '\r'))
                tl--;
            blyt_tracef(BLYT_TRACE_API, "console_debug(\"%.*s\")", (int)tl, buf);
        }

        if (g_run_ctx && g_run_ctx->log_fn)
            g_run_ctx->log_fn(buf);

        rv->PC += 4;
        return;
    }

    /* Tier-1 surface ops (ADR-0052/0086/0008, #188 / #195 / #205).  Host-side
     * rasterization into the destination surface's canonical buffer via the
     * shared integer core; a0 carries the destination surface handle.  A draw
     * into BLYT_SCREEN flips cart_has_drawn (displacing the PM5544 test card);
     * off-screen surface draws do not.  An unresolvable handle (wrong kind /
     * stale generation) is a defined no-op. */
    case BLYT_ECALL_SURFACE_CLEAR: {
        uint32_t dst = rv_get_reg(rv, rv_reg_a0);
        uint32_t color = rv_get_reg(rv, rv_reg_a1);
        blyt_tracef(BLYT_TRACE_API, "surface_clear(dst=0x%08x, color=%u)", dst, color);
        bool halted;
        if (!blyt_surface_phase_gate(rv, &halted)) {
            rv->PC += halted ? 0 : 4;
            return;
        }
        blyt_surface_slot_t *s = g_run_ctx ? blyt_resolve_surface(&g_run_ctx->surfaces, dst) : NULL;
        if (s) {
            blyt_raster_clear(s->pixels, s->w, s->w, s->h, (uint8_t)color);
            if (s->is_screen && g_run_ctx->cart_has_drawn)
                *g_run_ctx->cart_has_drawn = true;
        }
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_SURFACE_PIXEL: {
        uint32_t dst = rv_get_reg(rv, rv_reg_a0);
        int32_t x = (int32_t)rv_get_reg(rv, rv_reg_a1);
        int32_t y = (int32_t)rv_get_reg(rv, rv_reg_a2);
        uint32_t color = rv_get_reg(rv, rv_reg_a3);
        blyt_tracef(BLYT_TRACE_API, "surface_pixel(dst=0x%08x, x=%d, y=%d, color=%u)", dst, x, y,
                    color);
        bool halted;
        if (!blyt_surface_phase_gate(rv, &halted)) {
            rv->PC += halted ? 0 : 4;
            return;
        }
        blyt_surface_slot_t *s = g_run_ctx ? blyt_resolve_surface(&g_run_ctx->surfaces, dst) : NULL;
        if (s) {
            blyt_raster_pixel(s->pixels, s->w, s->w, s->h, x, y, (uint8_t)color);
            if (s->is_screen && g_run_ctx->cart_has_drawn)
                *g_run_ctx->cart_has_drawn = true;
        }
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_SURFACE_RECT_FILL: {
        uint32_t dst = rv_get_reg(rv, rv_reg_a0);
        int32_t x = (int32_t)rv_get_reg(rv, rv_reg_a1);
        int32_t y = (int32_t)rv_get_reg(rv, rv_reg_a2);
        int32_t w = (int32_t)rv_get_reg(rv, rv_reg_a3);
        int32_t h = (int32_t)rv_get_reg(rv, rv_reg_a4);
        uint32_t color = rv_get_reg(rv, rv_reg_a5);
        blyt_tracef(BLYT_TRACE_API,
                    "surface_rect_fill(dst=0x%08x, x=%d, y=%d, w=%d, h=%d, color=%u)", dst, x, y, w,
                    h, color);
        bool halted;
        if (!blyt_surface_phase_gate(rv, &halted)) {
            rv->PC += halted ? 0 : 4;
            return;
        }
        blyt_surface_slot_t *s = g_run_ctx ? blyt_resolve_surface(&g_run_ctx->surfaces, dst) : NULL;
        if (s) {
            blyt_raster_rect_fill(s->pixels, s->w, s->w, s->h, x, y, w, h, (uint8_t)color);
            if (s->is_screen && g_run_ctx->cart_has_drawn)
                *g_run_ctx->cart_has_drawn = true;
        }
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_SURFACE_LINE: {
        uint32_t dst = rv_get_reg(rv, rv_reg_a0);
        int32_t x0 = (int32_t)rv_get_reg(rv, rv_reg_a1);
        int32_t y0 = (int32_t)rv_get_reg(rv, rv_reg_a2);
        int32_t x1 = (int32_t)rv_get_reg(rv, rv_reg_a3);
        int32_t y1 = (int32_t)rv_get_reg(rv, rv_reg_a4);
        uint32_t color = rv_get_reg(rv, rv_reg_a5);
        blyt_tracef(BLYT_TRACE_API,
                    "surface_line(dst=0x%08x, x0=%d, y0=%d, x1=%d, y1=%d, color=%u)", dst, x0, y0,
                    x1, y1, color);
        bool halted;
        if (!blyt_surface_phase_gate(rv, &halted)) {
            rv->PC += halted ? 0 : 4;
            return;
        }
        blyt_surface_slot_t *s = g_run_ctx ? blyt_resolve_surface(&g_run_ctx->surfaces, dst) : NULL;
        if (s) {
            blyt_raster_line(s->pixels, s->w, s->w, s->h, x0, y0, x1, y1, (uint8_t)color);
            if (s->is_screen && g_run_ctx->cart_has_drawn)
                *g_run_ctx->cart_has_drawn = true;
        }
        rv->PC += 4;
        return;
    }

    /* Surface lifecycle + blit (tier-1, #205). */
    case BLYT_ECALL_SURFACE_CREATE: {
        int32_t w = (int32_t)rv_get_reg(rv, rv_reg_a0);
        int32_t h = (int32_t)rv_get_reg(rv, rv_reg_a1);
        uint32_t handle = BLYT_HANDLE_NONE;
        bool halted;
        if (!blyt_surface_phase_gate(rv, &halted)) {
            /* Out of phase: debug halts; release returns NONE (defined no-op). */
            if (halted)
                return;
            blyt_tracef(BLYT_TRACE_API, "surface_create(w=%d, h=%d) -> NONE (outside draw)", w, h);
            rv_set_reg(rv, rv_reg_a0, BLYT_HANDLE_NONE);
            rv->PC += 4;
            return;
        }
        handle = blyt_surface_create_slot(g_run_ctx, PRIV(rv)->mem, w, h);
        blyt_tracef(BLYT_TRACE_API, "surface_create(w=%d, h=%d) -> 0x%08x", w, h, handle);
        rv_set_reg(rv, rv_reg_a0, handle);
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_SURFACE_DESTROY: {
        uint32_t handle = rv_get_reg(rv, rv_reg_a0);
        blyt_tracef(BLYT_TRACE_API, "surface_destroy(0x%08x)", handle);
        blyt_surface_slot_t *s =
            g_run_ctx ? blyt_resolve_surface(&g_run_ctx->surfaces, handle) : NULL;
        if (s && !s->is_screen) {
            blyt_surface_free_slot(&g_run_ctx->surfaces, s);
            mem_acct_publish_footprint(g_run_ctx, PRIV(rv)->mem);
        }
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_SURFACE_BLIT: {
        uint32_t dst = rv_get_reg(rv, rv_reg_a0);
        uint32_t src = rv_get_reg(rv, rv_reg_a1);
        int32_t x = (int32_t)rv_get_reg(rv, rv_reg_a2);
        int32_t y = (int32_t)rv_get_reg(rv, rv_reg_a3);
        blyt_tracef(BLYT_TRACE_API, "surface_blit(dst=0x%08x, src=0x%08x, x=%d, y=%d)", dst, src, x,
                    y);
        bool halted;
        if (!blyt_surface_phase_gate(rv, &halted)) {
            rv->PC += halted ? 0 : 4;
            return;
        }
        blyt_surface_slot_t *ds =
            g_run_ctx ? blyt_resolve_surface(&g_run_ctx->surfaces, dst) : NULL;
        blyt_surface_slot_t *ss =
            g_run_ctx ? blyt_resolve_surface(&g_run_ctx->surfaces, src) : NULL;
        if (ds && ss) {
            blyt_raster_blit(ds->pixels, ds->w, ds->w, ds->h, ss->pixels, ss->w, ss->w, ss->h, x,
                             y);
            if (ds->is_screen && g_run_ctx->cart_has_drawn)
                *g_run_ctx->cart_has_drawn = true;
        }
        rv->PC += 4;
        return;
    }

    /* Tier-2 per-pixel lock (#205). */
    case BLYT_ECALL_SURFACE_ACQUIRE: {
        uint32_t surface = rv_get_reg(rv, rv_reg_a0);
        uint32_t out_vaddr = rv_get_reg(rv, rv_reg_a1);
        /* Out of phase: debug halts; release materializes a cleared (phantom)
         * lock so a read sees zeros, not the real surface (read-as-cleared). */
        bool halted;
        bool clear_only = !blyt_surface_phase_gate(rv, &halted);
        if (halted)
            return;
        int ok = 0;
        if (g_run_ctx)
            ok =
                blyt_surface_acquire_lock(g_run_ctx, PRIV(rv)->mem, surface, out_vaddr, clear_only);
        blyt_tracef(BLYT_TRACE_API, "surface_acquire(0x%08x)%s -> %s", surface,
                    clear_only ? " [cleared]" : "", ok ? "ok" : "fail");
        rv_set_reg(rv, rv_reg_a0, (uint32_t)ok);
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_SURFACE_RELEASE: {
        uint32_t token = rv_get_reg(rv, rv_reg_a0);
        blyt_tracef(BLYT_TRACE_API, "surface_release(0x%08x)", token);
        if (g_run_ctx) {
            blyt_surface_slot_t *s = blyt_surface_release_lock(g_run_ctx, PRIV(rv)->mem, token);
            if (s && s->is_screen && g_run_ctx->cart_has_drawn)
                *g_run_ctx->cart_has_drawn = true;
        }
        rv->PC += 4;
        return;
    }

    /* Raw framebuffer acquire/present (issue #188 / Spike X, Q1).  ACQUIRE hands
     * the cart the guest VA of the runtime-reserved framebuffer region so it can
     * write palette indices directly; PRESENT copies that region into
     * session->pixels[] and flips cart_has_drawn. */
    case BLYT_ECALL_GFX_ACQUIRE: {
        blyt_tracef(BLYT_TRACE_API, "gfx_acquire() -> 0x%08x", BLYT_GFX_FB_BASE);
        rv_set_reg(rv, rv_reg_a0, BLYT_GFX_FB_BASE);
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_GFX_PRESENT: {
        blyt_tracef(BLYT_TRACE_API, "gfx_present()");
        if (g_run_ctx && g_run_ctx->fb) {
            vm_attr_t *attr = PRIV(rv);
            memory_read(attr->mem, g_run_ctx->fb, BLYT_GFX_FB_BASE,
                        (uint32_t)(BLYT_FRAME_W * BLYT_FRAME_H));
            if (g_run_ctx->cart_has_drawn)
                *g_run_ctx->cart_has_drawn = true;
        }
        rv->PC += 4;
        return;
    }

    /* Resource lifecycle ECALLs (ADR-0027, #123).  Result codes are
     * blyt_result_t values (BLYT_OK=0, BLYT_ERR_INVALID_ARG=1, BLYT_ERR_IO=3,
     * BLYT_ERR_NOT_FOUND=4); the host does not include the guest blyt.h. */
    case BLYT_ECALL_RESOURCE_PIN: {
        /* a0=id (in) -> result (out); a1=out_ptr vaddr, a2=out_size vaddr.
         * Copies the bytes into the per-frame guest scratch, returns the guest
         * pointer + length, and bumps the pin count. */
        uint32_t handle = rv_get_reg(rv, rv_reg_a0); /* baked resource constant (ADR-0134) */
        uint32_t out_ptr_vaddr = rv_get_reg(rv, rv_reg_a1);
        uint32_t out_size_vaddr = rv_get_reg(rv, rv_reg_a2);
        vm_attr_t *attr = PRIV(rv);
        memory_t *mem = attr->mem;

        blyt_resource_entry_t *e =
            g_run_ctx ? blyt_resolve_resource(&g_run_ctx->resources, handle) : NULL;
        blyt_tracef(BLYT_TRACE_API, "resource_pin(h=0x%08x id=%u) -> %s len=%zu", handle,
                    blyt_resource_decode_id(handle), e ? "ok" : "NOT_FOUND",
                    e ? e->len : (size_t)0);

        uint32_t zero = 0;
        if (!e) {
            if (out_ptr_vaddr)
                memory_write(mem, out_ptr_vaddr, (const uint8_t *)&zero, 4);
            rv_set_reg(rv, rv_reg_a0, 4 /* BLYT_ERR_NOT_FOUND */);
            rv->PC += 4;
            return;
        }

        /* Unified-budget gate (#158): pinning a so-far-evictable resource adds
         * its bytes to the non-evictable footprint. Fail with BUFFER_FULL if that
         * would exceed the 16 MB budget (heap + footprint). Eviction can't help —
         * evictable cache is not in the predicate — so it is not attempted. */
        int pin_was_evictable = blyt_rl_is_evictable(&e->rl);
        if (!mem_acct_reference_fits(g_run_ctx, mem, e, pin_was_evictable)) {
            if (out_ptr_vaddr)
                memory_write(mem, out_ptr_vaddr, (const uint8_t *)&zero, 4);
            rv_set_reg(rv, rv_reg_a0, 2 /* BLYT_ERR_BUFFER_FULL: over budget */);
            rv->PC += 4;
            return;
        }

        /* Materialize the bytes: a compressed (zstd) resource is decompressed
         * lazily on first pin into an owned buffer, cached thereafter (#157).
         * Uncompressed resources return their zero-copy map alias. */
        const uint8_t *bytes = blyt_resource_entry_data(e);
        if (!bytes && e->len) {
            if (out_ptr_vaddr)
                memory_write(mem, out_ptr_vaddr, (const uint8_t *)&zero, 4);
            rv_set_reg(rv, rv_reg_a0, 3 /* BLYT_ERR_IO: decode failed */);
            rv->PC += 4;
            return;
        }

        /* Bump-allocate room in the guest scratch region; wrap to the start if a
         * single fetch would overflow it.  out_size is authoritative — the bytes
         * are raw (no NUL terminator; the text_get helper adds its own). */
        uint32_t off = g_run_ctx->resource_scratch_off;
        uint32_t need = (uint32_t)e->len;
        if (need > BLYT_RESOURCE_SCRATCH_SIZE) {
            if (out_ptr_vaddr)
                memory_write(mem, out_ptr_vaddr, (const uint8_t *)&zero, 4);
            rv_set_reg(rv, rv_reg_a0, 3 /* BLYT_ERR_IO: larger than scratch */);
            rv->PC += 4;
            return;
        }
        if (off + need > BLYT_RESOURCE_SCRATCH_SIZE)
            off = 0;
        uint32_t gptr = BLYT_RESOURCE_SCRATCH_BASE + off;
        if (need)
            memory_write(mem, gptr, bytes, need);
        g_run_ctx->resource_scratch_off = off + need;

        blyt_rl_pin(&e->rl);
        blyt_resource_table_touch(&g_run_ctx->resources, e); /* recency for LRU (#158) */
        mem_acct_publish_footprint(g_run_ctx, mem); /* footprint grew; bound cache */
        if (out_ptr_vaddr)
            memory_write(mem, out_ptr_vaddr, (const uint8_t *)&gptr, 4);
        if (out_size_vaddr) {
            uint32_t l = (uint32_t)e->len; /* size_t is 4 bytes on ilp32 */
            memory_write(mem, out_size_vaddr, (const uint8_t *)&l, 4);
        }
        rv_set_reg(rv, rv_reg_a0, 0 /* BLYT_OK */);
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_RESOURCE_UNPIN: {
        /* a0=resource constant -> result. */
        uint32_t handle = rv_get_reg(rv, rv_reg_a0);
        blyt_resource_entry_t *e =
            g_run_ctx ? blyt_resolve_resource(&g_run_ctx->resources, handle) : NULL;
        uint32_t res;
        if (!e) {
            res = 4u; /* NOT_FOUND */
        } else if (blyt_rl_unpin(&e->rl)) {
            /* The pin dropped; if the entry is now unreferenced its bytes leave
             * the footprint — republish so the guest sees the freed budget (#158). */
            vm_attr_t *attr = PRIV(rv);
            mem_acct_publish_footprint(g_run_ctx, attr->mem);
            res = 0u;
        } else {
            res = 1u; /* INVALID_ARG: nothing pinned */
        }
        blyt_tracef(BLYT_TRACE_API, "resource_unpin(id=%u) -> %u", blyt_resource_decode_id(handle),
                    res);
        rv_set_reg(rv, rv_reg_a0, res);
        rv->PC += 4;
        return;
    }

        /* RESOURCE_LOAD (63) / RESOURCE_RELEASE (64) retired: the cart-held residency
         * handle is gone (ADR-0134 / #196) — resources are referenced by their baked
         * constant directly and the runtime owns residency (demand-load + LRU evict
         * #137 + persistent #160). The ECALL numbers stay reserved (not renumbered),
         * as RESOURCE_TEXT_GET (60) is. */

    case BLYT_ECALL_MEM_RESOURCES: {
        /* a0=out resources array vaddr (or 0); a1=cap pairs. Enumerates the
         * resident resources — currently-loaded (load_count>0) AND persistent
         * (#160: resident from frame 0 with load_count==0) — writing up to `cap`
         * {u32 id, u32 size} pairs, and returns the full count. The on-demand half
         * of the introspection API (the scalars are read from the accounting
         * block, no ECALL; #159, ADR-0029). */
        uint32_t out_res_vaddr = rv_get_reg(rv, rv_reg_a0);
        uint32_t cap = rv_get_reg(rv, rv_reg_a1);
        vm_attr_t *attr = PRIV(rv);
        memory_t *mem = attr->mem;

        uint32_t loaded = 0;
        if (g_run_ctx) {
            const blyt_resource_table_t *t = &g_run_ctx->resources;
            for (size_t i = 0; i < t->count; i++) {
                const blyt_resource_entry_t *e = &t->entries[i];
                /* Resident working set (ADR-0134, advisory): persistent (#160), a
                 * decompressed entry currently in the cache (e->owned), or an
                 * uncompressed zero-copy entry the cart has accessed
                 * (last_access > 0). With the load handle gone, residency is the
                 * advisory cache, not a cart-held count; the id is reported as the
                 * baked constant so it matches the cart's R_<NAME>. */
                if (!(e->persistent || e->owned != NULL ||
                      (e->algo == BLYT_RES_ALGO_NONE && e->last_access > 0)))
                    continue;
                if (out_res_vaddr && loaded < cap) {
                    uint8_t pair[8];
                    write_u32_le(pair + 0, blyt_resource_encode(e->id, BLYT_RESOURCE_PROV_CART));
                    write_u32_le(pair + 4, (uint32_t)e->len);
                    memory_write(mem, out_res_vaddr + loaded * 8u, pair, sizeof(pair));
                }
                loaded++;
            }
        }

        blyt_tracef(BLYT_TRACE_API, "mem_resources() -> loaded=%u", loaded);
        rv_set_reg(rv, rv_reg_a0, loaded);
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_PHASE: {
        /* Lifecycle phase signal (#205): the guest blyt_main tells the runtime
         * which callback it is entering so surface access can be draw()-only. */
        uint32_t phase = rv_get_reg(rv, rv_reg_a0);
        blyt_tracef(BLYT_TRACE_API, "phase(%u)", phase);
        if (g_run_ctx)
            g_run_ctx->phase = (int32_t)phase;
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_FRAME_DONE: {
        blyt_tracef(BLYT_TRACE_API, "frame_done()");
        rv->PC += 4;
        /* Enforce FP determinism (ADR-0007): frm must be RNE (0) at every
         * frame boundary.  A non-zero frm means cart/library code called
         * fesetround() or modified frm and did not restore it, which would
         * cause FP results to diverge across emulator runs and native. */
        uint32_t frm = (rv->csr_fcsr >> 5) & 0x7u;
        if (frm != 0u) {
#ifndef NDEBUG
            fprintf(stderr,
                    "blyt: WARNING: cart set non-default FP rounding mode "
                    "(frm=%u); results may be non-deterministic\n",
                    (unsigned)frm);
#else
            fprintf(stderr, "blyt: cart set non-default FP rounding mode; "
                            "aborting for determinism\n");
            if (g_run_ctx)
                g_run_ctx->ecall_aborted = true;
            rv_halt(rv);
            return;
#endif
        }
        /* Reset frm to RNE and clear accumulated fflags for the next frame. */
        rv->csr_fcsr = 0;
        if (g_run_ctx)
            g_run_ctx->frame_done = true;
        /* Halt the emulator so rv_step() returns immediately.  Without this,
         * rv_step() continues running the next frame's instructions within the
         * same cycle budget (BLYT_CYCLE_PER_STEP), causing multiple game frames
         * to execute per outer-loop iteration while only one testcard draw
         * occurs.  blyt_session_run_frame() clears halt at the start of each
         * new frame. */
        rv_halt(rv);
        return;
    }

    case BLYT_ECALL_DAP_HOOK: {
        /* Probe (src=0, line<0) or line event from libblyt32lua master hook.
         * Always handled — even without BLYT_DAP — so probe returns 0 (inactive)
         * rather than trapping when the host was not built with DAP support. */
        if (!g_run_ctx || !g_run_ctx->dap_enabled) {
            rv_set_reg(rv, rv_reg_a0, 0);
            rv->PC += 4;
            return;
        }
#ifdef BLYT_DAP
        {
            uint32_t src_vaddr = rv_get_reg(rv, rv_reg_a0);
            uint32_t src_len = rv_get_reg(rv, rv_reg_a1);
            int32_t line = (int32_t)rv_get_reg(rv, rv_reg_a2);
            int32_t depth = (int32_t)rv_get_reg(rv, rv_reg_a3);

            /* Probe call from blyt_dap_active(): src=0 or line<0 */
            if (src_vaddr == 0 || line < 0) {
                blyt_tracef(BLYT_TRACE_API, "dap_hook(probe) -> 1");
                rv_set_reg(rv, rv_reg_a0, 1);
                rv->PC += 4;
                return;
            }

            vm_attr_t *attr = PRIV(rv);
            memory_t *mem = attr->mem;
            char src_buf[4096];
            if (src_len >= sizeof(src_buf))
                src_len = (uint32_t)(sizeof(src_buf) - 1);
            uint32_t ri;
            for (ri = 0; ri < src_len; ri++) {
                if (!GUEST_RAM_CONTAINS(mem, src_vaddr + ri, 1))
                    break;
                uint8_t c;
                memory_read(mem, &c, src_vaddr + ri, 1);
                src_buf[ri] = (char)c;
            }
            src_buf[ri] = '\0';

            int cmd = fc_dap_check_hook_line(src_buf, line, depth);
            blyt_tracef(BLYT_TRACE_API, "dap_hook(%s:%d, depth=%d) -> %d", src_buf, line, depth,
                        cmd);
            rv_set_reg(rv, rv_reg_a0, (uint32_t)cmd);
        }
#else
        rv_set_reg(rv, rv_reg_a0, 0);
#endif
        rv->PC += 4;
        return;
    }

#ifdef BLYT_DAP
    case BLYT_ECALL_DAP_SEND: {
        /* Guest sends a DAP JSON message; forward to connected TCP client. */
        if (!g_run_ctx || !g_run_ctx->dap_enabled) {
            rv->PC += 4;
            return;
        }
        uint32_t vaddr = rv_get_reg(rv, rv_reg_a0);
        uint32_t vlen = rv_get_reg(rv, rv_reg_a1);
        vm_attr_t *attr = PRIV(rv);
        memory_t *mem = attr->mem;
        if (vlen > 65535u)
            vlen = 65535u;
        char *json = malloc((size_t)vlen + 1);
        if (json) {
            uint32_t i;
            for (i = 0; i < vlen; i++) {
                if (!GUEST_RAM_CONTAINS(mem, vaddr + i, 1))
                    break;
                uint8_t c;
                memory_read(mem, &c, vaddr + i, 1);
                json[i] = (char)c;
            }
            json[i] = '\0';
            blyt_tracef(BLYT_TRACE_API, "dap_send(len=%u)", i);
            fc_dap_host_send(json, i);
            free(json);
        }
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_DAP_RECV: {
        /* Block until VS Code sends an inspection command; write JSON to guest. */
        if (!g_run_ctx || !g_run_ctx->dap_enabled) {
            rv_set_reg(rv, rv_reg_a0, 0);
            rv->PC += 4;
            return;
        }
        uint32_t buf_vaddr = rv_get_reg(rv, rv_reg_a0);
        uint32_t max_len = rv_get_reg(rv, rv_reg_a1);
        vm_attr_t *attr = PRIV(rv);
        memory_t *mem = attr->mem;
        if (max_len > 65535u)
            max_len = 65535u;
        char *buf = malloc((size_t)max_len + 1);
        int len = 0;
        if (buf) {
            len = fc_dap_host_recv(buf, (size_t)max_len);
            if (len > 0) {
                uint32_t wr = (uint32_t)len < max_len ? (uint32_t)len : max_len;
                memory_write(mem, buf_vaddr, (const uint8_t *)buf, wr);
            }
            free(buf);
        }
        blyt_tracef(BLYT_TRACE_API, "dap_recv -> len=%d", len);
        rv_set_reg(rv, rv_reg_a0, (uint32_t)len);
        rv->PC += 4;
        return;
    }

    case 6: { /* BLYT_ECALL_DAP_EXCEPTION: guest reports a caught Lua exception. */
        if (!g_run_ctx || !g_run_ctx->dap_enabled) {
            rv_set_reg(rv, rv_reg_a0, 0);
            rv->PC += 4;
            return;
        }
        uint32_t msg_vaddr = rv_get_reg(rv, rv_reg_a0);
        uint32_t msg_len = rv_get_reg(rv, rv_reg_a1);
        int is_uncaught = (int)rv_get_reg(rv, rv_reg_a2);
        vm_attr_t *attr6 = PRIV(rv);
        memory_t *mem6 = attr6->mem;
        if (msg_len > 255u)
            msg_len = 255u;
        char msg_buf[256];
        uint32_t mi;
        for (mi = 0; mi < msg_len; mi++) {
            if (!GUEST_RAM_CONTAINS(mem6, msg_vaddr + mi, 1))
                break;
            uint8_t c6;
            memory_read(mem6, &c6, msg_vaddr + mi, 1);
            msg_buf[mi] = (char)c6;
        }
        msg_buf[mi] = '\0';
        int r6 = fc_dap_on_exception(msg_buf, is_uncaught);
        blyt_tracef(BLYT_TRACE_API, "dap_exception(\"%s\", uncaught=%d) -> %d", msg_buf,
                    is_uncaught, r6);
        rv_set_reg(rv, rv_reg_a0, (uint32_t)r6);
        rv->PC += 4;
        return;
    }

    case 7: { /* BLYT_ECALL_DAP_GET_CONDITION: copy pending condition to guest. */
        if (!g_run_ctx || !g_run_ctx->dap_enabled) {
            rv_set_reg(rv, rv_reg_a0, 0);
            rv->PC += 4;
            return;
        }
        uint32_t buf7_vaddr = rv_get_reg(rv, rv_reg_a0);
        uint32_t buf7_len = rv_get_reg(rv, rv_reg_a1);
        if (buf7_len > 255u)
            buf7_len = 255u;
        char cond_buf[256];
        int clen = fc_dap_get_condition(cond_buf, (size_t)buf7_len);
        blyt_tracef(BLYT_TRACE_API, "dap_get_condition -> len=%d", clen);
        if (clen > 0) {
            vm_attr_t *attr7 = PRIV(rv);
            memory_t *mem7 = attr7->mem;
            memory_write(mem7, buf7_vaddr, (const uint8_t *)cond_buf, (uint32_t)clen);
        }
        rv_set_reg(rv, rv_reg_a0, (uint32_t)clen);
        rv->PC += 4;
        return;
    }

    case 8: { /* BLYT_ECALL_DAP_CONDITION_RESULT: guest reports condition eval. */
        if (!g_run_ctx || !g_run_ctx->dap_enabled) {
            rv_set_reg(rv, rv_reg_a0, 0);
            rv->PC += 4;
            return;
        }
        int result8 = (int)rv_get_reg(rv, rv_reg_a0);
        int r8 = fc_dap_on_condition_result(result8);
        blyt_tracef(BLYT_TRACE_API, "dap_condition_result(%d) -> %d", result8, r8);
        rv_set_reg(rv, rv_reg_a0, (uint32_t)r8);
        rv->PC += 4;
        return;
    }
#endif /* BLYT_DAP */

#ifdef BLYT_LUA
    case BLYT_ECALL_LUA_OP: {
        bridge_lua_op(rv);
        return;
    }
#endif

    case BLYT_ECALL_HOST_FN_RETURN: {
        /* Guest function called via blyt_session_begin_fn_call() returned.
         * a0 holds the return value (set by the C function's ret instruction).
         * Signal the host; blyt_session_fn_return_value() will read a0. */
        blyt_tracef(BLYT_TRACE_API, "host_fn_return(a0=0x%x)", rv_get_reg(rv, rv_reg_a0));
        rv->PC += 4;
        if (g_run_ctx)
            g_run_ctx->fn_return_done = true;
        rv_halt(rv);
        return;
    }

    case BLYT_ECALL_BUF_OP: {
        /* State buffer typed get/set + slot management (ADR-0009, ADR-0057,
         * ADR-0058, ADR-0096).
         * a0=sub-opcode, a1=buf_h (1-based), a2=slot, a3=field_h, a4=value_bits */
        if (!g_run_ctx || !g_run_ctx->state_ctx) {
            /* REF/REF_VALID must report "no ref" / "invalid" here: the legacy
             * -1 would read back as a valid-looking packed ref / true. */
            uint32_t no_ctx_op = rv_get_reg(rv, rv_reg_a0);
            uint32_t no_ctx_ret =
                (no_ctx_op == BUF_OP_REF || no_ctx_op == BUF_OP_REF_VALID) ? 0 : (uint32_t)-1;
            rv_set_reg(rv, rv_reg_a0, no_ctx_ret);
            rv->PC += 4;
            return;
        }
        blyt_state_ctx_t *sc = g_run_ctx->state_ctx;
        uint32_t op = rv_get_reg(rv, rv_reg_a0);
        uint32_t buf_id = rv_get_reg(rv, rv_reg_a1);
        int32_t slot = (int32_t)rv_get_reg(rv, rv_reg_a2);
        uint32_t field = rv_get_reg(rv, rv_reg_a3) & 0xFFFFu; /* lower 16 bits */
        uint32_t value_bits = rv_get_reg(rv, rv_reg_a4);

        switch (op) {
        case BUF_OP_GET_F32:
        case BUF_OP_GET_I32:
        case BUF_OP_GET_U32:
        case BUF_OP_GET_I16:
        case BUF_OP_GET_U16:
        case BUF_OP_GET_I8:
        case BUF_OP_GET_U8:
        case BUF_OP_GET_BOOL: {
            uint32_t bits = 0;
            blyt_state_get(sc, buf_id, slot, field, &bits);
            buf_op_trace(op, buf_id, slot, field, bits);
            rv_set_reg(rv, rv_reg_a0, bits);
            break;
        }
        case BUF_OP_SET_F32:
        case BUF_OP_SET_I32:
        case BUF_OP_SET_U32:
        case BUF_OP_SET_I16:
        case BUF_OP_SET_U16:
        case BUF_OP_SET_I8:
        case BUF_OP_SET_U8:
        case BUF_OP_SET_BOOL: {
            /* type_tag = (op / 2) - 1: SET_F32=2→0 (f32=6), etc.
             * We rely on the field declaration's type_tag in blyt_state_set. */
            buf_op_trace(op, buf_id, slot, field, value_bits);
            blyt_state_set(sc, buf_id, slot, field, value_bits, 0);
            rv_set_reg(rv, rv_reg_a0, 0);
            break;
        }
        case BUF_OP_ALLOC_SLOT: {
            /* a2 = guest pointer to int32_t out_slot */
            uint32_t out_vaddr = (uint32_t)slot; /* slot register holds the ptr */
            int32_t new_slot = -1;
            int r = blyt_state_alloc_slot(sc, buf_id, &new_slot);
            blyt_tracef(BLYT_TRACE_API, "buf_alloc_slot(buf=%u) -> slot=%d", buf_id, new_slot);
            /* Write result back to guest memory */
            if (out_vaddr) {
                vm_attr_t *attr = PRIV(rv);
                memory_t *mem = attr->mem;
                uint32_t v = (uint32_t)new_slot;
                memory_write(mem, out_vaddr, (const uint8_t *)&v, 4);
            }
            rv_set_reg(rv, rv_reg_a0, r == 0 ? 0 : (uint32_t)-1);
            break;
        }
        case BUF_OP_FREE_SLOT: {
            int r = blyt_state_free_slot(sc, buf_id, slot);
            blyt_tracef(BLYT_TRACE_API, "buf_free_slot(buf=%u, slot=%d) -> %d", buf_id, slot, r);
            rv_set_reg(rv, rv_reg_a0, r == 0 ? 0 : (uint32_t)-1);
            break;
        }
        case BUF_OP_REF: {
            uint32_t ref = blyt_state_ref(sc, buf_id, slot);
            blyt_tracef(BLYT_TRACE_API, "buf_ref(buf=%u, slot=%d) -> 0x%08x", buf_id, slot, ref);
            rv_set_reg(rv, rv_reg_a0, ref);
            break;
        }
        case BUF_OP_REF_VALID: {
            /* a2 carries the packed ref, not a slot index. */
            uint32_t ref = (uint32_t)slot;
            int v = blyt_state_ref_valid(sc, buf_id, ref);
            blyt_tracef(BLYT_TRACE_API, "buf_ref_valid(buf=%u, ref=0x%08x) -> %d", buf_id, ref, v);
            rv_set_reg(rv, rv_reg_a0, (uint32_t)v);
            break;
        }
        case BUF_OP_GET_F64: { /* f64: returns lo in a0, hi in a1 (Spike U) */
            uint64_t bits = 0;
            blyt_state_get64(sc, buf_id, slot, field, &bits);
            rv_set_reg(rv, rv_reg_a0, (uint32_t)bits);
            rv_set_reg(rv, rv_reg_a1, (uint32_t)(bits >> 32));
            break;
        }
        case BUF_OP_SET_F64: { /* f64: a4=lo, a5=hi (Spike U) */
            uint64_t bits = (uint64_t)value_bits | ((uint64_t)rv_get_reg(rv, rv_reg_a5) << 32);
            blyt_state_set64(sc, buf_id, slot, field, bits);
            rv_set_reg(rv, rv_reg_a0, 0);
            break;
        }
        default:
            blyt_tracef(BLYT_TRACE_API, "buf_op?(op=%u, buf=%u, slot=%d, field=%u) -> -1", op,
                        buf_id, slot, field);
            rv_set_reg(rv, rv_reg_a0, (uint32_t)-1);
            break;
        }
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_SAVE_WRITE: {
        /* a0=slot (uint32_t); blyt_save_write already called on_save_state. */
        uint32_t slot_n = rv_get_reg(rv, rv_reg_a0);
        int r = -1;
        if (g_run_ctx && g_run_ctx->state_ctx && g_run_ctx->save_dir) {
            r = blyt_save_write(g_run_ctx->state_ctx, g_run_ctx->save_dir, g_run_ctx->cart_name,
                                slot_n, g_run_ctx->save_version);
        }
        blyt_tracef(BLYT_TRACE_API, "save_write(slot=%u) -> %d", slot_n, r);
        rv_set_reg(rv, rv_reg_a0, r == 0 ? 0u : 3u); /* BLYT_ERR_IO=3 */
        rv->PC += 4;
        return;
    }

    case BLYT_ECALL_SAVE_READ: {
        /* a0=slot (uint32_t); guest stub calls on_load_state after we return,
         * using the saved cart version we return in a1 (ADR-0125/0087). */
        uint32_t slot_n = rv_get_reg(rv, rv_reg_a0);
        int r = -1;
        uint32_t saved_version = 0;
        if (g_run_ctx && g_run_ctx->state_ctx && g_run_ctx->save_dir) {
            r = blyt_save_read(g_run_ctx->state_ctx, g_run_ctx->save_dir, g_run_ctx->cart_name,
                               slot_n, &saved_version);
        }
        /* r==0 → BLYT_OK=0, r==-1 → BLYT_ERR_IO=3, r==-2 → BLYT_ERR_SCHEMA_MISMATCH=5 */
        uint32_t result = (r == 0) ? 0u : (r == -2) ? 5u : 3u;
        blyt_tracef(BLYT_TRACE_API, "save_read(slot=%u) -> %u (saved_version=%u)", slot_n, result,
                    saved_version);
        rv_set_reg(rv, rv_reg_a0, result);
        rv_set_reg(rv, rv_reg_a1, saved_version);
        rv->PC += 4;
        return;
    }

    case 93: /* SYS_exit (Linux NR 93) — blyt_exit on emulated path */
    case 94: /* SYS_exit_group (Linux NR 94) — blyt_exit on emulated path */
        blyt_tracef(BLYT_TRACE_API, "sys_exit%s(code=%u)", num == 94 ? "_group" : "",
                    rv_get_reg(rv, rv_reg_a0));
        rv_halt(rv);
        if (rv_get_reg(rv, rv_reg_a0) != 0 && g_run_ctx)
            g_run_ctx->ecall_aborted = true;
        return;

    default:
        /* Safety net: unknown ECALLs get a generic hex dump before trapping. */
        blyt_tracef(BLYT_TRACE_API, "unknown ecall a7=%u a0=0x%x a1=0x%x (trap)", num,
                    rv_get_reg(rv, rv_reg_a0), rv_get_reg(rv, rv_reg_a1));
        rv_halt(rv);
        if (g_run_ctx)
            g_run_ctx->ecall_trapped = true;
        return;
    }
}

#ifdef BLYT_GDB
/* Called by rv32emu when an ebreak instruction executes.  If the PC matches a
 * registered GDB software breakpoint, flag that the breakpoint fired so the
 * outer run loop can notify the client after rv_step() returns.  Leave PC at
 * the ebreak address so the outer loop can restore the original instruction and
 * execute it as a step-over (same as the check_break path).  Otherwise advance
 * PC past the ebreak (same as the rv32emu default). */
static void blyt_ebreak_handler(riscv_t *rv) {
    if (g_run_ctx && g_run_ctx->gdb_enabled && fc_gdb_stub_check_break(rv->PC)) {
        g_run_ctx->gdb_ebreak_pending = true;
        /* Keep PC at the ebreak address so the outer loop can restore the
         * original instruction and perform a proper step-over. */
        rv->halt = true; /* force rv_step() to exit so outer loop runs promptly */
        return;
    }
    /* Not a GDB breakpoint — skip the ebreak (same as rv32emu default). */
    rv->PC += rv->compressed ? 2 : 4;
}
#endif /* BLYT_GDB */

/* -------------------------------------------------------------------------
 * Combined symbol table across all loaded runtime libraries
 * ------------------------------------------------------------------------- */

typedef struct {
    char name[128];
    uint32_t guest_addr;
} blyt_sym_t;

typedef struct blyt_symtab {
    blyt_sym_t syms[MAX_SYMS];
    int count;
} blyt_symtab_t;

static uint32_t symtab_lookup(const blyt_symtab_t *st, const char *name) {
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->syms[i].name, name) == 0)
            return st->syms[i].guest_addr;
    }
    return 0;
}

/* Append src's entries onto dst (first-match precedence: dst's existing entries,
 * e.g. the cart's own symbols, win on lookup).  Used to layer the retained
 * runtime-lib symbols under a freshly-seeded cart on a hot swap (issue #127). */
static void append_symtab(blyt_symtab_t *dst, const blyt_symtab_t *src) {
    for (int i = 0; i < src->count && dst->count < MAX_SYMS; i++)
        dst->syms[dst->count++] = src->syms[i];
}

/* -------------------------------------------------------------------------
 * ELF virtual-address → host pointer (for ET_DYN with base=0 at link time)
 * ------------------------------------------------------------------------- */

static const void *vaddr_to_ptr(const uint8_t *map, size_t map_size, const Elf32_Ehdr *eh,
                                uint32_t vaddr) {
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        size_t off = (size_t)eh->e_phoff + (size_t)i * eh->e_phentsize;
        if (off + sizeof(Elf32_Phdr) > map_size)
            return NULL;
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(map + off);
        if (ph->p_type != PT_LOAD)
            continue;
        if (vaddr >= ph->p_vaddr && vaddr < ph->p_vaddr + ph->p_filesz) {
            size_t foff = ph->p_offset + (vaddr - ph->p_vaddr);
            if (foff < map_size)
                return map + foff;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Map PT_LOAD segments into rv32emu guest memory
 * ------------------------------------------------------------------------- */

static bool map_lib_segments(const blyt_lib_t *lib, memory_t *mem) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)lib->map;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        size_t off = (size_t)eh->e_phoff + (size_t)i * eh->e_phentsize;
        if (off + sizeof(Elf32_Phdr) > lib->size)
            return false;
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(lib->map + off);
        if (ph->p_type != PT_LOAD)
            continue;
        if ((size_t)ph->p_offset + ph->p_filesz > lib->size)
            return false;
        uint32_t g = lib->bias + ph->p_vaddr;
        if (!memory_write(mem, g, lib->map + ph->p_offset, ph->p_filesz))
            return false;
        if (ph->p_memsz > ph->p_filesz) {
            if (!memory_fill(mem, g + ph->p_filesz, ph->p_memsz - ph->p_filesz, 0))
                return false;
        }
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Apply load-time relocations from a library's SHT_RELA sections.
 *
 * Handles:
 *   R_RISCV_RELATIVE — B + A  (load-base relative; most data pointers)
 *   R_RISCV_32       — S + A  (symbol-relative; e.g. internal GOT pointers
 *                              for module-local data variables like the arena
 *                              globals in libblytc.so)
 *
 * R_RISCV_JUMP_SLOT and R_RISCV_GLOB_DAT are handled separately by
 * resolve_elf_plt (they require the combined cross-library symbol table).
 * ------------------------------------------------------------------------- */

static void apply_lib_rela(const blyt_lib_t *lib, memory_t *mem) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)lib->map;
    if (eh->e_shentsize < sizeof(Elf32_Shdr) || eh->e_shnum == 0)
        return;

    /* Locate .dynsym (needed for R_RISCV_32 symbol lookup). */
    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(lib->map + eh->e_shoff);
    const Elf32_Shdr *dynsym_sh = NULL;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_DYNSYM && !dynsym_sh)
            dynsym_sh = &shdrs[i];
    }

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        size_t off = (size_t)eh->e_shoff + (size_t)i * eh->e_shentsize;
        if (off + sizeof(Elf32_Shdr) > lib->size)
            break;
        const Elf32_Shdr *sh = (const Elf32_Shdr *)(lib->map + off);
        if (sh->sh_type != SHT_RELA || sh->sh_entsize < sizeof(Elf32_Rela))
            continue;

        size_t count = sh->sh_size / sh->sh_entsize;
        for (size_t j = 0; j < count; j++) {
            size_t roff = sh->sh_offset + j * sh->sh_entsize;
            if (roff + sizeof(Elf32_Rela) > lib->size)
                break;
            const Elf32_Rela *r = (const Elf32_Rela *)(lib->map + roff);
            uint32_t type = ELF32_R_TYPE(r->r_info);
            uint8_t v[4];

            if (type == R_RISCV_RELATIVE) {
                write_u32_le(v, lib->bias + (uint32_t)r->r_addend);
                memory_write(mem, lib->bias + r->r_offset, v, 4);
            } else if (type == R_RISCV_32 && dynsym_sh &&
                       dynsym_sh->sh_entsize >= sizeof(Elf32_Sym)) {
                uint32_t sym_idx = ELF32_R_SYM(r->r_info);
                size_t sym_off =
                    (size_t)dynsym_sh->sh_offset + (size_t)sym_idx * dynsym_sh->sh_entsize;
                if (sym_off + sizeof(Elf32_Sym) > lib->size)
                    continue;
                const Elf32_Sym *sym = (const Elf32_Sym *)(lib->map + sym_off);
                if (sym->st_shndx != SHN_UNDEF) {
                    uint32_t val = lib->bias + sym->st_value + (uint32_t)r->r_addend;
                    write_u32_le(v, val);
                    memory_write(mem, lib->bias + r->r_offset, v, 4);
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Add a library's exported symbols to the combined symbol table
 * ------------------------------------------------------------------------- */

static void build_symtab(const blyt_lib_t *lib, blyt_symtab_t *st) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)lib->map;
    if (eh->e_shentsize < sizeof(Elf32_Shdr) || eh->e_shnum == 0)
        return;

    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(lib->map + eh->e_shoff);
    const Elf32_Shdr *dynsym = NULL;
    const Elf32_Shdr *dynstr = NULL;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_DYNSYM && !dynsym)
            dynsym = &shdrs[i];
        if (dynsym && i == dynsym->sh_link && shdrs[i].sh_type == SHT_STRTAB)
            dynstr = &shdrs[i];
    }
    if (dynsym && !dynstr && dynsym->sh_link < eh->e_shnum)
        dynstr = &shdrs[dynsym->sh_link];
    if (!dynsym || !dynstr || dynsym->sh_entsize < sizeof(Elf32_Sym))
        return;

    const char *strtab = (const char *)(lib->map + dynstr->sh_offset);
    size_t strsz = dynstr->sh_size;
    size_t nsyms = dynsym->sh_size / dynsym->sh_entsize;

    for (size_t j = 0; j < nsyms && st->count < MAX_SYMS; j++) {
        const Elf32_Sym *sym =
            (const Elf32_Sym *)(lib->map + dynsym->sh_offset + j * dynsym->sh_entsize);
        if (sym->st_shndx == SHN_UNDEF)
            continue;
        uint8_t bind = ELF32_ST_BIND(sym->st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK)
            continue;
        if (sym->st_name >= strsz || sym->st_name == 0)
            continue;
        const char *name = strtab + sym->st_name;

        if (symtab_lookup(st, name) != 0)
            continue;

        blyt_sym_t *s = &st->syms[st->count++];
        strncpy(s->name, name, sizeof(s->name) - 1);
        s->name[sizeof(s->name) - 1] = '\0';
        s->guest_addr = lib->bias + sym->st_value;
    }
}

/* -------------------------------------------------------------------------
 * Parse DT_NEEDED entries from a library's PT_DYNAMIC segment
 * ------------------------------------------------------------------------- */

static int get_dt_needed(const blyt_lib_t *lib, const char **names, int max_out) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)lib->map;
    int count = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        size_t off = (size_t)eh->e_phoff + (size_t)i * eh->e_phentsize;
        if (off + sizeof(Elf32_Phdr) > lib->size)
            break;
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(lib->map + off);
        if (ph->p_type != PT_DYNAMIC)
            continue;

        const Elf32_Dyn *dyn = (const Elf32_Dyn *)(lib->map + ph->p_offset);
        size_t ndyn = ph->p_filesz / sizeof(Elf32_Dyn);
        const char *strtab = NULL;

        for (size_t k = 0; k < ndyn; k++) {
            if (dyn[k].d_tag == DT_NULL)
                break;
            if (dyn[k].d_tag == DT_STRTAB) {
                strtab = (const char *)vaddr_to_ptr(lib->map, lib->size, eh, dyn[k].d_un.d_val);
                break;
            }
        }
        if (!strtab)
            break;

        for (size_t k = 0; k < ndyn && count < max_out; k++) {
            if (dyn[k].d_tag == DT_NULL)
                break;
            if (dyn[k].d_tag == DT_NEEDED)
                names[count++] = strtab + dyn[k].d_un.d_val;
        }
        break;
    }
    return count;
}

/* -------------------------------------------------------------------------
 * Resolve PLT/GOT entries in an ELF binary against the combined symbol table
 * ------------------------------------------------------------------------- */

static void resolve_elf_plt(const uint8_t *map, size_t map_size, memory_t *mem, uint32_t elf_bias,
                            const blyt_symtab_t *syms) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)map;
    if (eh->e_shentsize < sizeof(Elf32_Shdr) || eh->e_shnum == 0)
        return;

    const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(map + eh->e_shoff);
    const Elf32_Shdr *shstrtab = &shdrs[eh->e_shstrndx];
    const char *shstr = (const char *)(map + shstrtab->sh_offset);

    const Elf32_Shdr *dynsym = NULL;
    const Elf32_Shdr *dynstr = NULL;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_DYNSYM && !dynsym)
            dynsym = &shdrs[i];
        if (shdrs[i].sh_type == SHT_STRTAB && strcmp(shstr + shdrs[i].sh_name, ".dynstr") == 0)
            dynstr = &shdrs[i];
    }
    if (!dynsym || !dynstr || dynsym->sh_entsize < sizeof(Elf32_Sym))
        return;

    const char *sym_strtab = (const char *)(map + dynstr->sh_offset);
    size_t sym_strsz = dynstr->sh_size;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sh = &shdrs[i];
        if (sh->sh_type != SHT_RELA || sh->sh_entsize < sizeof(Elf32_Rela))
            continue;

        size_t rcount = sh->sh_size / sh->sh_entsize;
        for (size_t j = 0; j < rcount; j++) {
            const Elf32_Rela *r = (const Elf32_Rela *)(map + sh->sh_offset + j * sh->sh_entsize);
            uint32_t type = ELF32_R_TYPE(r->r_info);
            if (type != R_RISCV_JUMP_SLOT && type != R_RISCV_GLOB_DAT && type != R_RISCV_32)
                continue;

            uint32_t sym_idx = ELF32_R_SYM(r->r_info);
            if (sym_idx * dynsym->sh_entsize + sizeof(Elf32_Sym) > dynsym->sh_size)
                continue;

            const Elf32_Sym *sym =
                (const Elf32_Sym *)(map + dynsym->sh_offset + sym_idx * dynsym->sh_entsize);
            if (sym->st_name >= sym_strsz)
                continue;

            const char *sym_name = sym_strtab + sym->st_name;
            /* R_RISCV_32 for locally-defined symbols is handled by apply_lib_rela;
             * only cross-module UNDEF references need resolution here. */
            if (type == R_RISCV_32 && sym->st_shndx != SHN_UNDEF)
                continue;
            uint32_t resolved = symtab_lookup(syms, sym_name);
            if (resolved == 0) {
                if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
                    uint8_t zero[4] = {0};
                    memory_write(mem, elf_bias + r->r_offset, zero, 4);
                } else {
                    fprintf(stderr, "[dynlink] UNRESOLVED STRONG: %s\n", sym_name);
                }
                continue;
            }

            uint8_t v[4];
            write_u32_le(v, resolved);
            memory_write(mem, elf_bias + r->r_offset, v, 4);
        }
    }
}

/* -------------------------------------------------------------------------
 * Open a library — check the in-memory registry first, then fall back to
 * loading from BLYT_LIB_DIR.  Sets *mmapped_out to true only when the data
 * was mmap'd from a file and must be munmap'd by the caller.
 * ------------------------------------------------------------------------- */

static bool open_lib(const char *lib_dir, const char *lib_name, const uint8_t **map_out,
                     size_t *size_out, bool *mmapped_out) {
    /* Registry lookup (e.g. libs embedded in the libretro core) */
    for (int i = 0; i < g_registered_lib_count; i++) {
        if (strcmp(g_registered_libs[i].name, lib_name) == 0) {
            *map_out = (const uint8_t *)g_registered_libs[i].data;
            *size_out = g_registered_libs[i].size;
            *mmapped_out = false;
            return true;
        }
    }

    /* Filesystem fallback */
    if (!lib_dir || lib_dir[0] == '\0') {
        fprintf(stderr, "blyt: %s not in registry and BLYT_LIB_DIR not set\n", lib_name);
        return false;
    }

    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", lib_dir, lib_name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "blyt: cannot open %s\n", path);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return false;
    }

    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return false;

    *map_out = (const uint8_t *)m;
    *size_out = (size_t)st.st_size;
    *mmapped_out = true;
    return true;
}

/* -------------------------------------------------------------------------
 * Cart-image bookkeeping shared by initial load and hot swap (issue #127)
 * ------------------------------------------------------------------------- */

/* Record the cart's image placement: base, span (highest mapped offset) and BSS
 * regions, used by session_cart_fn and blyt_reset_every_frame_cycle.  Resets the
 * BSS list so it is correct for the (possibly new) cart on a swap. */
static void record_cart_image_layout(blyt_session_t *s, const blyt_cart_t *cart,
                                     uint32_t load_base) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)cart->map;
    s->cart_base = load_base;
    s->cart_span = 0;
    s->n_cart_bss = 0;
    for (uint16_t pi = 0; pi < eh->e_phnum; pi++) {
        size_t off = (size_t)eh->e_phoff + (size_t)pi * eh->e_phentsize;
        if (off + sizeof(Elf32_Phdr) > cart->map_size)
            break;
        const Elf32_Phdr *ph = (const Elf32_Phdr *)((const uint8_t *)cart->map + off);
        if (ph->p_type != PT_LOAD)
            continue;
        uint32_t end = ph->p_vaddr + ph->p_memsz;
        if (end > s->cart_span)
            s->cart_span = end;
        if (ph->p_memsz > ph->p_filesz && s->n_cart_bss < MAX_BSS_REGIONS) {
            s->cart_bss[s->n_cart_bss].start = load_base + ph->p_vaddr + ph->p_filesz;
            s->cart_bss[s->n_cart_bss].size = ph->p_memsz - ph->p_filesz;
            s->n_cart_bss++;
        }
    }
}

#ifdef BLYT_GDB
/* Fill GDB svr4 library slot `idx` with the cart at guest base `base`, reported
 * at `path` (the file lldb reads DWARF from), and mirror it into the stable FFI
 * array (issue #119). */
static void fill_cart_gdb_lib_slot(blyt_session_t *s, int idx, const blyt_cart_t *cart,
                                   uint32_t base, const char *path) {
    blyt_gdb_lib_t *gl = &s->gdb_libs[idx];
    strncpy(gl->path, path, sizeof(gl->path) - 1);
    gl->path[sizeof(gl->path) - 1] = '\0';
    gl->l_addr = base;
    /* l_ld = base + PT_DYNAMIC.p_vaddr (lldb reads the dynamic section here). */
    gl->l_ld = 0;
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)cart->map;
    for (uint16_t pi = 0; pi < eh->e_phnum; pi++) {
        size_t off = (size_t)eh->e_phoff + (size_t)pi * eh->e_phentsize;
        if (off + sizeof(Elf32_Phdr) > cart->map_size)
            break;
        const Elf32_Phdr *ph = (const Elf32_Phdr *)((const uint8_t *)cart->map + off);
        if (ph->p_type == PT_DYNAMIC) {
            gl->l_ld = base + ph->p_vaddr;
            break;
        }
    }
    s->gdb_libs_ffi[idx].path = gl->path;
    s->gdb_libs_ffi[idx].l_addr = gl->l_addr;
    s->gdb_libs_ffi[idx].l_ld = gl->l_ld;
}

/* Rebuild the stable FFI mirror from gdb_libs[0..gdb_nlibs) — call after any
 * entry is moved/removed so the path pointers stay valid (issue #119). */
static void rebuild_gdb_libs_ffi(blyt_session_t *s) {
    for (int i = 0; i < s->gdb_nlibs; i++) {
        s->gdb_libs_ffi[i].path = s->gdb_libs[i].path;
        s->gdb_libs_ffi[i].l_addr = s->gdb_libs[i].l_addr;
        s->gdb_libs_ffi[i].l_ld = s->gdb_libs[i].l_ld;
    }
}

/* Register the cart's OWN entry in the GDB qXfer:libraries-svr4 list at attach
 * (issue #119): the cart is presented purely as a shared library, never the
 * main executable, so lldb can cleanly unload/reload it.  Returns the entry
 * index, or -1 if the table is full / no path. */
static int register_cart_gdb_library(blyt_session_t *s, const blyt_cart_t *cart, uint32_t base,
                                     const char *reported_path) {
    const char *path = (reported_path && reported_path[0]) ? reported_path : cart->path;
    if (!path)
        return -1;
    if (s->gdb_cart_lib_idx < 0) {
        if (s->gdb_nlibs >= MAX_GDB_LIBS)
            return -1;
        s->gdb_cart_lib_idx = s->gdb_nlibs++;
    }
    fill_cart_gdb_lib_slot(s, s->gdb_cart_lib_idx, cart, base, path);
    return s->gdb_cart_lib_idx;
}
#endif /* BLYT_GDB */

/* Cache the cart's lifecycle entry-point guest addresses (0 = not defined). */
static void resolve_cart_entry_points(blyt_session_t *s, const blyt_symtab_t *all) {
    s->fn_on_save_state = symtab_lookup(all, "blyt_cart_on_save_state");
    s->fn_init = symtab_lookup(all, "blyt_cart_init");
    s->fn_on_load_state = symtab_lookup(all, "blyt_cart_on_load_state");
    s->fn_on_new_state = symtab_lookup(all, "blyt_cart_on_new_state");
    s->fn_update = symtab_lookup(all, "blyt_cart_update");
    s->fn_draw = symtab_lookup(all, "blyt_cart_draw");
    s->fn_on_quit = symtab_lookup(all, "blyt_cart_on_quit");
    s->fn_cleanup = symtab_lookup(all, "blyt_cart_cleanup");
    s->fn_on_assets_reloaded = symtab_lookup(all, "blyt_cart_on_assets_reloaded");
    s->fn_is_quit_requested = symtab_lookup(all, "blyt_is_quit_requested");
}

#if defined(BLYT_LUA) && defined(__EMSCRIPTEN__)
/* Resolve .lua_exports for WASM hybrid trampolines.  The section contains one
 * blyt_lua_export_entry_t per BLYT_LUA_EXPORT_* macro; cache the resolved guest
 * addresses so run_lua_cart() can register host-side trampolines without
 * re-parsing the ELF.  Resets the table so it is correct after a swap. */
static void resolve_cart_lua_exports(blyt_session_t *s, const blyt_cart_t *cart,
                                     const blyt_symtab_t *all) {
    s->lua_nexports = 0;
    size_t esz = 0;
    const blyt_lua_export_entry_t *ent =
        (const blyt_lua_export_entry_t *)blyt_cart_find_section(cart, ".lua_exports", &esz);
    if (!ent)
        return;
    int n = (int)(esz / sizeof(*ent));
    for (int i = 0; i < n && s->lua_nexports < 32; i++) {
        uint32_t addr = symtab_lookup(all, ent[i].fn_sym);
        if (!addr)
            continue;
        /* ADR-0130: bridged exports are invoked through their wrapper (wrap_sym)
         * instead of typed argument conversion.  A bridged entry with an
         * unresolvable wrapper is skipped. */
        uint8_t flags = ent[i]._pad[0];
        uint32_t wrap_addr = 0;
        if (flags & BLYT_LUA_EXPORT_FLAG_BRIDGED) {
            wrap_addr = symtab_lookup(all, ent[i].wrap_sym);
            if (!wrap_addr)
                continue;
        }
        s->lua_exports[s->lua_nexports].fn_guest_addr = addr;
        s->lua_exports[s->lua_nexports].wrap_guest_addr = wrap_addr;
        s->lua_exports[s->lua_nexports].flags = flags;
        memcpy(s->lua_exports[s->lua_nexports].lua_name, ent[i].lua_name, 32);
        s->lua_exports[s->lua_nexports].nargs = ent[i].nargs;
        memcpy(s->lua_exports[s->lua_nexports].arg_types, ent[i].arg_types, 4);
        s->lua_exports[s->lua_nexports].ret_type = ent[i].ret_type;
        s->lua_nexports++;
    }
}
#endif

/* -------------------------------------------------------------------------
 * Top-level dynamic loader
 * ------------------------------------------------------------------------- */

static blyt_cart_run_err_t dynlink(blyt_session_t *s, const blyt_cart_t *cart) {
    riscv_t *rv = s->rv;
    const char *lib_dir = getenv("BLYT_LIB_DIR");

    vm_attr_t *attr = PRIV(rv);
    memory_t *mem = attr->mem;

    blyt_lib_t libs[MAX_RUNTIME_LIBS];
    int nlibs = 0;

    /* blyt_symtab_t holds 2048 × 132-byte entries (~264 KiB).  Heap-allocate
     * so it does not overflow small stacks (e.g. Emscripten's 64 KiB default).
     * Lua carts add ~450 exported symbols from libblytcommonlua.so; 512 was
     * too small once the Lua VM library is included in the symbol table. */
    blyt_symtab_t *all_syms = calloc(1, sizeof(*all_syms));
    if (!all_syms)
        return BLYT_RUN_ERR_EMU;

    /* Retain the runtime-lib symbols on the session so a hot swap can re-link a
     * new cart against the persistent libs without reloading them (issue #127).
     * Allocated once per session; freed in blyt_session_destroy. */
    if (!s->lib_syms) {
        s->lib_syms = calloc(1, sizeof(*s->lib_syms));
        if (!s->lib_syms) {
            free(all_syms);
            return BLYT_RUN_ERR_EMU;
        }
    } else {
        s->lib_syms->count = 0;
    }

    /* Seed cart symbols first so cart's strong definitions win over library stubs. */
    {
        blyt_lib_t cart_syms = {
            .map = (const uint8_t *)cart->map,
            .size = cart->map_size,
            .bias = 0,
            .mmapped = false,
        };
        build_symtab(&cart_syms, all_syms);
    }

    char name_buf[MAX_RUNTIME_LIBS][64];
    int qhead = 0;
    int qtail = 0;

    /* Seed BFS queue from the cart's DT_NEEDED */
    {
        const Elf32_Ehdr *ceh = (const Elf32_Ehdr *)cart->map;
        if (ceh->e_shentsize >= sizeof(Elf32_Shdr) && ceh->e_shnum > 0) {
            const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(cart->map + ceh->e_shoff);
            const Elf32_Shdr *shstrtab = &shdrs[ceh->e_shstrndx];
            const char *shstr = (const char *)(cart->map + shstrtab->sh_offset);
            const Elf32_Shdr *dynamic = NULL;
            const Elf32_Shdr *dynstr = NULL;
            for (uint16_t i = 0; i < ceh->e_shnum; i++) {
                if (shdrs[i].sh_type == SHT_DYNAMIC)
                    dynamic = &shdrs[i];
                if (shdrs[i].sh_type == SHT_STRTAB &&
                    strcmp(shstr + shdrs[i].sh_name, ".dynstr") == 0)
                    dynstr = &shdrs[i];
            }
            if (dynamic && dynstr) {
                const char *strtab = (const char *)(cart->map + dynstr->sh_offset);
                size_t ndyn = dynamic->sh_size / sizeof(Elf32_Dyn);
                const Elf32_Dyn *dyn = (const Elf32_Dyn *)(cart->map + dynamic->sh_offset);
                for (size_t k = 0; k < ndyn && qtail < MAX_RUNTIME_LIBS; k++) {
                    if (dyn[k].d_tag == DT_NULL)
                        break;
                    if (dyn[k].d_tag == DT_NEEDED) {
                        strncpy(name_buf[qtail], strtab + dyn[k].d_un.d_val,
                                sizeof(name_buf[qtail]) - 1);
                        name_buf[qtail][sizeof(name_buf[qtail]) - 1] = '\0';
                        qtail++;
                    }
                }
            }
        }
    }

    bool ok = true;

    while (qhead < qtail && nlibs < MAX_RUNTIME_LIBS) {
        const char *lib_name = name_buf[qhead++];

        const uint8_t *lmap;
        size_t lsz;
        bool mmapped;
        if (!open_lib(lib_dir, lib_name, &lmap, &lsz, &mmapped)) {
            ok = false;
            break;
        }

        const Elf32_Ehdr *leh = (const Elf32_Ehdr *)lmap;
        if (lsz < sizeof(Elf32_Ehdr) || leh->e_ident[EI_MAG0] != ELFMAG0 ||
            leh->e_machine != EM_RISCV) {
            if (mmapped)
                munmap((void *)lmap, lsz);
            ok = false;
            break;
        }

        uint32_t bias =
            (leh->e_type == ET_DYN) ? GUEST_LIB_BASE + (uint32_t)nlibs * GUEST_LIB_STRIDE : 0;

        blyt_lib_t lib = {.map = lmap, .size = lsz, .bias = bias, .mmapped = mmapped};

        if (!map_lib_segments(&lib, mem)) {
            if (mmapped)
                munmap((void *)lmap, lsz);
            ok = false;
            break;
        }

        apply_lib_rela(&lib, mem);
        /* Build into the retained lib-only table; the cart's symbols are layered
         * on top (below) so the cart still wins on lookup (issue #127). */
        build_symtab(&lib, s->lib_syms);
        libs[nlibs++] = lib;

#ifdef BLYT_GDB
        /* Capture library layout for GDB qXfer:libraries-svr4:read. */
        if (s->gdb_nlibs < MAX_GDB_LIBS) {
            int idx = s->gdb_nlibs++;
            blyt_gdb_lib_t *gl = &s->gdb_libs[idx];
            strncpy(gl->path, lib_name, sizeof(gl->path) - 1);
            gl->path[sizeof(gl->path) - 1] = '\0';
            gl->l_addr = bias;
            /* l_ld = bias + PT_DYNAMIC.p_vaddr */
            gl->l_ld = 0;
            for (uint16_t pi = 0; pi < leh->e_phnum; pi++) {
                size_t poff = (size_t)leh->e_phoff + (size_t)pi * leh->e_phentsize;
                if (poff + sizeof(Elf32_Phdr) > lsz)
                    break;
                const Elf32_Phdr *ph = (const Elf32_Phdr *)(lmap + poff);
                if (ph->p_type == PT_DYNAMIC) {
                    gl->l_ld = bias + ph->p_vaddr;
                    break;
                }
            }
            /* Mirror into the stable FFI array (path pointer stays valid for
             * the session's lifetime, avoiding a dangling pointer in layout). */
            s->gdb_libs_ffi[idx].path = gl->path;
            s->gdb_libs_ffi[idx].l_addr = gl->l_addr;
            s->gdb_libs_ffi[idx].l_ld = gl->l_ld;
        }
#endif /* BLYT_GDB */

        /* Enqueue transitive DT_NEEDED dependencies */
        const char *needed[MAX_RUNTIME_LIBS];
        int nneeded = get_dt_needed(&lib, needed, MAX_RUNTIME_LIBS);
        for (int i = 0; i < nneeded && qtail < MAX_RUNTIME_LIBS; i++) {
            bool already = false;
            for (int q = 0; q < qtail; q++) {
                if (strcmp(name_buf[q], needed[i]) == 0) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                strncpy(name_buf[qtail], needed[i], sizeof(name_buf[qtail]) - 1);
                name_buf[qtail][sizeof(name_buf[qtail]) - 1] = '\0';
                qtail++;
            }
        }
    }

    if (ok) {
        /* Apply cart's own R_RISCV_RELATIVE relocations (e.g. static arrays of
         * function pointers in src/lib/ C libraries, like luaL_Reg tables).
         * Libraries get this in the BFS loop above; the cart needs it too. */
        blyt_lib_t cart_lib = {
            .map = (const uint8_t *)cart->map,
            .size = cart->map_size,
            .bias = 0,
            .mmapped = false,
        };
        apply_lib_rela(&cart_lib, mem);

        /* Layer the retained runtime-lib symbols under the cart's (seeded above),
         * giving the combined table dynlink resolves against (issue #127). */
        append_symtab(all_syms, s->lib_syms);

        for (int i = 0; i < nlibs; i++) {
            resolve_elf_plt(libs[i].map, libs[i].size, mem, libs[i].bias, all_syms);
        }
        resolve_elf_plt(cart->map, cart->map_size, mem, 0, all_syms);

        /* Initialise the libblytc.so arena (ADR-0120). */
        uint32_t sym_base = symtab_lookup(all_syms, "blytc_arena_base");
        uint32_t sym_size = symtab_lookup(all_syms, "blytc_arena_size");
        if (sym_base != 0 && sym_size != 0) {
            uint8_t v[4];
            write_u32_le(v, BLYT_ARENA_BASE);
            memory_write(mem, sym_base, v, 4);
            write_u32_le(v, BLYT_ARENA_SIZE);
            memory_write(mem, sym_size, v, 4);
        }
        /* Resolve the unified-budget accounting block (ADR-0008 #158) so the
         * resource ECALLs can publish the footprint + read the heap total. Its
         * .data is zero-initialised in the freshly mapped lib (footprint 0,
         * matching "nothing loaded yet"), so no stamp is needed at fresh load. */
        s->ctx.mem_acct_vaddr = symtab_lookup(all_syms, "blyt_mem_acct");
    }

    /* Retain the lib images on the session (issue #119) so a hot swap can
     * re-resolve the libs' references back into the cart (libblytcommon's
     * blyt_main → blyt_cart_init/update/draw).  Ownership transfers here; the
     * mmapped ones are munmapped in blyt_session_destroy.  On failure (!ok) the
     * caller destroys the session, which frees them, so keep them either way. */
    s->n_retained_libs = nlibs;
    for (int i = 0; i < nlibs && i < MAX_RUNTIME_LIBS; i++)
        s->retained_libs[i] = libs[i];

#if defined(BLYT_LUA) && defined(__EMSCRIPTEN__)
    if (ok)
        resolve_cart_lua_exports(s, cart, all_syms);
#endif

    if (ok)
        resolve_cart_entry_points(s, all_syms);

    free(all_syms);
    return ok ? BLYT_RUN_OK : BLYT_RUN_ERR_EMU;
}

/* -------------------------------------------------------------------------
 * Session API — public entry points
 * ------------------------------------------------------------------------- */

/* Populate ctx->resources for a session (issue #91).  A packed cart carries
 * .cart.resource.<id> sections; a dev (project-dir) build has none, so read the
 * staging directory that sits alongside the dev ELF (the cart path's directory).
 * BLYT_RESOURCE_DIR overrides the location (e.g. the WASM dev path). */
void blyt_resource_table_load_for_cart(blyt_resource_table_t *t, const blyt_cart_t *cart) {
    if (blyt_resource_table_load_from_cart(t, cart) != 0)
        return; /* packed cart: resources come from embedded sections */

    const char *res_dir = getenv("BLYT_RESOURCE_DIR");
    char dir_buf[4096];
    if (!res_dir && cart->path) {
        snprintf(dir_buf, sizeof(dir_buf), "%s", cart->path);
        char *slash = strrchr(dir_buf, '/');
        if (slash) {
            *slash = '\0';
            res_dir = dir_buf;
        }
    }
    if (res_dir)
        blyt_resource_table_load_from_index(t, res_dir);
}

static void load_session_resources(blyt_run_ctx_t *ctx, const blyt_cart_t *cart) {
    blyt_resource_table_load_for_cart(&ctx->resources, cart);
}

blyt_resource_table_t *blyt_session_resources(blyt_session_t *s) {
    return &s->ctx.resources;
}

size_t blyt_session_resource_evict_all(blyt_session_t *s) {
    if (!s)
        return 0;
    size_t freed = blyt_resource_table_evict_all_evictable(&s->ctx.resources);
    /* Forced eviction freed owned bytes — republish the accounting block so the
     * advisory resource_cache_used the guest reads is current immediately, not
     * only after the next frame-boundary publish (#159). */
    mem_acct_publish_footprint(&s->ctx, s->attr.mem);
    return freed;
}

bool blyt_session_reload_resources(blyt_session_t *session, blyt_cart_t *cart) {
    if (!session)
        return false;
    /* load_from_cart / load_from_index each clear the table first, so this both
     * drops superseded entries and re-reads the current ones. */
    load_session_resources(&session->ctx, cart);
    return true;
}

/* Returns addr if it lies in the cart's own image [cart_base, cart_base+span),
 * else 0.  Used by the WASM frontend to decide whether to dispatch lifecycle
 * callbacks via the RV32 session or fall back to the Lua global.  Tracking the
 * cart's actual placement (rather than assuming bias 0 / < GUEST_LIB_BASE) keeps
 * this correct after blyt_session_swap_cart re-maps the cart at a fresh base
 * (issue #127; #119 uses a non-zero base). */
static uint32_t session_cart_fn(const blyt_session_t *s, uint32_t addr) {
    return (addr && addr >= s->cart_base && addr < s->cart_base + s->cart_span) ? addr : 0;
}
uint32_t blyt_session_cart_fn_init(blyt_session_t *s) {
    return session_cart_fn(s, s->fn_init);
}
uint32_t blyt_session_cart_fn_on_new_state(blyt_session_t *s) {
    return session_cart_fn(s, s->fn_on_new_state);
}
uint32_t blyt_session_cart_fn_update(blyt_session_t *s) {
    return session_cart_fn(s, s->fn_update);
}
uint32_t blyt_session_cart_fn_draw(blyt_session_t *s) {
    return session_cart_fn(s, s->fn_draw);
}
uint32_t blyt_session_cart_fn_on_quit(blyt_session_t *s) {
    return session_cart_fn(s, s->fn_on_quit);
}
uint32_t blyt_session_cart_fn_cleanup(blyt_session_t *s) {
    return session_cart_fn(s, s->fn_cleanup);
}

const void *blyt_session_vm_id(const blyt_session_t *s) {
    return s ? (const void *)s->rv : NULL;
}

/* Swap the running cart's code in place WITHOUT recreating the VM (issue #127,
 * spike-W β / gate G1).  The rv32 VM, the runtime libs (libblyt32/…) mapped at
 * GUEST_LIB_BASE, and all per-session debug/GDB state persist; only the cart
 * image is replaced.  See blyt_runtime.h for the contract.  State restore is the
 * caller's job (blyt_session_snapshot before / blyt_session_restore after). */
bool blyt_session_swap_cart(blyt_session_t *s, const blyt_cart_t *new_cart, uint32_t load_base,
                            const char *reported_path) {
    if (!s || !s->rv || !new_cart || !new_cart->map || !s->lib_syms)
        return false;

    riscv_t *rv = s->rv;
    vm_attr_t *attr = PRIV(rv);
    memory_t *mem = attr->mem;

    /* Remember the old image placement so its uncovered bytes can be zeroed for
     * hygiene once the new image is successfully mapped (below). */
    uint32_t old_base = s->cart_base;
    uint32_t old_span = s->cart_span;

    /* 1. Map the new cart's PT_LOAD segments at load_base and apply its own
     *    R_RISCV_RELATIVE/R_RISCV_32 relocations (as dynlink does for the cart).
     *    On a malformed image this fails before anything is overwritten, leaving
     *    the session running the old code. */
    blyt_lib_t cart_lib = {
        .map = (const uint8_t *)new_cart->map,
        .size = new_cart->map_size,
        .bias = load_base,
        .mmapped = false,
    };
    if (!map_lib_segments(&cart_lib, mem))
        return false;
    apply_lib_rela(&cart_lib, mem);

    /* 2. Re-link against the persistent runtime libs: seed the new cart's symbols
     *    (so they win) then layer the retained lib symbols, and resolve the
     *    cart's PLT/GOT.  The libs never moved, so their symbol addresses (held
     *    in s->lib_syms) are still valid. */
    blyt_symtab_t *all = calloc(1, sizeof(*all));
    if (!all)
        return false;
    build_symtab(&cart_lib, all);
    append_symtab(all, s->lib_syms);
    resolve_elf_plt(new_cart->map, new_cart->map_size, mem, load_base, all);

    /* 2b. Re-resolve the runtime libs' OWN references back into the cart against
     *     the new symbol table (issue #119).  libblytcommon's blyt_main calls
     *     blyt_cart_init/update/draw through GOT entries bound to the FIRST
     *     cart's addresses; after the swap those are stale (the cart moved to a
     *     fresh base and/or its functions shifted) and would jump into freed
     *     memory.  Re-running each retained lib's PLT/GOT rebinds them to the new
     *     cart while leaving lib→lib references unchanged. */
    for (int i = 0; i < s->n_retained_libs; i++) {
        resolve_elf_plt(s->retained_libs[i].map, s->retained_libs[i].size, mem,
                        s->retained_libs[i].bias, all);
    }

    /* 3. Re-stamp the libblytc.so arena globals (ADR-0120) so the reloaded cart
     *    starts from a clean arena base/size, and re-zero the arena allocator's
     *    ready flag (issue #133, spike-W gate G4) so the next malloc()
     *    re-initialises its bump pointer + free list.  libblytc persists across
     *    the swap, so without this the reloaded cart's allocations would
     *    continue past the previous cart's — drifting addresses (not
     *    bit-identical to a fresh load) and, accumulated over a long edit-reload
     *    session, exhausting the arena (each Lua init() allocates the whole Lua
     *    state from it). */
    uint32_t sym_base = symtab_lookup(all, "blytc_arena_base");
    uint32_t sym_size = symtab_lookup(all, "blytc_arena_size");
    if (sym_base != 0 && sym_size != 0) {
        uint8_t v[4];
        write_u32_le(v, BLYT_ARENA_BASE);
        memory_write(mem, sym_base, v, 4);
        write_u32_le(v, BLYT_ARENA_SIZE);
        memory_write(mem, sym_size, v, 4);
    }
    uint32_t sym_ready = symtab_lookup(all, "blytc_arena_ready");
    if (sym_ready != 0) {
        uint8_t v[4];
        write_u32_le(v, 0);
        memory_write(mem, sym_ready, v, 4);
    }
    /* Re-resolve and zero the unified-budget accounting block (#158): libblytc
     * persists across the in-VM swap, so its blyt_mem_acct still holds the old
     * cart's counters. The guest arena re-zeros guest_heap_used on its next
     * malloc (via the ready lever above); clear both fields now so there is no
     * stale-footprint window before the reloaded cart republishes. */
    s->ctx.mem_acct_vaddr = symtab_lookup(all, "blyt_mem_acct");
    if (s->ctx.mem_acct_vaddr) {
        uint8_t z[8] = {0};
        memory_write(mem, s->ctx.mem_acct_vaddr, z, 8);
    }

    /* 4. Re-resolve the cart's lifecycle entry points (and WASM hybrid exports)
     *    and re-record its image layout / BSS regions. */
    resolve_cart_entry_points(s, all);
#if defined(BLYT_LUA) && defined(__EMSCRIPTEN__)
    resolve_cart_lua_exports(s, new_cart, all);
#endif
    free(all);
    record_cart_image_layout(s, new_cart, load_base);

    /* 5. Zero any bytes of the OLD image the new one doesn't cover, so stale code
     *    or data from a larger previous build cannot survive (memory hygiene; the
     *    cart region sits below the runtime libs and the trampolines, so this
     *    never touches them).  Same base: zero only the tail past the new span;
     *    a different base (the debug layer's fresh-base reload, #119): the whole
     *    old region is uncovered.
     *
     *    Inter-segment gaps WITHIN the new image are deliberately NOT zeroed
     *    (issue #133 acceptance #4): they are never referenced by the cart and
     *    are not part of any determinism/replay contract, so the residue is
     *    harmless.  Making them fresh-load-identical would mean zeroing the whole
     *    region before mapping, which would forfeit the map-first guarantee above
     *    (a malformed new image must leave the old cart running untouched).  The
     *    accumulation hazard this issue targets is the arena allocator (step 3),
     *    not these gaps. */
    if (old_span) {
        if (old_base == s->cart_base) {
            if (old_span > s->cart_span)
                memory_fill(mem, old_base + s->cart_span, old_span - s->cart_span, 0);
        } else {
            memory_fill(mem, old_base, old_span, 0);
        }
    }

    /* 6. Re-initialise the cart-derived session state from the new cart: state
     *    buffer layouts, resource table, manifest id and save version.  The
     *    caller's snapshot was taken from the old state_ctx before this call;
     *    blyt_session_restore copies it back over the fresh buffers afterwards. */
    blyt_state_ctx_destroy(&s->state_ctx);
    blyt_state_ctx_init(new_cart, &s->state_ctx);
    s->ctx.state_ctx = &s->state_ctx;
    blyt_resource_table_clear(&s->ctx.resources);
    s->ctx.resource_scratch_off = 0;
    load_session_resources(&s->ctx, new_cart);
    /* Re-apply the reloaded cart's persistent set (#160): mark + pre-load so the
     * swapped-in resources are resident before the reloaded cart's init() reruns,
     * matching a fresh load. (A reload's footprint is republished by the caller's
     * subsequent frame; preload here keeps the bytes resident meanwhile.) */
    blyt_resource_table_load_persistent_from_cart(&s->ctx.resources, new_cart);
    blyt_resource_table_preload_persistent(&s->ctx.resources);
    snprintf(s->ctx.cart_name, sizeof(s->ctx.cart_name), "%s", new_cart->id);
    s->ctx.save_version = new_cart->save_version;

    /* The GDB library list is NOT touched here (issue #119): the debug-reload
     * solib swap is a two-phase add-then-remove driven by
     * blyt_session_gdb_notify_cart_reloaded after this returns, which needs the
     * OLD cart entry still present.  The exec-file stays empty (cart is a
     * library, never the main exe).  Run-mode reload leaves the list stale —
     * harmless without a debugger. */
    (void)reported_path;

    /* 7. Discard translated code from the OLD image — the block map keys blocks
     *    by guest PC, so blocks at the cart's addresses now hold stale (old)
     *    instructions and MUST be flushed, then re-boot the new cart from its
     *    entry (clearing regs/stack) so the next run_frame runs its init(). */
    block_map_clear(rv);
    rv_block_chain_reset();
    const Elf32_Ehdr *neh = (const Elf32_Ehdr *)new_cart->map;
    rv_reset(rv, load_base + neh->e_entry);
    rv_set_reg(rv, rv_reg_ra, BLYT_TRAMPOLINE_EXIT_ADDR);

    /* 8. Clear per-frame / ecall control flags so the next run_frame starts a
     *    clean frame on the new code. */
    s->ctx.ecall_trapped = false;
    s->ctx.ecall_aborted = false;
    s->ctx.frame_done = false;
    s->ctx.fn_return_done = false;
    s->cart_has_drawn = false;
    rv->halt = false;

    return true;
}

blyt_state_ctx_t *blyt_session_state_ctx(blyt_session_t *s) {
    return &s->state_ctx;
}
const char *blyt_session_save_dir(blyt_session_t *s) {
    return s->ctx.save_dir;
}
const char *blyt_session_cart_name(blyt_session_t *s) {
    return s->ctx.cart_name;
}

/*
 * Call blyt_is_quit_requested() in the guest and return 1 if the guest called
 * blyt_quit() since the last check.  Used by the WASM frontend to propagate
 * a C-native lifecycle callback's blyt_quit() call to the Lua coroutine's
 * blyt.should_quit() check.  Returns 0 when not applicable (NULL session, or
 * blyt_is_quit_requested not in the symtab — i.e. pre-SDK carts).
 */
int blyt_session_check_guest_quit(blyt_session_t *s) {
    if (!s || !s->fn_is_quit_requested)
        return 0;
    blyt_session_begin_fn_call(s, s->fn_is_quit_requested, 0, NULL);
    blyt_cart_run_err_t ferr;
    do {
        ferr = blyt_session_run_frame(s);
    } while (ferr != BLYT_RUN_FN_DONE && ferr != BLYT_RUN_OK);
    if (ferr != BLYT_RUN_FN_DONE)
        return 0;
    return (int)blyt_session_fn_return_value(s) != 0;
}

blyt_session_t *blyt_session_create(blyt_cart_t *cart, blyt_log_fn log_fn) {
    blyt_session_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
#ifdef BLYT_GDB
    s->gdb_cart_lib_idx = -1; /* set when the cart is registered as a library */
#endif

    {
        uint32_t lua_mask = blyt_cart_lua_lifecycle_mask(cart);
        uint32_t native_mask = blyt_cart_native_lifecycle_mask(cart);
        if (lua_mask & native_mask) {
            if (log_fn)
                log_fn("lifecycle conflict: callbacks defined in both Lua and native");
            free(s);
            return NULL;
        }
    }

    s->ctx.log_fn = log_fn;

    s->attr.mem_size = BLYT_EMU_MEM_SIZE;
    s->attr.stack_size = BLYT_STACK_SIZE;
    s->attr.args_offset_size = 0;
    s->attr.argc = 0;
    s->attr.argv = NULL;
    s->attr.log_level = LOG_WARN;
    s->attr.cycle_per_step = BLYT_CYCLE_PER_STEP;
    s->attr.allow_misalign = false;
    s->attr.fd_stdin = STDIN_FILENO;
    s->attr.fd_stdout = STDOUT_FILENO;
    s->attr.fd_stderr = STDERR_FILENO;
    s->attr.data.user.elf_program = cart->path;

    s->rv = rv_create(&s->attr);
    if (!s->rv) {
        if (log_fn)
            log_fn("session create: rv_create failed (guest VM allocation)");
        free(s);
        return NULL;
    }

    vm_attr_t *rattr = PRIV(s->rv);
    inject_exit_trampoline(rattr->mem);

    blyt_cart_run_err_t load_err = dynlink(s, cart);
    if (load_err != BLYT_RUN_OK) {
        if (log_fn)
            log_fn("session create: guest dynamic link failed");
        rv_delete(s->rv);
        free(s);
        return NULL;
    }

    /* Record cart image placement + BSS regions (the cart loads at bias 0 as the
     * rv32emu main program; blyt_session_swap_cart re-records on a hot swap). */
    record_cart_image_layout(s, cart, 0u);

    /* Initialise state buffer context from .cart.layouts (if present). */
    blyt_state_ctx_init(cart, &s->state_ctx);
    s->ctx.state_ctx = &s->state_ctx;

    /* The manifest id names the save file subdirectory (validated at load
     * time: ≤63 bytes, so it always fits cart_name[64]). */
    snprintf(s->ctx.cart_name, sizeof(s->ctx.cart_name), "%s", cart->id);

    /* save_version stamped into the save header on write (ADR-0125). */
    s->ctx.save_version = cart->save_version;

    /* Resource table (issue #91); see load_session_resources. */
    blyt_resource_table_init(&s->ctx.resources);
    s->ctx.resource_scratch_off = 0;
    load_session_resources(&s->ctx, cart);

    /* Graphics back buffer (issue #188): the gfx ECALL handlers draw into the
     * session's paletted framebuffer and flip cart_has_drawn via these pointers.
     * The session address is stable across hot swaps, so this is set once. */
    s->ctx.fb = s->pixels;
    s->ctx.cart_has_drawn = &s->cart_has_drawn;

    /* Surface registry (#205): slot 0 is BLYT_SCREEN, aliasing the session's
     * paletted framebuffer — never reaped, destroyed, or budget-counted.  Its
     * generation stays 0 so the reserved BLYT_SCREEN handle (0x40000000) always
     * resolves.  Off-screen surfaces occupy slots 1.. and are reaped each frame. */
    memset(&s->ctx.surfaces, 0, sizeof(s->ctx.surfaces));
    s->ctx.surfaces.slots[0] = (blyt_surface_slot_t){
        .pixels = s->pixels,
        .w = BLYT_FRAME_W,
        .h = BLYT_FRAME_H,
        .gen = 0,
        .in_use = true,
        .is_screen = true,
        .owned = false,
    };

    /* Draw()-only surface enforcement (#205): phase starts NONE (no callback);
     * the guest blyt_main advances it via BLYT_ECALL_PHASE.  A debug cart
     * (.cart.info debug flag) hard-errors on out-of-phase surface access; a
     * release cart gets the defined no-op. */
    s->ctx.phase = BLYT_PHASE_NONE;
    s->ctx.cart_is_debug = blyt_cart_is_debug(cart);
    s->ctx.dev_fault = false;

    /* Resolve save directory: BLYT_SAVE_DIR env var or ~/.local/share/blyt */
    {
        const char *env_dir = getenv("BLYT_SAVE_DIR");
        if (env_dir) {
            s->ctx.save_dir = strdup(env_dir);
        } else {
            const char *home = getenv("HOME");
            if (!home)
                home = "/tmp";
            char buf[512];
            snprintf(buf, sizeof(buf), "%s/.local/share/blyt", home);
            s->ctx.save_dir = strdup(buf);
        }
    }

    s->rv->io.on_ecall = blyt_ecall_handler;
#ifdef BLYT_GDB
    s->rv->io.on_ebreak = blyt_ebreak_handler;
#endif
    rv_set_reg(s->rv, rv_reg_ra, BLYT_TRAMPOLINE_EXIT_ADDR);

    blyt_testcard_init_palette(s->palette);

#ifdef BLYT_GDB
    /* Present the cart purely as a shared library, NOT as the main executable
     * (issue #119, Spike W §5d/§5e): lldb-dap's `program` is a stub ELF, so the
     * cart's main-exe copy (which Unix can never unload) never exists and a hot
     * reload leaves no stale duplicate breakpoint location.  Leave the exec-file
     * empty and announce the cart in the svr4 library list at attach so
     * breakpoints bind (with an address) before init() runs. */
    s->gdb_exec_path[0] = '\0';
    register_cart_gdb_library(s, cart, 0u, NULL);
    /* Register cpu_ops with the stub so it can read/write registers/memory
     * and set/clear software breakpoints via ebreak patches. */
    fc_gdb_stub_set_cpu_ops(&gdb_cpu_ops);
#endif

    /* Persistent resources (ADR-0028, #160): mark the declared set and pre-load
     * it BEFORE init() runs, so its bytes are resident from frame 0 and reserved
     * in the non-evictable footprint. This runs at guest_heap_used == 0 (no guest
     * code has executed yet), so the reservation always fits a budget the build
     * guard already validated. An over-budget set (only reachable via a
     * hand-crafted cart — the packer rejects it at build) or a decode failure on
     * a declared-essential resource fails cart start deterministically. */
    blyt_resource_table_load_persistent_from_cart(&s->ctx.resources, cart);
    if (blyt_resource_table_preload_persistent(&s->ctx.resources) != 0) {
        if (log_fn)
            log_fn("session create: persistent resources exceed the 16 MB budget "
                   "(or failed to load)");
        blyt_session_destroy(s);
        return NULL;
    }
    /* Publish the persistent footprint now so the guest's very first allocation
     * in init() already sees the reserved budget (no under-enforced window). */
    mem_acct_publish_footprint(&s->ctx, s->attr.mem);

    return s;
}

#ifdef BLYT_LUA
/* ADR-0130: a bridged wrapper raised a Lua error.  Restore the register
 * snapshot taken at begin_bridged_call so the abandoned guest frame does not
 * leak guest stack, and clear the bridged-call window.  The error value is
 * on top of the exchange thread; the frontend raises it from the trampoline
 * continuation. */
static void blyt_bridge_error_unwind(blyt_session_t *session) {
    for (uint32_t i = 1; i < 32; i++) /* x0 is hardwired */
        rv_set_reg(session->rv, i, session->bridge_saved_regs[i]);
    session->rv->csr_fcsr = session->bridge_saved_fcsr;
    for (uint32_t i = 0; i < 32; i++) /* restore FP file (RV32D, Spike U) */
        session->rv->F[i].v = session->bridge_saved_fregs[i];
    session->ctx.lua_bridge_active = false;
    session->ctx.lua_bridge_error = false;
}
#endif

/* Map a guest address back to the cart lifecycle callback it belongs to.
 * Returns NULL for addresses that are not cached lifecycle entry points
 * (e.g. Lua-export wrappers called via begin_fn_call). */
static const char *session_fn_name(const blyt_session_t *s, uint32_t addr) {
    if (addr == 0)
        return NULL;
    if (addr == s->fn_init)
        return "init";
    if (addr == s->fn_on_save_state)
        return "on_save_state";
    if (addr == s->fn_on_load_state)
        return "on_load_state";
    if (addr == s->fn_on_new_state)
        return "on_new_state";
    if (addr == s->fn_update)
        return "update";
    if (addr == s->fn_draw)
        return "draw";
    if (addr == s->fn_on_quit)
        return "on_quit";
    if (addr == s->fn_cleanup)
        return "cleanup";
    if (addr == s->fn_on_assets_reloaded)
        return "on_assets_reloaded";
    if (addr == s->fn_is_quit_requested)
        return "is_quit_requested";
    return NULL;
}

/* Emit the lifecycle "ret <name> a0=…" line for the in-flight host fn call. */
static void trace_fn_return(blyt_session_t *session) {
    if (session->trace_fn_addr == 0)
        return;
    if (blyt_trace_enabled(BLYT_TRACE_LIFECYCLE)) {
        const char *nm = session_fn_name(session, session->trace_fn_addr);
        uint32_t a0 = rv_get_reg(session->rv, rv_reg_a0);
        if (nm)
            blyt_tracef(BLYT_TRACE_LIFECYCLE, "ret %s a0=0x%x", nm, a0);
        else
            blyt_tracef(BLYT_TRACE_LIFECYCLE, "ret fn@0x%x a0=0x%x", session->trace_fn_addr, a0);
    }
    session->trace_fn_addr = 0;
}

/* Emit a deterministic hash of the final paletted framebuffer to the cart log
 * when BLYT_FRAME_HASH is set (issue #188 / Spike X).  Every execution leg runs
 * this same host runtime, so the line is identical across legs for identical
 * pixels — the integration harness asserts cross-leg framebuffer equality (and
 * a checked-in golden) straight from captured stdout, the one channel every leg
 * shares.  Off by default; zero cost when the env var is unset. */
static void blyt_emit_frame_hash(blyt_session_t *session) {
    static int s_on = -1;
    if (s_on < 0)
        s_on = getenv("BLYT_FRAME_HASH") != NULL ? 1 : 0;
    if (!s_on || !g_run_ctx || !g_run_ctx->log_fn)
        return;
    uint64_t h = blyt_frame_hash(session->pixels, (size_t)BLYT_FRAME_W * (size_t)BLYT_FRAME_H);
    char buf[64];
    snprintf(buf, sizeof(buf), "[blyt:fbhash] %016llx", (unsigned long long)h);
    g_run_ctx->log_fn(buf);
}

blyt_cart_run_err_t blyt_session_run_frame(blyt_session_t *session) {
    g_run_ctx = &session->ctx;
    session->ctx.frame_done = false;
    session->ctx.fn_return_done = false;
    /* Reset the resource scratch bump allocator and force-release any pins still
     * held: a resource pin (and the guest pointer it returned) is valid only for
     * the frame it was taken in (ADR-0027 frame-scope amendment, #123). */
    session->ctx.resource_scratch_off = 0;
    blyt_resource_table_force_release_pins(&session->ctx.resources);
    /* Reap the previous frame's off-screen surfaces: they are draw-scoped derived
     * artifacts (#205), so none survive the frame boundary.  Frees their buffers
     * and drops their bytes from the budget footprint (republished below). */
    blyt_surface_reap_all(&session->ctx, session->attr.mem);
    /* Frame-boundary housekeeping (#158): pins just dropped, so the footprint may
     * have shrunk — republish it and bound the resident evictable cache to the
     * room the current heap+footprint leaves. Covers the "heap grew mid-frame
     * with no resource ECALL" case; not load-bearing for the allocation decision
     * (the counters are always current), purely RSS/mem.stats hygiene. */
    mem_acct_publish_footprint(&session->ctx, session->attr.mem);

    /* BLYT_TRACE frame channel: open a frame unless this run_frame call is
     * driving a host-initiated fn call (begin_fn_call) or resuming a frame
     * already open (e.g. re-entry while GDB-paused). */
    if (session->trace_fn_addr == 0 && !session->trace_frame_open &&
        blyt_trace_enabled(BLYT_TRACE_FRAME)) {
        blyt_tracef(BLYT_TRACE_FRAME, "start");
        session->trace_frame_open = true;
    }
    /* Clear any halt set by the previous BLYT_ECALL_FRAME_DONE so the while
     * loop below can execute the new frame.  A halt from blyt_exit/abort would
     * have caused the caller to stop resubmitting frames entirely. */
    session->rv->halt = false;

#ifdef BLYT_DAP
    if (session->ctx.dap_enabled && fc_dap_is_restart_pending()) {
        g_run_ctx = NULL;
        return BLYT_RUN_RESTART;
    }
#endif

#ifdef BLYT_GDB
    if (session->ctx.gdb_enabled && session->ctx.debug_state == BLYT_DEBUG_RUNNING) {
#ifdef __EMSCRIPTEN__
        /* On WASM, drain the GDB WebSocket queue once per frame. */
        fc_gdb_stub_poll();
#endif
        /* If the stub was halted externally (WASM poll or TCP \x03 interrupt),
         * T02 has already been sent.  Stop executing until vCont arrives. */
        if (fc_gdb_stub_is_halted()) {
#ifdef __EMSCRIPTEN__
            session->ctx.debug_state = BLYT_DEBUG_PAUSED_GDB;
            g_run_ctx = NULL;
            return BLYT_RUN_GDB_PAUSED;
#else
            /* Native (TCP): block in the transport thread's poll loop. */
            fc_gdb_stub_block_until_resume();
            int ext_action = fc_gdb_stub_pending_action();
            if (ext_action == 2) {
                rv_halt(session->rv);
                g_run_ctx = NULL;
                return BLYT_RUN_OK;
            }
            session->ctx.gdb_single_step = (ext_action == 1);
#endif
        }
    }
#endif

    while (!rv_has_halted(session->rv) && !session->ctx.ecall_trapped &&
           !session->ctx.ecall_aborted) {
#ifdef BLYT_GDB
        /* Issue #146: a GDB software breakpoint was inserted/removed on the
         * transport thread, which patched guest memory but left rv32emu's
         * already-translated block holding the pre-patch instructions.  Discard
         * the translated-block cache here — on the CPU thread that owns rv —
         * before the next rv_step, so block_find re-translates from the patched
         * memory and the ebreak actually executes.  Mirrors the reload path
         * (block_map_clear + rv_block_chain_reset).  The plain volatile read is
         * the cheap common case; the mutex is taken only to perform the clear. */
        if (session->ctx.gdb_enabled && g_gdb_block_flush_pending) {
            pthread_mutex_lock(&g_bp_mutex);
            if (g_gdb_block_flush_pending) {
                block_map_clear(session->rv);
                rv_block_chain_reset();
                g_gdb_block_flush_pending = false;
            }
            pthread_mutex_unlock(&g_bp_mutex);
        }
        uint32_t gdb_bp_step_addr = 0;
        if (session->ctx.gdb_enabled) {
            /* If already paused waiting for vCont, poll for a resume packet. */
            if (session->ctx.debug_state == BLYT_DEBUG_PAUSED_GDB) {
#ifdef __EMSCRIPTEN__
                /* WASM: non-blocking poll — process one queued packet if any.
                 * T05 is NOT sent here proactively; LLDB will query the stop
                 * reason with '?' and the packet handler responds with T05 then.
                 * Sending T05 before the qSupported ACK corrupts the protocol. */
                fc_gdb_stub_poll();
                int _reentry_action = fc_gdb_stub_pending_action();
                if (_reentry_action < 0) {
                    g_run_ctx = NULL;
                    return BLYT_RUN_GDB_PAUSED; /* still paused, try again next tick */
                }
                session->ctx.debug_state = BLYT_DEBUG_RUNNING;
                if (_reentry_action == 2) {
                    rv_halt(session->rv);
                    break;
                }
                session->ctx.gdb_single_step = (_reentry_action == 1);
                /* Step-over: if we were paused at a software breakpoint, temporarily
                 * restore the original instruction so rv_step executes it (not the
                 * ebreak).  gdb_bp_step_addr != 0 will suppress check_break below
                 * (preventing an immediate re-stop) and trigger repatch after rv_step. */
                if (session->ctx.gdb_bp_resume_addr != 0) {
                    fc_gdb_stub_restore_bp_temp(session->ctx.gdb_bp_resume_addr);
                    gdb_bp_step_addr = session->ctx.gdb_bp_resume_addr;
                    session->ctx.gdb_bp_resume_addr = 0;
                }
                /* fall through to execute next instruction */
#else
                /* Native: this path should not be reached — TCP path blocks. */
                g_run_ctx = NULL;
                return BLYT_RUN_GDB_PAUSED;
#endif
            }
            /* Check for software breakpoint at current PC.
             * Skip if gdb_bp_step_addr is set — that means we just restored the
             * original instruction for a step-over and must not re-stop here.
             * On the native path, gdb_bp_resume_addr carries the same signal
             * across loop iterations (for mid-block ebreak step-overs). */
            uint32_t gdb_pc = rv_get_pc(session->rv);
#ifndef __EMSCRIPTEN__
            if (session->ctx.gdb_bp_resume_addr != 0) {
                /* Mid-block ebreak step-over: original instruction was already
                 * restored in the ebreak_pending handler.  Set gdb_bp_step_addr
                 * so repatch fires after rv_step, and suppress check_break. */
                gdb_bp_step_addr = session->ctx.gdb_bp_resume_addr;
                session->ctx.gdb_bp_resume_addr = 0;
            } else
#endif
                if (gdb_bp_step_addr == 0 && fc_gdb_stub_check_break(gdb_pc)) {
                session->ctx.debug_state = BLYT_DEBUG_PAUSED_GDB;
#ifdef __EMSCRIPTEN__
                session->ctx.gdb_bp_resume_addr = gdb_pc;
                if (fc_gdb_transport_wasm_is_connected())
                    fc_gdb_stub_notify_stopped();
                g_run_ctx = NULL;
                return BLYT_RUN_GDB_PAUSED;
#else
                fc_gdb_stub_notify_stopped();
                fc_gdb_stub_block_until_resume();
                int gdb_action = fc_gdb_stub_pending_action();
                session->ctx.debug_state = BLYT_DEBUG_RUNNING;
                if (gdb_action == 2) {
                    rv_halt(session->rv);
                    break;
                }
                session->ctx.gdb_single_step = (gdb_action == 1);
                /* Restore original instruction so rv_step executes it, not the
                 * ebreak.  Re-patch after rv_step via gdb_bp_step_addr. */
                fc_gdb_stub_restore_bp_temp(gdb_pc);
                gdb_bp_step_addr = gdb_pc;
#endif
            }
        }
#endif /* BLYT_GDB */

#if defined(BLYT_GDB) && defined(__EMSCRIPTEN__)
        /* On WASM, always step one instruction at a time when GDB is enabled
         * so check_break fires at every PC (rv_step is block-based). */
        if (session->ctx.gdb_enabled)
            rv_step_debug(session->rv);
        else
            rv_step(session->rv);
#elif defined(BLYT_GDB)
        /* When stepping over a software breakpoint, use rv_step_debug to
         * bypass rv32emu's block-IR cache: restore_bp_temp wrote the original
         * instruction to guest memory but the cached block still holds the
         * decoded EBREAK.  rv_step_debug reads via mem_ifetch (guest RAM
         * directly) so the restored instruction always executes. */
        if (session->ctx.gdb_enabled && gdb_bp_step_addr != 0)
            rv_step_debug(session->rv);
        else
            rv_step(session->rv);
#else
        rv_step(session->rv);
#endif

#ifdef BLYT_GDB
        /* Re-patch a SW BP that was temporarily removed to allow stepping over it. */
        if (session->ctx.gdb_enabled && gdb_bp_step_addr != 0)
            fc_gdb_stub_repatch_bp(gdb_bp_step_addr);
        /* Breakpoint fired inside rv_step() via on_ebreak: rv->halt was set to
         * force rv_step to return.  PC is at the ebreak address (the callback
         * does not advance it) so we can perform the same step-over as the
         * check_break path: restore the original instruction, execute it, then
         * re-patch the breakpoint. */
        if (session->ctx.gdb_enabled && session->ctx.gdb_ebreak_pending) {
            session->ctx.gdb_ebreak_pending = false;
            session->rv->halt = false; /* restore — cart has not actually halted */
            session->ctx.debug_state = BLYT_DEBUG_PAUSED_GDB;
            uint32_t gdb_ebreak_pc = rv_get_pc(session->rv);
#ifdef __EMSCRIPTEN__
            session->ctx.gdb_bp_resume_addr = gdb_ebreak_pc;
            if (fc_gdb_transport_wasm_is_connected())
                fc_gdb_stub_notify_stopped();
            if (session->ctx.frame_done) {
                if (!session->cart_has_drawn)
                    blyt_testcard_draw(session->frame_count++, session->pixels);
            }
            g_run_ctx = NULL;
            return BLYT_RUN_GDB_PAUSED;
#else
            fc_gdb_stub_notify_stopped();
            fc_gdb_stub_block_until_resume();
            int gdb_action = fc_gdb_stub_pending_action();
            session->ctx.debug_state = BLYT_DEBUG_RUNNING;
            if (gdb_action == 2) {
                rv_halt(session->rv);
                break;
            }
            session->ctx.gdb_single_step = (gdb_action == 1);
            /* Step-over: restore original instruction so the next rv_step call
             * executes it instead of the ebreak.  Use gdb_bp_resume_addr (a
             * session-level field) rather than the local gdb_bp_step_addr
             * because the local is reset at the top of each loop iteration;
             * the gdb_bp_resume_addr → gdb_bp_step_addr transfer in the
             * check_break section will then trigger repatch after rv_step. */
            fc_gdb_stub_restore_bp_temp(gdb_ebreak_pc);
            session->ctx.gdb_bp_resume_addr = gdb_ebreak_pc;
#endif
        }
        /* Post-instruction single-step pause. */
        if (session->ctx.gdb_enabled && session->ctx.gdb_single_step) {
            session->ctx.gdb_single_step = false;
            session->ctx.debug_state = BLYT_DEBUG_PAUSED_GDB;
            fc_gdb_stub_notify_stopped();
#ifdef __EMSCRIPTEN__
            if (session->ctx.frame_done) {
                if (!session->cart_has_drawn)
                    blyt_testcard_draw(session->frame_count++, session->pixels);
            }
            g_run_ctx = NULL;
            return BLYT_RUN_GDB_PAUSED;
#else
            fc_gdb_stub_block_until_resume();
            int gdb_action = fc_gdb_stub_pending_action();
            session->ctx.debug_state = BLYT_DEBUG_RUNNING;
            if (gdb_action == 2) {
                rv_halt(session->rv);
                break;
            }
            session->ctx.gdb_single_step = (gdb_action == 1);
#endif
        }
#endif /* BLYT_GDB */

        if (session->ctx.frame_done) {
            if (!session->cart_has_drawn)
                blyt_testcard_draw(session->frame_count++, session->pixels);
            blyt_emit_frame_hash(session);
            if (session->trace_frame_open) {
                blyt_tracef(BLYT_TRACE_FRAME, "end");
                session->trace_frame_open = false;
            }
            blyt_trace_frame_mark(++session->trace_frame_no);
            g_run_ctx = NULL;
            return BLYT_RUN_FRAME_DONE;
        }
        if (session->ctx.fn_return_done) {
#ifdef BLYT_LUA
            session->ctx.lua_bridge_active = false;
#endif
            trace_fn_return(session);
            g_run_ctx = NULL;
            return BLYT_RUN_FN_DONE;
        }
#ifdef BLYT_LUA
        if (session->ctx.lua_bridge_error) {
            blyt_bridge_error_unwind(session);
            session->trace_fn_addr = 0;
            g_run_ctx = NULL;
            return BLYT_RUN_FN_ERROR;
        }
#endif
    }

    if (session->ctx.fn_return_done) {
#ifdef BLYT_LUA
        session->ctx.lua_bridge_active = false;
#endif
        trace_fn_return(session);
        g_run_ctx = NULL;
        return BLYT_RUN_FN_DONE;
    }
#ifdef BLYT_LUA
    if (session->ctx.lua_bridge_error) {
        blyt_bridge_error_unwind(session);
        session->trace_fn_addr = 0;
        g_run_ctx = NULL;
        return BLYT_RUN_FN_ERROR;
    }
#endif

    bool trapped = session->ctx.ecall_trapped;
    bool aborted = session->ctx.ecall_aborted || session->ctx.dev_fault;
    g_run_ctx = NULL;

    if (trapped)
        return BLYT_RUN_ERR_ECALL_TRAP;
    if (aborted)
        return BLYT_RUN_ERR_ABORT;
    return BLYT_RUN_OK;
}

void blyt_session_destroy(blyt_session_t *session) {
    if (!session)
        return;
#ifdef BLYT_GDB
    blyt_session_gdb_shutdown(session);
#endif
    blyt_state_ctx_destroy(&session->state_ctx);
    blyt_resource_table_clear(&session->ctx.resources);
    free(session->ctx.save_dir);
    free(session->lib_syms);
    /* Release the retained runtime-lib images (issue #119). */
    for (int i = 0; i < session->n_retained_libs; i++) {
        if (session->retained_libs[i].mmapped)
            munmap((void *)session->retained_libs[i].map, session->retained_libs[i].size);
    }
    rv_delete(session->rv);
    free(session);
}

/* -------------------------------------------------------------------------
 * Host→guest function call API (WASM hybrid Lua+C carts)
 * ------------------------------------------------------------------------- */

int blyt_session_begin_fn_call(blyt_session_t *s, uint32_t fn_addr, int nargs,
                               const uint32_t args[]) {
    if (nargs > 4)
        nargs = 4;
    if (blyt_trace_enabled(BLYT_TRACE_LIFECYCLE)) {
        const char *nm = session_fn_name(s, fn_addr);
        if (nm)
            blyt_tracef(BLYT_TRACE_LIFECYCLE, "call %s", nm);
        else
            blyt_tracef(BLYT_TRACE_LIFECYCLE, "call fn@0x%x", fn_addr);
    }
    s->trace_fn_addr = fn_addr;
    s->rv->PC = fn_addr;
    rv_set_reg(s->rv, rv_reg_ra, BLYT_TRAMPOLINE_FN_RETURN_ADDR);
    for (int i = 0; i < nargs; i++)
        rv_set_reg(s->rv, (uint32_t)(rv_reg_a0 + (uint32_t)i), args[i]);
    s->rv->halt = false;
    s->ctx.fn_return_done = false;
    return 0;
}

uint32_t blyt_session_fn_return_value(const blyt_session_t *s) {
    return rv_get_reg(s->rv, rv_reg_a0);
}

void blyt_session_lua_bridge_attach(blyt_session_t *s, struct lua_State *exch) {
#ifdef BLYT_LUA
    s->ctx.lua_exch = exch;
#else
    (void)s;
    (void)exch;
#endif
}

int blyt_session_begin_bridged_call(blyt_session_t *s, uint32_t wrap_addr) {
#ifdef BLYT_LUA
    if (!s->ctx.lua_exch)
        return -1;
    /* Snapshot registers so a Lua error can unwind the abandoned frame. */
    for (uint32_t i = 0; i < 32; i++)
        s->bridge_saved_regs[i] = rv_get_reg(s->rv, i);
    s->bridge_saved_fcsr = s->rv->csr_fcsr;
    for (uint32_t i = 0; i < 32; i++) /* FLEN=64 FP file (RV32D, Spike U) */
        s->bridge_saved_fregs[i] = s->rv->F[i].v;
    if (++s->lua_bridge_next_token == 0)
        ++s->lua_bridge_next_token; /* token is nonzero */
    s->ctx.lua_bridge_token = s->lua_bridge_next_token;
    s->ctx.lua_bridge_active = true;
    s->ctx.lua_bridge_error = false;
    /* The wrapper receives the token as its opaque lua_State* (a0). */
    uint32_t args[1] = {s->ctx.lua_bridge_token};
    return blyt_session_begin_fn_call(s, wrap_addr, 1, args);
#else
    (void)s;
    (void)wrap_addr;
    return -1;
#endif
}

void blyt_session_visit_lua_exports(blyt_session_t *s, blyt_lua_export_visitor_t cb,
                                    void *userdata) {
#ifdef BLYT_LUA
    for (int i = 0; i < s->lua_nexports; i++)
        cb(s->lua_exports[i].lua_name, s->lua_exports[i].fn_guest_addr,
           s->lua_exports[i].wrap_guest_addr, s->lua_exports[i].flags, s->lua_exports[i].nargs,
           s->lua_exports[i].arg_types, s->lua_exports[i].ret_type, userdata);
#else
    (void)s;
    (void)cb;
    (void)userdata;
#endif
}

const uint8_t *blyt_session_get_pixels(const blyt_session_t *session) {
    return session->pixels;
}

const uint32_t *blyt_session_get_palette(const blyt_session_t *session) {
    return session->palette;
}

bool blyt_session_cart_has_drawn(const blyt_session_t *session) {
    return session->cart_has_drawn;
}

void blyt_session_set_phase(blyt_session_t *session, int32_t phase) {
    if (session)
        session->ctx.phase = phase;
}

void blyt_session_expand_frame(const blyt_session_t *session, uint32_t *xrgb_out) {
    const uint8_t *px = session->pixels;
    const uint32_t *pal = session->palette;
    for (int i = 0; i < BLYT_FRAME_W * BLYT_FRAME_H; i++)
        xrgb_out[i] = pal[px[i]];
}

/* -------------------------------------------------------------------------
 * DAP session helpers
 * ------------------------------------------------------------------------- */

int blyt_session_dap_listen(blyt_session_t *s, int *port_out) {
#ifdef BLYT_DAP
    int port = fc_consolelua_dap_listen(0);
    if (port < 0)
        return -1;
    s->ctx.dap_enabled = true;
    if (port_out)
        *port_out = port;
    return port;
#else
    (void)s;
    (void)port_out;
    return -1;
#endif
}

void blyt_session_dap_shutdown(blyt_session_t *s) {
#ifdef BLYT_DAP
    fc_consolelua_dap_shutdown();
    s->ctx.dap_enabled = false;
#else
    (void)s;
#endif
}

void blyt_session_dap_reattach(blyt_session_t *s) {
#ifdef BLYT_DAP
    if (s)
        s->ctx.dap_enabled = true;
#else
    (void)s;
#endif
}

int blyt_session_dap_wait_ready(blyt_session_t *s) {
#ifdef BLYT_DAP
    if (!s || !s->ctx.dap_enabled)
        return 0;
    return fc_dap_wait_configuration_done();
#else
    (void)s;
    return 0;
#endif
}

/* -------------------------------------------------------------------------
 * GDB session helpers
 * ------------------------------------------------------------------------- */

int blyt_session_gdb_listen(blyt_session_t *s, int *port_out) {
#ifdef BLYT_GDB
    if (!s)
        return -1;

    /* Register layout with the stub.  Use the stable gdb_libs_ffi array whose
     * path pointers remain valid for the session's lifetime. */
    fc_gdb_layout_t layout = {
        .exec_path = s->gdb_exec_path,
        .libraries = s->gdb_libs_ffi,
        .n_libraries = s->gdb_nlibs,
    };
    fc_gdb_stub_set_layout(&layout);

    g_gdb_session = s;
    s->ctx.gdb_enabled = true;

    int actual_port = port_out ? *port_out : 0;
#ifdef __EMSCRIPTEN__
    /* Start in paused state so the cart waits for vCont;c before running. */
    s->ctx.debug_state = BLYT_DEBUG_PAUSED_GDB;
    fc_gdb_transport_wasm_open(actual_port);
    if (port_out)
        *port_out = actual_port;
    return actual_port;
#else
    if (fc_gdb_transport_tcp_listen(actual_port, &actual_port) != 0) {
        s->ctx.gdb_enabled = false;
        g_gdb_session = NULL;
        return -1;
    }
    if (port_out)
        *port_out = actual_port;
    return actual_port;
#endif
#else
    (void)s;
    (void)port_out;
    return -1;
#endif
}

void blyt_session_gdb_set_cart_path(blyt_session_t *s, const char *host_path) {
#ifdef BLYT_GDB
    if (!s || s->gdb_cart_lib_idx < 0 || !host_path || !host_path[0])
        return;
    /* Rename the cart's already-registered svr4 entry; l_addr/l_ld are unchanged
     * (same ELF, same load base) — only the path lldb opens for DWARF differs
     * (issue #144).  The FFI mirror shares gl->path, so re-point it too. */
    blyt_gdb_lib_t *gl = &s->gdb_libs[s->gdb_cart_lib_idx];
    strncpy(gl->path, host_path, sizeof(gl->path) - 1);
    gl->path[sizeof(gl->path) - 1] = '\0';
    s->gdb_libs_ffi[s->gdb_cart_lib_idx].path = gl->path;
#else
    (void)s;
    (void)host_path;
#endif
}

void blyt_session_gdb_shutdown(blyt_session_t *s) {
#ifdef BLYT_GDB
    if (!s || !s->ctx.gdb_enabled)
        return;
#ifdef __EMSCRIPTEN__
    fc_gdb_transport_wasm_shutdown();
#else
    fc_gdb_transport_tcp_shutdown();
#endif
    s->ctx.gdb_enabled = false;
    if (g_gdb_session == s)
        g_gdb_session = NULL;
#else
    (void)s;
#endif
}

/* Pick the guest load base for the next debug hot reload (issue #119): the slot
 * the cart is NOT currently at, so the swap re-maps it to a fresh, non-
 * overlapping base (and zeroes the whole previous image).  The initial cart is
 * at base 0, so the first reload lands at A, then ping-pongs A↔B. */
uint32_t blyt_session_next_reload_base(const blyt_session_t *s) {
    return (s && s->cart_base == BLYT_RELOAD_BASE_A) ? BLYT_RELOAD_BASE_B : BLYT_RELOAD_BASE_A;
}

#ifdef BLYT_GDB
/* Publish the current GDB library layout and fire a solib-change event (a
 * library:; stop), prompting the client to re-read the list and re-resolve
 * breakpoints.  Does NOT wait — the caller decides how to wait for the client to
 * consume it (synchronous busy-wait for TCP, async tick pump for WASM). */
static void publish_libs_nowait(blyt_session_t *s) {
    fc_gdb_layout_t layout = {
        .exec_path = s->gdb_exec_path,
        .libraries = s->gdb_libs_ffi,
        .n_libraries = s->gdb_nlibs,
    };
    fc_gdb_stub_set_layout(&layout);
    fc_gdb_stub_notify_library_change();
}

/* Publish the current GDB library layout, fire a solib-change event, then block
 * until the client has FULLY processed it — re-read the library list and
 * continued (issue #119).  lldb treats a library-change stop as a stop and does
 * not auto-continue; the DAP client (the extension, or the test driver) must
 * continue, which both confirms processing finished and lets the next phase be
 * published without racing.  Waiting on the continue (not just the list read)
 * is what makes the two-phase add-then-remove land in order. */
static void publish_libs_and_wait(blyt_session_t *s) {
    unsigned cont = fc_gdb_stub_continue_gen();
    publish_libs_nowait(s);
    /* Wait up to ~3 s for the client to re-resolve and continue.  A timeout is
     * non-fatal — the next phase still publishes the final library state. */
    for (int i = 0; i < 3000 && fc_gdb_stub_continue_gen() == cont; i++)
        usleep(1000);
}
#endif

bool blyt_session_gdb_is_debugging(const blyt_session_t *s) {
#ifdef BLYT_GDB
    return s && s->ctx.gdb_enabled && s->gdb_cart_lib_idx >= 0;
#else
    (void)s;
    return false;
#endif
}

void blyt_session_gdb_notify_cart_reloaded(blyt_session_t *s, const blyt_cart_t *new_cart,
                                           uint32_t load_base, const char *reported_path) {
#ifdef BLYT_GDB
    if (!s || !s->ctx.gdb_enabled || s->gdb_cart_lib_idx < 0)
        return;
    const char *path = (reported_path && reported_path[0]) ? reported_path : new_cart->path;
    if (!path)
        return;

    int old_idx = s->gdb_cart_lib_idx;

    /* Phase 1 — ADD the rebuilt cart as a NEW library alongside the old one.
     * lldb loads it (fresh base + unique path → re-reads the new DWARF) and
     * re-resolves breakpoints onto the new code.  A single combined event makes
     * lldb unload the old module WITHOUT loading the new one, so we add first. */
    if (s->gdb_nlibs >= MAX_GDB_LIBS) {
        /* No room for a transient second entry: fall back to re-reporting in
         * place (re-resolution still works when the breakpoint was pending). */
        fill_cart_gdb_lib_slot(s, old_idx, new_cart, load_base, path);
        publish_libs_and_wait(s);
        return;
    }
    int new_idx = s->gdb_nlibs++;
    fill_cart_gdb_lib_slot(s, new_idx, new_cart, load_base, path);
    publish_libs_and_wait(s);

    /* Phase 2 — REMOVE the old cart library so its stale breakpoint location is
     * dropped, leaving exactly one location per breakpoint.  The new entry is
     * the last slot (just appended); move it into the old slot and shrink, so
     * the cart entry lands back at old_idx.  Rebuild the FFI mirror after. */
    (void)new_idx;
    s->gdb_libs[old_idx] = s->gdb_libs[s->gdb_nlibs - 1];
    s->gdb_nlibs--;
    s->gdb_cart_lib_idx = old_idx;
    rebuild_gdb_libs_ffi(s);
    publish_libs_and_wait(s);
#else
    (void)s;
    (void)new_cart;
    (void)load_base;
    (void)reported_path;
#endif
}

#ifdef BLYT_GDB
/* Phase 2 of the two-phase swap (no publish): drop the stale cart library entry
 * by folding the transient new slot (the last one) into the old slot and
 * shrinking, so exactly one cart entry remains and lands back at old_idx. */
static void reload_notify_drop_old(blyt_session_t *s) {
    int old_idx = s->reload_notify_old_idx;
    s->gdb_libs[old_idx] = s->gdb_libs[s->gdb_nlibs - 1];
    s->gdb_nlibs--;
    s->gdb_cart_lib_idx = old_idx;
    rebuild_gdb_libs_ffi(s);
}

/* Upper bound on async pump ticks per phase (issue #170): a real client
 * re-resolves in a handful of ticks; this only bounds a debug session with no
 * live client so the reload cannot wedge forever waiting on continue_gen. */
#define BLYT_RELOAD_NOTIFY_MAX_TICKS 100000
#endif

bool blyt_session_gdb_reload_notify_begin(blyt_session_t *s, const blyt_cart_t *new_cart,
                                          uint32_t load_base, const char *reported_path) {
#ifdef BLYT_GDB
    if (!blyt_session_gdb_is_debugging(s))
        return false;
    const char *path = (reported_path && reported_path[0]) ? reported_path : new_cart->path;
    if (!path)
        return false;

    s->reload_notify_old_idx = s->gdb_cart_lib_idx;
    s->reload_notify_ticks = 0;

    /* Phase 1 — ADD the rebuilt cart as a NEW library alongside the old one
     * (fresh base + unique path → lldb re-reads the new DWARF and re-resolves
     * breakpoints onto the new code).  Publish the solib-change stop and return;
     * the caller pumps until the client consumes it. */
    if (s->gdb_nlibs >= MAX_GDB_LIBS) {
        /* No room for a transient second entry: re-report in place — a single
         * publish (re-resolution still works when the breakpoint is pending). */
        fill_cart_gdb_lib_slot(s, s->gdb_cart_lib_idx, new_cart, load_base, path);
        s->reload_notify_cont = fc_gdb_stub_continue_gen();
        publish_libs_nowait(s);
        s->reload_notify_phase = 2; /* no old entry to drop; just await consume */
        return true;
    }
    int new_idx = s->gdb_nlibs++;
    fill_cart_gdb_lib_slot(s, new_idx, new_cart, load_base, path);
    s->reload_notify_cont = fc_gdb_stub_continue_gen();
    publish_libs_nowait(s);
    s->reload_notify_phase = 1;
    return true;
#else
    (void)s;
    (void)new_cart;
    (void)load_base;
    (void)reported_path;
    return false;
#endif
}

bool blyt_session_gdb_reload_notify_pump(blyt_session_t *s) {
#ifdef BLYT_GDB
    if (!s || s->reload_notify_phase == 0)
        return true; /* nothing in flight */

    /* Service one queued client packet so its library reads + vCont continue can
     * advance.  continue_gen ticks when the client re-resolves and continues. */
    fc_gdb_stub_poll();

    if (fc_gdb_stub_continue_gen() == s->reload_notify_cont) {
        /* This phase not yet consumed.  Bound the wait so a session with no live
         * client cannot wedge — on timeout, force the final library state. */
        if (++s->reload_notify_ticks < BLYT_RELOAD_NOTIFY_MAX_TICKS)
            return false;
        if (s->reload_notify_phase == 1) {
            reload_notify_drop_old(s);
            publish_libs_nowait(s);
        }
        s->reload_notify_phase = 0;
        return true;
    }

    if (s->reload_notify_phase == 1) {
        /* Phase 1 consumed → Phase 2: drop the stale entry, publish, await its
         * consume so exactly one breakpoint location survives. */
        reload_notify_drop_old(s);
        s->reload_notify_cont = fc_gdb_stub_continue_gen();
        publish_libs_nowait(s);
        s->reload_notify_ticks = 0;
        s->reload_notify_phase = 2;
        return false;
    }

    s->reload_notify_phase = 0; /* phase 2 consumed → swap complete */
    return true;
#else
    (void)s;
    return true;
#endif
}

int blyt_session_gdb_wait_attached(blyt_session_t *s) {
#ifdef BLYT_GDB
    if (!s || !s->ctx.gdb_enabled)
        return 0;
#ifdef __EMSCRIPTEN__
    /* On WASM, connection is async; caller cannot block. */
    return fc_gdb_transport_wasm_is_connected();
#else
    /* Block until a GDB client connects (has_client set by transport thread). */
    while (!fc_gdb_stub_has_client()) {
        usleep(10000); /* 10 ms */
    }
    return 1;
#endif
#else
    (void)s;
    return 0;
#endif
}

void blyt_session_gdb_continue_initial_halt(blyt_session_t *s) {
#ifdef BLYT_GDB
    if (s && s->ctx.gdb_enabled)
        fc_gdb_stub_continue_initial_halt();
#else
    (void)s;
#endif
}

/* Block until the GDB client finishes its initial configuration — i.e. fetches
 * the svr4 library list, inserts its breakpoints, and issues its first continue
 * (vCont;c) — so the native client's breakpoints are in place BEFORE any cart
 * code runs (issue #119).  This is purely an ORDERING gate: it guarantees a
 * breakpoint set before launch — notably one in blyt_cart_init(), the first
 * place most authors set one — stops on the FIRST launch rather than only the
 * next time its address executes.  A hybrid session would otherwise be released
 * by the Lua-first gate (blyt_session_gdb_continue_initial_halt) the instant the
 * client connects, letting an early native call — a hybrid cart's on_new_state →
 * a Lua-exported C function, or init() itself — run past a breakpoint line before
 * the insert lands.  Pure-native sessions already wait for the client's continue
 * implicitly (they never force-clear the initial halt); this gives hybrid the
 * same guarantee.
 *
 * Correctness no longer rests on this gate.  It originally also guarded the
 * rv32emu single-VM stale-translated-block hazard (cf. #42): an early native call
 * could cache a block with no ebreak that the later insert never re-translated,
 * so the breakpoint silently never fired (on slow hosts only — Linux CI lost the
 * race, macOS won it).  #146 fixes that directly — inserting or removing a
 * breakpoint now flushes the translated-block cache (g_gdb_block_flush_pending),
 * so a late insert re-translates and fires the next time its address executes.
 * The gate therefore only buys first-launch ordering, never eventual firing.
 *
 * Returns 1 once the client has continued, 0 on timeout — best-effort: the caller
 * then force-clears the halt so a client that never continues (e.g. no lldb on
 * the native side) cannot wedge boot.  A lost race now costs at most one launch's
 * ordering (a breakpoint in init() may not stop until init() next runs), never a
 * silent permanent miss, so the bound need not be conservative. */
int blyt_session_gdb_wait_client_continue(blyt_session_t *s) {
#ifdef BLYT_GDB
    if (!s || !s->ctx.gdb_enabled)
        return 0;
#ifdef __EMSCRIPTEN__
    /* WASM is async / single-threaded; the caller cannot block here. */
    return 0;
#else
    /* continue_gen counts the client's vCont;c packets and is 0 at boot until the
     * first one, so "has continued at least once" == continue_gen > 0.  The
     * client always sends its breakpoint inserts (Z0) before that first continue,
     * so observing it guarantees the breakpoints are in.  Test as ">0" rather
     * than "incremented from now" because the native client may already have
     * continued before this gate runs (e.g. the raw hybrid driver issues the GDB
     * continue before the DAP configurationDone this is sequenced after).  Up to
     * ~5 s for lldb-dap to read libraries, set breakpoints, and continue. */
    for (int i = 0; i < 5000 && fc_gdb_stub_continue_gen() == 0; i++)
        usleep(1000);
    return fc_gdb_stub_continue_gen() != 0;
#endif
#else
    (void)s;
    return 0;
#endif
}

/* -------------------------------------------------------------------------
 * --reset-every-frame cycle helpers
 * ------------------------------------------------------------------------- */

static void blyt_session_zero_guest_bss(blyt_session_t *s) {
    vm_attr_t *attr = PRIV(s->rv);
    for (int i = 0; i < s->n_cart_bss; i++)
        memory_fill(attr->mem, s->cart_bss[i].start, s->cart_bss[i].size, 0);
}

/* Run blyt_session_run_frame until FN_DONE (skipping any FRAME_DONE returns).
 * Returns BLYT_RUN_FN_DONE on success, an error code otherwise. */
static blyt_cart_run_err_t call_until_fn_done(blyt_session_t *s) {
    blyt_cart_run_err_t r;
    do {
        r = blyt_session_run_frame(s);
    } while (r == BLYT_RUN_FRAME_DONE);
    return r;
}

/* Defined below alongside the other lifecycle-hook callers. */
static void call_guest_on_load_state(blyt_session_t *s, uint32_t reason,
                                     uint32_t saved_cart_version, uint32_t buffers);

void blyt_reset_every_frame_cycle(blyt_session_t *s) {
    uint32_t saved_pc;
    uint32_t saved_regs[32];
    uint32_t saved_fcsr;
    blyt_state_snapshot_t *snap;
    blyt_cart_run_err_t r;
    uint32_t i;

    if (!s || s->fn_init == 0)
        return;

    /* Preserve emulator state so the normal game loop continues after the cycle. */
    saved_pc = s->rv->PC;
    for (i = 0; i < 32; i++)
        saved_regs[i] = rv_get_reg(s->rv, i);
    saved_fcsr = s->rv->csr_fcsr;

    /* 1. Ask the cart to flush any transient state into state buffers. */
    if (s->fn_on_save_state) {
        blyt_session_begin_fn_call(s, s->fn_on_save_state, 0, NULL);
        r = call_until_fn_done(s);
        if (r != BLYT_RUN_FN_DONE)
            goto restore;
    }

    /* 2. Snapshot state buffers. */
    snap = blyt_state_ctx_snapshot(&s->state_ctx);
    if (!snap)
        goto restore;

    /* 3. Zero state buffers and guest BSS (static vars). */
    blyt_state_ctx_zero_data(&s->state_ctx);
    blyt_session_zero_guest_bss(s);

    /* 4. Re-run init. */
    blyt_session_begin_fn_call(s, s->fn_init, 0, NULL);
    r = call_until_fn_done(s);
    if (r != BLYT_RUN_FN_DONE) {
        blyt_state_snapshot_free(snap);
        goto restore;
    }

    /* 5. Restore state buffers from snapshot. */
    blyt_state_ctx_restore_snapshot(&s->state_ctx, snap);
    blyt_state_snapshot_free(snap);

    /* 6. Notify cart that state has been restored (BLYT_LOAD_HOT_RELOAD = 3). */
    call_guest_on_load_state(s, 3u /* BLYT_LOAD_HOT_RELOAD */, 0u, 0u);

restore:
    /* Restore emulator state so the game loop continues from where it left off. */
    s->ctx.ecall_trapped = false;
    s->ctx.ecall_aborted = false;
    s->rv->PC = saved_pc;
    for (i = 1; i < 32; i++)
        rv_set_reg(s->rv, i, saved_regs[i]);
    s->rv->csr_fcsr = saved_fcsr;
    s->rv->halt = false;
}

/* -------------------------------------------------------------------------
 * Dev control channel host operations (issue #87)
 *
 * Each operation invokes guest lifecycle hooks (on_save_state / on_load_state)
 * via blyt_session_begin_fn_call.  Those host-initiated calls clobber the
 * emulator PC/registers, so we snapshot and restore them around each call —
 * the same technique blyt_reset_every_frame_cycle uses — leaving the running
 * game loop undisturbed.
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t pc;
    uint32_t regs[32];
    uint32_t fcsr;
} blyt_emu_state_t;

static void emu_state_save(blyt_session_t *s, blyt_emu_state_t *e) {
    uint32_t i;
    e->pc = s->rv->PC;
    for (i = 0; i < 32; i++)
        e->regs[i] = rv_get_reg(s->rv, i);
    e->fcsr = s->rv->csr_fcsr;
}

static void emu_state_restore(blyt_session_t *s, const blyt_emu_state_t *e) {
    uint32_t i;
    s->ctx.ecall_trapped = false;
    s->ctx.ecall_aborted = false;
    s->rv->PC = e->pc;
    for (i = 1; i < 32; i++)
        rv_set_reg(s->rv, i, e->regs[i]);
    s->rv->csr_fcsr = e->fcsr;
    s->rv->halt = false;
}

/* Call a guest lifecycle hook to completion, preserving emulator state. */
static void call_guest_hook(blyt_session_t *s, uint32_t fn, uint8_t nargs, const uint32_t *args) {
    blyt_emu_state_t e;
    if (fn == 0)
        return;
    emu_state_save(s, &e);
    blyt_session_begin_fn_call(s, fn, nargs, args);
    call_until_fn_done(s);
    emu_state_restore(s, &e);
}

/* Call blyt_cart_on_load_state(blyt_load_info_t), preserving emulator state.
 *
 * blyt_load_info_t is 12 bytes — larger than 2*XLEN — so the RV32 psABI passes
 * it *by reference*: the callee expects a0 to hold a pointer to the struct, not
 * the three fields spread across a0/a1/a2.  Materialise the struct in scratch
 * space just below the guest stack pointer and point a0 at it.  Setting sp to
 * the struct's address keeps it above the callee's own frame (which grows
 * downward from sp), so the hook reads it intact; emu_state_restore then puts
 * sp — and every other register — back when the hook returns. */
static void call_guest_on_load_state(blyt_session_t *s, uint32_t reason,
                                     uint32_t saved_cart_version, uint32_t buffers) {
    blyt_emu_state_t e;
    if (s->fn_on_load_state == 0)
        return;
    emu_state_save(s, &e);

    /* Field order must match blyt_load_info_t in runtime/guest/include/blyt.h:
     *   { reason (u32), saved_cart_version (u32), buffers (ptr) }. */
    const uint32_t info[3] = {reason, saved_cart_version, buffers};
    uint32_t sp = rv_get_reg(s->rv, rv_reg_sp);
    uint32_t scratch = (sp - (uint32_t)sizeof(info)) & ~(uint32_t)15; /* 16-byte aligned */
    memory_t *mem = PRIV(s->rv)->mem;
    if (GUEST_RAM_CONTAINS(mem, scratch, (uint32_t)sizeof(info))) {
        memory_write(mem, scratch, (const uint8_t *)info, (uint32_t)sizeof(info));
        rv_set_reg(s->rv, rv_reg_sp, scratch);
        uint32_t args[1] = {scratch};
        blyt_session_begin_fn_call(s, s->fn_on_load_state, 1, args);
        call_until_fn_done(s);
    }
    emu_state_restore(s, &e);
}

/* Call blyt_cart_on_assets_reloaded(const uint32_t *ids, size_t n) — the
 * dev-only asset hot-swap hook (issue #122) — preserving emulator state.
 *
 * Unlike blyt_load_info_t, the id array is variable-length, so we materialise
 * it in scratch space just below the guest stack pointer (16-byte aligned) and
 * pass a0 = pointer, a1 = count, mirroring the by-reference technique in
 * call_guest_on_load_state.  Setting sp to the array's address keeps it above
 * the callee's own frame; emu_state_restore puts sp and every other register
 * back when the hook returns.  No-op when the cart defines no hook (fn == 0),
 * the id set is empty, or the array does not fit in guest RAM. */
void blyt_session_notify_assets_reloaded(blyt_session_t *s, const uint32_t *ids, size_t n) {
    blyt_emu_state_t e;
    if (!s || s->fn_on_assets_reloaded == 0 || n == 0 || ids == NULL)
        return;
    uint32_t bytes = (uint32_t)(n * sizeof(uint32_t));
    if (bytes / sizeof(uint32_t) != n) /* overflow guard */
        return;
    emu_state_save(s, &e);
    uint32_t sp = rv_get_reg(s->rv, rv_reg_sp);
    uint32_t scratch = (sp - bytes) & ~(uint32_t)15; /* 16-byte aligned */
    memory_t *mem = PRIV(s->rv)->mem;
    if (GUEST_RAM_CONTAINS(mem, scratch, bytes)) {
        memory_write(mem, scratch, (const uint8_t *)ids, bytes);
        rv_set_reg(s->rv, rv_reg_sp, scratch);
        uint32_t args[2] = {scratch, (uint32_t)n};
        blyt_session_begin_fn_call(s, s->fn_on_assets_reloaded, 2, args);
        call_until_fn_done(s);
    }
    emu_state_restore(s, &e);
}

int blyt_session_save_state(blyt_session_t *s, uint32_t slot) {
    if (!s)
        return -1;
    /* Flush transient state into buffers, then serialise to disk. */
    call_guest_hook(s, s->fn_on_save_state, 0, NULL);
    return blyt_save_write(&s->state_ctx, s->ctx.save_dir, s->ctx.cart_name, slot,
                           s->ctx.save_version);
}

int blyt_session_load_state(blyt_session_t *s, uint32_t slot) {
    int r;
    uint32_t saved_version = 0;
    if (!s)
        return -1;
    r = blyt_save_read(&s->state_ctx, s->ctx.save_dir, s->ctx.cart_name, slot, &saved_version);
    if (r != 0)
        return r;
    /* Notify the cart (reason = BLYT_LOAD_SAVE_GAME = 0) with the version that
     * wrote the save, read from the save header (ADR-0087/0125). */
    call_guest_on_load_state(s, 0u, saved_version, 0u);
    return 0;
}

blyt_state_snapshot_t *blyt_session_snapshot(blyt_session_t *s) {
    if (!s)
        return NULL;
    call_guest_hook(s, s->fn_on_save_state, 0, NULL);
    return blyt_state_ctx_snapshot(&s->state_ctx);
}

void blyt_session_snapshot_free(blyt_state_snapshot_t *snap) {
    blyt_state_snapshot_free(snap);
}

void blyt_session_restore(blyt_session_t *s, blyt_state_snapshot_t *snap, uint32_t reason) {
    if (!snap)
        return;
    if (!s) {
        blyt_state_snapshot_free(snap);
        return;
    }
    blyt_state_ctx_restore_snapshot(&s->state_ctx, snap);
    blyt_state_snapshot_free(snap);
    call_guest_on_load_state(s, reason, 0u, 0u);
}

/* -------------------------------------------------------------------------
 * blyt_cart_run — blocking wrapper around the session API
 * ------------------------------------------------------------------------- */

blyt_cart_run_err_t blyt_cart_run(blyt_cart_t *cart, blyt_log_fn log_fn, blyt_frame_fn frame_fn,
                                  void *userdata) {
    blyt_session_t *s = blyt_session_create(cart, log_fn);
    if (!s)
        return BLYT_RUN_ERR_EMU;

    blyt_cart_run_err_t err;
    while ((err = blyt_session_run_frame(s)) == BLYT_RUN_FRAME_DONE) {
        if (frame_fn)
            frame_fn(userdata);
    }

    blyt_session_destroy(s);
    return err;
}
