/* kernel/arch/i386/elf32load.h -- ELF32 user-program loader
 * (I386_PLAN I5; the task I4's result note moved here, now that its
 * consumer -- init32 from the initrd -- exists).
 *
 * Accepts ELFCLASS32 / EM_386 / ET_EXEC only; the x86_64 kernel's
 * elf.c keeps refusing class 32, so each kernel rejects the other's
 * binaries at parse time (the same both-sides rule elf32.inc and
 * elf.inc follow in the bootloader).
 */

#ifndef AURALITE_ARCH_I386_ELF32LOAD_H
#define AURALITE_ARCH_I386_ELF32LOAD_H

#include <stdint.h>

/* Validate the image and map its PT_LOAD segments as user pages.
 * Returns the entry point, or 0 on refusal.  Segments must live in
 * [ELF32_USER_MIN, ELF32_USER_MAX); anything else is refused. */
#define ELF32_USER_MIN 0x08000000u
#define ELF32_USER_MAX 0x40000000u

uint32_t elf32load_map(const uint8_t *image, uint32_t size);

/* I7: mark/release nesting for SYS_SPAWN.  elf32load_mark() checkpoints
 * the mapping list before a nested child image; elf32load_unmap()
 * releases back to the newest mark, leaving the parent's pages alone.
 * Unnested callers just call unmap as before. */
void elf32load_mark(void);
void elf32load_unmap(void);

#endif /* AURALITE_ARCH_I386_ELF32LOAD_H */
