/* kernel/arch/aarch64/kheap_a64.h -- first-fit kernel heap (ARM64_PLAN A3). */

#ifndef AURALITE_ARCH_AARCH64_KHEAP_A64_H
#define AURALITE_ARCH_AARCH64_KHEAP_A64_H

#include <stdint.h>
#include <stddef.h>

/* A fixed window well away from the HHDM (which owns
 * 0xFFFFFFC000000000 + 256 GiB): pages committed on demand.  The
 * same window kheap_rv uses -- the two kernels' VA maps are the same
 * map (D3's geometry choice paying rent). */
#define KHEAP_A64_BASE 0xFFFFFFE000000000UL
#define KHEAP_A64_SIZE (64UL * 1024 * 1024)

void  kheap_a64_init(void);
void *kmalloc_a64(size_t size);
void  kfree_a64(void *p);
int   kheap_a64_selftest(void);   /* the [heap] gate */

#endif /* AURALITE_ARCH_AARCH64_KHEAP_A64_H */
