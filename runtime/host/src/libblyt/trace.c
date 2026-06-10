/* blyt_trace implementation — see runtime/host/include/blyt_trace.h. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "blyt_trace.h"

#define BLYT_TRACE_CAP_DEFAULT 512
#define BLYT_TRACE_CAP_FULL 4096

/* Parsed-state flag and mask.  Lazy initialisation may race between the main
 * thread and the GDB/DAP server threads, but both compute identical values
 * from the same environment, so the worst case is a redundant re-parse. */
static volatile int g_parsed;
static uint32_t g_mask;
static size_t g_payload_cap = BLYT_TRACE_CAP_DEFAULT;
static uint32_t g_frame;
static struct timespec g_epoch;
static volatile int g_epoch_set;

static void trace_parse(void) {
    uint32_t mask = 0;
    const char *env = getenv("BLYT_TRACE");
    if (env && env[0]) {
        const char *p = env;
        while (*p) {
            const char *end = strchr(p, ',');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            if (n == 3 && strncmp(p, "all", 3) == 0)
                mask |= BLYT_TRACE_GDB | BLYT_TRACE_DAP | BLYT_TRACE_LIFECYCLE | BLYT_TRACE_API |
                        BLYT_TRACE_FRAME;
            else if (n == 3 && strncmp(p, "gdb", 3) == 0)
                mask |= BLYT_TRACE_GDB;
            else if (n == 3 && strncmp(p, "dap", 3) == 0)
                mask |= BLYT_TRACE_DAP;
            else if (n == 9 && strncmp(p, "lifecycle", 9) == 0)
                mask |= BLYT_TRACE_LIFECYCLE;
            else if (n == 3 && strncmp(p, "api", 3) == 0)
                mask |= BLYT_TRACE_API;
            else if (n == 5 && strncmp(p, "frame", 5) == 0)
                mask |= BLYT_TRACE_FRAME;
            else if (n > 0)
                fprintf(stderr, "[blyt:trace] unknown BLYT_TRACE channel: %.*s\n", (int)n, p);
            p = end ? end + 1 : p + n;
        }
        const char *full = getenv("BLYT_TRACE_FULL");
        if (full && full[0] == '1')
            g_payload_cap = BLYT_TRACE_CAP_FULL;
    }
    g_mask = mask;
    g_parsed = 1;
}

int blyt_trace_enabled(uint32_t chan) {
    if (!g_parsed)
        trace_parse();
    return (g_mask & chan) != 0;
}

void blyt_trace_frame_mark(uint32_t frame_no) {
    g_frame = frame_no;
}

static const char *chan_name(uint32_t chan) {
    switch (chan) {
    case BLYT_TRACE_GDB:
        return "gdb";
    case BLYT_TRACE_DAP:
        return "dap";
    case BLYT_TRACE_LIFECYCLE:
        return "lifecycle";
    case BLYT_TRACE_API:
        return "api";
    case BLYT_TRACE_FRAME:
        return "frame";
    default:
        return "?";
    }
}

void blyt_tracef(uint32_t chan, const char *fmt, ...) {
    if (!blyt_trace_enabled(chan))
        return;

    if (!g_epoch_set) {
        clock_gettime(CLOCK_MONOTONIC, &g_epoch);
        g_epoch_set = 1;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - g_epoch.tv_sec) * 1000L + (now.tv_nsec - g_epoch.tv_nsec) / 1000000L;

    /* Header + payload formatted into one stack buffer; a single fprintf
     * keeps the line whole across threads (POSIX stdio per-call locking). */
    char buf[BLYT_TRACE_CAP_FULL + 64];
    int hn = snprintf(buf, sizeof buf, "[blyt:%s] f=%u t=%ld ", chan_name(chan), g_frame, ms);
    if (hn < 0)
        return;

    size_t cap = g_payload_cap;
    if (cap > sizeof(buf) - (size_t)hn - 1)
        cap = sizeof(buf) - (size_t)hn - 1;

    va_list ap;
    va_start(ap, fmt);
    int pn = vsnprintf(buf + hn, cap + 1, fmt, ap);
    va_end(ap);
    if (pn < 0)
        return;
    if ((size_t)pn > cap) {
        /* Truncated: overwrite the tail with a "...(+N)" marker. */
        char mark[32];
        int mn = snprintf(mark, sizeof mark, "...(+%zu)", (size_t)pn - cap);
        memcpy(buf + hn + cap - (size_t)mn, mark, (size_t)mn + 1);
    }
    fprintf(stderr, "%s\n", buf);
}
