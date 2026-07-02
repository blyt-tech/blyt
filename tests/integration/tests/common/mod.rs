//! Shared test fixtures for blyt integration tests.
#![allow(dead_code)]

use std::fs;
use std::path::PathBuf;

// -------------------------------------------------------------------------
// Path helpers
// -------------------------------------------------------------------------

pub fn repo_root() -> PathBuf {
    // CARGO_MANIFEST_DIR = tests/integration/
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent() // tests/
        .unwrap()
        .parent() // repo root
        .unwrap()
        .to_path_buf()
}

/// CMake build directory.  Overridable via BLYT_BUILD_DIR for CI environments
/// where the build tree lives outside the repo (e.g. the QEMU gate job).
pub fn build_dir() -> PathBuf {
    std::env::var("BLYT_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root().join("build"))
}

pub fn sdk_dir() -> PathBuf {
    build_dir().join("sdk")
}

/// The release player — uses the SDK bin/ copy so BLYT_LIB_DIR auto-inference
/// from the binary path resolves to sdk/lib/ (the release guest libs).
pub fn blytplay() -> PathBuf {
    sdk_dir().join("bin/blytplay")
}

/// The debug player (ADR-0129): GDB/DAP on, loads the debug guest libs.
/// Uses the SDK bin/ copy so BLYT_LIB_DIR auto-inference resolves to
/// sdk/lib/debug/ when --debug or --gdb flags are present.
pub fn blytdebug() -> PathBuf {
    sdk_dir().join("bin/blytdebug")
}

/// Path to the `blyt` devtool binary — always the SDK copy.
pub fn blyt_bin() -> PathBuf {
    sdk_dir().join("bin/blyt")
}

// -------------------------------------------------------------------------
// Spike X graphics reference (issue #188)
//
// One op list + reference rasterizer drives BOTH the generated C carts (which
// call the runtime's integer rasterizer — over the ECALL boundary on the
// emulated legs, natively on the bare-metal leg) AND the reference golden.  The
// cart's emitted framebuffer hash must equal the reference's hash, so the test
// catches a wrong rasterizer rather than self-confirming a captured value, and
// is palette-independent (it operates on palette indices).  The reference
// mirrors runtime/shared/blyt_raster.c primitive-for-primitive.
//
// Shared here (rather than per-suite) so the emulated trio (gfx.rs) and the
// native QEMU gate (native_qemu.rs) hash against ONE golden and cannot drift.
// -------------------------------------------------------------------------
pub mod gfx {
    pub const FRAME_W: usize = 320;
    pub const FRAME_H: usize = 240;
    /// XRGB8888 byte length of one expanded frame.
    pub const FRAME_BYTES: usize = FRAME_W * FRAME_H * 4;

    #[derive(Clone, Copy)]
    pub enum Op {
        Clear(u8),
        Pixel(i32, i32, u8),
        Rect(i32, i32, i32, i32, u8),
        Line(i32, i32, i32, i32, u8),
    }

    /// Emit a Lua `draw()` body that issues `ops` via the `blyt32.gfx.*`
    /// primitives — the Lua-cart counterpart of [`c_draw_body`], drawing the
    /// identical frame so a Lua cart hashes to the same golden across the
    /// emulated-Lua legs (native/libretro) and the host-Lua fast path (wasm).
    pub fn lua_draw_body(ops: &[Op]) -> String {
        let mut s = String::new();
        for op in ops {
            match *op {
                Op::Clear(c) => s += &format!("  blyt32.gfx.clear({c})\n"),
                Op::Pixel(x, y, c) => s += &format!("  blyt32.gfx.pixel({x}, {y}, {c})\n"),
                Op::Rect(x, y, w, h, c) => {
                    s += &format!("  blyt32.gfx.rect_fill({x}, {y}, {w}, {h}, {c})\n")
                }
                Op::Line(x0, y0, x1, y1, c) => {
                    s += &format!("  blyt32.gfx.line({x0}, {y0}, {x1}, {y1}, {c})\n")
                }
            }
        }
        s
    }

    /// Emit a Lua draw body issuing `ops` via the tier-1 surface API
    /// (`blyt32.surface.*(dst, …)`) targeting `dst` (e.g.
    /// `"blyt32.surface.SCREEN"` or a handle from `blyt32.surface.create`). The
    /// Lua counterpart of [`c_surface_draw_body`]; Lua is tier-1 only (#205), and
    /// this must hash identically to the C/Rust legs.
    pub fn lua_surface_draw_body(ops: &[Op], dst: &str) -> String {
        let mut s = String::new();
        for op in ops {
            match *op {
                Op::Clear(c) => s += &format!("  blyt32.surface.clear({dst}, {c})\n"),
                Op::Pixel(x, y, c) => {
                    s += &format!("  blyt32.surface.pixel({dst}, {x}, {y}, {c})\n")
                }
                Op::Rect(x, y, w, h, c) => {
                    s += &format!("  blyt32.surface.rect_fill({dst}, {x}, {y}, {w}, {h}, {c})\n")
                }
                Op::Line(x0, y0, x1, y1, c) => {
                    s += &format!("  blyt32.surface.line({dst}, {x0}, {y0}, {x1}, {y1}, {c})\n")
                }
            }
        }
        s
    }

    /// Emit a C `blyt_cart_draw` body that issues `ops` via the tier-1 surface
    /// API (`blyt_surface_*(dst, …)`) targeting `dst` (e.g. `"BLYT_SCREEN"` or a
    /// created surface handle variable). Drawing the torture frame into
    /// `BLYT_SCREEN` this way must hash identically to [`c_draw_body`] — the
    /// gfx.* sugar and the surface API share one host-side rasterizer (#205).
    pub fn c_surface_draw_body(ops: &[Op], dst: &str) -> String {
        let mut s = String::new();
        for op in ops {
            match *op {
                Op::Clear(c) => s += &format!("  blyt_surface_clear({dst}, {c});\n"),
                Op::Pixel(x, y, c) => {
                    s += &format!("  blyt_surface_pixel({dst}, {x}, {y}, {c});\n")
                }
                Op::Rect(x, y, w, h, c) => {
                    s += &format!("  blyt_surface_rect_fill({dst}, {x}, {y}, {w}, {h}, {c});\n")
                }
                Op::Line(x0, y0, x1, y1, c) => {
                    s += &format!("  blyt_surface_line({dst}, {x0}, {y0}, {x1}, {y1}, {c});\n")
                }
            }
        }
        s
    }

    /// Emit a C draw body that issues `ops` under a tier-2 lock on `surface`:
    /// acquire, draw with the freestanding `blyt_raster_*` primitives on the
    /// locked buffer (guest-side, no ECALL), then release.  Drawing the same
    /// frame this way must hash identically to the tier-1 path — the tier-1 ≡
    /// tier-2 guarantee (#205), since both call the same rasterizer source.
    /// `lk` is the emitted `blyt_lock_t` variable name.
    pub fn c_lock_draw_body(ops: &[Op], surface: &str, lk: &str) -> String {
        let mut s = format!("  blyt_lock_t {lk};\n  blyt_surface_acquire({surface}, &{lk});\n");
        let b = format!("{lk}.pixels, {lk}.stride, {lk}.w, {lk}.h");
        for op in ops {
            match *op {
                Op::Clear(c) => s += &format!("  blyt_raster_clear({b}, {c});\n"),
                Op::Pixel(x, y, c) => s += &format!("  blyt_raster_pixel({b}, {x}, {y}, {c});\n"),
                Op::Rect(x, y, w, h, c) => {
                    s += &format!("  blyt_raster_rect_fill({b}, {x}, {y}, {w}, {h}, {c});\n")
                }
                Op::Line(x0, y0, x1, y1, c) => {
                    s += &format!("  blyt_raster_line({b}, {x0}, {y0}, {x1}, {y1}, {c});\n")
                }
            }
        }
        s += &format!("  blyt_surface_release(&{lk});\n");
        s
    }

    /// Emit a Lua draw body that issues `ops` under a tier-2 lock on `surface`
    /// (#208 Stage 2): acquire, draw with the lock userdata's methods (per-pixel
    /// `lk:set` plus the in-lock bulk `lk:clear`/`rect_fill`/`line`), then
    /// release.  The Lua counterpart of [`c_lock_draw_body`]; drawing the same
    /// frame this way must hash identically to the tier-1 / C-tier-2 goldens on
    /// every leg.  `surface` is a Lua expression (e.g. `"blyt32.surface.SCREEN"`
    /// or a handle variable); `lk` is the emitted lock variable name.
    ///
    /// `Pixel` ops emit `lk:set`, which is *checked* (out-of-bounds → no-op) —
    /// equivalent to the raster clip for the in-bounds pixels in
    /// [`torture_frame`], so the golden is unchanged.
    pub fn lua_lock_draw_body(ops: &[Op], surface: &str, lk: &str) -> String {
        let mut s = format!("  local {lk} = blyt32.surface.acquire({surface})\n");
        for op in ops {
            match *op {
                Op::Clear(c) => s += &format!("  {lk}:clear({c})\n"),
                Op::Pixel(x, y, c) => s += &format!("  {lk}:set({x}, {y}, {c})\n"),
                Op::Rect(x, y, w, h, c) => {
                    s += &format!("  {lk}:rect_fill({x}, {y}, {w}, {h}, {c})\n")
                }
                Op::Line(x0, y0, x1, y1, c) => {
                    s += &format!("  {lk}:line({x0}, {y0}, {x1}, {y1}, {c})\n")
                }
            }
        }
        s += &format!("  {lk}:release()\n");
        s
    }

    /// Emit a Rust `blyt_cart_draw` body issuing `ops` via the tier-1 surface API
    /// (`blyt::gfx`) on `dst` (e.g. `"blyt::gfx::SCREEN"`). The Rust-cart
    /// counterpart of [`c_surface_draw_body`]; drawing the torture frame into the
    /// screen this way must hash to the same golden as every other leg (#205).
    pub fn rust_surface_draw_body(ops: &[Op], dst: &str) -> String {
        let mut s = String::new();
        for op in ops {
            match *op {
                Op::Clear(c) => s += &format!("    {dst}.clear({c});\n"),
                Op::Pixel(x, y, c) => s += &format!("    {dst}.pixel({x}, {y}, {c});\n"),
                Op::Rect(x, y, w, h, c) => {
                    s += &format!("    {dst}.rect_fill({x}, {y}, {w}, {h}, {c});\n")
                }
                Op::Line(x0, y0, x1, y1, c) => {
                    s += &format!("    {dst}.line({x0}, {y0}, {x1}, {y1}, {c});\n")
                }
            }
        }
        s
    }

    /// Emit the C `blyt_cart_draw` body that issues `ops` via the gfx primitives.
    pub fn c_draw_body(ops: &[Op]) -> String {
        let mut s = String::new();
        for op in ops {
            match *op {
                Op::Clear(c) => s += &format!("  blyt_gfx_clear({c});\n"),
                Op::Pixel(x, y, c) => s += &format!("  blyt_gfx_pixel({x}, {y}, {c});\n"),
                Op::Rect(x, y, w, h, c) => {
                    s += &format!("  blyt_gfx_rect_fill({x}, {y}, {w}, {h}, {c});\n")
                }
                Op::Line(x0, y0, x1, y1, c) => {
                    s += &format!("  blyt_gfx_line({x0}, {y0}, {x1}, {y1}, {c});\n")
                }
            }
        }
        s
    }

    fn put(fb: &mut [u8], x: i32, y: i32, c: u8) {
        if x >= 0 && (x as usize) < FRAME_W && y >= 0 && (y as usize) < FRAME_H {
            fb[y as usize * FRAME_W + x as usize] = c;
        }
    }

    /// Render the op list to a paletted framebuffer, mirroring blyt_raster.c.
    pub fn render(ops: &[Op]) -> Vec<u8> {
        let mut fb = vec![0u8; FRAME_W * FRAME_H];
        for op in ops {
            match *op {
                Op::Clear(c) => fb.iter_mut().for_each(|p| *p = c),
                Op::Pixel(x, y, c) => put(&mut fb, x, y, c),
                Op::Rect(x, y, w, h, c) => {
                    if w > 0 && h > 0 {
                        let x0 = x.max(0) as i64;
                        let y0 = y.max(0) as i64;
                        let x1 = (x as i64 + w as i64).min(FRAME_W as i64);
                        let y1 = (y as i64 + h as i64).min(FRAME_H as i64);
                        for yy in y0..y1 {
                            for xx in x0..x1 {
                                put(&mut fb, xx as i32, yy as i32, c);
                            }
                        }
                    }
                }
                Op::Line(mut x0, mut y0, x1, y1, c) => {
                    let dx = (x1 - x0).abs();
                    let dy = (y1 - y0).abs();
                    let sx = if x0 < x1 { 1 } else { -1 };
                    let sy = if y0 < y1 { 1 } else { -1 };
                    let mut err = dx - dy;
                    loop {
                        put(&mut fb, x0, y0, c);
                        if x0 == x1 && y0 == y1 {
                            break;
                        }
                        let e2 = 2 * err;
                        if e2 > -dy {
                            err -= dy;
                            x0 += sx;
                        }
                        if e2 < dx {
                            err += dx;
                            y0 += sy;
                        }
                    }
                }
            }
        }
        fb
    }

    /// Render `ops` into a paletted buffer of arbitrary `w` x `h` — the
    /// off-screen-surface counterpart of [`render`] (which is the 320x240
    /// screen).  Mirrors blyt_raster.c with the surface's own bounds/stride.
    pub fn render_dims(ops: &[Op], w: usize, h: usize) -> Vec<u8> {
        let mut fb = vec![0u8; w * h];
        let put = |fb: &mut [u8], x: i32, y: i32, c: u8| {
            if x >= 0 && (x as usize) < w && y >= 0 && (y as usize) < h {
                fb[y as usize * w + x as usize] = c;
            }
        };
        for op in ops {
            match *op {
                Op::Clear(c) => fb.iter_mut().for_each(|p| *p = c),
                Op::Pixel(x, y, c) => put(&mut fb, x, y, c),
                Op::Rect(x, y, rw, rh, c) => {
                    if rw > 0 && rh > 0 {
                        let x0 = x.max(0) as i64;
                        let y0 = y.max(0) as i64;
                        let x1 = (x as i64 + rw as i64).min(w as i64);
                        let y1 = (y as i64 + rh as i64).min(h as i64);
                        for yy in y0..y1 {
                            for xx in x0..x1 {
                                put(&mut fb, xx as i32, yy as i32, c);
                            }
                        }
                    }
                }
                Op::Line(mut x0, mut y0, x1, y1, c) => {
                    let dx = (x1 - x0).abs();
                    let dy = (y1 - y0).abs();
                    let sx = if x0 < x1 { 1 } else { -1 };
                    let sy = if y0 < y1 { 1 } else { -1 };
                    let mut err = dx - dy;
                    loop {
                        put(&mut fb, x0, y0, c);
                        if x0 == x1 && y0 == y1 {
                            break;
                        }
                        let e2 = 2 * err;
                        if e2 > -dy {
                            err -= dy;
                            x0 += sx;
                        }
                        if e2 < dx {
                            err += dx;
                            y0 += sy;
                        }
                    }
                }
            }
        }
        fb
    }

    /// Copy `src` (sw x sh) into `dst` (dw x dh) with top-left at (x,y), clipped
    /// to dst — the reference for blyt_raster_blit / blyt_surface_blit.
    #[allow(clippy::too_many_arguments)]
    pub fn blit(
        dst: &mut [u8],
        dw: usize,
        dh: usize,
        src: &[u8],
        sw: usize,
        sh: usize,
        x: i32,
        y: i32,
    ) {
        for sy in 0..sh as i32 {
            for sx in 0..sw as i32 {
                let dx = x + sx;
                let dy = y + sy;
                if dx >= 0 && (dx as usize) < dw && dy >= 0 && (dy as usize) < dh {
                    dst[dy as usize * dw + dx as usize] = src[sy as usize * sw + sx as usize];
                }
            }
        }
    }

    /// FNV-1a 64, matching runtime/shared/blyt_frame_hash.c.
    pub fn fnv1a(bytes: &[u8]) -> u64 {
        let mut h = 0xcbf29ce484222325u64;
        for &b in bytes {
            h ^= b as u64;
            h = h.wrapping_mul(0x100000001b3);
        }
        h
    }

    /// The `[blyt:fbhash] <hex>` line the runtime emits for a paletted frame —
    /// the substring every leg's harness asserts on captured stdout.
    pub fn expected_hash_line(fb: &[u8]) -> String {
        format!("[blyt:fbhash] {:016x}", fnv1a(fb))
    }

    /// The `[blyt:palhash] <hex>` line — the palette-sensitive oracle (#199/#204).
    /// FNV-1a over the 256-entry palette's little-endian XRGB8888 bytes, matching
    /// the runtime's `blyt_frame_hash((uint8_t*)palette, 1024)`.  The index-only
    /// fbhash cannot distinguish two palettes; this pins the actual RGB content so
    /// fbhash + palhash together prove fully-expanded colour parity across legs.
    pub fn expected_palhash_line(palette: &[u32]) -> String {
        let bytes: Vec<u8> = palette.iter().flat_map(|c| c.to_le_bytes()).collect();
        format!("[blyt:palhash] {:016x}", fnv1a(&bytes))
    }

    /// A frame exercising every primitive plus edge cases the brief calls out:
    /// off-screen clipping, zero size, negative coords, and i32-overflowing extents.
    pub fn torture_frame() -> Vec<Op> {
        vec![
            Op::Clear(3),
            Op::Rect(10, 20, 40, 30, 5),    // wholly on-screen
            Op::Rect(-20, -10, 50, 40, 7),  // negative origin, top-left clip
            Op::Rect(300, 220, 40, 40, 9),  // bottom-right clip
            Op::Rect(400, 400, 10, 10, 11), // wholly off-screen -> no-op
            Op::Rect(50, 50, 0, 10, 13),    // zero width -> no-op
            Op::Rect(2_000_000_000, 0, 2_000_000_000, 10, 200), // x+w overflows i32 -> clipped no-op
            Op::Pixel(0, 0, 1),                                 // corner
            Op::Pixel(319, 239, 2),                             // opposite corner
            Op::Pixel(-5, 5, 4),                                // off-screen -> no-op
            Op::Pixel(320, 0, 6),                               // just off the right edge -> no-op
            Op::Line(0, 0, 319, 239, 8),                        // full diagonal
            Op::Line(319, 0, 0, 239, 10),                       // anti-diagonal
            Op::Line(-50, 120, 400, 120, 12),                   // horizontal, off-screen endpoints
            Op::Line(160, -30, 160, 300, 14),                   // vertical, off-screen endpoints
            Op::Line(5, 5, 5, 5, 15),                           // degenerate single point
        ]
    }

    /// The deterministic per-pixel pattern the acquire/present probe writes
    /// directly into the runtime-reserved framebuffer region.  A function of
    /// (x, y) chosen so every byte differs from its neighbours (catches stride /
    /// byte-order bugs in the present copy).  Mirrored exactly by the C loop in
    /// [`raw_present_c_draw`].
    pub fn raw_pattern_frame() -> Vec<u8> {
        let mut fb = vec![0u8; FRAME_W * FRAME_H];
        for y in 0..FRAME_H {
            for x in 0..FRAME_W {
                fb[y * FRAME_W + x] = (x as i32 * 31 + y as i32 * 17) as u8;
            }
        }
        fb
    }

    /// The C `blyt_cart_draw` body that acquires the raw framebuffer pointer,
    /// writes [`raw_pattern_frame`] straight into it, and presents.
    pub fn raw_present_c_draw() -> String {
        format!(
            "  unsigned char *fb = blyt_gfx_acquire();\n\
             \x20 for (int y = 0; y < {FRAME_H}; y++)\n\
             \x20   for (int x = 0; x < {FRAME_W}; x++)\n\
             \x20     fb[y * {FRAME_W} + x] = (unsigned char)(x * 31 + y * 17);\n\
             \x20 blyt_gfx_present();\n"
        )
    }
}

// -------------------------------------------------------------------------
// Cart project fixture builder
// -------------------------------------------------------------------------

/// Generate the game crate's Cargo.toml, including any Rust lib dependencies.
/// `blyt build` injects the actual source paths via --config at build time.
fn rust_game_cargo_toml(rust_lib_names: &[String]) -> String {
    let mut s = "\
[package]\n\
name = \"cart\"\n\
version = \"0.1.0\"\n\
edition = \"2021\"\n\
publish = false\n\
\n\
[lib]\n\
crate-type = [\"staticlib\"]\n\
\n\
[dependencies]\n\
blyt = \"0.1\"\n"
        .to_string();
    for name in rust_lib_names {
        s.push_str(&format!("{name} = \"0.1\"\n"));
    }
    s
}

/// Generate a minimal Cargo.toml for a Rust library under src/lib/<name>/.
fn rust_lib_cargo_toml(name: &str) -> String {
    format!(
        "[package]\nname = \"{name}\"\nversion = \"0.1.0\"\nedition = \"2021\"\npublish = false\n\n[lib]\n"
    )
}

/// Builder for an on-disk cart project used in integration tests.
///
/// Call `.c(source)` and/or `.rust(lib_rs)` to declare languages, then
/// `.write(dir)` to materialise the project layout under `dir`.  The
/// `blyt.build.yaml` is generated automatically from the declared languages.
///
/// ```
/// CartProject::new().c(r#"..."#).write(&project_dir);
/// CartProject::new().rust(r#"..."#).write(&project_dir);
/// CartProject::new().lua(r#"..."#).write(&project_dir);
/// ```
pub struct CartProject {
    /// (filename, source) pairs for src/game/c/
    c_files: Vec<(String, String)>,
    /// (filename, source) pairs for src/game/c++/
    cpp_files: Vec<(String, String)>,
    /// Contents of src/game/rust/src/lib.rs
    rust_lib_rs: Option<String>,
    /// (filename, source) pairs for src/game/lua/
    lua_files: Vec<(String, String)>,
    /// (lib_name, relative_path_within_lib, content) — files for src/lib/<name>/
    lib_files: Vec<(String, String, String)>,
    /// Names of Rust libs declared via rust_lib(); used to generate the game Cargo.toml.
    rust_lib_names: Vec<String>,
    /// Contents of blyt.config.yaml (state buffers, fps, etc.)
    config_yaml: Option<String>,
    /// (relative_path_within_assets, bytes) pairs for assets/ (issue #91/#162).
    asset_files: Vec<(String, Vec<u8>)>,
    /// Resource names declared `persistent_resources` in blyt.build.yaml (#160).
    persistent_resources: Vec<String>,
}

impl CartProject {
    pub fn new() -> Self {
        CartProject {
            c_files: Vec::new(),
            cpp_files: Vec::new(),
            rust_lib_rs: None,
            config_yaml: None,
            lua_files: Vec::new(),
            lib_files: Vec::new(),
            rust_lib_names: Vec::new(),
            asset_files: Vec::new(),
            persistent_resources: Vec::new(),
        }
    }

    /// Declare `names` as `persistent_resources` in blyt.build.yaml (#160,
    /// ADR-0028): the runtime pre-loads them before init() and never evicts them.
    /// Names are resource names (the packer-derived names, e.g. `big` for
    /// `assets/big.bin`).
    pub fn persistent(mut self, names: &[&str]) -> Self {
        self.persistent_resources
            .extend(names.iter().map(|s| s.to_string()));
        self
    }

    /// Add `content` as `assets/<rel_path>` (issue #91). The packer derives the
    /// resource name from the path and emits an R_<NAME> constant. `write`
    /// auto-declares the asset's extension as an `include:` glob (#162: nothing
    /// is auto-scanned).
    pub fn asset(mut self, rel_path: &str, content: &str) -> Self {
        self.asset_files
            .push((rel_path.into(), content.as_bytes().to_vec()));
        self
    }

    /// Add opaque `bytes` as `assets/<rel_path>` — for raw resources whose
    /// content is binary (embedded NULs, non-UTF8). Same auto-declaration as
    /// `asset` (#162).
    pub fn asset_bytes(mut self, rel_path: &str, bytes: &[u8]) -> Self {
        self.asset_files.push((rel_path.into(), bytes.to_vec()));
        self
    }

    /// Add `source` as `src/game/c/main.c`.
    pub fn c(self, source: &str) -> Self {
        self.c_file("main.c", source)
    }

    /// Add `source` as `src/game/c/<name>`.
    pub fn c_file(mut self, name: &str, source: &str) -> Self {
        self.c_files.push((name.into(), source.into()));
        self
    }

    /// Add `source` as `src/game/c++/main.cpp`.
    pub fn cpp(self, source: &str) -> Self {
        self.cpp_file("main.cpp", source)
    }

    /// Add `source` as `src/game/c++/<name>`.
    pub fn cpp_file(mut self, name: &str, source: &str) -> Self {
        self.cpp_files.push((name.into(), source.into()));
        self
    }

    /// Add `source` as `src/game/lua/main.lua`.
    pub fn lua(self, source: &str) -> Self {
        self.lua_file("main.lua", source)
    }

    /// Add `source` as `src/game/lua/<name>`.
    pub fn lua_file(mut self, name: &str, source: &str) -> Self {
        self.lua_files.push((name.into(), source.into()));
        self
    }

    /// Set the contents of `src/game/rust/src/lib.rs`.
    /// `blyt build` injects the SDK crate path via `--config` so
    /// the `blyt = "0.1"` dependency resolves without a published crate.
    pub fn rust(mut self, lib_rs: &str) -> Self {
        self.rust_lib_rs = Some(lib_rs.into());
        self
    }

    /// Add a Rust library crate at `src/lib/<name>/`.
    ///
    /// Creates `src/lib/<name>/Cargo.toml` (package name = `<name>`) and
    /// `src/lib/<name>/src/lib.rs` from `lib_rs`.  The game's Cargo.toml is
    /// generated to include `<name> = "0.1"` as a dependency; `blyt build`
    /// injects the source path via `--config` at build time.
    pub fn rust_lib(mut self, name: &str, lib_rs: &str) -> Self {
        self.lib_files
            .push((name.into(), "Cargo.toml".into(), rust_lib_cargo_toml(name)));
        self.lib_files
            .push((name.into(), "src/lib.rs".into(), lib_rs.into()));
        self.rust_lib_names.push(name.into());
        self
    }

    /// Add a file to a library at `src/lib/<name>/<rel_path>`.
    ///
    /// Use `include/<filename>.h` as `rel_path` for public headers; the build
    /// tool exposes `src/lib/<name>/include/` as the include root when it exists.
    /// Use a bare filename for source files and headers in flat layouts.
    pub fn lib_file(mut self, name: &str, rel_path: &str, content: &str) -> Self {
        self.lib_files
            .push((name.into(), rel_path.into(), content.into()));
        self
    }

    /// Set the contents of `blyt.config.yaml` (state buffers, fps, etc.).
    pub fn config(mut self, yaml: &str) -> Self {
        self.config_yaml = Some(yaml.into());
        self
    }

    /// Write the project layout under `dir` and panic on any I/O error.
    pub fn write(self, dir: &std::path::Path) {
        let has_c = !self.c_files.is_empty();
        let has_cpp = !self.cpp_files.is_empty();
        let has_rust = self.rust_lib_rs.is_some();
        let has_lua = !self.lua_files.is_empty();
        assert!(
            has_c || has_cpp || has_rust || has_lua,
            "CartProject::write: no game language declared"
        );

        // Ensure the project root exists before writing any files into it.
        fs::create_dir_all(dir).unwrap();

        // blyt.info.yaml — mandatory for all blyt cart projects.  The title
        // contains a space on purpose (it is not filename-constrained) and
        // `version:` is omitted to exercise the 0.0.1-dev default.
        let project_name = dir.file_name().unwrap_or_default().to_string_lossy();
        fs::write(
            dir.join("blyt.info.yaml"),
            format!("id: {project_name}\ntitle: {project_name} Title\n"),
        )
        .unwrap();

        // blyt.build.yaml — language declaration (ADR-0073) + asset declaration
        // (ADR-0088 amendment, #162: nothing is auto-scanned, so any asset's
        // extension must be declared as an `include:` glob).  Pure Lua carts
        // omit the file unless they ship assets.
        let native_count = [has_c, has_cpp, has_rust].iter().filter(|&&b| b).count();
        let mut manifest = String::new();
        if has_lua && native_count == 0 {
            // pure Lua: language declaration only needed alongside an assets block
            if !self.asset_files.is_empty() {
                manifest.push_str("language: lua\n");
            }
        } else if !has_lua && native_count == 1 {
            // pure native: singular `language:` form
            manifest.push_str(if has_c {
                "language: c\n"
            } else if has_cpp {
                "language: \"c++\"\n"
            } else {
                "language: rust\n"
            });
        } else {
            // hybrid Lua + native: `languages:` map
            manifest.push_str("languages:\n");
            if has_lua {
                manifest.push_str("  lua:\n");
            }
            if has_c {
                manifest.push_str("  c:\n");
            }
            if has_cpp {
                manifest.push_str("  \"c++\":\n");
            }
            if has_rust {
                manifest.push_str("  rust:\n");
            }
        }
        manifest.push_str(&assets_include_block(&self.asset_files));
        if !self.persistent_resources.is_empty() {
            manifest.push_str("persistent_resources: [");
            manifest.push_str(&self.persistent_resources.join(", "));
            manifest.push_str("]\n");
        }
        if !manifest.is_empty() {
            fs::write(dir.join("blyt.build.yaml"), manifest).unwrap();
        }

        if has_c {
            let c_dir = dir.join("src/game/c");
            fs::create_dir_all(&c_dir).unwrap();
            for (name, source) in &self.c_files {
                fs::write(c_dir.join(name), source).unwrap();
            }
        }

        if has_cpp {
            let cpp_dir = dir.join("src/game/c++");
            fs::create_dir_all(&cpp_dir).unwrap();
            for (name, source) in &self.cpp_files {
                fs::write(cpp_dir.join(name), source).unwrap();
            }
        }

        if let Some(lib_rs) = self.rust_lib_rs {
            let rust_src = dir.join("src/game/rust/src");
            fs::create_dir_all(&rust_src).unwrap();
            fs::write(rust_src.join("lib.rs"), lib_rs).unwrap();
            fs::write(
                dir.join("src/game/rust/Cargo.toml"),
                rust_game_cargo_toml(&self.rust_lib_names),
            )
            .unwrap();
        }

        if has_lua {
            let lua_dir = dir.join("src/game/lua");
            fs::create_dir_all(&lua_dir).unwrap();
            for (name, source) in &self.lua_files {
                fs::write(lua_dir.join(name), source).unwrap();
            }
        }

        for (lib_name, rel_path, content) in &self.lib_files {
            let dest = dir.join("src/lib").join(lib_name).join(rel_path);
            fs::create_dir_all(dest.parent().unwrap()).unwrap();
            fs::write(dest, content).unwrap();
        }

        if let Some(ref yaml) = self.config_yaml {
            fs::write(dir.join("blyt.config.yaml"), yaml).unwrap();
        }

        for (rel_path, content) in &self.asset_files {
            let dest = dir.join("assets").join(rel_path);
            fs::create_dir_all(dest.parent().unwrap()).unwrap();
            fs::write(dest, content).unwrap();
        }
    }
}

/// Generate the `assets:` block declaring an `include:` glob for every distinct
/// extension present in `asset_files` (under the default `assets/` dir). Empty
/// when there are no assets. Mirrors a real cart explicitly declaring its asset
/// types (#162 — no auto-scan).
fn assets_include_block(asset_files: &[(String, Vec<u8>)]) -> String {
    let mut exts: Vec<String> = Vec::new();
    for (rel, _) in asset_files {
        if let Some(ext) = std::path::Path::new(rel)
            .extension()
            .and_then(|e| e.to_str())
        {
            let e = ext.to_string();
            if !exts.contains(&e) {
                exts.push(e);
            }
        }
    }
    if exts.is_empty() {
        return String::new();
    }
    exts.sort();
    let mut s = String::from("assets:\n  dirs:\n    - dir: assets/\n      include:\n");
    for e in exts {
        s.push_str(&format!("        - \"**/*.{e}\"\n"));
    }
    s
}

// -------------------------------------------------------------------------
// Convenience wrappers (thin delegators to CartProject)
// -------------------------------------------------------------------------

/// Shorthand for `CartProject::new().c(source).write(dir)`.
pub fn write_c_cart_project(dir: &std::path::Path, source: &str) {
    CartProject::new().c(source).write(dir);
}

/// Shorthand for `CartProject::new().rust(lib_rs).write(dir)`.
pub fn write_rust_cart_project(dir: &std::path::Path, lib_rs: &str) {
    CartProject::new().rust(lib_rs).write(dir);
}

// -------------------------------------------------------------------------
// Rust toolchain probe
// -------------------------------------------------------------------------

/// Returns true if Rust cart builds are possible: the pinned nightly toolchain
/// (or $BLYT_RUST_TOOLCHAIN override) is installed. Cart builds use a custom
/// JSON target (riscv32imafdc-blyt-none-elf) via -Z build-std, so the target
/// never appears in `rustup target list`; the nightly toolchain is the real gate.
pub fn has_rust_riscv_target() -> bool {
    let toolchain =
        std::env::var("BLYT_RUST_TOOLCHAIN").unwrap_or_else(|_| "nightly-2026-06-01".to_string());
    std::process::Command::new("rustup")
        .args(["toolchain", "list"])
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).contains(&toolchain))
        .unwrap_or(false)
}

/// Returns true if a usable `luac` is available (SDK blyt-luac, $BLYT_LUAC, or system luac).
pub fn has_luac() -> bool {
    // Check $BLYT_LUAC override first.
    if let Ok(c) = std::env::var("BLYT_LUAC") {
        return std::path::Path::new(&c).exists();
    }
    // SDK symlink.
    if sdk_dir().join("bin/blyt-luac").exists() {
        return true;
    }
    // System luac.
    std::process::Command::new("luac")
        .arg("-v")
        .output()
        .is_ok()
}

// -------------------------------------------------------------------------
// Tooling requirement assertions
//
// Call these at the top of tests that need a given tool.  They panic with an
// actionable message instead of silently returning, so a missing tool is a
// test failure, not an invisible skip.
// -------------------------------------------------------------------------

pub fn require_sdk() {
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run `cmake --build build --target sdk` first"
    );
    assert!(
        sdk_dir().join("lib/libblyt32.so").exists(),
        "libblyt32.so not in SDK — run `cmake --build build --target sdk` first"
    );
}

pub fn require_cpp_sdk() {
    assert!(
        sdk_dir().join("bin/blyt-clang++").exists(),
        "blyt-clang++ not in SDK — run `cmake --build build --target sdk` first"
    );
    assert!(
        sdk_dir().join("lib/libc++.a").exists(),
        "libc++.a not in SDK — run `cmake --build build --target sdk` first"
    );
}

pub fn require_lua_sdk() {
    assert!(
        sdk_dir().join("lib/libblyt32lua.so").exists(),
        "libblyt32lua.so not in SDK — run `cmake --build build --target sdk` first"
    );
    assert!(
        has_luac(),
        "luac not available — install luac or set BLYT_LUAC"
    );
}

pub fn require_rust_riscv_target() {
    let toolchain =
        std::env::var("BLYT_RUST_TOOLCHAIN").unwrap_or_else(|_| "nightly-2026-06-01".to_string());
    assert!(
        has_rust_riscv_target(),
        "Rust cart toolchain '{toolchain}' not installed — \
         run `rustup toolchain install {toolchain} --profile minimal --component rust-src`"
    );
}

pub fn require_test_session_api() {
    assert!(
        test_session_api().exists(),
        "test_session_api not built — run `cmake --build build` first"
    );
}

pub fn require_wasm() {
    assert!(
        find_wasm_dir().join("blytplay.js").exists(),
        "WASM runtime not built — install emscripten and run \
         `cmake --build build --target sdk` (incremental rebuild: \
         `cmake --build build/build-wasm`)"
    );
}

/// Require the debug WASM runtime (blytdebug.*, DAP/GDB enabled) for the WASM
/// DAP/GDB tests (ADR-0129).
pub fn require_wasm_debug() {
    assert!(
        find_wasm_debug_dir().join("blytdebug.js").exists(),
        "debug WASM runtime not built — run `cmake --build build --target sdk` \
         (builds share/wasm-debug/blytdebug.* with BLYT_DAP/BLYT_GDB; \
         incremental rebuild: `cmake --build build/build-wasm-debug`)"
    );
}

pub fn require_playwright() {
    let pkg = repo_root().join("tests/wasm/node_modules/playwright");
    assert!(
        pkg.exists(),
        "playwright not installed — run:\n  \
         cd tests/wasm && npm install && npx playwright install chromium"
    );
}

pub fn libretro_so() -> PathBuf {
    build_dir().join("blyt_libretro.so")
}

pub fn test_libretro_core() -> PathBuf {
    build_dir().join("test_libretro_core")
}

pub fn require_libretro_core() {
    assert!(
        libretro_so().exists(),
        "blyt_libretro.so not built — run `cmake --build build` first"
    );
    assert!(
        test_libretro_core().exists(),
        "test_libretro_core not built — run `cmake --build build` first \
         (requires RV32 toolchain for LIBBLYTC_OUT)"
    );
}

// -------------------------------------------------------------------------
// Cart build helpers
// -------------------------------------------------------------------------

/// Run `blyt build <project_dir>` and return the expected cart output path.
pub fn build_cart(project_dir: &std::path::Path) -> PathBuf {
    use assert_cmd::Command;
    let sdk = sdk_dir();
    let mut cmd = Command::new(blyt_bin());
    cmd.args(["build", project_dir.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"))
        .env("BLYT_RUST_SDK", repo_root().join("sdk/rust/blyt"));
    let sdk_clang = sdk.join("bin/blyt-clang");
    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    let sdk_clangpp = sdk.join("bin/blyt-clang++");
    if sdk_clangpp.exists() {
        cmd.env("BLYT_CLANGPP", &sdk_clangpp);
    }
    let sdk_ar = sdk.join("bin/blyt-llvm-ar");
    if sdk_ar.exists() {
        cmd.env("BLYT_AR", &sdk_ar);
    }
    cmd.assert().success();

    project_dir.join("build").join(format!(
        "{}.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Run `blyt build <project_dir>` expecting it to FAIL, returning combined
/// stdout+stderr so the caller can assert on the build-error message (#166:
/// invalid-text build errors, Rust typed-handle compile failures). Mirrors
/// `build_cart`'s full env (clang/clang++/ar/luac) so the failure is a real
/// build error, not a missing tool.
pub fn build_cart_expect_failure(project_dir: &std::path::Path) -> String {
    use assert_cmd::Command;
    let sdk = sdk_dir();
    let mut cmd = Command::new(blyt_bin());
    cmd.args(["build", project_dir.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"))
        .env("BLYT_RUST_SDK", repo_root().join("sdk/rust/blyt"));
    for (var, rel) in [
        ("BLYT_CLANG", "bin/blyt-clang"),
        ("BLYT_CLANGPP", "bin/blyt-clang++"),
        ("BLYT_AR", "bin/blyt-llvm-ar"),
        ("BLYT_LUAC", "bin/blyt-luac"),
    ] {
        let p = sdk.join(rel);
        if p.exists() {
            cmd.env(var, &p);
        }
    }
    let out = cmd.assert().failure().get_output().clone();
    let mut s = String::from_utf8_lossy(&out.stdout).into_owned();
    s.push_str(&String::from_utf8_lossy(&out.stderr));
    s
}

/// Build a dev ELF (`build/.elf`) via `blyt build code <project_dir>`.
/// Requires the SDK; does not require the WASM runtime.
pub fn build_dev_elf(project_dir: &std::path::Path) -> PathBuf {
    use assert_cmd::Command;
    let sdk = sdk_dir();
    let mut cmd = Command::new(blyt_bin());
    cmd.args(["build", "code", project_dir.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"));
    let sdk_clang = sdk.join("bin/blyt-clang");
    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    let sdk_luac = sdk.join("bin/blyt-luac");
    if sdk_luac.exists() {
        cmd.env("BLYT_LUAC", &sdk_luac);
    }
    cmd.assert().success();
    let elf_path = project_dir.join("build/.elf");
    assert!(
        elf_path.exists(),
        "build/.elf not created by blyt build code: {}",
        elf_path.display()
    );
    elf_path
}

/// Run `blyt build <project_dir>` with Lua-specific env vars and return the cart path.
pub fn build_lua_cart(project_dir: &std::path::Path) -> PathBuf {
    use assert_cmd::Command;
    let sdk = sdk_dir();
    let mut cmd = Command::new(blyt_bin());
    cmd.args(["build", project_dir.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"))
        .env("BLYT_RUST_SDK", repo_root().join("sdk/rust/blyt"));
    let sdk_clang = sdk.join("bin/blyt-clang");
    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    let sdk_clangpp = sdk.join("bin/blyt-clang++");
    if sdk_clangpp.exists() {
        cmd.env("BLYT_CLANGPP", &sdk_clangpp);
    }
    let sdk_ar = sdk.join("bin/blyt-llvm-ar");
    if sdk_ar.exists() {
        cmd.env("BLYT_AR", &sdk_ar);
    }
    let sdk_luac = sdk.join("bin/blyt-luac");
    if sdk_luac.exists() {
        cmd.env("BLYT_LUAC", &sdk_luac);
    }
    cmd.assert().success();

    project_dir.join("build").join(format!(
        "{}.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Run a cart with blytplay --headless; assert `expected` appears in stdout.
pub fn run_cart_native(cart: &std::path::Path, expected: &str) {
    run_cart_native_with_env(cart, &[], expected)
}

/// Run a cart with blytplay --headless; assert the process exits with a non-zero status.
pub fn run_cart_native_expect_fail(cart: &std::path::Path) {
    use assert_cmd::Command;
    Command::new(blytplay())
        .args(["--headless", cart.to_str().unwrap()])
        .assert()
        .failure();
}

/// Run a cart with blytplay --headless {extra_flags...} {cart}; assert `expected`
/// appears in stdout.
pub fn run_cart_native_with_flags(cart: &std::path::Path, extra_flags: &[&str], expected: &str) {
    use assert_cmd::Command;
    let mut cmd = Command::new(blytplay());
    cmd.arg("--headless");
    for f in extra_flags {
        cmd.arg(f);
    }
    cmd.arg(cart.to_str().unwrap());
    let output = cmd.assert().success().get_output().stdout.clone();
    assert!(
        String::from_utf8_lossy(&output).contains(expected),
        "expected {:?} in native output, got: {}",
        expected,
        String::from_utf8_lossy(&output)
    );
}

/// Run a cart with blytplay --headless plus extra environment variables; assert
/// `expected` appears in stdout.
pub fn run_cart_native_with_env(
    cart: &std::path::Path,
    extra_env: &[(&str, &str)],
    expected: &str,
) {
    use assert_cmd::Command;
    let mut cmd = Command::new(blytplay());
    cmd.args(["--headless", cart.to_str().unwrap()]);
    for (k, v) in extra_env {
        cmd.env(k, v);
    }
    let output = cmd.assert().success().get_output().stdout.clone();
    assert!(
        String::from_utf8_lossy(&output).contains(expected),
        "expected {:?} in native output, got: {}",
        expected,
        String::from_utf8_lossy(&output)
    );
}

/// Run a cart with the WASM runner; assert `expected` appears in stdout.
pub fn run_cart_wasm(cart: &std::path::Path, expected: &str) {
    run_cart_wasm_with_env(cart, &[], expected)
}

/// Run a cart with the WASM runner plus extra C environment variables; assert
/// `expected` appears in stdout.
///
/// Variables are injected via the 5th argument to `run_cart.js` as a JSON
/// object, read by `module_pre.js` into Emscripten's `ENV` table before C
/// startup.  This is needed because Emscripten does not inherit Node.js
/// `process.env` for the multi-environment (web+node) build.
pub fn run_cart_wasm_with_env(cart: &std::path::Path, extra_env: &[(&str, &str)], expected: &str) {
    use assert_cmd::Command;
    let driver = repo_root().join("tests/wasm/run_cart.js");
    let wasm_dir = find_wasm_dir();
    let mut cmd = Command::new("node");
    cmd.args([
        driver.to_str().unwrap(),
        wasm_dir.to_str().unwrap(),
        cart.to_str().unwrap(),
    ]);
    if !extra_env.is_empty() {
        // Build a minimal JSON object {"KEY":"VALUE",...} without serde_json.
        let pairs: Vec<String> = extra_env
            .iter()
            .map(|(k, v)| {
                format!(
                    "\"{}\":\"{}\"",
                    k,
                    v.replace('\\', "\\\\").replace('"', "\\\"")
                )
            })
            .collect();
        let env_json = format!("{{{}}}", pairs.join(","));
        // 4th arg = frame0OutPath (empty); 5th arg = env JSON
        cmd.args(["", &env_json]);
    }
    let output = cmd.assert().success().get_output().stdout.clone();
    assert!(
        String::from_utf8_lossy(&output).contains(expected),
        "expected {:?} in wasm output, got: {}",
        expected,
        String::from_utf8_lossy(&output)
    );
}

/// Like [`run_cart_wasm_with_env`], but also seeds files into the WASM MEMFS
/// before the cart runs: each `(memfs_path, host_path)` reads the host file and
/// writes it to `memfs_path` (parent dirs created).  Needed for cross-process
/// save sharing — a save written by one cart process is not visible to another
/// (each WASM run has a private MEMFS), so the writer's byte-identical `.blys`
/// is seeded into the reader's MEMFS.
pub fn run_cart_wasm_with_env_and_seed(
    cart: &std::path::Path,
    extra_env: &[(&str, &str)],
    seed_files: &[(&str, &std::path::Path)],
    expected: &str,
) {
    use assert_cmd::Command;
    let driver = repo_root().join("tests/wasm/run_cart.js");
    let wasm_dir = find_wasm_dir();
    // Minimal JSON encoding (no serde_json), matching run_cart_wasm_with_env.
    let json_obj = |pairs: Vec<String>| format!("{{{}}}", pairs.join(","));
    let esc = |s: &str| s.replace('\\', "\\\\").replace('"', "\\\"");
    let env_json = json_obj(
        extra_env
            .iter()
            .map(|(k, v)| format!("\"{}\":\"{}\"", esc(k), esc(v)))
            .collect(),
    );
    let seed_json = json_obj(
        seed_files
            .iter()
            .map(|(memfs, host)| format!("\"{}\":\"{}\"", esc(memfs), esc(host.to_str().unwrap())))
            .collect(),
    );
    let mut cmd = Command::new("node");
    // 4th arg = frame0OutPath (empty); 5th = env JSON; 6th = seed-files JSON.
    cmd.args([
        driver.to_str().unwrap(),
        wasm_dir.to_str().unwrap(),
        cart.to_str().unwrap(),
        "",
        &env_json,
        &seed_json,
    ]);
    let output = cmd.assert().success().get_output().stdout.clone();
    assert!(
        String::from_utf8_lossy(&output).contains(expected),
        "expected {:?} in wasm output, got: {}",
        expected,
        String::from_utf8_lossy(&output)
    );
}

/// Run a cart through the embedded libretro core (test_libretro_core dlopens
/// blyt_libretro.so); assert `expected` appears in the output.  This is the
/// third leg of the native/wasm test pairs: it exercises the core's EMBEDDED
/// guest lib blobs, which are a separate artifact from the sdk/lib files the
/// blytplay path loads.  Cart debug output arrives via the libretro log
/// callback, which the driver writes to stderr.
pub fn run_cart_libretro(cart: &std::path::Path, expected: &str) {
    run_cart_libretro_with_flags(cart, &[], expected)
}

/// Run a cart through the embedded libretro core with driver flags (e.g.
/// `--reset-every-frame` to drive retro_reset_every_frame_cycle() after
/// every frame — the same save-state stress cycle as blytplay
/// --reset-every-frame); assert `expected` appears in the output.
pub fn run_cart_libretro_with_flags(cart: &std::path::Path, flags: &[&str], expected: &str) {
    use assert_cmd::Command;
    let mut cmd = Command::new(test_libretro_core());
    for f in flags {
        cmd.arg(f);
    }
    cmd.args([libretro_so().to_str().unwrap(), cart.to_str().unwrap()]);
    let output = cmd.assert().success().get_output().stderr.clone();
    assert!(
        String::from_utf8_lossy(&output).contains(expected),
        "expected {:?} in libretro core output, got: {}",
        expected,
        String::from_utf8_lossy(&output)
    );
}

/// Run a cart through the embedded libretro core with extra process environment
/// variables (e.g. `BLYT_SAVE_DIR` for carts that do disk-backed
/// `save_write`/`save_read`); assert `expected` appears in the output. The host
/// runtime inside `test_libretro_core` reads the env, so these flow through to
/// the embedded guest libs' save path.
pub fn run_cart_libretro_with_env(
    cart: &std::path::Path,
    extra_env: &[(&str, &str)],
    expected: &str,
) {
    use assert_cmd::Command;
    let mut cmd = Command::new(test_libretro_core());
    cmd.args([libretro_so().to_str().unwrap(), cart.to_str().unwrap()]);
    for (k, v) in extra_env {
        cmd.env(k, v);
    }
    let output = cmd.assert().success().get_output().stderr.clone();
    assert!(
        String::from_utf8_lossy(&output).contains(expected),
        "expected {:?} in libretro core output, got: {}",
        expected,
        String::from_utf8_lossy(&output)
    );
}

/// Dump frame 0 (XRGB8888, 320x240) of a cart via `blytplay --headless
/// --dump-frame0` — the host-runtime (blytplay) leg.  The palette-agnostic test
/// card (#204) is a host-runtime fallback, so its expanded colours are only
/// observable on the host-runtime legs (blytplay / wasm-emulated / libretro),
/// not on bare metal.
pub fn dump_frame0_native(cart: &std::path::Path) -> Vec<u8> {
    use assert_cmd::Command;
    let tmp = tempfile::NamedTempFile::new().unwrap();
    Command::new(blytplay())
        .args([
            "--headless",
            "--dump-frame0",
            tmp.path().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .assert()
        .success();
    std::fs::read(tmp.path()).unwrap()
}

/// Dump frame 0 (XRGB8888) of a cart via the WASM runner: `run_cart.js` writes
/// the first rendered frame's expanded bytes to its 4th-arg path.
pub fn dump_frame0_wasm(cart: &std::path::Path) -> Vec<u8> {
    use assert_cmd::Command;
    let driver = repo_root().join("tests/wasm/run_cart.js");
    let wasm_dir = find_wasm_dir();
    let out = tempfile::NamedTempFile::new().unwrap();
    Command::new("node")
        .args([
            driver.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
            out.path().to_str().unwrap(),
        ])
        .assert()
        .success();
    std::fs::read(out.path()).unwrap()
}

/// Run a cart through the embedded libretro core; assert the driver exits
/// with a non-zero status (load failure or runtime error).
pub fn run_cart_libretro_expect_fail(cart: &std::path::Path) {
    use assert_cmd::Command;
    Command::new(test_libretro_core())
        .args([libretro_so().to_str().unwrap(), cart.to_str().unwrap()])
        .assert()
        .failure();
}

/// Run the same cart across all three legs (native / WASM / libretro) and assert
/// `expected` appears in each leg's output.
///
/// Cross-platform behaviour must be identical — determinism across every target
/// is the core contract — so a feature that diverges between legs fails here by
/// construction, instead of relying on someone remembering to assert each leg
/// separately. Callers must gate on `require_sdk()` + `require_wasm()` +
/// `require_libretro_core()` first (plus `require_lua_sdk()` for Lua carts).
pub fn run_cart_all_legs(cart: &std::path::Path, expected: &str) {
    run_cart_native(cart, expected);
    run_cart_wasm(cart, expected);
    run_cart_libretro(cart, expected);
}

/// Like [`run_cart_all_legs`], but drives each leg's `--reset-every-frame`
/// save/restore stress cycle, translating the per-leg knob (blytplay flag /
/// `BLYT_RESET_EVERY_FRAME` env / driver flag). The cart must terminate itself
/// (call `blyt.quit()` / `blyt_quit()`), as the WASM and libretro drivers do not
/// pass a frame cap.
pub fn run_cart_all_legs_reset_every_frame(cart: &std::path::Path, expected: &str) {
    run_cart_native_with_flags(cart, &["--reset-every-frame"], expected);
    run_cart_wasm_with_env(cart, &[("BLYT_RESET_EVERY_FRAME", "1")], expected);
    run_cart_libretro_with_flags(cart, &["--reset-every-frame"], expected);
}

/// Like [`run_cart_all_legs`], but force-evicts every evictable resource after
/// each frame (ADR-0027 v2, #137), translating the per-leg knob (blytplay
/// `--evict-every-frame` flag / `BLYT_RESOURCE_EVICT_EVERY_FRAME` env / driver
/// flag). A cart that re-reads a resource each frame thus rehydrates it from
/// scratch every frame; asserting the same `expected` here and under plain
/// [`run_cart_all_legs`] proves eviction is cart-invisible (bytes byte-identical
/// after rehydration, output unchanged). The cart must terminate itself.
pub fn run_cart_all_legs_evict_every_frame(cart: &std::path::Path, expected: &str) {
    run_cart_native_with_flags(cart, &["--evict-every-frame"], expected);
    run_cart_wasm_with_env(cart, &[("BLYT_RESOURCE_EVICT_EVERY_FRAME", "1")], expected);
    run_cart_libretro_with_flags(cart, &["--evict-every-frame"], expected);
}

/// Like [`run_cart_all_legs`], but for carts that do disk-backed
/// `save_write`/`save_read`: each leg is given a `BLYT_SAVE_DIR`. Native and
/// libretro share a fresh host tempdir; WASM uses `/tmp` (the only path that
/// exists in its Emscripten MEMFS by default — a host tempdir path would not).
/// Each leg writes its save before reading it back within the same run, so the
/// shared native/libretro dir is self-contained per leg.
pub fn run_cart_all_legs_with_save_dir(cart: &std::path::Path, expected: &str) {
    let save_dir = tempfile::TempDir::new().unwrap();
    let sd = save_dir.path().to_str().unwrap();
    run_cart_native_with_env(cart, &[("BLYT_SAVE_DIR", sd)], expected);
    run_cart_wasm_with_env(cart, &[("BLYT_SAVE_DIR", "/tmp")], expected);
    run_cart_libretro_with_env(cart, &[("BLYT_SAVE_DIR", sd)], expected);
}

/// Cross-version save round trip (issue #112): a `writer` cart (declaring one
/// `save_version`) writes a save; a `reader` cart (declaring a *different*
/// `save_version`) loads it and must observe the **writer's** version — the
/// value comes from the save header, not the reader's own manifest. `writer`
/// and `reader` must share a cart `id` (the save path is keyed by id) and an
/// identical state-buffer schema (so the load is accepted).
///
/// `writer_expected` is asserted on the writer run; `reader_expected` on every
/// reader leg. native(blytplay) + libretro share a real host save dir, so the
/// writer's `<sd>/<id>/slot_0.blys` is read back directly. WASM's per-process
/// MEMFS can't see another process's save, so that byte-identical file is
/// seeded into the WASM reader's MEMFS at `/tmp/<id>/slot_0.blys`.
pub fn run_cart_cross_version_all_legs(
    writer: &std::path::Path,
    reader: &std::path::Path,
    cart_id: &str,
    writer_expected: &str,
    reader_expected: &str,
) {
    let save_dir = tempfile::TempDir::new().unwrap();
    let sd = save_dir.path().to_str().unwrap();

    // Writer runs once (blytplay) to produce <sd>/<id>/slot_0.blys.
    run_cart_native_with_env(writer, &[("BLYT_SAVE_DIR", sd)], writer_expected);
    let blys = save_dir.path().join(cart_id).join("slot_0.blys");
    assert!(
        blys.exists(),
        "writer did not produce a save at {}",
        blys.display()
    );

    // native(blytplay) + libretro read the writer's save from the shared dir.
    run_cart_native_with_env(reader, &[("BLYT_SAVE_DIR", sd)], reader_expected);
    run_cart_libretro_with_env(reader, &[("BLYT_SAVE_DIR", sd)], reader_expected);

    // WASM: seed the writer's .blys into the reader process's MEMFS.
    let memfs_path = format!("/tmp/{cart_id}/slot_0.blys");
    run_cart_wasm_with_env_and_seed(
        reader,
        &[("BLYT_SAVE_DIR", "/tmp")],
        &[(memfs_path.as_str(), blys.as_path())],
        reader_expected,
    );
}

/// Path to the test_session_api binary produced by the CMake build.
pub fn test_session_api() -> PathBuf {
    build_dir().join("test_session_api")
}

/// Locate the WASM runtime directory (SDK layout).  The emcmake trees at
/// build/build-wasm[-debug] emit their artifacts directly here via
/// BLYT_WASM_OUT_DIR, so this is always the authoritative copy.
pub fn find_wasm_dir() -> PathBuf {
    sdk_dir().join("share/wasm")
}

/// Locate the DEBUG WASM runtime dir (blytdebug.*, built with BLYT_DAP/BLYT_GDB).
/// The release blytplay has DAP/GDB compiled out (ADR-0129), so the WASM DAP/GDB
/// tests must use this variant.
pub fn find_wasm_debug_dir() -> PathBuf {
    sdk_dir().join("share/wasm-debug")
}

// -------------------------------------------------------------------------
// GDB helpers
// -------------------------------------------------------------------------

pub fn require_gdb() {
    // ADR-0129: GDB/DAP debugging lives in blytdebug, not the release blytplay.
    assert!(
        blytdebug().exists(),
        "blytdebug not built — run `cmake --build build` first"
    );
    let out = std::process::Command::new(blytdebug())
        .arg("--help")
        .output()
        .unwrap_or_else(|_| {
            // blytdebug exits non-zero for --help; capture output anyway
            std::process::Command::new(blytdebug())
                .args(["--gdb", "0"])
                .output()
                .unwrap()
        });
    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);
    let _ = (stdout, stderr); // just ensure blytplay accepts --gdb
}

/// Parse "blyt: GDB listening on port N" from combined stdout/stderr output.
pub fn blytplay_gdb_port(output: &str) -> Option<u16> {
    let m = output.find("GDB listening on port ")?;
    let rest = &output[m + "GDB listening on port ".len()..];
    let end = rest
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(rest.len());
    rest[..end].parse().ok()
}

/// Run `blyt build <project_dir>` with Lua-specific env vars and the `--debug`
/// flag, returning the cart path.
pub fn build_debug_lua_cart(project_dir: &std::path::Path) -> PathBuf {
    use assert_cmd::Command;
    let sdk = sdk_dir();
    let mut cmd = Command::new(blyt_bin());
    cmd.args(["build", "--debug", project_dir.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"));
    let sdk_clang = sdk.join("bin/blyt-clang");
    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    let sdk_clangpp = sdk.join("bin/blyt-clang++");
    if sdk_clangpp.exists() {
        cmd.env("BLYT_CLANGPP", &sdk_clangpp);
    }
    let sdk_ar = sdk.join("bin/blyt-llvm-ar");
    if sdk_ar.exists() {
        cmd.env("BLYT_AR", &sdk_ar);
    }
    let sdk_luac = sdk.join("bin/blyt-luac");
    if sdk_luac.exists() {
        cmd.env("BLYT_LUAC", &sdk_luac);
    }
    cmd.assert().success();

    // ADR-0129: debug builds are named <name>.dbg.blyt.
    project_dir.join("build").join(format!(
        "{}.dbg.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Build a cart with debug information.
///
/// Runs `blyt build --debug <project_dir>` and returns the expected cart path.
pub fn build_debug_cart(project_dir: &std::path::Path) -> PathBuf {
    use assert_cmd::Command;
    let sdk = sdk_dir();
    let mut cmd = Command::new(blyt_bin());
    cmd.args(["build", "--debug", project_dir.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"))
        .env("BLYT_RUST_SDK", repo_root().join("sdk/rust/blyt"));
    let sdk_clang = sdk.join("bin/blyt-clang");
    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    let sdk_clangpp = sdk.join("bin/blyt-clang++");
    if sdk_clangpp.exists() {
        cmd.env("BLYT_CLANGPP", &sdk_clangpp);
    }
    let sdk_ar = sdk.join("bin/blyt-llvm-ar");
    if sdk_ar.exists() {
        cmd.env("BLYT_AR", &sdk_ar);
    }
    cmd.assert().success();

    // ADR-0129: debug builds are named <name>.dbg.blyt.
    project_dir.join("build").join(format!(
        "{}.dbg.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Find the lldb-dap binary — prefers the SDK-bundled copy, falls back to PATH.
pub fn lldb_dap_bin() -> Option<PathBuf> {
    let sdk_candidate = sdk_dir().join("bin/blyt-lldb-dap");
    if sdk_candidate.exists() {
        return Some(sdk_candidate);
    }
    let out = std::process::Command::new("which")
        .arg("lldb-dap")
        .output()
        .ok()?;
    if out.status.success() {
        let path = String::from_utf8_lossy(&out.stdout).trim().to_string();
        if !path.is_empty() {
            return Some(PathBuf::from(path));
        }
    }
    None
}

/// Skip test if lldb-dap is not available.
pub fn require_lldb_dap() {
    assert!(
        lldb_dap_bin().is_some(),
        "lldb-dap not found — install LLVM or build SDK with blyt-lldb-dap"
    );
}

/// Find the virtual address of a symbol in a cart ELF using `readelf -s`.
///
/// Returns `None` if `readelf` is not available or the symbol is not found.
/// find_symbol_addr or panic.  Missing symbol lookup must be a test failure,
/// not a silent skip or a degraded handshake-only fallback: vacuous passes on
/// macOS (which lacks GNU readelf) hid a Linux-only GDB regression (65d8341).
pub fn require_symbol_addr(cart: &std::path::Path, symbol: &str) -> u64 {
    find_symbol_addr(cart, symbol).unwrap_or_else(|| {
        panic!(
            "symbol {symbol} not found in {} — install readelf or llvm-readelf \
             (brew install llvm); symbol lookup must not be skipped",
            cart.display()
        )
    })
}

pub fn find_symbol_addr(cart: &std::path::Path, symbol: &str) -> Option<u64> {
    // GNU readelf on Linux; llvm-readelf elsewhere (macOS has no readelf —
    // falling through silently here used to skip the GDB native-breakpoint
    // test sections on macOS entirely).
    let candidates = [
        "readelf",
        "llvm-readelf",
        "/opt/homebrew/opt/llvm/bin/llvm-readelf",
    ];
    for tool in candidates {
        let Ok(out) = std::process::Command::new(tool)
            .args(["-s", "--wide", cart.to_str()?])
            .output()
        else {
            continue;
        };
        let stdout = String::from_utf8_lossy(&out.stdout);
        for line in stdout.lines() {
            let parts: Vec<&str> = line.split_whitespace().collect();
            // readelf -s format: Num: Value Size Type Bind Vis Ndx Name
            if parts.len() >= 8 && parts[7] == symbol {
                return u64::from_str_radix(parts[1], 16).ok();
            }
        }
    }
    None
}
