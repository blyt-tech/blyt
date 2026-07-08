# frontends/native-hostlua/CMakeLists.txt — Spike Z native host-Lua determinism
# leg (issue #225, ADR-0135).
#
# A minimal native host-Lua execution leg that runs a cart's Lua the same way the
# WASM host-Lua fast path does, but compiled for the native host. NOT a shipped
# player — a determinism gate. It builds on the SHARED VM recipe
# (cmake/blyt_hostlua_vm.cmake, included at top level, #238) so the gate and the
# shipped host-Lua path in libblyt build the *identical* VM: a divergence in the
# pinned hash seed or the contraction flag is exactly the class of bug this gate
# exists to catch, so there must be exactly one copy of the recipe.

if(NOT BLYT_HOSTLUA_VM_AVAILABLE)
  message(STATUS "blyt native host-Lua leg: host-Lua VM unavailable — skipping")
  return()
endif()

# On x86-64, FMA (FMA3) is not baseline `-march=x86-64`, so enable FMA codegen
# explicitly for the negative control; on AArch64 FMA is baseline.
set(_NHL_FMA_ENABLE "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  set(_NHL_FMA_ENABLE -mfma)
endif()

# blyt_add_native_hostlua(<name> <contract_flag> [extra_flags]) — builds the VM
# (via the shared helper) + the runner executable <name>. The runner carries the
# variant's contraction setting so its C-level Q1 torture (torture_digest)
# reflects -ffp-contract=off vs =fast; -O2 so the compiler actually fuses
# multiply-adds under =fast (never at -O0).
function(blyt_add_native_hostlua NAME CONTRACT_FLAG)
  set(_EXTRA ${ARGN})
  blyt_add_hostlua_vm(${NAME}_vm ${CONTRACT_FLAG} ${_EXTRA})

  add_executable(${NAME} "${CMAKE_CURRENT_SOURCE_DIR}/hostlua_runner.c"
                         "${CMAKE_SOURCE_DIR}/runtime/shared/blyt_elf_section.c")
  target_include_directories(${NAME} PRIVATE "${CMAKE_SOURCE_DIR}/runtime/shared")
  target_compile_options(${NAME} PRIVATE -O2 ${CONTRACT_FLAG} ${_EXTRA} -fno-fast-math)
  target_link_libraries(${NAME} PRIVATE ${NAME}_vm)
endfunction()

# The determinism leg: -ffp-contract=off, native f64 (Q2 Mode A — the shippable
# realization). This is the artifact the fp_parity cross-arch gate drives.
blyt_add_native_hostlua(blyt_hostlua_native -ffp-contract=off)

# Q1 negative control (#225): the SAME build with FMA contraction ENABLED
# (-ffp-contract=fast) and FMA codegen available (baseline on arm64, -mfma on
# x86-64). On FMA silicon a fused a*b+c rounds once instead of twice, so the
# runner's C torture MUST diverge from the -ffp-contract=off build — proving
# -ffp-contract=off is load-bearing and the gate has teeth.
blyt_add_native_hostlua(blyt_hostlua_native_fma -ffp-contract=fast
                        ${_NHL_FMA_ENABLE})
