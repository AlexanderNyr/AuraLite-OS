/* stackguard.c — userspace probe for stack guard-page enforcement.
 *
 * Deliberately overflows the user stack via unbounded recursion with a large
 * per-frame buffer.  When the kernel maps the stack with an unmapped guard
 * page, the overflow takes a page fault on the guard, the kernel reports
 * "[GUARD] user stack overflow", and the process is killed with SIGSEGV instead
 * of silently corrupting adjacent memory.
 *
 * Reaching a "STACKGUARD FAIL" line means recursion completed without faulting,
 * i.e. the guard page did not catch the overflow.
 */

#include "stdio.h"
#include "string.h"
#include "unistd.h"

/* Touch every page of a sizeable on-stack buffer so each recursion level
 * actually advances RSP past a page boundary and eventually into the guard. */
static volatile unsigned long sink;

/* A volatile, runtime-derived bound the compiler cannot fold away.  It is set
 * so large the user stack guard page is hit long before it is reached, but its
 * presence stops the optimiser proving the recursion is unbounded. */
static volatile unsigned long recurse_limit = 0xFFFFFFFFUL;

static unsigned long recurse(unsigned long depth) {
    volatile unsigned char frame[4096];
    /* Write the whole frame so the compiler cannot elide it and so we step
     * through guard territory one page at a time. */
    for (unsigned i = 0; i < sizeof(frame); i += 256) {
        frame[i] = (unsigned char)(depth + i);
    }
    sink += frame[(depth * 7u) % sizeof(frame)];

    if ((depth % 64u) == 0u) {
        printf("STACKGUARD: depth=%lu rsp-walking...\n", depth);
        fflush(stdout);
    }
    if (depth >= recurse_limit) {
        return frame[0];
    }
    return recurse(depth + 1u) + frame[0];
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("STACKGUARD: begin (expect kernel [GUARD] user stack overflow)\n");
    fflush(stdout);

    sink = recurse(1u);

    /* Unreachable if the guard page works. */
    printf("STACKGUARD FAIL: recursion returned without faulting (sink=%lu)\n",
           sink);
    fflush(stdout);
    return 1;
}
