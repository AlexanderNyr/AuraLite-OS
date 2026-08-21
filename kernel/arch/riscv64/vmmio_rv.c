/* kernel/arch/riscv64/vmmio_rv.c -- the rv64 vmmio_arch_ops table
 * (ARM64_PLAN A7: what stayed behind when virtio_mmio.c moved to
 * kernel/drivers/).
 *
 * Everything the transport used to call by name, now handed over as
 * function pointers -- same allocator, same HHDM translation, same
 * console, same clock, so the rv64 kernel's device behaviour is
 * byte-for-byte the pre-promotion behaviour (the V7 gates re-run
 * green over the shared object; that is the promotion's
 * non-regression contract).
 */

#include <stdint.h>

#include "kernel/arch/riscv64/vmmio_rv.h"
#include "kernel/arch/riscv64/paging_rv.h"
#include "kernel/arch/riscv64/pmm_rv.h"
#include "kernel/arch/riscv64/sbi.h"
#include "kernel/arch/riscv64/trap.h"

static uint64_t alloc_frame_rv(void) { return pmm_rv_alloc_frame(); }
static void *p2v_hook_rv(uint64_t phys) { return p2v_rv(phys); }
static void puts_rv(const char *s) { sbi_puts(s); }
static uint64_t ticks_hook_rv(void) { return rv_rdtime(); }

/* Sv39 without Svpbmt has no per-PTE memory-type bits: the PMAs
 * (platform memory attributes) decide how the virtio windows behave,
 * and there is nothing in the page tables for a hook to check.  Say
 * yes honestly rather than fake a check -- the refusal contract this
 * hook implements is aarch64's (MAIR indices exist there), and the
 * asymmetry is recorded here instead of hidden. */
static int mmio_is_device_rv(const volatile void *va)
{
    (void)va;
    return 1;
}

static const struct vmmio_arch_ops rv_ops = {
    .alloc_frame    = alloc_frame_rv,
    .p2v            = p2v_hook_rv,
    .puts           = puts_rv,
    .ticks          = ticks_hook_rv,
    .ticks_per_sec  = 10000000,     /* the virt board's 10 MHz timebase */
    .mmio_is_device = mmio_is_device_rv,
};

const struct vmmio_arch_ops *vmmio_rv_ops(void)
{
    return &rv_ops;
}
