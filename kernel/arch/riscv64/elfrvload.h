/* kernel/arch/riscv64/elfrvload.h -- ELF64 user-program loader
 * (RISCV_PLAN V5).
 *
 * Accepts ELFCLASS64 / EM_RISCV / ET_EXEC only.  elf.c (x86_64)
 * refuses EM_RISCV, elf32load.c (i386) refuses class 64, and this
 * loader refuses both of theirs -- the three-way mutual refusal
 * completing the pattern the plan asked for.
 *
 * Permissions are REAL here: p_flags map to PTE bits -- PF_X gives
 * R+X (never W), PF_W gives R+W (never X).  The machinery V3 built
 * and V4 first applied is what makes this arch's loader the only one
 * of the three whose W^X promise is enforced rather than stated.
 */

#ifndef AURALITE_ARCH_RISCV64_ELFRVLOAD_H
#define AURALITE_ARCH_RISCV64_ELFRVLOAD_H

#include <stdint.h>

/* Segments must live in [ELF_RV_USER_MIN, ELF_RV_USER_MAX). */
#define ELF_RV_USER_MIN 0x08000000UL
#define ELF_RV_USER_MAX 0x40000000UL

/* Validate the image and map its PT_LOAD segments as user pages.
 * Returns the entry point, or 0 on refusal. */
uint64_t elfrvload_map(const uint8_t *image, uint64_t size);

/* Mark/release nesting for SYS_SPAWN, initrd32's shape: mark
 * checkpoints the mapping list before a nested child; unmap releases
 * back to the newest mark, leaving the parent's pages alone. */
void elfrvload_mark(void);
void elfrvload_unmap(void);

#endif /* AURALITE_ARCH_RISCV64_ELFRVLOAD_H */
