# Spike Y — Lua per-pixel plotting throughput

Harness behind **Spike Y** (`#195` Stage 2 / `#208` spin-out): how fast can a
Lua cart plot individual pixels, and how much does each optimization strategy
buy? Measured on a Pi Zero 2 W, **emulated** (rv32emu) and **native** (Lua
compiled for the host CPU — what blyt's host-Lua fast path does).

The write-up, the analysis, and the decision are in the planning repo:
[`blyt-planning/docs/design/spike-y-lua-per-pixel.md`](../../../blyt-planning/docs/design/spike-y-lua-per-pixel.md).
The raw numbers are in [`RESULTS.md`](RESULTS.md); the 2026-07-26 repeat against
the **shipped** host-Lua fast path is in [`REPEAT-RESULTS.md`](REPEAT-RESULTS.md).

## TL;DR

Native Lua is **~44–58× faster than emulated on the same device** and reaches
PICO-8-class full-screen per-pixel (~52–65 k px/frame) with the *shipped*
`lk:set` — the VM fast paths barely move it there. On the emulated floor the
fastest Lua path reaches ~1.2–1.4 k px/frame (up from ~266 for `lk:set`). So the
order-of-magnitude lever is **running Lua native**, not the per-pixel VM patches;
bulk per-pixel work belongs in C tier-2 (`blyt_raster_*`).

## Layout

```
carts/           workload sources — plot a W×H region each frame
                 {method,pset,setpixel,index}-{1k,4k,10k}.lua
                   method   lk:set(x,y,c)        — the shipped 2a form (OP_SELF+OP_CALL)
                   pset     pset(lk,x,y,c)       — plain function, no OP_SELF
                   setpixel set_pixel(x,y,c)     — implicit lock, no explicit handle arg
                   index    lk[y*320+x]=c        — table-index form (OP_SETTABLE)
runner.c         native aarch64 bench host: binds a minimal blyt32.surface API to
                 native C writing a plain buffer, luaL_dofile's a cart, times draw()
scripts/
  build-native.sh  build native_bench (aarch64, in a debian:trixie container)
  run-native.sh    scp + time each cart on a target (e.g. pi@pizero.local)
  --- added by the 2026-07-26 repeat (shipped host-Lua leg) ---
  make-carts.sh            wrap the workloads into .blyt cart projects (shipped API)
  Dockerfile.arm64         linux/arm64 debian:trixie build image
  build-aarch64-player.sh  cross-build an aarch64 blytplay from any worktree
  bench-player.sh          on-device two-point per-frame timing (runs on the target)
  run-player.sh            ship player + carts to a target and time them
  fbhash-check.sh          cross-leg [blyt:fbhash] diff for every workload
RESULTS.md       measured numbers (Pi Zero 2 W, emulated + native)
REPEAT-RESULTS.md 2026-07-26 repeat: SHIPPED host-Lua vs proxy vs emulated
```

## Two legs, one set of workloads

The four `carts/*.lua` sources are the shared workload. They run through two
harnesses:

1. **Native** (`runner.c`) — Lua compiled aarch64, no rv32emu. A *proxy* for the
   host-Lua fast path (which did not exist when this spike ran). With
   `FASTPIXEL=0` **and an unpatched fork** it matches the shipped host-Lua path
   to ~1 % (REPEAT-RESULTS.md); with the patched fork it is 2.7× faster than
   anything blyt ships. Note `FASTPIXEL=0` does *not* disable the patches — the
   guards are `#ifdef`, so `-DBLYT_LUA_FASTPIXEL=0` still compiles them in;
   a true baseline needs the macro left undefined (or an unpatched fork).
   Build + run:
   ```sh
   scripts/build-native.sh              # → native_bench (FASTPIXEL=1, patched)
   FASTPIXEL=0 scripts/build-native.sh  # baseline VM, for the patched-vs-not delta
   scripts/run-native.sh pi@pizero.local 2000
   ```

2. **Emulated** (`blytplay`) — the same `.lua` wrapped into `.blyt` carts and run
   through the rv32 emulator. Reproduce from the main repo build (see below);
   this is the floor a hybrid cart hits on a handheld.

### VM fast-path patches

The `FASTPIXEL=1` variant needs the Lua-VM per-pixel fast paths (fast `OP_SELF`,
`OP_CALL`-inline for `set_pixel`, `OP_GETTABLE`/`OP_SETTABLE` index path), which
live on branch **`spike/208-lua-vm-fastpixel`** as patches to the `blyt-tech/lua`
fork (`third_party/lua`, branch `blyt-patches-v0`). To reproduce the patched
numbers:

```sh
git clone https://github.com/blyt-tech/lua third_party/lua
git -C third_party/lua checkout blyt-patches-v0
git -C third_party/lua cherry-pick <the lvm.c fast-path commits from spike/208-lua-vm-fastpixel>
```

The `FASTPIXEL=0` baseline needs only the plain fork.

## Reproducing the emulated leg

From the main repo build tree, with the fork wired in as
`-DFETCHCONTENT_SOURCE_DIR_LUA` (uppercase — the lowercase `_lua` is ignored):

```sh
# emulator + guest libs at -O2 (NOT the repo default -O0; -O3 needs -fno-strict-aliasing)
cmake -B build -G Ninja \
  -DCMAKE_C_FLAGS="-O2 -fno-strict-aliasing" \
  -DFETCHCONTENT_SOURCE_DIR_LUA="$PWD/third_party/lua" \
  -DBLYT_LUA_FASTPIXEL=ON            # OFF for baseline VM
cmake --build build --target sdk
# cross-build an aarch64 blytplay for the Pi (debian:trixie container), wrap the
# carts/*.lua into .blyt, scp the kit + carts, run blytplay --headless per cart.
```

> **Gotcha (secondary finding).** The repo configures at `-O0` by default (no
> `CMAKE_BUILD_TYPE`), which makes the emulator ~4× slower than a release build —
> the first pass of this spike measured the `-O0` floor by mistake. Always pin
> `-O2 -fno-strict-aliasing` for any perf measurement. `-O3` miscompiles rv32emu
> (PC=0) *unless* `-fno-strict-aliasing` is set; with it, `-O3 ≡ -O2`.
