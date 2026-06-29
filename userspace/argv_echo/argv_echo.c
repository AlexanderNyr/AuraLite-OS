/* argv_echo.c — print argc/argv/environ exactly as received.
 *
 * This program is the target of an execve() in the kernel boot self-test
 * (process_self_test -> /execve_child -> execve("/argv_echo", argv, envp)).
 * It proves the System V AMD64 initial-stack image built by the kernel
 * (argc, argv[], NULL, envp[], NULL, AT_NULL auxv, strings) is decoded
 * correctly by crt0/__libc_start_main.
 *
 * Output is a set of stable, greppable markers so an integration test can
 * assert exact argv/envp contents without depending on ordering noise.
 */

#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"   /* environ, getenv */

extern char **environ;

int main(int argc, char **argv, char **envp) {
    /* argc */
    printf("ARGV_ECHO argc=%d\n", argc);

    /* Each argv entry on its own line: "ARGV_ECHO argv[i]=<value>". */
    for (int i = 0; i < argc; i++) {
        printf("ARGV_ECHO argv[%d]=%s\n", i, argv[i] ? argv[i] : "(null)");
    }
    /* argv[argc] must be NULL. */
    printf("ARGV_ECHO argv_terminated=%d\n", argv[argc] == 0 ? 1 : 0);

    /* envp passed as the third main() argument should match environ. */
    printf("ARGV_ECHO envp_eq_environ=%d\n", (envp == environ) ? 1 : 0);

    /* Dump the environment via environ. */
    int nenv = 0;
    if (environ) {
        for (char **e = environ; *e; e++) {
            printf("ARGV_ECHO env[%d]=%s\n", nenv, *e);
            nenv++;
        }
    }
    printf("ARGV_ECHO envc=%d\n", nenv);

    /* getenv() lookup on a value the kernel self-test injects. */
    const char *p = getenv("P10");
    printf("ARGV_ECHO getenv_P10=%s\n", p ? p : "(unset)");

    return 0;
}
