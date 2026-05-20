# cmake/blyt_sdk.cmake
#
# Assembles a self-contained blyt SDK under build/sdk/:
#
# build/sdk/ bin/        blyt (devtool), blytrun (SDL2 runtime) include/ blyt.h,
# blyt32.h, blyt_runtime.h lib/        libblytcommon.so, libblyt32.so
# (RV32IMAFC) toolchain/  clang, ld.lld, llvm-objcopy, … (downloaded LLVM)
#
# Invoked via `cmake --build build --target sdk`. Required variables (injected
# by the sdk target in CMakeLists.txt): BLYT_SOURCE_DIR, BLYT_BINARY_DIR,
# CMAKE_HOST_SYSTEM_NAME, CMAKE_HOST_SYSTEM_PROCESSOR

set(SDK_DIR "${BLYT_BINARY_DIR}/sdk")
set(SDK_BIN "${SDK_DIR}/bin")
set(SDK_LIB "${SDK_DIR}/lib")
set(SDK_INC "${SDK_DIR}/include")
set(SDK_TOOLCHAIN "${SDK_DIR}/toolchain")
set(DOWNLOADS "${BLYT_BINARY_DIR}/downloads")

# -------------------------------------------------------------------------
# Step 1: Locate or download a riscv32-capable LLVM toolchain
# -------------------------------------------------------------------------

# Check known locations for an existing riscv32-capable clang + lld pair.
set(CANDIDATE_PAIRS
    "/opt/homebrew/opt/llvm/bin/clang"
    "/opt/homebrew/opt/llvm/bin/ld.lld"
    "/opt/homebrew/opt/llvm/bin/clang"
    "/opt/homebrew/opt/llvm/bin/lld"
    "/usr/bin/clang"
    "/usr/bin/ld.lld"
    "/usr/local/bin/clang"
    "/usr/local/bin/ld.lld")

set(FOUND_CLANG "")
set(FOUND_LLD "")
set(FOUND_OBJCOPY "")

list(LENGTH CANDIDATE_PAIRS PAIR_LEN)
math(EXPR PAIR_COUNT "${PAIR_LEN} / 2")
math(EXPR PAIR_LAST "${PAIR_COUNT} - 1")

foreach(IDX RANGE ${PAIR_LAST})
  math(EXPR CI "${IDX} * 2")
  math(EXPR LI "${IDX} * 2 + 1")
  list(GET CANDIDATE_PAIRS ${CI} CAND_CLANG)
  list(GET CANDIDATE_PAIRS ${LI} CAND_LLD)
  if(NOT EXISTS "${CAND_CLANG}" OR NOT EXISTS "${CAND_LLD}")
    continue()
  endif()
  execute_process(
    COMMAND "${CAND_CLANG}" --target=riscv32 -march=rv32imafc -mabi=ilp32f -x c
            -E - -o /dev/null
    INPUT_FILE /dev/null
    RESULT_VARIABLE RV32_OK
    ERROR_QUIET OUTPUT_QUIET)
  if(RV32_OK EQUAL 0)
    set(FOUND_CLANG "${CAND_CLANG}")
    set(FOUND_LLD "${CAND_LLD}")
    get_filename_component(TOOL_DIR "${CAND_CLANG}" DIRECTORY)
    if(EXISTS "${TOOL_DIR}/llvm-objcopy")
      set(FOUND_OBJCOPY "${TOOL_DIR}/llvm-objcopy")
    endif()
    break()
  endif()
endforeach()

if(NOT FOUND_CLANG)
  # Download a pre-built LLVM release.
  set(LLVM_VER "18.1.8")
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
      set(LLVM_TRIPLE "arm64-apple-macos11")
    else()
      set(LLVM_TRIPLE "x86_64-apple-darwin")
    endif()
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "aarch64")
      set(LLVM_TRIPLE "aarch64-linux-gnu")
    else()
      set(LLVM_TRIPLE "x86_64-linux-gnu-ubuntu-22.04")
    endif()
  else()
    message(
      FATAL_ERROR "Unsupported host platform '${CMAKE_HOST_SYSTEM_NAME}'. "
                  "Set BLYT_CLANG to a clang that can target riscv32.")
  endif()

  set(LLVM_ARCHIVE "clang+llvm-${LLVM_VER}-${LLVM_TRIPLE}.tar.xz")
  set(LLVM_URL
      "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VER}/${LLVM_ARCHIVE}"
  )
  set(LLVM_ARCHIVE_PATH "${DOWNLOADS}/${LLVM_ARCHIVE}")

  if(NOT EXISTS "${SDK_TOOLCHAIN}/bin/clang")
    if(NOT EXISTS "${LLVM_ARCHIVE_PATH}")
      message(STATUS "Downloading LLVM ${LLVM_VER} (${LLVM_TRIPLE})…")
      file(MAKE_DIRECTORY "${DOWNLOADS}")
      file(
        DOWNLOAD "${LLVM_URL}" "${LLVM_ARCHIVE_PATH}"
        SHOW_PROGRESS
        STATUS DL_STATUS)
      list(GET DL_STATUS 0 DL_CODE)
      if(NOT DL_CODE EQUAL 0)
        file(REMOVE "${LLVM_ARCHIVE_PATH}")
        message(FATAL_ERROR "LLVM download failed: ${DL_STATUS}")
      endif()
    endif()

    message(STATUS "Extracting LLVM toolchain…")
    file(MAKE_DIRECTORY "${SDK_DIR}")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xf "${LLVM_ARCHIVE_PATH}"
      WORKING_DIRECTORY "${SDK_DIR}"
      RESULT_VARIABLE EXTRACT_RESULT)
    if(NOT EXTRACT_RESULT EQUAL 0)
      message(FATAL_ERROR "Extraction failed")
    endif()

    file(GLOB EXTRACTED_DIR "${SDK_DIR}/clang+llvm-*")
    if(NOT EXTRACTED_DIR)
      message(
        FATAL_ERROR "Could not find extracted LLVM directory in ${SDK_DIR}")
    endif()
    file(RENAME "${EXTRACTED_DIR}" "${SDK_TOOLCHAIN}")
  endif()

  set(FOUND_CLANG "${SDK_TOOLCHAIN}/bin/clang")
  set(FOUND_LLD "${SDK_TOOLCHAIN}/bin/ld.lld")
  set(FOUND_OBJCOPY "${SDK_TOOLCHAIN}/bin/llvm-objcopy")
endif()

message(STATUS "blyt SDK: toolchain clang = ${FOUND_CLANG}")

# -------------------------------------------------------------------------
# Step 2: Build RV32IMAFC runtime libraries
# -------------------------------------------------------------------------

file(MAKE_DIRECTORY "${SDK_LIB}")

set(RV32_BASE
    --target=riscv32
    -march=rv32imafc
    -mabi=ilp32f
    -shared
    -fPIC
    -nostdlib
    # -Wl,--shared explicitly tells lld to produce ET_DYN; clang may inject
    # -Bstatic for bare-metal riscv targets which overrides -shared otherwise.
    -Wl,--shared
    -I
    "${BLYT_SOURCE_DIR}/runtime/guest/include")
if(FOUND_LLD)
  list(APPEND RV32_BASE "-fuse-ld=${FOUND_LLD}")
endif()

message(STATUS "Building libblytcommon.so…")
# --allow-undefined: blyt_cart_init/update/draw are provided by the cart at
# runtime (reverse symbol lookup); lld requires explicit permission for this.
execute_process(
  COMMAND
    "${FOUND_CLANG}" ${RV32_BASE} -Wl,-soname,libblytcommon.so -o
    "${SDK_LIB}/libblytcommon.so"
    "${BLYT_SOURCE_DIR}/runtime/guest/src/libblytcommon/blyt_common.c"
  RESULT_VARIABLE R)
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to build libblytcommon.so")
endif()

message(STATUS "Building libblytc.so…")
# libblytc.so — trimmed musl-based C library (ADR-0120). Curated musl source
# subsets + our arena allocator and internal stubs.
set(MUSL_DIR "${BLYT_SOURCE_DIR}/third_party/musl")
set(LIBBLYTC_BITS_DIR "${BLYT_BINARY_DIR}/libblytc/bits")
set(LIBBLYTC_ALLTYPES_H "${LIBBLYTC_BITS_DIR}/alltypes.h")

if(NOT EXISTS "${MUSL_DIR}/include/stdio.h")
  message(FATAL_ERROR "third_party/musl not initialised. "
                      "Run: git submodule update --init third_party/musl")
endif()

# Set up bits/ directory: copy all arch/riscv32/bits/*.h files, then generate
# the two that require processing (alltypes.h from template; syscall.h from
# .in).
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
  RESULT_VARIABLE R)
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to generate bits/alltypes.h for libblytc.so")
endif()

# Generate bits/syscall.h: copy .in then append SYS_xxx aliases for each
# __NR_xxx (matching musl's Makefile: sed -n -e s/__NR_/SYS_/p < .in >> out).
execute_process(
  COMMAND
    sh -c "cp '${MUSL_DIR}/arch/riscv32/bits/syscall.h.in' \
        '${LIBBLYTC_BITS_DIR}/syscall.h' \
     && sed -n -e 's/__NR_/SYS_/p' \
         '${MUSL_DIR}/arch/riscv32/bits/syscall.h.in' \
         >> '${LIBBLYTC_BITS_DIR}/syscall.h'"
  RESULT_VARIABLE R)
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to generate bits/syscall.h for libblytc.so")
endif()

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
    "${BLYT_SOURCE_DIR}/runtime/guest/src/libblytc/blytc_arena.c"
    "${BLYT_SOURCE_DIR}/runtime/guest/src/libblytc/blytc_stubs.c")

set(LIBBLYTC_INCLUDES
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

set(LIBBLYTC_CFLAGS
    -ffp-contract=off
    -fno-fast-math
    -fno-strict-aliasing
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-implicit-fallthrough
    -Wno-unused-variable
    -Wno-deprecated-non-prototype)

execute_process(
  COMMAND "${FOUND_CLANG}" ${RV32_BASE} ${LIBBLYTC_INCLUDES} ${LIBBLYTC_CFLAGS}
          -Wl,-soname,libblytc.so -o "${SDK_LIB}/libblytc.so" ${LIBBLYTC_SRCS}
  RESULT_VARIABLE R)
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to build libblytc.so")
endif()

message(STATUS "Building libblyt32.so…")
# libblyt32.so (emulated path) — Blyt32 ECALL stubs.
#
# Self-contained for cart LINK TIME: absorbs libblytcommon sources and ALL
# libblytc sources so carts need only -lblyt32 at build time.  Exporting
# malloc/free/string/math functions directly from libblyt32.so's .dynsym lets
# lld resolve them without adding libblytc.so to the cart's DT_NEEDED.
#
# Runtime: libblyt32.so declares DT_NEEDED: libblytc.so (forced via
# --no-as-needed).  The runtime's BFS dynamic loader picks up libblytc.so
# transitively.  The first-wins symbol rule means libblyt32.so's baked-in copies
# of malloc etc. are used on the emulated/libretro path; libblytc.so's copies
# are shadowed but the library is present for the hardware trusted-exec path
# where ld.so resolves against it directly.
execute_process(
  COMMAND
    "${FOUND_CLANG}" ${RV32_BASE} ${LIBBLYTC_INCLUDES} ${LIBBLYTC_CFLAGS}
    -Wl,-soname,libblyt32.so -L "${SDK_LIB}" -Wl,--no-as-needed -lblytc
    -Wl,--as-needed -Wl,-rpath-link,"${SDK_LIB}" -o "${SDK_LIB}/libblyt32.so"
    "${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32/blyt32.c"
    "${BLYT_SOURCE_DIR}/runtime/guest/src/libblytcommon/blyt_common.c"
    ${LIBBLYTC_SRCS}
  RESULT_VARIABLE R)
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to build libblyt32.so")
endif()

message(STATUS "Building libblyt32.so (native build)…")
# libblyt32.so (native path) — real API implementations for trusted native exec.
#
# Compiled from frontends/native/src/libblyt32/blyt32.c.  Provides real Linux
# syscall implementations (blyt_console_debug → write(2,...)) and the restricted
# seccomp constructor.  Placed in sdk/lib/native/ so the launcher can set
# LD_LIBRARY_PATH=<sdk>/lib/native to load this over the emulated version.
set(SDK_LIB_NATIVE "${SDK_LIB}/native")
file(MAKE_DIRECTORY "${SDK_LIB_NATIVE}")
execute_process(
  COMMAND
    "${FOUND_CLANG}" ${RV32_BASE} -I
    "${BLYT_SOURCE_DIR}/frontends/native/src/libblyt32" -Wl,-soname,libblyt32.so
    -L "${SDK_LIB}" -Wl,--no-as-needed -lblytcommon -Wl,--as-needed
    -Wl,-rpath-link,"${SDK_LIB}" -o "${SDK_LIB_NATIVE}/libblyt32.so"
    "${BLYT_SOURCE_DIR}/frontends/native/src/libblyt32/blyt32.c"
  RESULT_VARIABLE R)
# Note: libblytc.so is intentionally NOT linked here.  On native execution the
# cart process already has musl from the ld.so interpreter; loading libblytc.so
# (our musl subset) would create two conflicting musl instances → SIGSEGV.
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to build libblyt32.so (native build)")
endif()
# Stage libblytcommon.so into native/ so one LD_LIBRARY_PATH covers it.
# libblytc.so is NOT staged: it must not be loaded on the native path.
file(COPY "${SDK_LIB}/libblytcommon.so" DESTINATION "${SDK_LIB_NATIVE}")

# -------------------------------------------------------------------------
# Step 3: SDK headers
# -------------------------------------------------------------------------

# Start with a clean include/ so removed headers don't linger across rebuilds.
file(REMOVE_RECURSE "${SDK_INC}")
file(MAKE_DIRECTORY "${SDK_INC}")
file(COPY "${BLYT_SOURCE_DIR}/runtime/guest/include/blyt.h"
          "${BLYT_SOURCE_DIR}/runtime/guest/include/blyt32.h"
     DESTINATION "${SDK_INC}")

# Copy the curated subset of musl public headers that correspond to functions
# libblytc.so actually provides (ADR-0120).  Excluded: filesystem I/O (fopen,
# unistd, dirent), pthreads, signals, dynamic loading, networking. Cart code
# that includes an omitted header gets a compile-time error rather than a
# confusing link-time undefined-symbol failure.
foreach(
  _H
  # Infrastructure pulled in by most musl headers
  features.h
  alloca.h
  stdc-predef.h
  # Our curated API surface (ADR-0120)
  assert.h
  complex.h
  ctype.h
  endian.h
  errno.h
  fenv.h
  float.h
  inttypes.h
  iso646.h
  limits.h
  locale.h
  math.h
  stdarg.h
  stdalign.h
  stdbool.h
  stddef.h
  stdint.h
  stdnoreturn.h
  stdio.h
  stdlib.h
  string.h
  strings.h
  time.h
  uchar.h
  wchar.h
  wctype.h)
  file(COPY "${MUSL_DIR}/include/${_H}" DESTINATION "${SDK_INC}")
endforeach()

# subdirectory headers needed by the above
file(COPY "${MUSL_DIR}/include/sys/types.h" DESTINATION "${SDK_INC}/sys")

# bits/ (generated): alltypes.h, syscall.h, and arch-specific headers
file(COPY "${LIBBLYTC_BITS_DIR}/" DESTINATION "${SDK_INC}/bits")

# -------------------------------------------------------------------------
# Step 4: blyt devtool
# -------------------------------------------------------------------------

file(MAKE_DIRECTORY "${SDK_BIN}")
execute_process(
  COMMAND cargo build --manifest-path "${BLYT_SOURCE_DIR}/Cargo.toml" --bin blyt
  RESULT_VARIABLE R)
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to build blyt devtool")
endif()
file(COPY "${BLYT_SOURCE_DIR}/target/debug/blyt" DESTINATION "${SDK_BIN}")

# Expose toolchain binaries under blyt-prefixed names in bin/.
#
# blyt build uses -fuse-ld=<absolute-path-to-blyt-ld.lld> when the SDK lld
# is available, so the exact binary validated at SDK assembly time is always
# used — no dependency on PATH ordering or clang's own-directory lookup.
# The blyt- prefix keeps the name distinct from system ld.lld when sdk/bin/
# is on PATH or installed into a shared directory like /usr/bin.  FOUND_*
# may point into a downloaded SDK_TOOLCHAIN (macOS) or to system tools
# (Linux); we create the symlinks in both cases.
#
# Remove any stale symlinks from prior builds so no dead entries linger.
foreach(_stale blyt-clang blyt-ld.lld blyt-lld blyt-objcopy ld.lld llvm-objcopy)
  file(REMOVE "${SDK_BIN}/${_stale}")
endforeach()
if(FOUND_CLANG)
  file(CREATE_LINK "${FOUND_CLANG}" "${SDK_BIN}/blyt-clang" SYMBOLIC)
endif()
if(FOUND_LLD)
  # blyt-ld.lld: the SDK's private lld, referenced via absolute path in
  # blyt build (-fuse-ld=<abs-path>).  The blyt- prefix avoids any clash with
  # system ld.lld when sdk/bin/ is on PATH or installed into /usr/bin.
  file(CREATE_LINK "${FOUND_LLD}" "${SDK_BIN}/blyt-ld.lld" SYMBOLIC)
endif()
if(FOUND_OBJCOPY)
  file(CREATE_LINK "${FOUND_OBJCOPY}" "${SDK_BIN}/blyt-objcopy" SYMBOLIC)
endif()

# -------------------------------------------------------------------------
# Step 5: blytrun (if built)
# -------------------------------------------------------------------------

if(EXISTS "${BLYT_BINARY_DIR}/blytrun")
  file(COPY "${BLYT_BINARY_DIR}/blytrun" DESTINATION "${SDK_BIN}")
endif()

# -------------------------------------------------------------------------
# Step 6: blyt_libretro.so — host-side libretro core
#
# The libretro core embeds the guest .so files built above so it is
# self-contained.  It links against the cmake-built host static libs (libblyt,
# rv32emu, softfloat, flatccrt).
#
# If the cmake main build already produced blyt_libretro.so (Linux CI, where lld
# is available), that file is used as-is.  Otherwise the core is compiled here
# using the host C compiler.
# -------------------------------------------------------------------------

set(LIBRETRO_OUT "${BLYT_BINARY_DIR}/blyt_libretro.so")

if(EXISTS "${LIBRETRO_OUT}")
  message(STATUS "blyt_libretro.so: already built by cmake — using existing")
else()
  message(STATUS "Building blyt_libretro.so (host)…")

  set(EMBEDDED_LIBS_C "${BLYT_BINARY_DIR}/blyt_embedded_libs.c")

  execute_process(
    COMMAND
      python3 "${BLYT_SOURCE_DIR}/cmake/bin2c.py" blytcommon_so
      "${SDK_LIB}/libblytcommon.so" blytc_so "${SDK_LIB}/libblytc.so" blyt32_so
      "${SDK_LIB}/libblyt32.so" "${EMBEDDED_LIBS_C}"
    RESULT_VARIABLE R)
  if(NOT R EQUAL 0)
    message(FATAL_ERROR "Failed to generate blyt_embedded_libs.c")
  endif()

  # Locate host C compiler (prefer cc, fall back to clang/gcc)
  find_program(HOST_CC NAMES cc clang gcc)
  if(NOT HOST_CC)
    message(FATAL_ERROR "Could not find a host C compiler (cc/clang/gcc)")
  endif()

  # Platform-specific shared-library flags
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(LINK_FLAGS -bundle -undefined dynamic_lookup)
  else()
    set(LINK_FLAGS -shared)
  endif()

  execute_process(
    COMMAND
      "${HOST_CC}" ${LINK_FLAGS} -fPIC -o "${LIBRETRO_OUT}"
      "-DBLYT_VERSION=\"${BLYT_VERSION}\"" "-DBLYT_EMBED_LIBS"
      "${BLYT_SOURCE_DIR}/frontends/libretro/blyt_libretro.c"
      "${EMBEDDED_LIBS_C}" -I
      "${BLYT_SOURCE_DIR}/third_party/libretro-common/include" -I
      "${BLYT_SOURCE_DIR}/runtime/host/include" "${BLYT_BINARY_DIR}/libblyt.a"
      "${BLYT_BINARY_DIR}/liblibblytemu.a"
      "${BLYT_BINARY_DIR}/liblibsoftfloat.a"
      "${BLYT_SOURCE_DIR}/third_party/flatcc/lib/libflatccrt.a"
    RESULT_VARIABLE R)
  if(NOT R EQUAL 0)
    message(FATAL_ERROR "Failed to build blyt_libretro.so")
  endif()

  # macOS requires a code signature on dynamically loaded bundles; use ad-hoc
  # signing (no developer certificate) so dlopen succeeds under SIP.
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    find_program(CODESIGN codesign)
    if(CODESIGN)
      execute_process(COMMAND "${CODESIGN}" --force --sign - "${LIBRETRO_OUT}"
                      RESULT_VARIABLE R)
      if(NOT R EQUAL 0)
        message(FATAL_ERROR "codesign failed for blyt_libretro.so")
      endif()
    endif()
  endif()

  message(STATUS "Built ${LIBRETRO_OUT}")
endif()

# -------------------------------------------------------------------------
# Step 7: WASM runtime (optional — requires Emscripten)
#
# Builds blyt_wasm.html + blyt_wasm.js + blyt_wasm.wasm using the guest
# libraries assembled in Step 2.  The outputs are placed in build/sdk/wasm/ so
# `blyt run` can locate them via the SDK layout.
#
# Skipped silently when emcc is not on PATH.
# -------------------------------------------------------------------------

set(SDK_WASM "${SDK_DIR}/wasm")

find_program(EMCC emcc)
if(EMCC)
  message(STATUS "Building WASM runtime (emcc found at ${EMCC})…")

  file(MAKE_DIRECTORY "${SDK_WASM}")

  # Configure the WASM cmake project pointing at the SDK guest libs.
  execute_process(
    COMMAND
      emcmake ${CMAKE_COMMAND} -B "${BLYT_BINARY_DIR}/build-wasm" -S
      "${BLYT_SOURCE_DIR}/frontends/wasm" "-DBLYT_GUEST_LIB_DIR=${SDK_LIB}"
      "-DBLYT_VERSION=${BLYT_VERSION}" -G "Unix Makefiles"
    RESULT_VARIABLE R
    OUTPUT_QUIET)
  if(NOT R EQUAL 0)
    message(
      WARNING "blyt_wasm: emcmake cmake configure failed — skipping WASM step")
    set(EMCC "")
  endif()
endif()

if(EMCC)
  execute_process(COMMAND ${CMAKE_COMMAND} --build
                          "${BLYT_BINARY_DIR}/build-wasm" RESULT_VARIABLE R)
  if(NOT R EQUAL 0)
    message(WARNING "blyt_wasm: build failed — skipping WASM step")
    set(EMCC "")
  endif()
endif()

if(EMCC)
  foreach(_F blyt_wasm.html blyt_wasm.js blyt_wasm.wasm)
    if(EXISTS "${BLYT_BINARY_DIR}/build-wasm/${_F}")
      file(COPY "${BLYT_BINARY_DIR}/build-wasm/${_F}" DESTINATION "${SDK_WASM}")
    endif()
  endforeach()
  message(STATUS "blyt WASM runtime assembled at ${SDK_WASM}")
  message(STATUS "  Run a cart:  ${SDK_BIN}/blyt run <cart.blyt>")
else()
  message(
    STATUS
      "blyt_wasm: skipped (emcc not found — install Emscripten to build WASM runtime)"
  )
endif()

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------

message(STATUS "blyt SDK assembled at ${SDK_DIR}")
message(STATUS "  bin:       ${SDK_BIN}")
message(STATUS "  include:   ${SDK_INC}")
message(STATUS "  lib:       ${SDK_LIB}")
if(EXISTS "${SDK_TOOLCHAIN}")
  message(STATUS "  toolchain: ${SDK_TOOLCHAIN}")
endif()
message(STATUS "")
message(
  STATUS
    "Build a cart:  BLYT_SDK_DIR=${SDK_DIR} ${SDK_BIN}/blyt build <project>")
