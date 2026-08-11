#ifndef AURALITE_MM_SHMEM_H
#define AURALITE_MM_SHMEM_H

#include <stdint.h>

/*
 * shmem — Anonymous shared memory objects for MAP_SHARED|MAP_ANONYMOUS.
 *
 * MATURITY_PLAN.md phase M4.
 *
 * A shmem object is a named (by shmid) collection of physical frames keyed
 * by page offset.  Multiple VMAs in different processes can reference the
 * same shmid; page faults resolve through shmem_get_or_alloc(), which
 * returns the SAME physical frame for the same (shmid, offset) pair.
 * PMM refcount tracks how many page tables reference each frame; the frame
 * is freed only when the last reference drops.
 *
 * The shmem table is process-global (kernel-wide), protected by a spinlock.
 * shmid is allocated sequentially; the creating process receives it via the
 * mmap return and can share it through fork/inheritance or a POSIX shm_*
 * API (future work).
 */

/* Create a new anonymous shared memory object of @size bytes (page-rounded).
 * Returns shmid >= 0 on success, -1 on failure. */
int      shmem_create(uint64_t size);

/* Destroy a shared memory object.  Frames are freed via PMM refcount
 * (only released when no page table still references them). */
void     shmem_destroy(int shmid);

/* Look up or allocate a physical frame for (shmid, offset).
 * Returns 0 on success with *phys_out set; -1 on failure.
 * The caller gets a PMM refcount increment on the frame. */
int      shmem_get_or_alloc(int shmid, uint64_t offset, uint64_t *phys_out);

/* Query whether a shmid is valid. */
int      shmem_valid(int shmid);

/* Query the size (in bytes, page-rounded) of a shmem object. */
uint64_t shmem_size(int shmid);

#endif /* AURALITE_MM_SHMEM_H */
