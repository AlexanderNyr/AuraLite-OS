/* kernel/arch/i386/paging32.h -- non-PAE 2-level paging (I386_PLAN I3).
 *
 * Layout (plan D3; the heap window moved above the direct map's true
 * ceiling after the vmm self-test caught the original 0xF0000000 plan
 * sitting INSIDE the map -- 0xC0000000 + 896 MiB = 0xF8000000, so
 * anything below that is already a 4 MiB PSE page):
 *   [0xC0000000, 0xF8000000)  direct map of phys [0, 896 MiB)
 *   [0xF8000000, 0xFC000000)  kernel heap window (64 MiB)
 *   [0xFC000000, ...)         vmm self-test probe territory
 *   below 0xC0000000          user space (I4+)
 *
 * The direct map uses 4 MiB PSE pages (224 PDEs, zero page tables),
 * which is why check_i686 requires PSE.  No PAE => no NX: the plan's
 * D3 consequence, recorded in docs/status.md rather than hidden --
 * PAGE32_FLAG_NO_EXEC is accepted by paging32_map and ignored, so
 * portable callers keep compiling while the truth lives in one place.
 *
 * hhdm_offset in boot_info_t is 0xC0000000 on this path -- the field
 * is 64-bit wide and the kernel reads it rather than assuming, exactly
 * so that both widths can share the contract.
 */

#ifndef AURALITE_ARCH_I386_PAGING32_H
#define AURALITE_ARCH_I386_PAGING32_H

#include <stdint.h>

#define KERNEL_VBASE_32     0xC0000000u
#define DIRECT_MAP_BYTES    (896u * 1024u * 1024u)
#define KHEAP32_BASE        0xF8000000u
#define KHEAP32_SIZE        (64u * 1024u * 1024u)

#define PAGE32_FLAG_WRITE    (1u << 0)
#define PAGE32_FLAG_USER     (1u << 1)
#define PAGE32_FLAG_NO_EXEC  (1u << 2)   /* accepted, unenforceable: no PAE */

/* phys <-> direct-mapped virt (valid below DIRECT_MAP_BYTES only). */
static inline void *p2v_32(uint32_t phys) {
    return (void *)(phys + KERNEL_VBASE_32);
}
static inline uint32_t v2p_32(const void *virt) {
    return (uint32_t)virt - KERNEL_VBASE_32;
}

/* Build the kernel page directory (identity + direct map), load CR3,
 * set CR4.PSE, CR0.PG+WP.  After this the kernel runs higher-half. */
void paging32_init(void);

/* Drop the [0, 896 MiB) identity window once execution is higher-half.
 * The identity map exists only to survive the mov-to-CR0 instruction. */
void paging32_drop_identity(void);

int  paging32_map(uint32_t virt, uint32_t phys, uint32_t flags);
int  paging32_unmap(uint32_t virt);
/* 1 = mapped, 0 = not; *phys_out optionally receives the frame. */
int  paging32_probe(uint32_t virt, uint32_t *phys_out);

int  paging32_selftest(void);

#endif /* AURALITE_ARCH_I386_PAGING32_H */
