# Native RISC-V gate tests

Native RISC-V gate tests for trusted native cart execution on RISC-V ILP32.

## Test components

| File | Description |
|------|-------------|
| `seccomp_restricted_test.c` | ILP32 binary: installs restricted seccomp filter, attempts socket() → expects SIGSYS |

## Gate 1 — blyt_console_debug via write(2)

Runs `hello.blyt` through the launcher.  Expects "init", "update", "draw" in
stderr output (from `blyt_console_debug` in the hello cart).

## Gate 2 — restricted seccomp blocks socket(2)

`seccomp_restricted_test` installs the restricted seccomp filter then calls `socket()`.
Expects exit code 159 (128 + SIGSYS=31): the kernel kills the process via
seccomp `SECCOMP_RET_KILL_PROCESS` before socket returns.

## Running locally

```sh
# Required:
export BLYT_QEMU_KERNEL=/path/to/Image   # patched c-sky 6.5-rc1 kernel
export BLYT_QEMU_ROOTFS=/path/to/rootfs.qcow2

# Optional:
export BLYT_QEMU_SSH_KEY=tests/native/.ssh/id_ed25519
export BLYT_BUILD_DIR=build

cargo test -p blyt-integration-tests -- native_riscv_qemu_gate --nocapture
```

## QEMU image setup (one-time)

The test image requires:

1. **Kernel**: c-sky/csky-linux `sg2042-master-qspinlock-64ilp32_v4` (Linux
   6.5-rc1) with 3 patches applied (see `docs/design/spike-s-results.md`).

2. **Rootfs**: Alpine Linux riscv64 qcow2 with a custom ILP32F musl dynamic
   linker, built for `rv32imafc` + `ilp32f` using LLVM 18:
   - `/lib/ld-blyt.so.1` → `ld-musl-riscv32-sp.so.1` (symlink)
   - SSH key authorized for root (see `.ssh/` directory)
   - Optional: `riscv32-linux-musl-gcc` for building `seccomp_restricted_test`

3. **SSH key**: a dedicated test key pair in `tests/native/.ssh/`.
   Generate with: `ssh-keygen -t ed25519 -f tests/native/.ssh/id_ed25519 -N ""`
   Inject the public key into the rootfs image with `guestfish`:
   ```sh
   guestfish -a rootfs.qcow2 -i \
     mkdir-p /root/.ssh : \
     upload tests/native/.ssh/id_ed25519.pub /root/.ssh/authorized_keys
   ```

4. **CI**: set the `BLYT_QEMU_IMAGE_URL` repository variable to the base URL
   of a release on a companion image repository that serves `Image`,
   `rootfs.qcow2`, and `id_ed25519`.
