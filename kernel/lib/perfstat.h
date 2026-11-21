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

    /* HW_PLAN H4 → RESIDUE R11: BACKED by the implementation now
     * (pcid.c counts NOFLUSH re-entries granted by pcid_policy.h).
     * Still zero on every TCG lane — TCG has no PCID (measured at
     * R11: `+pcid` is refused) — so the perf smoke pins zero there
     * and demands movement on a pcid=1 lane (the user's WHPX
     * machine; metal_receipts.md slots 5/6). */
    PERF_CR3_NOFLUSH_SWITCHES,

    /* HW_PLAN H4 → RESIDUE R11: generation bumps (the lazy full
     * flush of the invpcid-less fallback).  Same gating, same
     * receipt slots. */
    PERF_PCID_GENERATION_WRAPS,

    /* REALINTERNET2 Y0: the transport counters.  Retransmissions and
     * RTO events are LIVE from day one (X5's retransmit path counts
     * them); fast retransmits count M6's dup-ACK trigger; the
     * cwnd-limited counter is RESERVED AT ZERO until Y1 makes cwnd a
     * variable (today cwnd == TCP_WINDOW, so the send budget is
     * never cwnd-bound — the H4 pattern: the slot exists before the
     * win). */
    PERF_TCP_RETRANSMITS,
    PERF_TCP_FAST_RETRANSMITS,
    PERF_TCP_RTO_EVENTS,
    PERF_TCP_CWND_LIMITED_SENDS,

    /* ----------------------------------------------------------------
     * GPU_ACCEL_PLAN H1: the present accounting.  Before these eight
     * slots existed the tree could not answer "is the 2D present path
     * faster than not using it?" — /proc/perf had zero GPU counters
     * (measured: `grep -c 'GPU\|VIRGL\|VIRTIO_GPU' perfstat.h` = 0).
     * The names are the wire format; `test_perf_smoke` and
     * `check_gpu_claims.py` both pin them.
     *
     * The three copies per full-screen flip are charged SEPARATELY
     * (H1's whole point): the back→front memcpy, the back→gpu_fb
     * memcpy inside virtio_gpu_present_rect(), and the command pair
     * (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH).
     * ---------------------------------------------------------------- */

    /* gfx_flip() presents that took the full-screen virtio-gpu path.
     * Bumped once per call that reached virtio_gpu_present(). */
    PERF_GPU_PRESENTS_FULL,

    /* gfx_flip_rect() presents that reached virtio_gpu_present_rect().
     * The compositor's dirty-rect path (OPT O4) lands here. */
    PERF_GPU_PRESENTS_RECT,

    /* Pixels handed to the device by TRANSFER_TO_HOST_2D, i.e. the
     * clip-rect area per present.  H2's claim is that a full repaint
     * moves exactly one screen's pixels: 786 432 at 1024×768, not
     * twice that. */
    PERF_GPU_PIXELS_TRANSFERRED,

    /* Bytes copied back_fb → gpu_fb (the driver's own memcpy, separate
     * from the back→front framebuffer copy).  Grows by w×4 per row. */
    PERF_GPU_TRANSFER_BYTES,

    /* Bytes covered by RESOURCE_FLUSH rectangles — the host-side upload
     * the flush asks the device for (same area, counted separately so a
     * future damage-coalescing change shows up as a delta between the
     * two, not as a mystery). */
    PERF_GPU_FLUSH_BYTES,

    /* Presents that fell back to the plain framebuffer path because the
     * device was absent, not ready, or the command failed.  This is the
     * counter that makes D2 auditable: a "2D acceleration works" claim
     * with a non-zero value here is a claim about the fallback. */
    PERF_GPU_PRESENT_FALLBACKS,

    /* Bounded busy-waits in submit_2desc() that FOUND the used ring
     * advanced before the deadline (the common case). */
    PERF_GPU_CONTROL_QUEUE_WAITS,

    /* Busy-waits that hit the 10 000 000-iteration deadline.  A non-zero
     * value is a stalled device, and Fact 5's cursor-queue decision
     * turns on whether this stays at zero. */
    PERF_GPU_CONTROL_QUEUE_TIMEOUTS,

    PERF_COUNTER_MAX
};

void        perfstat_add(int id, uint64_t n);
void        perfstat_set(int id, uint64_t v);
uint64_t    perfstat_get(int id);
int         perfstat_counter_count(void);
const char *perfstat_name(int id);

#endif /* KERNEL_LIB_PERFSTAT_H */
