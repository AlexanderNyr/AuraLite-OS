/* kernel/arch/riscv64/kheap_rv.h -- first-fit kernel heap (RISCV_PLAN V3). */

#ifndef AURALITE_ARCH_RISCV64_KHEAP_RV_H
#define AURALITE_ARCH_RISCV64_KHEAP_RV_H

#include <stdint.h>
#include <stddef.h>

/* A fixed window well away from the HHDM (which owns
 * 0xFFFFFFC000000000 + 256 GiB): pages committed on demand. */
#define KHEAP_RV_BASE 0xFFFFFFE000000000UL
#define KHEAP_RV_SIZE (64UL * 1024 * 1024)

void  kheap_rv_init(void);
void *kmalloc_rv(size_t size);
void  kfree_rv(void *p);
int   kheap_rv_selftest(void);   /* the [heap] gate */

#endif /* AURALITE_ARCH_RISCV64_KHEAP_RV_H */
