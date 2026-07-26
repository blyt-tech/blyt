# Spike Y — measured results (Pi Zero 2 W)

Full analysis and the strategic conclusion live in the planning repo:
`blyt-planning/docs/design/spike-y-lua-per-pixel.md`. This file is the raw
number record for the harness in this directory.

> **Repeat, 2026-07-26 — see [REPEAT-RESULTS.md](REPEAT-RESULTS.md).** The
> *shipped* host-Lua fast path was measured on the same Pi and matches the
> native proxy below to within ~1 % — but only against the **baseline** VM. The
> "Native" table in this file was measured with the `spike/208-lua-vm-fastpixel`
> VM patches, which were never shipped; the shipped VM is 2.7× slower per pixel
> (`lk:set` 1 k = 0.887 ms, not 0.333 ms), so the "PICO-8-class with the shipped
> `lk:set`" takeaway below does not hold for the shipped runtime. The repeat
> reproduced both this file's emulated numbers (within 1.5 %) and its patched
> native numbers (exactly).

Device: Raspberry Pi Zero 2 W (Cortex-A53 @ 1 GHz, arm64 userland).
60 Hz frame budget = 16.67 ms. "px in budget" = pixels/frame that fit a full frame.

## Emulated (rv32emu blytplay, core `-O2`≡`-O3` + guest libs `-O3`)

ms/frame (lower is better):

| strategy | 1 k | 4 k | 10 k | ~px in 16.67 ms | VM patch |
|---|---:|---:|---:|---:|---|
| method `lk:set(x,y,c)` (shipped 2a) | 57.6 | 228 | 552 | ~266 | none |
| plain fn `pset(lk,x,y,c)` | 50.8 | — | — | ~300 | none |
| implicit `set_pixel(x,y,c)` | 25.3 | 91.7 | — | ~640 | none |
| method + fast `OP_SELF` + `OP_CALL`-inline | 19.4 | 70.9 | — | ~860 | 2 tiny |
| implicit `set_pixel` **inline** | 14.1 | 52.1 | — | ~1 150 | 1 |
| index `lk[y*w+x]` (realistic) | 13.8 | 46.2 | 90 | ~1 300 | table op |
| index `lk[row+x]` (row hoisted) | 12.0 | 39.4 | — | ~1 400 | table op |
| C tier-2 ceiling (raw ptr) | — | 2.9 | — | ~170 000 | n/a |

## Native (**patched** Lua VM compiled aarch64 — no emulation)

**Not the shipped VM** — these need the `spike/208-lua-vm-fastpixel` patches.
Baseline (shipped) VM on the same device: `lk:set` 0.887 / 3.540 / 8.640 ms,
`set_pixel` 0.431 / 1.697 / 4.128, index form unsupported. See REPEAT-RESULTS.md.

ms/frame:

| strategy | 1 k | 4 k | µs/px | ~px in 16.67 ms |
|---|---:|---:|---:|---:|
| method `lk:set` | 0.333 | 1.313 | ~0.32 | ~52 000 |
| `set_pixel` | 0.296 | 1.163 | ~0.28 | ~59 000 |
| index `lk[y*w+x]` | 0.270 | 1.058 | ~0.26 | ~65 000 |

## The gap

Native is **~44–58× faster than emulated on the same device** (method 1 k:
0.333 ms native vs 19.4 ms emulated) — well above Spike A's ~15× CoreMark tax,
because the Lua VM (branch-heavy, pointer-chasing, function dispatch) is
rv32emu's interpreter worst case.

## Takeaways

- The order-of-magnitude lever is **running Lua native** (host-Lua fast path),
  not the per-pixel VM patches. Native reaches PICO-8-class full-screen
  per-pixel with the *shipped* `lk:set`; the VM fast paths add only ~22 %.
- The VM patches matter only on the **emulated hybrid-on-handheld** case, where
  they lift the floor from ~266 to ~1.2–1.4 k px/frame — clearing the low end of
  the 1 k–10 k use-case bar but never bulk (4 k–10 k) fills.
- Bulk per-pixel work belongs in **C tier-2** (`blyt_raster_*`, ~0.7 µs/px
  emulated), which clears the whole bar by ~130×.
