/* kernel/arch/i386/user32.h -- Ring 3 entry + int 0x80 syscalls
 * (I386_PLAN I4).
 *
 * ABI (plan D4): int 0x80, EAX = AuraLite's own syscall number (the
 * SAME table lib/libc/include/unistd.h publishes for x86_64 -- one
 * table, two trap mechanisms), args in EBX, ECX, EDX, ESI, EDI, EBP,
 * result in EAX.  Linux's *register convention*, deliberately not
 * Linux's *numbers*.
 *
 * Bring-up scope: enough syscalls to run a hand-built Ring 3 image
 * end to end -- write to the console, yield, exit.  The ELF32 user
 * loader and the real process model arrive with the 32-bit libc (I5);
 * this phase proves the privilege boundary, the gate DPL, the TSS
 * esp0 path and the register marshalling, which is what I4's gate
 * demands.
 */

#ifndef AURALITE_ARCH_I386_USER32_H
#define AURALITE_ARCH_I386_USER32_H

#include <stdint.h>

/* AuraLite numbers (lib/libc/include/unistd.h), the I4 subset + I7. */
#define SYS32_READ       0
#define SYS32_WRITE      1
#define SYS32_GETPID    39
#define SYS32_EXIT      60
#define SYS32_SPAWN     81   /* non-standard: spawn from initrd path */
#define SYS32_YIELD    158   /* SYS_SCHED_YIELD */

/* PARITY P4: the file five (one table, D4).  The i386 backing store
 * is the INITRD (read-only) until P7 mounts ext2 through the seam --
 * the honest bring-up filesystem this port already has. */
#define SYS32_OPEN       2
#define SYS32_CLOSE      3
#define SYS32_STAT       4
#define SYS32_LSEEK      8
#define SYS32_READDIR   78
#define SYS32_BRK       12   /* R6 */

/* Register the int 0x80 gate (DPL=3) in the IDT. */
void syscall32_init(void);

/* Map a user .text page + user stack, then iret into Ring 3 at
 * entry_phys (identity of content: the caller memcpy'd code into the
 * frame).  Returns the Ring 3 exit code once the program calls
 * SYS32_EXIT.  Runs on the calling kernel thread. */
int user32_run_image(const uint8_t *code, uint32_t code_len);

/* Boot self-test: a built-in Ring 3 program writes a banner via
 * int 0x80, checks getpid, exits 42; plus a negative control -- a
 * privileged instruction from Ring 3 must #GP and be terminated, not
 * executed.  Returns 0 when every check holds. */
int user32_selftest(void);

/* I5: load an ELF32 executable from the initrd (path like
 * "bin32/init32") through elf32load_map and run it Ring 3.  Returns
 * the exit code, or -1 on lookup/parse refusal. */
int user32_run_elf(const char *path);

#endif /* AURALITE_ARCH_I386_USER32_H */
