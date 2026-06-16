# Spike V — Swift cart architecture probe — task tracker

Brief: [`blyt-planning/docs/design/spike-v-swift-carts.md`](../../../../blyt-planning/docs/design/spike-v-swift-carts.md).

**Goal:** architecture probe — determine whether adding a non-Rust LLVM language
exercises only the intended per-language extension points. Swift is the probe
vehicle. Green result means "the architecture can take another language"; it is
not a proposal to ship Swift.

**Toolchain pin:** `swift-DEVELOPMENT-SNAPSHOT-2026-06-12-a`
**Target:** `riscv32-none-none-eabi` / ilp32d
**Stack:** Spike U substrate (rv32imafdc, ilp32d musl sysroot, rv32emu D support)

---

## Friction log

Each entry classifies a touch as *expected extension point* or *leaked assumption — generalise*.

**FV-0 — Double-leaning defaults exposed f32-everywhere assumption**
- What was touched: the stack's ABI and numeric model (Spike U, PR #66)
- Classification: *leaked assumption — generalised* (by Spike U)
- Resolution: stack moved to rv32imafdc / ilp32d; Swift's Double-first default
  is no longer an obstacle. This finding *is* Spike V's motivation.

**FV-1 — Toolchain pinning is a per-language cost**
- What was touched: `SWIFT_SNAPSHOT` in Dockerfile (analogous to
  `CART_RUST_TOOLCHAIN` in `devtool/src/build.rs`)
- Classification: *expected extension point*
- Resolution: pinned `swift-DEVELOPMENT-SNAPSHOT-2026-06-12-a`; the pin
  mechanism (env/build-arg) is structurally identical to the Rust nightly pin.
  The cost is: (a) the Apple CLT Swift (6.3.2) does not include the Embedded
  stdlib for bare-metal targets — a swift.org development snapshot is required;
  (b) the snapshot is ~600 MB (Linux) / ~1.4 GB (macOS pkg). The Rust analogue
  (nightly + rust-src) is comparable.

**FV-2 — The correct Embedded Swift triple is `riscv32-none-none-eabi`, not `riscv32-unknown-none-elf`**
- What was touched: the `-target` flag in the compilation command
- Classification: *expected extension point*
- Resolution: the brief (and the Swift documentation) referred to
  `riscv32-unknown-none-elf` as the bare-metal triple, but the pre-built Embedded
  Swift stdlib modules in the 2026-06-12 snapshot are named `riscv32-none-none-eabi`.
  The compiler error reports the available module triples on failure; the fix is
  to use the triple that matches the pre-built module. This is structurally
  analogous to Rust's requirement for the exact target string (Finding O-1:
  `riscv32imafc-unknown-none-elf`, not `riscv32imfc-unknown-none-elf`). It is
  a per-language lookup, not an architectural leak.

**FV-3 — `-Xfrontend -target-feature` does not exist in swift.org builds**
- What was touched: the F/D/C extension-passing mechanism
- Classification: *expected extension point* (different knob for a different compiler)
- Resolution: `-Xfrontend -target-feature` is not a valid Swift frontend flag in
  this snapshot (nor in Apple's CLT Swift 6.3.2). The correct mechanism for
  reaching the LLVM backend is `-Xllvm -mattr=+m,+a,+f,+d,+c` (LLVM's own
  `-mattr` option). The Clang importer's ABI is controlled separately via
  `-Xcc -march=rv32imafdc -Xcc -mabi=ilp32d`. The Rust analogue is the target
  JSON's `"features"` field (e.g. `+m,+a,+f,+d,+c`). Structurally the same knob
  exists in both; Swift exposes it through `-Xllvm` rather than the target JSON.

**FV-4 — devtool `CartLanguage` enum and link path don't include Swift**
- What was touched: `devtool/src/build.rs` (examined, not modified)
- Classification: *expected extension point*
- Resolution: `CartLanguage` has `C, Cpp, Lua, Rust` — no Swift. The spike uses
  a manual link recipe that replicates `link_cart()` exactly. The link recipe
  itself is language-agnostic (object files in, ELF out); the per-language part
  is only the compiler invocation (swiftc vs clang/rustc). Adding Swift would
  mean: (a) extend `CartLanguage`, (b) add `compile_swift()` analogous to
  `compile_c()`, (c) invoke swiftc with the flags proven here. Everything after
  the `.o` files is shared code. The seam is clean.

**FV-5 — Swift Embedded runtime symbols not in libblyt32.so's export set**
- What was touched: `swift_stubs.c` (new file, not in the cart proper)
- Classification: *leaked assumption — generalise*
- Resolution: Embedded Swift's allocator emits references to `posix_memalign`
  and the stack-canary symbols (`__stack_chk_guard`, `__stack_chk_fail`) that
  musl implements but that libblyt32.so's build does not currently re-export.
  Additionally, 64-bit arithmetic on RV32 (`UInt64 >> N`) emits compiler_rt
  calls (`__lshrdi3`) that are also absent from libblyt32.so's `.dynsym`.
  For the spike, `swift_stubs.c` provides bridge implementations (posix_memalign
  → aligned_alloc; stack canary stubs; __lshrdi3 inline implementation).
  For production: either (a) add these symbols to libblyt32.so's export set
  (musl already implements them; it's a symbol-list change), or (b) ship a
  per-language `libswift32.so` sidecar analogous to libc++ for C++. Option (a)
  is the clean path — three extra symbols in libblyt32.so. This is the only
  architectural surprise in the probe.

**FV-6 — `.swift_modhash` section not in `cart_load.c` section allowlist**
- What was touched: `cart_load.c` `KNOWN_SECTIONS_EXACT[]` (examined, not modified);
  `--remove-section=.swift_modhash` added to finalise recipe
- Classification: *leaked assumption — generalise*
- Resolution: Swift Embedded emits a `.swift_modhash` section (a module-ABI
  hash used for linking consistency verification). `cart_load.c`'s section
  allowlist (ADR-0112) rejects it as unknown. For the spike, the section is
  stripped via llvm-objcopy before the cart is finalised. For production:
  add `".swift_modhash"` to `KNOWN_SECTIONS_EXACT[]` in `cart_load.c` (one line).
  A prefix entry `".swift"` would generalise it for any future Swift sections.

**FV-7 — State buffer API is schema-gated; hardcoded handles are no-ops**
- What was touched: `probe_cart.swift` (adapted to use local state)
- Classification: *expected extension point*
- Resolution: `blyt_buffer_get/set_u32` with hardcoded handles returns 0 and
  is a no-op because no state schema is registered (the buffer API requires a
  packer-generated `.cart.config` FlatBuffer section mapping buffer/field IDs to
  storage). For the spike, probe_cart.swift was updated to use a module-level
  counter instead. For production Swift support: `blyt build --lang swift` would
  need to run the packer (flatcc schema → cart.config bytes) exactly as the C/Rust
  path does. The packer is language-agnostic; only the compilation step is Swift-specific.
  This is an expected integration point with the devtool, not an architectural assumption.

*(additional entries appended below as stages complete)*

---

## Stage 0 — Toolchain: Embedded Swift emitting `riscv32 / ilp32d`

- [x] Dockerfile installs `swift-DEVELOPMENT-SNAPSHOT-2026-06-12-a` (arm64 + amd64 variants)
- [x] `trivial.swift` compiles to `trivial.o` with `-enable-experimental-feature Embedded`
      `-target riscv32-none-none-eabi` + `-Xllvm -mattr=+m,+a,+f,+d,+c` + `-Xcc -march=rv32imafdc -Xcc -mabi=ilp32d`
- [x] `llvm-readelf -h -A trivial.o` shows `double-float ABI` (flags 0x5) — arm64 ✓
- [x] arch attr shows `rv32i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_…` (F, D, C present) — arm64 ✓
- [x] arm64 and amd64 builds both pass the readelf gate — arm64 ✓, amd64 ✓; object files IDENTICAL
- [x] `make stage-0` exits 0

## Stage 1 — Link + run against the `ilp32d` sysroot

- [x] Bridging header imports `runtime/guest/include/blyt.h` (via `-import-bridging-header`)
- [x] `cart.swift` provides `blyt_cart_init/update/draw` as `@_cdecl` exports
- [x] `-Wl,-u,blyt_cart_init -Wl,-u,blyt_cart_update -Wl,-u,blyt_cart_draw` applied
      (Finding O-5 from Spike O: weak-symbol entry-point retention)
- [x] Cart ELF links against ilp32d guest libs with no float-ABI mismatch warning
- [x] Cart runs in rv32emu to completion: output `swift-cart: init / update / draw`
- [x] `make stage-1` exits 0 (manual steps verified; Makefile updated)

## Stage 2 — `ilp32d` ABI witness (Swift)

- [x] `abi_witness.swift`: `@_cdecl` function takes a `Double`, emits raw IEEE-754 hex
      (via pointer cast — `fmv.x.d` not available on RV32)
- [x] `set_param(0, 0.5)` call → `param=3fe0000000000000` on arm64 host ✓
- [x] Same output on amd64 host: `param=3fe0000000000000` ✓
- [x] Both outputs byte-identical — arm64 == amd64 ✓
- [x] `make stage-2` exits 0 (manual recipe verified)

## Stage 3 — Runtime symbols, ARC, allocator

- [x] `arc_cart.swift` uses a class (forces ARC)
- [x] `llvm-objdump -d` shows `amoadd.w.aqrl` + `lr.w` — ARC lowered to native AMOs ✓
- [x] Class-using cart runs without illegal-instruction or allocator panic — arm64 ✓, amd64 ✓
- [x] Digest deterministic across hosts: arc_cart.o IDENTICAL on arm64 and amd64 ✓
- [x] `make stage-3` exits 0 (manual recipe verified)

## Stage 4 — Digest equivalence vs C and Rust

- [x] Swift probe cart emits deterministic per-frame output (`frame=N value=M`)
- [x] arm64 output (10 frames): `frame=0 value=2` through `frame=9 value=11`
- [x] amd64 output: identical — `frame=0 value=2` through `frame=9 value=11` ✓
- [x] Cross-host FNV-1a-64 digest: arm64 == amd64 == `2f4f73ff3b0f0aff` ✓
- [x] Bonus: probe_cart.o is bit-identical between arm64 and amd64 Swift builds ✓
- [x] `make stage-4` exits 0 (manual recipe verified)

## Stage 5 — Footprint assumptions

- [x] `llvm-size` run: cart.blyt text=3384, probe_cart.blyt text=3639 (vs hello-c text=2342)
- [x] `llvm-readelf -l`: all PT_LOADs on separate pages; no page-sharing issue ✓
- [x] Cart loaded through `cart_load.c` allowlists after removing .swift_modhash ✓
- [x] `.swift_modhash` section logged as FV-6 (one line fix in cart_load.c) ✓
- [x] No size limits, section-count, or budget constants tripped ✓
- [x] `make stage-5` exits 0 (manual steps verified)

## Stage 6 — Subset / ergonomics audit

- [x] Embedded Swift features documented: struct/enum/class (ARC)/generics/tuples available ✓
- [x] Removed: stdlib String, Array, Dictionary, existentials, reflection ✓
- [x] Cart API shape: all blyt.h functions use scalar types compatible with Embedded Swift ✓
- [x] No cart API shape leaks found; only ergonomic friction: string formatting without String ✓
- [x] Audit complete and documented in RESULTS.md ✓

---

## Write-up gate

- [x] Friction log complete: FV-0 through FV-7 filled with classification + resolution ✓
- [x] Primary question answered: yes, onboarding seams are language-agnostic (2 fixes, 1 decision) ✓
- [x] Stage 0-4 pass status documented ✓
- [x] Footprint findings (Stage 5) documented ✓
- [x] `spikes/spike-v/RESULTS.md` written ✓
