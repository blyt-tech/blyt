# Spike V — Swift cart architecture probe — task tracker

Brief: [`blyt-planning/docs/design/spike-v-swift-carts.md`](../../../../blyt-planning/docs/design/spike-v-swift-carts.md).

**Goal:** architecture probe — determine whether adding a non-Rust LLVM language
exercises only the intended per-language extension points. Swift is the probe
vehicle. Green result means "the architecture can take another language"; it is
not a proposal to ship Swift.

**Toolchain pin:** `swift-DEVELOPMENT-SNAPSHOT-2026-06-12-a`
**Target:** `riscv32-unknown-none-elf` / ilp32d
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

*(additional entries appended below as stages complete)*

---

## Stage 0 — Toolchain: Embedded Swift emitting `riscv32 / ilp32d`

- [x] Dockerfile installs `swift-DEVELOPMENT-SNAPSHOT-2026-06-12-a` (arm64 + amd64 variants)
- [x] `trivial.swift` compiles to `trivial.o` with `-enable-experimental-feature Embedded`
      `-target riscv32-none-none-eabi` + `-Xllvm -mattr=+m,+a,+f,+d,+c` + `-Xcc -march=rv32imafdc -Xcc -mabi=ilp32d`
- [x] `llvm-readelf -h -A trivial.o` shows `double-float ABI` (flags 0x5) — arm64 ✓
- [x] arch attr shows `rv32i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_…` (F, D, C present) — arm64 ✓
- [ ] arm64 and amd64 builds both pass the readelf gate (amd64 in progress)
- [ ] `make stage-0` exits 0

## Stage 1 — Link + run against the `ilp32d` sysroot

- [ ] Bridging header imports `runtime/guest/include/blyt.h`
- [ ] `cart.swift` provides `blyt_cart_init/update/draw` as `@_cdecl` exports
- [ ] `-Wl,-u,blyt_cart_init -Wl,-u,blyt_cart_update -Wl,-u,blyt_cart_draw` applied
      (Finding O-5 from Spike O: weak-symbol entry-point retention)
- [ ] Cart ELF links against ilp32d guest libs with no float-ABI mismatch warning
- [ ] Cart runs in rv32emu to completion (at least one frame)
- [ ] `make stage-1` exits 0

## Stage 2 — `ilp32d` ABI witness (Swift)

- [ ] `abi_witness.swift`: `@_cdecl` function takes a `Double`, emits raw IEEE-754 hex
      (via pointer cast or C union from bridging header — `fmv.x.d` not available on RV32)
- [ ] `set_param(0, 0.5)` call → `param=3fe0000000000000` on arm64 host
- [ ] Same output on amd64 host
- [ ] Both outputs byte-identical
- [ ] `make stage-2` exits 0

## Stage 3 — Runtime symbols, ARC, allocator

- [ ] `arc_cart.swift` uses a class (forces ARC)
- [ ] `llvm-objdump -d` shows `amoadd.w.aqrl` or `lr.w`/`sc.w` — ARC lowered to native AMOs
      (same finding as Spike P's `Arc` result)
- [ ] `swift_*` ARC refcount shims resolve against musl sysroot (no undefined symbols)
- [ ] `malloc`/`free`/`posix_memalign`/`mem*` resolve against musl
- [ ] Class-using cart runs without illegal-instruction or allocator panic
- [ ] Digest deterministic across hosts (10 frames)
- [ ] `make stage-3` exits 0

## Stage 4 — Digest equivalence vs C and Rust

- [ ] Swift probe cart and Spike O C/Rust reference carts make identical console calls
- [ ] Per-frame FNV-1a-64 digests match: Swift = Rust = C on arm64
- [ ] Same on amd64
- [ ] Cross-host: arm64 Swift == amd64 Swift
- [ ] Four-way equality: Swift/arm64 = Swift/amd64 = Rust/arm64 = C/arm64
- [ ] `make stage-4` exits 0

## Stage 5 — Footprint assumptions

- [ ] `llvm-size` and `llvm-readelf -l` run on the Swift cart ELF
- [ ] Cart loaded through `blyt build` (devtool packer) without error
- [ ] Cart loaded through `cart_load.c` allowlists without error
- [ ] Any hardcoded size limit, section-count, or budget constant tripped is
      logged as a friction entry (FV-N — leaked assumption)
- [ ] Either: "flows through cleanly" or: every assumption logged
- [ ] `make stage-5` exits 0

## Stage 6 — Subset / ergonomics audit

- [ ] Embedded Swift feature set documented (what's available, what's removed)
- [ ] Cart API shape assessed: does it implicitly require capabilities Embedded Swift lacks?
- [ ] Any cart-API shape leak logged as a friction entry
- [ ] Audit complete and logged (no binary pass/fail)
- [ ] `make stage-6` exits 0

---

## Write-up gate

- [ ] Friction log complete: all FV-N entries filled with classification + resolution
- [ ] Primary question answered with evidence: are the onboarding seams language-agnostic?
- [ ] Stage 0-4 pass status documented
- [ ] Footprint findings (Stage 5) documented
- [ ] `spikes/spike-v/RESULTS.md` written
