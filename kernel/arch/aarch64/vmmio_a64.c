/* kernel/arch/aarch64/vmmio_a64.c -- the aarch64 vmmio_arch_ops table
 * (ARM64_PLAN A7).
 *
 * The fourth tenant's answers to the promoted transport's questions:
 * frames from the A3 bitmap PMM (walks upward -- the contiguous-vring
 * adjacency check inherits the property it checks), the HHDM
 * translation, the PL011 console, CNTVCT as the deadline clock at
 * CNTFRQ's measured rate (a register, not folklore -- plan Fact 2.3).
 *
 * And the one hook that is THIS architecture's reason the seam has
 * it: mmio_is_device walks the live page tables and reports the MAIR
 * index.  A virtio window that is not Device-nGnRE is refused at
 * attach time -- Fact 5.2's reordered/combined/speculated device
 * access bug class, prevented by refusal, not convention.  (The A3
 * HHDM maps PA 0x08000000..0x0A010000 as A64_MAP_RW_DEVICE, which
 * covers all 32 virt windows; this hook is what makes that sentence
 * checked instead of believed.)
 */

#include <stdint.h>

#include "kernel/arch/aarch64/vmmio_a64.h"
#include "kernel/arch/aarch64/paging_a64.h"
#include "kernel/arch/aarch64/pmm_a64.h"
#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/trap_a64.h"

static uint64_t alloc_frame_a64(void) { return pmm_a64_alloc_frame(); }
static void *p2v_hook_a64(uint64_t phys) { return p2v_a64(phys); }
static void puts_a64(const char *s) { pl011_puts(s); }
static uint64_t ticks_hook_a64(void) { return a64_cntvct(); }

static int mmio_is_device_a64(const volatile void *va)
{
    return paging_a64_attr_index((uint64_t)(uintptr_t)va)
           == MAIR_IDX_DEVICE;
}

static struct vmmio_arch_ops a64_ops = {
    .alloc_frame    = alloc_frame_a64,
    .p2v            = p2v_hook_a64,
    .puts           = puts_a64,
    .ticks          = ticks_hook_a64,
    .ticks_per_sec  = 0,            /* CNTFRQ, filled on first use */
    .mmio_is_device = mmio_is_device_a64,
};

const struct vmmio_arch_ops *vmmio_a64_ops(void)
{
    if (a64_ops.ticks_per_sec == 0) {
        uint64_t f;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
        a64_ops.ticks_per_sec = f;
    }
    return &a64_ops;
}
