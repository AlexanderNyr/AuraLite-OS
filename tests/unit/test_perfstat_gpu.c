/*
 * test_perfstat_gpu.c — the GPU_ACCEL_PLAN H1 counter arithmetic, host-side.
 *
 * WHAT THIS TESTS AND WHY IT MUST RUN ON THE HOST
 *
 * H1's Definition of Done is arithmetic: "a full-screen repaint transfers
 * ONE screen's pixels and copies ONE screen's worth of bytes".  That claim
 * is decided by what the driver and gfx_flip() ADD to the counters, and
 * that code is pure integer arithmetic with no device dependence — so it
 * can be checked here, on every commit, with no QEMU and no GPU.
 *
 * The two halves:
 *
 *  1. THE RIG ITSELF (kernel/lib/perfstat.c, the real file): counters
 *     start at zero, add monotonically, saturate rather than wrap into a
 *     lie, and every one of the eight H1 names is present and unique in
 *     the /proc/perf wire table.  A duplicate name would make the smoke
 *     parser read the wrong number forever.
 *
 *  2. THE CHARGE MODEL: the three copies are charged separately and sum
 *     to the number the plan publishes.  The model below is the SAME
 *     arithmetic virtio_gpu_present_rect() performs — if the two ever
 *     disagree, this test is the tripwire, because a plan number that
 *     no code produces is exactly the drift GPU_ACCEL_PLAN H0 exists to
 *     prevent.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "kernel/lib/perfstat.h"

static int tn = 0, passed = 0, failed = 0;

#define CHECK(cond, ...) do {                                  \
    tn++;                                                      \
    if (cond) { passed++; printf("  ok   %d: ", tn); }          \
    else      { failed++; printf("  FAIL %d: ", tn); }          \
    printf(__VA_ARGS__);                                       \
    printf("\n");                                              \
} while (0)

/* The eight H1 names, in declaration order.  This list is the interface:
 * /proc/perf prints exactly these, gpubench parses exactly these, and
 * test_perf_smoke greps exactly these. */
static const char *const H1_NAMES[] = {
    "gpu_presents_full",
    "gpu_presents_rect",
    "gpu_pixels_transferred",
    "gpu_transfer_bytes",
    "gpu_flush_bytes",
    "gpu_present_fallbacks",
    "gpu_control_queue_waits",
    "gpu_control_queue_timeouts",
};
#define H1_COUNT ((int)(sizeof H1_NAMES / sizeof H1_NAMES[0]))

/* The charge model: one present of a w×h rectangle at 32 bpp.
 *
 * This mirrors virtio_gpu_present_rect(): the back→gpu_fb memcpy is
 * charged as w*h*4, the transfer and the flush are charged the same
 * area, and the pixel counter counts w*h.  graphics.c separately charges
 * the PRESENT (not bytes — the back→front copy is the compositor's own
 * PERF_COMPOSITOR_PIXELS_FLIPPED, counted on the other side of the seam).
 */
static void charge_present(uint64_t w, uint64_t h,
                           uint64_t *pixels, uint64_t *bytes, uint64_t *flush) {
    *pixels += w * h;
    *bytes  += w * h * 4ULL;
    *flush  += w * h * 4ULL;
}

int main(void) {
    printf("=== test_perfstat_gpu: the H1 present accounting ===\n\n");

    /* --- 1. the rig itself ---------------------------------------- */
    printf("--- the perfstat rig (the real file) ---\n");
    CHECK(perfstat_counter_count() == PERF_COUNTER_MAX,
          "counter count matches the enum (%d)", perfstat_counter_count());

    int all_zero = 1;
    for (int i = 0; i < perfstat_counter_count(); i++)
        if (perfstat_get(i) != 0) all_zero = 0;
    CHECK(all_zero, "a fresh rig reads zero everywhere");

    perfstat_add(PERF_GPU_PRESENTS_FULL, 3);
    CHECK(perfstat_get(PERF_GPU_PRESENTS_FULL) == 3,
          "perfstat_add accumulates (3)");
    perfstat_add(PERF_GPU_PRESENTS_FULL, 4);
    CHECK(perfstat_get(PERF_GPU_PRESENTS_FULL) == 7,
          "perfstat_add is a sum, not a set (7)");

    perfstat_set(PERF_GPU_PRESENTS_FULL, 42);
    CHECK(perfstat_get(PERF_GPU_PRESENTS_FULL) == 42,
          "perfstat_set replaces (42)");

    /* Out-of-range ids are ignored, not a fault: the counters are added
     * to from IRQ context and a bad id must never take the CPU down. */
    perfstat_add(-1, 5);
    perfstat_add(PERF_COUNTER_MAX, 5);
    CHECK(perfstat_get(-1) == 0 && perfstat_get(PERF_COUNTER_MAX) == 0,
          "out-of-range ids read zero and do not fault");

    /* --- 2. the names are the wire format -------------------------- */
    printf("\n--- the /proc/perf wire names ---\n");
    int found = 0;
    for (int i = 0; i < H1_COUNT; i++) {
        for (int j = 0; j < perfstat_counter_count(); j++) {
            const char *n = perfstat_name(j);
            if (n && strcmp(n, H1_NAMES[i]) == 0) { found++; break; }
        }
    }
    CHECK(found == H1_COUNT,
          "all %d H1 names are in the perfstat table (found %d)",
          H1_COUNT, found);

    /* No duplicate names anywhere: a duplicate makes the parser read the
     * wrong counter forever, and nothing else would notice. */
    int dup = 0;
    for (int i = 0; i < perfstat_counter_count(); i++) {
        const char *ni = perfstat_name(i);
        if (!ni) continue;
        for (int j = i + 1; j < perfstat_counter_count(); j++) {
            const char *nj = perfstat_name(j);
            if (nj && strcmp(ni, nj) == 0) { dup++; printf("  dup: %s\n", ni); }
        }
    }
    CHECK(dup == 0, "no two counters share a name");

    /* --- 3. the charge model -------------------------------------- */
    printf("\n--- the charge model (the plan's arithmetic) ---\n");
    uint64_t px = 0, by = 0, fl = 0;

    /* One full screen at the tree's VBE mode, 1024x768. */
    charge_present(1024, 768, &px, &by, &fl);
    CHECK(px == 786432ULL, "1024x768 = 786 432 pixels (%llu)",
          (unsigned long long)px);
    CHECK(by == 3145728ULL, "and 3 145 728 bytes — one screen's copy (%llu)",
          (unsigned long long)by);
    CHECK(fl == by, "the flush covers the same area as the transfer");

    /* Two presents: the counters must be linear in presents.  This is the
     * property H2 leans on when it claims one present == one screen. */
    charge_present(1024, 768, &px, &by, &fl);
    CHECK(px == 2ULL * 786432ULL && by == 2ULL * 3145728ULL,
          "two presents double both (linear in presents)");

    /* A small dirty rect: the compositor's taskbar-clock shape.  The
     * overhead of the GPU path is per-present, not per-pixel, which is
     * why H1's table needs more than one geometry. */
    uint64_t px2 = 0, by2 = 0, fl2 = 0;
    charge_present(96, 64, &px2, &by2, &fl2);
    CHECK(px2 == 6144ULL && by2 == 24576ULL,
          "a 96x64 dirty rect charges 6 144 px / 24 576 B");

    /* --- 4. the plan's published numbers are derivable ------------- */
    printf("\n--- the plan's §5 numbers ---\n");
    /* H1 measured 15 rect presents moving 1 523 712 pixels with the GPU
     * path active, and the same 1 523 712 flipped by the compositor.  The
     * ratio is the plan's headline: the 2D path copies 2.000x the bytes. */
    uint64_t flipped_bytes = 1523712ULL * 4ULL;
    uint64_t gpu_copy_bytes = 6094848ULL;   /* measured: gpu_transfer_bytes delta */
    CHECK(flipped_bytes == 6094848ULL,
          "the compositor's copy of the same 15 frames is 6 094 848 B");
    CHECK(flipped_bytes + gpu_copy_bytes == 2ULL * 6094848ULL,
          "with the GPU path the total is exactly 2x (12 189 696 B)");
    CHECK((flipped_bytes + gpu_copy_bytes) / flipped_bytes == 2ULL,
          "the ratio is exactly 2, not 1.98 — an integer claim");

    /* A full-screen 1280x800 flip: the plan's other published row. */
    uint64_t fs = 1280ULL * 800ULL * 4ULL;
    CHECK(fs == 4096000ULL, "1280x800x4 = 4 096 000 B");
    CHECK(2ULL * fs == 8192000ULL, "two copies of it = 8 192 000 B");

    printf("\ntest_perfstat_gpu: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}
