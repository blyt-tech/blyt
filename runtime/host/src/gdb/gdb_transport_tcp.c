/* GDB RSP TCP transport — native (SDL2 / libretro).
 *
 * Starts a listener on 127.0.0.1:<port>, accepts one client at a time,
 * and drives the gdb_stub packet loop from a background pthread.
 *
 * fc_gdb_transport_tcp_on_stop() blocks in a poll loop until the client
 * sends vCont (setting pending_action ≥ 0), then returns to the caller.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "gdb_stub.h"

#define MAX_PACKET (1 << 16)

/* ── state ────────────────────────────────────────────────────────────────── */

static struct {
    pthread_t thr;
    int       listen_fd;
    int       client_fd;
    int       running;
} g_tcp = { .listen_fd = -1, .client_fd = -1 };

/* Forward-declare internal gdb_stub helper (not in the public header). */
extern void fc_gdb_stub_set_has_client(int val);

/* ── GDB RSP framing ──────────────────────────────────────────────────────── */

static int tcp_send_pkt(const char *payload) {
    if (g_tcp.client_fd < 0) return -1;
    size_t plen = strlen(payload);
    char *buf = malloc(plen + 16);
    if (!buf) return -1;
    int csum = 0;
    for (size_t i = 0; i < plen; i++) csum += (unsigned char)payload[i];
    int n = snprintf(buf, plen + 16, "$%s#%02x", payload, csum & 0xff);
    int rc = (int)send(g_tcp.client_fd, buf, (size_t)n, MSG_NOSIGNAL);
    free(buf);
    /* Drain ack (+/-). */
    char ack;
    recv(g_tcp.client_fd, &ack, 1, 0);
    return rc;
}

/* Read one framed packet from fd; returns payload length or -1. */
static int tcp_recv_pkt_fd(int fd, char *buf, size_t cap) {
    char c;
    /* Sync to '$'. */
    while (1) {
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '$') break;
        /* Ctrl-C (0x03): not handled — ignore. */
    }
    size_t i = 0;
    while (i + 1 < cap) {
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '#') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    /* Drain 2-byte checksum. */
    char csum[2];
    if (recv(fd, csum, 2, 0) != 2) return -1;
    /* Send ack. */
    char ack = '+';
    send(fd, &ack, 1, MSG_NOSIGNAL);
    return (int)i;
}

/* Transport recv_pkt: blocks until a packet arrives on the active client fd. */
static int tcp_recv_pkt(char *buf, size_t cap) {
    if (g_tcp.client_fd < 0) return -1;
    return tcp_recv_pkt_fd(g_tcp.client_fd, buf, cap);
}

/* on_stop: called from the main run loop when the CPU hits a breakpoint.
 * Blocks until the GDB client sends vCont (pending_action ≥ 0). */
static void tcp_on_stop(void) {
    while (g_tcp.running) {
        if (fc_gdb_stub_pending_action() >= 0) break;
        usleep(2000);
    }
}

static const fc_gdb_transport_t tcp_transport = {
    .send_pkt = tcp_send_pkt,
    .recv_pkt = tcp_recv_pkt,
    .on_stop  = tcp_on_stop,
};

/* ── reader thread ────────────────────────────────────────────────────────── */

static void *tcp_thread(void *arg) {
    (void)arg;
    static char pkt[MAX_PACKET];
    while (g_tcp.running) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof cli;
        int fd = accept(g_tcp.listen_fd, (struct sockaddr *)&cli, &cl);
        if (fd < 0) {
            if (!g_tcp.running) break;
            continue;
        }
        g_tcp.client_fd = fd;
        fc_gdb_stub_set_has_client(1);
        /* Drive packet loop: read from socket, process via stub. */
        while (g_tcp.running) {
            int n = tcp_recv_pkt_fd(fd, pkt, sizeof pkt);
            if (n < 0) break;  /* client disconnected */
            fc_gdb_stub_process_pkt(pkt);
        }
        g_tcp.client_fd = -1;
        fc_gdb_stub_set_has_client(0);
        close(fd);
    }
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int fc_gdb_transport_tcp_listen(int port, int *port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { close(fd); return -1; }
    if (listen(fd, 1) < 0) { close(fd); return -1; }

    if (port_out) {
        struct sockaddr_in bound = {0};
        socklen_t sl = sizeof bound;
        getsockname(fd, (struct sockaddr *)&bound, &sl);
        *port_out = ntohs(bound.sin_port);
    }

    g_tcp.listen_fd = fd;
    g_tcp.client_fd = -1;
    g_tcp.running   = 1;

    fc_gdb_stub_set_transport(&tcp_transport);

    if (pthread_create(&g_tcp.thr, NULL, tcp_thread, NULL) != 0) {
        close(fd);
        g_tcp.running = 0;
        return -1;
    }
    return 0;
}

void fc_gdb_transport_tcp_shutdown(void) {
    if (!g_tcp.running) return;
    g_tcp.running = 0;
    if (g_tcp.client_fd >= 0) shutdown(g_tcp.client_fd, SHUT_RDWR);
    if (g_tcp.listen_fd >= 0) {
        shutdown(g_tcp.listen_fd, SHUT_RDWR); /* interrupt accept() on Linux */
        close(g_tcp.listen_fd);               /* required on macOS */
        g_tcp.listen_fd = -1;
    }
    pthread_join(g_tcp.thr, NULL);
    if (g_tcp.client_fd >= 0) { close(g_tcp.client_fd); g_tcp.client_fd = -1; }
}
