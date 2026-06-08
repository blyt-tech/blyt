# ADR-0131 — Filesystem Sandboxing for Native Carts

**Status:** Draft  
**Date:** 2026-06-08

---

## Context

The native cart execution path (LP64 launcher → ILP32 cart process via `execve`) uses seccomp to restrict which *syscalls* the cart process may make (the two-layer model: launcher arch-dispatch filter + restricted filter installed by `libblyt32.so`'s constructor). Phase 9 added `openat`, `read`, `write`, `mkdirat`, and `fsync` to the seccomp allowlist to support save/load file I/O. The restricted filter contains 11 syscalls; the launcher's ILP32 list contains 21.

Seccomp restricts *which syscalls* are permitted, but not *which filesystem paths* those syscalls may access. A cart that calls `openat(AT_FDCWD, "/home/user/.ssh/id_rsa", O_RDONLY)` will succeed if the syscall is in the allowlist and the user has read permission. Phase 9 opened that door.

This ADR closes the gap by adding a **path-level** sandbox layer.

---

## Mechanism comparison

### chroot(2)

`chroot(2)` remaps the process root directory; paths outside the jail become unreachable. In principle it can confine the cart to a subtree.

Problems for this use case:

- Requires `CAP_SYS_CHROOT` or root to set up. The launcher runs unprivileged; there is no setup step that could establish the jail without privilege.
- The entire library tree (`ld-blyt.so.1`, `libblyt32.so`, `libblytcommon.so`, `libblytc.so`) would need to be replicated inside the jail, or the jail would need to be the root filesystem — negating any path restriction.
- Does not restrict operations *within* the jail; a chrooted cart can still modify any file the running user owns that is visible inside the jail.
- Symlinks to paths inside the jail can escape if the jail is not set up carefully.
- Does not compose with seccomp: chroot affects path namespace only.

**Verdict:** Not viable without privilege; fragile to set up correctly.

### Mount namespaces (container-style)

`unshare(CLONE_NEWNS)` creates a private mount namespace. The launcher could then bind-mount only the required directories into it, hiding everything else from the cart process entirely.

This is the mechanism used by containers (Docker, systemd-nspawn, bubblewrap). It is stronger than landlock: paths outside the bind-mounts are invisible, not just inaccessible.

Problems for this use case:

- Requires `CAP_SYS_ADMIN` or an unprivileged user namespace (`clone(CLONE_NEWUSER | CLONE_NEWNS)`).
- User namespaces have their own attack surface: UID/GID mapping, interaction with `newuidmap`/`newgidmap`, and seccomp-mediated kernel exploit surface. They are disabled on some hardened deployments (`/proc/sys/kernel/unprivileged_userns_clone = 0`).
- The QEMU test environment and target hardware may not support nested user namespaces. The sg2042 kernel is already a non-mainline fork; adding user-namespace complexity compounds the support burden.
- Per-process setup is significantly more involved than a seccomp BPF program or a landlock ruleset.

**Verdict:** Too complex; privilege and availability cannot be guaranteed on the target kernel.

### Landlock(2)

`landlock(2)` is an in-kernel mechanism that lets an *unprivileged process* restrict its own filesystem access to a declared set of path+access-right pairs. It was added to mainline Linux in 5.13 (June 2021) and has been stable through three ABI versions.

Properties relevant here:

- **Unprivileged.** No capability required; any process can install a landlock ruleset on itself.
- **Inherited across `execve`.** A ruleset installed by the launcher before `execve` applies to the ILP32 child — exactly the right place to install it.
- **Stacks with seccomp.** Both are enforced independently. Seccomp gates *which syscalls* are made; landlock gates *which paths* those syscalls may touch. A syscall must pass both.
- **Per-path, per-right.** The ruleset specifies which paths get which subset of access rights (`READ_FILE`, `WRITE_FILE`, `MAKE_DIR`, `MAKE_REG`, `REMOVE_FILE`, `REMOVE_DIR`, `READ_DIR`, `EXECUTE`). Rights not granted default to denied.
- **Graceful version detection.** `landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)` returns the ABI version the running kernel supports. Callers mask their rights bitmask against the supported ABI's rights to avoid `EINVAL` on older kernels.

**Verdict:** Chosen mechanism.

---

## Kernel version

The supported kernel is `c-sky/csky-linux` branch `sg2042-master-qspinlock-64ilp32_v4` (Linux 6.5-rc1 + 3 patches for the rv64ilp32 ABI). This is the minimum required kernel for native RISC-V ILP32 cart execution; the rv64ilp32 patchset is not yet mainlined.

Landlock ABI history in mainline:

| ABI | Kernel | New rights added |
|-----|--------|-----------------|
| v1 | 5.13 | `READ_FILE`, `WRITE_FILE`, `MAKE_REG`, `MAKE_DIR`, `REMOVE_DIR`, `REMOVE_FILE`, `MAKE_SOCK`, `MAKE_FIFO`, `MAKE_BLOCK`, `MAKE_SYM`, `EXECUTE`, `READ_DIR`, `MAKE_CHAR` |
| v2 | 5.19 | `REFER` (cross-directory rename) |
| v3 | 6.2 | `TRUNCATE` |
| v4 | 6.7 | Network access (`LANDLOCK_ACCESS_NET_*`) |

The 6.5-rc1 base includes landlock **ABI v3**, provided `CONFIG_SECURITY_LANDLOCK=y` is set in the kernel config. Since the project already requires a custom-patched kernel, **`CONFIG_SECURITY_LANDLOCK=y` is a required build-time kernel configuration**. Kernels built without it will cause the launcher to warn and skip the path sandbox (see fallback behaviour below).

All access rights needed by the policy (ABI v1) are available in the minimum supported kernel.

---

## Network access

Network access is **already completely blocked by the seccomp allowlist**. Neither the launcher's arch-dispatch filter nor the restricted filter installed by `libblyt32.so`'s constructor includes any network syscalls:

- `socket` (198), `connect` (203), `bind` (200), `sendto` (206), `recvfrom` (207), and all other network calls are absent from both filters.

Landlock network access rights (`LANDLOCK_ACCESS_NET_*`, ABI v4 / kernel 6.7) would be entirely redundant. Landlock in this ADR covers only filesystem paths.

---

## Decision: Landlock filesystem policy

The LP64 launcher installs a landlock ruleset before `execve`. The ruleset is inherited by the ILP32 child and governs all filesystem access for the lifetime of the cart process.

### Launcher pre-creates all directories

All directories in the policy are created by the **launcher** before landlock is installed and before `execve`. The cart process never needs to create a directory:

- `BLYT_SAVE_DIR` (and any required subdirectories): created by the launcher with `mkdir -p`.
- `BLYT_WORK_DIR` (pid-scoped default `/tmp/blyt-<pid>`): created by the launcher; deleted by the launcher after the cart exits.
- `BLYT_LIB_DIR` and `BLYT_DATA_DIR`: read-only; must already exist.

Landlock also requires a path to exist at ruleset-install time, so pre-creation is necessary anyway.

This means `MAKE_DIR` is not granted to the cart process for any path category. As a direct consequence, `mkdirat(34)` — which was added to the restricted seccomp filter in Phase 9 solely so `blyt_save_write` could create the save directory — can be removed from both the restricted filter (`seccomp_restricted.h`) and the launcher's ILP32 allowlist (`seccomp_allowlist.h`). The cart's `blyt_save_write` implementation must be updated to omit the `mkdirat` call; the launcher guarantees the directory exists before the cart runs.

### Path categories

| Category | Env var | Default | Landlock rights |
|---|---|---|---|
| Cart binary | *(launcher `argv[0]`)* | — | `READ_FILE` + `EXECUTE` |
| Library directory | `BLYT_LIB_DIR` | auto-inferred from exe (`<exe>/../lib`) | `READ_FILE` + `READ_DIR` |
| Runtime data/assets | `BLYT_DATA_DIR` | `<exe>/../share/blyt` | `READ_FILE` + `READ_DIR` |
| Save directory | `BLYT_SAVE_DIR` | `$HOME/.local/share/blyt` | `READ_FILE` + `WRITE_FILE` + `MAKE_REG` + `REMOVE_FILE` |
| Work directory | `BLYT_WORK_DIR` | `/tmp/blyt-<pid>` | `READ_FILE` + `WRITE_FILE` + `MAKE_REG` + `REMOVE_FILE` |

**`BLYT_DATA_DIR`** and **`BLYT_WORK_DIR`** are new env vars introduced by this ADR. `BLYT_LIB_DIR` and `BLYT_SAVE_DIR` already exist.

### Work directory and `.cart.resources`

The work directory is a per-run scratch space. Its full read/write/create/delete rights support two use cases:

1. **Pre-expansion of `.cart.resources` assets.** The `.cart.resources` ELF section (already reserved in `cart_load.c:55`) is intended to carry compressed assets (sprites, audio, map data) embedded in the cart binary. Before `blyt_cart_init` is called, the runtime can decompress these assets into the work directory so the cart can `mmap` or `read` them without holding the compressed form in memory. This also provides deterministic access latency — relevant for speed-running where timing is measured from cart start.

2. **General cart scratch space.** Carts may use the work directory for any temporary file I/O (e.g. decompressing downloaded content, building a level cache). The directory is cleaned up by the launcher after the cart exits.

The launcher creates a pid-scoped default path (`/tmp/blyt-<pid>`) if `BLYT_WORK_DIR` is not set, ensuring isolation between concurrent cart processes.

This ADR establishes the sandbox boundary for pre-expansion. The implementation of `.cart.resources` parsing and pre-expansion is a separate task.

### Fallback behaviour

```c
int abi = landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
if (abi < 0) {
    /* ENOSYS: kernel < 5.13 or CONFIG_SECURITY_LANDLOCK not set.
     * EOPNOTSUPP: landlock disabled at runtime.
     * Warn and proceed — seccomp remains active as the primary gate. */
    fprintf(stderr,
            "blyt_native: warning: landlock unavailable (%s); "
            "filesystem path sandboxing disabled\n",
            strerror(errno));
} else {
    /* Mask the rights bitmask to what abi version supports,
     * add one rule per path category, then restrict self. */
    ...
    landlock_restrict_self(ruleset_fd, 0);
    close(ruleset_fd);
}
```

The rights bitmask must be masked against the ABI-supported set to avoid `EINVAL` on kernels whose landlock implementation predates a given right. For the minimum supported kernel (ABI v3), all required rights are available.

---

## Consequences

- `mkdirat(34)` is removed from the restricted seccomp filter (`seccomp_restricted.h`) and the launcher's ILP32 allowlist (`seccomp_allowlist.h`). The cart process no longer has permission to create directories. The native `blyt_save_write` implementation must remove its `blyt_rs_mkdirat` call.
- The launcher gains responsibility for creating `BLYT_SAVE_DIR`, `BLYT_WORK_DIR`, and any required subdirectories before exec.
- The launcher gains a cleanup step: remove `BLYT_WORK_DIR` (when pid-scoped default) after the cart process exits.

## Out of scope

- **Non-native frontends.** WASM, libretro, and SDL2 frontends run in the host process and rely on OS-level process isolation. Landlock is not applied to them.
- **Network access.** Already fully blocked by the seccomp allowlist. No landlock network rules are needed or added.
- **Hard-fail when landlock is unavailable.** The launcher warns and proceeds. The cart is still seccomp-filtered. Mandatory landlock enforcement may be revisited once the minimum supported kernel is known to always include `CONFIG_SECURITY_LANDLOCK=y`.
- **`.cart.resources` pre-expansion implementation.** The work directory boundary is established here; the expansion mechanism is a separate ADR/task.
