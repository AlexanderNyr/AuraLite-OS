/* kernel/boot_info.c -- implementation of the boot_info_t accessor layer.
 *
 * Replaces kernel/limine_requests.c.  The kernel calls boot_info_init()
 * at the top of kmain() (before any subsystem runs); every other
 * function in this file is a thin, side-effect-free reader.
 *
 * Style: no external dependencies beyond the shared header so the same
 * translation unit compiles cleanly under both the freestanding kernel
 * toolchain and the host compiler used by the unit-test suite.
 */

#include <stddef.h>
#include <stdint.h>
#include "kernel/boot_info.h"

/* File-local latch of the pointer handed in by the bootloader. */
static boot_info_t *g_boot_info;

void boot_info_init(boot_info_t *info) {
    if (info == NULL || info->magic != BOOT_MAGIC) {
        /* We cannot log yet -- UART is not initialised at this point.
         * The only safe response is a permanent halt: any further
         * subsystem access would dereference garbage. */
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
    g_boot_info = info;
}

boot_fb_t *boot_get_framebuffer(void) {
    if (g_boot_info == NULL || g_boot_info->fb.phys_base == 0) {
        return NULL;
    }
    return &g_boot_info->fb;
}

boot_mmap_entry_t *boot_get_memmap(uint64_t *out_count) {
    if (g_boot_info == NULL) {
        if (out_count) {
            *out_count = 0;
        }
        return NULL;
    }
    if (out_count) {
        *out_count = g_boot_info->mmap_count;
    }
    return g_boot_info->mmap;
}

uint64_t boot_get_usable_memory(void) {
    if (g_boot_info == NULL) {
        return 0;
    }
    uint64_t total = 0;
    for (uint32_t i = 0; i < g_boot_info->mmap_count; i++) {
        if (g_boot_info->mmap[i].type == BOOT_MEM_USABLE) {
            total += g_boot_info->mmap[i].length;
        }
    }
    return total;
}

uint64_t boot_get_hhdm_offset(void) {
    /* Return the value from the struct; fall back to the x86_64
     * architectural constant if the bootloader left it zero.  This
     * guarantees callers always see a valid HHDM base even if a bug in
     * the loader forgot the field. */
    if (g_boot_info != NULL && g_boot_info->hhdm_offset != 0) {
        return g_boot_info->hhdm_offset;
    }
    return 0xffff800000000000ULL;
}

uint64_t boot_get_initrd(uint64_t *out_size) {
    if (g_boot_info == NULL) {
        if (out_size) {
            *out_size = 0;
        }
        return 0;
    }
    if (out_size) {
        *out_size = g_boot_info->initrd_size;
    }
    return g_boot_info->initrd_phys;
}

boot_cpu_t *boot_get_smp_info(uint64_t *out_count,
                              uint32_t *out_bsp_lapic_id) {
    if (g_boot_info == NULL || g_boot_info->cpu_count == 0) {
        if (out_count) {
            *out_count = 0;
        }
        if (out_bsp_lapic_id) {
            *out_bsp_lapic_id = 0;
        }
        return NULL;
    }
    if (out_count) {
        *out_count = g_boot_info->cpu_count;
    }
    if (out_bsp_lapic_id) {
        *out_bsp_lapic_id = g_boot_info->bsp_lapic_id;
    }
    return g_boot_info->cpus;
}
