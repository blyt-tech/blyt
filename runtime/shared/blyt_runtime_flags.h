/* blyt_runtime_flags.h — host→guest runtime flags block (#208 Stage 2)
 *
 * A small guest-visible struct the host fills in once at session setup, read by
 * the guest without an ECALL — the same mechanism as the unified-budget
 * accounting block (`blyt_mem_acct`, #158): a guest lib exports the global, the
 * host resolves its guest address from the cart's symbol table and writes into
 * guest memory, and the guest reads the fields as plain memory.
 *
 * Today it carries just `cart_is_debug`, which the tier-2 Lua per-pixel lock
 * reads to choose its out-of-bounds / stale-access behaviour (debug: a hard Lua
 * error; release: a defined no-op) without crossing to the host per access.
 * `cart_is_debug` is a per-build property — identical on every leg for a given
 * cart — so it does not affect the cross-leg determinism contract.
 */
#ifndef BLYT_RUNTIME_FLAGS_H
#define BLYT_RUNTIME_FLAGS_H

#include <stdint.h>

typedef struct {
    uint32_t cart_is_debug; /* 1 in a debug cart (blyt build --debug), else 0 */
} blyt_runtime_flags_t;

#endif /* BLYT_RUNTIME_FLAGS_H */
