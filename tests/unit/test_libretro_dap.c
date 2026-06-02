/* tests/unit/test_libretro_dap.c
 *
 * Minimal libretro runner that verifies the DAP TCP server starts and
 * responds to an "initialize" request.
 *
 * Test sequence:
 *   1. dlopen blyt_libretro.so (argv[1])
 *   2. Set up minimal retro callbacks
 *   3. Set BLYT_DAP_PORT=0
 *   4. retro_load_game(argv[2])  — DAP TCP server starts
 *   5. Parse "DAP listening on port N" from the log callback
 *   6. TCP connect → send DAP initialize → read response
 *   7. Verify success:true in the response
 *   8. retro_unload_game → retro_deinit → dlclose
 *
 * Exit 0 on success, 1 on failure.
 */

#include <arpa/inet.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "libretro.h"

/* ── Captured DAP port ──────────────────────────────────────────────────── */

static volatile int g_dap_port = 0;

static void RETRO_CALLCONV test_log(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[libretro] %s", buf);

    const char *prefix = "blyt: DAP listening on port ";
    if (strncmp(buf, prefix, strlen(prefix)) == 0)
        g_dap_port = atoi(buf + strlen(prefix));
}

/* ── Libretro callbacks (stubs) ─────────────────────────────────────────── */

static bool env_callback(unsigned cmd, void *data) {
    if (cmd == RETRO_ENVIRONMENT_GET_LOG_INTERFACE) {
        struct retro_log_callback *log = (struct retro_log_callback *)data;
        log->log = test_log;
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT)
        return true;
    return false;
}

static void video_refresh(const void *d, unsigned w, unsigned h, size_t p) {
    (void)d;
    (void)w;
    (void)h;
    (void)p;
}
static void audio_sample(int16_t l, int16_t r) {
    (void)l;
    (void)r;
}
static size_t audio_batch(const int16_t *d, size_t f) {
    (void)d;
    return f;
}
static void input_poll(void) {
}
static int16_t input_state(unsigned p, unsigned d, unsigned i, unsigned id) {
    (void)p;
    (void)d;
    (void)i;
    (void)id;
    return 0;
}

/* ── TCP helpers ────────────────────────────────────────────────────────── */

static int tcp_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int write_cl(int fd, const char *json) {
    char hdr[64];
    int n = snprintf(hdr, sizeof hdr, "Content-Length: %zu\r\n\r\n", strlen(json));
    if (write(fd, hdr, (size_t)n) < 0)
        return -1;
    if (write(fd, json, strlen(json)) < 0)
        return -1;
    return 0;
}

/* Read one Content-Length–framed message into buf (NUL-terminated).
 * Returns message length on success, -1 on error. */
static int read_cl(int fd, char *buf, size_t bufsz) {
    /* Read header byte-by-byte until \r\n\r\n */
    size_t hi = 0;
    while (hi + 4 <= bufsz) {
        if (read(fd, buf + hi, 1) <= 0)
            return -1;
        hi++;
        if (hi >= 4 && memcmp(buf + hi - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    buf[hi] = '\0';
    const char *cl = strstr(buf, "Content-Length:");
    if (!cl)
        return -1;
    int len = atoi(cl + 15);
    if (len <= 0 || (size_t)len >= bufsz)
        return -1;
    size_t got = 0;
    while ((int)got < len) {
        ssize_t r = read(fd, buf + got, (size_t)(len - (int)got));
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    buf[len] = '\0';
    return len;
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: test_libretro_dap <blyt_libretro.so> <cart.blyt>\n");
        return 1;
    }
    const char *so_path = argv[1];
    const char *cart_path = argv[2];

    /* 1. Load libretro core. */
    void *lib = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

#define LOAD(name)                                                                                 \
    __typeof__(name) *p_##name = (__typeof__(name) *)dlsym(lib, #name);                            \
    if (!p_##name) {                                                                               \
        fprintf(stderr, "dlsym(%s): %s\n", #name, dlerror());                                      \
        dlclose(lib);                                                                              \
        return 1;                                                                                  \
    }

    LOAD(retro_set_environment)
    LOAD(retro_set_video_refresh)
    LOAD(retro_set_audio_sample)
    LOAD(retro_set_audio_sample_batch)
    LOAD(retro_set_input_poll)
    LOAD(retro_set_input_state)
    LOAD(retro_init)
    LOAD(retro_deinit)
    LOAD(retro_load_game)
    LOAD(retro_unload_game)
#undef LOAD

    /* 2. Register callbacks. */
    p_retro_set_environment(env_callback);
    p_retro_set_video_refresh(video_refresh);
    p_retro_set_audio_sample(audio_sample);
    p_retro_set_audio_sample_batch(audio_batch);
    p_retro_set_input_poll(input_poll);
    p_retro_set_input_state(input_state);
    p_retro_init();

    /* 3. Enable DAP (OS-assigned port). */
    setenv("BLYT_DAP_PORT", "0", 1);

    /* 4. Load cart — the DAP TCP server starts here. */
    struct retro_game_info game = {cart_path, NULL, 0, NULL};
    if (!p_retro_load_game(&game)) {
        fprintf(stderr, "retro_load_game failed\n");
        p_retro_deinit();
        dlclose(lib);
        return 1;
    }

    if (g_dap_port <= 0) {
        fprintf(stderr, "DAP port not reported by log callback\n");
        p_retro_unload_game();
        p_retro_deinit();
        dlclose(lib);
        return 1;
    }
    fprintf(stderr, "[test] DAP port: %d\n", g_dap_port);

    /* 5. TCP connect (the server is already listening). */
    int sock = tcp_connect(g_dap_port);
    if (sock < 0) {
        perror("connect");
        p_retro_unload_game();
        p_retro_deinit();
        dlclose(lib);
        return 1;
    }

    /* 6. Send DAP initialize request. */
    const char *init_req = "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\","
                           "\"arguments\":{\"clientID\":\"test_libretro_dap\","
                           "\"adapterID\":\"blyt-lua\",\"linesStartAt1\":true}}";
    if (write_cl(sock, init_req) < 0) {
        fprintf(stderr, "write failed\n");
        close(sock);
        p_retro_unload_game();
        p_retro_deinit();
        dlclose(lib);
        return 1;
    }

    /* 7. Read messages until we see the initialize response with success:true.
     *    The server may also send an "initialized" event before or after. */
    char buf[65536];
    int found = 0;
    for (int i = 0; i < 5 && !found; i++) {
        int n = read_cl(sock, buf, sizeof buf);
        if (n <= 0) {
            fprintf(stderr, "read_cl failed on message %d\n", i);
            break;
        }
        fprintf(stderr, "[test] recv: %.120s\n", buf);
        if (strstr(buf, "\"command\":\"initialize\"") && strstr(buf, "\"success\":true"))
            found = 1;
    }

    close(sock);

    if (!found) {
        fprintf(stderr, "did not receive successful initialize response\n");
        p_retro_unload_game();
        p_retro_deinit();
        dlclose(lib);
        return 1;
    }

    /* 8. Tear down. */
    p_retro_unload_game();
    p_retro_deinit();
    dlclose(lib);
    fprintf(stderr, "[test] libretro DAP handshake OK\n");
    return 0;
}
