/* test_dualstack.c — host gate for the Y3 address-selection core. */
#include <stdio.h>
#include "../../kernel/net/dualstack.h"

static int passed, failed;
#define CHECK(c, ...) do { \
    if (c) passed++; \
    else { failed++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

int main(void) {
    CHECK(dualstack_pick(1, 1, 1) == DS_V6,
          "global + AAAA + A prefers v6");
    CHECK(dualstack_pick(1, 1, 0) == DS_V6,
          "global + AAAA (no A) picks v6");
    CHECK(dualstack_pick(1, 0, 1) == DS_V4,
          "global but no AAAA falls back to A");
    CHECK(dualstack_pick(0, 1, 1) == DS_V4,
          "no global address: v4 even if AAAA exists");
    CHECK(dualstack_pick(0, 1, 0) == DS_V6,
          "AAAA-only still usable when nothing else exists");
    CHECK(dualstack_pick(0, 0, 1) == DS_V4, "A-only picks v4");
    CHECK(dualstack_pick(0, 0, 0) == DS_NONE, "nothing learned is NONE");
    CHECK(dualstack_pick(1, 0, 0) == DS_NONE, "global alone is not an address");
    printf("test_dualstack: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
