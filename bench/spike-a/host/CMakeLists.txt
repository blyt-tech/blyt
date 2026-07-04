# Spike A measurement runner — self-contained build.
#
# Builds the Spike A `runner` (see runner.c): a host-native program that runs a
# bare RV32IMAFDC ELF through the SAME rv32emu interpreter core the blyt runtime
# ships (interpreter-only; JIT/T2C/SYSTEM off; Berkeley SoftFloat for F/D) and
# reports effective guest MIPS from rv->csr_cycle.
#
# Deliberately standalone (NOT part of the blyt root build / CTest graph): it
# does not touch runtime/ and needs only CMake + a C compiler + the pinned
# rv32emu source. That makes it buildable both natively on the dev Mac and,
# statically linked, inside a minimal linux/arm64 container for the Pi Zero 2 W.
#
# rv32emu source: pass -DRV32EMU_SRC=<dir> pointing at an extracted
# rv32emu tree (containing src/ and src/softfloat). If omitted, the pinned
# tarball is fetched (same version/hash as the blyt root CMakeLists.txt).

cmake_minimum_required(VERSION 3.16)
project(blyt_spike_a C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Optional fully-static link (used for the Pi container build so the binary is
# self-contained on Trixie/glibc).
option(SPIKE_A_STATIC "Link the runner statically" OFF)

# Interpreter optimization level. This is THE dominant factor in the measured
# MIPS, so it is explicit rather than inherited from CMAKE_BUILD_TYPE. rv32emu
# type-puns heavily and MUST be built with -fno-strict-aliasing at -O2/-O3
# (without it, optimized builds miscompile and the guest starts at PC=0). The
# blyt runtime as built by `cmake -B build` (no CMAKE_BUILD_TYPE) currently uses
# -O0; measure several levels and report them (see run-matrix.sh / RESULTS.md).
set(SPIKE_A_EMU_OPT "-O2" CACHE STRING "Interpreter core optimization level")
message(STATUS "spike-a: interpreter opt = ${SPIKE_A_EMU_OPT} -fno-strict-aliasing")

# --------------------------------------------------------------------------
# Locate / fetch the pinned rv32emu source.
# --------------------------------------------------------------------------
set(RV32EMU_VERSION "g244cfb3-blyt-v0-p7")
if(NOT RV32EMU_SRC)
  include(FetchContent)
  FetchContent_Declare(
    rv32emu
    URL "https://github.com/blyt-tech/rv32emu/releases/download/${RV32EMU_VERSION}/rv32emu-${RV32EMU_VERSION}.tar.gz"
    URL_HASH
      SHA256=0fe03e8e8b0190a75a8561a1cfebd14f6a91a15425080d15947c8650ca2dbe9b)
  FetchContent_MakeAvailable(rv32emu)
  set(RV32EMU_SRC "${rv32emu_SOURCE_DIR}")
endif()
set(RV32EMU_DIR "${RV32EMU_SRC}/src")
if(NOT EXISTS "${RV32EMU_DIR}/riscv.c")
  message(FATAL_ERROR "RV32EMU_SRC='${RV32EMU_SRC}' has no src/riscv.c")
endif()
message(STATUS "spike-a: rv32emu source at ${RV32EMU_DIR}")

# --------------------------------------------------------------------------
# Berkeley SoftFloat (mirrors the blyt root CMakeLists.txt recipe exactly).
# --------------------------------------------------------------------------
set(SOFTFLOAT_SRC "${RV32EMU_SRC}/src/softfloat/source")
set(SOFTFLOAT_PLATFORM_DIR "${CMAKE_BINARY_DIR}/softfloat")
file(MAKE_DIRECTORY "${SOFTFLOAT_PLATFORM_DIR}")
file(WRITE "${SOFTFLOAT_PLATFORM_DIR}/platform.h" "")

file(GLOB SOFTFLOAT_SOURCES "${SOFTFLOAT_SRC}/*.c")
list(FILTER SOFTFLOAT_SOURCES EXCLUDE REGEX "/\\._")
set(SOFTFLOAT_EXCLUDE_NAMES
    bf16_isSignalingNaN.c
    bf16_to_f32.c
    f32_to_bf16.c
    s_addCarryM.c
    s_addComplCarryM.c
    s_addExtF80M.c
    s_addF128M.c
    s_addM.c
    s_compare128M.c
    s_compare96M.c
    s_compareNonnormExtF80M.c
    s_invalidExtF80M.c
    s_invalidF128M.c
    s_isNaNF128M.c
    s_mul128MTo256M.c
    s_mul64To128M.c
    s_mulAddF128M.c
    s_negXM.c
    s_normExtF80SigM.c
    s_normRoundPackMToExtF80M.c
    s_normRoundPackMToF128M.c
    s_normSubnormalBF16Sig.c
    s_normSubnormalF128SigM.c
    s_remStepMBy32.c
    s_roundMToI64.c
    s_roundMToUI64.c
    s_roundPackMToExtF80M.c
    s_roundPackMToF128M.c
    s_roundPackToBF16.c
    s_shiftLeftM.c
    s_shiftNormSigF128M.c
    s_shiftRightJamM.c
    s_shiftRightM.c
    s_shortShiftLeft64To96M.c
    s_shortShiftLeftM.c
    s_shortShiftRightExtendM.c
    s_shortShiftRightJamM.c
    s_shortShiftRightM.c
    s_sub1XM.c
    s_subM.c
    s_tryPropagateNaNExtF80M.c
    s_tryPropagateNaNF128M.c)
foreach(f ${SOFTFLOAT_EXCLUDE_NAMES})
  list(REMOVE_ITEM SOFTFLOAT_SOURCES "${SOFTFLOAT_SRC}/${f}")
endforeach()
list(
  APPEND
  SOFTFLOAT_SOURCES
  "${SOFTFLOAT_SRC}/RISCV/softfloat_raiseFlags.c"
  "${SOFTFLOAT_SRC}/RISCV/s_propagateNaNF16UI.c"
  "${SOFTFLOAT_SRC}/RISCV/s_propagateNaNF32UI.c"
  "${SOFTFLOAT_SRC}/RISCV/s_propagateNaNF64UI.c"
  "${SOFTFLOAT_SRC}/RISCV/s_propagateNaNF128UI.c")

separate_arguments(_emu_opt UNIX_COMMAND "${SPIKE_A_EMU_OPT}")
add_library(spike_a_softfloat STATIC ${SOFTFLOAT_SOURCES})
target_include_directories(
  spike_a_softfloat PUBLIC "${SOFTFLOAT_SRC}/include" "${SOFTFLOAT_SRC}/RISCV"
                           "${SOFTFLOAT_PLATFORM_DIR}")
target_compile_options(
  spike_a_softfloat
  PRIVATE ${_emu_opt}
          -DLITTLEENDIAN=1
          -DINLINE_LEVEL=5
          -DSOFTFLOAT_ROUND_ODD
          -DSOFTFLOAT_FAST_INT64
          -DSOFTFLOAT_FAST_DIV64TO32
          "-DINLINE=static inline"
          -Wno-unused-parameter
          -Wno-unused-variable
          -Wno-sign-compare
          -Wno-implicit-fallthrough
          -Wno-uninitialized)

# --------------------------------------------------------------------------
# rv32emu interpreter core (same source subset + feature defs as libblytemu).
# --------------------------------------------------------------------------
set(RV32EMU_SOURCES
    ${RV32EMU_DIR}/riscv.c
    ${RV32EMU_DIR}/emulate.c
    ${RV32EMU_DIR}/decode.c
    ${RV32EMU_DIR}/elf.c
    ${RV32EMU_DIR}/io.c
    ${RV32EMU_DIR}/map.c
    ${RV32EMU_DIR}/mpool.c
    ${RV32EMU_DIR}/cache.c
    ${RV32EMU_DIR}/syscall.c
    ${RV32EMU_DIR}/utils.c
    ${RV32EMU_DIR}/log.c)

set(RV32EMU_FEATURE_DEFS
    RV32_FEATURE_SDL=0
    RV32_FEATURE_GDBSTUB=0
    RV32_FEATURE_JIT=0
    RV32_FEATURE_T2C=0
    RV32_FEATURE_SYSTEM=0
    RV32_FEATURE_ELF_LOADER=0
    RV32_FEATURE_ARCH_TEST=0)

# Force-include common.h then, on macOS, override HAVE_MMAP back to 0 (matches
# the root build's handling of the Tahoe malloc/__OBJC_RO overlap).
set(RV32EMU_CONFIG_H "${CMAKE_BINARY_DIR}/rv32emu_config.h")
if(APPLE)
  file(WRITE "${RV32EMU_CONFIG_H}"
       "#include \"${RV32EMU_DIR}/common.h\"\n#undef HAVE_MMAP\n#define HAVE_MMAP 0\n")
else()
  file(WRITE "${RV32EMU_CONFIG_H}" "#include \"${RV32EMU_DIR}/common.h\"\n")
endif()

add_library(spike_a_emu STATIC ${RV32EMU_SOURCES})
target_compile_definitions(spike_a_emu PRIVATE ${RV32EMU_FEATURE_DEFS})
# NB: -fno-strict-aliasing is mandatory for a correct optimized rv32emu build.
separate_arguments(_emu_opt UNIX_COMMAND "${SPIKE_A_EMU_OPT}")
target_compile_options(
  spike_a_emu PRIVATE ${_emu_opt} -fno-strict-aliasing -include
                      "${RV32EMU_CONFIG_H}" -Wno-unused-parameter)
target_include_directories(spike_a_emu PUBLIC ${RV32EMU_DIR})
target_link_libraries(spike_a_emu PRIVATE spike_a_softfloat)

# --------------------------------------------------------------------------
# The runner.
# --------------------------------------------------------------------------
add_executable(runner runner.c)
# The runner must see the SAME feature defs so riscv_t/vm_attr_t layouts match.
target_compile_definitions(runner PRIVATE ${RV32EMU_FEATURE_DEFS})
target_link_libraries(runner PRIVATE spike_a_emu spike_a_softfloat)
find_package(Threads)
if(Threads_FOUND)
  target_link_libraries(runner PRIVATE Threads::Threads)
endif()
if(SPIKE_A_STATIC)
  target_link_options(runner PRIVATE -static)
endif()
