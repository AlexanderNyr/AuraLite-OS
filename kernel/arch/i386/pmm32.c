/* kernel/arch/i386/pmm32.c -- bitmap frame allocator (I386_PLAN I3).
 *
 * Mirrors kernel/mm/pmm.c at bring-up scope: one bit per 4 KiB frame
 * over kernel/lib/bitmap.h (the identical, host-tested header), an
 * early-boot low reserve, and a boot self-test with the same PASS
 * contract.  Refcounting (COW's requirement) arrives with fork in I4+.
 *
 * Width discipline (plan D6): E820 walks in uint64_t, allocatable
 * frames in uint32_t -- an entry above the 4 GiB horizon is skipped
 * with a log line, never truncated.
 */

#include <stdint.h>

#include "kernel/arch/i386/pmm32.h"
#include "kernel/arch/i386/kprintf32.h"
#include "kernel/lib/bitmap.h"

/* Same reasoning as pmm.c's PMM_EARLY_BOOT_RESERVE: the loader parked
 * the kernel image, boot_info and the initrd below 40 MiB; keep all of
 * it allocated until the VFS has copied what it needs. */
#define EARLY_RESERVE (40u * 1024u * 1024u)

/* The bitmap lives in .bss: 896 MiB / 4 KiB = 229376 frames = 28 KiB.
 * Static rather than heap-carved because the heap does not exist yet
 * (kheap32 sits above the PMM in the init order, exactly as on x86_64). */
#define MAX_FRAMES (PMM32_HORIZON / PAGE_SIZE_32)

static uint8_t  frame_bitmap[MAX_FRAMES / 8];
static uint32_t total_frames;   /* tracked (below horizon and in E820)   */
static uint32_t free_count;

static void mark_used(uint32_t frame)
{
    if (!bm_test(frame_bitmap, frame)) {
        bm_set(frame_bitmap, frame);
        free_count--;
    }
}

static void mark_free(uint32_t frame)
{
    if (bm_test(frame_bitmap, frame)) {
        bm_clear(frame_bitmap, frame);
        free_count++;
    }
}

void pmm32_init(const boot_info_t *bi)
{
    /* Everything starts used; E820-usable ranges open frames up. */
    for (uint32_t i = 0; i < sizeof(frame_bitmap); i++)
        frame_bitmap[i] = 0xFF;
    total_frames = 0;
    free_count   = 0;

    uint32_t skipped_high = 0;

    for (uint32_t i = 0; i < bi->mmap_count; i++) {
        const boot_mmap_entry_t *e = &bi->mmap[i];
        if (e->type != BOOT_MEM_USABLE)
            continue;

        uint64_t base = e->base;
        uint64_t end  = e->base + e->length;

        if (base >= PMM32_HORIZON) {
            skipped_high++;
            continue;                    /* skipped, not truncated (D6) */
        }
        if (end > PMM32_HORIZON)
            end = PMM32_HORIZON;

        /* Frame-align inward. */
        uint32_t first = (uint32_t)((base + PAGE_SIZE_32 - 1) / PAGE_SIZE_32);
        uint32_t last  = (uint32_t)(end / PAGE_SIZE_32);

        for (uint32_t f = first; f < last; f++) {
            if (bm_test(frame_bitmap, f)) {
                bm_clear(frame_bitmap, f);
                free_count++;
                total_frames++;
            }
        }
    }

    /* Low reserve: loader artefacts + this kernel + the initrd. */
    for (uint32_t f = 0; f < EARLY_RESERVE / PAGE_SIZE_32; f++) {
        if (!bm_test(frame_bitmap, f)) {
            bm_set(frame_bitmap, f);
            free_count--;
        }
    }

    kprintf32("[pmm] bitmap in .bss, %u bytes; tracked frames: %u (%u MiB)\n",
              (uint32_t)sizeof(frame_bitmap), total_frames,
              total_frames / 256);
    kprintf32("[pmm] free frames:   %u (%u MiB); low %u MiB reserved\n",
              free_count, free_count / 256, EARLY_RESERVE / (1024 * 1024));
    if (skipped_high)
        kprintf32("[pmm] %u E820 region(s) above the %u MiB horizon skipped\n",
                  skipped_high, PMM32_HORIZON / (1024 * 1024));
}

uint32_t pmm32_alloc_frame(void)
{
    int64_t idx = bm_first_free(frame_bitmap, MAX_FRAMES);
    if (idx < 0)
        return 0;
    mark_used((uint32_t)idx);
    return (uint32_t)idx * PAGE_SIZE_32;
}

void pmm32_free_frame(uint32_t phys)
{
    mark_free(phys / PAGE_SIZE_32);
}

uint32_t pmm32_free_frames(void)  { return free_count;   }
uint32_t pmm32_total_frames(void) { return total_frames; }

/* Same shape as pmm.c's boot self-test: allocate a batch, check
 * uniqueness and alignment, free, check no leak. */
#define SELFTEST_N 1000

int pmm32_selftest(void)
{
    static uint32_t got[SELFTEST_N];
    uint32_t before = free_count;

    kprintf32("[pmm] self-test: allocating %u frames...\n", (uint32_t)SELFTEST_N);

    for (int i = 0; i < SELFTEST_N; i++) {
        got[i] = pmm32_alloc_frame();
        if (got[i] == 0 || (got[i] & (PAGE_SIZE_32 - 1)) != 0)
            return -1;
        for (int j = 0; j < i; j++)
            if (got[j] == got[i])
                return -1;               /* duplicate */
    }

    for (int i = 0; i < SELFTEST_N; i++)
        pmm32_free_frame(got[i]);

    if (free_count != before)
        return -1;                       /* leak */

    kprintf32("[pmm] PASS: %u unique aligned frames, no leak\n",
              (uint32_t)SELFTEST_N);
    return 0;
}
