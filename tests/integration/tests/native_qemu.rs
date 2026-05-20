/// Native RISC-V QEMU gate tests: trusted native cart execution on RISC-V ILP32 QEMU.
///
/// Skipped unless the following are set:
///   BLYT_QEMU_KERNEL — path to patched c-sky 6.5-rc1 kernel Image
///   BLYT_QEMU_ROOTFS — path to Fedora 42 rootfs qcow2
///                      (must have /lib/ld-blyt.so.1 symlink + blyt SSH key)
///   BLYT_QEMU_SSH_KEY (optional) — SSH key; default: tests/native/.ssh/id_ed25519
///
/// Run with:
///   BLYT_QEMU_KERNEL=... BLYT_QEMU_ROOTFS=... cargo test \
///       -- native_riscv_qemu_gate --nocapture
use std::fs;
use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

// ── Helpers ───────────────────────────────────────────────────────────────

fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf()
}

fn build_dir() -> PathBuf {
    std::env::var("BLYT_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root().join("build"))
}

fn sdk_dir() -> PathBuf {
    build_dir().join("sdk")
}

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

// ── Cart builder (mirrors e2e.rs) ─────────────────────────────────────────

fn write_cart_project(dir: &Path, source: &str) {
    let c_dir = dir.join("src/game/c");
    fs::create_dir_all(&c_dir).unwrap();
    fs::write(c_dir.join("main.c"), source).unwrap();
}

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

    project_dir.parent().unwrap().join(format!(
        "{}.blyt",
        project_dir.file_name().unwrap().to_str().unwrap()
    ))
}

// ── Gate test ─────────────────────────────────────────────────────────────

#[test]
fn native_riscv_qemu_gate() {
    // ── Prerequisites ──────────────────────────────────────────────────

    let kernel = match std::env::var("BLYT_QEMU_KERNEL") {
        Ok(k) => PathBuf::from(k),
        Err(_) => {
            eprintln!("native_riscv_qemu_gate: BLYT_QEMU_KERNEL not set — skip");
            return;
        }
    };
    let rootfs = match std::env::var("BLYT_QEMU_ROOTFS") {
        Ok(r) => PathBuf::from(r),
        Err(_) => {
            eprintln!("native_riscv_qemu_gate: BLYT_QEMU_ROOTFS not set — skip");
            return;
        }
    };
    let ssh_key = std::env::var("BLYT_QEMU_SSH_KEY")
        .map(PathBuf::from)
        .unwrap_or_else(|_| repo_root().join("tests/native/.ssh/id_ed25519"));

    for (label, path) in [
        ("BLYT_QEMU_KERNEL", &kernel),
        ("BLYT_QEMU_ROOTFS", &rootfs),
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

    let native_dir = build_dir().join("native");
    assert!(
        native_dir.join("libblyt32.so").exists(),
        "native/libblyt32.so not found — run: cmake --build build --target libblyt32_native_so"
    );
    assert!(
        sdk_dir().join("bin/blyt-clang").exists(),
        "SDK not assembled — run: cmake --build build --target sdk"
    );

    // ── Build the hello cart ───────────────────────────────────────────
    let tmp = tempfile::TempDir::new().unwrap();
    let project = tmp.path().join("hello");
    write_cart_project(
        &project,
        r#"
#include "blyt.h"
static int s_frame = 0;
void blyt_cart_init(void)   { blyt_console_debug("init"); }
void blyt_cart_update(void) {
    blyt_console_debug("update");
    if (++s_frame >= 2) blyt_quit_ready();
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

    // Native RV32 runtime libraries.  libblytc.so is intentionally omitted:
    // the native process gets its C library from the musl ld.so interpreter;
    // loading our musl subset would create two conflicting musl instances.
    for lib in ["libblyt32.so", "libblytcommon.so"] {
        let p = native_dir.join(lib);
        if p.exists() {
            assert!(
                qemu.scp_to(&p, "/tmp/blyt_gate/native/"),
                "scp {lib} failed"
            );
        }
    }

    // Cart.
    assert!(
        qemu.scp_to(&hello_cart, "/tmp/blyt_gate/"),
        "scp hello.blyt failed"
    );

    // ── Seccomp test binary (optional) ───────────────────────────────
    //
    // Requires riscv32-linux-musl-gcc in the VM (not installed by default).
    // Cross-compilation of the ILP32 test binary from the CI host requires a
    // musl-riscv32 sysroot not currently available in CI.
    let have_seccomp_test = {
        // SCP the source and header if the compiler is available in the VM.
        let compiler_present = qemu.ssh_ok("command -v riscv32-linux-musl-gcc");
        if compiler_present {
            qemu.scp_to(
                &repo_root().join("tests/native/seccomp_restricted_test.c"),
                "/tmp/blyt_gate/",
            );
            qemu.ssh_ok("mkdir -p /tmp/blyt_gate/seccomp_h");
            qemu.scp_to(
                &repo_root().join("frontends/native/src/libblyt32/seccomp_restricted.h"),
                "/tmp/blyt_gate/seccomp_h/",
            );
            let built = qemu
                .ssh(
                    "riscv32-linux-musl-gcc \
                     -I/tmp/blyt_gate/seccomp_h \
                     -o /tmp/blyt_gate/seccomp_restricted_test \
                     /tmp/blyt_gate/seccomp_restricted_test.c 2>&1",
                )
                .status
                .success();
            if !built {
                eprintln!("  note: seccomp_restricted_test build failed — gate 2 skipped");
            }
            built
        } else {
            eprintln!(
                "  note: riscv32-linux-musl-gcc not in VM — \
                 restricted seccomp gate will be skipped"
            );
            false
        }
    };

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
    if have_seccomp_test {
        println!("Gate 2: restricted seccomp blocks socket(2)...");
        let out = qemu.ssh("/tmp/blyt_gate/seccomp_restricted_test 2>&1");
        // SIGSYS (31) → shell exit code 128+31 = 159
        let rc = out.status.code().unwrap_or(-1);
        assert_eq!(
            rc, 159,
            "expected exit 159 (SIGSYS) from seccomp_restricted_test, got {rc}"
        );
        println!("  PASS: exit {rc} (SIGSYS)");
    } else {
        println!("Gate 2: SKIP (riscv32-linux-musl-gcc not available in QEMU VM)");
    }

    println!("Gate tests passed.");
}
