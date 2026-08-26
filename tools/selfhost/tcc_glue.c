/* tools/selfhost/tcc_glue.c -- AuraLite glue for the guest TinyCC build.
 *
 * SELFHOST_PLAN.md SH1, decision D8: this repository ships only AuraLite's
 * own glue for the self-host toolchain; the tcc sources themselves are
 * fetched by `make selfhost-deps` and never vendored.
 *
 * tccrun.c (the -run in-memory execution mode) is deliberately NOT built
 * for AuraLite: the self-host loop compiles and links ELF files, it never
 * executes C in memory, and tccrun.c drags in <sys/ucontext.h>, mprotect
 * and freopen -- three things this libc/kernel does not need to carry for
 * a mode the build never uses.  These two stubs keep tcc.c/libtcc.c
 * linkable without it:
 *
 *   - tcc_run()      is reached only by the -run command line (tcc.c:402);
 *                    we fail visibly instead of pretending.
 *   - tcc_run_free() is the tccrun state release on tcc_delete
 *                    (libtcc.c:955); there is no run state, so no-op.
 *
 * Compiled from the fetched tcc source directory, where tcc.h/config.h
 * live, so the signatures are the real ones by construction.
 */

#include "tcc.h"

int tcc_run(TCCState *s1, int argc, char **argv)
{
    (void)s1; (void)argc; (void)argv;
    /* No tccrun.o: -run is unsupported on AuraLite.  Say so on stderr,
     * and return failure so the caller's error path is taken. */
    fprintf(stderr, "tcc: -run is not supported on AuraLite (self-host "
                    "builds use -o <elf>)\n");
    return -1;
}

void tcc_run_free(TCCState *s1)
{
    (void)s1;   /* no run state was ever allocated */
}
