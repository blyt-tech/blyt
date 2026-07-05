# frontends/native-hostlua/CMakeLists.txt — Spike Z native host-Lua determinism
# leg (issue #225, ADR-0135).
#
# Builds a minimal native host-Lua execution leg that runs a cart's Lua the same
# way the WASM host-Lua fast path does (frontends/wasm/), but compiled for the
# native host (x86-64 / arm64) instead of wasm32. It is the FMA-silicon +
# cross-arch determinism probe the WASM-only Phase A could not run: does the
# native host-Lua VM reproduce the Berkeley-SoftFloat reference bit-for-bit?
#
# NOT a shipped player — a determinism gate. The real native host-Lua player is
# separate implementation work, gated on this spike going green.

if(NOT lua_SOURCE_DIR OR NOT EXISTS "${lua_SOURCE_DIR}/onelua.c")
  message(STATUS "blyt native host-Lua leg: Lua fork source unavailable — skipping")
  return()
endif()
if(NOT musl_SOURCE_DIR OR NOT EXISTS "${musl_SOURCE_DIR}/src/math/sin.c")
  message(STATUS "blyt native host-Lua leg: musl source unavailable — skipping")
  return()
endif()

# Native musl arch include dir (LP64 host). The routed Zone-2 kernels
# (sin/cos/tan/asin/acos/atan/atan2/exp/log/log2/log10/pow) and __rem_pio2 are
# pure `double` — they do not touch the 80-bit vs 128-bit long-double difference
# between x86-64 and the riscv reference — so the native arch's bits/ config is
# safe here; the reference-matching result is what the parity gate verifies.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  set(_NHL_MUSL_ARCH x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  set(_NHL_MUSL_ARCH aarch64)
else()
  message(
    STATUS
    "blyt native host-Lua leg: unsupported host arch ${CMAKE_SYSTEM_PROCESSOR} — skipping")
  return()
endif()

# musl's <bits/alltypes.h> is a generated file (sed template), per arch. The
# guest build generates it from arch/riscv32; here we generate the native-arch
# (LP64) copy so the type sizes match the host ABI. Mirrors the guest recipe in
# cmake/blyt_guest_libs.cmake.
set(_NHL_BITS_DIR "${CMAKE_CURRENT_BINARY_DIR}/musl-bits")
file(MAKE_DIRECTORY "${_NHL_BITS_DIR}/bits")
execute_process(
  COMMAND
    sh -c
    "cat '${musl_SOURCE_DIR}/arch/${_NHL_MUSL_ARCH}/bits/alltypes.h.in' '${musl_SOURCE_DIR}/include/alltypes.h.in' | sed -f '${musl_SOURCE_DIR}/tools/mkalltypes.sed'"
  OUTPUT_FILE "${_NHL_BITS_DIR}/bits/alltypes.h"
  RESULT_VARIABLE _NHL_ALLTYPES_RC)
if(NOT _NHL_ALLTYPES_RC EQUAL 0)
  message(
    WARNING "blyt native host-Lua leg: failed to generate bits/alltypes.h — skipping")
  return()
endif()

# The in-house blyt-tech musl generic-C kernels (NOT any host per-arch asm —
# invariant 3, ADR-0135): only the top-level src/math/*.c, never src/math/<arch>/.
file(GLOB _NHL_MUSL_MATH_SRCS_ALL "${musl_SOURCE_DIR}/src/math/*.c")
# Drop the handful of legacy/alias kernels (exp10, lgamma, remainder, tgamma,
# signgam) that use musl's weak_alias — __attribute__((alias)) is unsupported on
# Darwin, and none is reachable from the Lua math surface (sin/cos/tan/asin/acos/
# atan/atan2/exp/log*/pow + Zone-1 sqrt/floor/ceil/fmod/frexp/ldexp), so dropping
# them is behaviour-neutral and keeps one source list across Darwin and Linux.
set(_NHL_MUSL_MATH_SRCS "")
foreach(_src ${_NHL_MUSL_MATH_SRCS_ALL})
  file(STRINGS "${_src}" _uses_alias REGEX "weak_alias" LIMIT_COUNT 1)
  if(NOT _uses_alias)
    list(APPEND _NHL_MUSL_MATH_SRCS "${_src}")
  endif()
endforeach()

# On x86-64 hardware FMA (FMA3) is NOT in the baseline `-march=x86-64` ISA — it
# arrived with Haswell — so the compiler cannot emit `vfmadd` and
# -ffp-contract=fast has nothing to fuse unless FMA codegen is explicitly
# enabled. On AArch64, FMA (fmadd) IS baseline, so it is always available. The
# negative control adds this so its FMA divergence is demonstrable on both arches
# (a real determinism finding: a default-baseline x86-64 host-Lua build cannot
# contract regardless of the flag; the risk only appears under -march=native/-mfma).
set(_NHL_FMA_ENABLE "")
if(_NHL_MUSL_ARCH STREQUAL "x86_64")
  set(_NHL_FMA_ENABLE -mfma)
endif()

# Q2 finding (#225): only Mode A (native f64) is built — Mode B (-msoft-float
# lowering of the Zone-2 kernels) is NOT viable on either target, so there is no
# soft-float variant to instantiate:
#   • x86-64: -msoft-float alone still emits SSE (no real soft-float); adding
#     -mno-sse fails to compile musl's `double exp(double)` kernels — "SSE
#     register return with SSE disabled" (the SysV ABI returns doubles in xmm),
#     and the kernels pass `double` between internal helpers, so the uint64_t
#     seam boundary cannot rescue it.
#   • arm64: clang ignores -msoft-float ("argument unused"); -mgeneral-regs-only
#     (the AArch64 equivalent) rejects the double ABI ("darwinpcs does not
#     support it").
# Mode B is also unnecessary: Mode A reproduces the SoftFloat reference
# bit-for-bit on both arches (fp_native_hostlua_core_parity), so the ADR-0135
# "future native -msoft-float lowering" is neither needed nor realizable here.
#
# blyt_add_native_hostlua(<name> <contract_flag> [extra_flags])
#   <contract_flag> — e.g. -ffp-contract=off (the determinism setting) or
#                     -ffp-contract=fast (the Q1 negative control that must
#                     diverge on FMA hardware).
#   [extra_flags]   — extra compile options for all three TUs (e.g. -mfma to
#                     enable FMA codegen on x86-64 for the negative control).
# Produces the runner target <name> (plus <name>_fpm / <name>_vm static libs).
function(blyt_add_native_hostlua NAME CONTRACT_FLAG)
  set(_EXTRA ${ARGN})
  set(_fpm ${NAME}_fpm)
  set(_vm ${NAME}_vm)

  # ---- Zone-2 seam kernels: blyt_fpm_soft.c + in-house musl src/math --------
  # Mirrors frontends/wasm/CMakeLists.txt's blyt_hostlua_fpm: a scoped static lib
  # so musl's internal headers apply ONLY to these TUs. Its strong sin/cos/…
  # definitions are the ones the VM links (hermetic: no host libm).
  add_library(${_fpm} STATIC "${CMAKE_SOURCE_DIR}/runtime/shared/blyt_fpm_soft.c"
                             ${_NHL_MUSL_MATH_SRCS})
  target_include_directories(
    ${_fpm}
    PRIVATE "${CMAKE_SOURCE_DIR}/runtime/shared" # blyt_fpm.h
            "${_NHL_BITS_DIR}" # generated <bits/alltypes.h> (native LP64)
            "${musl_SOURCE_DIR}/src/include" "${musl_SOURCE_DIR}/include"
            "${musl_SOURCE_DIR}/arch/${_NHL_MUSL_ARCH}"
            "${musl_SOURCE_DIR}/arch/generic" "${musl_SOURCE_DIR}/src/internal")
  # Determinism (ADR-0007): the given contraction setting, no fast-math, no
  # const-fold to the host libm; -w silences musl's intentional idioms. -O2
  # matches a shipped/optimized host-Lua build and the reference guest libs — and
  # is where FMA contraction actually happens (at -O0 the compiler never fuses,
  # which would mask the very divergence this leg is built to detect).
  target_compile_options(${_fpm} PRIVATE -O2 ${CONTRACT_FLAG} ${_EXTRA}
                                         -fno-fast-math -fno-builtin -w)
  set_target_properties(${_fpm} PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # ---- The seam-compiled native Lua fork VM --------------------------------
  # Same fork + onelua.c the emulated/WASM legs use, but native. MAKE_LIB
  # suppresses main(); BLYT_LUA_I32_F64 matches the ilp32d cart bytecode;
  # BLYT_HOSTLUA_FP_SEAM engages the lmathlib/llimits routing through blyt_fpm.
  add_library(${_vm} STATIC "${lua_SOURCE_DIR}/onelua.c")
  target_compile_definitions(
    ${_vm} PRIVATE MAKE_LIB=1 BLYT_LUA_I32_F64=1 BLYT_HOSTLUA_FP_SEAM=1)
  # Fixed Lua hash seed (ADR-0130/ADR-0066), byte-identical to the WASM and guest
  # builds — WITHOUT this the default time()-based seed randomizes string hashing,
  # so table iteration order (pairs()) diverges from the reference. Q5 (#225)
  # surfaced this as a native-only divergence; the shipped native player must
  # carry the same define. 0x424C5954 == "BLYT".
  target_compile_options(${_vm} PRIVATE "-Dluai_makeseed()=0x424C5954u")
  target_include_directories(${_vm} PUBLIC "${lua_SOURCE_DIR}"
                             PRIVATE "${CMAKE_SOURCE_DIR}/runtime/shared")
  target_compile_options(${_vm} PRIVATE -O2 ${CONTRACT_FLAG} ${_EXTRA} -fno-fast-math)
  # Hermetic: the VM reaches ONLY the in-house musl kernels via the seam lib —
  # never the host libm. (Zone-1 sqrt/floor/fmod/… also resolve to the seam
  # lib's musl generic-C definitions.)
  target_link_libraries(${_vm} PUBLIC ${_fpm})
  set_target_properties(${_vm} PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # ---- The runner ----------------------------------------------------------
  add_executable(${NAME} "${CMAKE_CURRENT_SOURCE_DIR}/hostlua_runner.c"
                         "${CMAKE_SOURCE_DIR}/runtime/shared/blyt_elf_section.c")
  target_include_directories(${NAME} PRIVATE "${CMAKE_SOURCE_DIR}/runtime/shared")
  # The runner carries the variant's contraction setting so its C-level Q1
  # torture (torture_digest) reflects -ffp-contract=off vs =fast. -O2 so the
  # compiler actually fuses multiply-adds under =fast (never at -O0).
  target_compile_options(${NAME} PRIVATE -O2 ${CONTRACT_FLAG} ${_EXTRA} -fno-fast-math)
  target_link_libraries(${NAME} PRIVATE ${_vm})
endfunction()

# The determinism leg: -ffp-contract=off, native f64 (Q2 Mode A — the shippable
# realization). This is the artifact the fp_parity cross-arch gate drives.
blyt_add_native_hostlua(blyt_hostlua_native -ffp-contract=off)

# Q1 negative control (#225): the SAME build with FMA contraction ENABLED
# (-ffp-contract=fast) and FMA codegen available (baseline on arm64, -mfma on
# x86-64). On FMA silicon a fused a*b+c rounds once instead of twice, so the
# runner's C torture MUST diverge from the -ffp-contract=off build — proving
# -ffp-contract=off is load-bearing and the gate has teeth (the AC2 intent WASM's
# missing scalar FMA cannot exercise).
blyt_add_native_hostlua(blyt_hostlua_native_fma -ffp-contract=fast
                        ${_NHL_FMA_ENABLE})
