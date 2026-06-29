#ifndef AURALITE_KERNEL_TIME_TYPES_H
#define AURALITE_KERNEL_TIME_TYPES_H

#include <stdint.h>

typedef long time_t;

/* Per-process interval-timer state (stored in tcb_t::itimers[]). */
struct itimer_state {
    uint64_t interval_ticks;
    uint64_t deadline_ticks;
};

#endif /* AURALITE_KERNEL_TIME_TYPES_H */
