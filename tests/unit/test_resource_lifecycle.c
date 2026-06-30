/*
 * test_resource_lifecycle — exercises the shared resource pin refcount state
 * machine (runtime/shared/blyt_resource_lifecycle.h, ADR-0027 as amended by
 * ADR-0134, #123/#196).
 *
 * The cart-held load/release residency handle is gone (ADR-0134): resources are
 * referenced by their baked constant directly and the runtime owns residency.
 * What remains is the within-frame pin counter, the frame-boundary force-release,
 * and the eviction predicate (now pin-only). The module is header-only and
 * freestanding; it also compiles into the native guest lib, so host==native
 * behaviour is pinned here once.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "blyt_resource_lifecycle.h"

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

static void test_frame_boundary_force_releases_pins(void) {
    blyt_rl_state_t s = {0};
    blyt_rl_pin(&s);
    blyt_rl_pin(&s);
    assert(s.pin_count == 2);

    blyt_rl_force_release_pins(&s);
    assert(s.pin_count == 0); /* every pin force-released at the boundary */
}

/* Eviction eligibility (ADR-0027 v2, #137; simplified by ADR-0134): an entry's
 * owned/decompressed bytes may be reclaimed exactly when no within-frame pin
 * references it. Persistence (ADR-0028, #160) is a separate property the caller
 * AND-checks. */
static void test_is_evictable_predicate(void) {
    blyt_rl_state_t s = {0};
    assert(blyt_rl_is_evictable(&s)); /* never-pinned: evictable */

    blyt_rl_pin(&s);
    assert(!blyt_rl_is_evictable(&s)); /* a within-frame pin blocks eviction */
    assert(blyt_rl_unpin(&s) == 1);
    assert(blyt_rl_is_evictable(&s)); /* unpinned back to zero: evictable again */

    /* Multiple pins block until all drop; force-release clears them at once. */
    blyt_rl_pin(&s);
    blyt_rl_pin(&s);
    assert(!blyt_rl_is_evictable(&s));
    blyt_rl_force_release_pins(&s);
    assert(blyt_rl_is_evictable(&s));
}

int main(void) {
    test_pin_unpin_counter();
    test_frame_boundary_force_releases_pins();
    test_is_evictable_predicate();
    printf("test_resource_lifecycle: all passed\n");
    return 0;
}
