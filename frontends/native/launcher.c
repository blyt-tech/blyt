/*
 * launcher.c — LP64 RISC-V launcher for trusted native cart execution.
 *
 * Implements the launcher side of the two-layer seccomp model (Spike S):
 *
 *   1. Validate the cart binary (basic ELF sanity check).
 *   2. Set LD_LIBRARY_PATH to the native runtime library directory so the
 *      musl ILP32 dynamic linker finds libblyt32.so (native build),
 *      libblytcommon.so, and libblytc.so.
 *   3. Call prctl(PR_SET_NO_NEW_PRIVS) — required before seccomp install;
 *      the flag is inherited across execve by the ILP32 cart process.
 *   4. Install the arch-dispatch launcher seccomp filter which applies
 *      different allowlists to this LP64 process and to the ILP32 child
 *      after exec.
 *   5. execve the ILP32 cart binary.
 *
 * After exec the kernel's compat ELF loader runs the cart; musl's ILP32 ld.so
 * resolves DT_NEEDED libraries; libblyt32.so's constructor installs the
 * more-restrictive restricted filter before cart code runs.
 *
 * Usage:
 *   blyt_native [--lib-dir DIR] [--no-seccomp] -- /path/to/cart [args...]
 *
 * Requirements:
 *   Linux RISC-V 64-bit kernel with CONFIG_COMPAT and the rv64ilp32 patchset
 *   (c-sky 6.5-rc1 + 3 patches, see docs/design/spike-s-results.md).
 *   musl ILP32 ld.so at the path the cart binary's PT_INTERP specifies.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "seccomp_allowlist.h"

#ifdef BLYT_WITH_CART_VALIDATE
/* Normal build: version string baked into libblyt.a (version.c). */
#include "blyt_runtime.h"
#define LAUNCHER_VERSION blyt_runtime_version()
#else
/* Standalone build without libblyt.a (e.g. natively inside the QEMU VM). */
#define LAUNCHER_VERSION "dev"
#endif

/* ── Argument parsing ────────────────────────────────────────────────── */

struct launcher_opts {
    const char *lib_dir;
    int do_seccomp;
    int do_validate; /* 0 = skip blyt_cart_open; for testing without libblyt.a */
    char **cart_argv;
};

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [--lib-dir DIR] [--no-seccomp] [--no-validate] -- /path/to/cart [args...]\n"
            "  --lib-dir DIR   LD_LIBRARY_PATH for the ILP32 cart (native libs)\n"
            "  --no-seccomp    skip seccomp filter install (testing only)\n"
            "  --no-validate   skip blyt_cart_open validation (CI use only)\n",
            prog);
}

static int parse_opts(int argc, char **argv, struct launcher_opts *o) {
    o->lib_dir = NULL;
    o->do_seccomp = 1;
    o->do_validate = 1;
    o->cart_argv = NULL;

    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp(argv[i], "--lib-dir") && i + 1 < argc) {
            o->lib_dir = argv[++i];
        } else if (!strcmp(argv[i], "--no-seccomp")) {
            o->do_seccomp = 0;
        } else if (!strcmp(argv[i], "--no-validate")) {
            o->do_validate = 0;
        } else if (!strcmp(argv[i], "--version")) {
            printf("blyt_native %s\n", LAUNCHER_VERSION);
            exit(0);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "blyt_native: unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }
    if (i >= argc) {
        fprintf(stderr, "blyt_native: missing cart path after --\n");
        usage(argv[0]);
        return -1;
    }
    o->cart_argv = &argv[i];
    return 0;
}

/* ── Cart validation ─────────────────────────────────────────────────── */

#ifdef BLYT_WITH_CART_VALIDATE
/* Full validation via blyt_cart_open (requires linking against libblyt.a). */

static int validate_cart(const char *path) {
    blyt_cart_t *cart = NULL;
    blyt_cart_err_t err = blyt_cart_open(path, &cart);
    if (err != BLYT_CART_OK) {
        fprintf(stderr, "blyt_native: %s: %s\n", path, blyt_cart_err_str(err));
        return -1;
    }
    /* Startup line — before fork/seccomp install (no fprintf after that). */
    fprintf(stderr, "Blyt %s - %s (%s %s)\n", blyt_runtime_version(), blyt_cart_title(cart),
            blyt_cart_id(cart), blyt_cart_version(cart));
    blyt_cart_close(cart);
    return 0;
}
#else
/* Stub for building without libblyt.a (e.g. natively inside the QEMU VM).
 * This path is only reached when --no-validate is NOT passed, which should
 * not happen in that build context.  Print an error and fail loudly. */
static int validate_cart(const char *path) {
    (void)path;
    fprintf(stderr, "blyt_native: built without BLYT_WITH_CART_VALIDATE — "
                    "use --no-validate\n");
    return -1;
}
#endif

/* ── Child process: validate, configure, exec ────────────────────────── */

static int child_main(struct launcher_opts *o) {
    /* Validate cart before installing the filter (blyt_cart_open uses
     * open/mmap/read which are not in the post-filter RV64 allowlist).
     * --no-validate skips this for CI contexts where the cart was already
     * validated on the emulated path and libblyt.a is not available. */
    if (o->do_validate && validate_cart(o->cart_argv[0]) != 0)
        return 127;

    if (setenv("BLYT_CART_PATH", o->cart_argv[0], 1) != 0) {
        fprintf(stderr, "blyt_native: setenv BLYT_CART_PATH: %s\n", strerror(errno));
        return 127;
    }

    if (o->lib_dir) {
        if (setenv("LD_LIBRARY_PATH", o->lib_dir, 1) != 0) {
            fprintf(stderr, "blyt_native: setenv LD_LIBRARY_PATH: %s\n", strerror(errno));
            return 127;
        }
        fprintf(stderr, "blyt_native: LD_LIBRARY_PATH=%s\n", o->lib_dir);
    }

    /* prctl must precede seccomp; the flag is inherited by the ILP32 child. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        fprintf(stderr, "blyt_native: prctl NO_NEW_PRIVS: %s\n", strerror(errno));
        return 127;
    }

    /* After this point only execve, write, and exit_group are allowed for
     * this LP64 process.  Avoid fprintf after filter install where possible. */
    if (o->do_seccomp && blyt_install_launcher_filter() != 0)
        return 127;

    execve(o->cart_argv[0], o->cart_argv, environ);
    /* write(2) is in the RV64 allowlist so this fprintf is safe */
    fprintf(stderr, "blyt_native: execve %s: %s\n", o->cart_argv[0], strerror(errno));
    return 127;
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    struct launcher_opts o;
    if (parse_opts(argc, argv, &o) != 0)
        return 2;

    /* Fork so the parent can report the child's exit status cleanly.
     * The seccomp filter and PR_SET_NO_NEW_PRIVS are installed inside the
     * child; the parent remains unrestricted to print diagnostics. */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "blyt_native: fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0)
        _exit(child_main(&o));

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "blyt_native: waitpid: %s\n", strerror(errno));
        return 1;
    }
    if (WIFEXITED(status)) {
        int rc = WEXITSTATUS(status);
        if (rc != 0)
            fprintf(stderr, "blyt_native: cart exited rc=%d\n", rc);
        return rc;
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        fprintf(stderr, "blyt_native: cart killed by signal %d (%s)\n", sig, strsignal(sig));
        /* SIGSYS (31) from seccomp → exit code 159 (128 + 31) */
        return 128 + sig;
    }
    return 1;
}
