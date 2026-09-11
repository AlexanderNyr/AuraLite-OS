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
#define PT_PHDR       6           /* program-header table itself (M5 auxv) */
#define PT_INTERP     3           /* LX_COMPAT L4: dynamic interpreter path */
#define PT_GNU_STACK  0x6474e551  /* LX_COMPAT L4: stack NX/exec hint */

/* e_type values (LX_COMPAT L4: ET_DYN needs a load bias). */
#define ET_EXEC       2           /* fixed addresses (p_vaddr is absolute) */
#define ET_DYN        3           /* position-independent (PIE / shared obj) */

/* Program-header permission bits. */
#define PF_X          0x1
#define PF_W          0x2
#define PF_R          0x4

/* ELF64 header (64 bytes). */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
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
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* ELF64 program header (56 bytes). */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
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
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/*
 * Load an ELF64 binary from memory (the embedded image).
 *
 * @param  image     pointer to the raw ELF file bytes
 * @param  size      image size in bytes
 * @param  out_brk   if non-NULL, receives the program break (end of the
 *                   highest PT_LOAD segment) for brk() tracking
 * @param  out_phdr  if non-NULL, receives the virtual address of the mapped
 *                   program-header table (for the AT_PHDR auxv entry): the
 *                   PT_PHDR p_vaddr when the ELF has one, else the PT_LOAD
 *                   vaddr that contains e_phoff, else 0.
 * @param  out_phnum if non-NULL, receives e_phnum (for AT_PHNUM)
 * @returns the entry-point virtual address, or 0 on failure
 */
uint64_t elf_load(const void *image, uint64_t size, uint64_t *out_brk,
                  uint64_t *out_phdr, uint64_t *out_phnum);

/* LX_COMPAT L4: the dynamic-loader hooks.
 *
 * elf_load_at() is elf_load() with an explicit load bias: every PT_LOAD
 * p_vaddr (and the returned entry/phdr) is offset by @bias.  An ET_EXEC
 * image passes bias 0; an ET_DYN image (a PIE executable or a shared
 * object like ld-linux) is position-independent — its p_vaddr/e_entry
 * are offsets, and the caller chooses a bias in user space.  Linux does
 * this for every PIE/DSO; AuraLite has no per-image ASLR yet, so the
 * bias is a fixed constant chosen by the caller (see process.c).
 *
 * elf_interp_path() answers whether @image carries a PT_INTERP segment
 * and copies its NUL-terminated path into @out (bounded by @cap).
 *
 * elf_gnu_stack_x() answers whether the image's PT_GNU_STACK (when
 * present) requests an executable stack (p_flags & PF_X); absent, the
 * answer is 0 and the stack stays NX — the correct default for every
 * modern glibc image (PT_GNU_STACK is RW, not X). */
uint64_t elf_load_at(const void *image, uint64_t size, uint64_t bias,
                     uint64_t *out_brk, uint64_t *out_phdr, uint64_t *out_phnum);
int elf_interp_path(const void *image, uint64_t size, char *out, uint64_t cap);
int elf_gnu_stack_x(const void *image, uint64_t size);

#endif /* AURALITE_PROC_ELF_H */
