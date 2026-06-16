# Spike V Results — Embedded Swift as a blyt Cart Language Probe

**Date:** 2026-06-16  
**Toolchain:** `swift-DEVELOPMENT-SNAPSHOT-2026-06-12-a` (Embedded Swift)  
**Target:** `riscv32-none-none-eabi` / ilp32d (`-Xcc -march=rv32imafdc -Xcc -mabi=ilp32d -Xllvm -mattr=+m,+a,+f,+d,+c`)  
**Question:** Does adding a new LLVM-based cart language (Swift) exercise only the
intended per-language extension points, or does it reveal leaked architectural
assumptions?

---

## Primary result

**The architecture is language-agnostic at the seams that matter.** Seven
friction findings were logged; after classification, only **two are architectural
issues** (FV-5, FV-6). Both are small, localized fixes. The other five are
expected per-language extension points with direct analogues in the Rust/C/C++
onboarding paths.

---

## Stage results

| Stage | Description | Result |
|-------|-------------|--------|
| 0 | Embedded Swift → riscv32/ilp32d object + readelf gate | PASS — arm64 ✓, amd64 ✓ |
| 1 | Link + run via blytplay | PASS — `swift-cart: init/update/draw` ✓ |
| 2 | ilp32d ABI witness (Double 0.5 → `3fe0000000000000`) | PASS — arm64 ✓, amd64 ✓ |
| 3 | ARC lowers to native AMOs (amoadd.w.aqrl); no traps | PASS — arm64 ✓, amd64 ✓ |
| 4 | Cross-host digest: arm64 == amd64 (`2f4f73ff3b0f0aff`) | PASS |
| 5 | Footprint; `cart_load.c` allowlists (after FV-6 workaround) | PASS |
| 6 | Ergonomics audit (see below) | Complete |

**Object-file identity**: The Swift Embedded compiler produces **bit-identical**
`.o` files from arm64 and amd64 Linux hosts for all four cart sources (`cart`,
`abi_witness`, `arc_cart`, `probe_cart`). This is a stronger cross-host
guarantee than the spike required.

---

## Friction findings

### FV-1 — Toolchain pinning (expected extension point)

`swift-DEVELOPMENT-SNAPSHOT-2026-06-12-a` is the required pin — the Apple
CLT Swift (6.3.2) does not include the Embedded stdlib for bare-metal targets.
Structurally identical to `CART_RUST_TOOLCHAIN` in `devtool/src/build.rs`.
The snapshot is ~600 MB (Linux). **No architecture change required.**

### FV-2 — Embedded Swift triple is `riscv32-none-none-eabi` (expected extension point)

The pre-built module name in the 2026-06-12 snapshot is `riscv32-none-none-eabi`,
not `riscv32-unknown-none-elf` as documented in some Swift references. The
compiler reports available triples on failure. Analogous to Rust's exact target
name requirement. **No architecture change required.**

### FV-3 — F/D/C extension flags via `-Xllvm -mattr=` (expected extension point)

`-Xfrontend -target-feature` is not a valid Swift flag. The correct path to the
LLVM backend is `-Xllvm -mattr=+m,+a,+f,+d,+c`. The Clang importer ABI is
set with `-Xcc -march=rv32imafdc -Xcc -mabi=ilp32d`. Structurally the same
knob as Rust's target-json `"features"` field. **No architecture change required.**

### FV-4 — `CartLanguage` enum in devtool (expected extension point)

`devtool/src/build.rs` has `CartLanguage { C, Cpp, Lua, Rust }`. The spike's
manual link recipe **exactly replicates** `link_cart()`; the only new code would
be a `compile_swift()` function analogous to `compile_c()`. Everything from the
`.o` files onward is shared. **Production path: ~50 lines in build.rs.** No
architecture change required.

### FV-5 — `posix_memalign`, `__stack_chk_*`, `__lshrdi3` not in libblyt32.so (leaked assumption)

Embedded Swift's allocator uses `posix_memalign` for over-aligned heap
allocation. The Swift compiler inserts stack-protection canary references
(`__stack_chk_guard`, `__stack_chk_fail`) in generated code. 64-bit right
shifts on RV32 (`UInt64 >> N`) emit `__lshrdi3` (a compiler_rt helper).

None of these are currently in libblyt32.so's `.dynsym` export set, even though
musl implements all three. The spike provides `swift_stubs.c` as a workaround.

**Fix for production:** Add three entries to the musl export list in
`libblyt32.so`'s build: `posix_memalign`, `__stack_chk_guard`,
`__stack_chk_fail`. For `__lshrdi3`: add to export list OR ship a
`libswift32.so` sidecar with compiler_rt support (analogous to `libc++` for
C++ carts). The sidecar approach is cleaner if more compiler_rt symbols emerge.
**This is the one real gap that needs an architectural decision.**

### FV-6 — `.swift_modhash` not in `cart_load.c` section allowlist (leaked assumption)

Swift Embedded emits a `.swift_modhash` section (module ABI hash for
link-time consistency checks). `cart_load.c`'s `KNOWN_SECTIONS_EXACT[]`
(ADR-0112) rejects it as unknown. **Fix:** one line in `cart_load.c`:
```c
".swift_modhash",   /* Swift module hash — Embedded Swift linker artifact */
```
Or add `".swift"` to `KNOWN_SECTIONS_PREFIX[]` for all future Swift sections.
The spike strips the section with `--remove-section=.swift_modhash` in the
finalise step.

### FV-7 — State buffer API is schema-gated (expected extension point)

`blyt_buffer_get/set_u32` with hardcoded handles returns 0 and is a no-op
because no state schema is registered — the buffer API requires a
packer-generated `.cart.config` FlatBuffer section. This is an expected
integration point with the `blyt build` packer, which is already language-agnostic.
**No architecture change required.**

---

## Stage 5: Footprint

| Cart | text (B) | data (B) | bss (B) | total (B) |
|------|----------|----------|---------|-----------|
| cart.blyt (Stage 1 minimal) | 3384 | 220 | 217 | 3821 |
| probe_cart.blyt (Stage 4 with ARC) | 3639 | 220 | 4064 | 7923 |
| hello-c.blyt (reference) | 2342 | 176 | 1636 | 4154 |

The minimal Swift cart is ~1.6× the size of hello-c in text. This is expected:
Embedded Swift includes ARC runtime, stack protection, and allocator bootstrap
code that C doesn't need. With `--gc-sections` and `-Osize`, unused code is
eliminated but the Swift runtime glue is always present.

Sections in the final `.blyt`: all standard (`.interp`, `.dynsym`, `.gnu.hash`,
`.hash`, `.dynstr`, `.rela.dyn`, `.rela.plt`, `.rodata`, `.text`, `.plt`,
`.dynamic`, `.got`, `.got.plt`, `.relro_padding`, `.data`, `.bss`, `.cart.info`).
`.swift_modhash` requires removal; `.riscv.attributes` is removed as for all
other carts. **No size limits or section-count assumptions were tripped.**

---

## Stage 6: Ergonomics audit

### What Embedded Swift provides
- `@_cdecl`: C name mangling → works for all three entry points ✓
- `-import-bridging-header`: C header import → `blyt.h` imports cleanly ✓
- Structs, enums, tuples: fully available ✓
- Classes with ARC: available; ARC lowers to native `amoadd.w.aqrl` on RV32A ✓
- Generics: available (static dispatch, no existentials) ✓
- `posix_memalign` / `malloc` / `free`: via stubs → heap allocation works ✓
- Integer types (`Int32`, `UInt32`, `Float`, `Double`): fully available ✓
- 64-bit arithmetic (`UInt64`): available; emits `__lshrdi3` compiler_rt call ✓

### What Embedded Swift removes
- No `String` (Swift stdlib heap-allocated string)
- No `Array<T>` in the general case (short constant arrays of value types work)
- No `Dictionary`
- No existentials (`any Protocol`, `some Protocol`)
- No reflection (`Mirror`)
- No dynamic dispatch via existentials (protocol conformances are static)
- No ABI stability (each module is an island)

### Cart API shape assessment
The blyt cart API (`blyt.h`) does not implicitly require any capability that
Embedded Swift lacks:
- All functions take and return scalar types (Int32, UInt32, Float, Double,
  pointer to CChar) — all available in Embedded Swift
- The `blyt_console_debug(const char *)` function accepts a `UnsafePointer<CChar>`
  in Swift — works with `assumingMemoryBound(to: CChar.self)` from a raw buffer
- No stdlib String required; carts can use tuple-based byte buffers for output

**The cart API shape is compatible with Embedded Swift's capabilities.** The
only ergonomic friction is string formatting (no `String.init(format:)`) — carts
must format output using C-style byte manipulation or C `sprintf` via bridging.
This is the same constraint as C carts without `<stdio.h>` snprintf.

---

## Conclusion

The blyt architecture can onboard Embedded Swift as a cart language with **two
small concrete fixes** and one architectural decision:

1. **`cart_load.c`** (FV-6): one line — add `".swift_modhash"` to
   `KNOWN_SECTIONS_EXACT[]`.

2. **`libblyt32.so` symbol set** (FV-5): add `posix_memalign`,
   `__stack_chk_guard`, `__stack_chk_fail` to the musl export list —
   **OR** — ship a `libswift32.so` sidecar with the full compiler_rt subset
   Embedded Swift needs (recommended if more compiler_rt symbols emerge as more
   Swift features are used).

3. **`devtool/src/build.rs`** (FV-4, expected): add `CartLanguage::Swift`,
   `compile_swift()`, and update the invocation path (~50 lines).

Everything else (ELF format, link flags, `PT_INTERP`, RELRO/BIND_NOW, `.cart.info`,
the state buffer API, the lifecycle entry points) works as-is. **The architectural
seams are language-agnostic.**
