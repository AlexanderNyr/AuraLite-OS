#ifndef AURALITE_MM_PMM_H
#define AURALITE_MM_PMM_H

#include <stdint.h>

/*
 * Physical Memory Manager.
 *
 * Tracks every physical 4 KiB frame with a bitmap: bit SET = in use, bit
 * CLEAR = free. The bitmap itself is carved out of a memory region and reached
 * through Limine's higher-half direct map (HHDM).
 *
 * Return-value convention: the null physical address (0) is never handed out,
 * because frame 0 (BIOS/IVT region) is reserved. Therefore a return of 0 from
 * pmm_alloc_frame / pmm_alloc_contiguous means OUT OF MEMORY.
 */

#define PMM_PAGE_SIZE  4096
#define PMM_PAGE_SHIFT 12

/* Build the bitmap from the Limine memory map and print statistics. */
void pmm_init(void);

/*
 * Allocate one free frame.
 *
 * Returned frames are scrubbed to zero before being handed to the caller so
 * stale contents from prior kernel/user allocations are not re-exposed.
 *
 * @returns its physical address (page aligned), or 0 if out of memory.
 */
uint64_t pmm_alloc_frame(void);

/*
 * Allocate `count` physically-contiguous free frames.
 * @returns the base physical address, or 0 if no run is available.
 */
uint64_t pmm_alloc_contiguous(uint64_t count);

/*
 * Release a frame previously obtained from pmm_alloc_frame. Logging a warning
 * (and ignoring) on a bad address or double free.
 */
void pmm_free_frame(uint64_t phys);

/* Increase/decrease/read the sharing reference count for an allocated frame.
 * Used by copy-on-write fork(). pmm_free_frame() is refcount-aware: it only
 * returns the frame to the free bitmap when the count reaches zero. */
int      pmm_inc_frame_ref(uint64_t phys);
uint32_t pmm_get_frame_refcount(uint64_t phys);

/* Live counters for /proc-style reporting and tests. */
uint64_t pmm_get_free_frames(void);
uint64_t pmm_get_usable_frames(void);

/* Print the current PMM statistics to the kernel console. */
void pmm_dump_stats(void);

/* Gate self-test: allocate 1000 unique frames, free them, verify no leak. */
void pmm_self_test(void);

#endif /* AURALITE_MM_PMM_H */
