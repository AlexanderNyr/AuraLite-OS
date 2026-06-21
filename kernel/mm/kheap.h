#ifndef AURALITE_MM_KHEAP_H
#define AURALITE_MM_KHEAP_H

#include <stdint.h>

/*
 * Kernel heap: a thin wrapper over the generic allocator (heap.c) that backs
 * it with physical frames from the PMM, mapped into a fixed kernel virtual
 * region by the VMM. Pages are committed on demand as the heap grows.
 */

/* Kernel heap lives at 0xFFFFFFFF88000000 and may grow to 16 MiB. */
#define KHEAP_BASE  0xFFFFFFFF88000000ULL
#define KHEAP_LIMIT 0x01000000ULL            /* 16 MiB */

void  kheap_init(void);

/* Standard kernel allocation interface. NULL on OOM. */
void *kmalloc(uint64_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, uint64_t size);

/* Print heap statistics (committed / used / free) to the kernel console. */
void kheap_dump(void);

/* Gate self-test: 10 000 alloc/free cycles with corruption + leak checks. */
void kheap_self_test(void);

#endif /* AURALITE_MM_KHEAP_H */
