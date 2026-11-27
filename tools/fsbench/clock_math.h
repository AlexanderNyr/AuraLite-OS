#ifndef AURALITE_FSBENCH_CLOCK_MATH_H
#define AURALITE_FSBENCH_CLOCK_MATH_H
#include <stdint.h>

/* CLOCK_MONOTONIC intervals in the benchmark are short (one calibration
 * sleep). nsec can decrease at a second boundary; subtract before dividing. */
static inline uint64_t fsbench_elapsed_us(int64_t start_sec, int64_t start_nsec,
                                         int64_t end_sec, int64_t end_nsec) {
    int64_t ns = (end_sec - start_sec) * 1000000000LL + end_nsec - start_nsec;
    return ns > 0 ? (uint64_t)ns / 1000u : 0;
}
#endif
