/*
 * CoreMark port layer for the blyt Spike A harness.
 *
 * Target: bare RV32IMAFDC/ilp32d static-musl ELF run through the blyt rv32emu
 * interpreter core (see ../../host/runner.c). This replaces CoreMark's
 * barebones/core_portme.c (which ships #error "implement me" stubs); we reuse
 * the barebones core_portme.h, ee_printf.c (uart_send_char patched to write(2)),
 * and cvt.c unchanged.
 *
 * Timing here feeds only CoreMark's *own* reported score. The authoritative
 * effective-MIPS figure comes from the runner's retired-instruction counter
 * (rv->csr_cycle) measured against host wall-clock — independent of this clock.
 */

#include <stddef.h>
#include <stdlib.h>
#include <time.h>

#include "coremark.h"

/* SEED_METHOD == SEED_VOLATILE: CoreMark reads its seeds from these globals
 * (overwritten from argv in a performance run). Values mirror CoreMark's
 * barebones/core_portme.c for each run type. */
#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

/* clock() is a thin musl wrapper over clock_gettime; the runner services that
 * syscall with monotonic host time, so CoreMark's internal seconds track wall
 * time. CLOCKS_PER_SEC comes from musl's <time.h> (1e6). */
CORETIMETYPE barebones_clock(void) {
    return (CORETIMETYPE)clock();
}

#define GETMYTIME(_t) (*_t = barebones_clock())
#define MYTIMEDIFF(fin, ini) ((fin) - (ini))
#define TIMER_RES_DIVIDER 1
#define EE_TICKS_PER_SEC (CLOCKS_PER_SEC / TIMER_RES_DIVIDER)

static CORETIMETYPE start_time_val, stop_time_val;

void start_time(void) {
    GETMYTIME(&start_time_val);
}

void stop_time(void) {
    GETMYTIME(&stop_time_val);
}

CORE_TICKS get_time(void) {
    return (CORE_TICKS)(MYTIMEDIFF(stop_time_val, start_time_val));
}

secs_ret time_in_secs(CORE_TICKS ticks) {
    return ((secs_ret)ticks) / (secs_ret)EE_TICKS_PER_SEC;
}

ee_u32 default_num_contexts = 1;

void portable_init(core_portable *p, int *argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (sizeof(ee_ptr_int) != sizeof(ee_u8 *))
        ee_printf("ERROR! ee_ptr_int must hold a pointer!\n");
    if (sizeof(ee_u32) != 4)
        ee_printf("ERROR! ee_u32 must be a 32-bit unsigned type!\n");
    p->portable_id = 1;
}

void portable_fini(core_portable *p) {
    p->portable_id = 0;
}
