/* kernel/boot_info.h -- kernel-side accessors for the boot_info_t handoff.
 *
 * This header replaces kernel/limine_requests.h.  The kernel no longer
 * pokes at a bootloader-specific request table; instead the bootloader
 * (BIOS Stage 2 or UEFI BOOTX64.EFI) fills a boot_info_t struct and
 * passes its address in RDI.  boot_info_init() latches the pointer and
 * every subsystem retrieves its data through the boot_get_*() calls
 * below.
 *
 * All calls are safe to invoke after boot_info_init() has run.  Before
 * that they return zero-equivalents rather than dereferencing NULL, but
 * kmain() is expected to call boot_info_init() first thing anyway.
 */

#ifndef AURALITE_KERNEL_BOOT_INFO_H
#define AURALITE_KERNEL_BOOT_INFO_H

#include <stddef.h>
#include <stdint.h>
#include "boot/shared/boot_info.h"

/* Latch the bootloader-provided pointer.  Must be called before any
 * other boot_* accessor.  If the pointer is NULL or the magic is wrong
 * the kernel halts immediately (there is no safe way to continue). */
void boot_info_init(boot_info_t *info);

/* Framebuffer.  Returns NULL if the bootloader did not provide one
 * (phys_base == 0).  Callers must treat the returned struct as read-only. */
boot_fb_t *boot_get_framebuffer(void);

/* Physical memory map.  Sets *out_count to the number of entries and
 * returns a pointer to the mmap[] array embedded in boot_info_t.  Each
 * entry is a boot_mmap_entry_t (struct value, NOT pointer -- this is
 * the main API difference from the old Limine bridge). */
boot_mmap_entry_t *boot_get_memmap(uint64_t *out_count);

/* Sum of BOOT_MEM_USABLE region lengths in bytes.  Used only for the
 * informational log line during boot. */
uint64_t boot_get_usable_memory(void);

/* Higher-Half Direct Map offset (always 0xffff800000000000ULL on x86_64). */
uint64_t boot_get_hhdm_offset(void);

/* Initial RAM disk.  Returns the physical address; the caller adds
 * hhdm_offset to obtain a kernel-visible pointer.  *out_size is set to
 * the byte length (0 if no initrd was loaded). */
uint64_t boot_get_initrd(uint64_t *out_size);

/* SMP information.  Returns a pointer to the cpus[] array embedded in
 * boot_info_t; *out_count receives the number of CPUs (including the
 * BSP) and *out_bsp_lapic_id receives the BSP's Local APIC ID.  Returns
 * NULL and zeros the outputs when no SMP data is available. */
boot_cpu_t *boot_get_smp_info(uint64_t *out_count,
                              uint32_t *out_bsp_lapic_id);

#endif /* AURALITE_KERNEL_BOOT_INFO_H */
