/* kernel/arch/riscv64/pmm_rv.c -- bitmap frame allocator (RISCV_PLAN V3).
 *
 * kernel/lib/bitmap.h's third consumer, zero edits to the header (its
 * whole point -- the same algorithms are host-tested in test_pmm.c
 * and drive pmm.c and pmm32.c).  Same shape as pmm32.c at one more
 * pointer width: everything starts used, usable mmap entries open
 * frames, then every non-usable entry closes its range again --
 * order-independent, which matters because V1's shim emits reserved
 * entries BEFORE usable ones (rsvmap first) and kernel/initrd/DTB
 * entries after.
 */

#include <stdint.h>

#include "kernel/arch/riscv64/pmm_rv.h"
#include "kernel/arch/riscv64/sbi.h"
#include "kernel/lib/bitmap.h"

#define FRAME_SIZE 4096ULL
#define MAX_FRAMES (PMM_RV_HORIZON / FRAME_SIZE)   /* 1 Mi frames */

static uint8_t  frame_bitmap[MAX_FRAMES / 8];      /* 128 KiB in .bss */
static uint64_t total_frames;
static uint64_t free_count;

static void put_udec_(uint64_t v)
{
    char buf[20]; int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) sbi_putc(buf[i]);
}

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

void pmm_rv_init(const boot_info_t *bi)
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

    /* Pass 2: every non-usable entry closes its range -- reserved
     * (OpenSBI), kernel image, initrd, the DTB.  Round OUT: a
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

    sbi_puts("[pmm]  frames tracked: ");
    put_udec_(total_frames);
    sbi_puts(", free: ");
    put_udec_(free_count);
    sbi_puts(" (");
    put_udec_(free_count * FRAME_SIZE / (1024 * 1024));
    sbi_puts(" MiB)");
    if (skipped_high) {
        sbi_puts(", skipped above 4 GiB horizon: ");
        put_udec_(skipped_high);
    }
    sbi_puts("\n");
}

uint64_t pmm_rv_alloc_frame(void)
{
    int64_t f = bm_first_free(frame_bitmap, MAX_FRAMES);
    if (f < 0)
        return 0;
    mark_used((uint64_t)f);
    return (uint64_t)f * FRAME_SIZE;
}

void pmm_rv_free_frame(uint64_t phys)
{
    mark_free(phys / FRAME_SIZE);
}

uint64_t pmm_rv_free_frames(void)
{
    return free_count;
}

/* The [pmm] gate: N distinct page-aligned frames out, N back, the
 * free count restored -- pmm32.c's contract verbatim. */
int pmm_rv_selftest(void)
{
    enum { N = 64 };
    uint64_t got[N];
    uint64_t before = free_count;

    for (int i = 0; i < N; i++) {
        got[i] = pmm_rv_alloc_frame();
        if (got[i] == 0 || (got[i] & (FRAME_SIZE - 1)))
            return -1;
        for (int j = 0; j < i; j++)
            if (got[i] == got[j])
                return -1;
    }
    for (int i = 0; i < N; i++)
        pmm_rv_free_frame(got[i]);

    return free_count == before ? 0 : -1;
}
