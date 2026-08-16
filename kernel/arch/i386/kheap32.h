/* kernel/arch/i386/kheap32.h -- kernel heap for the i386 kernel
 * (I386_PLAN I3).  First-fit with coalescing over an on-demand
 * committed window at 0xF0000000 (64 MiB), the same design and PASS
 * contract as kernel/mm/kheap.c one width down.
 */

#ifndef AURALITE_ARCH_I386_KHEAP32_H
#define AURALITE_ARCH_I386_KHEAP32_H

#include <stdint.h>
#include <stddef.h>

void  kheap32_init(void);
void *kmalloc32(size_t size);
void  kfree32(void *ptr);
int   kheap32_selftest(void);

#endif /* AURALITE_ARCH_I386_KHEAP32_H */
