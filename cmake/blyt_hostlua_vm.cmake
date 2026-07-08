# cmake/blyt_hostlua_vm.cmake — the shipped native host-Lua VM build recipe.
#
# Single source of truth for the seam-compiled native fork Lua VM (issue #238,
# epic #230, ADR-0136/0135). Extracted from the Spike Z determinism leg
# (frontends/native-hostlua/CMakeLists.txt, #225) so the shipped host-Lua path in
# libblyt and the determinism gate build the *identical* VM — a divergence in the
# pinned hash seed or the contraction flag is exactly the class of bug that would
# silently break cross-platform determinism.
#
# Provides blyt_add_hostlua_vm(<name> <contract_flag> [extra_flags]): produces a
# static library <name> — the native Lua fork VM (onelua.c) seam-wired to the
# in-house blyt-tech musl transcendental + number-format kernels via blyt_fpm
# (BLYT_HOSTLUA_FP_SEAM, ADR-0135) — plus its private <name>_fpm seam lib. The VM
# is hermetic: it reaches ONLY the in-house musl kernels, never the host libm, so
# it reproduces the Berkeley-SoftFloat reference bit-for-bit on FMA silicon
# (Spike Z: proven on real arm64 + x86-64 + wasm).
#
# Sets BLYT_HOSTLUA_VM_AVAILABLE (parent scope) TRUE when the recipe's inputs are
# present and the host arch is supported (x86-64 / arm64). Callers must guard on
# it — on any other host the deterministic VM cannot be built and the host-Lua
# execution path is unavailable (callers fall back to emulated RV32 Lua).

# Run the one-time setup (arch detection, musl bits generation, source lists)
# exactly once even if this file is include()d from more than one scope.
if(NOT DEFINED _BLYT_HOSTLUA_VM_SETUP_DONE)
  set(_BLYT_HOSTLUA_VM_SETUP_DONE TRUE)
  set(BLYT_HOSTLUA_VM_AVAILABLE FALSE)

  if(NOT lua_SOURCE_DIR OR NOT EXISTS "${lua_SOURCE_DIR}/onelua.c")
    message(STATUS "blyt host-Lua VM: Lua fork source unavailable — host-Lua path disabled")
  elseif(NOT musl_SOURCE_DIR OR NOT EXISTS "${musl_SOURCE_DIR}/src/math/sin.c")
    message(STATUS "blyt host-Lua VM: musl source unavailable — host-Lua path disabled")
  else()
    # Native musl arch include dir (LP64 host). The routed Zone-2 kernels
    # (sin/cos/tan/asin/acos/atan/atan2/exp/log/log2/log10/pow) and __rem_pio2 are
    # pure `double` — they do not touch the 80-bit vs 128-bit long-double
    # difference between x86-64 and the riscv reference — so the native arch's
    # bits/ config is safe here; the parity gate verifies the reference-matching
    # result.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
      set(_BHLV_MUSL_ARCH x86_64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
      set(_BHLV_MUSL_ARCH aarch64)
    else()
      set(_BHLV_MUSL_ARCH "")
      message(
        STATUS
        "blyt host-Lua VM: unsupported host arch ${CMAKE_SYSTEM_PROCESSOR} — host-Lua path disabled")
    endif()

    if(_BHLV_MUSL_ARCH)
      set(_BHLV_DIR "${CMAKE_BINARY_DIR}/blyt-hostlua-vm")
      file(MAKE_DIRECTORY "${_BHLV_DIR}/bits")

      # musl's <bits/alltypes.h> is a generated file (sed template), per arch.
      # Generate the native-arch (LP64) copy so type sizes match the host ABI.
      # Mirrors the guest recipe in cmake/blyt_guest_libs.cmake.
      execute_process(
        COMMAND
          sh -c
          "cat '${musl_SOURCE_DIR}/arch/${_BHLV_MUSL_ARCH}/bits/alltypes.h.in' '${musl_SOURCE_DIR}/include/alltypes.h.in' | sed -f '${musl_SOURCE_DIR}/tools/mkalltypes.sed'"
        OUTPUT_FILE "${_BHLV_DIR}/bits/alltypes.h"
        RESULT_VARIABLE _BHLV_ALLTYPES_RC)
      if(NOT _BHLV_ALLTYPES_RC EQUAL 0)
        message(WARNING "blyt host-Lua VM: failed to generate bits/alltypes.h — host-Lua path disabled")
      else()
        # The Phase-B stdio subset pulls musl's <stdio_impl.h> → <sys/syscall.h> →
        # <bits/syscall.h> and src/internal/syscall.h → "syscall_arch.h" (per-arch
        # syscall machinery). The seam issues no syscalls, so both are shadowed by
        # the empty shims in runtime/shared/blyt_fpm_musl_shim (on the include path
        # below) — keeping the vendored stdio host-arch-agnostic.
        set(_BHLV_MUSL_SHIM "${CMAKE_SOURCE_DIR}/runtime/shared/blyt_fpm_musl_shim")

        # musl's <bits/float.h> hardcodes the arch's `long double` width — e.g.
        # arch/aarch64 declares LDBL_MANT_DIG 113 (Linux AArch64 quad). But the
        # host compiler's real `long double` varies: 80-bit on x86-64, 128-bit
        # quad on Linux AArch64 / wasm, but *64-bit binary64* on Apple AArch64
        # (macOS). musl's floatscan / fmt_fp (and frexpl/scalbnl) branch on
        # LDBL_MANT_DIG and do union bit-surgery sized to it, so a header that
        # disagrees with the compiler's actual `long double` corrupts memory
        # (frexpl recurses into a stack overflow on macOS). Override bits/float.h
        # with the compiler's real values (__LDBL_*__ predefined macros) so musl
        # always sees the true host `long double`. Correctly-rounded double
        # conversion is the invariant the parity gate verifies bit-for-bit against
        # the softfloat reference across arches.
        file(
          WRITE "${_BHLV_DIR}/bits/float.h"
          "/* Generated by cmake/blyt_hostlua_vm.cmake (blyt#238/#225 Phase B):\n"
          "   musl <bits/float.h> pinned to the host compiler's real long double so the\n"
          "   vendored strtod/vfprintf float path sees the true LDBL width (Apple AArch64\n"
          "   is binary64, not the 113-bit quad musl's arch header assumes). */\n"
          "#ifndef _BLYT_FPM_BITS_FLOAT_H\n"
          "#define _BLYT_FPM_BITS_FLOAT_H\n"
          "#define FLT_EVAL_METHOD __FLT_EVAL_METHOD__\n"
          "#define DECIMAL_DIG __DECIMAL_DIG__\n"
          "#define LDBL_TRUE_MIN __LDBL_DENORM_MIN__\n"
          "#define LDBL_MIN __LDBL_MIN__\n"
          "#define LDBL_MAX __LDBL_MAX__\n"
          "#define LDBL_EPSILON __LDBL_EPSILON__\n"
          "#define LDBL_MANT_DIG __LDBL_MANT_DIG__\n"
          "#define LDBL_MIN_EXP __LDBL_MIN_EXP__\n"
          "#define LDBL_MAX_EXP __LDBL_MAX_EXP__\n"
          "#define LDBL_DIG __LDBL_DIG__\n"
          "#define LDBL_MIN_10_EXP __LDBL_MIN_10_EXP__\n"
          "#define LDBL_MAX_10_EXP __LDBL_MAX_10_EXP__\n"
          "#endif\n")

        # The in-house blyt-tech musl generic-C kernels (NOT any host per-arch asm
        # — invariant 3, ADR-0135): only the top-level src/math/*.c, never
        # src/math/<arch>/. Drop the handful of legacy/alias kernels (exp10,
        # lgamma, remainder, tgamma, signgam) that use musl's weak_alias —
        # __attribute__((alias)) is unsupported on Darwin, and none is reachable
        # from the Lua math surface (sin/cos/tan/asin/acos/atan/atan2/exp/log*/pow
        # + Zone-1 sqrt/floor/ceil/fmod/frexp/ldexp), so dropping them is
        # behaviour-neutral and keeps one source list across Darwin and Linux.
        file(GLOB _BHLV_MATH_SRCS_ALL "${musl_SOURCE_DIR}/src/math/*.c")
        set(_BHLV_MUSL_MATH_SRCS "")
        foreach(_src ${_BHLV_MATH_SRCS_ALL})
          file(STRINGS "${_src}" _uses_alias REGEX "weak_alias" LIMIT_COUNT 1)
          if(NOT _uses_alias)
            list(APPEND _BHLV_MUSL_MATH_SRCS "${_src}")
          endif()
        endforeach()

        # Phase B (ADR-0135, #225): the renamed blyt-tech musl string↔number
        # conversion subset — strtod + floatscan + the vfprintf float-format path
        # — compiled under a blyt_fpm_ namespace (blyt_fpm_musl_renames.h,
        # force-included) so it does not override the module libc. blyt_fpm_conv.c
        # re-provides the byte-neutral plumbing leaves
        # (__towrite/__fwritex/__uflow/wctomb/__errno_location). Pins
        # tostring/tonumber/string.format to the same conversion implementation the
        # emulated softfloat reference uses.
        set(_BHLV_MUSL_CONV_SRCS
            "${musl_SOURCE_DIR}/src/stdlib/strtod.c"
            "${musl_SOURCE_DIR}/src/internal/floatscan.c"
            "${musl_SOURCE_DIR}/src/internal/shgetc.c"
            "${musl_SOURCE_DIR}/src/stdio/vfprintf.c"
            "${musl_SOURCE_DIR}/src/stdio/vsnprintf.c"
            "${musl_SOURCE_DIR}/src/stdio/snprintf.c"
            "${CMAKE_SOURCE_DIR}/runtime/shared/blyt_fpm_conv.c")
        set(_BHLV_RENAMES_HDR "${CMAKE_SOURCE_DIR}/runtime/shared/blyt_fpm_musl_renames.h")

        # On x86-64 hardware FMA (FMA3) is NOT in the baseline `-march=x86-64` ISA
        # — it arrived with Haswell — so the compiler cannot emit `vfmadd` and
        # -ffp-contract=fast has nothing to fuse unless FMA codegen is explicitly
        # enabled. On AArch64, FMA (fmadd) IS baseline. The negative-control leg
        # adds this so its FMA divergence is demonstrable on both arches.
        set(_BHLV_FMA_ENABLE "")
        if(_BHLV_MUSL_ARCH STREQUAL "x86_64")
          set(_BHLV_FMA_ENABLE -mfma)
        endif()

        set(BLYT_HOSTLUA_VM_AVAILABLE TRUE)
        message(STATUS "blyt host-Lua VM: available (${_BHLV_MUSL_ARCH}) — native host-Lua path enabled")
      endif()
    endif()
  endif()
endif()

# blyt_add_hostlua_vm(<name> <contract_flag> [extra_flags])
#   <contract_flag> — e.g. -ffp-contract=off (the determinism setting) or
#                     -ffp-contract=fast (the Spike Z negative control).
#   [extra_flags]   — extra compile options for all TUs (e.g. -mfma to enable FMA
#                     codegen on x86-64 for the negative control).
# Produces static lib <name> (the seam-wired VM) + <name>_fpm (its seam kernels).
# The exported ${_BHLV_FMA_ENABLE} is available to callers that build the FMA
# negative control.
function(blyt_add_hostlua_vm NAME CONTRACT_FLAG)
  if(NOT BLYT_HOSTLUA_VM_AVAILABLE)
    message(FATAL_ERROR "blyt_add_hostlua_vm(${NAME}): guard on BLYT_HOSTLUA_VM_AVAILABLE first")
  endif()
  set(_EXTRA ${ARGN})
  set(_fpm ${NAME}_fpm)

  # ---- Zone-2 seam kernels: blyt_fpm_soft.c + in-house musl src/math ----------
  # A scoped static lib so musl's internal headers apply ONLY to these TUs. Its
  # strong sin/cos/… definitions are the ones the VM links (hermetic: no host
  # libm).
  add_library(${_fpm} STATIC "${CMAKE_SOURCE_DIR}/runtime/shared/blyt_fpm_soft.c"
                             ${_BHLV_MUSL_MATH_SRCS} ${_BHLV_MUSL_CONV_SRCS})
  # The vendored conversion TUs (and the plumbing support file) compile against
  # blyt_fpm_musl_renames.h so their public + musl-internal symbols land in the
  # blyt_fpm_ namespace. The math kernels keep their real names (the seam
  # overrides host libm) so the rename header is NOT applied to them.
  set_source_files_properties(
    ${_BHLV_MUSL_CONV_SRCS} PROPERTIES COMPILE_OPTIONS "-include;${_BHLV_RENAMES_HDR}")
  target_include_directories(
    ${_fpm}
    PRIVATE "${_BHLV_MUSL_SHIM}" # empty syscall_arch.h + bits/syscall.h shims
            "${CMAKE_SOURCE_DIR}/runtime/shared" # blyt_fpm.h
            "${_BHLV_DIR}" # generated <bits/alltypes.h>, <bits/float.h>
            "${musl_SOURCE_DIR}/src/include" "${musl_SOURCE_DIR}/include"
            "${musl_SOURCE_DIR}/arch/${_BHLV_MUSL_ARCH}"
            "${musl_SOURCE_DIR}/arch/generic" "${musl_SOURCE_DIR}/src/internal")
  # Determinism (ADR-0007): the given contraction setting, no fast-math, no
  # const-fold to the host libm; -w silences musl's intentional idioms. -O2
  # matches a shipped host-Lua build and the reference guest libs.
  target_compile_options(${_fpm} PRIVATE -O2 ${CONTRACT_FLAG} ${_EXTRA}
                                         -fno-fast-math -fno-builtin -w)
  set_target_properties(${_fpm} PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # ---- The seam-compiled native Lua fork VM ----------------------------------
  # Same fork + onelua.c the emulated/WASM legs use, but native. MAKE_LIB
  # suppresses main(); BLYT_LUA_I32_F64 matches the ilp32d cart bytecode;
  # BLYT_HOSTLUA_FP_SEAM engages the lmathlib/llimits routing through blyt_fpm.
  add_library(${NAME} STATIC "${lua_SOURCE_DIR}/onelua.c")
  target_compile_definitions(
    ${NAME} PRIVATE MAKE_LIB=1
    PUBLIC BLYT_LUA_I32_F64=1)
  target_compile_definitions(${NAME} PRIVATE BLYT_HOSTLUA_FP_SEAM=1)
  # Fixed Lua hash seed (ADR-0130/ADR-0066), byte-identical to the WASM and guest
  # builds — WITHOUT this the default time()-based seed randomizes string hashing,
  # so table iteration order (pairs()) diverges from the reference. 0x424C5954 ==
  # "BLYT". The shipped native player MUST carry this (Spike Z Q5).
  target_compile_options(${NAME} PRIVATE "-Dluai_makeseed()=0x424C5954u")
  target_include_directories(${NAME} PUBLIC "${lua_SOURCE_DIR}"
                             PRIVATE "${CMAKE_SOURCE_DIR}/runtime/shared")
  target_compile_options(${NAME} PRIVATE -O2 ${CONTRACT_FLAG} ${_EXTRA} -fno-fast-math)
  # Hermetic: the VM reaches ONLY the in-house musl kernels via the seam lib —
  # never the host libm. (Zone-1 sqrt/floor/fmod/… also resolve to the seam lib's
  # musl generic-C definitions.)
  target_link_libraries(${NAME} PUBLIC ${_fpm})
  set_target_properties(${NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()
