/* kernel/arch/riscv64/user_rv.h -- U-mode + ecall (RISCV_PLAN V4). */

#ifndef AURALITE_ARCH_RISCV64_USER_RV_H
#define AURALITE_ARCH_RISCV64_USER_RV_H

#include <stdint.h>

#include "kernel/arch/riscv64/trap.h"

/* Syscall numbers: AuraLite's one shared table (D4).  The subset a
 * V4 image can invoke; the full table arrives with libcrv in V5. */
#define SYS_RV_WRITE   1
#define SYS_RV_GETPID  39
#define SYS_RV_YIELD   24
#define SYS_RV_EXIT    60

/* Run a flat U-mode image (code copied to a fresh user page at
 * USER_TEXT_VADDR_RV, stack below it) to completion; returns its exit
 * code, or 128+scause if it faulted, or -1 on setup failure. */
#define USER_TEXT_VADDR_RV 0x0000000040000000UL

int user_rv_run_image(const uint8_t *code, uint64_t code_len);

/* trap.c hooks. */
void user_rv_syscall(rv_trap_frame_t *f);
int  user_rv_fault(rv_trap_frame_t *f, uint64_t scause, uint64_t stval);

/* The boot self-test: hand-assembled image writes RING-U-OK via
 * ecall, exits 42; the negative control executes a privileged csrr
 * and must be contained with 128+2.  Returns 0 on pass. */
int  user_rv_selftest(void);

#endif /* AURALITE_ARCH_RISCV64_USER_RV_H */
