/* execve_child.c — kernel boot self-test helper for execve(argv, envp).
 *
 * The kernel's process_self_test() spawns this program in a fresh, isolated
 * address space. We immediately replace ourselves with /argv_echo via the
 * POSIX execve(path, argv, envp) path — exercising the kernel's argv/envp
 * marshalling onto the new process's initial user stack.
 *
 * Because we are a spawned child (not a fork of the interactive shell), there
 * is no competing user-space wait4 racing on the per-thread SYSCALL save area,
 * so this is the safe place to exercise execve end-to-end.
 *
 * If execve() returns, it failed — we say so and exit non-zero.
 */

#include "unistd.h"
#include "stdio.h"

int main(void) {
    static char *argv[] = {
        "/argv_echo",
        "alpha",
        "beta gamma",   /* embedded space: must survive as one argv entry */
        "42",
        0,
    };
    static char *envp[] = {
        "P10=on",
        "SHELL=/init",
        0,
    };

    printf("EXECVE_CHILD calling execve(/argv_echo, argv, envp)\n");
    fflush(stdout);

    execve("/argv_echo", argv, envp);

    /* Only reached on failure. */
    printf("EXECVE_CHILD execve FAILED\n");
    return 1;
}
