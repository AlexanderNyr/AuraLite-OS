/* boot/uefi/efi_main.c -- AuraLite OS UEFI bootloader entry point.
 *
 * BOOTX64.EFI enters here with:
 *   RCX = EFI_HANDLE  ImageHandle
 *   RDX = EFI_SYSTEM_TABLE *SystemTable
 * per the MS x64 ABI.  We stay in that ABI throughout so we can call
 * UEFI protocol methods without gymnastics.  Only at the very last
 * step (jumping to the kernel) do we set up RDI = &boot_info_t and
 * transfer to the SysV entry point.
 *
 * Sequence:
 *   1.  Locate the GOP protocol; record framebuffer info in boot_info.
 *   2.  Locate the SimpleFileSystem protocol on our own DeviceHandle
 *       (obtained via HandleProtocol on the Loaded Image protocol);
 *       open \\EFI\\BOOT\\KERNEL.ELF, read the whole file into a
 *       pool allocation.
 *   3.  (Optional) Open \\EFI\\BOOT\\INITRD.TAR similarly.
 *   4.  Parse the ELF and copy every PT_LOAD to its physical
 *       destination.  Record e_entry in kernel_entry.
 *   5.  Build our own page tables (identity + HHDM + higher-half).
 *   6.  Call GetMemoryMap twice: once to sizeof, once to fetch;
 *       translate the descriptor list into boot_info.mmap[].
 *   7.  ExitBootServices with the map key we just obtained.
 *   8.  Load our CR3, then jump to kernel_entry with RDI pointing at
 *       boot_info.
 */

#include <stdint.h>
#include "boot/uefi/efi_types.h"
#include "boot/shared/boot_info.h"

extern uint64_t   efi_elf_load(const void *image);
extern EFI_STATUS efi_setup_hhdm_paging(EFI_BOOT_SERVICES *BS);
extern void       efi_activate_paging(void);
extern uint64_t   pml4_phys;
extern void       efi_acpi_discover_cpus(EFI_SYSTEM_TABLE *st, boot_info_t *info);

static EFI_SYSTEM_TABLE *gST;

/* efi_print -- write a narrow ASCII string to EFI ConOut.
 * Converts each byte to CHAR16 on the fly (ASCII subset only). */
static void efi_print(EFI_SYSTEM_TABLE *st, const char *msg) {
    if (!st || !st->ConOut || !st->ConOut->OutputString) return;

    CHAR16 buf[2] = {0, 0};
    while (*msg) {
        buf[0] = (CHAR16)(uint8_t)*msg++;
        st->ConOut->OutputString(st->ConOut, buf);
    }
}

/* Handy GUID literals.  UEFI passes GUIDs by pointer to `void*` in
 * LocateProtocol / OpenProtocol / HandleProtocol calls. */
static uint8_t GOP_GUID[16] = {
    0xde, 0xa9, 0x42, 0x90,  0xdc, 0x23,  0x38, 0x4a,
    0x96, 0xfb,  0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a,
};
static uint8_t SFS_GUID[16] = {
    0x22, 0x5b, 0x4e, 0x96,  0x59, 0x64,  0xd2, 0x11,
    0x8e, 0x39,  0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b,
};
static uint8_t LOADED_IMAGE_GUID[16] = {
    0xa1, 0x31, 0x1b, 0x5b,  0x62, 0x95,  0xd2, 0x11,
    0x8e, 0x3f,  0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b,
};
static uint8_t FILE_INFO_GUID[16] = {
    0x92, 0x6e, 0x57, 0x09,  0x3f, 0x6d,  0xd2, 0x11,
    0x8e, 0x39,  0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b,
};

/* Direct COM1 serial output, for progress prints that survive the
 * transition to raw-hardware mode after ExitBootServices. */
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static void serial_init(void) {
    outb(0x3F9, 0x00);   /* IER = 0 (no interrupts) */
    outb(0x3FB, 0x80);   /* DLAB on */
    outb(0x3F8, 0x01);   /* divisor low  = 1 -> 115200 baud */
    outb(0x3F9, 0x00);   /* divisor high = 0 */
    outb(0x3FB, 0x03);   /* 8N1, DLAB off */
    outb(0x3FA, 0xC7);   /* FIFO on */
    outb(0x3FC, 0x0B);   /* DTR + RTS + OUT2 */
}
static void serial_putc(char c) {
    while ((inb(0x3FD) & 0x20) == 0) { /* wait THR empty */ }
    outb(0x3F8, (uint8_t)c);
}
static void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}

/* Local memset -- freestanding builds cannot rely on libc.  We don't
 * need memcpy anywhere in this TU (the ELF loader has its own). */
static void mymemset(void *d, int v, uint64_t n) {
    uint8_t *b = (uint8_t *)d;
    while (n--) *b++ = (uint8_t)v;
}

/* Convert an EFI memory descriptor type to our BOOT_MEM_*. */
static uint32_t efi_mem_type_to_boot(uint32_t t) {
    switch (t) {
    case EfiConventionalMemory:    return BOOT_MEM_USABLE;
    case EfiACPIReclaimMemory:     return BOOT_MEM_ACPI_RECLAIM;
    case EfiACPIMemoryNVS:         return BOOT_MEM_ACPI_NVS;
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:      return BOOT_MEM_BOOTLOADER;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:   return BOOT_MEM_RESERVED;
    default:                       return BOOT_MEM_RESERVED;
    }
}

/* Open a file at "\EFI\BOOT\<name>" from the ESP root, read the whole
 * thing into a pool allocation, return pointer + size in *out_ptr /
 * *out_sz.  Returns EFI_SUCCESS or the failing status. */
static EFI_STATUS read_esp_file(EFI_BOOT_SERVICES *BS,
                                EFI_SYSTEM_TABLE  *st,
                                EFI_HANDLE         image_handle,
                                CHAR16            *filename,
                                const char        *label,
                                int                quiet_open_error,
                                void             **out_ptr,
                                uint64_t          *out_sz)
{
    EFI_LOADED_IMAGE_PROTOCOL       *li = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_FILE_PROTOCOL               *root = 0, *file = 0;
    EFI_STATUS s;

    *out_ptr = 0;
    *out_sz = 0;

    s = BS->HandleProtocol(image_handle, LOADED_IMAGE_GUID, (void **)&li);
    if (s != EFI_SUCCESS || !li) {
        efi_print(st, "UEFI: LoadedImage protocol not found\r\n");
        return (s != EFI_SUCCESS) ? s : EFI_NOT_FOUND;
    }

    s = BS->HandleProtocol(li->DeviceHandle, SFS_GUID, (void **)&fs);
    if (s != EFI_SUCCESS || !fs) {
        efi_print(st, "UEFI: SimpleFileSystem not found on boot device\r\n");
        return (s != EFI_SUCCESS) ? s : EFI_NOT_FOUND;
    }

    s = fs->OpenVolume(fs, &root);
    if (s != EFI_SUCCESS || !root) {
        efi_print(st, "UEFI: OpenVolume failed\r\n");
        return (s != EFI_SUCCESS) ? s : EFI_NOT_FOUND;
    }

    s = root->Open(root, &file, filename, EFI_FILE_MODE_READ, 0);
    if (s != EFI_SUCCESS || !file) {
        if (!quiet_open_error) {
            efi_print(st, "UEFI: file not found: ");
            efi_print(st, label);
            efi_print(st, "\r\n");
        }
        root->Close(root);
        return (s != EFI_SUCCESS) ? s : EFI_NOT_FOUND;
    }

    /* Learn file size via GetInfo. */
    uint8_t info_buf[512];
    UINTN   info_sz = sizeof(info_buf);
    s = file->GetInfo(file, FILE_INFO_GUID, &info_sz, info_buf);
    if (s != EFI_SUCCESS) {
        efi_print(st, "UEFI: GetInfo failed for ");
        efi_print(st, label);
        efi_print(st, "\r\n");
        file->Close(file);
        root->Close(root);
        return s;
    }
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    uint64_t sz = info->FileSize;

    /* Allocate + read. */
    void *buf = 0;
    s = BS->AllocatePool(EfiLoaderData_Pool, sz, &buf);
    if (s != EFI_SUCCESS || !buf) {
        efi_print(st, "UEFI: AllocatePool failed for ");
        efi_print(st, label);
        efi_print(st, "\r\n");
        file->Close(file);
        root->Close(root);
        return (s != EFI_SUCCESS) ? s : EFI_LOAD_ERROR;
    }
    UINTN read_sz = sz;
    s = file->Read(file, &read_sz, buf);
    file->Close(file);
    root->Close(root);
    if (s != EFI_SUCCESS || read_sz != sz) {
        efi_print(st, "UEFI: read failed for ");
        efi_print(st, label);
        efi_print(st, "\r\n");
        BS->FreePool(buf);
        return EFI_LOAD_ERROR;
    }

    *out_ptr = buf;
    *out_sz  = sz;
    return EFI_SUCCESS;
}

/* Halt on fatal error.  Cannot use ConOut once ExitBootServices ran,
 * so we always fall back to COM1. */
__attribute__((noreturn))
static void fatal(const char *msg) {
    efi_print(gST, "UEFI: fatal: ");
    efi_print(gST, msg);
    efi_print(gST, "\r\n");
    serial_puts("[BL6] FATAL: ");
    serial_puts(msg);
    serial_puts("\r\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

/* Trampoline: switch CR3 to our PML4 and jump to the kernel.  RDI
 * holds &boot_info per SysV; we set it up here.  Written as one asm
 * block so the compiler cannot spill anything between the CR3 load
 * and the JMP -- the identity mapping in our PML4 must cover the
 * physical page containing this very code so the CPU keeps fetching
 * correctly. */
__attribute__((noreturn))
static void jump_to_kernel(uint64_t entry, uint64_t boot_info_phys) {
    __asm__ volatile (
        "mov %[cr3], %%cr3\n"
        "mov %[info], %%rdi\n"
        "jmp *%[entry]\n"
        :
        : [cr3]"r"(pml4_phys),
          [info]"r"(boot_info_phys),
          [entry]"r"(entry)
        : "memory");
    __builtin_unreachable();
}

/* ------------------------------------------------------------------------
 * Entry point.  Marked EFIAPI-equivalent by the Windows target ABI.
 * ------------------------------------------------------------------------ */
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st) {
    gST = st;
    EFI_BOOT_SERVICES *BS = st->BootServices;
    EFI_STATUS status;

    serial_init();
    serial_puts("\r\n[BL6] BOOTX64.EFI entered\r\n");

    /* ---- 1) Allocate and pre-populate boot_info. ---------------------- */
    boot_info_t *info = 0;
    status = BS->AllocatePool(EfiLoaderData_Pool, sizeof(*info), (void **)&info);
    if (status != EFI_SUCCESS || !info)
        fatal("AllocatePool boot_info");
    mymemset(info, 0, sizeof(*info));
    info->magic          = BOOT_MAGIC;
    info->hhdm_offset    = 0xffff800000000000ULL;
    info->boot_from_uefi = 1;
    info->cpu_count      = 1;
    info->bsp_lapic_id   = 0;

    /* ---- 1b) ACPI MADT CPU enumeration ------------------------------- */
    /* Must run while BootServices/ConfigurationTable are still valid, i.e.
     * before ExitBootServices below. cpu_count/cpus[]/rsdp_phys are updated
     * only if a well-formed RSDP+MADT with at least one enabled Local APIC
     * is found; otherwise the single-CPU default above stands untouched. */
    efi_acpi_discover_cpus(st, info);
    if (info->cpu_count > 1) {
        serial_puts("[BL6] ACPI MADT: multiple CPUs detected\r\n");
    } else {
        serial_puts("[BL6] ACPI MADT: single-CPU (or not found); BSP-only\r\n");
    }

    /* ---- 2) Framebuffer via GOP -------------------------------------- */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    status = BS->LocateProtocol(GOP_GUID, 0, (void **)&gop);
    if (status == EFI_SUCCESS && gop && gop->Mode && gop->Mode->Info) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;
        info->fb.phys_base = gop->Mode->FrameBufferBase;
        info->fb.width     = mi->HorizontalResolution;
        info->fb.height    = mi->VerticalResolution;
        info->fb.pitch     = mi->PixelsPerScanLine * 4;
        info->fb.bpp       = 32;
        /* PixelBlueGreenRedReserved8BitPerColor -> BGRX -> B=0,G=8,R=16. */
        if (mi->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
            info->fb.red_shift = 16; info->fb.green_shift = 8; info->fb.blue_shift = 0;
        } else if (mi->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
            info->fb.red_shift = 0;  info->fb.green_shift = 8; info->fb.blue_shift = 16;
        } else {
            /* Bitmask / other formats: leave zero -- kernel treats
             * an all-zero shift set as "unknown, skip". */
        }
        serial_puts("[BL6] GOP framebuffer located\r\n");
    } else {
        efi_print(st, "UEFI: no GOP framebuffer\r\n");
        serial_puts("[BL6] no GOP framebuffer (fb=0)\r\n");
    }

    /* ---- 3) Load /EFI/BOOT/KERNEL.ELF ------------------------------- */
    void   *kernel_buf = 0;
    uint64_t kernel_sz = 0;
    CHAR16   name_kernel[] = { '\\','E','F','I','\\','B','O','O','T','\\',
                               'K','E','R','N','E','L','.','E','L','F', 0 };
    status = read_esp_file(BS, st, image_handle, name_kernel,
                           "\\EFI\\BOOT\\KERNEL.ELF", 0,
                           &kernel_buf, &kernel_sz);
    if (status != EFI_SUCCESS)
        fatal("cannot read \\EFI\\BOOT\\KERNEL.ELF");
    if (kernel_sz < 4 ||
        ((uint8_t *)kernel_buf)[0] != 0x7F ||
        ((uint8_t *)kernel_buf)[1] != 'E'  ||
        ((uint8_t *)kernel_buf)[2] != 'L'  ||
        ((uint8_t *)kernel_buf)[3] != 'F')
        fatal("kernel.elf has bad magic");
    serial_puts("[BL6] KERNEL.ELF loaded from ESP\r\n");

    /* Optional: initrd. */
    CHAR16 name_initrd[] = { '\\','E','F','I','\\','B','O','O','T','\\',
                             'I','N','I','T','R','D','.','T','A','R', 0 };
    void   *initrd_buf = 0;
    uint64_t initrd_sz = 0;
    status = read_esp_file(BS, st, image_handle, name_initrd,
                           "\\EFI\\BOOT\\INITRD.TAR", 1,
                           &initrd_buf, &initrd_sz);
    if (status == EFI_SUCCESS) {
        info->initrd_phys = (uint64_t)(uintptr_t)initrd_buf;
        info->initrd_size = initrd_sz;
        serial_puts("[BL6] INITRD.TAR loaded from ESP\r\n");
    }

    /* ---- 4) Parse ELF, copy PT_LOAD to phys. ------------------------- */
    uint64_t kernel_entry = efi_elf_load(kernel_buf);
    if (kernel_entry == 0)
        fatal("ELF magic / class mismatch");
    serial_puts("[BL6] PT_LOAD segments copied to phys\r\n");

    /* ---- 5) Build page tables mirroring the BIOS path. --------------- */
    if (efi_setup_hhdm_paging(BS) != EFI_SUCCESS)
        fatal("AllocatePages for page tables");
    serial_puts("[BL6] page tables built (identity + HHDM + higher half)\r\n");

    /* ---- 6) Snapshot the memory map. --------------------------------- */
    /* First call: get required size (BS returns EFI_BUFFER_TOO_SMALL). */
    UINTN     map_size = 0;
    UINTN     map_key = 0;
    UINTN     desc_size = 0;
    uint32_t  desc_ver = 0;
    status = BS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    if (status != EFI_BUFFER_TOO_SMALL)
        fatal("GetMemoryMap sizing");
    /* Add slack -- our AllocatePool calls right below may add entries. */
    map_size += 8 * (desc_size ? desc_size : 48);

    EFI_MEMORY_DESCRIPTOR *map = 0;
    status = BS->AllocatePool(EfiLoaderData_Pool, map_size, (void **)&map);
    if (status != EFI_SUCCESS || !map)
        fatal("AllocatePool for memory map");

    status = BS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
    if (status != EFI_SUCCESS)
        fatal("GetMemoryMap (final)");

    /* ---- 7) ExitBootServices -- no more UEFI calls after this. ------- */
    status = BS->ExitBootServices(image_handle, map_key);
    if (status != EFI_SUCCESS) {
        /* Some firmwares return EFI_INVALID_PARAMETER if the map key
         * changed under us; retry once with a fresh map. */
        map_size = 0;
        status = BS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
        if (status != EFI_BUFFER_TOO_SMALL)
            fatal("GetMemoryMap retry sizing");
        map_size += 8 * desc_size;
        status = BS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
        if (status != EFI_SUCCESS)
            fatal("GetMemoryMap retry");
        status = BS->ExitBootServices(image_handle, map_key);
        if (status != EFI_SUCCESS)
            fatal("ExitBootServices failed twice");
    }
    serial_puts("[BL6] ExitBootServices OK\r\n");

    /* ---- 8) Translate EFI mmap into boot_info.mmap[]. ---------------- */
    uint32_t n = 0;
    uint8_t *p = (uint8_t *)map;
    UINTN    count = map_size / desc_size;
    for (UINTN i = 0; i < count && n < BOOT_MAX_MMAP; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(p + i*desc_size);
        info->mmap[n].base   = d->PhysicalStart;
        info->mmap[n].length = d->NumberOfPages * 4096;
        info->mmap[n].type   = efi_mem_type_to_boot(d->Type);
        n++;
    }
    info->mmap_count = n;

    /* ---- 9) Activate our page tables and hand off. ------------------- */
    serial_puts("[BL6] jumping to kernel _start\r\n");
    jump_to_kernel(kernel_entry, (uint64_t)(uintptr_t)info);
}
