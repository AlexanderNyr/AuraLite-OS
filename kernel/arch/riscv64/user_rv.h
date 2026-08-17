/* kernel/arch/riscv64/user_rv.h -- U-mode + ecall (RISCV_PLAN V4). */

#ifndef AURALITE_ARCH_RISCV64_USER_RV_H
#define AURALITE_ARCH_RISCV64_USER_RV_H

#include <stdint.h>

#include "kernel/arch/riscv64/trap.h"

/* Syscall numbers: AuraLite's one shared table (D4).  V5 grew READ
 * and SPAWN alongside libcrv (YIELD moved to its table number too --
 * V4's 24 was a transcription slip caught by the shared header:
 * libc32/libcrv agree on 158 and the smoke test dials through
 * them). */
#define SYS_RV_READ    0
#define SYS_RV_WRITE   1
#define SYS_RV_GETPID  39
#define SYS_RV_EXIT    60
#define SYS_RV_SPAWN   81
#define SYS_RV_YIELD   158

/* Run a flat U-mode image (code copied to a fresh user page at
 * USER_TEXT_VADDR_RV, stack below it) to completion; returns its exit
 * code, or 128+scause if it faulted, or -1 on setup failure. */
#define USER_TEXT_VADDR_RV 0x0000000040000000UL

int user_rv_run_image(const uint8_t *code, uint64_t code_len);

/* V5: run an ELF64/EM_RISCV program from the initrd to completion.
 * Returns its exit code, 128+scause on a contained fault, -1 on
 * lookup/parse refusal.  Nested (SYS_SPAWN) up to depth 2, the
 * user32_run_elf shape. */
int user_rv_run_elf(const char *path);

/* V5: cooked console line over sbi_getchar (echo, backspace, CR->LF);
 * kbd32's cons32_readline at the third arch.  Blocks via wfi. */
uint64_t cons_rv_readline(char *buf, uint64_t cap);

/* trap.c hooks. */
void user_rv_syscall(rv_trap_frame_t *f);
int  user_rv_fault(rv_trap_frame_t *f, uint64_t scause, uint64_t stval);

/* The boot self-test: hand-assembled image writes RING-U-OK via
 * ecall, exits 42; the negative control executes a privileged csrr
 * and must be contained with 128+2.  Returns 0 on pass. */
int  user_rv_selftest(void);

#endif /* AURALITE_ARCH_RISCV64_USER_RV_H */
