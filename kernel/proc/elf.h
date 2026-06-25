#ifndef AURALITE_PROC_ELF_H
#define AURALITE_PROC_ELF_H

#include <stdint.h>

/*
 * Minimal ELF64 loader.
 *
 * Parses the ELF header, validates it (magic, 64-bit, x86_64), then maps each
 * PT_LOAD program header into the current address space with USER permissions.
 * Segment data is copied from the file image; .bss (p_memsz > p_filesz) is
 * zero-filled. Returns the ELF entry point for the caller to jump to.
 */

/* ELF identification indices (e_ident[EI_*]). */
#define EI_MAG0       0
#define EI_MAG1       1
#define EI_MAG2       2
#define EI_MAG3       3
#define EI_CLASS      4
#define EI_DATA       5

#define ELFMAG0       0x7F
#define ELFMAG1       'E'
#define ELFMAG2       'L'
#define ELFMAG3       'F'

#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EM_X86_64     62

#define PT_LOAD       1

/* ELF64 header (64 bytes). */
struct elf64_ehdr {
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
} __attribute__((packed));

/* ELF64 program header (56 bytes). */
struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

/*
 * Load an ELF64 binary from memory (the embedded image).
 *
 * @param  image  pointer to the raw ELF file bytes
 * @param  size   image size in bytes
 * @returns the entry-point virtual address, or 0 on failure
 */
uint64_t elf_load(const void *image, uint64_t size, uint64_t *out_brk);

#endif /* AURALITE_PROC_ELF_H */
