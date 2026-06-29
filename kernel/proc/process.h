#ifndef AURALITE_PROC_PROCESS_H
#define AURALITE_PROC_PROCESS_H

#include <stdint.h>

/*
 * Per-process address space management: fork, execve, wait4.
 *
 * Each user process gets its own PML4 (stored in the TCB's pml4_phys field).
 * The scheduler switches CR3 during context_switch based on pml4_phys.
 */

/* fork(): clone the current process's address space + TCB.
 * Returns the child PID to the parent, 0 to the child, or -1 on failure.
 * The child resumes at the point of the syscall with RAX=0. */
int64_t do_fork(void);

/* execve(): replace the current process's address space with a new ELF loaded
 * from the VFS path `path`. Does not return on success; returns -1 on failure. */
int64_t do_execve(const char *path, uint64_t user_argv, uint64_t user_envp);

/* wait4(): block until a child process exits. Returns the child's PID, or
 * -1 if no children exist. Sets *exit_code to the child's exit status. */
int64_t do_wait4(int64_t *exit_code);

/*
 * Run a program from the VFS in its own address space. Creates a new process
 * (new address space + thread) and returns immediately with the child PID.
 * This is used by the shell to launch programs.
 */
int64_t process_spawn(const char *path);

/*
 * Self-test: spawn /hello in its own address space, wait for it, verify the
 * output. Demonstrates per-process isolation.
 */
void process_self_test(void);

#endif /* AURALITE_PROC_PROCESS_H */
