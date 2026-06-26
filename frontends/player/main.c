#include <SDL.h>
#include <arpa/inet.h> /* htons, ntohs */
#include <errno.h>
#include <fcntl.h> /* fcntl, O_NONBLOCK */
#include <netinet/in.h> /* sockaddr_in */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h> /* socket, bind, listen, accept */
#include <sys/stat.h> /* stat, S_ISDIR */
#include <unistd.h> /* setenv, readlink, read, write, close, usleep */
#ifdef __APPLE__
#include <mach-o/dyld.h> /* _NSGetExecutablePath */
#endif

#include "blyt_runtime.h"
#include "libretro.h"

/* blyt_libretro.c is compiled into this binary (without BLYT_EMBED_LIBS),
 * so dynlink falls back to BLYT_LIB_DIR for the guest runtime libraries. */

/* -------------------------------------------------------------------------
 * Forward declarations of the retro_* functions from blyt_libretro.c
 * ------------------------------------------------------------------------- */

void retro_set_environment(retro_environment_t cb);
void retro_set_video_refresh(retro_video_refresh_t cb);
void retro_set_audio_sample(retro_audio_sample_t cb);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void retro_set_input_poll(retro_input_poll_t cb);
void retro_set_input_state(retro_input_state_t cb);
void retro_init(void);
void retro_deinit(void);
void retro_get_system_av_info(struct retro_system_av_info *info);
bool retro_load_game(const struct retro_game_info *game);
void retro_unload_game(void);
void retro_run(void);
void retro_reset(void);

/* Dev control channel operations (issue #87) — extensions on the libretro core. */
void blyt_libretro_reset(void);
bool blyt_libretro_save_state(uint32_t slot);
bool blyt_libretro_load_state(uint32_t slot);
bool blyt_libretro_reload(void);
bool blyt_libretro_reload_at(const char *path); /* issue #140 */
bool blyt_libretro_reload_for_debug(const char *reported_path); /* issue #119 */
bool blyt_libretro_update_assets(const uint32_t *ids, size_t n);

/* Used only by the SDL frontend (direct link) for loop termination and exit
 * status — not part of the standard libretro interface. */
bool blyt_libretro_is_done(void);
blyt_cart_run_err_t blyt_libretro_run_err(void);
void retro_reset_every_frame_cycle(void);
#ifdef BLYT_DAP
bool blyt_libretro_dap_wait_ready(void);
#endif
#ifdef BLYT_GDB
bool blyt_libretro_gdb_wait_attached(void);
void blyt_libretro_gdb_continue_initial_halt(void);
bool blyt_libretro_gdb_wait_client_continue(void);
#endif

/* -------------------------------------------------------------------------
 * Logging
 * ------------------------------------------------------------------------- */

static void RETRO_CALLCONV sdl_log(enum retro_log_level level, const char *fmt, ...) {
    FILE *out = (level >= RETRO_LOG_ERROR) ? stderr : stdout;
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
    fflush(out);
}

/* -------------------------------------------------------------------------
 * Environment callback
 * ------------------------------------------------------------------------- */

static bool env_callback(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        struct retro_log_callback *log = (struct retro_log_callback *)data;
        log->log = sdl_log;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return true; /* noted; ignored until graphics land */
    default:
        return false;
    }
}

/* -------------------------------------------------------------------------
 * Libretro callbacks
 * ------------------------------------------------------------------------- */

static SDL_Renderer *g_renderer = NULL;
static SDL_Texture *g_texture = NULL;

/* --dump-frame0: write first XRGB8888 frame as raw bytes then exit. */
static const char *g_dump_frame0_path = NULL;
static bool g_quit; /* forward declaration — defined below */

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (g_dump_frame0_path && data) {
        FILE *f = fopen(g_dump_frame0_path, "wb");
        if (f) {
            for (unsigned row = 0; row < height; row++)
                fwrite((const char *)data + row * pitch, 4, width, f);
            fclose(f);
        }
        g_dump_frame0_path = NULL; /* only dump once */
        g_quit = true;
        return;
    }
    if (!data || !g_renderer || !g_texture)
        return;
    SDL_UpdateTexture(g_texture, NULL, data, (int)pitch);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
}

static void audio_sample(int16_t left, int16_t right) {
    (void)left;
    (void)right;
}

static size_t audio_sample_batch(const int16_t *data, size_t frames) {
    (void)data;
    return frames;
}

static bool g_quit = false;
static bool g_sdl_ready = false;

static void input_poll(void) {
    if (!g_sdl_ready)
        return;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
            g_quit = true;
    }
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)port;
    (void)device;
    (void)index;
    (void)id;
    return 0;
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

static int g_dap_port = -1; /* -1 = disabled, 0 = OS-assigned, >0 = fixed */
static int g_gdb_port = -1; /* -1 = disabled, 0 = OS-assigned, >0 = fixed */
static const char *g_trace = NULL; /* --trace: BLYT_TRACE channel list */
static int g_quit_after = -1; /* -1 = disabled; >=0 = exit after N frames */
static bool g_reset_every_frame =
    false; /* --reset-every-frame: run a save/clear/restore cycle after each frame */
/* --dev-ctrl-port: dev control channel TCP port (issue #87).
 * -1 = disabled, 0 = OS-assigned (actual port announced on stdout), >0 = fixed.
 * The player listens on this port for newline-delimited JSON lifecycle commands
 * (reload / save_state / load_state / reset) — the same role the browser runtime
 * fills over the devtool's WebSocket relay.  A tool that already owns the port
 * (e.g. a test harness) connects here to drive hot reload. */
static int g_dev_ctrl_port = -1;

/* --dev-ctrl-connect: dial an existing dev control hub instead of listening
 * (issue #90, "option 2" reload wiring).  -1 = disabled, >0 = the devtool's dev
 * control TCP port.  In project-dir dev mode the devtool (`blyt run`/`blyt
 * debug ./dir`) owns the broadcast hub and its file watcher; the player dials in
 * as one more client so a watcher-driven `reload` reaches the native window the
 * same way it reaches the browser page.  The devtool is unchanged — only the
 * player learns to connect outward.  Speaks the identical newline-delimited JSON
 * protocol as the listen path, so the two share all dispatch machinery. */
static int g_dev_ctrl_connect_port = -1;

/* If BLYT_LIB_DIR is unset, try to infer it as <binary_dir>/../lib.
 * This lets blytplay/blytdebug work without any environment setup when
 * installed in an SDK whose layout is bin/ and lib/ side by side.
 * When prefer_debug is true (DAP or GDB enabled), prefer lib/debug/ if it
 * contains libblyt32.so — that variant includes the DAP ECALL hook code. */
static void maybe_setenv_lib_dir(bool prefer_debug) {
    if (getenv("BLYT_LIB_DIR") && getenv("BLYT_LIB_DIR")[0] != '\0')
        return;

    char exe[4096];
#ifdef __APPLE__
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0)
        return;
#else
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0)
        return;
    exe[len] = '\0';
#endif

    char resolved[4096];
    if (!realpath(exe, resolved))
        return;

    /* resolved = /path/to/bin/blytplay; go up two levels: dirname + "/../lib" */
    char *slash = strrchr(resolved, '/');
    if (!slash)
        return;
    *slash = '\0'; /* resolved is now the bin/ directory */

    char lib_dir[4096];
    char lib_dir_real[4096];
    char probe[4096];
    int n;

    /* When running with DAP or GDB enabled, prefer lib/debug/ — it contains the
     * guest libraries built with BLYT_DAP=1 (master_hook_ecall ECALL stubs). */
    if (prefer_debug) {
        n = snprintf(lib_dir, sizeof(lib_dir), "%s/../lib/debug", resolved);
        if (n > 0 && (size_t)n < sizeof(lib_dir) && realpath(lib_dir, lib_dir_real)) {
            n = snprintf(probe, sizeof(probe), "%s/libblyt32.so", lib_dir_real);
            if (n > 0 && (size_t)n < sizeof(probe) && access(probe, R_OK) == 0) {
                setenv("BLYT_LIB_DIR", lib_dir_real, /* overwrite */ 0);
                return;
            }
        }
    }

    n = snprintf(lib_dir, sizeof(lib_dir), "%s/../lib", resolved);
    if (n <= 0 || (size_t)n >= sizeof(lib_dir))
        return;

    if (!realpath(lib_dir, lib_dir_real))
        return;

    /* Verify libblyt32.so is present before committing. */
    n = snprintf(probe, sizeof(probe), "%s/libblyt32.so", lib_dir_real);
    if (n <= 0 || (size_t)n >= sizeof(probe))
        return;
    if (access(probe, R_OK) != 0)
        return;

    setenv("BLYT_LIB_DIR", lib_dir_real, /* overwrite */ 0);
}

static const char *parse_args(int argc, char *argv[], bool *headless) {
    const char *cart = NULL;
    *headless = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            *headless = true;
        } else if (strcmp(argv[i], "--dump-frame0") == 0 && i + 1 < argc) {
            g_dump_frame0_path = argv[++i];
            *headless = true; /* frame dump implies headless */
        } else if (strcmp(argv[i], "--debug") == 0) {
            /* Optional port number; defaults to 0 (OS-assigned) */
            if (i + 1 < argc && argv[i + 1][0] != '-')
                g_dap_port = atoi(argv[++i]);
            else
                g_dap_port = 0;
        } else if (strcmp(argv[i], "--gdb") == 0) {
            /* Optional port number; defaults to 0 (OS-assigned) */
            if (i + 1 < argc && argv[i + 1][0] != '-')
                g_gdb_port = atoi(argv[++i]);
            else
                g_gdb_port = 0;
        } else if (strcmp(argv[i], "--quit-after") == 0 && i + 1 < argc) {
            g_quit_after = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            g_trace = argv[++i];
        } else if (strncmp(argv[i], "--trace=", 8) == 0) {
            g_trace = argv[i] + 8;
        } else if (strcmp(argv[i], "--reset-every-frame") == 0) {
            g_reset_every_frame = true;
        } else if (strcmp(argv[i], "--dev-ctrl-port") == 0 && i + 1 < argc) {
            g_dev_ctrl_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dev-ctrl-connect") == 0 && i + 1 < argc) {
            g_dev_ctrl_connect_port = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            cart = argv[i];
        } else {
            fprintf(stderr, "blytplay: unknown flag: %s\n", argv[i]);
            return NULL;
        }
    }
    return cart;
}

/* -------------------------------------------------------------------------
 * Dev control channel server (issue #87)
 *
 * A non-blocking TCP listener polled from the main loop (no threads): accepts
 * one client and reads newline-delimited JSON lifecycle commands, dispatching
 * them between frames so all session mutation stays on the main thread.  This
 * is the native counterpart to the browser runtime's WebSocket handler — the
 * release player supports it too, so hot reload is not tied to the debugger.
 * ------------------------------------------------------------------------- */

static int g_dev_ctrl_listen_fd = -1;
static int g_dev_ctrl_client_fd = -1;
static char g_dev_ctrl_rx[4096];
static size_t g_dev_ctrl_rx_len = 0;

/* Minimal single-line JSON readers for the trusted command stream. */
static const char *dev_ctrl_value(const char *json, const char *key) {
    char pat[24];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pat))
        return NULL;
    const char *p = strstr(json, pat);
    if (!p)
        return NULL;
    p += n;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static long dev_ctrl_int(const char *json, const char *key, long fallback) {
    const char *v = dev_ctrl_value(json, key);
    if (!v)
        return fallback;
    char *end = NULL;
    long r = strtol(v, &end, 10);
    return (end == v) ? fallback : r;
}

static bool dev_ctrl_str(const char *json, const char *key, char *out, size_t outsz) {
    const char *v = dev_ctrl_value(json, key);
    if (!v || *v != '"')
        return false;
    v++;
    size_t i = 0;
    while (*v && *v != '"' && i + 1 < outsz)
        out[i++] = *v++;
    out[i] = '\0';
    return *v == '"';
}

/* Parse a JSON array of integers ("key":[1,2,3]) into out[0..cap), returning the
 * count parsed.  Used for the `assets` id list of the update_assets command. */
static size_t dev_ctrl_int_array(const char *json, const char *key, uint32_t *out, size_t cap) {
    const char *v = dev_ctrl_value(json, key);
    if (!v || *v != '[')
        return 0;
    v++;
    size_t cnt = 0;
    while (*v && *v != ']' && cnt < cap) {
        while (*v == ' ' || *v == '\t' || *v == ',')
            v++;
        if (*v == ']' || *v == '\0')
            break;
        char *end = NULL;
        long r = strtol(v, &end, 10);
        if (end == v)
            break;
        out[cnt++] = (uint32_t)r;
        v = end;
    }
    return cnt;
}

static void dev_ctrl_send(const char *json) {
    if (g_dev_ctrl_client_fd < 0)
        return;
    char line[512];
    int n = snprintf(line, sizeof(line), "%s\n", json);
    if (n > 0) {
        ssize_t w = write(g_dev_ctrl_client_fd, line, (size_t)n);
        (void)w;
    }
}

static void dev_ctrl_ok(long id, const char *cmd) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"id\":%ld,\"status\":\"ok\",\"cmd\":\"%s\"}", id, cmd);
    dev_ctrl_send(buf);
}

static void dev_ctrl_err(long id, const char *cmd, const char *reason) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"id\":%ld,\"status\":\"error\",\"cmd\":\"%s\",\"reason\":\"%s\"}",
             id, cmd, reason);
    dev_ctrl_send(buf);
}

static void dev_ctrl_dispatch(const char *json) {
    long id = dev_ctrl_int(json, "id", 0);
    char cmd[32];
    if (!dev_ctrl_str(json, "cmd", cmd, sizeof(cmd))) {
        dev_ctrl_err(id, "", "malformed command (missing cmd)");
        return;
    }

    if (strcmp(cmd, "reset") == 0) {
        /* Boot the cart (run init) before acking so a save_state/load_state
         * dispatched in the same poll pass sees real guest state, not a
         * pre-init blank (issue #105). */
        blyt_libretro_reset();
        dev_ctrl_ok(id, cmd);
    } else if (strcmp(cmd, "save_state") == 0) {
        uint32_t slot = (uint32_t)dev_ctrl_int(json, "slot", 0);
        if (blyt_libretro_save_state(slot))
            dev_ctrl_ok(id, cmd);
        else
            dev_ctrl_err(id, cmd, "save_state failed");
    } else if (strcmp(cmd, "load_state") == 0) {
        uint32_t slot = (uint32_t)dev_ctrl_int(json, "slot", 0);
        if (blyt_libretro_load_state(slot))
            dev_ctrl_ok(id, cmd);
        else
            dev_ctrl_err(id, cmd, "load_state failed");
    } else if (strcmp(cmd, "reload") == 0) {
        /* Reload-while-debugging (issue #119 / #140): extract the optional
         * staged-cart path from the command.  GDB sessions re-map the cart at a
         * fresh base with a unique path so lldb re-reads the new DWARF and fires
         * a solib event (issue #119).  All other sessions reload in place — same
         * base, no solib event — but also honour the explicit `path` when the
         * watcher supplies one (issue #140). */
        char path[4096];
        const char *p = dev_ctrl_str(json, "path", path, sizeof(path)) ? path : NULL;
        bool ok;
        if (g_gdb_port >= 0) {
            ok = blyt_libretro_reload_for_debug(p);
        } else {
            ok = blyt_libretro_reload_at(p);
        }
        if (ok)
            dev_ctrl_ok(id, cmd);
        else
            dev_ctrl_err(id, cmd, "reload failed");
    } else if (strcmp(cmd, "update_assets") == 0) {
        /* Hot-swap edited assets between frames; no VM restart (issue #91).
         * The runtime re-reads the whole resource-id-index (which already
         * reflects the new content), then hands the changed `assets` ids to the
         * cart's on_assets_reloaded hook (issue #122). */
        uint32_t ids[64];
        size_t n = dev_ctrl_int_array(json, "assets", ids, sizeof(ids) / sizeof(ids[0]));
        if (blyt_libretro_update_assets(ids, n))
            dev_ctrl_ok(id, cmd);
        else
            dev_ctrl_err(id, cmd, "update_assets failed");
    } else {
        dev_ctrl_err(id, cmd, "unknown command");
    }
}

/* Dispatch every complete line in the receive buffer; keep any trailing
 * partial line. */
static void dev_ctrl_process_buffer(void) {
    size_t start = 0;
    for (size_t i = 0; i < g_dev_ctrl_rx_len; i++) {
        if (g_dev_ctrl_rx[i] != '\n')
            continue;
        g_dev_ctrl_rx[i] = '\0';
        char *line = g_dev_ctrl_rx + start;
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r')
            line[ll - 1] = '\0';
        if (line[0])
            dev_ctrl_dispatch(line);
        start = i + 1;
    }
    if (start > 0) {
        memmove(g_dev_ctrl_rx, g_dev_ctrl_rx + start, g_dev_ctrl_rx_len - start);
        g_dev_ctrl_rx_len -= start;
    }
}

static void dev_ctrl_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Bind the dev control listener and announce the actual port.  port==0 lets the
 * OS assign one. */
static void dev_ctrl_start(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("blytplay: dev-ctrl socket");
        return;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("blytplay: dev-ctrl bind");
        close(fd);
        return;
    }
    if (listen(fd, 1) < 0) {
        perror("blytplay: dev-ctrl listen");
        close(fd);
        return;
    }
    socklen_t alen = sizeof(addr);
    int actual = port;
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) == 0)
        actual = ntohs(addr.sin_port);
    dev_ctrl_set_nonblocking(fd);
    g_dev_ctrl_listen_fd = fd;
    printf("Dev control: listening on 127.0.0.1:%d\n", actual);
    fflush(stdout);
}

/* Dial an existing dev control hub and adopt the connection as our client fd
 * (issue #90).  The devtool's hub is already listening by the time the player
 * starts (VS Code waits for the "Dev control:" banner before launching the
 * player), but a few short retries absorb any startup race.  On success the
 * usual non-blocking read/dispatch loop in dev_ctrl_poll() takes over. */
static void dev_ctrl_connect(int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    for (int attempt = 0; attempt < 20; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("blytplay: dev-ctrl socket");
            return;
        }
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            dev_ctrl_set_nonblocking(fd);
            g_dev_ctrl_client_fd = fd;
            g_dev_ctrl_rx_len = 0;
            printf("Dev control: connected to 127.0.0.1:%d\n", port);
            fflush(stdout);
            return;
        }
        close(fd);
        usleep(100 * 1000); /* 100 ms between attempts */
    }
    fprintf(stderr, "blytplay: dev-ctrl could not connect to 127.0.0.1:%d\n", port);
}

/* Accept a pending client (listen mode) or service the dialed connection
 * (connect mode), draining any available command bytes.  Called once per frame;
 * never blocks. */
static void dev_ctrl_poll(void) {
    /* Listen mode: accept the next client if the listener is up and idle. */
    if (g_dev_ctrl_listen_fd >= 0 && g_dev_ctrl_client_fd < 0) {
        int c = accept(g_dev_ctrl_listen_fd, NULL, NULL);
        if (c < 0)
            return; /* EWOULDBLOCK — no client yet */
        dev_ctrl_set_nonblocking(c);
        g_dev_ctrl_client_fd = c;
        g_dev_ctrl_rx_len = 0;
    }

    /* No client (connect mode not yet dialed, or listener idle): nothing to do.
     * The connect path closes the fd on hub EOF and does not re-dial — the
     * devtool hub lives for the whole session, so a drop means it is gone. */
    if (g_dev_ctrl_client_fd < 0)
        return;

    for (;;) {
        if (g_dev_ctrl_rx_len >= sizeof(g_dev_ctrl_rx) - 1)
            g_dev_ctrl_rx_len = 0; /* overlong line — drop and resync */
        ssize_t n = read(g_dev_ctrl_client_fd, g_dev_ctrl_rx + g_dev_ctrl_rx_len,
                         sizeof(g_dev_ctrl_rx) - 1 - g_dev_ctrl_rx_len);
        if (n > 0) {
            g_dev_ctrl_rx_len += (size_t)n;
            dev_ctrl_process_buffer();
            continue;
        }
        if (n == 0) { /* client closed */
            close(g_dev_ctrl_client_fd);
            g_dev_ctrl_client_fd = -1;
            return;
        }
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return; /* no more data this frame */
        if (errno == EINTR)
            continue;
        close(g_dev_ctrl_client_fd);
        g_dev_ctrl_client_fd = -1;
        return;
    }
}

static void dev_ctrl_shutdown(void) {
    if (g_dev_ctrl_client_fd >= 0)
        close(g_dev_ctrl_client_fd);
    if (g_dev_ctrl_listen_fd >= 0)
        close(g_dev_ctrl_listen_fd);
    g_dev_ctrl_client_fd = -1;
    g_dev_ctrl_listen_fd = -1;
}

int main(int argc, char *argv[]) {
    bool headless;
    const char *cart_path = parse_args(argc, argv, &headless);
    if (!cart_path) {
        fprintf(stderr, "usage: blytplay [--headless] <cart.blyt>\n");
        return 1;
    }

    bool debug_mode = (g_dap_port >= 0 || g_gdb_port >= 0);

    /* Project-dir mode: if the argument is a directory, derive the dev ELF
     * path (build/.elf for release, build/.dbg.elf for debug).  The dev ELF
     * is produced by `blyt run`/`blyt debug` and is a valid ELF with all
     * required sections — no changes to the cart loading path. */
    char elf_path_buf[4096];
    {
        struct stat st;
        if (stat(cart_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            const char *elf_name = debug_mode ? ".dbg.elf" : ".elf";
            int n =
                snprintf(elf_path_buf, sizeof(elf_path_buf), "%s/build/%s", cart_path, elf_name);
            if (n <= 0 || (size_t)n >= sizeof(elf_path_buf)) {
                fprintf(stderr, "blytplay: project path too long\n");
                return 1;
            }
            cart_path = elf_path_buf;
        }
    }

    maybe_setenv_lib_dir(debug_mode);

    /* --trace is just the flag form of the BLYT_TRACE env var (same pattern
     * as --debug → BLYT_DAP_PORT); set it before retro_init(). */
    if (g_trace)
        setenv("BLYT_TRACE", g_trace, 1);

#ifdef BLYT_DAP
    if (g_dap_port >= 0) {
        char portbuf[16];
        snprintf(portbuf, sizeof(portbuf), "%d", g_dap_port);
        setenv("BLYT_DAP_PORT", portbuf, 1);
    }
#endif
#ifdef BLYT_GDB
    if (g_gdb_port >= 0) {
        char portbuf[16];
        snprintf(portbuf, sizeof(portbuf), "%d", g_gdb_port);
        setenv("BLYT_GDB_PORT", portbuf, 1);
    }
#endif

    retro_set_environment(env_callback);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_sample_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    struct retro_game_info game = {.path = cart_path, .data = NULL, .size = 0, .meta = NULL};
    if (!retro_load_game(&game)) {
        fprintf(stderr, "blytplay: failed to load cart: %s\n", cart_path);
        retro_deinit();
        return 1;
    }

    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    double fps = av.timing.fps > 0.0 ? av.timing.fps : 60.0;
    uint32_t frame_ms = (uint32_t)(1000.0 / fps);

    /* Dev control channel (issue #87): start the listener — or dial the devtool
     * hub (issue #90, option 2) — BEFORE any debugger wait, so the dev-control
     * port is announced up front and an orchestrator can drive a hot reload
     * without first attaching a debugger (issue #119).  Non-blocking in either
     * case — the cart runs whether or not a controller is present.  The two
     * modes are mutually exclusive; --dev-ctrl-connect wins if both are set. */
    if (g_dev_ctrl_connect_port >= 0)
        dev_ctrl_connect(g_dev_ctrl_connect_port);
    else if (g_dev_ctrl_port >= 0)
        dev_ctrl_start(g_dev_ctrl_port);

#ifdef BLYT_DAP
    /* Wait for the DAP client to finish configuration (setBreakpoints +
     * configurationDone) before starting the game loop, so that breakpoints
     * are registered before init() executes. */
    if (g_dap_port >= 0)
        blyt_libretro_dap_wait_ready();
#endif
#ifdef BLYT_GDB
    /* Wait for a GDB client to connect before running the cart, so that the
     * client can set breakpoints before blyt_cart_init() executes. */
    if (g_gdb_port >= 0)
        blyt_libretro_gdb_wait_attached();
#endif
#if defined(BLYT_DAP) && defined(BLYT_GDB)
    /* Hybrid mode (both DAP and GDB active): the Lua breakpoints are already
     * registered (DAP configurationDone received above).  Wait for the native
     * side (lldb-dap) to finish ITS initial configuration too — fetch the svr4
     * library list, insert its breakpoints, and issue its first continue —
     * before releasing the cart, so a native breakpoint's ebreak is patched in
     * before any cart code runs (issue #119).  Otherwise an early native call
     * (e.g. on_new_state → a Lua-exported C function) executes first and caches a
     * translated block with no ebreak that the later insertion never
     * re-translates (rv32emu single-VM block cache, cf. #42), so the breakpoint
     * silently never fires — observed only on slow hosts where the insert loses
     * the race.  Fall back to force-clearing the halt if the native client never
     * continues (e.g. no lldb on the native side), so boot cannot wedge. */
    if (g_dap_port >= 0 && g_gdb_port >= 0) {
        if (!blyt_libretro_gdb_wait_client_continue())
            blyt_libretro_gdb_continue_initial_halt();
    }
#endif

    SDL_Window *win = NULL;
    if (!headless) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "blytplay: SDL_Init failed: %s\n", SDL_GetError());
            retro_unload_game();
            retro_deinit();
            return 1;
        }
        g_sdl_ready = true;
        win = SDL_CreateWindow("blyt", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               (int)av.geometry.base_width * 2, (int)av.geometry.base_height * 2,
                               SDL_WINDOW_SHOWN);
        if (!win) {
            fprintf(stderr, "blytplay: SDL_CreateWindow failed: %s\n", SDL_GetError());
            SDL_Quit();
            retro_unload_game();
            retro_deinit();
            return 1;
        }
        g_renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (g_renderer)
            g_texture =
                SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                  (int)av.geometry.base_width, (int)av.geometry.base_height);
    }

    int frame_count = 0;
    while (!g_quit && !blyt_libretro_is_done()) {
        uint32_t t0 = g_sdl_ready ? SDL_GetTicks() : 0;
        dev_ctrl_poll();
        retro_run();
        if (g_quit_after >= 0 && ++frame_count >= g_quit_after)
            g_quit = true;
        if (g_reset_every_frame && !g_quit)
            retro_reset_every_frame_cycle();
        if (g_sdl_ready) {
            uint32_t elapsed = SDL_GetTicks() - t0;
            if (elapsed < frame_ms)
                SDL_Delay(frame_ms - elapsed);
        } else if (g_dev_ctrl_port >= 0 || g_dev_ctrl_connect_port >= 0) {
            /* Pace headless dev control sessions to ~frame rate instead of
             * busy-spinning while waiting for commands. */
            usleep(frame_ms * 1000);
        }
    }

    dev_ctrl_shutdown();

    if (win) {
        if (g_texture)
            SDL_DestroyTexture(g_texture);
        if (g_renderer)
            SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(win);
        SDL_Quit();
    }

    blyt_cart_run_err_t run_err = blyt_libretro_run_err();
    retro_unload_game();
    retro_deinit();
    return (run_err == BLYT_RUN_OK) ? 0 : 1;
}
