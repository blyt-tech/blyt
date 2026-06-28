# cmake/blyt_sdk.cmake
#
# Assembles a self-contained blyt SDK under build/sdk/:
#
# build/sdk/ bin/        blyt (devtool), blytplay (SDL2 runtime) include/
# blyt.h, blyt32.h, blyt_runtime.h lib/        libblytcommon.so, libblyt32.so
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

# -------------------------------------------------------------------------
# Toolchain (discovered at configure time, passed in by the sdk target)
# -------------------------------------------------------------------------
set(FOUND_CLANG "${BLYT_RV32_CLANG}")
set(FOUND_CLANGPP "${BLYT_RV32_CLANGPP}")
set(FOUND_LLD "${BLYT_RV32_LLD}")
set(FOUND_OBJCOPY "${BLYT_RV32_OBJCOPY}")
set(FOUND_AR "${BLYT_RV32_AR}")
set(FOUND_LLDB_DAP "${BLYT_RV32_LLDB_DAP}")
if(NOT FOUND_CLANG)
  message(
    FATAL_ERROR
      "No riscv32-capable clang was found at configure time. Install LLVM "
      "(Homebrew llvm+lld / apt clang-22 lld-22) or configure with "
      "-DBLYT_RV32_FETCH=ON, then re-run cmake -B build before building sdk.")
endif()
message(STATUS "blyt SDK: toolchain clang = ${FOUND_CLANG}")

# ccache launcher args for the nested libc++/WASM configures (empty when ccache
# was not found at configure time — expands to nothing).
set(CCACHE_LAUNCHER_ARGS "")
if(BLYT_CCACHE_PROGRAM)
  list(APPEND CCACHE_LAUNCHER_ARGS
       "-DCMAKE_C_COMPILER_LAUNCHER=${BLYT_CCACHE_PROGRAM}"
       "-DCMAKE_CXX_COMPILER_LAUNCHER=${BLYT_CCACHE_PROGRAM}")
endif()

# -------------------------------------------------------------------------
# Step 2: RV32 guest libraries — built by the main ninja graph
#
# All guest libraries (release + debug + native variants, the Lua libs, and
# blyt-luac) are ninja rules emitting directly into sdk/lib[/debug|/native] and
# sdk/bin (cmake/blyt_guest_libs.cmake); the sdk target depends on them, so they
# are already fresh by the time this script runs.  Verify, and define the path
# vars the later steps (libc++, headers) use.
# -------------------------------------------------------------------------
set(SDK_LIB_DEBUG "${SDK_LIB}/debug")
if(NOT EXISTS "${SDK_LIB}/libblyt32.so")
  message(FATAL_ERROR "${SDK_LIB}/libblyt32.so not found — run "
                      "`cmake --build build` (guest_libs target) first.")
endif()

set(MUSL_DIR "${BLYT_MUSL_SOURCE_DIR}")
set(LIBBLYTC_BITS_DIR "${BLYT_BINARY_DIR}/libblytc/bits")
if(NOT EXISTS "${MUSL_DIR}/include/stdio.h")
  message(
    FATAL_ERROR
      "musl source not found at ${MUSL_DIR}. "
      "Re-run cmake -B build to re-fetch, "
      "or clone blyt-tech/musl into third_party/musl.")
endif()

# -------------------------------------------------------------------------
# Step 2b: Build libc++ and libc++abi for RV32IMAFC (C++ cart support)
#
# Builds a static libc++.a + libc++abi.a from third_party/libcxx (the LLVM
# monorepo fork) targeting riscv32imafdc / ilp32d.  Configured with:
# -fno-exceptions -fno-rtti   (mandatory for cart C++ code)
# LIBCXX_ENABLE_THREADS=OFF   (carts are single-threaded)
# LIBCXX_ENABLE_FILESYSTEM=OFF (no filesystem access in sandboxed carts)
# LIBCXX_ENABLE_LOCALIZATION=OFF (locale state is not serialisable)
# LIBCXX_HAS_MUSL_LIBC=ON    (libblytc is our musl-derived C library)
#
# The step is skipped with a warning when: - third_party/libcxx is not
# initialised (the submodule is absent) - clang++ is not available
# (FOUND_CLANGPP is empty)
# -------------------------------------------------------------------------

set(LIBCXX_SOURCE_DIR "${BLYT_LIBCXX_SOURCE_DIR}")
set(LIBCXX_BUILD_DIR "${BLYT_BINARY_DIR}/build-libcxx-rv32")
set(SDK_INC_LIBCXX "${SDK_INC}/c++/v1")

if(NOT EXISTS "${LIBCXX_SOURCE_DIR}/runtimes/CMakeLists.txt")
  message(
    WARNING "libcxx source not found — C++ cart support will not be built.\n"
            "Re-run cmake -B build to re-fetch, "
            "or clone blyt-tech/llvm-project into third_party/libcxx.")
elseif(NOT FOUND_CLANGPP)
  message(WARNING "clang++ not found — C++ cart support will not be built.")
elseif(EXISTS "${SDK_LIB}/libc++.a" AND EXISTS
                                        "${SDK_INC_LIBCXX}/__config_site")
  message(
    STATUS
      "libc++ already built — skipping (delete ${SDK_LIB}/libc++.a to rebuild)")
else()
  message(STATUS "Building libc++ for RV32IMAFC…")

  # Musl include paths: libcxxabi sources include <stdlib.h> via libcxx's
  # wrapper, which does #include_next <stdlib.h>.  Without these paths the cross
  # build can't find ldiv_t, lldiv_t, FP_NAN, etc.
  set(_LXX_MUSL_FLAGS
      "-isystem ${MUSL_DIR}/include -isystem ${MUSL_DIR}/arch/riscv32 -isystem ${MUSL_DIR}/arch/generic -isystem ${MUSL_DIR}/src/internal -isystem ${LIBBLYTC_BITS_DIR}/.."
  )

  # -ffunction-sections/-fdata-sections put each libc++ function/datum in its
  # own section so the cart link's --gc-sections can drop unused std-lib code
  # (e.g. the wide-char std::to_wstring/wcsto* path and aligned operator new
  # pulled in transitively by std::string but never used). Required for debug
  # cart builds, which don't use LTO. (ADR-0121 mandates LTO for release;
  # gc-sections covers both modes.)
  set(_LXX_SECTION_FLAGS "-ffunction-sections -fdata-sections")
  set(_LXX_C_FLAGS
      "--target=riscv32-linux-gnu -march=rv32imafdc -mabi=ilp32d -nostdlib ${_LXX_SECTION_FLAGS} ${_LXX_MUSL_FLAGS}"
  )
  set(_LXX_CXX_FLAGS
      "--target=riscv32-linux-gnu -march=rv32imafdc -mabi=ilp32d -nostdlib -fno-exceptions -fno-rtti ${_LXX_SECTION_FLAGS} ${_LXX_MUSL_FLAGS}"
  )

  if(FOUND_LLD)
    # -fuse-ld must sit in the compile flags so the nested configure's link
    # checks use lld, but compile-only steps then warn it is unused.
    string(APPEND _LXX_C_FLAGS
           " -fuse-ld=${FOUND_LLD} -Wno-unused-command-line-argument")
    string(APPEND _LXX_CXX_FLAGS
           " -fuse-ld=${FOUND_LLD} -Wno-unused-command-line-argument")
  endif()

  # Canonical path remapping for the release libc++ too (issue #46 §4): no DWARF
  # is emitted, but __FILE__/assert strings still embed paths, so this keeps the
  # release archive free of the build machine's absolute paths (byte
  # reproducibility).  Same abs + ccache-relative + comp_dir forms as debug.
  file(RELATIVE_PATH _lxx_rel "${LIBCXX_BUILD_DIR}" "${LIBCXX_SOURCE_DIR}")
  set(_LXX_PREFIX_MAP
      "-ffile-prefix-map=${LIBCXX_SOURCE_DIR}=/blyt/sdk/src/libcxx -ffile-prefix-map=${_lxx_rel}=/blyt/sdk/src/libcxx -ffile-prefix-map=${LIBCXX_BUILD_DIR}=/blyt/sdk/build-libcxx -ffile-prefix-map=${LIBCXX_SOURCE_DIR}/libcxx/include=/blyt/sdk/include/c++/v1 -ffile-prefix-map=${_lxx_rel}/libcxx/include=/blyt/sdk/include/c++/v1"
  )
  string(APPEND _LXX_C_FLAGS " ${_LXX_PREFIX_MAP}")
  string(APPEND _LXX_CXX_FLAGS " ${_LXX_PREFIX_MAP}")

  # Configure
  execute_process(
    COMMAND
      ${CMAKE_COMMAND} -S "${LIBCXX_SOURCE_DIR}/runtimes" -B
      "${LIBCXX_BUILD_DIR}" -G Ninja "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi"
      "-DCMAKE_C_COMPILER=${FOUND_CLANG}"
      "-DCMAKE_CXX_COMPILER=${FOUND_CLANGPP}" ${CCACHE_LAUNCHER_ARGS}
      "-DCMAKE_C_FLAGS=${_LXX_C_FLAGS}" "-DCMAKE_CXX_FLAGS=${_LXX_CXX_FLAGS}"
      -DCMAKE_BUILD_TYPE=MinSizeRel -DLIBCXX_ENABLE_SHARED=OFF
      -DLIBCXX_ENABLE_EXCEPTIONS=OFF -DLIBCXX_ENABLE_RTTI=OFF
      -DLIBCXX_ENABLE_THREADS=OFF -DLIBCXX_ENABLE_FILESYSTEM=OFF
      -DLIBCXX_ENABLE_LOCALIZATION=OFF -DLIBCXX_HAS_MUSL_LIBC=ON
      -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXX_CXX_ABI=libcxxabi
      -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON -DLIBCXXABI_ENABLE_SHARED=OFF
      -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF -DLIBCXXABI_ENABLE_THREADS=OFF
      -DLIBCXXABI_USE_COMPILER_RT=ON -DLIBCXXABI_USE_LLVM_UNWINDER=OFF
      -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF
      -DLLVM_INCLUDE_TESTS=OFF
      # On macOS, cmake injects -arch arm64 / -isysroot into every build even
      # when cross-compiling.  Setting CMAKE_SYSTEM_NAME=Linux tells cmake this
      # is a Linux cross-compile so it suppresses all Apple toolchain defaults.
      # No-op on Linux (CMAKE_SYSTEM_NAME is already Linux).
      -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=riscv32
    RESULT_VARIABLE _LXX_CFG_R
    OUTPUT_QUIET)

  if(NOT _LXX_CFG_R EQUAL 0)
    message(FATAL_ERROR "libc++ cmake configure failed (exit ${_LXX_CFG_R})")
  endif()

  # Build — only the static libraries, not tests
  execute_process(COMMAND ${CMAKE_COMMAND} --build "${LIBCXX_BUILD_DIR}"
                          --target cxx cxxabi RESULT_VARIABLE _LXX_BUILD_R)

  if(NOT _LXX_BUILD_R EQUAL 0)
    message(FATAL_ERROR "libc++ build failed (exit ${_LXX_BUILD_R})")
  endif()

  # Copy static libraries to SDK lib/
  foreach(_lib libc++.a libc++abi.a)
    if(EXISTS "${LIBCXX_BUILD_DIR}/lib/${_lib}")
      file(COPY "${LIBCXX_BUILD_DIR}/lib/${_lib}" DESTINATION "${SDK_LIB}")
    else()
      message(
        FATAL_ERROR
          "libc++ build succeeded but ${_lib} not found in ${LIBCXX_BUILD_DIR}/lib/"
      )
    endif()
  endforeach()

  # Copy libc++ headers to SDK include/c++/v1/. Source headers come from the
  # libcxx include/ tree; cmake-generated files (__config_site,
  # __assertion_handler, etc.) come from the build tree. Copy the build tree's
  # generated include dir on top so all generated files are captured, not just
  # __config_site.
  file(MAKE_DIRECTORY "${SDK_INC_LIBCXX}")
  file(
    COPY "${LIBCXX_SOURCE_DIR}/libcxx/include/"
    DESTINATION "${SDK_INC_LIBCXX}"
    PATTERN "*.in" EXCLUDE) # exclude cmake template files
  if(EXISTS "${LIBCXX_BUILD_DIR}/include/c++/v1")
    file(COPY "${LIBCXX_BUILD_DIR}/include/c++/v1/"
         DESTINATION "${SDK_INC_LIBCXX}")
  endif()

  message(
    STATUS "libc++ built: ${SDK_LIB}/libc++.a + headers at ${SDK_INC_LIBCXX}")
endif()

# -------------------------------------------------------------------------
# Step 2b-debug: debug libc++ / libc++abi (ADR-0129)
#
# A second, unoptimised (-O0 -g) build of the static libc++ for source-level
# debugging of cart C++ code.  Same musl/cross flags as the release build (LTO
# is a separate ADR-0127 follow-up); only the archives land in ${SDK_LIB}/debug
# — headers are shared with the release build above.
# -------------------------------------------------------------------------
set(LIBCXX_DEBUG_BUILD_DIR "${BLYT_BINARY_DIR}/build-libcxx-rv32-debug")

if(NOT EXISTS "${LIBCXX_SOURCE_DIR}/runtimes/CMakeLists.txt" OR NOT
                                                                FOUND_CLANGPP)
  # Skipped: same conditions as the release build warn above.
elseif(EXISTS "${SDK_LIB_DEBUG}/libc++.a")
  message(
    STATUS
      "debug libc++ already built — skipping (delete ${SDK_LIB_DEBUG}/libc++.a to rebuild)"
  )
else()
  message(STATUS "Building debug libc++ for RV32IMAFC (-O0 -g)…")

  set(_LXXD_MUSL_FLAGS
      "-isystem ${MUSL_DIR}/include -isystem ${MUSL_DIR}/arch/riscv32 -isystem ${MUSL_DIR}/arch/generic -isystem ${MUSL_DIR}/src/internal -isystem ${LIBBLYTC_BITS_DIR}/.."
  )
  set(_LXXD_SECTION_FLAGS "-ffunction-sections -fdata-sections -O0 -g")
  set(_LXXD_C_FLAGS
      "--target=riscv32-linux-gnu -march=rv32imafdc -mabi=ilp32d -nostdlib ${_LXXD_SECTION_FLAGS} ${_LXXD_MUSL_FLAGS}"
  )
  set(_LXXD_CXX_FLAGS
      "--target=riscv32-linux-gnu -march=rv32imafdc -mabi=ilp32d -nostdlib -fno-exceptions -fno-rtti ${_LXXD_SECTION_FLAGS} ${_LXXD_MUSL_FLAGS}"
  )
  if(FOUND_LLD)
    # Same -fuse-ld / unused-argument trade-off as the release build above.
    string(APPEND _LXXD_C_FLAGS
           " -fuse-ld=${FOUND_LLD} -Wno-unused-command-line-argument")
    string(APPEND _LXXD_CXX_FLAGS
           " -fuse-ld=${FOUND_LLD} -Wno-unused-command-line-argument")
  endif()

  # Canonical DWARF paths for the debug libc++ archive (issue #46 §4): rewrite
  # the libcxx source tree to /blyt/sdk/src/libcxx and pin comp_dir, so stepping
  # into precompiled libc++ .cpp resolves from the shipped SDK source on any
  # machine and the archive is byte-identical across build dirs.  Abs + the
  # ccache-base_dir-relative form (the nested build runs in its own dir, so the
  # relative path is computed from there), same rationale as the guest libs. The
  # libcxx headers already ship in include/c++/v1, so remap them there rather
  # than duplicating the 15 MB include tree under src/.  This map is more
  # specific than the general libcxx map and is listed last so it wins for
  # header paths (clang applies the last matching -ffile-prefix-map).  Both abs
  # and the ccache-relative form.
  file(RELATIVE_PATH _lxxd_rel "${LIBCXX_DEBUG_BUILD_DIR}"
       "${LIBCXX_SOURCE_DIR}")
  set(_LXXD_PREFIX_MAP
      "-ffile-prefix-map=${LIBCXX_SOURCE_DIR}=/blyt/sdk/src/libcxx -ffile-prefix-map=${_lxxd_rel}=/blyt/sdk/src/libcxx -ffile-prefix-map=${LIBCXX_DEBUG_BUILD_DIR}=/blyt/sdk/build-libcxx -ffile-prefix-map=${LIBCXX_SOURCE_DIR}/libcxx/include=/blyt/sdk/include/c++/v1 -ffile-prefix-map=${_lxxd_rel}/libcxx/include=/blyt/sdk/include/c++/v1"
  )
  string(APPEND _LXXD_C_FLAGS " ${_LXXD_PREFIX_MAP}")
  string(APPEND _LXXD_CXX_FLAGS " ${_LXXD_PREFIX_MAP}")

  execute_process(
    COMMAND
      ${CMAKE_COMMAND} -S "${LIBCXX_SOURCE_DIR}/runtimes" -B
      "${LIBCXX_DEBUG_BUILD_DIR}" -G Ninja
      "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi"
      "-DCMAKE_C_COMPILER=${FOUND_CLANG}"
      "-DCMAKE_CXX_COMPILER=${FOUND_CLANGPP}" ${CCACHE_LAUNCHER_ARGS}
      "-DCMAKE_C_FLAGS=${_LXXD_C_FLAGS}" "-DCMAKE_CXX_FLAGS=${_LXXD_CXX_FLAGS}"
      -DCMAKE_BUILD_TYPE=Debug -DLIBCXX_ENABLE_SHARED=OFF
      -DLIBCXX_ENABLE_EXCEPTIONS=OFF -DLIBCXX_ENABLE_RTTI=OFF
      -DLIBCXX_ENABLE_THREADS=OFF -DLIBCXX_ENABLE_FILESYSTEM=OFF
      -DLIBCXX_ENABLE_LOCALIZATION=OFF -DLIBCXX_HAS_MUSL_LIBC=ON
      -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXX_CXX_ABI=libcxxabi
      -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON -DLIBCXXABI_ENABLE_SHARED=OFF
      -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF -DLIBCXXABI_ENABLE_THREADS=OFF
      -DLIBCXXABI_USE_COMPILER_RT=ON -DLIBCXXABI_USE_LLVM_UNWINDER=OFF
      -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF
      -DLLVM_INCLUDE_TESTS=OFF
      -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=riscv32
    RESULT_VARIABLE _LXXD_CFG_R
    OUTPUT_QUIET)
  if(NOT _LXXD_CFG_R EQUAL 0)
    message(
      FATAL_ERROR "debug libc++ cmake configure failed (exit ${_LXXD_CFG_R})")
  endif()

  execute_process(COMMAND ${CMAKE_COMMAND} --build "${LIBCXX_DEBUG_BUILD_DIR}"
                          --target cxx cxxabi RESULT_VARIABLE _LXXD_BUILD_R)
  if(NOT _LXXD_BUILD_R EQUAL 0)
    message(FATAL_ERROR "debug libc++ build failed (exit ${_LXXD_BUILD_R})")
  endif()

  foreach(_lib libc++.a libc++abi.a)
    if(EXISTS "${LIBCXX_DEBUG_BUILD_DIR}/lib/${_lib}")
      file(COPY "${LIBCXX_DEBUG_BUILD_DIR}/lib/${_lib}"
           DESTINATION "${SDK_LIB_DEBUG}")
    else()
      message(
        FATAL_ERROR
          "debug libc++ build succeeded but ${_lib} not found in ${LIBCXX_DEBUG_BUILD_DIR}/lib/"
      )
    endif()
  endforeach()

  message(STATUS "debug libc++ built: ${SDK_LIB_DEBUG}/libc++.a")
endif()

# -------------------------------------------------------------------------
# Step 2c: ship guest source for debug stepping (issue #46 §5)
# -------------------------------------------------------------------------
# The debug guest libs and libc++ embed canonical /blyt/sdk/src/<component>
# DWARF paths (§4); ship the matching source trees here so a debug client
# resolves them against the installed SDK on any machine.  Layout mirrors the
# prefix maps exactly.  Copied unconditionally — the in-repo build/sdk must be
# complete because the test suite debugs against it; a release tarball may split
# this tree into an optional debug package (ADR-0129).  Each third-party tree
# carries its upstream licence.  libcxx headers are NOT duplicated here: they
# already ship in include/c++/v1 and the libc++ build remaps header DWARF there.
set(SDK_SRC "${SDK_DIR}/src")
file(REMOVE_RECURSE "${SDK_SRC}")
file(MAKE_DIRECTORY "${SDK_SRC}")

# First-party guest libraries: runtime/guest → src/blyt (src/ + include/).
file(COPY "${BLYT_SOURCE_DIR}/runtime/guest/" DESTINATION "${SDK_SRC}/blyt")

# Freestanding shared sources (runtime/shared) compiled into guest libs — e.g.
# the unified-budget arena (blyt_arena.c, #158) in libblytc → src/blyt-shared,
# matching the -ffile-prefix-map so the debug libs' DWARF resolves.
file(COPY "${BLYT_SOURCE_DIR}/runtime/shared/" DESTINATION "${SDK_SRC}/blyt-shared")

# Debug-only DAP master hook, compiled into the debug libblyt32lua →
# src/blyt-dap.
file(COPY "${BLYT_SOURCE_DIR}/runtime/host/src/dap/master_hook.c"
          "${BLYT_SOURCE_DIR}/runtime/host/src/dap/master_hook.h"
     DESTINATION "${SDK_SRC}/blyt-dap")

# musl (libblytc): the compiled subset spans several src/ areas; ship src/,
# include/ and arch/ wholesale so any linked libc function resolves.
if(EXISTS "${BLYT_MUSL_SOURCE_DIR}/include/stdio.h")
  foreach(_d src include arch)
    file(COPY "${BLYT_MUSL_SOURCE_DIR}/${_d}" DESTINATION "${SDK_SRC}/musl")
  endforeach()
  file(COPY "${BLYT_MUSL_SOURCE_DIR}/COPYRIGHT" DESTINATION "${SDK_SRC}/musl")
endif()

# Lua VM (libblyt32lua): flat layout, source at the tree root → src/lua.
if(EXISTS "${BLYT_LUA_SOURCE_DIR}/lua.h")
  file(
    COPY "${BLYT_LUA_SOURCE_DIR}/"
    DESTINATION "${SDK_SRC}/lua"
    FILES_MATCHING
    PATTERN "*.c"
    PATTERN "*.h")
endif()

# rv32emu softfloat builtins, pulled into the debug libblyt32lua.
if(EXISTS "${BLYT_RV32EMU_SOURCE_DIR}/src/softfloat")
  file(COPY "${BLYT_RV32EMU_SOURCE_DIR}/src/softfloat"
       DESTINATION "${SDK_SRC}/rv32emu/src")
  if(EXISTS "${BLYT_RV32EMU_SOURCE_DIR}/LICENSE")
    file(COPY "${BLYT_RV32EMU_SOURCE_DIR}/LICENSE"
         DESTINATION "${SDK_SRC}/rv32emu")
  endif()
endif()

# libc++ / libc++abi .cpp sources (public libcxx headers ship in include/c++/v1
# instead).  Of LLVM libc, only the __support/ utility headers (and libc/shared)
# are pulled into libc++ — ship just those, not the whole 27 MB libc/src tree.
if(EXISTS "${LIBCXX_SOURCE_DIR}/libcxx/src")
  foreach(_d libcxx/src libcxxabi/src libcxxabi/include libc/src/__support
             libc/shared)
    if(EXISTS "${LIBCXX_SOURCE_DIR}/${_d}")
      get_filename_component(_dest_parent "${SDK_SRC}/libcxx/${_d}" DIRECTORY)
      file(COPY "${LIBCXX_SOURCE_DIR}/${_d}" DESTINATION "${_dest_parent}")
    endif()
  endforeach()
  if(EXISTS "${LIBCXX_SOURCE_DIR}/LICENSE.TXT")
    file(COPY "${LIBCXX_SOURCE_DIR}/LICENSE.TXT"
         DESTINATION "${SDK_SRC}/libcxx")
  endif()
endif()

message(STATUS "Guest debug source shipped to ${SDK_SRC}")

# -------------------------------------------------------------------------
# Step 3: SDK headers
# -------------------------------------------------------------------------

# Start with a clean include/ so removed headers don't linger across rebuilds.
file(REMOVE_RECURSE "${SDK_INC}")
file(MAKE_DIRECTORY "${SDK_INC}")
file(
  COPY "${BLYT_SOURCE_DIR}/runtime/guest/include/blyt.h"
       "${BLYT_SOURCE_DIR}/runtime/guest/include/blyt32.h"
       "${BLYT_SOURCE_DIR}/runtime/guest/include/blyt_lua_internal.h"
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

# bits/ (generated): alltypes.h, syscall.h, and arch-specific headers. Also copy
# arch/generic/bits/ which has stdint.h (fast integer types) included
# unconditionally by musl's <stdint.h>.
file(COPY "${MUSL_DIR}/arch/generic/bits/" DESTINATION "${SDK_INC}/bits")
file(COPY "${LIBBLYTC_BITS_DIR}/" DESTINATION "${SDK_INC}/bits")

# Restore libc++ headers after the REMOVE_RECURSE above wiped SDK_INC. Step 2b
# installs them into SDK_INC/c++/v1/ but Step 3 cleans the whole directory;
# re-copy them here so cart C++ builds find their headers.
if(EXISTS "${SDK_LIB}/libc++.a")
  set(SDK_INC_LIBCXX "${SDK_INC}/c++/v1")
  file(MAKE_DIRECTORY "${SDK_INC_LIBCXX}")
  file(
    COPY "${LIBCXX_SOURCE_DIR}/libcxx/include/"
    DESTINATION "${SDK_INC_LIBCXX}"
    PATTERN "*.in" EXCLUDE)
  if(EXISTS "${LIBCXX_BUILD_DIR}/include/c++/v1")
    file(COPY "${LIBCXX_BUILD_DIR}/include/c++/v1/"
         DESTINATION "${SDK_INC_LIBCXX}")
  endif()
endif()

# Lua headers (lua.h, lauxlib.h, lualib.h, luaconf.h) are NOT installed in the
# public SDK include path.  Cart code must not include them directly; use
# BLYT_LUA_EXPORT_* macros which pull in blyt_lua_internal.h automatically. The
# raw Lua C API headers are SDK-internal and only used by devtool-generated
# cart_lua_modules glue code that the cart developer never writes by hand.

# -------------------------------------------------------------------------
# Step 4: blyt devtool
# -------------------------------------------------------------------------

file(MAKE_DIRECTORY "${SDK_BIN}")
set(_cargo_target_dir "${BLYT_BINARY_DIR}/cargo-target")
set(ENV{CARGO_HOME} "${BLYT_BINARY_DIR}/cargo-home")
set(ENV{CARGO_TARGET_DIR} "${_cargo_target_dir}")
execute_process(
  COMMAND cargo build --release --manifest-path "${BLYT_SOURCE_DIR}/Cargo.toml"
          --bin blyt
  RESULT_VARIABLE R)
unset(ENV{CARGO_HOME})
unset(ENV{CARGO_TARGET_DIR})
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to build blyt devtool")
endif()
file(COPY "${_cargo_target_dir}/release/blyt" DESTINATION "${SDK_BIN}")

# Copy Rust SDK crate so blyt can find it via sdk_root_from_exe().
file(COPY "${BLYT_SOURCE_DIR}/sdk/rust" DESTINATION "${SDK_DIR}")

# Expose toolchain binaries under blyt-prefixed names in bin/.
#
# blyt build uses -fuse-ld=<absolute-path-to-blyt-ld.lld> when the SDK lld is
# available, so the exact binary validated at SDK assembly time is always used —
# no dependency on PATH ordering or clang's own-directory lookup. The blyt-
# prefix keeps the name distinct from system ld.lld when sdk/bin/ is on PATH or
# installed into a shared directory like /usr/bin.  FOUND_* may point into a
# downloaded SDK_TOOLCHAIN (macOS) or to system tools (Linux); we create the
# symlinks in both cases.
#
# Remove any stale symlinks from prior builds so no dead entries linger.
foreach(
  _stale
  blyt-clang
  blyt-clang++
  blyt-ld.lld
  blyt-lld
  blyt-objcopy
  blyt-llvm-ar
  blyt-lldb-dap
  ld.lld
  llvm-objcopy)
  file(REMOVE "${SDK_BIN}/${_stale}")
endforeach()
if(FOUND_CLANG)
  file(CREATE_LINK "${FOUND_CLANG}" "${SDK_BIN}/blyt-clang" SYMBOLIC)
endif()
if(FOUND_CLANGPP)
  file(CREATE_LINK "${FOUND_CLANGPP}" "${SDK_BIN}/blyt-clang++" SYMBOLIC)
endif()
if(FOUND_LLD)
  # blyt-ld.lld: the SDK's private lld, referenced via absolute path in blyt
  # build (-fuse-ld=<abs-path>).  The blyt- prefix avoids any clash with system
  # ld.lld when sdk/bin/ is on PATH or installed into /usr/bin.
  file(CREATE_LINK "${FOUND_LLD}" "${SDK_BIN}/blyt-ld.lld" SYMBOLIC)
endif()
if(FOUND_OBJCOPY)
  file(CREATE_LINK "${FOUND_OBJCOPY}" "${SDK_BIN}/blyt-objcopy" SYMBOLIC)
endif()
if(FOUND_AR)
  file(CREATE_LINK "${FOUND_AR}" "${SDK_BIN}/blyt-llvm-ar" SYMBOLIC)
endif()
if(FOUND_LLDB_DAP)
  file(CREATE_LINK "${FOUND_LLDB_DAP}" "${SDK_BIN}/blyt-lldb-dap" SYMBOLIC)
endif()
# -------------------------------------------------------------------------
# Step 5: blytplay / blytdebug
#
# Nothing to do: the main cmake build emits both players directly into sdk/bin/
# (RUNTIME_OUTPUT_DIRECTORY), so a plain `cmake --build build` keeps them fresh
# — no copy step to go stale.  ADR-0129: blytdebug is SDK-only — never embedded
# in / shipped with the production runtime.
# -------------------------------------------------------------------------

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

  if(EXISTS "${SDK_LIB}/libblyt32lua.so")
    execute_process(
      COMMAND
        python3 "${BLYT_SOURCE_DIR}/cmake/bin2c.py" blytcommon_so
        "${SDK_LIB}/libblytcommon.so" blytc_so "${SDK_LIB}/libblytc.so"
        blyt32_so "${SDK_LIB}/libblyt32.so" blyt32lua_so
        "${SDK_LIB}/libblyt32lua.so" "${EMBEDDED_LIBS_C}"
      RESULT_VARIABLE R)
    set(_libretro_embed_defs "-DBLYT_EMBED_LIBS" "-DBLYT_EMBED_LUA")
  else()
    execute_process(
      COMMAND
        python3 "${BLYT_SOURCE_DIR}/cmake/bin2c.py" blytcommon_so
        "${SDK_LIB}/libblytcommon.so" blytc_so "${SDK_LIB}/libblytc.so"
        blyt32_so "${SDK_LIB}/libblyt32.so" "${EMBEDDED_LIBS_C}"
      RESULT_VARIABLE R)
    set(_libretro_embed_defs "-DBLYT_EMBED_LIBS")
  endif()
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
      ${_libretro_embed_defs}
      "${BLYT_SOURCE_DIR}/frontends/libretro/blyt_libretro.c"
      "${EMBEDDED_LIBS_C}" -I "${BLYT_LIBRETRO_COMMON_SOURCE_DIR}/include" -I
      "${BLYT_SOURCE_DIR}/runtime/host/include" "${BLYT_BINARY_DIR}/libblyt.a"
      "${BLYT_BINARY_DIR}/liblibblytemu.a"
      "${BLYT_BINARY_DIR}/liblibsoftfloat.a"
      "${BLYT_BINARY_DIR}/flatcc-lib/libflatccrt.a"
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
# Builds blytplay.js + blytplay.wasm using the guest libraries from Step 2.
# Outputs land in sdk/share/wasm/ so `blyt run` can locate them and developers
# can embed them directly without any build commands.
#
# Skipped silently when emcc is not on PATH.
# -------------------------------------------------------------------------

set(SDK_SHARE_WASM "${SDK_DIR}/share/wasm")
set(SDK_SHARE_WASM_DEBUG "${SDK_DIR}/share/wasm-debug")

find_program(EMCC emcc)
if(EMCC)
  message(STATUS "Building WASM runtimes (emcc found at ${EMCC})…")
  file(MAKE_DIRECTORY "${SDK_SHARE_WASM}")
  file(MAKE_DIRECTORY "${SDK_SHARE_WASM_DEBUG}")

  # ADR-0129: build the WASM player twice. release → blytplay.*  (DAP/GDB OFF,
  # release libs) → share/wasm debug   → blytdebug.* (DAP/GDB ON,  debug libs) →
  # share/wasm-debug `blyt run` serves the release set; `blyt debug` serves the
  # debug set.
  foreach(_w release debug)
    if("${_w}" STREQUAL "debug")
      set(_WDIR "${BLYT_BINARY_DIR}/build-wasm-debug")
      set(_WLIBS "${SDK_LIB_DEBUG}")
      set(_WNAME "blytdebug")
      set(_WDEST "${SDK_SHARE_WASM_DEBUG}")
      set(_WDBG ON)
    else()
      set(_WDIR "${BLYT_BINARY_DIR}/build-wasm")
      set(_WLIBS "${SDK_LIB}")
      set(_WNAME "blytplay")
      set(_WDEST "${SDK_SHARE_WASM}")
      set(_WDBG OFF)
    endif()

    message(STATUS "Building ${_WNAME} WASM runtime (${_w})…")
    # BLYT_WASM_OUT_DIR makes the emcmake tree emit .html/.js/.wasm directly
    # into the SDK share dir, so later incremental `cmake --build ${_WDIR}`
    # invocations update the SDK in place (no copy step to go stale).
    execute_process(
      COMMAND
        emcmake ${CMAKE_COMMAND} -B "${_WDIR}" -S
        "${BLYT_SOURCE_DIR}/frontends/wasm" "-DBLYT_GUEST_LIB_DIR=${_WLIBS}"
        "-DBLYT_VERSION=${BLYT_VERSION}" "-DBLYT_WASM_NAME=${_WNAME}"
        "-DBLYT_WASM_OUT_DIR=${_WDEST}" "-DBLYT_DAP=${_WDBG}"
        "-DBLYT_GDB=${_WDBG}" "-DBLYT_LUA_SOURCE_DIR=${BLYT_LUA_SOURCE_DIR}"
        "-DBLYT_RV32EMU_SOURCE_DIR=${BLYT_RV32EMU_SOURCE_DIR}"
        "-DBLYT_FLATCC_SOURCE_DIR=${BLYT_FLATCC_SOURCE_DIR}"
        "-DBLYT_ZSTD_SOURCE_DIR=${BLYT_ZSTD_SOURCE_DIR}"
        ${CCACHE_LAUNCHER_ARGS} -G Ninja
      RESULT_VARIABLE R
      OUTPUT_QUIET)
    if(NOT R EQUAL 0)
      message(WARNING "${_WNAME} WASM: configure failed — skipping")
    else()
      execute_process(COMMAND ${CMAKE_COMMAND} --build "${_WDIR}"
                      RESULT_VARIABLE R)
      if(NOT R EQUAL 0)
        message(WARNING "${_WNAME} WASM: build failed — skipping")
      else()
        message(STATUS "${_WNAME} WASM assembled at ${_WDEST}")
      endif()
    endif()
  endforeach()

  # Write the embedding README alongside the release runtime files.
  file(
    WRITE "${SDK_SHARE_WASM}/README.md"
    "# blytplay WASM runtime\n\
\n\
`blytplay.js` and `blytplay.wasm` are the blyt emulator compiled to WebAssembly\n\
via Emscripten.  Drop them on any HTTP server alongside your cart and a page\n\
that wires up the canvas — no build tools required.\n\
\n\
## Requirements\n\
\n\
- Files must be served over HTTP (not `file://`) — browsers block WASM loading\n\
  from the local filesystem.\n\
- The server must send `Content-Type: application/wasm` for `.wasm` files;\n\
  most web servers do this automatically.\n\
- A `<canvas id=\"canvas\" width=\"320\" height=\"240\">` element must exist\n\
  before the script runs.\n\
\n\
## Minimal page\n\
\n\
```html\n\
<!doctype html>\n\
<html>\n\
<head><meta charset=\"utf-8\"><title>My Game</title></head>\n\
<body style=\"background:#111;display:flex;justify-content:center\">\n\
  <canvas id=\"canvas\" width=\"320\" height=\"240\"\n\
          style=\"width:640px;height:480px;image-rendering:pixelated\"></canvas>\n\
  <script>\n\
    var Module = {\n\
      canvas: document.getElementById(\"canvas\"),\n\
      preRun: [function() {\n\
        /* Fetch the cart and write it into the WASM virtual filesystem\n\
         * before main() starts.  addRunDependency delays startup until\n\
         * the fetch completes. */\n\
        Module.addRunDependency(\"cart\");\n\
        fetch(\"my-game.blyt\")\n\
          .then(function(r) { return r.arrayBuffer(); })\n\
          .then(function(buf) {\n\
            FS.writeFile(\"/cart.blyt\", new Uint8Array(buf));\n\
            Module.removeRunDependency(\"cart\");\n\
          });\n\
      }],\n\
      print:    function(s) { console.log(s); },\n\
      printErr: function(s) { console.error(s); },\n\
    };\n\
  </script>\n\
  <script src=\"blytplay.js\"></script>\n\
</body>\n\
</html>\n\
```\n\
\n\
Replace `my-game.blyt` with the path to your cart relative to the page.\n\
\n\
## Development server\n\
\n\
`blyt run <cart.blyt>` serves the runtime and cart together on a local port\n\
and opens the correct URL.  It uses an internal shell page; for a\n\
production-ready page use the template above as a starting point.\n\
")

  message(STATUS "WASM runtimes assembled:")
  message(STATUS "  release: ${SDK_SHARE_WASM} (blyt run)")
  message(STATUS "  debug:   ${SDK_SHARE_WASM_DEBUG} (blyt debug)")
  message(STATUS "  Embed in a page: see ${SDK_SHARE_WASM}/README.md")
else()
  message(
    STATUS
      "blytplay WASM: skipped (emcc not found — install Emscripten to build WASM runtime)"
  )
endif()

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------

message(STATUS "blyt SDK assembled at ${SDK_DIR}")
message(STATUS "  bin:       ${SDK_BIN}")
message(STATUS "  include:   ${SDK_INC}")
message(STATUS "  lib:       ${SDK_LIB}")
message(STATUS "  share:     ${SDK_DIR}/share/")
if(EXISTS "${SDK_TOOLCHAIN}")
  message(STATUS "  toolchain: ${SDK_TOOLCHAIN}")
endif()
message(STATUS "")
message(
  STATUS
    "Build a cart:  BLYT_SDK_DIR=${SDK_DIR} ${SDK_BIN}/blyt build <project>")
