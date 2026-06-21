#ifndef AURALITE_PROC_USER_H
#define AURALITE_PROC_USER_H

#include <stdint.h>

/*
 * Ring 3 (userspace) execution.
 *
 * jump_to_user() performs an iretq to Ring 3: it sets up the user stack, the
 * user instruction pointer, user CS/SS (RPL=3), and RFLAGS with IF set, then
 * iretq atomically switches to Ring 3. The caller must have already mapped the
 * user code + stack into the current address space (Phase 8 maps a fixed
 * user region; per-process address spaces arrive with the ELF loader).
 */

void jump_to_user(uint64_t entry, uint64_t stack_top, uint64_t stack_bottom);

/*
 * Phase 8 gate test: map a tiny user program, jump to Ring 3, and let it
 * attempt a privileged instruction (cli). The #GP handler recovers by killing
 * the user thread, proving the kernel is protected from userspace.
 */
void user_mode_self_test(void);

#endif /* AURALITE_PROC_USER_H */
