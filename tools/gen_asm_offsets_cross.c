/* Emit kernel ABI offsets as assembly comments.  This is compiled for the
 * kernel target and never executed on the host. */
#include <stddef.h>
#include "kernel/proc/thread.h"
#include "kernel/arch/x86_64/cpu_local.h"

#define OFFSET(name, type, member) \
    __asm__ volatile ("# AURALITE_OFFSET " #name " %c0" : : "i" (offsetof(type, member)))
#define SIZE(name, type) \
    __asm__ volatile ("# AURALITE_OFFSET " #name " %c0" : : "i" (sizeof(type)))

void auralite_emit_asm_offsets(void) {
    OFFSET(TCB_RSP, tcb_t, rsp);
    OFFSET(TCB_TLS_BASE, tcb_t, tls_base);
    OFFSET(TCB_USER_GS, tcb_t, user_gs_base);
    OFFSET(TCB_PML4, tcb_t, pml4_phys);
    SIZE(TCB_SIZE, tcb_t);
    OFFSET(TCB_SWITCH_PARKED, tcb_t, switch_parked);
    OFFSET(TCB_FPU, tcb_t, fpu_area);
    OFFSET(TCB_FPU_VALID, tcb_t, fpu_valid);
    OFFSET(CL_SYS_RIP, struct cpu_local, syscall_saved_rip);
    OFFSET(CL_SYS_RFLAGS, struct cpu_local, syscall_saved_rflags);
    OFFSET(CL_SYS_RSP, struct cpu_local, syscall_saved_rsp);
    OFFSET(CL_SYS_KRSP, struct cpu_local, syscall_kernel_rsp);
    OFFSET(CL_SYS_RBX, struct cpu_local, syscall_saved_rbx);
    OFFSET(CL_SYS_RBP, struct cpu_local, syscall_saved_rbp);
    OFFSET(CL_SYS_R12, struct cpu_local, syscall_saved_r12);
    OFFSET(CL_SYS_R13, struct cpu_local, syscall_saved_r13);
    OFFSET(CL_SYS_R14, struct cpu_local, syscall_saved_r14);
    OFFSET(CL_SYS_R15, struct cpu_local, syscall_saved_r15);
}
