/* perfstat.c — named monotonic performance counters (OPT_PLAN.md O0).
 *
 * Storage is a flat array of _Atomic uint64_t bumped with relaxed
 * fetch-adds.  Relaxed is enough: each counter is independent, nothing
 * orders against them, and the only reader (/proc/perf) tolerates a
 * mid-update snapshot — a monitoring value that is microseconds stale is
 * still the truth.  No locks means the counters are safe from IRQ
 * context and from the earliest boot instruction that can execute C
 * (static storage, no init required), which uart_putchar needs.
 */
#include <stdint.h>
#include "kernel/lib/perfstat.h"

/* Plain uint64_t + __atomic builtins (not C11 _Atomic: clang refuses the
 * mix of _Atomic-qualified pointees and __atomic_* builtins). */
static uint64_t counters[PERF_COUNTER_MAX];

/* Names are the /proc/perf wire format; tests grep them.  Renaming one is
 * an interface change and wants the same care as renaming a syscall. */
static const char *const names[PERF_COUNTER_MAX] = {
    [PERF_BOOT_TICKS_TO_SHELL]           = "boot_ticks_to_shell",
    [PERF_COMPOSITOR_FRAMES_FULL]        = "compositor_frames_full",
    [PERF_COMPOSITOR_FRAMES_PARTIAL]     = "compositor_frames_partial",
    [PERF_COMPOSITOR_PIXELS_COMPOSITED]  = "compositor_pixels_composited",
    [PERF_COMPOSITOR_PIXELS_FLIPPED]     = "compositor_pixels_flipped",
    [PERF_TLB_SHOOTDOWNS_FULL]           = "tlb_shootdowns_full",
    [PERF_TLB_SHOOTDOWNS_RANGED]         = "tlb_shootdowns_ranged",
    [PERF_TLB_IPIS_SKIPPED]              = "tlb_ipis_skipped",
    [PERF_KMALLOC_WALK_STEPS]            = "kmalloc_walk_steps",
    [PERF_KMALLOC_CLASS_HITS]            = "kmalloc_class_hits",
    [PERF_UART_TX_SYNC_BYTES]            = "uart_tx_sync_bytes",
    [PERF_UART_TX_RING_BYTES]            = "uart_tx_ring_bytes",
    [PERF_CR3_NOFLUSH_SWITCHES]          = "cr3_noflush_switches",
    [PERF_PCID_GENERATION_WRAPS]         = "pcid_generation_wraps",
    [PERF_TCP_RETRANSMITS]               = "tcp_retransmits",
    [PERF_TCP_FAST_RETRANSMITS]          = "tcp_fast_retransmits",
    [PERF_TCP_RTO_EVENTS]                = "tcp_rto_events",
    [PERF_TCP_CWND_LIMITED_SENDS]        = "tcp_cwnd_limited_sends",
};

void perfstat_add(int id, uint64_t n) {
    if (id < 0 || id >= PERF_COUNTER_MAX) return;
    __atomic_fetch_add(&counters[id], n, __ATOMIC_RELAXED);
}

void perfstat_set(int id, uint64_t v) {
    if (id < 0 || id >= PERF_COUNTER_MAX) return;
    __atomic_store_n(&counters[id], v, __ATOMIC_RELAXED);
}

uint64_t perfstat_get(int id) {
    if (id < 0 || id >= PERF_COUNTER_MAX) return 0;
    return __atomic_load_n(&counters[id], __ATOMIC_RELAXED);
}

int perfstat_counter_count(void) {
    return PERF_COUNTER_MAX;
}

const char *perfstat_name(int id) {
    if (id < 0 || id >= PERF_COUNTER_MAX || !names[id]) return "?";
    return names[id];
}
