/*
 * doom_tick_c.c — native-C twin of doom_bench.lua, for the "host-Lua vs
 * emulated-native-C" comparison. SAME game logic, same doubles, same LCG, same
 * order as the Lua version — differing only in language (compiled C, no Lua VM).
 * Projectiles use a fixed pool with swap-with-last removal (the C-game idiom;
 * Doom's mobj list works the same). Because the checksum counts how many
 * projectiles/mobs survive (not their storage), it still lands on the Lua
 * version's 117/sim — a three-way determinism cross-check (host-Lua ==
 * emulated-Lua == emulated-C). bench: doom_tick_c <repeats>.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define N_MOBS 64
#define NFRAMES 100
#define MAXP 1024

enum { S_IDLE = 1, S_CHASE, S_ATTACK, S_DEAD };
#define PLAYER_X 64.0
#define PLAYER_Y 64.0
#define SIGHT 40.0
#define ATTACKR 12.0
#define ATTACKP 8
#define CHASE_SPEED 1.5
#define PROJ_SPEED 4.0
#define PROJ_TTL 30

typedef struct {
    double x, y, vx, vy;
    int hp, state, tics, alive;
} mob_t;
typedef struct {
    double x, y, vx, vy;
    int ttl;
} proj_t;

static mob_t mobs[N_MOBS];
static proj_t proj[MAXP];
static int nproj;
static uint32_t seed;

static double lrand(void) {
    seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
    return (double)seed / 2147483647.0;
}

static void spawn(const mob_t *m) {
    double dx = PLAYER_X - m->x, dy = PLAYER_Y - m->y;
    double d = sqrt(dx * dx + dy * dy);
    if (d < 0.001)
        d = 1.0;
    if (nproj < MAXP) {
        proj[nproj].x = m->x;
        proj[nproj].y = m->y;
        proj[nproj].vx = dx / d * PROJ_SPEED;
        proj[nproj].vy = dy / d * PROJ_SPEED;
        proj[nproj].ttl = PROJ_TTL;
        nproj++;
    }
}

static void tick_mob(mob_t *m) {
    if (!m->alive)
        return;
    m->tics++;
    double dx = PLAYER_X - m->x, dy = PLAYER_Y - m->y;
    double dist = sqrt(dx * dx + dy * dy);
    switch (m->state) {
    case S_IDLE:
        if (dist < SIGHT) {
            m->state = S_CHASE;
            m->tics = 0;
        }
        break;
    case S_CHASE:
        if (dist < ATTACKR) {
            m->state = S_ATTACK;
            m->tics = 0;
        } else {
            if (dist > 0.001) {
                m->vx = dx / dist * CHASE_SPEED;
                m->vy = dy / dist * CHASE_SPEED;
            }
            m->x += m->vx;
            m->y += m->vy;
        }
        break;
    case S_ATTACK:
        if (dist > ATTACKR)
            m->state = S_CHASE;
        else if (m->tics % ATTACKP == 0)
            spawn(m);
        break;
    case S_DEAD:
        m->alive = 0;
        break;
    }
}

static void tick_proj(void) {
    for (int i = 0; i < nproj; i++) {
        proj[i].x += proj[i].vx;
        proj[i].y += proj[i].vy;
        proj[i].ttl--;
    }
    for (int i = nproj - 1; i >= 0; i--) {
        proj_t *p = &proj[i];
        if (p->ttl <= 0 || p->x < 0 || p->x > 128 || p->y < 0 || p->y > 128)
            proj[i] = proj[--nproj]; /* swap-with-last */
    }
}

static int run_one_sim(void) {
    seed = 12345;
    nproj = 0;
    for (int i = 0; i < N_MOBS; i++) {
        mobs[i].x = lrand() * 128.0;
        mobs[i].y = lrand() * 128.0;
        mobs[i].vx = mobs[i].vy = 0.0;
        mobs[i].hp = 10;
        mobs[i].state = S_IDLE;
        mobs[i].tics = 0;
        mobs[i].alive = 1;
    }
    for (int t = 0; t < NFRAMES; t++) {
        for (int i = 0; i < N_MOBS; i++)
            tick_mob(&mobs[i]);
        tick_proj();
        if (lrand() < 0.02) {
            int idx = (int)(lrand() * N_MOBS);
            if (idx >= N_MOBS)
                idx = N_MOBS - 1;
            if (mobs[idx].alive)
                mobs[idx].state = S_DEAD;
        }
    }
    int total = nproj;
    for (int i = 0; i < N_MOBS; i++)
        total += mobs[i].alive ? 1 : 0;
    return total;
}

int main(int argc, char **argv) {
    long repeats = (argc > 1) ? atol(argv[1]) : 1000;
    long total = 0;
    for (long r = 0; r < repeats; r++)
        total += run_one_sim();
    printf("[doom] repeats=%ld checksum=%ld\n", repeats, total);
    return 0;
}
