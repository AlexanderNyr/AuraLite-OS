/* pmm.c — bitmap physical memory manager.
 *
 * Initialisation consumes the Limine memory map: the highest usable address
 * fixes the bitmap size, the bitmap is carved out of memory (preferring
 * bootloader-reclaimable RAM so we do not shrink usable RAM), everything is
 * marked used, then only USABLE regions are marked free. Allocations and frees
 * are serialised by an interrupt-saving spinlock.
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel/mm/pmm.h"
#include "kernel/lib/bitmap.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/boot_info.h"

#define PMM_TAG "[pmm] "

#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define MIB            (1024ULL * 1024ULL)
#define GIB            (1024ULL * MIB)

/* BIOS Stage 2 initially maps the HHDM only for physical 0..4 GiB.  Keep
 * early PMM metadata inside that range until the kernel builds its own maps. */
#define PMM_EARLY_HHDM_LIMIT (4ULL * GIB)

/* Keep low boot-critical memory out of the allocator.  This covers the BIOS
 * loader, boot_info_t, the loaded kernel image, scratch buffers, and the page
 * tables built by Stage 2 at 16 MiB. */
/* Reserve the low 40 MiB for boot-critical data the loader placed there.
 *
 * The BIOS Stage 2 layout puts initrd.tar at 24 MiB
 * (INITRD_LOAD_PHYS in boot/bios/stage2/stage2_start.asm), so the reserve
 * ceiling is what caps the archive: at 32 MiB the slot was exactly 8 MiB,
 * and the initrd had grown to within ~12 KiB of that, so a single added
 * userspace binary (or merely a compiler that emits slightly larger code
 * on another host) overflowed it and failed `make iso` in CI.  40 MiB
 * gives the archive a 16 MiB slot -- roughly double the current 8.0 MiB
 * -- while still costing well under 10% of the smallest configuration we
 * boot (512 MiB).  Everything else living in the reserve (the SMP
 * trampoline at 0x7000/0x8000, the kernel image at 1 MiB, its staging
 * buffer at 2 MiB, and the boot page tables at 16 MiB) sits below 24 MiB
 * and is unaffected. */
#define PMM_EARLY_BOOT_RESERVE (40ULL * MIB)

struct pmm_state {
    uint8_t  *bitmap;             /* HHDM-mapped pointer to the bitmap        */
    uint64_t  nframes;            /* bits (frames) the bitmap holds           */
    uint64_t  bitmap_bytes;       /* size of the bitmap in bytes              */
    uint64_t  usable_frames;      /* total USABLE frames (allocatable pool)   */
    uint64_t  free_frames;        /* currently free (allocatable) frames      */
    uint64_t  bitmap_phys;        /* physical base of the bitmap              */
    uint64_t  bitmap_frames;      /* frames the bitmap occupies               */
    uint32_t *refcount;           /* per-frame sharing count for COW pages    */
    uint64_t  refcount_bytes;     /* size of refcount array in bytes          */
    uint64_t  refcount_phys;      /* physical base of refcount array          */
    uint64_t  refcount_frames;    /* frames the refcount array occupies       */
    uint64_t  hhdm;               /* cached direct-map offset                 */
    spinlock_t lock;
};

static struct pmm_state pmm;

/* Mark all frames overlapping [base, base+len) as used(1) or free(0). */
static void mark_range(uint64_t base, uint64_t len, int used) {
    uint64_t start = ALIGN_UP(base, PMM_PAGE_SIZE) >> PMM_PAGE_SHIFT;
    uint64_t end   = (base + len) >> PMM_PAGE_SHIFT;          /* floor */
    for (uint64_t f = start; f < end && f < pmm.nframes; f++) {
        if (used) {
            bm_set(pmm.bitmap, f);
        } else {
            bm_clear(pmm.bitmap, f);
        }
    }
}

/* Locate a region large enough to hold the bitmap; prefer bootloader-
   reclaimable memory, fall back to usable memory.  Allocate from the top of the
   chosen range so PMM metadata does not overwrite the low BIOS kernel image or
   Stage 2 page tables. */
static uint64_t find_bitmap_region(boot_mmap_entry_t *entries,
                                   uint64_t count) {
    uint64_t need = ALIGN_UP(pmm.bitmap_bytes + pmm.refcount_bytes,
                             PMM_PAGE_SIZE);

    for (int pass = 0; pass < 2; pass++) {
        uint32_t want_type = (pass == 0)
            ? BOOT_MEM_BOOTLOADER
            : BOOT_MEM_USABLE;
        for (uint64_t i = 0; i < count; i++) {
            if (entries[i].type != want_type) {
                continue;
            }

            uint64_t start = ALIGN_UP(entries[i].base, PMM_PAGE_SIZE);
            uint64_t end = ALIGN_DOWN(entries[i].base + entries[i].length,
                                      PMM_PAGE_SIZE);
            if (end > PMM_EARLY_HHDM_LIMIT) {
                end = PMM_EARLY_HHDM_LIMIT;
            }
            if (end <= start || end - start < need) {
                continue;
            }

            return end - need;
        }
    }
    return 0;
}

void pmm_init(void) {
    uint64_t entry_count = 0;
    boot_mmap_entry_t *entries = boot_get_memmap(&entry_count);
    uint64_t hhdm = boot_get_hhdm_offset();

    if (entries == NULL || hhdm == 0 || entry_count == 0) {
        kprintf(PMM_TAG "FATAL: no memory map or HHDM available\n");
        return;
    }

    /* 1) Highest usable address + total usable bytes size the bitmap. */
    uint64_t highest = 0;
    uint64_t usable_bytes = 0;
    for (uint64_t i = 0; i < entry_count; i++) {
        boot_mmap_entry_t *e = &entries[i];
        if (e->type == BOOT_MEM_USABLE) {
            uint64_t top = e->base + e->length;
            if (top > highest) {
                highest = top;
            }
            usable_bytes += e->length;
        }
    }
    pmm.nframes       = ALIGN_UP(highest, PMM_PAGE_SIZE) >> PMM_PAGE_SHIFT;
    pmm.bitmap_bytes  = ALIGN_UP(pmm.nframes / 8, PMM_PAGE_SIZE);
    pmm.refcount_bytes = ALIGN_UP(pmm.nframes * sizeof(uint32_t), PMM_PAGE_SIZE);
    pmm.usable_frames = usable_bytes >> PMM_PAGE_SHIFT;
    if (pmm.nframes == 0) {
        kprintf(PMM_TAG "FATAL: no usable memory detected\n");
        return;
    }

    /* 2) Place the bitmap in memory (preferring bootloader-reclaimable). */
    pmm.hhdm        = hhdm;
    pmm.bitmap_phys = find_bitmap_region(entries, entry_count);
    if (pmm.bitmap_phys == 0) {
        kprintf(PMM_TAG "FATAL: no region large enough for the bitmap (%llu B)\n",
                (unsigned long long)pmm.bitmap_bytes);
        return;
    }
    pmm.bitmap_frames = pmm.bitmap_bytes >> PMM_PAGE_SHIFT;
    pmm.bitmap        = (uint8_t *)(uintptr_t)(hhdm + pmm.bitmap_phys);
    pmm.refcount_phys = pmm.bitmap_phys + pmm.bitmap_bytes;
    pmm.refcount_frames = pmm.refcount_bytes >> PMM_PAGE_SHIFT;
    pmm.refcount      = (uint32_t *)(uintptr_t)(hhdm + pmm.refcount_phys);
    memset(pmm.refcount, 0, pmm.refcount_bytes);

    /* 3) Start with everything marked used, then clear USABLE regions. */
    memset(pmm.bitmap, 0xFF, pmm.bitmap_bytes);
    for (uint64_t i = 0; i < entry_count; i++) {
        boot_mmap_entry_t *e = &entries[i];
        if (e->type == BOOT_MEM_USABLE) {
            mark_range(e->base, e->length, 0);
        }
    }
    /* 4) Reserve early boot-critical frames and PMM metadata frames
     *    (idempotent if non-usable). */
    mark_range(0, PMM_EARLY_BOOT_RESERVE, 1);
    mark_range(pmm.bitmap_phys, pmm.bitmap_bytes + pmm.refcount_bytes, 1);

    /* 5) Count free frames straight from the bitmap. */
    pmm.free_frames = 0;
    for (uint64_t i = 0; i < pmm.nframes; i++) {
        if (!bm_test(pmm.bitmap, i)) {
            pmm.free_frames++;
        }
    }

    spinlock_init(&pmm.lock);
    pmm_dump_stats();
}

void pmm_dump_stats(void) {
    kprintf(PMM_TAG "bitmap at phys 0x%016llx, %llu bytes (%llu frames)\n",
            (unsigned long long)pmm.bitmap_phys,
            (unsigned long long)pmm.bitmap_bytes,
            (unsigned long long)pmm.bitmap_frames);
    kprintf(PMM_TAG "refcnt at phys 0x%016llx, %llu bytes (%llu frames)\n",
            (unsigned long long)pmm.refcount_phys,
            (unsigned long long)pmm.refcount_bytes,
            (unsigned long long)pmm.refcount_frames);
    kprintf(PMM_TAG "tracked frames: %llu (%llu MiB)\n",
            (unsigned long long)pmm.nframes,
            (unsigned long long)(pmm.nframes * PMM_PAGE_SIZE / MIB));
    kprintf(PMM_TAG "usable frames: %llu (%llu MiB)\n",
            (unsigned long long)pmm.usable_frames,
            (unsigned long long)(pmm.usable_frames * PMM_PAGE_SIZE / MIB));
    kprintf(PMM_TAG "free frames:   %llu (%llu MiB)\n",
            (unsigned long long)pmm.free_frames,
            (unsigned long long)(pmm.free_frames * PMM_PAGE_SIZE / MIB));
}

uint64_t pmm_alloc_frame(void) {
    uint64_t rflags = spinlock_acquire_irqsave(&pmm.lock);
    int64_t idx = bm_first_free(pmm.bitmap, pmm.nframes);
    if (idx < 0) {
        spinlock_release_irqrestore(&pmm.lock, rflags);
        return 0;                                          /* out of memory */
    }
    bm_set(pmm.bitmap, (uint64_t)idx);
    pmm.refcount[(uint64_t)idx] = 1;
    pmm.free_frames--;
    spinlock_release_irqrestore(&pmm.lock, rflags);
    uint64_t phys = (uint64_t)idx << PMM_PAGE_SHIFT;
    memset((void *)(uintptr_t)(pmm.hhdm + phys), 0, PMM_PAGE_SIZE);
    return phys;
}

uint64_t pmm_alloc_contiguous(uint64_t count) {
    uint64_t rflags = spinlock_acquire_irqsave(&pmm.lock);
    int64_t idx = bm_find_contiguous(pmm.bitmap, pmm.nframes, count);
    if (idx < 0) {
        spinlock_release_irqrestore(&pmm.lock, rflags);
        return 0;
    }
    for (uint64_t i = 0; i < count; i++) {
        bm_set(pmm.bitmap, (uint64_t)idx + i);
        pmm.refcount[(uint64_t)idx + i] = 1;
    }
    pmm.free_frames -= count;
    spinlock_release_irqrestore(&pmm.lock, rflags);
    uint64_t phys = (uint64_t)idx << PMM_PAGE_SHIFT;
    memset((void *)(uintptr_t)(pmm.hhdm + phys), 0, count * PMM_PAGE_SIZE);
    return phys;
}

void pmm_free_contiguous(uint64_t phys, uint64_t count) {
    if (!phys || !count) return;
    /* Frame-by-frame through pmm_free_frame(), so the refcount handling
     * (shared pages from copy-on-write fork) stays in exactly one place. */
    for (uint64_t i = 0; i < count; i++) {
        pmm_free_frame(phys + i * PMM_PAGE_SIZE);
    }
}

void pmm_free_frame(uint64_t phys) {
    uint64_t idx = phys >> PMM_PAGE_SHIFT;
    if (idx == 0 || idx >= pmm.nframes) {
        kprintf(PMM_TAG "free: invalid physical address 0x%016llx\n",
                (unsigned long long)phys);
        return;
    }
    uint64_t rflags = spinlock_acquire_irqsave(&pmm.lock);
    if (!bm_test(pmm.bitmap, idx)) {
        kprintf(PMM_TAG "free: double free of 0x%016llx\n",
                (unsigned long long)phys);
        spinlock_release_irqrestore(&pmm.lock, rflags);
        return;
    }
    if (pmm.refcount[idx] > 1) {
        pmm.refcount[idx]--;
        spinlock_release_irqrestore(&pmm.lock, rflags);
        return;
    }
    pmm.refcount[idx] = 0;
    bm_clear(pmm.bitmap, idx);
    pmm.free_frames++;
    spinlock_release_irqrestore(&pmm.lock, rflags);
}

int pmm_inc_frame_ref(uint64_t phys) {
    uint64_t idx = phys >> PMM_PAGE_SHIFT;
    if (idx == 0 || idx >= pmm.nframes) {
        kprintf(PMM_TAG "ref: invalid physical address 0x%016llx\n",
                (unsigned long long)phys);
        return -1;
    }
    uint64_t rflags = spinlock_acquire_irqsave(&pmm.lock);
    if (!bm_test(pmm.bitmap, idx) || pmm.refcount[idx] == 0) {
        kprintf(PMM_TAG "ref: frame 0x%016llx is not allocated\n",
                (unsigned long long)phys);
        spinlock_release_irqrestore(&pmm.lock, rflags);
        return -1;
    }
    pmm.refcount[idx]++;
    spinlock_release_irqrestore(&pmm.lock, rflags);
    return 0;
}

uint32_t pmm_get_frame_refcount(uint64_t phys) {
    uint64_t idx = phys >> PMM_PAGE_SHIFT;
    if (idx == 0 || idx >= pmm.nframes) return 0;
    uint64_t rflags = spinlock_acquire_irqsave(&pmm.lock);
    uint32_t rc = pmm.refcount[idx];
    spinlock_release_irqrestore(&pmm.lock, rflags);
    return rc;
}

uint64_t pmm_get_free_frames(void)   { return pmm.free_frames; }
uint64_t pmm_get_usable_frames(void) { return pmm.usable_frames; }

void pmm_self_test(void) {
    enum { N = 1000 };
    static uint64_t addrs[N];          /* static: keep it off the stack */
    uint64_t before = pmm.free_frames;

    kprintf(PMM_TAG "self-test: allocating %d frames...\n", N);

    /* Allocate and validate each frame: non-null, page-aligned. */
    for (int i = 0; i < N; i++) {
        addrs[i] = pmm_alloc_frame();
        if (addrs[i] == 0) {
            kprintf(PMM_TAG "FAIL: out of memory at index %d\n", i);
            return;
        }
        if (addrs[i] & (PMM_PAGE_SIZE - 1)) {
            kprintf(PMM_TAG "FAIL: address 0x%016llx not page-aligned\n",
                    (unsigned long long)addrs[i]);
            return;
        }
    }

    /* Uniqueness: O(N^2) is fine for N=1000 and needs no allocator. */
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            if (addrs[i] == addrs[j]) {
                kprintf(PMM_TAG "FAIL: duplicate address 0x%016llx (%d/%d)\n",
                        (unsigned long long)addrs[i], i, j);
                return;
            }
        }
    }

    /* Free everything; the pool must return to its starting level. */
    for (int i = 0; i < N; i++) {
        pmm_free_frame(addrs[i]);
    }

    /* Contiguous allocation: 4 frames, base page-aligned, fully returned. */
    uint64_t base = pmm_alloc_contiguous(4);
    if (base == 0) {
        kprintf(PMM_TAG "FAIL: contiguous alloc of 4 frames returned OOM\n");
        return;
    }
    if (base & (PMM_PAGE_SIZE - 1)) {
        kprintf(PMM_TAG "FAIL: contiguous base 0x%016llx not aligned\n",
                (unsigned long long)base);
        return;
    }
    for (int i = 0; i < 4; i++) {
        pmm_free_frame(base + (uint64_t)i * PMM_PAGE_SIZE);
    }

    uint64_t after = pmm.free_frames;
    if (after != before) {
        kprintf(PMM_TAG "FAIL: frame leak (before=%llu, after=%llu)\n",
                (unsigned long long)before, (unsigned long long)after);
        return;
    }

    kprintf(PMM_TAG "PASS: %d unique frames, no leak, contiguous alloc OK\n", N);
}
