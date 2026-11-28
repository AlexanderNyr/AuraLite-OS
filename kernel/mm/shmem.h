#ifndef AURALITE_MM_SHMEM_H
#define AURALITE_MM_SHMEM_H

#include <stdint.h>

/*
 * shmem — anonymous shared-memory objects for MAP_SHARED|MAP_ANONYMOUS.
 *
 * Object references and physical-frame references are separate:
 *   - each handle/VMA/in-flight operation owns one object reference;
 *   - the object owns one PMM reference for every cached frame entry;
 *   - each installed PTE owns one additional PMM reference.
 *
 * shmem_get_or_alloc() returns a PMM reference for the caller's mapping.
 * The caller must drop that reference if it does not install a PTE, and
 * unmapping/reaping a PTE must drop exactly one mapping reference.
 */

/* Create a page-rounded object. The caller owns one reference on success. */
int      shmem_create(uint64_t size);

/* Acquire/release an object reference. shmem_put() releases the object and
 * its cached-frame references when the final object reference is dropped. */
int      shmem_get(int shmid);
void     shmem_put(int shmid);

/* Compatibility spelling for dropping the creator/handle reference. */
void     shmem_destroy(int shmid);

/* Return the same physical frame for a given object/page offset. On success,
 * returns one PMM reference owned by the caller (normally transferred to a
 * user PTE). */
int      shmem_get_or_alloc(int shmid, uint64_t offset, uint64_t *phys_out);

int      shmem_valid(int shmid);
uint64_t shmem_size(int shmid);

#endif /* AURALITE_MM_SHMEM_H */
