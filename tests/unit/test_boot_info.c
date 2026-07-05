/* tests/unit/test_boot_info.c -- host-side unit test for kernel/boot_info.c.
 *
 * Verifies that the bootloader handoff accessor layer (BL1) reads every
 * field of boot_info_t correctly, matches the magic word, and derives
 * boot_get_usable_memory() from the mmap[] entries.  This test compiles
 * against the real kernel/boot_info.c translation unit with the host
 * compiler; that transitively exercises boot/shared/boot_info.h too.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "boot/shared/boot_info.h"
#include "kernel/boot_info.h"

int main(void) {
    boot_info_t info;
    memset(&info, 0, sizeof(info));

    info.magic       = BOOT_MAGIC;
    info.hhdm_offset = 0xffff800000000000ULL;

    /* Two mmap entries: one usable (127 MiB) starting at 1 MiB, and one
     * reserved covering the first megabyte. */
    info.mmap_count = 2;
    info.mmap[0].base   = 0x100000ULL;
    info.mmap[0].length = 0x7F00000ULL;
    info.mmap[0].type   = BOOT_MEM_USABLE;
    info.mmap[1].base   = 0x0ULL;
    info.mmap[1].length = 0x100000ULL;
    info.mmap[1].type   = BOOT_MEM_RESERVED;

    /* A believable framebuffer. */
    info.fb.phys_base   = 0xFD000000ULL;
    info.fb.width       = 1024;
    info.fb.height      = 768;
    info.fb.pitch       = 1024 * 4;
    info.fb.bpp         = 32;
    info.fb.red_shift   = 16;
    info.fb.green_shift = 8;
    info.fb.blue_shift  = 0;

    /* Initrd. */
    info.initrd_phys = 0x01000000ULL;
    info.initrd_size = 65536;

    /* SMP: a single-CPU box. */
    info.cpu_count    = 1;
    info.bsp_lapic_id = 0;
    info.cpus[0].processor_id = 0;
    info.cpus[0].lapic_id     = 0;

    info.boot_from_uefi = 1;

    boot_info_init(&info);

    /* Basic sanity. */
    assert(boot_get_hhdm_offset() == 0xffff800000000000ULL);

    /* Framebuffer. */
    boot_fb_t *fb = boot_get_framebuffer();
    assert(fb != NULL);
    assert(fb->width       == 1024);
    assert(fb->height      == 768);
    assert(fb->bpp         == 32);
    assert(fb->pitch       == 1024 * 4);
    assert(fb->phys_base   == 0xFD000000ULL);
    assert(fb->red_shift   == 16);
    assert(fb->green_shift == 8);
    assert(fb->blue_shift  == 0);

    /* Memory map. */
    uint64_t cnt = 0;
    boot_mmap_entry_t *m = boot_get_memmap(&cnt);
    assert(cnt == 2);
    assert(m   != NULL);
    assert(m[0].type == BOOT_MEM_USABLE);
    assert(m[0].base == 0x100000ULL);
    assert(m[1].type == BOOT_MEM_RESERVED);

    /* boot_get_usable_memory() must sum only BOOT_MEM_USABLE regions. */
    assert(boot_get_usable_memory() == 0x7F00000ULL);

    /* Initrd. */
    uint64_t sz   = 0;
    uint64_t phys = boot_get_initrd(&sz);
    assert(phys == 0x01000000ULL);
    assert(sz   == 65536);

    /* SMP. */
    uint64_t ncpu = 999;
    uint32_t bsp  = 999;
    boot_cpu_t *cpus = boot_get_smp_info(&ncpu, &bsp);
    assert(cpus != NULL);
    assert(ncpu == 1);
    assert(bsp  == 0);
    assert(cpus[0].lapic_id == 0);

    /* Boot path indicator survives round-trip. */
    assert(info.boot_from_uefi == 1);

    /* Zero-fill scenario: a fresh boot_info with only magic set must
     * still yield sane defaults (no crashes, no NULL fb, no CPUs). */
    boot_info_t empty;
    memset(&empty, 0, sizeof(empty));
    empty.magic = BOOT_MAGIC;
    boot_info_init(&empty);
    assert(boot_get_framebuffer()       == NULL);
    assert(boot_get_usable_memory()     == 0);
    /* HHDM falls back to the x86_64 architectural constant. */
    assert(boot_get_hhdm_offset()       == 0xffff800000000000ULL);
    uint64_t esz = 999;
    assert(boot_get_initrd(&esz) == 0 && esz == 0);
    uint64_t enc = 999;
    uint32_t ebsp = 999;
    assert(boot_get_smp_info(&enc, &ebsp) == NULL);
    assert(enc == 0 && ebsp == 0);

    puts("test_boot_info: ALL PASS");
    return 0;
}
