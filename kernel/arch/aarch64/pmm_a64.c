/* kernel/arch/aarch64/pmm_a64.c -- bitmap frame allocator (ARM64_PLAN A3).
 *
 * kernel/lib/bitmap.h's FOURTH consumer, zero edits to the header --
 * the same algorithms are host-tested in test_pmm.c and drive pmm.c,
 * pmm32.c and pmm_rv.c.  Same shape as pmm_rv.c because it is the
 * same problem: everything starts used, usable mmap entries open
 * frames, then every non-usable entry closes its range again --
 * order-independent, which matters because the shared walker emits
 * reserved entries BEFORE usable ones (rsvmap first) and
 * kernel/initrd/DTB entries after.
 */

#include <stdint.h>

#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/pmm_a64.h"
#include "kernel/lib/bitmap.h"

#define FRAME_SIZE 4096ULL
#define MAX_FRAMES (PMM_A64_HORIZON / FRAME_SIZE)   /* 1 Mi frames */

static uint8_t  frame_bitmap[MAX_FRAMES / 8];       /* 128 KiB in .bss */
static uint64_t total_frames;
static uint64_t free_count;

static void mark_used(uint64_t frame)
{
    if (!bm_test(frame_bitmap, frame)) {
        bm_set(frame_bitmap, frame);
        free_count--;
    }
}

static void mark_free(uint64_t frame)
{
    if (bm_test(frame_bitmap, frame)) {
        bm_clear(frame_bitmap, frame);
        free_count++;
    }
}

void pmm_a64_init(const boot_info_t *bi)
{
    for (uint64_t i = 0; i < sizeof(frame_bitmap); i++)
        frame_bitmap[i] = 0xFF;
    total_frames = 0;
    free_count   = 0;

    uint64_t skipped_high = 0;

    /* Pass 1: usable RAM opens frames. */
    for (uint32_t i = 0; i < bi->mmap_count; i++) {
        const boot_mmap_entry_t *e = &bi->mmap[i];
        if (e->type != BOOT_MEM_USABLE)
            continue;
        uint64_t start = (e->base + FRAME_SIZE - 1) / FRAME_SIZE;
        uint64_t end   = (e->base + e->length) / FRAME_SIZE;
        for (uint64_t f = start; f < end; f++) {
            if (f >= MAX_FRAMES) { skipped_high++; continue; }
            if (bm_test(frame_bitmap, f)) {
                bm_clear(frame_bitmap, f);
                free_count++;
                total_frames++;
            }
        }
    }

    /* Pass 2: every non-usable entry closes its range -- the kernel
     * image, the DTB, whatever the tree reserved.  Round OUT: a
     * partial frame of firmware is a used frame. */
    for (uint32_t i = 0; i < bi->mmap_count; i++) {
        const boot_mmap_entry_t *e = &bi->mmap[i];
        if (e->type == BOOT_MEM_USABLE)
            continue;
        uint64_t start = e->base / FRAME_SIZE;
        uint64_t end   = (e->base + e->length + FRAME_SIZE - 1) / FRAME_SIZE;
        for (uint64_t f = start; f < end && f < MAX_FRAMES; f++)
            mark_used(f);
    }

    pl011_puts("[pmm]  frames tracked: ");
    pl011_putdec64(total_frames);
    pl011_puts(", free: ");
    pl011_putdec64(free_count);
    pl011_puts(" (");
    pl011_putdec64(free_count * FRAME_SIZE / (1024 * 1024));
    pl011_puts(" MiB)");
    if (skipped_high) {
        pl011_puts(", skipped above 4 GiB horizon: ");
        pl011_putdec64(skipped_high);
    }
    pl011_puts("\n");
}

uint64_t pmm_a64_alloc_frame(void)
{
    int64_t f = bm_first_free(frame_bitmap, MAX_FRAMES);
    if (f < 0)
        return 0;
    mark_used((uint64_t)f);
    return (uint64_t)f * FRAME_SIZE;
}

void pmm_a64_free_frame(uint64_t phys)
{
    mark_free(phys / FRAME_SIZE);
}

uint64_t pmm_a64_free_frames(void)
{
    return free_count;
}

/* The [pmm] gate: N distinct page-aligned frames out, N back, the
 * free count restored -- the pmm32/pmm_rv contract verbatim. */
int pmm_a64_selftest(void)
{
    enum { N = 64 };
    uint64_t got[N];
    uint64_t before = free_count;

    for (int i = 0; i < N; i++) {
        got[i] = pmm_a64_alloc_frame();
        if (got[i] == 0 || (got[i] & (FRAME_SIZE - 1)))
            return -1;
        for (int j = 0; j < i; j++)
            if (got[i] == got[j])
                return -1;
    }
    for (int i = 0; i < N; i++)
        pmm_a64_free_frame(got[i]);

    return free_count == before ? 0 : -1;
}
