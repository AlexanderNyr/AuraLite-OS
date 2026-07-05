/* boot/uefi/efi_elf.c -- ELF64 loader for the UEFI path.
 *
 * The BIOS path uses boot/bios/stage2/elf.inc (real-mode assembly);
 * the UEFI path uses this C module.  Both apply the same transform:
 *
 *     phys_dest = p_paddr - KERNEL_VMA
 *
 * where KERNEL_VMA = 0xFFFFFFFF80000000 and the linker script leaves
 * p_paddr equal to p_vaddr (no AT() clause).  The kernel image lives
 * at physical 0x00100000 after this loader runs.
 *
 * Returns the 64-bit virtual entry point (Ehdr.e_entry) on success,
 * 0 on failure (bad magic or class).
 */

#include <stdint.h>

#define PT_LOAD           1
#define KERNEL_VMA        0xFFFFFFFF80000000ULL

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

static void mymemcpy(void *dst, const void *src, uint64_t n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static void mymemset(void *dst, uint8_t v, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = v;
}

uint64_t efi_elf_load(const void *image) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;

    /* Magic 0x7F 'E' 'L' 'F' + class 2 (ELFCLASS64). */
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F' ||
        eh->e_ident[4] != 2)
        return 0;

    const uint8_t     *base  = (const uint8_t *)image;
    const Elf64_Phdr  *phdrs = (const Elf64_Phdr *)(base + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        uint64_t phys = ph->p_paddr - KERNEL_VMA;

        mymemcpy((void *)(uintptr_t)phys,
                 base + ph->p_offset,
                 ph->p_filesz);

        if (ph->p_memsz > ph->p_filesz) {
            mymemset((void *)(uintptr_t)(phys + ph->p_filesz),
                     0,
                     ph->p_memsz - ph->p_filesz);
        }
    }
    return eh->e_entry;
}
