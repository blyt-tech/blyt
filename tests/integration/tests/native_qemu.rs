/// Native RISC-V QEMU gate tests: trusted native cart execution on RISC-V ILP32 QEMU.
///
/// Images default to qemu-images/{kernel,rootfs.qcow2,id_ed25519} in the repo root.
/// Download them with: cmake --build build --target fetch_qemu_images
///
/// Override paths with env vars:
///   BLYT_QEMU_KERNEL   — path to patched c-sky 6.5-rc1 kernel Image
///   BLYT_QEMU_ROOTFS   — path to blyt-qemu-images rootfs qcow2 (Alpine + ILP32F musl)
///   BLYT_QEMU_SSH_KEY  — SSH private key (default: qemu-images/id_ed25519)
///
/// Silently skipped if images or qemu-system-riscv64 are not present.
mod common;

use common::{CartProject, build_dir, has_luac, repo_root, sdk_dir, write_c_cart_project};
use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

// ── Helpers ───────────────────────────────────────────────────────────────

/// Bind to port 0, record the assigned port, then drop the listener so QEMU
/// can use it.  There is a small TOCTOU window but it is acceptable for tests.
fn free_port() -> u16 {
    TcpListener::bind("127.0.0.1:0")
        .expect("bind to port 0")
        .local_addr()
        .unwrap()
        .port()
}

// ── QEMU handle ───────────────────────────────────────────────────────────

struct Qemu {
    child: Child,
    ssh_port: u16,
    ssh_key: PathBuf,
}

impl Qemu {
    fn start(kernel: &Path, rootfs: &Path, ssh_port: u16, ssh_key: &Path) -> Self {
        let netdev = format!("user,id=net0,hostfwd=tcp::{ssh_port}-:22");
        let drive = format!("id=rootfs,file={},format=qcow2,if=none", rootfs.display());

        let child = Command::new("qemu-system-riscv64")
            .args([
                "-machine",
                "virt",
                "-m",
                "1G",
                "-smp",
                "2",
                "-kernel",
                kernel.to_str().unwrap(),
                "-append",
                "root=/dev/vda rw init=/sbin/init-blyt console=ttyS0,115200n8 earlycon rootwait",
                "-drive",
                &drive,
                "-device",
                "virtio-blk-device,drive=rootfs",
                "-netdev",
                &netdev,
                "-device",
                "virtio-net-device,netdev=net0",
                "-nographic",
            ])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("failed to spawn qemu-system-riscv64");

        Qemu {
            child,
            ssh_port,
            ssh_key: ssh_key.to_path_buf(),
        }
    }

    fn ssh_flags(&self) -> Vec<String> {
        vec![
            "-o".into(),
            "StrictHostKeyChecking=no".into(),
            "-o".into(),
            "UserKnownHostsFile=/dev/null".into(),
            "-o".into(),
            "LogLevel=ERROR".into(),
            "-o".into(),
            "ConnectTimeout=5".into(),
            "-p".into(),
            self.ssh_port.to_string(),
            "-i".into(),
            self.ssh_key.to_str().unwrap().into(),
        ]
    }

    fn wait_for_ssh(&self, timeout: Duration) -> bool {
        let deadline = Instant::now() + timeout;
        print!("  waiting for QEMU SSH");
        while Instant::now() < deadline {
            let ok = Command::new("ssh")
                .args(self.ssh_flags())
                .args(["root@localhost", "echo ok"])
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .status()
                .map(|s| s.success())
                .unwrap_or(false);
            if ok {
                println!(" ready");
                return true;
            }
            print!(".");
            std::thread::sleep(Duration::from_secs(5));
        }
        println!(" timed out");
        false
    }

    fn ssh(&self, cmd: &str) -> std::process::Output {
        Command::new("ssh")
            .args(self.ssh_flags())
            .args(["root@localhost", cmd])
            .output()
            .unwrap_or_else(|e| panic!("ssh failed: {e}"))
    }

    fn ssh_ok(&self, cmd: &str) -> bool {
        self.ssh(cmd).status.success()
    }

    fn scp_to(&self, local: &Path, remote_dir: &str) -> bool {
        // scp uses uppercase -P for port (ssh uses lowercase -p); rebuild
        // the flags list with -P instead of -p for the port argument.
        let flags: Vec<String> = self
            .ssh_flags()
            .into_iter()
            .map(|f| if f == "-p" { "-P".to_string() } else { f })
            .collect();
        Command::new("scp")
            .args(flags)
            .arg(local)
            .arg(format!("root@localhost:{remote_dir}"))
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
    }
}

impl Drop for Qemu {
    fn drop(&mut self) {
        // Graceful shutdown, then kill if needed.
        let _ = Command::new("ssh")
            .args(self.ssh_flags())
            .args(["root@localhost", "poweroff"])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
        std::thread::sleep(Duration::from_secs(3));
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

// ── Cart builder ─────────────────────────────────────────────────────────

fn build_cart(project_dir: &Path) -> PathBuf {
    let sdk = sdk_dir();
    let sdk_clang = sdk.join("bin/blyt-clang");
    let sdk_blyt = sdk.join("bin/blyt");

    let mut cmd = if sdk_blyt.exists() {
        Command::new(&sdk_blyt)
    } else {
        let mut c =
            Command::new(std::env::var("CARGO_BIN_EXE_blyt").unwrap_or_else(|_| "blyt".into()));
        c.env("BLYT_SDK_DIR", &sdk)
            .env("BLYT_OBJCOPY", sdk.join("bin/blyt-objcopy"));
        c
    };

    if sdk_clang.exists() {
        cmd.env("BLYT_CLANG", &sdk_clang);
    }
    cmd.args(["build", project_dir.to_str().unwrap()])
        .status()
        .expect("blyt build failed")
        .success()
        .then_some(())
        .expect("blyt build returned non-zero");

    project_dir.join("build").join(format!(
        "{}.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

/// Recursively find the first file named `name` under `dir` (the host save path
/// is `<save_dir>/<cart_name>/slot_0.blys`; this avoids hard-coding cart_name).
fn find_file(dir: &Path, name: &str) -> Option<PathBuf> {
    for entry in std::fs::read_dir(dir).ok()?.flatten() {
        let p = entry.path();
        if p.is_dir() {
            if let Some(found) = find_file(&p, name) {
                return Some(found);
            }
        } else if p.file_name().is_some_and(|n| n == name) {
            return Some(p);
        }
    }
    None
}

// ── Gate test ─────────────────────────────────────────────────────────────

#[test]
fn native_riscv_qemu_gate() {
    // The RISC-V trusted-exec behaviour this gate verifies is host-independent
    // (it runs inside a RISC-V VM), so it adds nothing in the Linux Docker test
    // image — where booting qemu-system-riscv64 would itself be nested under
    // Rosetta x86 translation and time out.  The Docker target sets this to skip
    // it; the gate still runs natively (local dev + GitHub CI).
    if std::env::var_os("BLYT_SKIP_QEMU_GATE").is_some() {
        eprintln!("native_riscv_qemu_gate: skipped (BLYT_SKIP_QEMU_GATE set)");
        return;
    }

    // ── Prerequisites ──────────────────────────────────────────────────

    let kernel = std::env::var("BLYT_QEMU_KERNEL")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root().join("qemu-images/kernel"));
    let rootfs = std::env::var("BLYT_QEMU_ROOTFS")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root().join("qemu-images/rootfs.qcow2"));
    let ssh_key = std::env::var("BLYT_QEMU_SSH_KEY")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root().join("qemu-images/id_ed25519"));

    for (label, path) in [
        ("kernel", &kernel),
        ("rootfs", &rootfs),
        ("ssh key", &ssh_key),
    ] {
        if !path.exists() {
            eprintln!(
                "native_riscv_qemu_gate: {label} not found ({}) — skip",
                path.display()
            );
            return;
        }
    }

    if Command::new("qemu-system-riscv64")
        .arg("--version")
        .output()
        .is_err()
    {
        eprintln!("native_riscv_qemu_gate: qemu-system-riscv64 not found — skip");
        return;
    }

    if !Command::new("ssh").arg("-V").output().is_ok() {
        eprintln!("native_riscv_qemu_gate: ssh not found — skip");
        return;
    }

    let native_dir = sdk_dir().join("lib/native");
    assert!(
        native_dir.join("libblyt32.so").exists(),
        "sdk/lib/native/libblyt32.so not found — \
         run: cmake --build build --target libblyt32_native_so"
    );
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run: cmake --build build --target sdk"
    );

    // ── Build the hello cart ───────────────────────────────────────────
    let tmp = tempfile::TempDir::new().unwrap();
    let project = tmp.path().join("hello");
    write_c_cart_project(
        &project,
        r#"
#include "blyt.h"
static int s_frame = 0;
void blyt_cart_init(void)   { blyt_console_debug("init"); }
void blyt_cart_update(void) {
    blyt_console_debug("update");
    if (++s_frame >= 2) blyt_quit();
}
void blyt_cart_draw(void)   { blyt_console_debug("draw"); }
"#,
    );
    let hello_cart = build_cart(&project);
    assert!(
        hello_cart.exists(),
        "hello.blyt not built: {}",
        hello_cart.display()
    );

    // ── Build the dirty-frm cart (FCSR gate 4) ────────────────────────
    let dirty_project = tmp.path().join("dirty_frm");
    write_c_cart_project(
        &dirty_project,
        r#"
#include "blyt.h"
static int s_frame = 0;
void blyt_cart_init(void)   { }
void blyt_cart_update(void) {
    /* Leave frm=3 (round toward +infinity) dirty at frame boundary.
     * Write to fcsr (CSR 0x003) not frm (CSR 0x002): rv32emu maps CSR_FCSR
     * but not CSR_FRM, so csrwi frm,3 is a silent no-op on the emulated path.
     * 3u<<5 = 0x60 sets FCSR.frm=3 while leaving FCSR.fflags=0. */
    unsigned int fcsr_val = 3u << 5;
    __asm__ volatile("csrw fcsr, %0" : : "r"(fcsr_val));
    if (++s_frame >= 2) blyt_quit();
}
void blyt_cart_draw(void)   { }
"#,
    );
    let dirty_cart = build_cart(&dirty_project);
    assert!(
        dirty_cart.exists(),
        "dirty_frm.blyt not built: {}",
        dirty_cart.display()
    );

    // ── Build Lua and hybrid carts (gates 7 & 8) ──────────────────────
    let lua_sdk_native = sdk_dir().join("lib/native/libblyt32lua.so");
    let have_lua_gate = lua_sdk_native.exists() && has_luac();

    let lua_cart = if have_lua_gate {
        let project = tmp.path().join("lua_metal");
        CartProject::new()
            .lua(
                r#"
function init()
    blyt32.debug.print("lua-metal-ok")
end
function update() blyt.quit() end
function draw() end
"#,
            )
            .write(&project);
        Some(build_cart(&project))
    } else {
        None
    };

    let hybrid_cart = if have_lua_gate {
        let project = tmp.path().join("hybrid_metal");
        CartProject::new()
            .lib_file(
                "mathlib",
                "mathlib.c",
                "#include \"blyt.h\"\n\
                 BLYT_LUA_MODULE_EXPORT_I32(mathlib, double, int32_t x) { return x * 2; }\n",
            )
            .lua(
                r#"
function init()
    local m = require("mathlib")
    if m.double(21) == 42 then
        blyt32.debug.print("lua+c metal ok")
    else
        blyt32.debug.print("lua+c metal wrong")
    end
end
function update() blyt.quit() end
function draw() end
"#,
            )
            .write(&project);
        Some(build_cart(&project))
    } else {
        None
    };

    // ── Build the gfx carts (gates 16 & 17, #188 / Spike X) ───────────
    // Two probes drawing the SAME frames the emulated legs draw: the torture
    // frame via the gfx primitives (which rasterize natively on bare metal, no
    // ECALL), and the acquire/present raw-write path.  Both must emit the SAME
    // `[blyt:fbhash]` golden the host/wasm/libretro legs produce — the
    // bare-metal Q2 proof that the RV32-compiled rasterizer matches.
    let gfx_cart = |name: &str, draw_body: String| -> PathBuf {
        let project = tmp.path().join(name);
        let src = format!(
            "#include \"blyt.h\"\n\
             void blyt_cart_init(void) {{}}\n\
             void blyt_cart_update(void) {{ blyt_quit(); }}\n\
             void blyt_cart_draw(void) {{\n{draw_body}}}\n"
        );
        write_c_cart_project(&project, &src);
        build_cart(&project)
    };
    let gfx_torture_cart = gfx_cart(
        "gfx_torture",
        common::gfx::c_draw_body(&common::gfx::torture_frame()),
    );
    let gfx_raw_cart = gfx_cart("gfx_raw", common::gfx::raw_present_c_draw());

    // ── Build the Lua and hybrid gfx carts (gates 20 & 21, #193) ──────
    // Fill the bare-metal cells of the gfx execution matrix the spike left
    // untested: a pure-Lua gfx cart (native RV32 Lua VM → blyt32lua.c binding →
    // native libblyt32 primitives → native framebuffer → weak-hook hash), and a
    // hybrid cart whose torture frame is split across the Lua and native-C
    // halves.  Both draw the SAME torture frame and must emit the SAME golden as
    // every emulated leg — on metal the whole cart shares one framebuffer, so
    // this is the low-risk single-buffer end of the #193 coherence proof.
    let gfx_lua_cart = if have_lua_gate {
        let project = tmp.path().join("gfx_lua");
        CartProject::new()
            .lua(&format!(
                "function init() end\n\
                 function update() blyt.quit() end\n\
                 function draw()\n{}end\n",
                common::gfx::lua_draw_body(&common::gfx::torture_frame())
            ))
            .write(&project);
        Some(build_cart(&project))
    } else {
        None
    };
    let gfx_hybrid_cart = if have_lua_gate {
        // Native half draws the clear + rects + first pixel; Lua draws the rest.
        let ops = common::gfx::torture_frame();
        let (native_ops, lua_ops) = ops.split_at(8);
        let project = tmp.path().join("gfx_hybrid");
        CartProject::new()
            .lib_file(
                "gfxlib",
                "gfxlib.c",
                &format!(
                    "#include \"blyt.h\"\n\
                     BLYT_LUA_MODULE_EXPORT_VOID(gfxlib, native_draw) {{\n{}}}\n",
                    common::gfx::c_draw_body(native_ops)
                ),
            )
            .lua(&format!(
                "local g = require(\"gfxlib\")\n\
                 function init() end\n\
                 function update() blyt.quit() end\n\
                 function draw()\n  g.native_draw()\n{}end\n",
                common::gfx::lua_draw_body(lua_ops)
            ))
            .write(&project);
        Some(build_cart(&project))
    } else {
        None
    };

    // ── Build the surface carts (gates 22–24, #205) ───────────────────
    // Exercise the generalized surface API on bare metal: off-screen create +
    // tier-1 draw + blit-to-screen, tier-2 acquire/release on the screen, and a
    // raw per-pixel round-trip through an off-screen lock.  The native pool
    // hands a tier-2 lock the canonical buffer pointer directly (one address
    // space, no copy), so these prove the direct-pointer path hashes identically
    // to the copy-in/out emulated legs and the golden reference rasterizer.
    const SURF_SW: i32 = 64;
    const SURF_SH: i32 = 48;
    const SURF_BX: i32 = 100;
    const SURF_BY: i32 = 80;
    let surf_ops = [
        common::gfx::Op::Clear(5),
        common::gfx::Op::Rect(10, 10, 20, 15, 9),
        common::gfx::Op::Line(0, 0, 63, 47, 12),
    ];
    let surf_bg = 3u8;

    let surface_offscreen_cart = {
        let mut body = format!("  blyt_surface_h s = blyt_surface_create({SURF_SW}, {SURF_SH});\n");
        body += &common::gfx::c_surface_draw_body(&surf_ops, "s");
        body += &format!("  blyt_surface_clear(BLYT_SCREEN, {surf_bg});\n");
        body += &format!("  blyt_surface_blit(BLYT_SCREEN, s, {SURF_BX}, {SURF_BY});\n");
        gfx_cart("surface_offscreen", body)
    };
    let surface_offscreen_golden = {
        let src = common::gfx::render_dims(&surf_ops, SURF_SW as usize, SURF_SH as usize);
        let mut screen = vec![surf_bg; common::gfx::FRAME_W * common::gfx::FRAME_H];
        common::gfx::blit(
            &mut screen,
            common::gfx::FRAME_W,
            common::gfx::FRAME_H,
            &src,
            SURF_SW as usize,
            SURF_SH as usize,
            SURF_BX,
            SURF_BY,
        );
        common::gfx::expected_hash_line(&screen)
    };

    // Tier-2 lock on the screen drawing the torture frame — must equal the
    // tier-1 golden (tier-1 ≡ tier-2 by construction, direct-pointer path).
    let surface_lock_cart = gfx_cart(
        "surface_lock",
        common::gfx::c_lock_draw_body(&common::gfx::torture_frame(), "BLYT_SCREEN", "lk"),
    );
    let surface_lock_golden =
        common::gfx::expected_hash_line(&common::gfx::render(&common::gfx::torture_frame()));

    // Raw per-pixel round-trip through an off-screen lock, then blit to screen.
    let surface_raw_cart = {
        const RW: i32 = 40;
        const RH: i32 = 30;
        let mut body = format!("  blyt_surface_clear(BLYT_SCREEN, {surf_bg});\n");
        body += &format!("  blyt_surface_h s = blyt_surface_create({RW}, {RH});\n");
        body += "  blyt_lock_t lk;\n  blyt_surface_acquire(s, &lk);\n";
        body += "  for (int yy = 0; yy < lk.h; yy++)\n";
        body += "    for (int xx = 0; xx < lk.w; xx++)\n";
        body += "      lk.pixels[(long)yy * lk.stride + xx] = (unsigned char)((xx * 7 + yy * 13) & 0xff);\n";
        body += "  blyt_surface_release(&lk);\n";
        body += "  blyt_surface_blit(BLYT_SCREEN, s, 20, 15);\n";
        gfx_cart("surface_raw", body)
    };
    let surface_raw_golden = {
        const RW: i32 = 40;
        const RH: i32 = 30;
        let mut src = vec![0u8; (RW * RH) as usize];
        for yy in 0..RH {
            for xx in 0..RW {
                src[(yy * RW + xx) as usize] = ((xx * 7 + yy * 13) & 0xff) as u8;
            }
        }
        let mut screen = vec![surf_bg; common::gfx::FRAME_W * common::gfx::FRAME_H];
        common::gfx::blit(
            &mut screen,
            common::gfx::FRAME_W,
            common::gfx::FRAME_H,
            &src,
            RW as usize,
            RH as usize,
            20,
            15,
        );
        common::gfx::expected_hash_line(&screen)
    };

    // Exclusive-lock rejection carts (gates 26–27, #207).  On bare metal the
    // tier-2 lock exposes the canonical buffer *directly* (no copy), so without
    // the fix a handle-path op on a locked surface hits the same buffer the lock
    // holds and its write is *kept* — whereas the emulated legs copy-in/out and
    // the release flush *loses* it.  These carts pin that the native path rejects
    // the op, matching the emulated golden bit-for-bit.
    //
    // Gate 26: a blit reading a locked surface as its SRC.  Create s, acquire it,
    // fill the lock buffer with 7, blit s→screen while locked (must be rejected),
    // release.  Golden = pure background: s never reaches the screen.
    let surface_locked_blit_cart = {
        let mut body = format!("  blyt_surface_clear(BLYT_SCREEN, {surf_bg});\n");
        body += "  blyt_surface_h s = blyt_surface_create(64, 48);\n";
        body += "  blyt_lock_t lk;\n  blyt_surface_acquire(s, &lk);\n";
        body += "  blyt_raster_clear(lk.pixels, lk.stride, lk.w, lk.h, 7);\n";
        body += "  blyt_surface_blit(BLYT_SCREEN, s, 100, 80); /* s locked -> rejected */\n";
        body += "  blyt_surface_release(&lk);\n";
        gfx_cart("surface_locked_blit", body)
    };
    let surface_locked_blit_golden = common::gfx::expected_hash_line(&vec![
            surf_bg;
            common::gfx::FRAME_W * common::gfx::FRAME_H
        ]);

    // Gate 27: a tier-1 op on the locked surface itself.  Acquire the screen, fill
    // the lock buffer with 9, issue tier-1 clear(SCREEN, 4) while locked (must be
    // rejected), release (flushes 9).  Golden = uniform 9: the stray 4 never lands.
    // This is the native-discriminated case — the emulated flush masks the 4 either
    // way, but a bare-metal cart without the fix keeps it.
    let surface_locked_tier1_cart = {
        let mut body =
            String::from("  blyt_lock_t lk;\n  blyt_surface_acquire(BLYT_SCREEN, &lk);\n");
        body += "  blyt_raster_clear(lk.pixels, lk.stride, lk.w, lk.h, 9);\n";
        body += "  blyt_surface_clear(BLYT_SCREEN, 4); /* locked -> rejected */\n";
        body += "  blyt_surface_release(&lk);\n";
        gfx_cart("surface_locked_tier1", body)
    };
    let surface_locked_tier1_golden =
        common::gfx::expected_hash_line(&vec![9u8; common::gfx::FRAME_W * common::gfx::FRAME_H]);

    // Lua off-screen surface cart (gate 25, #205): the native RV32 Lua VM →
    // blyt32.surface.* binding → native surface pool → framebuffer path.  Same
    // frame + golden as the C off-screen cart (surface_offscreen_golden).
    let surface_lua_cart = if have_lua_gate {
        let mut draw = format!("  local s = blyt32.surface.create({SURF_SW}, {SURF_SH})\n");
        draw += &common::gfx::lua_surface_draw_body(&surf_ops, "s");
        draw += &format!("  blyt32.surface.clear(blyt32.surface.SCREEN, {surf_bg})\n");
        draw += &format!("  blyt32.surface.blit(blyt32.surface.SCREEN, s, {SURF_BX}, {SURF_BY})\n");
        let project = tmp.path().join("surface_lua");
        CartProject::new()
            .lua(&format!(
                "function init() end\n\
                 function update() blyt.quit() end\n\
                 function draw()\n{draw}end\n"
            ))
            .write(&project);
        Some(build_cart(&project))
    } else {
        None
    };

    // ── Start QEMU ────────────────────────────────────────────────────
    let ssh_port = free_port();
    println!("Starting QEMU (SSH port {ssh_port})...");
    let qemu = Qemu::start(&kernel, &rootfs, ssh_port, &ssh_key);

    assert!(
        qemu.wait_for_ssh(Duration::from_secs(300)),
        "QEMU SSH not available within 5 minutes"
    );

    // ── Stage binary artifacts ────────────────────────────────────────
    //
    // blyt_native is cross-compiled for riscv64 on the CI host; no build tools
    // are needed inside the VM.  The VM only needs sshd and /lib/ld-blyt.so.1.
    assert!(qemu.ssh_ok("mkdir -p /tmp/blyt_gate/native"));

    // Cross-compiled LP64 launcher.
    let launcher = repo_root().join("build-riscv64/blyt_native");
    assert!(
        launcher.exists(),
        "blyt_native riscv64 binary not found at {} — \
         run: cmake -B build-riscv64 -DCMAKE_TOOLCHAIN_FILE=cmake/riscv64-linux-gnu.cmake \
                    -DBLYT_PREGENERATED_DIR=$PWD/build/generated/flatbuffers && \
              cmake --build build-riscv64 --target blyt_native",
        launcher.display()
    );
    assert!(
        qemu.scp_to(&launcher, "/tmp/blyt_gate/"),
        "scp blyt_native failed"
    );
    qemu.ssh_ok("chmod +x /tmp/blyt_gate/blyt_native");

    // Test binaries: cross-compiled on the host via libblyt32_native_so target.
    let test_bin_dir = build_dir().join("test-rv32");
    let seccomp_test = test_bin_dir.join("seccomp_restricted_test");
    let fcsr_debug = test_bin_dir.join("fcsr_debug_test");
    let fcsr_release = test_bin_dir.join("fcsr_release_test");
    for bin in [&seccomp_test, &fcsr_debug, &fcsr_release] {
        assert!(
            bin.exists(),
            "{} not found — rebuild: cmake --build build --target libblyt32_native_so",
            bin.display()
        );
        assert!(
            qemu.scp_to(bin, "/tmp/blyt_gate/"),
            "scp {} failed",
            bin.display()
        );
    }
    qemu.ssh_ok(
        "chmod +x /tmp/blyt_gate/seccomp_restricted_test \
         /tmp/blyt_gate/fcsr_debug_test /tmp/blyt_gate/fcsr_release_test",
    );

    // Native RV32 runtime libraries.  libblytc.so is the thin wrapper that
    // DT_NEEDs ld-blyt.so.1 (system musl) so carts can resolve stdlib symbols.
    for lib in ["libblyt32.so", "libblytcommon.so", "libblytc.so"] {
        let p = native_dir.join(lib);
        if p.exists() {
            assert!(
                qemu.scp_to(&p, "/tmp/blyt_gate/native/"),
                "scp {lib} failed"
            );
        }
    }

    // Lua runtime library (optional; gates 7 & 8 are skipped when absent).
    // libblyt32lua.so embeds the Lua VM and has DT_NEEDED: libblyt32.so only;
    // no separate libblytcommonlua.so is needed at runtime.
    if have_lua_gate {
        assert!(
            qemu.scp_to(&lua_sdk_native, "/tmp/blyt_gate/native/"),
            "scp libblyt32lua.so failed"
        );
    }

    // Carts.
    assert!(
        qemu.scp_to(&hello_cart, "/tmp/blyt_gate/"),
        "scp hello.blyt failed"
    );
    assert!(
        qemu.scp_to(&dirty_cart, "/tmp/blyt_gate/"),
        "scp dirty_frm.blyt failed"
    );
    if let Some(ref cart) = lua_cart {
        assert!(
            qemu.scp_to(cart, "/tmp/blyt_gate/"),
            "scp lua_metal.blyt failed"
        );
    }
    if let Some(ref cart) = hybrid_cart {
        assert!(
            qemu.scp_to(cart, "/tmp/blyt_gate/"),
            "scp hybrid_metal.blyt failed"
        );
    }
    assert!(
        qemu.scp_to(&gfx_torture_cart, "/tmp/blyt_gate/"),
        "scp gfx_torture.blyt failed"
    );
    assert!(
        qemu.scp_to(&gfx_raw_cart, "/tmp/blyt_gate/"),
        "scp gfx_raw.blyt failed"
    );
    if let Some(ref cart) = gfx_lua_cart {
        assert!(
            qemu.scp_to(cart, "/tmp/blyt_gate/"),
            "scp gfx_lua.blyt failed"
        );
    }
    if let Some(ref cart) = gfx_hybrid_cart {
        assert!(
            qemu.scp_to(cart, "/tmp/blyt_gate/"),
            "scp gfx_hybrid.blyt failed"
        );
    }
    assert!(
        qemu.scp_to(&surface_offscreen_cart, "/tmp/blyt_gate/"),
        "scp surface_offscreen.blyt failed"
    );
    assert!(
        qemu.scp_to(&surface_lock_cart, "/tmp/blyt_gate/"),
        "scp surface_lock.blyt failed"
    );
    assert!(
        qemu.scp_to(&surface_raw_cart, "/tmp/blyt_gate/"),
        "scp surface_raw.blyt failed"
    );
    if let Some(ref cart) = surface_lua_cart {
        assert!(
            qemu.scp_to(cart, "/tmp/blyt_gate/"),
            "scp surface_lua.blyt failed"
        );
    }
    assert!(
        qemu.scp_to(&surface_locked_blit_cart, "/tmp/blyt_gate/"),
        "scp surface_locked_blit.blyt failed"
    );
    assert!(
        qemu.scp_to(&surface_locked_tier1_cart, "/tmp/blyt_gate/"),
        "scp surface_locked_tier1.blyt failed"
    );

    // ── Diagnostics ───────────────────────────────────────────────────
    // Print environment info before the gate test to help diagnose failures.
    for cmd in [
        "ls -la /tmp/blyt_gate/native/",
        // Confirm libblytcommon.so is in DT_NEEDED of native libblyt32.so.
        "strings /tmp/blyt_gate/native/libblyt32.so | grep libblytcommon || echo 'MISSING: libblytcommon not in DT_NEEDED'",
        // Check e_phnum in hello.blyt: expect 10 (phdr+interp+4xload+dynamic+relro+stack+attr)
        // with the PIE linker script (ENTRY-only, lld default layout).
        "od -j44 -N2 -tu2 -An /tmp/blyt_gate/hello.blyt | tr -d ' \\n'",
        "/lib/ld-blyt.so.1 --version 2>&1 | head -2",
        "/tmp/blyt_gate/blyt_native --version 2>&1",
        "/tmp/blyt_gate/blyt_native --no-seccomp \
         --lib-dir /tmp/blyt_gate/native \
         -- /tmp/blyt_gate/hello.blyt 2>&1",
    ] {
        let o = qemu.ssh(cmd);
        println!(
            "  $ {cmd}\n    rc={:?} out={}",
            o.status.code(),
            String::from_utf8_lossy(&o.stdout).trim()
        );
    }

    // ── Gate 1: blyt_console_debug works via write(2) ─────────────────
    println!("Gate 1: blyt_console_debug via write(2)...");

    let out = qemu.ssh(
        "/tmp/blyt_gate/blyt_native \
         --lib-dir /tmp/blyt_gate/native \
         -- /tmp/blyt_gate/hello.blyt 2>&1",
    );
    let stdout = String::from_utf8_lossy(&out.stdout);

    assert!(
        out.status.success(),
        "hello.blyt exited non-zero ({:?})\noutput: {stdout}",
        out.status.code()
    );
    assert!(
        stdout.contains("init"),
        "expected 'init' in output — blyt_console_debug not working\noutput: {stdout}"
    );
    assert!(
        stdout.contains("update"),
        "expected 'update' in output\noutput: {stdout}"
    );
    assert!(
        stdout.contains("draw"),
        "expected 'draw' in output\noutput: {stdout}"
    );
    println!("  PASS: output = {:?}", stdout.trim());

    // ── Gate 2: restricted seccomp blocks socket(2) ──────────────────────
    println!("Gate 2: restricted seccomp blocks socket(2)...");
    let out2 = qemu.ssh("/tmp/blyt_gate/seccomp_restricted_test 2>&1; echo __exit:$?");
    let out2_txt = String::from_utf8_lossy(&out2.stdout);
    // macOS OpenSSH returns 255 (not 128+31=159) for SIGSYS-killed processes,
    // so we read the exit code from the shell's echo instead of out2.status.
    let inner_rc: i32 = out2_txt
        .lines()
        .find(|l| l.starts_with("__exit:"))
        .and_then(|l| l.trim_start_matches("__exit:").parse().ok())
        .unwrap_or(-1);
    assert_eq!(
        inner_rc, 159,
        "expected exit 159 (SIGSYS) from seccomp_restricted_test, got {inner_rc}\noutput: {out2_txt}"
    );
    println!("  PASS: exit {inner_rc} (SIGSYS)");

    // ── Gate 3: FCSR clean — no spurious warning ──────────────────────
    //
    // The hello cart does not touch frm; blyt_frame_done() must not emit
    // a rounding-mode warning when the cart leaves FCSR clean.
    println!("Gate 3: FCSR clean — no spurious warning...");
    {
        let out = qemu.ssh(
            "/tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/hello.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "hello.blyt exited non-zero\noutput: {output}"
        );
        assert!(
            !output.contains("non-default FP rounding mode"),
            "unexpected FCSR warning from clean cart\noutput: {output}"
        );
        println!("  PASS: no FCSR warning in clean-cart output");
    }

    // ── Gate 4: FCSR dirty frm — debug warning via libblyt32.so ──────
    //
    // The dirty-frm cart leaves frm=3 at every frame boundary.  The debug
    // build of libblyt32.so must emit a warning and still exit cleanly.
    println!("Gate 4: FCSR dirty frm — debug warning (via libblyt32.so)...");
    {
        let out = qemu.ssh(
            "/tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/dirty_frm.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "dirty_frm.blyt exited non-zero in debug mode ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains("non-default FP rounding mode"),
            "expected FCSR warning in debug output\noutput: {output}"
        );
        println!("  PASS: warning present, exit 0");
    }

    // ── Gates 5 & 6: FCSR standalone binary (debug + release) ────────
    println!("Gate 5: FCSR standalone — debug warn (no NDEBUG)...");
    let out5 = qemu.ssh("/tmp/blyt_gate/fcsr_debug_test 2>&1");
    let output5 = String::from_utf8_lossy(&out5.stdout);
    assert!(
        out5.status.success(),
        "fcsr_debug_test exited non-zero ({:?})\noutput: {output5}",
        out5.status.code()
    );
    assert!(
        output5.contains("non-default FP rounding mode"),
        "expected FCSR warning in debug output\noutput: {output5}"
    );
    println!("  PASS: warning present, exit 0");

    println!("Gate 6: FCSR standalone — release abort (-DNDEBUG)...");
    let out6 = qemu.ssh("/tmp/blyt_gate/fcsr_release_test 2>&1");
    let output6 = String::from_utf8_lossy(&out6.stdout);
    assert_eq!(
        out6.status.code().unwrap_or(-1),
        1,
        "expected exit 1 from fcsr_release_test, got {:?}\noutput: {output6}",
        out6.status.code()
    );
    assert!(
        output6.contains("aborting for determinism"),
        "expected abort message in release output\noutput: {output6}"
    );
    println!("  PASS: abort message present, exit 1");

    // ── Gate 7: pure Lua cart ─────────────────────────────────────────
    if have_lua_gate {
        println!("Gate 7: pure Lua cart on metal...");
        let out = qemu.ssh(
            "/tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/lua_metal.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "lua_metal.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains("lua-metal-ok"),
            "expected 'lua-metal-ok' in output\noutput: {output}"
        );
        println!("  PASS: output = {:?}", output.trim());
    } else {
        println!("Gate 7: SKIP (libblyt32lua.so not available or luac not found)");
    }

    // ── Gate 8: hybrid Lua + C lib cart ──────────────────────────────
    if have_lua_gate {
        println!("Gate 8: hybrid Lua+C cart on metal...");
        let out = qemu.ssh(
            "/tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/hybrid_metal.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "hybrid_metal.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains("lua+c metal ok"),
            "expected 'lua+c metal ok' in output\noutput: {output}"
        );
        println!("  PASS: output = {:?}", output.trim());
    } else {
        println!("Gate 8: SKIP (libblyt32lua.so not available or luac not found)");
    }

    // ── Gate 9: state buffer save/load round-trip on metal ───────────
    //
    // Verifies the full native save path: alloc two slots in a mixed-width
    // record (i32/i8/u16/f64), write distinct values, save (blyt_save_write),
    // clobber, reload (blyt_save_read), read back, confirm every field in both
    // slots.  The mixed widths + 2 slots exercise the field-major true-width
    // packing and per-field save layout (#134) — a single i32 field would not
    // catch a wrong stride or width.  BLYT_SAVE_DIR points to a tmpdir on the VM.
    println!("Gate 9: state buffer save/load round-trip on metal...");
    {
        let sb_project = tmp.path().join("sb_metal");
        CartProject::new()
            .config(
                "save_version: 9\n\
                 records:\n  Game:\n    fields:\n\
                 \x20     - { name: score, type: i32 }\n\
                 \x20     - { name: lives, type: i8 }\n\
                 \x20     - { name: level, type: u16 }\n\
                 \x20     - { name: ratio, type: f64 }\n\
                 state_buffers:\n  game:\n    record: Game\n    count: 2\n",
            )
            .c(r#"
#include "blyt.h"
#include "cart_state.h"

static int32_t g_a = -1, g_b = -1;
static int g_phase = 0;

static void set_slot(int32_t slot, int32_t score, int8_t lives, uint16_t level, double ratio) {
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, score);
    blyt_buffer_set_i8(S_GAME, slot, S_GAME_LIVES, lives);
    blyt_buffer_set_u16(S_GAME, slot, S_GAME_LEVEL, level);
    blyt_buffer_set_f64(S_GAME, slot, S_GAME_RATIO, ratio);
}

void blyt_cart_init(void) {
    blyt_buffer_alloc_slot(S_GAME, &g_a);
    blyt_buffer_alloc_slot(S_GAME, &g_b);
    blyt_console_debug("sb-init");
}
/* save_read fires on_load_state with the version that wrote the save, read
 * from the .blys header (ADR-0125/0087) — here the cart's own save_version 9. */
void blyt_cart_on_load_state(blyt_load_info_t info) {
    if (info.saved_cart_version == 9) {
        blyt_console_debug("version-ok");
    } else {
        blyt_console_debug("version-fail");
    }
}
void blyt_cart_update(void) {
    if (g_phase == 0) {
        set_slot(g_a, 42, -5, 1000, 3.5);
        set_slot(g_b, 777, 100, 65000, -2.25);
        blyt_save_write(0);
        set_slot(g_a, 0, 0, 0, 0.0); /* clobber both slots, every field */
        set_slot(g_b, 0, 0, 0, 0.0);
        g_phase = 1;
    } else {
        blyt_save_read(0);
        if (blyt_buffer_get_i32(S_GAME, g_a, S_GAME_SCORE) == 42) {
            blyt_console_debug("score-ok");
        } else {
            blyt_console_debug("score-fail");
        }
        int mixed_ok = blyt_buffer_get_i8(S_GAME, g_a, S_GAME_LIVES) == -5 &&
                       blyt_buffer_get_u16(S_GAME, g_a, S_GAME_LEVEL) == 1000 &&
                       blyt_buffer_get_f64(S_GAME, g_a, S_GAME_RATIO) == 3.5 &&
                       blyt_buffer_get_i32(S_GAME, g_b, S_GAME_SCORE) == 777 &&
                       blyt_buffer_get_i8(S_GAME, g_b, S_GAME_LIVES) == 100 &&
                       blyt_buffer_get_u16(S_GAME, g_b, S_GAME_LEVEL) == 65000 &&
                       blyt_buffer_get_f64(S_GAME, g_b, S_GAME_RATIO) == -2.25;
        if (mixed_ok) {
            blyt_console_debug("mixed-ok");
        } else {
            blyt_console_debug("mixed-fail");
        }
        blyt_quit();
    }
}
void blyt_cart_draw(void) {}
"#)
            .write(&sb_project);
        let sb_cart = build_cart(&sb_project);
        assert!(
            sb_cart.exists(),
            "sb_metal.blyt not built: {}",
            sb_cart.display()
        );

        assert!(
            qemu.scp_to(&sb_cart, "/tmp/blyt_gate/"),
            "scp sb_metal.blyt failed"
        );

        // Pre-create the save directory; the cart no longer calls mkdirat
        // (ADR-0131: launcher responsibility, mkdirat removed from seccomp).
        assert!(qemu.ssh_ok("mkdir -p /tmp/blyt_save_gate"));

        let out = qemu.ssh(
            "BLYT_SAVE_DIR=/tmp/blyt_save_gate \
             /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/sb_metal.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "sb_metal.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains("sb-init"),
            "expected 'sb-init' in output\noutput: {output}"
        );
        assert!(
            output.contains("score-ok"),
            "expected 'score-ok' in output (save/load round-trip)\noutput: {output}"
        );
        assert!(
            output.contains("mixed-ok"),
            "expected 'mixed-ok' (i8/u16/f64 fields + 2 slots round-trip through the \
             field-major true-width save layout; issue #134)\noutput: {output}"
        );
        assert!(
            output.contains("version-ok"),
            "expected 'version-ok' (saved_cart_version 9 from .cart.config stamped \
             into and read back from the native save header; issue #112)\noutput: {output}"
        );
        println!("  PASS: output = {:?}", output.trim());
    }

    // ── Gate 10: entity refs + generation counters on metal ──────────
    //
    // The only coverage of the native libblyt32 generation/ref code (ADR-0096)
    // and its save round-trip: lifecycle (alloc/ref/free/stale/realloc),
    // free-slot field zeroing parity, and gens round-tripping through
    // blyt_save_write/blyt_save_read.
    println!("Gate 10: entity refs + generation counters on metal...");
    {
        let ref_project = tmp.path().join("ref_metal");
        CartProject::new()
            .config(
                "records:
  Globals:
    fields:
      - { name: target, ref: entity }
  Entity:
    fields:
      - { name: hp, type: i32 }
state_buffers:
  globals:
    record: Globals
    count: 1
  entity:
    record: Entity
    count: 4
",
            )
            .c(r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {}

/* On failure prints "ref-metal-fail s<stage>" so the gate log shows which
 * check diverged. */
static int s_stage = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        s_stage++;                                                                                 \
        if (!(cond)) {                                                                             \
            char msg[32] = "ref-metal-fail s";                                                     \
            msg[16] = (char)('0' + s_stage / 10);                                                  \
            msg[17] = (char)('0' + s_stage % 10);                                                  \
            msg[18] = '\0';                                                                        \
            blyt_console_debug(msg);                                                               \
            blyt_quit();                                                                           \
            return;                                                                                \
        }                                                                                          \
    } while (0)

void blyt_cart_update(void) {
    int32_t g = -1, e = -1;
    blyt_buffer_alloc_slot(S_GLOBALS, &g);
    blyt_buffer_alloc_slot(S_ENTITY, &e);
    blyt_buffer_set_i32(S_ENTITY, e, S_ENTITY_HP, 7);

    /* lifecycle: gen starts at 1; free goes stale; realloc stays stale */
    blyt_entity_ref_t r = blyt_buffer_ref(S_ENTITY, e);
    CHECK(r == ((1u << 16) | (uint32_t)e));        /* s1 */
    CHECK(blyt_buffer_ref_valid(S_ENTITY, r));     /* s2 */
    CHECK(blyt_buffer_ref_slot(r) == e);           /* s3 */
    blyt_buffer_free_slot(S_ENTITY, e);
    CHECK(!blyt_buffer_ref_valid(S_ENTITY, r));    /* s4 */
    blyt_buffer_alloc_slot(S_ENTITY, &e);
    CHECK(!blyt_buffer_ref_valid(S_ENTITY, r));    /* s5 */
    /* freed slot's field data was zeroed (host parity) */
    CHECK(blyt_buffer_get_i32(S_ENTITY, e, S_ENTITY_HP) == 0); /* s6 */
    blyt_entity_ref_t r2 = blyt_buffer_ref(S_ENTITY, e);       /* gen 2 */
    CHECK(r2 == ((2u << 16) | (uint32_t)e));       /* s7 */

    /* gens round-trip through the BLYS save format */
    blyt_buffer_set_u32(S_GLOBALS, 0, S_GLOBALS_TARGET, r2);
    blyt_save_write(0);
    blyt_buffer_free_slot(S_ENTITY, e); /* gen 3; r2 stale */
    CHECK(!blyt_buffer_ref_valid(S_ENTITY, r2));   /* s8 */
    blyt_save_read(0);
    blyt_entity_ref_t restored = blyt_buffer_get_u32(S_GLOBALS, 0, S_GLOBALS_TARGET);
    CHECK(restored == r2);                         /* s9 */
    CHECK(blyt_buffer_ref_valid(S_ENTITY, restored)); /* s10 */

    /* s11-s13: fill the remaining entity slots (count: 4, one already used) */
    {
        int32_t extra_count = 0;
        for (;;) {
            int32_t slot = -1;
            if (blyt_buffer_alloc_slot(S_ENTITY, &slot) != BLYT_OK)
                break;
            extra_count++;
        }
        /* 1 slot occupied (e=0 restored by save_read); 3 more fill count=4 */
        CHECK(extra_count == 3);                 /* s11 */
        blyt_console_debug("alloc_limit: 4\n");
    }

    blyt_console_debug("ref-metal-ok");
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#)
            .write(&ref_project);
        let ref_cart = build_cart(&ref_project);
        assert!(
            ref_cart.exists(),
            "ref_metal.blyt not built: {}",
            ref_cart.display()
        );

        assert!(
            qemu.scp_to(&ref_cart, "/tmp/blyt_gate/"),
            "scp ref_metal.blyt failed"
        );
        assert!(qemu.ssh_ok("mkdir -p /tmp/blyt_save_gate_ref"));

        let out = qemu.ssh(
            "BLYT_SAVE_DIR=/tmp/blyt_save_gate_ref BLYT_TRACE=api \
             /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/ref_metal.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "ref_metal.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains("ref-metal-ok"),
            "expected 'ref-metal-ok' in output (entity refs on metal)\noutput: {output}"
        );
        assert!(
            output.contains("alloc_limit: 4"),
            "expected 'alloc_limit: 4' in output (slot count enforced on metal)\noutput: {output}"
        );
        println!("  PASS: output = {:?}", output.trim());
    }

    // ── Gate 11: cross-platform BLYS save contract (#129) ─────────────
    //
    // The native and emulated (host) runtimes must produce byte-identical BLYS
    // saves for the same cart + state, and each must load the other's save.
    // This is the contract the old ad-hoc native NLBY format broke; Gate 9 only
    // checked a native-only round-trip and could not catch the divergence.
    //
    // One cart drives both: on update it tries blyt_save_read(0); if a save is
    // present it verifies every field (mixed widths + a freed slot, so the SOA,
    // slot bitset and generation blobs are all exercised) and prints xload-ok;
    // otherwise it writes the canonical state and prints wrote.  Running it once
    // on each platform with an empty save dir yields the two saves to compare;
    // feeding each platform the other's save exercises the cross-load paths.
    println!("Gate 11: cross-platform BLYS byte-identity + cross-load...");
    {
        let xplat_project = tmp.path().join("xplat");
        CartProject::new()
            .config(
                "save_version: 5\n\
                 records:\n  Game:\n    fields:\n\
                 \x20     - { name: score, type: i32 }\n\
                 \x20     - { name: lives, type: i8 }\n\
                 \x20     - { name: level, type: u16 }\n\
                 \x20     - { name: ratio, type: f64 }\n\
                 \x20     - { name: alive, type: bool }\n\
                 state_buffers:\n  game:\n    record: Game\n    count: 4\n",
            )
            .c(r#"
#include "blyt.h"
#include "cart_state.h"

static int g_done = 0;

static void set_slot(int32_t slot, int32_t score, int8_t lives, uint16_t level, double ratio,
                     bool alive) {
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, score);
    blyt_buffer_set_i8(S_GAME, slot, S_GAME_LIVES, lives);
    blyt_buffer_set_u16(S_GAME, slot, S_GAME_LEVEL, level);
    blyt_buffer_set_f64(S_GAME, slot, S_GAME_RATIO, ratio);
    blyt_buffer_set_bool(S_GAME, slot, S_GAME_ALIVE, alive);
}

static int verify(void) {
    return blyt_buffer_get_i32(S_GAME, 0, S_GAME_SCORE) == 42 &&
           blyt_buffer_get_i8(S_GAME, 0, S_GAME_LIVES) == -5 &&
           blyt_buffer_get_u16(S_GAME, 0, S_GAME_LEVEL) == 1000 &&
           blyt_buffer_get_f64(S_GAME, 0, S_GAME_RATIO) == 3.5 &&
           blyt_buffer_get_bool(S_GAME, 0, S_GAME_ALIVE) == true &&
           blyt_buffer_get_i32(S_GAME, 2, S_GAME_SCORE) == 777 &&
           blyt_buffer_get_i8(S_GAME, 2, S_GAME_LIVES) == 100 &&
           blyt_buffer_get_u16(S_GAME, 2, S_GAME_LEVEL) == 65000 &&
           blyt_buffer_get_f64(S_GAME, 2, S_GAME_RATIO) == -2.25 &&
           blyt_buffer_get_bool(S_GAME, 2, S_GAME_ALIVE) == false;
}

void blyt_cart_init(void) {
    int32_t a = -1, b = -1, c = -1;
    blyt_buffer_alloc_slot(S_GAME, &a); /* 0 */
    blyt_buffer_alloc_slot(S_GAME, &b); /* 1 */
    blyt_buffer_alloc_slot(S_GAME, &c); /* 2 */
    blyt_buffer_free_slot(S_GAME, b); /* free 1 -> gen[1] bumped, bitset = 0,2 */
}
void blyt_cart_on_load_state(blyt_load_info_t info) { (void)info; }
void blyt_cart_update(void) {
    if (g_done)
        return;
    g_done = 1;
    if (blyt_save_read(0) == BLYT_OK) {
        blyt_console_debug(verify() ? "xload-ok" : "xload-fail");
    } else {
        set_slot(0, 42, -5, 1000, 3.5, true);
        set_slot(2, 777, 100, 65000, -2.25, false);
        blyt_save_write(0);
        blyt_console_debug("wrote");
    }
    blyt_quit();
}
void blyt_cart_draw(void) {}
"#)
            .write(&xplat_project);
        let xplat_cart = build_cart(&xplat_project);
        assert!(xplat_cart.exists(), "xplat.blyt not built");
        assert!(
            qemu.scp_to(&xplat_cart, "/tmp/blyt_gate/"),
            "scp xplat.blyt failed"
        );

        // Helper: run xplat on blytplay (host/emulated path) with a save dir.
        let run_host = |save_dir: &Path| -> String {
            let out = Command::new(common::blytplay())
                .args(["--headless", xplat_cart.to_str().unwrap()])
                .env("BLYT_SAVE_DIR", save_dir)
                .output()
                .expect("run blytplay");
            assert!(
                out.status.success(),
                "blytplay xplat exited non-zero ({:?})\n{}",
                out.status.code(),
                String::from_utf8_lossy(&out.stderr)
            );
            String::from_utf8_lossy(&out.stdout).into_owned()
        };

        // Host writes its BLYS save to <host_sd>/<cart_name>/slot_0.blys.
        let host_sd = tmp.path().join("xplat_host_sd");
        std::fs::create_dir_all(&host_sd).unwrap();
        let host_out = run_host(&host_sd);
        assert!(host_out.contains("wrote"), "host write leg: {host_out}");
        let host_path = find_file(&host_sd, "slot_0.blys").expect("host save not written");
        let host_bytes = std::fs::read(&host_path).unwrap();

        // Native writes its BLYS save to /tmp/blyt_xplat/slot_0.blys.
        assert!(qemu.ssh_ok("rm -rf /tmp/blyt_xplat && mkdir -p /tmp/blyt_xplat"));
        let nout = qemu.ssh(
            "BLYT_SAVE_DIR=/tmp/blyt_xplat \
             /tmp/blyt_gate/blyt_native --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/xplat.blyt 2>&1",
        );
        let nout_s = String::from_utf8_lossy(&nout.stdout);
        assert!(
            nout.status.success() && nout_s.contains("wrote"),
            "native write leg: {nout_s}"
        );
        let od = qemu.ssh("od -An -v -tx1 /tmp/blyt_xplat/slot_0.blys");
        assert!(od.status.success(), "od native save failed");
        let native_bytes: Vec<u8> = String::from_utf8_lossy(&od.stdout)
            .split_whitespace()
            .map(|h| u8::from_str_radix(h, 16).expect("hex byte"))
            .collect();

        // Byte-identity: the whole point of the format unification.
        assert_eq!(
            host_bytes.len(),
            native_bytes.len(),
            "BLYS size differs: host {} vs native {} bytes",
            host_bytes.len(),
            native_bytes.len()
        );
        assert_eq!(
            host_bytes, native_bytes,
            "host and native BLYS bytes differ for identical cart + state"
        );
        println!("  byte-identity: {} bytes match", host_bytes.len());

        // Cross-load host->native: feed the host-written save to the native runtime.
        assert!(qemu.ssh_ok("rm -rf /tmp/blyt_xload && mkdir -p /tmp/blyt_xload"));
        assert!(
            qemu.scp_to(&host_path, "/tmp/blyt_xload/"),
            "scp host save failed"
        );
        let xn = qemu.ssh(
            "BLYT_SAVE_DIR=/tmp/blyt_xload \
             /tmp/blyt_gate/blyt_native --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/xplat.blyt 2>&1",
        );
        let xn_s = String::from_utf8_lossy(&xn.stdout);
        assert!(
            xn.status.success() && xn_s.contains("xload-ok"),
            "host->native cross-load failed: {xn_s}"
        );
        println!("  cross-load host->native: xload-ok");

        // Cross-load native->host: feed the native-written save to the host runtime.
        std::fs::write(&host_path, &native_bytes).unwrap();
        let xh_out = run_host(&host_sd);
        assert!(
            xh_out.contains("xload-ok"),
            "native->host cross-load failed: {xh_out}"
        );
        println!("  cross-load native->host: xload-ok");
        println!("  PASS: BLYS saves are portable across host and native");
    }

    // ── Gate 12: resource pin on metal (#123, #196) ──────────────────
    //
    // The only coverage of the native libblytcommon resource path: a packed
    // cart's embedded `.cart.resource.<id>` section reaches the cart by
    // referencing the baked constant directly (ADR-0134), served via pin by a
    // direct pointer into the retained cart mmap (no host).  One deterministic
    // line encodes the result so the metal output can be matched against the
    // emulated legs': pin delivers the exact bytes+length.
    println!("Gate 12: resource pin on metal...");
    {
        let res_project = tmp.path().join("res_metal");
        CartProject::new()
            .asset("greeting.txt", "Hello from metal!")
            .c(r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>
#include <string.h>

void blyt_cart_init(void) {
    const void *ptr = NULL;
    size_t size = 0;
    blyt_result_t pr = blyt_resource_pin(R_GREETING, &ptr, &size);
    char g[64];
    size_t n = size < sizeof(g) - 1 ? size : sizeof(g) - 1;
    if (ptr) memcpy(g, ptr, n);
    g[ptr ? n : 0] = '\0';
    blyt_resource_unpin(R_GREETING);

    char line[256];
    snprintf(line, sizeof(line), "R[%s] pin=%d bytes=%d", g, (int)pr, (int)size);
    blyt_console_debug(line);
}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#)
            .write(&res_project);
        let res_cart = build_cart(&res_project);
        assert!(
            res_cart.exists(),
            "res_metal.blyt not built: {}",
            res_cart.display()
        );
        assert!(
            qemu.scp_to(&res_cart, "/tmp/blyt_gate/"),
            "scp res_metal.blyt failed"
        );

        let out = qemu.ssh(
            "/tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/res_metal.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "res_metal.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains("R[Hello from metal!] pin=0 bytes=18"),
            "expected the resource-pin line on metal (pin delivers exact bytes; \
             bytes=18 = the 17-char greeting + the build-appended trailing NUL the \
             byte-blind pin reports, #166)\noutput: {output}"
        );
        println!("  PASS: output = {:?}", output.trim());
    }

    // ── Gate 13: >64 distinct resources on metal (#141) ──────────────
    //
    // The native resource table historically had a fixed 64-entry cap
    // (NATIVE_MAX_RES), so a cart touching a 65th distinct id silently got
    // NOT_FOUND from pin/load on metal — a native-only divergence from the
    // emulated/WASM/libretro legs, whose table grows dynamically. This cart
    // pins AND loads all 100 distinct ids and verifies each pin delivers the
    // exact embedded bytes (not merely OK; the #98 anti-pattern), emitting one
    // deterministic summary line. Ids are 1-based in sorted-name order, so the
    // zero-padded res_<k>.dat asset maps id (k+1) -> "payload-<k>". Raw `.dat`
    // (not text) so the pinned size is exactly the payload length — a text
    // resource's build-appended trailing NUL (#166) would make size == elen+1.
    println!("Gate 13: >64 distinct resources on metal...");
    {
        let res_project = tmp.path().join("res_many_metal");
        let mut proj = CartProject::new().c(r#"
#include "blyt.h"
#include <stdio.h>
#include <string.h>

#define N 100
#define R_ENCODE(id) (0x20000000u | (blyt_resource_id_t)(id)) /* kind RESOURCE, prov cart */

void blyt_cart_init(void) {
    int pin_ok = 0, match = 0, first_bad = -1;
    for (int k = 0; k < N; k++) {
        blyt_resource_id_t id = R_ENCODE(k + 1);
        char expect[32];
        int elen = snprintf(expect, sizeof(expect), "payload-%03d", k);

        const void *ptr = NULL;
        size_t size = 0;
        blyt_result_t pr = blyt_resource_pin(id, &ptr, &size);
        if (pr == BLYT_OK && ptr) {
            pin_ok++;
            if ((int)size == elen && memcmp(ptr, expect, (size_t)elen) == 0)
                match++;
            else if (first_bad < 0)
                first_bad = k + 1;
        } else if (first_bad < 0) {
            first_bad = k + 1;
        }
        blyt_resource_unpin(id);
    }

    char line[128];
    snprintf(line, sizeof(line), "RES_MANY pin_ok=%d match=%d first_bad=%d", pin_ok, match,
             first_bad);
    blyt_console_debug(line);
}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#);
        for k in 0..100 {
            proj = proj.asset_bytes(
                &format!("res_{k:03}.dat"),
                format!("payload-{k:03}").as_bytes(),
            );
        }
        proj.write(&res_project);
        let res_cart = build_cart(&res_project);
        assert!(
            res_cart.exists(),
            "res_many_metal.blyt not built: {}",
            res_cart.display()
        );
        assert!(
            qemu.scp_to(&res_cart, "/tmp/blyt_gate/"),
            "scp res_many_metal.blyt failed"
        );

        let out = qemu.ssh(
            "/tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/res_many_metal.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "res_many_metal.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains("RES_MANY pin_ok=100 match=100 first_bad=-1"),
            "expected all 100 distinct resources to pin with exact bytes on metal \
             (no NATIVE_MAX_RES cap)\noutput: {output}"
        );
        println!("  PASS: output = {:?}", output.trim());
    }

    // ── Gate 14: zstd-compressed resource decodes on metal (#157) ────
    //
    // The only coverage of native bare-metal decompression: a highly
    // compressible resource packs zstd (8-byte header + frame), and the native
    // libblytcommon must decode it — lazily, post-seccomp (malloc works because
    // mmap is allowlisted) — into the exact original bytes. The cart pins it and
    // emits an FNV-1a over the full decoded content, so the value proves every
    // byte decoded correctly and matches the other legs (assets.rs), not merely
    // that pin returned OK (the #98 anti-pattern).
    println!("Gate 14: zstd-compressed resource decodes on metal...");
    {
        // Highly compressible blob → packs zstd; FNV-1a (32-bit) over its bytes.
        let blob: Vec<u8> = "blyt-resource-compression-test-payload\n"
            .repeat(512)
            .into_bytes();
        let mut fnv: u32 = 2166136261;
        for &b in &blob {
            fnv ^= b as u32;
            fnv = fnv.wrapping_mul(16777619);
        }
        let expected = format!("RES_ZSTD pin=0 size={} fnv={fnv:08x}", blob.len());

        let zstd_project = tmp.path().join("res_zstd_metal");
        CartProject::new()
            .c(r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>

void blyt_cart_init(void) {
    const void *ptr = NULL;
    size_t size = 0;
    blyt_result_t pr = blyt_resource_pin(R_BLOB, &ptr, &size);
    unsigned int h = 2166136261u;
    const unsigned char *b = (const unsigned char *)ptr;
    for (size_t i = 0; i < size; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    char line[96];
    snprintf(line, sizeof(line), "RES_ZSTD pin=%d size=%d fnv=%08x", (int)pr, (int)size, h);
    blyt_console_debug(line);
}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#)
            .asset_bytes("blob.dat", &blob)
            .write(&zstd_project);
        let zstd_cart = build_cart(&zstd_project);
        assert!(
            zstd_cart.exists(),
            "res_zstd_metal.blyt not built: {}",
            zstd_cart.display()
        );
        assert!(
            qemu.scp_to(&zstd_cart, "/tmp/blyt_gate/"),
            "scp res_zstd_metal.blyt failed"
        );

        let out = qemu.ssh(
            "/tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/res_zstd_metal.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "res_zstd_metal.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains(&expected),
            "native bare-metal must decode the zstd resource to the exact bytes \
             (expected {expected:?}; byte-identical to the other legs, #157)\noutput: {output}"
        );
        println!("  PASS: output = {:?}", output.trim());
    }

    // ── Gate 15: resource eviction + rehydration on metal (#137) ─────
    //
    // The only coverage of the native bare-metal evict/rehydrate path: with
    // BLYT_RESOURCE_EVICT_EVERY_FRAME=1 the runtime force-evicts every evictable
    // resource at each frame boundary, freeing the owned decompressed bytes. A
    // cart that re-pins the zstd resource each frame therefore rehydrates it from
    // the cart section (re-decode, post-seccomp) on every frame after the first —
    // the FNV-1a must stay byte-identical, proving rehydration yields the original
    // bytes and eviction is cart-invisible. The launcher execve's `environ`
    // through, so the env var reaches the cart process.
    println!("Gate 15: resource eviction + rehydration on metal...");
    {
        let blob: Vec<u8> = "blyt-resource-compression-test-payload\n"
            .repeat(512)
            .into_bytes();
        let mut fnv: u32 = 2166136261;
        for &b in &blob {
            fnv ^= b as u32;
            fnv = fnv.wrapping_mul(16777619);
        }
        let expected = format!("RES_EVICT pin=0 size={} fnv={fnv:08x}", blob.len());

        let evict_project = tmp.path().join("res_evict_metal");
        CartProject::new()
            .c(r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>

static int g_frames = 0;

static void read_and_print(void) {
    const void *ptr = NULL;
    size_t size = 0;
    blyt_result_t pr = blyt_resource_pin(R_BLOB, &ptr, &size);
    unsigned int h = 2166136261u;
    const unsigned char *b = (const unsigned char *)ptr;
    for (size_t i = 0; i < size; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    blyt_resource_unpin(R_BLOB);
    char line[96];
    snprintf(line, sizeof(line), "RES_EVICT pin=%d size=%d fnv=%08x", (int)pr, (int)size, h);
    blyt_console_debug(line);
}

void blyt_cart_init(void) { read_and_print(); }
void blyt_cart_update(void) {
    read_and_print(); /* frames after the first re-read a rehydrated resource */
    if (++g_frames >= 3)
        blyt_quit();
}
void blyt_cart_draw(void) {}
"#)
            .asset_bytes("blob.dat", &blob)
            .write(&evict_project);
        let evict_cart = build_cart(&evict_project);
        assert!(
            evict_cart.exists(),
            "res_evict_metal.blyt not built: {}",
            evict_cart.display()
        );
        assert!(
            qemu.scp_to(&evict_cart, "/tmp/blyt_gate/"),
            "scp res_evict_metal.blyt failed"
        );

        let out = qemu.ssh(
            "BLYT_RESOURCE_EVICT_EVERY_FRAME=1 /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/res_evict_metal.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "res_evict_metal.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        // The line must appear for every frame (init + 3 updates); the updates
        // after the first read a rehydrated resource. Identical FNV throughout.
        assert!(
            output.matches(&expected).count() >= 3,
            "native bare-metal must rehydrate the evicted zstd resource to the exact \
             bytes on every frame (expected {expected:?} repeated; #137)\noutput: {output}"
        );
        println!("  PASS: output = {:?}", output.trim());
    }

    // ── Gate 16: unified 16 MB budget enforcement on metal (#158) ─────
    //
    // The only bare-metal coverage of the unified-budget backend (ADR-0008/0027):
    // a guest `malloc` and a resource `load` draw on ONE 16 MB budget, enforced by
    // the single runtime/shared arena that now backs the native cart heap
    // (libblytc) plus the native libblytcommon footprint/budget mirror. The two
    // sub-carts are byte-for-byte the emulated mem_budget.rs carts; the metal
    // counts MUST equal the emulated legs' (a=255, b=191; ok=4, first_fail=5) —
    // that identity IS the determinism contract (#158): a native cart and a
    // desktop cart hit the cap at the same logical point. Anything resident in the
    // cart heap at init (e.g. musl-internal allocations leaking into the arena)
    // would shift these counts, so an exact match also proves the heap starts
    // clean on metal.
    println!("Gate 16: unified 16 MB budget enforcement on metal...");
    {
        // 16a: heap headroom shrinks by a resident resource's footprint.
        let budget_project = tmp.path().join("budget_metal");
        CartProject::new()
            .c(r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stdio.h>
#include <stdlib.h>

#define CHUNK (64 * 1024)
#define MAXP 512
static void *g_ptrs[MAXP];

static int fill_heap(void) {
    int n = 0;
    void *p;
    while (n < MAXP && (p = malloc(CHUNK)) != NULL)
        g_ptrs[n++] = p;
    for (int i = 0; i < n; i++)
        free(g_ptrs[i]);
    return n;
}

void blyt_cart_init(void) {
    int a = fill_heap(); /* full 16 MB available to the heap */
    /* Pin reserves 4 MiB of non-evictable footprint, held to the frame boundary
     * (ADR-0134: pin is the residency reservation). */
    const void *ptr = NULL;
    size_t size = 0;
    blyt_result_t pr = blyt_resource_pin(R_BIG, &ptr, &size);
    int b = fill_heap(); /* only ~12 MB now available to the heap */
    char line[128];
    snprintf(line, sizeof(line), "BUDGET a=%d b=%d lr=%d shrank=%d", a, b, (int)pr,
             (int)(b > 0 && b < a));
    blyt_console_debug(line);
}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#)
            .asset_bytes("big.bin", &[0u8; 4 * 1024 * 1024])
            .write(&budget_project);
        let budget_cart = build_cart(&budget_project);
        assert!(
            budget_cart.exists(),
            "budget_metal.blyt not built: {}",
            budget_cart.display()
        );
        assert!(
            qemu.scp_to(&budget_cart, "/tmp/blyt_gate/"),
            "scp budget_metal.blyt failed"
        );
        let out = qemu.ssh(
            "/tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/budget_metal.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "budget_metal.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains("BUDGET a=255 b=191 lr=0 shrank=1"),
            "native heap budget must shrink by the resident resource's footprint at \
             the identical logical point as the emulated legs (a=255 full, b=191 after \
             a 4 MiB load, load ok, shrank) — the #158 determinism contract\noutput: {output}"
        );
        println!("  PASS (16a): output = {:?}", output.trim());

        // 16b: resource pins fail deterministically at the cap.
        let cap_project = tmp.path().join("loadcap_metal");
        let mut p = CartProject::new().c(r#"
#include "blyt.h"
#include <stdio.h>

#define R_ENCODE(id) (0x20000000u | (blyt_resource_id_t)(id)) /* kind RESOURCE, prov cart */

void blyt_cart_init(void) {
    int ok = 0, first_fail = -1;
    for (int id = 1; id <= 5; id++) {
        const void *ptr = NULL;
        size_t size = 0;
        blyt_result_t r = blyt_resource_pin(R_ENCODE(id), &ptr, &size); /* held: reserves footprint */
        if (r == BLYT_OK && ptr)
            ok++;
        else if (first_fail < 0)
            first_fail = id;
    }
    char line[96];
    snprintf(line, sizeof(line), "LOADCAP ok=%d first_fail=%d", ok, first_fail);
    blyt_console_debug(line);
}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#);
        for k in 0..5 {
            p = p.asset_bytes(&format!("big{k}.bin"), &[0u8; 4 * 1024 * 1024]);
        }
        p.write(&cap_project);
        let cap_cart = build_cart(&cap_project);
        assert!(
            cap_cart.exists(),
            "loadcap_metal.blyt not built: {}",
            cap_cart.display()
        );
        assert!(
            qemu.scp_to(&cap_cart, "/tmp/blyt_gate/"),
            "scp loadcap_metal.blyt failed"
        );
        let out = qemu.ssh(
            "/tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/loadcap_metal.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "loadcap_metal.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains("LOADCAP ok=4 first_fail=5"),
            "native must refuse the resource load that crosses the 16 MB cap at the \
             identical point as the emulated legs (four 4 MiB loads fit exactly, the \
             fifth fails) — the #158 determinism contract\noutput: {output}"
        );
        println!("  PASS (16b): output = {:?}", output.trim());
    }

    // ── Gate 17: gfx torture frame hashes to the golden on metal (#188) ──
    //
    // The torture frame drawn via the gfx primitives, which rasterize natively
    // on bare metal (no ECALL) using the SAME runtime/shared integer core the
    // host/wasm/libretro legs compile.  The emitted framebuffer hash must equal
    // the reference golden — proving the RV32-compiled rasterizer is
    // bit-identical to every other target (Q2's highest-value assertion).
    {
        println!("Gate 17: gfx torture frame hashes to golden on metal...");
        let expected =
            common::gfx::expected_hash_line(&common::gfx::render(&common::gfx::torture_frame()));
        let out = qemu.ssh(
            "BLYT_FRAME_HASH=1 /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/gfx_torture.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "gfx_torture.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains(&expected),
            "native bare-metal rasterizer must hash the torture frame identically \
             to the emulated legs (expected {expected:?}; #188)\noutput: {output}"
        );
        println!("  PASS: {expected}");
    }

    // ── Gate 18: gfx acquire/present raw write hashes to golden (#188) ──
    //
    // The acquire/present contract on bare metal: acquire returns the live back
    // buffer, the cart writes the pattern directly, present flushes.  Same
    // golden as the emulated legs — proving one direct-framebuffer mechanism
    // spans every execution model (Q1) and round-trips byte-exactly.
    {
        println!("Gate 18: gfx acquire/present raw write hashes to golden on metal...");
        let expected = common::gfx::expected_hash_line(&common::gfx::raw_pattern_frame());
        let out = qemu.ssh(
            "BLYT_FRAME_HASH=1 /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/gfx_raw.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "gfx_raw.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains(&expected),
            "native bare-metal acquire/present must hash identically to the \
             emulated legs (expected {expected:?}; #188)\noutput: {output}"
        );
        println!("  PASS: {expected}");
    }

    // ── Gate 19: persistent resources on metal (#160, ADR-0028) ──────────
    //
    // The only bare-metal coverage of the native persistent path (mark from
    // `.cart.persistent`, pre-load before init, footprint reservation, eviction
    // exclusion, mem_resources residency). Two sub-carts are byte-for-byte the
    // emulated persistent_resources.rs carts; the metal output MUST equal the
    // emulated legs' — that identity is the determinism contract.
    println!("Gate 19: persistent resources on metal...");
    {
        // 19a: a persistent resource is resident from frame 0, usable without a
        // prior load, and release/unpin are no-ops on residency.
        let usable_project = tmp.path().join("pers_usable_metal");
        CartProject::new()
            .c(r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stddef.h>
#include <stdio.h>

static int is_resident(blyt_resource_id_t id) {
    blyt_mem_resource_t buf[8];
    uint32_t n = blyt_mem_resources(buf, 8);
    for (uint32_t i = 0; i < n; i++)
        if (buf[i].id == id)
            return 1;
    return 0;
}
static unsigned sum_bytes(const void *p, size_t n) {
    unsigned s = 0;
    for (size_t i = 0; i < n; i++)
        s += ((const unsigned char *)p)[i];
    return s;
}
void blyt_cart_init(void) {
    blyt_resource_id_t pid = (blyt_resource_id_t)R_PERS;
    int r0 = is_resident(pid);
    const void *p = NULL;
    size_t sz = 0;
    unsigned sum = 0;
    if (blyt_resource_pin(pid, &p, &sz) == BLYT_OK && p)
        sum = sum_bytes(p, sz);
    blyt_resource_unpin(pid);
    int r1 = is_resident(pid);
    const void *p2 = NULL;
    size_t sz2 = 0;
    unsigned sum2 = 0;
    if (blyt_resource_pin(pid, &p2, &sz2) == BLYT_OK && p2)
        sum2 = sum_bytes(p2, sz2);
    blyt_resource_unpin(pid);
    char line[128];
    snprintf(line, sizeof(line), "PERS r0=%d sum=%u sz=%u r1=%d sum2=%u", r0, sum, (unsigned)sz, r1,
             sum2);
    blyt_console_debug(line);
}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#)
            .asset_bytes("pers.bin", &[0x10, 0x20, 0x30, 0x40, 0x50])
            .persistent(&["pers"])
            .write(&usable_project);
        let usable_cart = build_cart(&usable_project);
        assert!(
            usable_cart.exists(),
            "pers_usable_metal.blyt not built: {}",
            usable_cart.display()
        );
        assert!(
            qemu.scp_to(&usable_cart, "/tmp/blyt_gate/"),
            "scp pers_usable_metal.blyt failed"
        );
        let out = qemu.ssh(
            "/tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/pers_usable_metal.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "pers_usable_metal.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains("PERS r0=1 sum=240 sz=5 r1=1 sum2=240"),
            "native persistent resource must be resident from frame 0, usable without \
             load, and survive release/unpin — identical to the emulated legs (#160)\n\
             output: {output}"
        );
        println!("  PASS (19a): output = {:?}", output.trim());

        // 19b: under pressure the persistent resource reserves budget (not evicted)
        // while a non-persistent sibling does not (evictable) — the deterministic
        // budget signature of "persistent is not evicted, sibling is".
        let evict_project = tmp.path().join("pers_evict_metal");
        CartProject::new()
            .c(r#"
#include "blyt.h"
#include "cart_resources.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define CHUNK (64 * 1024)
#define MAXP 512
static void *g_ptrs[MAXP];
static int fill_heap(void) {
    int n = 0;
    void *p;
    while (n < MAXP && (p = malloc(CHUNK)) != NULL)
        g_ptrs[n++] = p;
    for (int i = 0; i < n; i++)
        free(g_ptrs[i]);
    return n;
}
void blyt_cart_init(void) {
    const void *p = NULL;
    size_t s = 0;
    blyt_resource_pin((blyt_resource_id_t)R_TRANS4, &p, &s);
    blyt_resource_unpin((blyt_resource_id_t)R_TRANS4);
    int n = fill_heap();
    const void *pp = NULL;
    size_t ps = 0;
    int pers_ok = (blyt_resource_pin((blyt_resource_id_t)R_PERS4, &pp, &ps) == BLYT_OK && pp &&
                   ps == 4u * 1024u * 1024u);
    blyt_resource_unpin((blyt_resource_id_t)R_PERS4);
    char line[96];
    snprintf(line, sizeof(line), "PERSEVICT n=%d pers_ok=%d", n, pers_ok);
    blyt_console_debug(line);
}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#)
            .asset_bytes("pers4.bin", &[0u8; 4 * 1024 * 1024])
            .asset_bytes("trans4.bin", &[0u8; 4 * 1024 * 1024])
            .persistent(&["pers4"])
            .write(&evict_project);
        let evict_cart = build_cart(&evict_project);
        assert!(
            evict_cart.exists(),
            "pers_evict_metal.blyt not built: {}",
            evict_cart.display()
        );
        assert!(
            qemu.scp_to(&evict_cart, "/tmp/blyt_gate/"),
            "scp pers_evict_metal.blyt failed"
        );
        let out = qemu.ssh(
            "/tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/pers_evict_metal.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "pers_evict_metal.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains("PERSEVICT n=191 pers_ok=1"),
            "native persistent resource must reserve 4 MiB of budget (12 MiB headroom -> \
             191) while the non-persistent sibling stays evictable, identical to the \
             emulated legs (#160)\noutput: {output}"
        );
        println!("  PASS (19b): output = {:?}", output.trim());
    }

    // ── Gate 20: Lua gfx torture frame hashes to golden on metal (#193) ──
    //
    // The Lua × native bare-metal cell the spike left untested: a pure-Lua cart
    // draws the torture frame via blyt32.gfx.*, running on the native RV32 Lua
    // VM (blyt32lua.c binding → native libblyt32 primitives → native framebuffer
    // → weak-hook hash).  Must emit the SAME golden as the C metal leg (gate 17)
    // and every emulated Lua leg.
    if gfx_lua_cart.is_some() {
        println!("Gate 20: Lua gfx torture frame hashes to golden on metal...");
        let expected =
            common::gfx::expected_hash_line(&common::gfx::render(&common::gfx::torture_frame()));
        let out = qemu.ssh(
            "BLYT_FRAME_HASH=1 /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/gfx_lua.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "gfx_lua.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains(&expected),
            "native bare-metal Lua gfx must hash the torture frame identically to \
             the C and emulated legs (expected {expected:?}; #193)\noutput: {output}"
        );
        println!("  PASS: {expected}");
    } else {
        println!("Gate 20: SKIP (libblyt32lua.so not available or luac not found)");
    }

    // ── Gate 21: hybrid gfx torture frame hashes to golden on metal (#193) ──
    //
    // The hybrid × native bare-metal cell: the torture frame split across the Lua
    // and native-C halves.  On metal the whole cart shares one framebuffer, so
    // both halves' primitives compose into the same back buffer and must hash to
    // the single-buffer golden — the low-risk end of the #193 coherence proof
    // (its high-risk end is the wasm two-address-space cell in gfx.rs).
    if gfx_hybrid_cart.is_some() {
        println!("Gate 21: hybrid gfx torture frame hashes to golden on metal...");
        let expected =
            common::gfx::expected_hash_line(&common::gfx::render(&common::gfx::torture_frame()));
        let out = qemu.ssh(
            "BLYT_FRAME_HASH=1 /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/gfx_hybrid.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "gfx_hybrid.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains(&expected),
            "native bare-metal hybrid gfx (Lua + native-C halves) must hash the \
             torture frame identically to the single-buffer golden (expected \
             {expected:?}; #193)\noutput: {output}"
        );
        println!("  PASS: {expected}");
    } else {
        println!("Gate 21: SKIP (libblyt32lua.so not available or luac not found)");
    }

    // ── Gate 22: off-screen surface draw + blit on metal (#205) ───────
    //
    // Create a blank off-screen surface from the native pool, draw into it with
    // the tier-1 primitives, clear the screen to a background, and blit the
    // surface onto it.  The presented screen must equal the reference golden —
    // proving native off-screen create + tier-1 draw + blit compose identically
    // to the copy-based emulated legs.
    {
        println!("Gate 22: off-screen surface draw + blit on metal...");
        let out = qemu.ssh(
            "BLYT_FRAME_HASH=1 /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/surface_offscreen.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "surface_offscreen.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains(&surface_offscreen_golden),
            "native off-screen surface draw + blit must hash identically to the \
             emulated legs (expected {surface_offscreen_golden:?}; #205)\noutput: {output}"
        );
        println!("  PASS: {surface_offscreen_golden}");
    }

    // ── Gate 23: tier-2 lock on the screen == tier-1 golden (#205) ────
    //
    // Acquire the screen, draw the torture frame with the freestanding
    // blyt_raster_* primitives on the materialized buffer, release.  On metal the
    // lock exposes the canonical framebuffer pointer directly (no copy), so the
    // result must equal the tier-1 golden — pinning tier-1 ≡ tier-2 on bare metal.
    {
        println!("Gate 23: tier-2 lock on screen == tier-1 golden on metal...");
        let out = qemu.ssh(
            "BLYT_FRAME_HASH=1 /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/surface_lock.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "surface_lock.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains(&surface_lock_golden),
            "native tier-2 screen lock must hash identically to the tier-1 golden \
             (expected {surface_lock_golden:?}; #205)\noutput: {output}"
        );
        println!("  PASS: {surface_lock_golden}");
    }

    // ── Gate 24: raw per-pixel round-trip through an off-screen lock (#205) ──
    //
    // Acquire an off-screen surface, write a deterministic pattern straight into
    // lock.pixels (raw, no rasterizer), release, then blit to the screen.  The
    // presented screen must equal the reference — proving raw per-pixel writes
    // through the native direct-pointer lock survive byte-exactly and blit onto
    // the screen identically to the emulated copy-in/out path.
    {
        println!("Gate 24: raw per-pixel off-screen lock round-trip on metal...");
        let out = qemu.ssh(
            "BLYT_FRAME_HASH=1 /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/surface_raw.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "surface_raw.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains(&surface_raw_golden),
            "native off-screen tier-2 raw round-trip must hash identically to the \
             emulated legs (expected {surface_raw_golden:?}; #205)\noutput: {output}"
        );
        println!("  PASS: {surface_raw_golden}");
    }

    // ── Gate 25: Lua off-screen surface draw + blit on metal (#205) ───
    //
    // The bare-metal Lua surface cell: the native RV32 Lua VM drives
    // blyt32.surface.* → blyt32lua.c binding → the native surface pool →
    // framebuffer.  Same frame + golden as the C off-screen cart (gate 22),
    // proving the Lua surface binding is pixel-identical to C on metal too.
    if surface_lua_cart.is_some() {
        println!("Gate 25: Lua off-screen surface draw + blit on metal...");
        let out = qemu.ssh(
            "BLYT_FRAME_HASH=1 /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/surface_lua.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "surface_lua.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains(&surface_offscreen_golden),
            "native bare-metal Lua surface draw + blit must hash identically to the \
             C off-screen cart and the emulated legs (expected \
             {surface_offscreen_golden:?}; #205)\noutput: {output}"
        );
        println!("  PASS: {surface_offscreen_golden}");
    } else {
        println!("Gate 25: SKIP (libblyt32lua.so not available or luac not found)");
    }

    // ── Gate 26: blit reading a locked surface as src is rejected (#207) ──
    //
    // A blit whose SRC is a locked surface must no-op on metal, so the screen
    // stays pure background — matching the emulated golden.  Without the fix the
    // direct-pointer lock lets the blit read the lock's in-place colour-7 fill,
    // compositing it onto the screen and diverging from the emulated legs.
    {
        println!("Gate 26: blit from a locked surface src is rejected on metal...");
        let out = qemu.ssh(
            "BLYT_FRAME_HASH=1 /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/surface_locked_blit.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "surface_locked_blit.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains(&surface_locked_blit_golden),
            "native blit from a locked src must be rejected — screen must stay the \
             background golden, identical to the emulated legs (expected \
             {surface_locked_blit_golden:?}; #207)\noutput: {output}"
        );
        println!("  PASS: {surface_locked_blit_golden}");
    }

    // ── Gate 27: tier-1 op on the locked surface itself is rejected (#207) ──
    //
    // A tier-1 clear on the locked screen must no-op, so the released frame is the
    // lock's uniform 9 — not the stray 4.  This is the native-discriminated case:
    // the emulated flush masks the 4 either way, but a bare-metal cart without the
    // fix keeps it (the lock exposes the canonical buffer directly), diverging.
    {
        println!("Gate 27: tier-1 op on a locked surface is rejected on metal...");
        let out = qemu.ssh(
            "BLYT_FRAME_HASH=1 /tmp/blyt_gate/blyt_native \
             --lib-dir /tmp/blyt_gate/native \
             -- /tmp/blyt_gate/surface_locked_tier1.blyt 2>&1",
        );
        let output = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "surface_locked_tier1.blyt exited non-zero ({:?})\noutput: {output}",
            out.status.code()
        );
        assert!(
            output.contains(&surface_locked_tier1_golden),
            "native tier-1 op on a locked surface must be rejected — the frame must \
             be the lock's uniform 9, not the stray 4 (expected \
             {surface_locked_tier1_golden:?}; #207)\noutput: {output}"
        );
        println!("  PASS: {surface_locked_tier1_golden}");
    }

    println!("Gate tests passed.");
}
