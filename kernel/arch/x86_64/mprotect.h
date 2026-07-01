#ifndef AURALITE_ARCH_X86_64_MPROTECT_H
#define AURALITE_ARCH_X86_64_MPROTECT_H

#include <stdint.h>
#include "kernel/mm/vma.h"

int  mprotect_update_vma_range(vma_t *list, uint64_t addr, uint64_t len, uint64_t prot);
void mprotect_remap_present_pages(uint64_t addr, uint64_t len, uint64_t prot);

#endif /* AURALITE_ARCH_X86_64_MPROTECT_H */
