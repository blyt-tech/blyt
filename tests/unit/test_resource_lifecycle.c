/*
 * test_resource_lifecycle — exercises the shared resource load/pin refcount
 * state machine (runtime/shared/blyt_resource_lifecycle.h, ADR-0027, #123).
 *
 * Pure logic: handle packing, idempotent refcounted load with generation bump
 * on the 0->1 epoch transition, stale-handle rejection on release, the
 * frame-scoped pin/unpin counter, and the frame-boundary force-release. The
 * module is header-only and freestanding; it also compiles into the native
 * guest lib, so host==native behaviour is pinned here once.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "blyt_resource_lifecycle.h"

static void test_handle_packing(void) {
    uint32_t h = blyt_rl_make_handle(7, 3);
    assert(blyt_rl_handle_id(h) == 7);
    assert(blyt_rl_handle_gen(h) == 3);
    assert(h == ((3u << 16) | 7u));
    /* gen 0 (invalid sentinel) packs to a handle whose high half is zero. */
    assert(blyt_rl_make_handle(5, 0) == 5u);
}

static void test_load_idempotent_refcounted(void) {
    blyt_rl_state_t s = {0};

    uint32_t h1 = blyt_rl_load(&s, 42);
    assert(s.load_count == 1);
    assert(s.load_gen == 1); /* first ever load: gen 0 -> 1 */
    assert(h1 == blyt_rl_make_handle(42, 1));

    /* Idempotent while already resident: same handle, bumped refcount. */
    uint32_t h2 = blyt_rl_load(&s, 42);
    assert(s.load_count == 2);
    assert(s.load_gen == 1);
    assert(h2 == h1);

    /* Two loads need two releases; the handle stays valid until the last. */
    assert(blyt_rl_release(&s, h1) == 1);
    assert(s.load_count == 1);
    assert(blyt_rl_release(&s, h1) == 1);
    assert(s.load_count == 0);

    /* Released to zero: a further release of the same handle is rejected. */
    assert(blyt_rl_release(&s, h1) == 0);
}

static void test_reload_mints_new_generation(void) {
    blyt_rl_state_t s = {0};
    uint32_t h1 = blyt_rl_load(&s, 9);
    assert(blyt_rl_release(&s, h1) == 1);
    assert(s.load_count == 0);

    /* Fresh load epoch (0->1) bumps the generation: a new, distinct handle. */
    uint32_t h2 = blyt_rl_load(&s, 9);
    assert(s.load_gen == 2);
    assert(h2 == blyt_rl_make_handle(9, 2));
    assert(h2 != h1);

    /* The stale handle from the previous epoch is now rejected. */
    assert(blyt_rl_release(&s, h1) == 0);
    assert(s.load_count == 1); /* rejected release did not decrement */
    assert(blyt_rl_release(&s, h2) == 1);
}

static void test_generation_wraps_skipping_zero(void) {
    blyt_rl_state_t s = {0};
    s.load_gen = 0xFFFF; /* about to wrap */
    uint32_t h = blyt_rl_load(&s, 1);
    assert(s.load_gen == 1); /* 65535 -> 1, never 0 */
    assert(blyt_rl_handle_gen(h) == 1);
}

static void test_pin_unpin_counter(void) {
    blyt_rl_state_t s = {0};
    blyt_rl_pin(&s);
    blyt_rl_pin(&s);
    assert(s.pin_count == 2);
    assert(blyt_rl_unpin(&s) == 1);
    assert(s.pin_count == 1);
    assert(blyt_rl_unpin(&s) == 1);
    assert(s.pin_count == 0);
    /* Unpin with nothing pinned is an error, not an underflow. */
    assert(blyt_rl_unpin(&s) == 0);
    assert(s.pin_count == 0);
}

static void test_frame_boundary_force_releases_pins_only(void) {
    blyt_rl_state_t s = {0};
    uint32_t h = blyt_rl_load(&s, 3); /* residency outlives the frame */
    blyt_rl_pin(&s);
    blyt_rl_pin(&s);
    assert(s.pin_count == 2);
    assert(s.load_count == 1);

    blyt_rl_force_release_pins(&s);
    assert(s.pin_count == 0); /* pins force-released at the boundary */
    assert(s.load_count == 1); /* load residency untouched */
    assert(blyt_rl_release(&s, h) == 1); /* handle still valid next frame */
}

int main(void) {
    test_handle_packing();
    test_load_idempotent_refcounted();
    test_reload_mints_new_generation();
    test_generation_wraps_skipping_zero();
    test_pin_unpin_counter();
    test_frame_boundary_force_releases_pins_only();
    printf("test_resource_lifecycle: all passed\n");
    return 0;
}
