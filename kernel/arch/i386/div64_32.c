/* div64_32.c -- PARITY P7: the 64/64 division helpers -m32 codegen
 * calls (__udivdi3 & friends).  The adopted shared objects (ext2.c,
 * kprintf.c) divide uint64_t values; i686 has no 64-bit divide
 * instruction, so clang emits libcalls that a freestanding kernel
 * must carry itself.  Plain binary long division: 64 iterations,
 * no lookup tables, correctness over speed (mounts and prints,
 * not hot paths).
 */

#include <stdint.h>

static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
    if (d == 0) {
        /* Freestanding: no trap to raise; an honest all-ones. */
        if (rem)
            *rem = n;
        return ~0ULL;
    }
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= 1ULL << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    return udivmod64(n, d, 0);
}

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    udivmod64(n, d, &r);
    return r;
}

int64_t __divdi3(int64_t n, int64_t d)
{
    int neg = (n < 0) ^ (d < 0);
    uint64_t un = n < 0 ? (uint64_t)-n : (uint64_t)n;
    uint64_t ud = d < 0 ? (uint64_t)-d : (uint64_t)d;
    uint64_t q = udivmod64(un, ud, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t un = n < 0 ? (uint64_t)-n : (uint64_t)n;
    uint64_t ud = d < 0 ? (uint64_t)-d : (uint64_t)d;
    uint64_t r;
    udivmod64(un, ud, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
