# Spike Y repeat — does the SHIPPED host-Lua path deliver native-Lua per-pixel?

**Date:** 2026-07-26 · **Device:** Raspberry Pi Zero 2 W Rev 1.0 (Cortex-A53
@ 1 GHz, 4 cores, 415 MB, Debian 13 trixie, kernel 6.18.34, glibc 2.41,
governor `ondemand` pinned at 1 000 000 kHz throughout, `get_throttled=0x0`,
41 → 48 °C across the session) · **Tree:** `bench-spike-y-repeat` cut from
`origin/host-lua` @ `f1a93e7` (ADR-0136 end state: host-Lua is the *default*
for pure-Lua carts on non-RISC-V hosts; `BLYT_HOSTLUA` deleted).

Spike Y measured a **proxy** for host-Lua (`runner.c`: native Lua + a hand-bound
surface API) because the real path did not exist yet. It exists now. This repeat
answers: **does the shipped host-Lua fast path actually deliver native-Lua
per-pixel throughput on the floor device?**

## Verdict

**Yes — the shipped host-Lua pixel path is functionally complete.** It runs the
per-pixel workload at **native-Lua speed within ~1 %** of a hand-bound native
proxy built from the same VM, and at **~62× the emulated floor** measured on the
same device in the same session (Spike Y claimed 44–58×). Per-pixel output is
**bit-identical to the emulated leg** — every cart's `[blyt:fbhash]`/`[blyt:palhash]`
stream matches frame for frame.

Two qualifications, both about Spike Y's *record*, not about host-Lua:

1. **Spike Y's "Native" table is the PATCHED Lua VM, not the shipped one.** Those
   numbers (`lk:set` 1 k = 0.333 ms → ~52 k px/frame, "PICO-8-class") are only
   reachable with the `spike/208-lua-vm-fastpixel` VM patches, which were **never
   shipped** (see [Reproducibility risk](#reproducibility-risk)). Rebuilt here on
   the same device they reproduce to 3 decimal places — and the *unpatched* VM,
   which is what blyt ships, is **2.7× slower per pixel**. The README's framing of
   the native table as "the *host-Lua fast path* speed" is therefore wrong for the
   shipped runtime, and RESULTS.md's takeaway ("Native reaches PICO-8-class
   full-screen per-pixel with the *shipped* `lk:set`") does not hold.
2. **The shipped runtime reaches ~18 k px/frame, not ~52 k.** That still clears
   the Spike Y success bar (1 k–10 k px/frame at ≤ 50 % of the 16.67 ms budget)
   across essentially the whole range — see [Against the bar](#against-the-bar).

A follow-up then measured the method-dispatch patch **through the real runtime**
(not the proxy) — it delivers 2.3× and reaches 53.6 k px/frame, but the striking
result is that **~2.05× of that is available with no VM patch at all**, purely by
offering an implicit `set_pixel(x,y,c)` form. See
[the #208 follow-up](#follow-up-the-208-method-dispatch-fast-path-measured-through-the-real-runtime).

## The three legs

All three ran on the same Pi in one session. Player legs are timed with a
two-point slope (`t(N₂) − t(N₁)` over `N₂ − N₁` frames, ~4 s per point, 3 rounds,
minimum reported) so cart load + VM boot are excluded; `blytplay --headless`
free-runs (it only paces to 60 Hz with a window or a dev-control channel
attached). `runner.c` times its own `update()+draw()` loop over 2000 frames.

Workloads plot a W×H region per frame with `c = (x+y+t) & 0xFF` — 1 k = 32²
= 1024 px, 4 k = 64² = 4096 px, 10 k = 100² = 10 000 px.

### Leg 1 — SHIPPED host-Lua (`blytplay` from this tree, aarch64)

ms/frame (lower is better):

| strategy | floor | 1 k | 4 k | 10 k | µs/px | px @ 16.67 ms | px @ 8.33 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| method `lk:set(x,y,c)` (shipped 2a) | 0.558 | 1.413 | 4.125 | 9.252 | **0.873** | ~18 400 | ~8 900 |
| plain call `set(lk,x,y,c)` (hoisted) | 0.558 | 1.317 | 3.625 | 7.984 | **0.743** | ~21 700 | ~10 500 |

`floor` = a cart that acquires the screen, plots **one** pixel and releases: the
runtime's fixed per-frame cost (frame lifecycle + paletted → XRGB8888 convert),
0.558 ms — 3.3 % of a 60 Hz frame. `µs/px` is the 1 k→10 k slope, which cancels
that intercept; `px @ …` is `(budget − floor) / slope`.

Repeatability: a second full run of leg 1 gave 1.420 / 4.133 / 8.973 (method) and
1.307 / 3.626 / 8.052 (pset) — within 3 %.

Two Spike Y strategies **do not exist in the shipped API** and could not be run:
`blyt32.surface.set_pixel` and `blyt32.surface.pset` are `nil`, and index
assignment `lk[i] = c` raises *"attempt to index a blyt.surface.lock value"* (the
lock metatable has an `__index` method table but no `__newindex`; index *reads*
return nil). Both were Spike Y hypotheticals that needed the VM patches and/or an
API addition. The shipped tier-2 surface is `acquire` → `get`/`set`/`clear`/
`rect_fill`/`line`/`release`.

### Leg 2 — `runner.c` native proxy (aarch64, same device)

ms/frame, and the two VM builds side by side:

| strategy | VM | 1 k | 4 k | 10 k | µs/px |
|---|---|---:|---:|---:|---:|
| method `lk:set` | baseline (**= shipped VM**) | 0.887 | 3.540 | 8.640 | **0.864** |
| method `lk:set` | patched (`spike/208-…`) | 0.333 | 1.313 | 3.192 | 0.319 |
| plain fn `pset(lk,…)` | baseline | 0.745 | 2.992 | 7.410 | 0.743 |
| plain fn `pset(lk,…)` | patched | 0.331 | 1.313 | 3.161 | 0.315 |
| implicit `set_pixel` | baseline | 0.431 | 1.697 | 4.128 | 0.412 |
| implicit `set_pixel` | patched | 0.296 | 1.146 | 2.826 | 0.282 |
| index `lk[y*w+x]` | baseline | *unsupported* | | | |
| index `lk[y*w+x]` | patched | 0.270 | 1.058 | 2.580 | 0.257 |

The patched column **reproduces Spike Y's recorded native table exactly**
(recorded: method 0.333 / 1.313, `set_pixel` 0.296 / 1.163, index 0.270 / 1.058)
— the device and harness are in the same state as when Spike Y ran. The baseline
column is the same fork with the fast paths compiled out; two different fork
revisions agree (p4 `0.887` vs the spike fork `0.891`), so the 2.7× is the
**patches**, not the fork version.

### Leg 3 — emulated floor (rv32emu `blytplay`, sibling `bench-spike-y` worktree)

Built from `/Users/tom/code/blyt-worktrees/bench-spike-y` (main-based, `#213`
tier-2 lock, still has the emulated pure-Lua path — `host-lua` retired it), core
pinned `-O2 -fno-strict-aliasing`, guest libs from that tree's `sdk/lib`.

ms/frame:

| strategy | floor | 1 k | 4 k | 10 k | µs/px | px @ 16.67 ms | Spike Y recorded |
|---|---:|---:|---:|---:|---:|---:|---|
| method `lk:set` | 0.840 | 56.578 | 223.289 | 544.604 | 54.4 | ~291 | 57.6 / 228 / 552 |
| plain fn `pset` | 0.840 | 47.821 | 186.939 | 455.647 | 45.4 | ~348 | 50.8 / — / — |

**Reproduced Spike Y's emulated numbers within 1.5 %** — confirming the `-O0`
trap was avoided (see [Method](#method)) and that the two sessions are comparable.

## The comparisons that matter

**Host-Lua vs the native proxy (same VM) — the completeness question:**

| strategy | leg 1 shipped host-Lua | leg 2 proxy, same VM | delta |
|---|---:|---:|---:|
| method `lk:set` | 0.873 µs/px | 0.864 µs/px | **+1.1 %** |
| plain call | 0.743 µs/px | 0.743 µs/px | **±0 %** |

The shipped path costs nothing measurable over hand-written native Lua bindings.
`l_lock_set` in `cart_run_hostlua.c` does the same work as the proxy's `l_set`
(`lua_getextraspace` for the runtime handle, `luaL_checkudata`, three
`luaL_checkinteger`, bounds + epoch check, one byte store) — and measures like it.

**Host-Lua vs the emulated floor:**

| | host-Lua | emulated | ratio |
|---|---:|---:|---:|
| method, 1 k (gross ms/frame) | 1.413 | 56.578 | **40.0×** |
| method, 10 k (gross ms/frame) | 9.252 | 544.604 | **58.9×** |
| method (µs/px, floor removed) | 0.873 | 54.4 | **62.3×** |
| plain call (µs/px) | 0.743 | 45.4 | **61.2×** |

At or above Spike Y's 44–58× claim.

**Cost of not shipping the VM patches:** 2.71× per pixel (0.864 → 0.319 µs/px for
`lk:set`), i.e. ~52 k px/frame vs ~18 k.

## Against the bar

Spike Y's success bar: a realistic **1 k–10 k px/frame at 60 Hz within ≤ ~50 %**
of the 16.67 ms budget (8.33 ms). Shipped host-Lua, `lk:set`:

| workload | ms/frame | % of 16.67 ms budget | verdict |
|---|---:|---:|---|
| 1 k px | 1.413 | 8.5 % | ✅ |
| 4 k px | 4.125 | 24.7 % | ✅ |
| 10 k px | 9.252 | 55.5 % | ⚠️ just over the ≤50 % bar |
| 10 k px, hoisted plain call | 7.984 | 47.9 % | ✅ |

The half-budget ceiling is **~8 900 px/frame** with `lk:set` and **~10 500** with
the hoisted plain call — so the shipped API clears the stated use-case bar across
effectively the whole 1 k–10 k range (the very top end needs the one-line
`local set = lk.set` hoist). For comparison, the emulated floor manages ~138
px/frame at the same bar. Bulk per-pixel work still belongs in C tier-2 /
the in-lock primitives (`lk:clear`, `lk:rect_fill`, `lk:line`), exactly as
Spike Y concluded.

## Correctness

`BLYT_FRAME_HASH=1`, 8 frames, every workload, shipped host-Lua vs emulated on
the Pi — all `[blyt:fbhash]` **and** `[blyt:palhash]` lines identical:

```
floor.blyt:      MATCH (16 hash lines, 784b30f72a0ae324 …)
method-10k.blyt: MATCH (16 hash lines, 4724d4062c09781d …)
method-1k.blyt:  MATCH (16 hash lines, f2ae8a64c1867325 …)
method-4k.blyt:  MATCH (16 hash lines, 1532a8567e044125 …)
pset-10k.blyt:   MATCH (16 hash lines, 4724d4062c09781d …)
pset-1k.blyt:    MATCH (16 hash lines, f2ae8a64c1867325 …)
pset-4k.blyt:    MATCH (16 hash lines, 1532a8567e044125 …)
```

`method-Nk` and `pset-Nk` hash identically to each other (same pixels, different
call form), and the host-Lua hashes are also identical to the same carts run
under macOS/arm64 `blytplay` — the determinism contract holds across host arch
*and* execution model. The workload is self-checking in the strong sense: a
dropped or misplaced write changes the hash.

## Follow-up: the #208 method-dispatch fast path, measured through the real runtime

The tables above use the **proxy** to price the unshipped VM patches. This
section measures them **in `blytplay`** — the patched VM wired into the actual
host-Lua runtime — so the 2.7× proxy projection can be checked against reality.
Scope per the request: **method dispatch only**. The patch's other two hunks
(`OP_GETTABLE`/`OP_SETTABLE`, i.e. `lk[i] = c`) are deliberately left out — that
form is a cart-visible API change, not a dispatch optimisation.

Three build configurations, all pixel-verified against each other (below):

| | Lua VM | `blyt32.surface.set_pixel` |
|---|---|---|
| **A — shipped** | stock p4 fork | absent |
| **B — bench** | stock p4 fork | added (`BLYT_HOSTLUA_PIXEL_BENCH`) |
| **D — fastpixel** | p4 + the 2 method-dispatch hunks | added |

µs/px on the Pi (1 k→10 k slope; each config's own frame floor removed):

| form | B: stock VM | D: patched VM | patch buys |
|---|---:|---:|---:|
| `lk:set(x,y,c)` — the shipped form | 0.826 | **0.360** | 2.30× |
| `set(lk,x,y,c)` — hoisted plain call | 0.740 | **0.334** | 2.21× |
| `set_pixel(x,y,c)` — implicit lock | **0.404** | **0.301** | 1.34× |

Raw ms/frame (floor / 1 k / 4 k / 10 k):

| config · form | floor | 1 k | 4 k | 10 k | px @ 16.67 ms | px @ 8.33 ms |
|---|---:|---:|---:|---:|---:|---:|
| A shipped · `lk:set` | 0.558 | 1.413 | 4.125 | 9.252 | ~18 400 | ~8 900 |
| B · `lk:set` | 0.564 | 1.412 | 3.956 | 8.830 | ~19 500 | ~9 400 |
| B · `set_pixel` | 0.564 | 0.983 | 2.232 | 4.610 | **~39 900** | ~19 200 |
| D · `lk:set` | 0.557 | 0.929 | 2.037 | 4.160 | **~44 800** | ~21 600 |
| D · `set_pixel` | 0.557 | 0.870 | 1.800 | 3.568 | **~53 600** | ~25 900 |

### What this says

- **The proxy was a good but slightly optimistic predictor.** It called
  patched `lk:set` at 0.319 µs/px; through the real runtime it is 0.360 (11 %
  worse). Baseline `set_pixel` it called at 0.412 vs 0.404 measured (2 %).
- **PICO-8-class is reachable through the shipped runtime** — 53.6 k px/frame,
  vs Spike Y's ~52 k proxy projection. The full 10 k-px workload costs 3.57 ms,
  21 % of the frame.
- **The biggest single win needs no VM patch at all.** Simply offering the
  implicit `set_pixel(x,y,c)` form on the *stock* VM is **2.05×** over the
  shipped `lk:set` (0.826 → 0.404 µs/px) — that is ~75 % of the total available
  win, from a binding, with the Lua VM untouched. The VM patch then adds a
  further 1.34×. Ranked by payoff-per-risk the order is clear: **API shape
  first, VM patches second.**
- Why: `lk:set` pays an `OP_SELF` metatable `__index` lookup, a `luaL_checkudata`
  and an extra stack slot per pixel; the implicit form pays none of them. The
  VM patch removes what is left (the C call frame and the `luaL_check*`
  revalidation).

### Correctness

Every workload, 8 frames, `BLYT_FRAME_HASH=1`, on both macOS arm64 and the Pi —
all three configurations produce **identical** `[blyt:fbhash]` streams, and the
`set_pixel` carts hash identically to the `lk:set` and hoisted-call carts:

```
floor        shipped=1b9b8a0a  stockVM+bind=1b9b8a0a  patchedVM=1b9b8a0a
method-1k    shipped=0556137f  stockVM+bind=0556137f  patchedVM=0556137f
method-4k    shipped=0b2bbb7b  stockVM+bind=0b2bbb7b  patchedVM=0b2bbb7b
method-10k   shipped=6d0e916a  stockVM+bind=6d0e916a  patchedVM=6d0e916a
pset-*       (identical to the method-* row of the same size)
setpixel-1k  shipped=absent    stockVM+bind=0556137f  patchedVM=0556137f
setpixel-4k  shipped=absent    stockVM+bind=0b2bbb7b  patchedVM=0b2bbb7b
setpixel-10k shipped=absent    stockVM+bind=6d0e916a  patchedVM=6d0e916a
```

The fast path is genuinely engaged (2.3× is not a fall-through) *and* pixel-exact.

### What productionizing this would cost — three real hazards found

Wiring the patch into the runtime was not mechanical. Each of these is a
concrete cost to weigh, not a hypothetical:

1. **The patch reads the lock through a hardcoded struct copy.** `lvm.c` casts
   the userdata to its own `struct blyt_fp_lock { pixels; stride, w, h; token,
   epoch; released; }`. That matches the *guest* binding's `lua_lock_t`, but
   host-Lua's `hl_lock_t` had diverged (no `stride`, an extra `handle` before
   `token`, `bool released`) — so the patch would have read `h` where it wanted
   `w`: **silently wrong pixels, not a link error.** Fixed by re-laying-out
   `hl_lock_t` with the prefix pinned by `_Static_assert`s. Any future field
   reorder in either place is a silent corruption; the layout becomes a
   cross-component ABI.
2. **The VM reaches the runtime through process globals** (`blyt_lua_lock_epoch`,
   `blyt_lua_lock_mt`, three function pointers). Fine while a process hosts one
   host-Lua VM, but it hard-codes that assumption into the VM — the same class of
   single-VM-per-process global state already noted for rv32emu.
3. **The fast path bypasses `hl->drawn`.** Storing pixels inline never reaches
   `l_lock_set`, so the "cart has drawn ⇒ retire the PM5544 test card" flag never
   flips: a cart drawing *only* through the fast path would show the test card
   forever. Worked around here by setting `drawn` when the screen is acquired —
   acceptable for a bench, but a real change needs a deliberate answer.

Also note the added `set_pixel` binding is **host-Lua-only** in these builds. As
a shipped API it would have to land on the guest (`blyt32lua.c`) and WASM
bindings in the same change, or a cart using it would work on desktop/browser
and fail on the emulated and bare-metal legs — the cross-leg divergence class
this project treats as blocking.

**None of this is shipped.** The runtime side is behind `BLYT_HOSTLUA_PIXEL_BENCH`
(off by default) and the VM side needs a patched fork; the default build is
byte-for-byte the shipped configuration, which the identical hashes above
confirm.

## Method

- **`-O0` trap avoided.** Both players were configured
  `-DCMAKE_C_FLAGS="-O2 -fno-strict-aliasing"`. The repo default (no
  `CMAKE_BUILD_TYPE`) builds the emulator core `-O0`, ~4× too slow. That the
  emulated leg landed within 1.5 % of Spike Y's record is the evidence it was
  pinned correctly in both sessions.
- **Cross-build, not on-device.** Everything is built on the Mac in a
  `linux/arm64 debian:trixie` container (`scripts/Dockerfile.arm64`) — the same
  Debian release the Pi runs, so glibc/SDL2 match exactly. The Pi needs no
  toolchain.
- **Host-Lua path proven, not assumed.** The host-Lua kit shipped to the Pi
  contains no guest libraries at all (`libblyt32lua.so` etc. are absent, and
  `BLYT_LIB_DIR` is unset). The emulated path cannot load a Lua cart without
  them, so the fact that the cart runs *is* the proof that execution went through
  the native host-Lua VM.
- **Timing.** Two-point slope removes startup; 3 rounds, minimum reported (median
  was within 1 % of minimum on every cart bar one). The floor cart plots **one**
  pixel deliberately: a cart that never draws keeps the PM5544 test card alive
  and the runtime re-renders it (76 800 px of C) every frame — an early
  no-pixel "noop" cart measured 5.46 ms/frame of test card, not the frame floor.

### Reproducibility risk

The VM fast-path patches behind Spike Y's headline native numbers exist **only as
an uncommitted working-tree diff** in `../208-lua-fast-pixel/third_party/lua`
(`lvm.c`, 4 `#ifdef BLYT_LUA_FASTPIXEL` regions). There is no
`spike/208-lua-vm-fastpixel` branch on `blyt-tech/lua` (remote has
`master`, `blyt-patches-v0`, `hostlua-heap-seam`, `spike-u-lua-i32f64`), so
README.md's reproduction recipe cannot be followed as written, and the patches
are one `git checkout` away from being lost.

Also: `scripts/build-native.sh` documents `FASTPIXEL=0` as "baseline VM", but it
passes `-DBLYT_LUA_FASTPIXEL=0` and the patch guards are `#ifdef` — so against a
*patched* fork, `FASTPIXEL=0` still compiles the fast paths in (the two builds
came out byte-identical). The genuine baseline here was obtained by not defining
the macro at all, and cross-checked against the unpatched p4 fork.

## Harness added by this repeat

```
scripts/make-carts.sh            wrap the workloads into .blyt cart projects (shipped API)
scripts/Dockerfile.arm64         linux/arm64 debian:trixie build image
scripts/build-aarch64-player.sh  cross-build an aarch64 blytplay from any worktree
scripts/bench-player.sh          on-device two-point per-frame timing (runs on the Pi)
scripts/run-player.sh            ship player + carts to a target and time them
scripts/fbhash-check.sh          cross-leg [blyt:fbhash] diff for every workload
```

Reproduce end to end:

```sh
cmake -B build -G Ninja -DCMAKE_C_FLAGS="-O2 -fno-strict-aliasing"
cmake --build build --target sdk
bench/spike-y/scripts/make-carts.sh
bench/spike-y/scripts/build-aarch64-player.sh
bench/spike-y/scripts/run-player.sh pizero "$PWD/bench/spike-y/dist/blytplay-aarch64" hostlua
bench/spike-y/scripts/fbhash-check.sh pizero 8
```
