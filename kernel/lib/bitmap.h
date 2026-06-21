#ifndef NOVOS_LIB_BITMAP_H
#define NOVOS_LIB_BITMAP_H

/*
 * Header-only bitmap used by the physical memory manager. Deliberately pure C
 * with no kernel dependencies, so the same algorithms are unit-tested on the
 * host (tests/unit/test_pmm.c) and used in the kernel.
 *
 * Convention (PMM): bit SET   => frame in use / not allocatable
 *                   bit CLEAR => frame free / allocatable
 */

#include <stdint.h>

/* ---- Single-bit operations (index over a uint8_t array) ---- */

static inline void bm_set(uint8_t *bm, uint64_t i) {
    bm[i / 8] |= (uint8_t)(1u << (i % 8));
}

static inline void bm_clear(uint8_t *bm, uint64_t i) {
    bm[i / 8] &= (uint8_t)~(1u << (i % 8));
}

static inline int bm_test(const uint8_t *bm, uint64_t i) {
    return (bm[i / 8] >> (i % 8)) & 1u;
}

/*
 * Index of the first clear bit in [0, nframes), or -1 if none.
 * Byte-granular scan: skip a whole byte while it is 0xFF (full).
 */
static inline int64_t bm_first_free(const uint8_t *bm, uint64_t nframes) {
    uint64_t full_bytes = nframes / 8;
    for (uint64_t b = 0; b < full_bytes; b++) {
        if (bm[b] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (((bm[b] >> bit) & 1u) == 0) {
                    uint64_t idx = b * 8 + (uint64_t)bit;
                    if (idx < nframes) {
                        return (int64_t)idx;
                    }
                }
            }
        }
    }
    /* Tail bits that do not fill a whole byte. */
    for (uint64_t idx = full_bytes * 8; idx < nframes; idx++) {
        if (!bm_test(bm, idx)) {
            return (int64_t)idx;
        }
    }
    return -1;
}

/*
 * Index of the first run of `count` consecutive clear bits in [0, nframes),
 * or -1 if no such run exists. Single linear pass tracking the current run.
 */
static inline int64_t bm_find_contiguous(const uint8_t *bm, uint64_t nframes,
                                         uint64_t count) {
    if (count == 0) {
        return 0;
    }
    uint64_t run = 0;
    uint64_t run_start = 0;
    for (uint64_t i = 0; i < nframes; i++) {
        if (!bm_test(bm, i)) {
            if (run == 0) {
                run_start = i;
            }
            run++;
            if (run >= count) {
                return (int64_t)run_start;
            }
        } else {
            run = 0;
        }
    }
    return -1;
}

#endif /* NOVOS_LIB_BITMAP_H */
