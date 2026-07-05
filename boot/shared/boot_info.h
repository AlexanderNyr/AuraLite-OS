/* boot/shared/boot_info.h -- AuraLite OS bootloader/kernel handoff structure.
 *
 * Introduced in Phase BL1 of the custom-bootloader plan.  Both the BIOS
 * (Stage 2) and UEFI (BOOTX64.EFI) bootloaders fill one of these structures
 * and pass a physical pointer to it in RDI when jumping to the kernel
 * entry point (_start in kernel/arch/x86_64/boot.asm), per the System V
 * AMD64 calling convention.
 *
 * The kernel must NOT free or modify the struct: it lives inside memory
 * that the loader marks as BOOT_MEM_BOOTLOADER (safe to reclaim later,
 * but not before the kernel has copied out everything it needs).
 *
 * All physical addresses in this struct are raw physical addresses.  The
 * kernel adds hhdm_offset (see below) to reach them through the direct
 * map once paging is active.
 */

#ifndef AURALITE_BOOT_SHARED_BOOT_INFO_H
#define AURALITE_BOOT_SHARED_BOOT_INFO_H

#include <stdint.h>

/* Magic identifier written by the bootloader at offset 0.  The kernel checks
 * this in boot_info_init() before trusting any other field.
 *
 * Byte pattern in memory (little-endian): 'D','L','T','B','A','R','U','A'
 * which reads as "AURABLT" + trailing 'D'.  The value 0x4155524142544C44
 * therefore encodes the ASCII string "AURABLTD" when stored as a qword. */
#define BOOT_MAGIC        0x4155524142544C44ULL   /* AURABLT (little-endian) */

/* Static caps for the embedded arrays.  The struct is fixed-size so the
 * bootloader can allocate it with a single page (~9 KiB fits in three
 * pages including padding). */
#define BOOT_MAX_MMAP     256
#define BOOT_MAX_CPUS     64

/* Memory region types.  These are a strict superset of the E820 memory
 * types (ACPI 6.5 s.15.3) and the UEFI EFI_MEMORY_TYPE enumeration.  The
 * bootloader is responsible for translating platform-specific values into
 * BOOT_MEM_*.
 *
 * BOOT_MEM_USABLE == 1 so that the E820 wire value (1 = usable) maps
 *   directly, saving a comparison in the BIOS stage 2 assembler. */
#define BOOT_MEM_USABLE       1
#define BOOT_MEM_RESERVED     2
#define BOOT_MEM_ACPI_RECLAIM 3
#define BOOT_MEM_ACPI_NVS     4
#define BOOT_MEM_BAD          5
#define BOOT_MEM_BOOTLOADER   6   /* safe to reclaim after boot */
#define BOOT_MEM_KERNEL       7   /* kernel image + initrd */
#define BOOT_MEM_FRAMEBUF     8

typedef struct {
    uint64_t base;              /* physical start */
    uint64_t length;            /* bytes */
    uint32_t type;              /* BOOT_MEM_* */
    uint32_t _pad;              /* keeps the struct naturally aligned */
} boot_mmap_entry_t;

typedef struct {
    uint64_t phys_base;         /* physical address of the linear framebuffer */
    uint32_t width;             /* horizontal resolution in pixels */
    uint32_t height;            /* vertical   resolution in pixels */
    uint32_t pitch;             /* bytes per scan line */
    uint8_t  bpp;               /* bits per pixel (32 typical for 8:8:8:8) */
    uint8_t  red_shift;         /* red   channel LSB position */
    uint8_t  green_shift;       /* green channel LSB position */
    uint8_t  blue_shift;        /* blue  channel LSB position */
    uint8_t  _pad[1];
} boot_fb_t;

typedef struct {
    uint32_t processor_id;      /* ACPI processor ID */
    uint32_t lapic_id;          /* Local APIC ID */
    uint64_t goto_address;      /* 0 = AP is still parked; SMP code writes
                                 *   a function pointer here and the AP
                                 *   jumps to it once observed non-zero. */
    uint64_t extra_argument;    /* passed to the AP entry in RDI */
} boot_cpu_t;

typedef struct {
    uint64_t          magic;            /* must equal BOOT_MAGIC */

    /* Framebuffer (all zero if the bootloader could not obtain one). */
    boot_fb_t         fb;

    /* Physical memory map (E820 on BIOS, GetMemoryMap() on UEFI). */
    boot_mmap_entry_t mmap[BOOT_MAX_MMAP];
    uint32_t          mmap_count;
    uint32_t          _pad_mmap;

    /* Higher-Half Direct Map offset.  The bootloader maps all physical
     * RAM at this virtual offset before jumping to the kernel.  On
     * x86_64 this is always 0xffff800000000000ULL. */
    uint64_t          hhdm_offset;

    /* Initial RAM disk (USTAR tar archive). */
    uint64_t          initrd_phys;      /* 0 => no initrd present */
    uint64_t          initrd_size;

    /* SMP information (from the ACPI MADT or UEFI MP services). */
    uint32_t          cpu_count;
    uint32_t          bsp_lapic_id;
    boot_cpu_t        cpus[BOOT_MAX_CPUS];

    /* ACPI Root System Description Pointer.  0 if not located. */
    uint64_t          rsdp_phys;

    /* Boot path indicator. */
    uint8_t           boot_from_uefi;   /* 1 = UEFI, 0 = legacy BIOS */
    uint8_t           _pad[7];
} boot_info_t;
/* NOTE: this struct is intentionally NOT __attribute__((packed)).
 * Every field is naturally aligned thanks to explicit _pad members, so
 * the packed attribute would only strip alignment guarantees (breaking
 * pointer-to-inner-array returns) without changing the memory layout.
 * If you ever add a field, keep the natural alignment of the following
 * field by inserting explicit padding here rather than reaching for
 * __attribute__((packed)). */

#endif /* AURALITE_BOOT_SHARED_BOOT_INFO_H */
