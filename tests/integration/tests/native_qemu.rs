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
    // Verifies the full Phase 9 native path: alloc slot, write score=42,
    // save (blyt_save_write), clobber score, reload (blyt_save_read), read
    // back, confirm value.  BLYT_SAVE_DIR points to a tmpdir on the VM.
    println!("Gate 9: state buffer save/load round-trip on metal...");
    {
        let sb_project = tmp.path().join("sb_metal");
        CartProject::new()
            .config(
                "save_version: 9\n\
                 records:\n  Game:\n    fields:\n      - { name: score, type: i32 }\n\
                 state_buffers:\n  game:\n    record: Game\n    count: 1\n",
            )
            .c(r#"
#include "blyt.h"
#include "cart_state.h"

static int32_t g_slot = -1;
static int g_phase = 0;

void blyt_cart_init(void) {
    blyt_buffer_alloc_slot(S_GAME, &g_slot);
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
        blyt_buffer_set_i32(S_GAME, g_slot, S_GAME_SCORE, 42);
        blyt_save_write(0);
        blyt_buffer_set_i32(S_GAME, g_slot, S_GAME_SCORE, 0);
        g_phase = 1;
    } else {
        blyt_save_read(0);
        int32_t v = blyt_buffer_get_i32(S_GAME, g_slot, S_GAME_SCORE);
        if (v == 42) {
            blyt_console_debug("score-ok");
        } else {
            blyt_console_debug("score-fail");
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
            output.contains("version-ok"),
            "expected 'version-ok' (saved_cart_version 9 from .cart.config stamped \
             into and read back from the native save header; issue #112)\noutput: {output}"
        );
        println!("  PASS: output = {:?}", output.trim());
    }

    // ── Gate 10: entity refs + generation counters on metal ──────────
    //
    // The only coverage of the native libblyt32 generation/ref code (ADR-0096)
    // and its NLBY save extension: lifecycle (alloc/ref/free/stale/realloc),
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

    /* gens round-trip through the NLBY save format */
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

    println!("Gate tests passed.");
}
