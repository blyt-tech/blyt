#include <stdio.h>
#include <stdlib.h>

#include "blyt_runtime.h"

/*
 * verify_cart <path>
 *
 * Opens a .blyt cart file and runs the load-time security checks
 * (blyt_cart_open). Exits 0 on success, 1 on failure.  Used in CI to
 * confirm that blyt build produces a structurally valid cart.
 */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: verify_cart <cart.blyt>\n");
        return 1;
    }

    blyt_cart_t *cart = NULL;
    blyt_cart_err_t err = blyt_cart_open(argv[1], &cart);
    if (err != BLYT_CART_OK) {
        fprintf(stderr, "FAIL %s: %s\n", argv[1], blyt_cart_err_str(err));
        return 1;
    }

    blyt_cart_close(cart);
    printf("PASS %s\n", argv[1]);
    return 0;
}
