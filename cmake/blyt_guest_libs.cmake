# cmake/blyt_guest_libs.cmake
#
# RV32IMAFC guest libraries, built by the main ninja graph DIRECTLY into the SDK
# layout (build/sdk/lib, lib/debug, lib/native, sdk/bin) so a plain `cmake
# --build build` keeps every guest artifact fresh — the sdk script
# (cmake/blyt_sdk.cmake) no longer compiles anything RV32 itself.
#
# Inputs (from cmake/blyt_rv32_toolchain.cmake): BLYT_RV32_CLANG / _LLD /
# _OBJCOPY.  When no toolchain is available every rule here is skipped, the same
# way the old inline rules were.
#
# Library set (ADR-0119/0120/0129/0130): sdk/lib/            libblytcommon,
# libblytc, libblyt32 (thick), libblytcommonlua, libblyt32lua,
# libblyt32lua-bridge — release: -O2, stripped (.dynsym kept) sdk/lib/debug/
# same set minus libblytcommonlua — -O0 -g, unstripped sdk/lib/native/
# trusted-native-exec variants for the QEMU gate sdk/bin/blyt-luac   host-native
# Lua bytecode compiler (LUA_32BITS)
#
# Aggregate targets: guest_libs (ALL), libblyt32_native_so (ALL, gated by
# BLYT_BUILD_NATIVE).

option(BLYT_BUILD_NATIVE
       "Build the native trusted-exec guest libs (RISC-V QEMU gate)" ON)

set(SDK_LIB "${CMAKE_BINARY_DIR}/sdk/lib")
set(SDK_LIB_DEBUG "${SDK_LIB}/debug")
set(SDK_LIB_NATIVE "${SDK_LIB}/native")
set(SDK_BIN "${CMAKE_BINARY_DIR}/sdk/bin")

# Release guest-lib paths consumed elsewhere in the main build (libretro embed,
# test_libretro_core gate).  Empty until the toolchain check passes.
set(LIBBLYTCOMMON_OUT "")
set(LIBBLYTC_OUT "")
set(LIBBLYT32_OUT "")
set(LIBBLYT32LUA_OUT "")

if(NOT BLYT_RV32_CLANG)
  message(STATUS "Guest libraries: skipped (no riscv32-capable clang)")
  return()
endif()

file(MAKE_DIRECTORY "${SDK_LIB}" "${SDK_LIB_DEBUG}" "${SDK_BIN}")

# ── Base flags ───────────────────────────────────────────────────────────────
set(RV32_BASE
    # riscv32-linux-gnu (not bare-metal riscv32): lld injects -static for
    # bare-metal targets which rejects .so inputs; using a Linux triple keeps
    # shared-library semantics.  -nostdlib suppresses the sysroot linkage.
    --target=riscv32-linux-gnu
    -march=rv32imafdc
    -mabi=ilp32d
    -shared
    -fPIC
    -nostdlib
    # The emulated↔native split overrides libblytcommon's ECALL stubs (e.g.
    # blyt_frame_done) via symbol preemption.  -O2 defaults to
    # -fno-semantic-interposition, which would bind libblytcommon's intra-module
    # calls (blyt_main → blyt_frame_done) directly to its own stub and ecall on
    # the native path (SIGSYS under the restricted seccomp filter).  Keep
    # interposition so the native libblyt32.so override binds (ADR-0129).
    -fsemantic-interposition
    -Wl,--shared
    -I
    "${CMAKE_SOURCE_DIR}/runtime/guest/include"
    "-fuse-ld=${BLYT_RV32_LLD}")

set(GUEST_INC_DIR "${CMAKE_SOURCE_DIR}/runtime/guest/include")
set(BLYT_H "${GUEST_INC_DIR}/blyt.h")
set(BLYT32_H "${GUEST_INC_DIR}/blyt32.h")
set(BLYT_LUA_INTERNAL_H "${GUEST_INC_DIR}/blyt_lua_internal.h")

# Per-variant settings (ADR-0129): release → SDK_LIB, -O2, stripped (no DWARF,
# no .symtab; --strip-unneeded keeps .dynsym so cart-facing exports survive);
# debug → SDK_LIB/debug, -O0 -g, unstripped.  A macro so it writes to the caller
# scope.
macro(blyt_set_variant _var)
  if("${_var}" STREQUAL "debug")
    set(_VDIR "${SDK_LIB_DEBUG}")
    set(_VOPT -O0 -g)
    set(_VSTRIP FALSE)
  else()
    set(_VDIR "${SDK_LIB}")
    set(_VOPT -O2)
    set(_VSTRIP TRUE)
  endif()
endmacro()

# Declare one guest .so as a single compile+link ninja rule.  ARGS = clang args
# after RV32_BASE (must include -o <out>); release variants get a strip pass
# appended.  Used for trivial single-source / non-file-source libs; the
# multi-source libs go through blyt_guest_so_objs below so each TU is its own
# cacheable, parallel, depfile-tracked rule.
function(blyt_guest_so out strip comment)
  cmake_parse_arguments(G "" "" "ARGS;DEPENDS" ${ARGN})
  set(_link COMMAND "${BLYT_RV32_CLANG}" ${RV32_BASE} ${G_ARGS})
  if(strip AND BLYT_RV32_OBJCOPY)
    list(
      APPEND
      _link
      COMMAND
      "${BLYT_RV32_OBJCOPY}"
      --strip-debug
      --strip-unneeded
      "${out}")
  endif()
  add_custom_command(
    OUTPUT "${out}" ${_link}
    DEPENDS ${G_DEPENDS}
    COMMENT "${comment}"
    VERBATIM)
endfunction()

# ── Per-object compilation ───────────────────────────────────────────────────
#
# Canonical source-path remapping for guest-lib DWARF (issue #46 §4).  Guest
# objects compile with absolute source paths; without these maps clang emits
# build-relative DW_AT_names against an absolute DW_AT_comp_dir, so the debug
# libs only resolve on the machine that built them and their bytes vary by build
# directory.  Each map rewrites a source tree to the /blyt/sdk/src/<component>
# layout the SDK ships (§5).  -ffile-prefix-map also rewrites DW_AT_comp_dir, so
# the build-dir map canonicalises comp_dir.  Applied to every guest TU; harmless
# in the stripped release variant (and keeps its __FILE__ strings canonical).
#
# Two forms per source tree.  When ccache is the launcher its `base_dir`
# rewrites absolute source paths to build-dir-relative before clang sees them
# (for cross-checkout cache hits), so clang records e.g. "../runtime/guest/…"
# and an absolute-only map would miss the file names — only comp_dir (clang's
# getcwd, not a ccache-rewritten argument) matches the absolute build-dir map.
# Without ccache, clang sees the absolute paths.  Emitting both the absolute and
# the build-dir-relative form covers both; only one matches any given path, the
# other is inert.  The relative form is computed against CMAKE_BINARY_DIR (the
# compile CWD) so it is correct for any build-directory location. rv32emu's
# softfloat sources are pulled into libblyt32lua via softfloat_builtins.c, so
# they appear in guest DWARF and need mapping + shipping too (only its
# src/softfloat subtree is referenced). The debug-only DAP master hook
# (runtime/host/src/dap/master_hook.{c,h}) is compiled into the debug
# libblyt32lua, so it appears in guest DWARF and is mapped + shipped under
# /blyt/sdk/src/blyt-dap.
file(RELATIVE_PATH _rel_guest "${CMAKE_BINARY_DIR}"
     "${CMAKE_SOURCE_DIR}/runtime/guest")
file(RELATIVE_PATH _rel_shared "${CMAKE_BINARY_DIR}"
     "${CMAKE_SOURCE_DIR}/runtime/shared")
file(RELATIVE_PATH _rel_musl "${CMAKE_BINARY_DIR}" "${musl_SOURCE_DIR}")
file(RELATIVE_PATH _rel_lua "${CMAKE_BINARY_DIR}" "${lua_SOURCE_DIR}")
file(RELATIVE_PATH _rel_rv32 "${CMAKE_BINARY_DIR}" "${rv32emu_SOURCE_DIR}")
file(RELATIVE_PATH _rel_dap "${CMAKE_BINARY_DIR}"
     "${CMAKE_SOURCE_DIR}/runtime/host/src/dap")
# runtime/shared is a sibling of runtime/guest (neither a prefix of the other),
# so it needs its own map: the unified-budget arena (blyt_arena.c, #158) is the
# first runtime/shared TU pulled into an emulated guest lib (libblytc), and
# without this its DWARF would leak the build machine path (source_paths.rs §4).
set(RV32_PREFIX_MAP
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}/runtime/guest=/blyt/sdk/src/blyt"
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}/runtime/shared=/blyt/sdk/src/blyt-shared"
    "-ffile-prefix-map=${musl_SOURCE_DIR}=/blyt/sdk/src/musl"
    "-ffile-prefix-map=${lua_SOURCE_DIR}=/blyt/sdk/src/lua"
    "-ffile-prefix-map=${rv32emu_SOURCE_DIR}=/blyt/sdk/src/rv32emu"
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}/runtime/host/src/dap=/blyt/sdk/src/blyt-dap"
    "-ffile-prefix-map=${_rel_guest}=/blyt/sdk/src/blyt"
    "-ffile-prefix-map=${_rel_shared}=/blyt/sdk/src/blyt-shared"
    "-ffile-prefix-map=${_rel_musl}=/blyt/sdk/src/musl"
    "-ffile-prefix-map=${_rel_lua}=/blyt/sdk/src/lua"
    "-ffile-prefix-map=${_rel_rv32}=/blyt/sdk/src/rv32emu"
    "-ffile-prefix-map=${_rel_dap}=/blyt/sdk/src/blyt-dap"
    "-ffile-prefix-map=${CMAKE_BINARY_DIR}=/blyt/sdk/build")

# Compile-phase subset of RV32_BASE: only flags that affect codegen of a -c
# compile.  Link-only flags (-shared, -nostdlib, -Wl,--shared, -fuse-ld) are
# excluded; -fsemantic-interposition stays (codegen flag, see RV32_BASE).
set(RV32_COMPILE_BASE
    --target=riscv32-linux-gnu
    -march=rv32imafdc
    -mabi=ilp32d
    -fPIC
    -fsemantic-interposition
    ${RV32_PREFIX_MAP}
    -I
    "${CMAKE_SOURCE_DIR}/runtime/guest/include"
    # runtime/shared carries the freestanding determinism core (blyt_arena.h,
    # blyt_mem_budget.h). libblytcommon_emu reads the unified accounting block to
    # serve blyt_mem_stats without an ECALL (#159), so every guest TU can reach
    # these headers; the matching -ffile-prefix-map is already global above.
    -I
    "${CMAKE_SOURCE_DIR}/runtime/shared")

set(GUEST_OBJ_ROOT "${CMAKE_BINARY_DIR}/guest-obj")

# ccache launcher for the per-object compiles (cmake/blyt_ccache.cmake); empty
# list when ccache is absent.
set(GUEST_CC_LAUNCHER)
if(BLYT_CCACHE_PROGRAM)
  set(GUEST_CC_LAUNCHER "${BLYT_CCACHE_PROGRAM}")
endif()

# Compile SRCS to one object per source under guest-obj/<objns>/, returning the
# object list (source order preserved — lld's first-wins symbol semantics and
# the ADR-0129 interposition layering depend on it) in <outvar>. Pre-built .o
# entries in SRCS pass through in place.  Each compile is its own ninja rule
# with a depfile, so header edits recompile exactly the affected objects, ninja
# parallelises within a library, and ccache caches every TU. BASE overrides the
# compile base flags (default RV32_COMPILE_BASE).
function(blyt_guest_objects outvar objns)
  cmake_parse_arguments(O "" "" "SRCS;CFLAGS;BASE" ${ARGN})
  if(NOT O_BASE)
    set(O_BASE ${RV32_COMPILE_BASE})
  endif()
  set(_objs)
  foreach(_src ${O_SRCS})
    if(_src MATCHES "\\.o$")
      list(APPEND _objs "${_src}")
      continue()
    endif()
    file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_src}")
    if(_rel MATCHES "^\\.\\.")
      # Source is outside the source tree (e.g. FetchContent cache).  A relative
      # path starting with ".." would escape the per-variant objns directory,
      # collapsing output paths across variants and triggering cmake's "already
      # has a custom rule" error.  Use an MD5-addressed path instead.
      get_filename_component(_fname "${_src}" NAME)
      string(MD5 _hash "${_src}")
      set(_rel "_ext/${_hash}/${_fname}")
    endif()
    set(_obj "${GUEST_OBJ_ROOT}/${objns}/${_rel}.o")
    get_filename_component(_objdir "${_obj}" DIRECTORY)
    file(MAKE_DIRECTORY "${_objdir}")
    add_custom_command(
      OUTPUT "${_obj}"
      COMMAND ${GUEST_CC_LAUNCHER} "${BLYT_RV32_CLANG}" ${O_BASE} ${O_CFLAGS}
              -MD -MF "${_obj}.d" -c "${_src}" -o "${_obj}"
      DEPENDS "${_src}"
      DEPFILE "${_obj}.d"
      COMMENT "CC(rv32) ${objns}/${_rel}"
      VERBATIM)
    list(APPEND _objs "${_obj}")
  endforeach()
  set(${outvar}
      "${_objs}"
      PARENT_SCOPE)
endfunction()

# Declare one guest .so from per-object compiles + a link step.  The link
# command must stay byte-for-byte equivalent to the old single-command form:
# same RV32_BASE, LINK_ARGS in the old argument order (soname, version scripts,
# --no-as-needed/--as-needed bracketed .so inputs), then -o and the objects in
# source order.  OBJNS namespaces the object dir (include the variant so
# release/debug objects never collide).  LINK_DEPENDS lists link inputs
# (.so/.sym) — kept off the compile rules so relinking a dependency does not
# recompile this library's objects.
function(blyt_guest_so_objs out strip comment)
  cmake_parse_arguments(G "" "OBJNS" "SRCS;CFLAGS;LINK_ARGS;LINK_DEPENDS"
                        ${ARGN})
  blyt_guest_objects(_objs "${G_OBJNS}" SRCS ${G_SRCS} CFLAGS ${G_CFLAGS})
  set(_link
      COMMAND
      "${BLYT_RV32_CLANG}"
      ${RV32_BASE}
      -Wno-unused-command-line-argument
      ${G_LINK_ARGS}
      -o
      "${out}"
      ${_objs})
  if(strip AND BLYT_RV32_OBJCOPY)
    list(
      APPEND
      _link
      COMMAND
      "${BLYT_RV32_OBJCOPY}"
      --strip-debug
      --strip-unneeded
      "${out}")
  endif()
  add_custom_command(
    OUTPUT "${out}" ${_link}
    DEPENDS ${_objs} ${G_LINK_DEPENDS}
    COMMENT "${comment}"
    VERBATIM)
endfunction()

# ── libblytcommon.so — variant-portable blyt API ────────────────────────────
# blyt_common.c is the portable lifecycle driver; blytcommon_emu.c holds the
# emulated-path impls of the variant-agnostic lifecycle/IO APIs (frame_done,
# console_debug, exit, runtime_startup) — their native counterparts live in
# frontends/native/src/libblytcommon/blytcommon.c (issue #128).
set(LIBBLYTCOMMON_SRC
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/blyt_common.c"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/blytcommon_emu.c"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/resources.c")
foreach(_var release debug)
  blyt_set_variant(${_var})
  blyt_guest_so_objs(
    "${_VDIR}/libblytcommon.so"
    ${_VSTRIP}
    "Linking libblytcommon.so (${_var})"
    OBJNS
    libblytcommon-${_var}
    SRCS
    ${LIBBLYTCOMMON_SRC}
    CFLAGS
    ${_VOPT}
    LINK_ARGS
    -Wl,-soname,libblytcommon.so)
endforeach()
set(LIBBLYTCOMMON_OUT "${SDK_LIB}/libblytcommon.so")

# ── blyt-debug-stub.elf — lldb-dap `program` stub (issue #119) ───────────────
# A minimal riscv32 ELF that lldb-dap loads as its `program` so the cart is
# never the main executable (and so stays a cleanly unloadable/reloadable shared
# library across a hot reload — Spike W §5d/§5e).  Debug-only; never run.
#
# Built as a STATIC ET_EXEC linked at a fixed high base (0x40000000) so its
# PT_LOAD segments cannot overlap the cart, which the emulated loader maps at
# guest base 0 (issue #119).  An ET_DYN stub does not work: lldb treats a shared
# object as relocatable and rebases it to ~0 regardless of its link-time
# --image-base (confirmed: lldb 22 loads an ET_DYN stub's .text at ~0x1250),
# colliding with the cart's text so it misattributes the cart's code to the stub
# and native frames resolve to line 0 (worst for hybrid carts, whose text spans
# the stub range).  An ET_EXEC has fixed vaddrs lldb honours on every platform.
# The stub is never mapped into the guest VM (it is purely lldb-side metadata),
# so 0x40000000 — above both the cart region and the runtime libs at 0x08000000
# — only needs to be a valid rv32 vaddr that no real module uses.  Entry is the
# stub function (never executed); -static -nostartfiles avoids a dynamic linker
# and a missing-_start warning.
add_custom_command(
  OUTPUT "${SDK_LIB_DEBUG}/blyt-debug-stub.elf"
  COMMAND
    "${BLYT_RV32_CLANG}" --target=riscv32-linux-gnu -march=rv32imafdc
    -mabi=ilp32d -no-pie -fno-pic -static -nostdlib -nostartfiles -O0 -g -I
    "${GUEST_INC_DIR}" "-fuse-ld=${BLYT_RV32_LLD}" -Wl,--image-base=0x40000000
    -Wl,-e,blyt_debug_stub
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/debug-stub/blyt_debug_stub.c" -o
    "${SDK_LIB_DEBUG}/blyt-debug-stub.elf"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/guest/src/debug-stub/blyt_debug_stub.c"
  COMMENT "Linking blyt-debug-stub.elf (lldb-dap program stub, issue #119)"
  VERBATIM)
add_custom_target(blyt_debug_stub ALL
                  DEPENDS "${SDK_LIB_DEBUG}/blyt-debug-stub.elf")

# ── musl bits/ generation (configure time) ──────────────────────────────────
set(MUSL_DIR "${musl_SOURCE_DIR}")
set(LIBBLYTC_BITS_DIR "${CMAKE_BINARY_DIR}/libblytc/bits")
set(LIBBLYTC_ALLTYPES_H "${LIBBLYTC_BITS_DIR}/alltypes.h")

if(EXISTS "${MUSL_DIR}/include/stdio.h")
  # Set up bits/: copy all arch/riscv32/bits/*.h, then generate the two that
  # need processing (alltypes.h from template; syscall.h from .in).
  file(MAKE_DIRECTORY "${LIBBLYTC_BITS_DIR}")
  file(GLOB _BITS_HDRS "${MUSL_DIR}/arch/riscv32/bits/*.h")
  foreach(_H ${_BITS_HDRS})
    file(COPY "${_H}" DESTINATION "${LIBBLYTC_BITS_DIR}")
  endforeach()

  execute_process(
    COMMAND sh -c "cat '${MUSL_DIR}/arch/riscv32/bits/alltypes.h.in' \
            '${MUSL_DIR}/include/alltypes.h.in' \
       | sed -f '${MUSL_DIR}/tools/mkalltypes.sed'"
    OUTPUT_FILE "${LIBBLYTC_ALLTYPES_H}"
    RESULT_VARIABLE _R
    ERROR_QUIET)
  if(NOT _R EQUAL 0)
    message(WARNING "libblytc.so: failed to generate bits/alltypes.h")
    set(MUSL_DIR "")
  endif()

  if(MUSL_DIR)
    execute_process(
      COMMAND
        sh -c "cp '${MUSL_DIR}/arch/riscv32/bits/syscall.h.in' \
            '${LIBBLYTC_BITS_DIR}/syscall.h' \
         && sed -n -e 's/__NR_/SYS_/p' \
             '${MUSL_DIR}/arch/riscv32/bits/syscall.h.in' \
             >> '${LIBBLYTC_BITS_DIR}/syscall.h'"
      RESULT_VARIABLE _R
      ERROR_QUIET)
    if(NOT _R EQUAL 0)
      message(WARNING "libblytc.so: failed to generate bits/syscall.h")
      set(MUSL_DIR "")
    endif()
  endif()
else()
  message(
    STATUS
      "libblytc.so: musl source not available — skipping \
(re-run cmake -B build to re-fetch, or clone blyt-tech/musl into third_party/musl)"
  )
  set(MUSL_DIR "")
endif()

if(NOT MUSL_DIR)
  # Without musl there is no libblytc, no thick libblyt32, no Lua libs.  Build a
  # minimal libblyt32.so (libblytcommon only) so the embed gate still works.
  foreach(_var release debug)
    blyt_set_variant(${_var})
    blyt_guest_so(
      "${_VDIR}/libblyt32.so"
      ${_VSTRIP}
      "Cross-compiling libblyt32.so (${_var}, no musl)"
      ARGS
      ${_VOPT}
      -Wl,-soname,libblyt32.so
      -Wl,-Bdynamic
      "${_VDIR}/libblytcommon.so"
      -o
      "${_VDIR}/libblyt32.so"
      "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32/blyt32.c"
      DEPENDS
      "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32/blyt32.c"
      "${BLYT_H}"
      "${_VDIR}/libblytcommon.so")
  endforeach()
  set(LIBBLYT32_OUT "${SDK_LIB}/libblyt32.so")
  add_custom_target(guest_libs ALL DEPENDS "${SDK_LIB}/libblyt32.so"
                                           "${SDK_LIB_DEBUG}/libblyt32.so")
  return()
endif()

# ── libblytc.so — trimmed musl-based C library (ADR-0120) ───────────────────
file(GLOB LIBBLYTC_STRING_SRCS "${MUSL_DIR}/src/string/*.c")
# strsignal calls __lctrans_cur (locale translation); exclude it.
list(REMOVE_ITEM LIBBLYTC_STRING_SRCS "${MUSL_DIR}/src/string/strsignal.c")
file(GLOB LIBBLYTC_MATH_SRCS "${MUSL_DIR}/src/math/*.c")
file(GLOB LIBBLYTC_CTYPE_SRCS "${MUSL_DIR}/src/ctype/*.c")

set(LIBBLYTC_SRCS
    ${LIBBLYTC_STRING_SRCS}
    ${LIBBLYTC_MATH_SRCS}
    ${LIBBLYTC_CTYPE_SRCS}
    "${MUSL_DIR}/src/stdlib/strtol.c"
    "${MUSL_DIR}/src/stdlib/strtod.c"
    "${MUSL_DIR}/src/stdlib/qsort.c"
    "${MUSL_DIR}/src/stdlib/qsort_nr.c"
    "${MUSL_DIR}/src/stdlib/abs.c"
    "${MUSL_DIR}/src/stdlib/atoi.c"
    "${MUSL_DIR}/src/stdlib/atol.c"
    "${MUSL_DIR}/src/stdlib/atof.c"
    "${MUSL_DIR}/src/internal/floatscan.c"
    "${MUSL_DIR}/src/internal/intscan.c"
    "${MUSL_DIR}/src/internal/shgetc.c"
    "${MUSL_DIR}/src/stdio/vsnprintf.c"
    "${MUSL_DIR}/src/stdio/snprintf.c"
    "${MUSL_DIR}/src/stdio/vfprintf.c"
    "${MUSL_DIR}/src/stdio/fwrite.c"
    "${MUSL_DIR}/src/stdio/fmemopen.c"
    "${MUSL_DIR}/src/stdio/__overflow.c"
    "${MUSL_DIR}/src/stdio/__towrite.c"
    "${MUSL_DIR}/src/stdio/__toread.c"
    "${MUSL_DIR}/src/stdio/__uflow.c"
    "${MUSL_DIR}/src/stdio/__fmodeflags.c"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytc/blytc_arena.c"
    "${CMAKE_SOURCE_DIR}/runtime/shared/blyt_arena.c"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytc/blytc_stubs.c"
    "${MUSL_DIR}/src/malloc/posix_memalign.c"
    "${MUSL_DIR}/src/stdio/sscanf.c"
    "${MUSL_DIR}/src/stdio/vsscanf.c"
    "${MUSL_DIR}/src/stdio/vfscanf.c"
    "${MUSL_DIR}/src/setjmp/riscv32/setjmp.S"
    "${MUSL_DIR}/src/setjmp/riscv32/longjmp.S")

set(LIBBLYTC_INCLUDES
    -I
    "${CMAKE_SOURCE_DIR}/runtime/shared" # blyt_arena.h / blyt_mem_budget.h
                                         # (#158)
    -I
    "${MUSL_DIR}/src/include" # defines `hidden` and other internal macros
    -I
    "${MUSL_DIR}/include"
    -I
    "${MUSL_DIR}/arch/riscv32"
    -I
    "${MUSL_DIR}/arch/generic"
    -I
    "${MUSL_DIR}/src/internal"
    -I
    "${LIBBLYTC_BITS_DIR}/..")

# Determinism (ADR-0007) and musl-compatibility compile flags.  musl relies on
# unparenthesised shift idioms (`x >> 64-d`, `1U<<*s-' '`) that are
# range-guarded and well-defined; upstream builds itself with -w under clang.
set(LIBBLYTC_CFLAGS
    -ffp-contract=off
    -fno-fast-math
    -fno-strict-aliasing
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-implicit-fallthrough
    -Wno-unused-variable
    -Wno-deprecated-non-prototype
    -Wno-shift-op-parentheses)

foreach(_var release debug)
  blyt_set_variant(${_var})
  blyt_guest_so_objs(
    "${_VDIR}/libblytc.so"
    ${_VSTRIP}
    "Linking libblytc.so (${_var}, musl subset)"
    OBJNS
    libblytc-${_var}
    SRCS
    ${LIBBLYTC_SRCS}
    CFLAGS
    ${LIBBLYTC_INCLUDES}
    ${LIBBLYTC_CFLAGS}
    ${_VOPT}
    LINK_ARGS
    -Wl,-soname,libblytc.so)
endforeach()
set(LIBBLYTC_OUT "${SDK_LIB}/libblytc.so")

# ── Berkeley SoftFloat — compiler-rt soft-double/quad ABI builtins ──────────
#
# clang emits calls to __adddf3/__muldf3/__udivdi3/__extendsfdf2/etc. for any
# cart code doing double or 64-bit-integer arithmetic on this hardware-single-
# float target.  musl does NOT provide these.  Compiled into BOTH libblyt32.so
# and libblyt32lua.so so every cart resolves them from the .so's .dynsym
# (cart_load.c's import allowlist already permits them, ADR-0112).  Same RISC-V
# NaN-propagation specialisation rv32emu uses on the host, so behaviour is
# bit-identical to the emulator.
set(SF_SRC "${rv32emu_SOURCE_DIR}/src/softfloat/source")
set(BLYT_HAVE_SOFTFLOAT FALSE)
if(EXISTS "${SF_SRC}/f64_add.c")
  set(BLYT_HAVE_SOFTFLOAT TRUE)
  set(SF_PLATFORM_DIR "${CMAKE_BINARY_DIR}/softfloat-rv32")
  file(MAKE_DIRECTORY "${SF_PLATFORM_DIR}")
  # Minimal platform.h: disable thread-local so softfloat_roundingMode is
  # global, and declare the target little-endian.  LITTLEENDIAN is required for
  # SoftFloat's float128_t to use {v0(low), v64(high)} word order; without it
  # SoftFloat defaults to big-endian word order (high word at v[0]), which
  # silently mismatches the compiler's IEEE binary128 memory layout (Spike U:
  # this broke quad<->compiler interchange, e.g. musl's long-double printf).
  # f32/f64 are single-word and so were unaffected.
  file(WRITE "${SF_PLATFORM_DIR}/platform.h"
       "#define THREAD_LOCAL\n#define LITTLEENDIAN 1\n")

  # Core SoftFloat: all s_*.c and f32/f64/f128/conversion files.  Exclude extF80
  # (80-bit), M-variant (multi-word array), bf16, f16.
  file(GLOB SF_ALL "${SF_SRC}/*.c")
  # Exclude macOS AppleDouble sidecar files (._*) that libarchive may extract
  # from tarballs containing LIBARCHIVE.xattr PAX headers.
  list(FILTER SF_ALL EXCLUDE REGEX "/\\._")
  foreach(
    _EXCL_PATTERN
    "${SF_SRC}/extF80*"
    "${SF_SRC}/*M_*"
    "${SF_SRC}/*M.*"
    "${SF_SRC}/bf16*"
    "${SF_SRC}/f16*"
    "${SF_SRC}/f32_to_extF80*"
    "${SF_SRC}/f64_to_extF80*"
    "${SF_SRC}/f128_to_extF80*")
    file(GLOB _EXCL_FILES "${_EXCL_PATTERN}")
    list(REMOVE_ITEM SF_ALL ${_EXCL_FILES})
  endforeach()
  # RISC-V NaN propagation specialisations (specialize.h declares them all).
  file(
    GLOB
    SF_RISCV
    "${SF_SRC}/RISCV/s_propagateNaNF16UI.c"
    "${SF_SRC}/RISCV/s_propagateNaNF32UI.c"
    "${SF_SRC}/RISCV/s_propagateNaNF64UI.c"
    "${SF_SRC}/RISCV/s_propagateNaNF128UI.c"
    "${SF_SRC}/RISCV/s_propagateNaNExtF80UI.c"
    "${SF_SRC}/RISCV/s_f16UIToCommonNaN.c"
    "${SF_SRC}/RISCV/s_f32UIToCommonNaN.c"
    "${SF_SRC}/RISCV/s_f64UIToCommonNaN.c"
    "${SF_SRC}/RISCV/s_f128UIToCommonNaN.c"
    "${SF_SRC}/RISCV/s_extF80UIToCommonNaN.c"
    "${SF_SRC}/RISCV/s_commonNaNToF16UI.c"
    "${SF_SRC}/RISCV/s_commonNaNToF32UI.c"
    "${SF_SRC}/RISCV/s_commonNaNToF64UI.c"
    "${SF_SRC}/RISCV/s_commonNaNToF128UI.c"
    "${SF_SRC}/RISCV/s_commonNaNToExtF80UI.c"
    "${SF_SRC}/RISCV/softfloat_raiseFlags.c")
  # Multi-word-array helpers needed by f128_mul even with SOFTFLOAT_FAST_INT64.
  set(SF_MWORD "${SF_SRC}/s_add256M.c" "${SF_SRC}/s_sub256M.c"
               "${SF_SRC}/s_mul128To256M.c" "${SF_SRC}/s_shiftRightJam256M.c")
  set(SF_INCLUDES -I "${SF_SRC}/include" -I "${SF_SRC}/RISCV" -I
                  "${SF_PLATFORM_DIR}")
  set(SF_DEFINES -DSOFTFLOAT_FAST_INT64=1 -DSOFTFLOAT_ROUND_ODD=1)
  set(SF_BUILTINS
      "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32lua/softfloat_builtins.c")
else()
  message(STATUS "Guest SoftFloat: rv32emu source not available — \
libblyt32.so will lack double/64-bit builtins")
  set(SF_ALL "")
  set(SF_RISCV "")
  set(SF_MWORD "")
  set(SF_INCLUDES "")
  set(SF_DEFINES "")
  set(SF_BUILTINS "")
endif()

# ── libblyt32.so — Blyt32 ECALL stubs (thick) ───────────────────────────────
#
# Self-contained for cart LINK TIME: absorbs libblytcommon sources and ALL
# libblytc sources so carts need only -lblyt32 at build time.  Exporting
# malloc/free/string/math directly from libblyt32.so's .dynsym lets lld resolve
# them without adding libblytc.so to the cart's DT_NEEDED.
#
# Runtime: libblyt32.so declares DT_NEEDED: libblytc.so (forced via
# --no-as-needed); the runtime's BFS dynamic loader picks it up transitively.
# First-wins symbol rule: libblyt32.so's baked-in copies win on the
# emulated/libretro path; libblytc.so's are shadowed but present for the
# hardware trusted-exec path where ld.so resolves against it directly.
foreach(_var release debug)
  blyt_set_variant(${_var})
  blyt_guest_so_objs(
    "${_VDIR}/libblyt32.so"
    ${_VSTRIP}
    "Linking libblyt32.so (${_var})"
    OBJNS
    libblyt32-${_var}
    SRCS
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32/blyt32.c"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/blyt_common.c"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/blytcommon_emu.c"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/resources.c"
    # Tier-2 in-lock primitives (#205): the shared integer rasterizer, so a cart
    # holding a surface lock draws into the materialized buffer guest-side (zero
    # ECALL) via the SAME source the host tier-1 handlers use — the tier-1 ≡
    # tier-2 determinism guarantee holds by construction.
    "${CMAKE_SOURCE_DIR}/runtime/shared/blyt_raster.c"
    ${LIBBLYTC_SRCS}
    ${SF_ALL}
    ${SF_RISCV}
    ${SF_MWORD}
    ${SF_BUILTINS}
    CFLAGS
    ${LIBBLYTC_INCLUDES}
    ${LIBBLYTC_CFLAGS}
    ${SF_INCLUDES}
    ${SF_DEFINES}
    ${_VOPT}
    LINK_ARGS
    -Wl,-soname,libblyt32.so
    -Wl,--no-as-needed
    "${_VDIR}/libblytc.so"
    -Wl,--as-needed
    LINK_DEPENDS
    "${_VDIR}/libblytc.so")
endforeach()
set(LIBBLYT32_OUT "${SDK_LIB}/libblyt32.so")

set(_guest_lib_outputs
    "${SDK_LIB}/libblytcommon.so" "${SDK_LIB}/libblytc.so"
    "${SDK_LIB}/libblyt32.so" "${SDK_LIB_DEBUG}/libblytcommon.so"
    "${SDK_LIB_DEBUG}/libblytc.so" "${SDK_LIB_DEBUG}/libblyt32.so")

# ── Lua guest libraries (ADR-0025/0066/0111/0130) ───────────────────────────
set(LUA_DIR "${lua_SOURCE_DIR}")
if(NOT EXISTS "${LUA_DIR}/lvm.c")
  message(STATUS "Lua guest libraries: skipped \
(re-run cmake -B build to re-fetch, or clone lua/lua into third_party/lua)")
else()
  file(GLOB LUA_GUEST_SRCS "${LUA_DIR}/*.c")
  # Remove standalone interpreter, bytecode compiler, and excluded sandboxed
  # libs (no I/O, no OS access, no dlopen, no debug hooks).  utf8 is KEPT:
  # ADR-0079 allows it (read-only iteration utilities; deterministic) and carts
  # need character-level UTF-8 (issue #167).
  foreach(
    _EXCL
    "${LUA_DIR}/lua.c"
    "${LUA_DIR}/luac.c"
    "${LUA_DIR}/onelua.c"
    "${LUA_DIR}/liolib.c"
    "${LUA_DIR}/loslib.c"
    "${LUA_DIR}/loadlib.c"
    "${LUA_DIR}/ldblib.c")
    list(REMOVE_ITEM LUA_GUEST_SRCS "${_EXCL}")
  endforeach()

  # Public musl headers for Lua: the standard include paths minus musl's
  # internal src/include/, which defines `weak` as an attribute macro that
  # collides with Lua's `GCObject *weak` field in lstate.h.
  set(LUA_MUSL_INCLUDES
      -I
      "${MUSL_DIR}/include"
      -I
      "${MUSL_DIR}/arch/riscv32"
      -I
      "${MUSL_DIR}/arch/generic"
      -I
      "${LIBBLYTC_BITS_DIR}/..")

  # Fixed Lua hash seed (ADR-0130/ADR-0066): identical string hashing — and
  # therefore pairs()/lua_next order, table.sort pivots, math.random streams —
  # across the rv32 and WASM execution paths.  Must match every Lua build (guest
  # libraries, host WASM Lua, blyt-luac).  0x424C5954 = "BLYT".
  set(LUA_SEED_DEF "-Dluai_makeseed()=0x424C5954u")

  set(LUA32_SYM
      "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32lua/blyt32lua.sym")
  set(LUA32_DIR "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32lua")

  # libblytcommonlua.so — standalone sandboxed Lua VM (tooling use; carts get
  # the VM via libblyt32lua.so below).
  blyt_guest_so_objs(
    "${SDK_LIB}/libblytcommonlua.so"
    FALSE
    "Linking libblytcommonlua.so"
    OBJNS
    libblytcommonlua
    SRCS
    ${LUA_GUEST_SRCS}
    CFLAGS
    ${LUA_MUSL_INCLUDES}
    ${LIBBLYTC_CFLAGS}
    -DBLYT_LUA_I32_F64=1
    -DLUA_USE_LONGJMP=1
    ${LUA_SEED_DEF}
    -I
    "${LUA_DIR}"
    LINK_ARGS
    -Wl,-soname,libblytcommonlua.so)
  list(APPEND _guest_lib_outputs "${SDK_LIB}/libblytcommonlua.so")

  # libblyt32lua.so — Lua VM embedded directly (exports lua_*/luaL_* in .dynsym)
  # + blyt32 API bindings + cart lifecycle + everything libblytc does NOT
  # provide (setjmp/longjmp, soft-float ops, stubs).  ADR-0129: the guest-side
  # DAP master hook (per-instruction stepping) is debug-only — the debug variant
  # defines BLYT_DAP=1 and links the master hook sources.
  foreach(_var release debug)
    blyt_set_variant(${_var})
    if("${_var}" STREQUAL "debug")
      set(_VLUA_DAP_FLAGS -DBLYT_DAP=1 -I
                          "${CMAKE_SOURCE_DIR}/runtime/host/src/dap")
      set(_VLUA_DAP_SRCS
          "${CMAKE_SOURCE_DIR}/runtime/host/src/dap/master_hook.c"
          "${LUA32_DIR}/master_hook_ecall.c")
    else()
      set(_VLUA_DAP_FLAGS "")
      set(_VLUA_DAP_SRCS "")
    endif()
    blyt_guest_so_objs(
      "${_VDIR}/libblyt32lua.so"
      ${_VSTRIP}
      "Linking libblyt32lua.so (${_var})"
      OBJNS
      libblyt32lua-${_var}
      SRCS
      ${LUA_GUEST_SRCS}
      "${MUSL_DIR}/src/setjmp/riscv32/setjmp.S"
      "${MUSL_DIR}/src/setjmp/riscv32/longjmp.S"
      ${SF_ALL}
      ${SF_RISCV}
      ${SF_MWORD}
      "${LUA32_DIR}/softfloat_builtins.c"
      "${LUA32_DIR}/lua_runtime_stubs.c"
      "${LUA32_DIR}/blyt32lua.c"
      ${_VLUA_DAP_SRCS}
      CFLAGS
      ${LUA_MUSL_INCLUDES}
      ${LIBBLYTC_CFLAGS}
      ${_VOPT}
      -DBLYT_LUA_I32_F64=1
      -DLUA_USE_LONGJMP=1
      ${LUA_SEED_DEF}
      ${_VLUA_DAP_FLAGS}
      -I
      "${LUA_DIR}"
      ${SF_INCLUDES}
      ${SF_DEFINES}
      LINK_ARGS
      -Wl,-soname,libblyt32lua.so
      "-Wl,--version-script,${LUA32_SYM}"
      -Wl,--as-needed
      "${_VDIR}/libblyt32.so"
      LINK_DEPENDS
      "${LUA32_SYM}"
      "${_VDIR}/libblyt32.so")
    list(APPEND _guest_lib_outputs "${_VDIR}/libblyt32lua.so")
  endforeach()
  set(LIBBLYT32LUA_OUT "${SDK_LIB}/libblyt32lua.so")

  # libblyt32lua-bridge.so (ADR-0130) — the WASM-target variant: no Lua VM,
  # every lua_* export is a BLYT_ECALL_LUA_OP stub serviced by the host.  Same
  # soname and export surface (blyt32lua.sym), so the WASM frontend embeds it
  # under the name "libblyt32lua.so" and carts need no rebuild.
  set(LUA_BRIDGE_SRC
      "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32lua-bridge/blyt32lua_bridge.c"
  )
  foreach(_var release debug)
    blyt_set_variant(${_var})
    blyt_guest_so_objs(
      "${_VDIR}/libblyt32lua-bridge.so"
      ${_VSTRIP}
      "Linking libblyt32lua-bridge.so (${_var})"
      OBJNS
      libblyt32lua-bridge-${_var}
      SRCS
      "${LUA_BRIDGE_SRC}"
      CFLAGS
      ${LIBBLYTC_CFLAGS}
      ${_VOPT}
      LINK_ARGS
      -Wl,-soname,libblyt32lua.so
      "-Wl,--version-script,${LUA32_SYM}"
      -Wl,--no-as-needed
      "${_VDIR}/libblyt32.so"
      -Wl,--as-needed
      LINK_DEPENDS
      "${LUA32_SYM}"
      "${_VDIR}/libblyt32.so")
    list(APPEND _guest_lib_outputs "${_VDIR}/libblyt32lua-bridge.so")
  endforeach()

  # blyt-luac — host-native Lua bytecode compiler (BLYT_LUA_I32_F64=1 to match
  # the guest VMs' 4-byte lua_Integer / lua_Number).
  file(GLOB LUA_HOST_SRCS "${LUA_DIR}/*.c")
  foreach(_EXCL "${LUA_DIR}/lua.c" "${LUA_DIR}/onelua.c" "${LUA_DIR}/ltests.c")
    list(REMOVE_ITEM LUA_HOST_SRCS "${_EXCL}")
  endforeach()
  add_custom_command(
    OUTPUT "${SDK_BIN}/blyt-luac"
    COMMAND
      "${BLYT_RV32_CLANG}" -DBLYT_LUA_I32_F64=1 ${LUA_SEED_DEF} -O2 -I
      "${LUA_DIR}" -Wno-unused-parameter -Wno-sign-compare
      -Wno-implicit-fallthrough -Wno-deprecated-non-prototype -o
      "${SDK_BIN}/blyt-luac" ${LUA_HOST_SRCS}
      "${CMAKE_SOURCE_DIR}/runtime/tools/blyt-luac.c" -lm
    DEPENDS ${LUA_HOST_SRCS} "${CMAKE_SOURCE_DIR}/runtime/tools/blyt-luac.c"
    COMMENT "Compiling blyt-luac (host-native, BLYT_LUA_I32_F64=1)"
    VERBATIM)
  list(APPEND _guest_lib_outputs "${SDK_BIN}/blyt-luac")
endif()

add_custom_target(guest_libs ALL DEPENDS ${_guest_lib_outputs})
message(STATUS "Guest libraries → ${SDK_LIB} (release + debug)")

# ═════════════════════════════════════════════════════════════════════════════
# Native trusted-exec guest libs (sdk/lib/native) — RISC-V QEMU gate
#
# /lib/ld-blyt.so.1 on hardware is the system musl ILP32 interpreter.  The
# native chain: libblyt32.so → libblytc.so (thin DT_NEEDED carrier) →
# ld-blyt.so.1, which makes malloc/getenv/snprintf/… resolvable at runtime.
# ═════════════════════════════════════════════════════════════════════════════
if(BLYT_BUILD_NATIVE)
  file(MAKE_DIRECTORY "${SDK_LIB_NATIVE}" "${CMAKE_BINARY_DIR}/native")

  # ld-blyt.so.1 stub: empty shared lib with SONAME=ld-blyt.so.1.  Used only at
  # link time so lld emits DT_NEEDED: ld-blyt.so.1 in libblytc.so.  Never staged
  # to QEMU; the real /lib/ld-blyt.so.1 (system musl) is loaded by the kernel as
  # the ELF interpreter.
  set(LD_BLYT_STUB_SRC
      "${CMAKE_SOURCE_DIR}/frontends/native/src/libblytc/ld_blyt_stub.c")
  set(LD_BLYT_STUB_OUT "${CMAKE_BINARY_DIR}/native/ld-blyt.so.1.stub")
  blyt_guest_so(
    "${LD_BLYT_STUB_OUT}"
    FALSE
    "Cross-compiling ld-blyt.so.1 stub"
    ARGS
    -O2
    -Wl,-soname,ld-blyt.so.1
    -o
    "${LD_BLYT_STUB_OUT}"
    "${LD_BLYT_STUB_SRC}"
    DEPENDS
    "${LD_BLYT_STUB_SRC}")

  # Native libblytc.so: carries DT_NEEDED ld-blyt.so.1 so carts and libblyt32.so
  # resolve stdlib symbols from the system musl interpreter at runtime on QEMU,
  # and now defines the cart-heap malloc family backed by the runtime/shared
  # arena hosted in libblytcommon.so (#158) — hence DT_NEEDED libblytcommon.so to
  # resolve blyt_cart_heap_*.  libblytcommon.so is built later in this file; the
  # DEPENDS on its output orders the build (ninja is dependency- not
  # declaration-ordered).
  set(LIBBLYTC_NATIVE_SRC
      "${CMAKE_SOURCE_DIR}/frontends/native/src/libblytc/libblytc_native.c")
  blyt_guest_so(
    "${SDK_LIB_NATIVE}/libblytc.so"
    FALSE
    "Cross-compiling libblytc.so (native, DT_NEEDED: ld-blyt.so.1 + libblytcommon.so)"
    ARGS
    -O2
    -Wl,-soname,libblytc.so
    -Wl,-Bdynamic
    -Wl,--no-as-needed
    "${LD_BLYT_STUB_OUT}"
    "${SDK_LIB_NATIVE}/libblytcommon.so"
    -Wl,--as-needed
    -o
    "${SDK_LIB_NATIVE}/libblytc.so"
    "${LIBBLYTC_NATIVE_SRC}"
    DEPENDS
    "${LIBBLYTC_NATIVE_SRC}"
    "${LD_BLYT_STUB_OUT}"
    "${SDK_LIB_NATIVE}/libblytcommon.so")

  set(LIBBLYT32_NATIVE_INC "${CMAKE_SOURCE_DIR}/frontends/native/src/libblyt32")
  set(LIBBLYTCOMMON_NATIVE_DIR
      "${CMAKE_SOURCE_DIR}/frontends/native/src/libblytcommon")
  set(SHARED_DIR "${CMAKE_SOURCE_DIR}/runtime/shared")

  # Native libblytcommon.so (issue #128): the variant-agnostic real-work impls
  # (state buffers, save/load, frame_done FCSR, console_debug, startup, exit),
  # recompiled from the native source set — NOT copied from the emulated variant
  # — plus the portable lifecycle driver (blyt_common.c, shared with the
  # emulated variant) and the freestanding runtime/shared determinism
  # primitives.  Keep -O0: the native impls use hand-rolled helpers and raw
  # inline-asm syscalls; -O0 avoids loop-idiom rewrites into libcalls.  Built
  # via the multi-source path so each TU (incl. runtime/shared) is its own
  # cacheable, depfile-tracked rule.  Strong definitions in this variant are
  # what the cart resolves over the DT_NEEDED chain (-fsemantic-interposition in
  # RV32_BASE keeps intra-module calls like blyt_main → blyt_frame_done routing
  # through the PLT, ADR-0129).
  blyt_guest_so_objs(
    "${SDK_LIB_NATIVE}/libblytcommon.so"
    FALSE
    "Linking libblytcommon.so (native)"
    OBJNS
    libblytcommon-native
    SRCS
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/blyt_common.c"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/resources.c"
    "${LIBBLYTCOMMON_NATIVE_DIR}/blytcommon.c"
    "${SHARED_DIR}/blyt_arena.c" # single-sourced cart-heap arena (#158)
    "${SHARED_DIR}/blyt_fp_canon.c"
    "${SHARED_DIR}/blyt_elf_section.c"
    "${SHARED_DIR}/blyt_resource_codec.c" # per-resource decode (#157)
    "${SHARED_DIR}/blyt_blys.c"
    # zstd decode-only (common + decompress) cross-compiled into the native lib
    # so a compressed resource decodes bit-identically on bare metal (#157,
    # ADR-0026).  Its libc calls (memcpy/malloc/free/…) resolve as dynamic
    # undefs against system musl over the DT_NEEDED chain, like the rest of this
    # lib.  ASM disabled + NDEBUG: pure-C, assert-free decode.
    ${ZSTD_DECODE_SOURCES}
    CFLAGS
    -O0
    # NB: do NOT add -DNDEBUG here — this lib's FCSR determinism check is
    # `#ifndef NDEBUG` (warn vs abort), and the native variant must warn (#157
    # regression: a global -DNDEBUG for zstd's asserts flipped it to abort).
    # zstd's own asserts resolve __assert_fail via musl and never fire on valid
    # frames.
    -DZSTD_DISABLE_ASM=1
    -I
    "${LIBBLYTCOMMON_NATIVE_DIR}"
    -I
    "${LIBBLYT32_NATIVE_INC}" # seccomp_restricted.h
    -I
    "${SHARED_DIR}"
    -I
    "${ZSTD_LIB_DIR}"
    # musl headers (string.h/stdlib.h) for the bundled zstd decode sources —
    # same RV32 set the libc++ cross-build uses (blyt_sdk.cmake).  The other TUs
    # in this lib don't include libc headers, so this only feeds zstd.
    -isystem
    "${MUSL_DIR}/include"
    -isystem
    "${MUSL_DIR}/arch/riscv32"
    -isystem
    "${MUSL_DIR}/arch/generic"
    -isystem
    "${LIBBLYTC_BITS_DIR}/.."
    LINK_ARGS
    -Wl,-soname,libblytcommon.so)

  # Native libblyt32.so: the Blyt32 variant-specific graphics surface (issue
  # #188).  Compiles the SAME shared integer rasterizer + frame-hash sources
  # (runtime/shared) the host and wasm legs use, so the bare-metal pixels are
  # bit-identical (the Q2 proof).  Built via the multi-source path so each TU
  # (incl. runtime/shared) is its own cacheable, depfile-tracked rule.  It is the
  # cart's direct DT_NEEDED and carries DT_NEEDED libblytcommon.so + libblytc.so
  # so the cart resolves the relocated symbols and the system C library over the
  # chain.  -O2 like the host raster compile; the rasterizer avoids signed
  # overflow via int64 intermediates (no -fwrapv needed — same as host/wasm).
  set(LIBBLYT32_NATIVE_SRC
      "${CMAKE_SOURCE_DIR}/frontends/native/src/libblyt32/blyt32.c")
  blyt_guest_so_objs(
    "${SDK_LIB_NATIVE}/libblyt32.so"
    FALSE
    "Linking libblyt32.so (native)"
    OBJNS
    libblyt32-native
    SRCS
    "${LIBBLYT32_NATIVE_SRC}"
    "${SHARED_DIR}/blyt_raster.c"
    "${SHARED_DIR}/blyt_frame_hash.c"
    CFLAGS
    -O2
    -I
    "${LIBBLYT32_NATIVE_INC}"
    -I
    "${LIBBLYTCOMMON_NATIVE_DIR}" # blyt_native_trace.h (getenv + write helpers)
    -I
    "${SHARED_DIR}"
    # musl headers for blyt_raster.c's <string.h> (memset); memset resolves as a
    # dynamic undef against libblytc/musl over the DT_NEEDED chain, like the
    # bundled zstd decode sources in libblytcommon.  Same RV32 musl header set.
    -isystem
    "${MUSL_DIR}/include"
    -isystem
    "${MUSL_DIR}/arch/riscv32"
    -isystem
    "${MUSL_DIR}/arch/generic"
    -isystem
    "${LIBBLYTC_BITS_DIR}/.."
    LINK_ARGS
    -Wl,-soname,libblyt32.so
    -Wl,-Bdynamic
    -Wl,--no-as-needed
    "${SDK_LIB_NATIVE}/libblytcommon.so"
    "${SDK_LIB_NATIVE}/libblytc.so"
    -Wl,--as-needed
    LINK_DEPENDS
    "${BLYT_H}"
    "${SDK_LIB_NATIVE}/libblytcommon.so"
    "${SDK_LIB_NATIVE}/libblytc.so")

  set(_native_outputs
      "${SDK_LIB_NATIVE}/libblyt32.so" "${SDK_LIB_NATIVE}/libblytc.so"
      "${SDK_LIB_NATIVE}/libblytcommon.so")

  # ── RV32 native test binaries (QEMU gates 2, 5, 6) ──────────────────────────
  # Self-contained executables compiled with -nostdlib; no musl sysroot needed.
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-rv32")

  set(_RV32_EXE_FLAGS
      --target=riscv32-linux-gnu
      -march=rv32imafc
      -mabi=ilp32f
      -O2
      -nostdlib
      -static
      "-fuse-ld=${BLYT_RV32_LLD}")

  set(_SECCOMP_TEST_OUT "${CMAKE_BINARY_DIR}/test-rv32/seccomp_restricted_test")
  add_custom_command(
    OUTPUT "${_SECCOMP_TEST_OUT}"
    COMMAND
      "${BLYT_RV32_CLANG}" ${_RV32_EXE_FLAGS} -I "${LIBBLYT32_NATIVE_INC}" -o
      "${_SECCOMP_TEST_OUT}"
      "${CMAKE_SOURCE_DIR}/tests/native/seccomp_restricted_test.c"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/native/seccomp_restricted_test.c"
            "${LIBBLYT32_NATIVE_INC}/seccomp_restricted.h"
    COMMENT "Cross-compiling seccomp_restricted_test (RV32 ILP32F)"
    VERBATIM)

  set(_FCSR_TEST_SRC "${CMAKE_SOURCE_DIR}/tests/native/fcsr_frame_test.c")
  set(_FCSR_DEBUG_OUT "${CMAKE_BINARY_DIR}/test-rv32/fcsr_debug_test")
  set(_FCSR_RELEASE_OUT "${CMAKE_BINARY_DIR}/test-rv32/fcsr_release_test")
  add_custom_command(
    OUTPUT "${_FCSR_DEBUG_OUT}"
    COMMAND "${BLYT_RV32_CLANG}" ${_RV32_EXE_FLAGS} -o "${_FCSR_DEBUG_OUT}"
            "${_FCSR_TEST_SRC}"
    DEPENDS "${_FCSR_TEST_SRC}"
    COMMENT "Cross-compiling fcsr_debug_test (RV32 ILP32F)"
    VERBATIM)
  add_custom_command(
    OUTPUT "${_FCSR_RELEASE_OUT}"
    COMMAND "${BLYT_RV32_CLANG}" ${_RV32_EXE_FLAGS} -DNDEBUG -o
            "${_FCSR_RELEASE_OUT}" "${_FCSR_TEST_SRC}"
    DEPENDS "${_FCSR_TEST_SRC}"
    COMMENT "Cross-compiling fcsr_release_test (RV32 ILP32F, -DNDEBUG)"
    VERBATIM)

  list(APPEND _native_outputs "${_SECCOMP_TEST_OUT}" "${_FCSR_DEBUG_OUT}"
       "${_FCSR_RELEASE_OUT}")

  # Native libblyt32lua.so — Lua VM + bindings for trusted native exec.
  # ld-blyt.so.1 does not export libc symbols under standard names, so the
  # needed functionality is embedded directly (curated musl subset compiled as a
  # separate partial-link object to dodge the musl `weak` macro vs Lua lstate.h
  # collision; mmap-based malloc; native stdio/luaopen stubs).
  if(EXISTS "${LUA_DIR}/lvm.c" AND BLYT_HAVE_SOFTFLOAT)
    # Curated musl subset → single partial-link object, compiled with
    # LIBBLYTC_INCLUDES (musl internal headers).  Excluded vs LIBBLYTC_SRCS:
    # fwrite/fmemopen (FILE* internals; stubbed natively), blytc_arena
    # (emulated-path arena), blytc_stubs (ECALL-based).
    set(_LIBBLYTC_NATIVE_SRCS
        ${LIBBLYTC_STRING_SRCS}
        ${LIBBLYTC_MATH_SRCS}
        ${LIBBLYTC_CTYPE_SRCS}
        "${MUSL_DIR}/src/stdlib/strtol.c"
        "${MUSL_DIR}/src/stdlib/strtod.c"
        "${MUSL_DIR}/src/stdlib/qsort.c"
        "${MUSL_DIR}/src/stdlib/qsort_nr.c"
        "${MUSL_DIR}/src/stdlib/abs.c"
        "${MUSL_DIR}/src/stdlib/atoi.c"
        "${MUSL_DIR}/src/stdlib/atol.c"
        "${MUSL_DIR}/src/stdlib/atof.c"
        "${MUSL_DIR}/src/internal/floatscan.c"
        "${MUSL_DIR}/src/internal/intscan.c"
        "${MUSL_DIR}/src/internal/shgetc.c"
        "${MUSL_DIR}/src/stdio/vsnprintf.c"
        "${MUSL_DIR}/src/stdio/snprintf.c"
        "${MUSL_DIR}/src/stdio/vfprintf.c"
        "${LUA32_DIR}/lua_native_fwritex.c"
        "${MUSL_DIR}/src/stdio/__overflow.c"
        "${MUSL_DIR}/src/stdio/__towrite.c"
        "${MUSL_DIR}/src/stdio/__toread.c"
        "${MUSL_DIR}/src/stdio/__uflow.c"
        "${MUSL_DIR}/src/stdio/__fmodeflags.c")
    set(_LIBBLYTC_NATIVE_OBJ "${CMAKE_BINARY_DIR}/libblytc_native.o")
    # Partial-link flags: same target/arch/ABI as RV32_BASE but no -shared.
    # -no-pie prevents the clang driver injecting -pie into the lld command,
    # which would conflict with -r (relocatable partial link).
    set(_RV32_PARTIAL
        --target=riscv32-linux-gnu
        -march=rv32imafdc
        -mabi=ilp32d
        -fPIC
        -nostdlib
        -no-pie
        "-fuse-ld=${BLYT_RV32_LLD}")
    # Per-object compile base for the partial link: _RV32_PARTIAL's codegen
    # subset — deliberately NO -fsemantic-interposition (the old single command
    # never had it) and no guest-include -I.
    set(_RV32_PARTIAL_COMPILE --target=riscv32-linux-gnu -march=rv32imafdc
                              -mabi=ilp32d -fPIC)
    blyt_guest_objects(
      _libblytc_native_objs
      libblytc-native-partial
      BASE
      ${_RV32_PARTIAL_COMPILE}
      SRCS
      ${_LIBBLYTC_NATIVE_SRCS}
      CFLAGS
      ${LIBBLYTC_INCLUDES}
      ${LIBBLYTC_CFLAGS}
      -O2)
    add_custom_command(
      OUTPUT "${_LIBBLYTC_NATIVE_OBJ}"
      COMMAND
        "${BLYT_RV32_CLANG}" ${_RV32_PARTIAL} -Wno-unused-command-line-argument
        "-Wl,-r" -o "${_LIBBLYTC_NATIVE_OBJ}" ${_libblytc_native_objs}
      DEPENDS ${_libblytc_native_objs}
      COMMENT "Partial-linking libblytc_native.o"
      VERBATIM)

    # libc-stub.so: zero-symbol stub with SONAME=libc.so.  musl's is_self path
    # adds the ld.so to the symbol chain for any reserved-name DT_NEEDED (libc.,
    # libm., …); symbols ld.so exports under standard names become available.
    set(_LIBC_STUB "${CMAKE_BINARY_DIR}/libc-stub.so")
    blyt_guest_so(
      "${_LIBC_STUB}"
      FALSE
      "Cross-compiling libc-stub.so"
      ARGS
      -Wl,-soname,libc.so
      -x
      c
      /dev/null
      -o
      "${_LIBC_STUB}")

    blyt_guest_so_objs(
      "${SDK_LIB_NATIVE}/libblyt32lua.so"
      TRUE
      "Linking libblyt32lua.so (native)"
      OBJNS
      libblyt32lua-native
      SRCS
      ${LUA_GUEST_SRCS}
      "${MUSL_DIR}/src/setjmp/riscv32/setjmp.S"
      "${MUSL_DIR}/src/setjmp/riscv32/longjmp.S"
      ${SF_ALL}
      ${SF_RISCV}
      ${SF_MWORD}
      "${LUA32_DIR}/softfloat_builtins.c"
      "${_LIBBLYTC_NATIVE_OBJ}"
      "${LUA32_DIR}/lua_native_malloc.c"
      "${LUA32_DIR}/lua_native_stubs.c"
      "${LUA32_DIR}/blyt32lua.c"
      CFLAGS
      ${LUA_MUSL_INCLUDES}
      ${LIBBLYTC_CFLAGS}
      -O2
      -DBLYT_LUA_I32_F64=1
      -DLUA_USE_LONGJMP=1
      ${LUA_SEED_DEF}
      -I
      "${LUA_DIR}"
      ${SF_INCLUDES}
      ${SF_DEFINES}
      LINK_ARGS
      -Wl,-soname,libblyt32lua.so
      "-Wl,--version-script,${LUA32_SYM}"
      -Wl,-z,now
      # --no-as-needed forces DT_NEEDED: libc.so even though the stub exports
      # nothing; the is_self path adds ldso to the symbol chain.
      -Wl,--no-as-needed
      "${_LIBC_STUB}"
      -Wl,--as-needed
      "${SDK_LIB_NATIVE}/libblyt32.so"
      # DT_NEEDED libblytcommon.so so the Lua VM's malloc family (lua_native_malloc.c)
      # resolves the cart-heap arena entry points (blyt_cart_heap_*) directly,
      # rather than relying on the cart transitively pulling libblytcommon via
      # libblyt32.so (#158).  --as-needed keeps it: blyt_cart_heap_* is referenced.
      "${SDK_LIB_NATIVE}/libblytcommon.so"
      LINK_DEPENDS
      "${_LIBBLYTC_NATIVE_OBJ}"
      "${LUA32_SYM}"
      "${_LIBC_STUB}"
      "${SDK_LIB_NATIVE}/libblyt32.so"
      "${SDK_LIB_NATIVE}/libblytcommon.so")
    list(APPEND _native_outputs "${SDK_LIB_NATIVE}/libblyt32lua.so")
  endif()

  add_custom_target(libblyt32_native_so ALL DEPENDS ${_native_outputs})
  message(STATUS "Native guest libraries → ${SDK_LIB_NATIVE}")
else()
  message(STATUS "Native guest libraries: skipped (BLYT_BUILD_NATIVE=OFF)")
endif()
