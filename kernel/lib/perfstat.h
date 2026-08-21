/* perfstat.h — named monotonic performance counters (OPT_PLAN.md O0).
 *
 * The measuring rig the optimization plan hangs every claim on: a fixed
 * table of counters, added to from anywhere in the kernel (IRQ context
 * included), read out through /proc/perf.  An unread counter costs one
 * relaxed atomic add at the site that bumps it — there is deliberately no
 * registration, no locking and no allocation here, so the rig itself can
 * never be the thing the numbers are measuring.
 *
 * Counter semantics are documented next to the enum entry that names
 * them, because a counter whose meaning drifts is worse than no counter
 * (the D1 rule: the number is the claim).
 */
#ifndef KERNEL_LIB_PERFSTAT_H
#define KERNEL_LIB_PERFSTAT_H

#include <stdint.h>

enum perfstat_id {
    /* Ticks (PIT, timer_get_frequency() Hz) from kmain entry to the
     * "[kernel] shell active" line.  Set once per boot, not added. */
    PERF_BOOT_TICKS_TO_SHELL = 0,

    /* Compositor frames that took the full_dirty path (whole scene
     * rendered AND whole screen flipped). */
    PERF_COMPOSITOR_FRAMES_FULL,

    /* Compositor frames that took the dirty-rect path. */
    PERF_COMPOSITOR_FRAMES_PARTIAL,

    /* Pixels COMPOSITED into the back buffer.  As of O0 the partial path
     * still re-composites the entire scene (OPT_PLAN Fact 3), so this
     * counts a full screen for both branches — that honesty is the point:
     * O4's whole job is to make this number track the dirty union. */
    PERF_COMPOSITOR_PIXELS_COMPOSITED,

    /* Pixels FLIPPED back→front.  Already union-clipped on the partial
     * path today (H1), so composited − flipped is O4's headroom. */
    PERF_COMPOSITOR_PIXELS_FLIPPED,

    /* TLB shootdown IPIs serviced with a full CR3 reload — since O5,
     * only the degradation paths land here: scattered-page requests
     * (npages == 0), ranges past TLB_INVLPG_MAX, collapsed-IPI sequence
     * gaps and torn payloads. */
    PERF_TLB_SHOOTDOWNS_FULL,

    /* Shootdown IPIs serviced by an invlpg loop over the mailbox range
     * (OPT_PLAN O5) — the precise path. */
    PERF_TLB_SHOOTDOWNS_RANGED,

    /* IPIs never sent because the target CPU's current CR3 proves it
     * cannot hold stale entries for the affected address space (O5's
     * sender-side filter; architectural fact, not a heuristic — no
     * PCID means a CR3 load is a full flush). */
    PERF_TLB_IPIS_SKIPPED,

    /* Free-list nodes visited by the kernel heap's first-fit search
     * (OPT_PLAN Fact 5).  Since O6 the walk is paid only on size-class
     * MISSES; this counter's collapse is O6's whole claim. */
    PERF_KMALLOC_WALK_STEPS,

    /* kmalloc requests served O(1) from the O6 size-class cache. */
    PERF_KMALLOC_CLASS_HITS,

    /* Bytes pushed through the synchronous busy-wait UART TX path
     * (OPT_PLAN Fact 2).  After O3, this counts only the pre-ring boot
     * banner, ring-full spill and panic/halt bytes. */
    PERF_UART_TX_SYNC_BYTES,

    /* Bytes carried by the O3 TX ring (enqueued and later written from
     * the opportunistic drain or the THRE interrupt).  ring >> sync is
     * the proof the ring took over. */
    PERF_UART_TX_RING_BYTES,

    /* HW_PLAN H4: RESERVED AT ZERO.  CR3 loads that carried bit 63
     * (NOFLUSH) because the target PCID's translations were still
     * valid.  Stays zero until the PCID implementation lands — which
     * waits for a lane that can EXECUTE it (no QEMU configuration
     * here exposes PCID; measured in HW H0).  The counter exists NOW
     * so the first metal/KVM boot has its receipt slot ready. */
    PERF_CR3_NOFLUSH_SWITCHES,

    /* HW_PLAN H4: RESERVED AT ZERO.  12-bit PCID space exhaustions
     * (generation wrap => full flush + re-allocation).  Same deferral,
     * same reason. */
    PERF_PCID_GENERATION_WRAPS,

    PERF_COUNTER_MAX
};

void        perfstat_add(int id, uint64_t n);
void        perfstat_set(int id, uint64_t v);
uint64_t    perfstat_get(int id);
int         perfstat_counter_count(void);
const char *perfstat_name(int id);

#endif /* KERNEL_LIB_PERFSTAT_H */
