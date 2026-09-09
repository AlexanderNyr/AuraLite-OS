/* userspace/apps/lxrun/lxrun.c — the lx personality's front door
 * (LX_COMPAT_PLAN.md L1).
 *
 * The personality itself is selected by the KERNEL's /linux path-prefix
 * rule at execve time; lxrun exists so a native shell session can launch
 * a Linux binary without first copying it anywhere or typing an absolute
 * path twice.  It is a native AuraLite program (libc, crt0, the usual
 * user.ld) that does exactly one thing: replace itself with argv[1].
 *
 * Deliberately no path magic: `lxrun /linux/tests/hello` execve's
 * "/linux/tests/hello"; a path outside /linux executes too — as a
 * NATIVE binary (the prefix rule, not lxrun, decides the personality).
 */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "lxrun: usage: lxrun /linux/program [args...]\n"
                "lxrun: programs under /linux run with the Linux syscall\n"
                "lxrun: number map (LX_COMPAT_PLAN.md)\n");
        return 2;
    }

    /* argv[1..] becomes the new image's argv: argv[0] is the program's
     * own path, exactly as posix_spawn/execv build it.  The environment
     * is inherited as an empty vector — the native libc exports no
     * environ, and the L1 ladder's programs read no environment. */
    char *const env_empty[] = { NULL };
    execve(argv[1], &argv[1], env_empty);

    /* execve only returns on failure. */
    fprintf(stderr, "lxrun: execve('%s') failed\n", argv[1]);
    return 127;
}
