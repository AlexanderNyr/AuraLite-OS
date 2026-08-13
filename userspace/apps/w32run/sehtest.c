/* userspace/apps/w32run/sehtest.c — WIN32_PLAN.md phase W32-6 gate.
 *
 * Exercises the __try/__except shim from native code.
 *
 * This is a native ELF rather than a PE on purpose.  The shim is a property
 * of the w32 runtime library, not of the PE loader, and testing it here
 * isolates it: a failure is the SEH shim's and cannot be a loader or import
 * binding problem.  The PE side is already covered by crttest.exe.
 *
 * The case that matters most is FAULTING TWICE.  A signal is blocked while
 * its own handler runs, so a shim built on plain setjmp/longjmp catches the
 * first fault and then dies on the second, with the mask still blocking it.
 * That bug is invisible in a test that faults once.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32/w32_crt.h"

/* volatile so the compiler cannot fold the division away at compile time --
 * it is entitled to, and then the fault under test never happens. */
static volatile int zero = 0;
static volatile int sink;

/* A local modified between sigsetjmp() and siglongjmp() and then read after
 * the jump has an INDETERMINATE value unless it is volatile -- C11 7.13.2.1.
 * GCC diagnoses exactly this (-Wclobbered), and it is a real bug, not a
 * false positive: these counters are written inside __try bodies and read
 * after a fault has jumped out.  File scope also sidesteps the question of
 * which registers the jump restores. */
static int failures;

/* The address used for the deliberate access violation.  Kept behind a
 * volatile pointer variable so the compiler cannot prove at compile time
 * that the dereference is invalid and reject the program instead of letting
 * it fault at run time (-Warray-bounds fires on a literal constant). */
static volatile int *volatile bad_ptr = (volatile int *)(void *)16;

/* The last-chance filter for "filter" mode.  ms_abi because that is how a
 * Win32 program would supply it. */
static int32_t W32ABI last_chance(void *info) {
    (void)info;
    printf("SEH-FILTER-CALLED\n");
    fflush(stdout);
    return W32_EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv) {
    /* "crash" mode: fault with NO __try active.  The gate requires this to
     * terminate the process cleanly -- with the right signal, and without
     * taking the kernel with it -- rather than being swallowed.  A shim that
     * caught faults it had no handler for would turn a real crash into a
     * hang or a wrong exit status. */
    if (argc > 1 && strcmp(argv[1], "crash") == 0) {
        if (w32_seh_init() != 0) return 1;
        printf("SEH-UNGUARDED-FAULT\n");
        fflush(stdout);
        sink = 10 / zero;
        printf("SEH-SURVIVED-UNGUARDED\n");   /* must NOT be reached */
        return 1;
    }

    /* "filter" mode: the same unguarded fault, but with an unhandled
     * exception filter installed.  Returning EXECUTE_HANDLER means "handled;
     * terminate" -- the process should exit quietly through the filter
     * instead of dying on the signal. */
    if (argc > 1 && strcmp(argv[1], "filter") == 0) {
        if (w32_seh_init() != 0) return 1;
        w32_SetUnhandledExceptionFilter(last_chance);
        printf("SEH-FILTER-INSTALLED\n");
        fflush(stdout);
        sink = 10 / zero;
        printf("SEH-FILTER-NOT-CALLED\n");    /* must NOT be reached */
        return 1;
    }


    if (w32_seh_init() != 0) {
        printf("SEH-INIT-FAILED\n");
        return 1;
    }

    /* 1. divide by zero inside __try reaches __except */
    if (w32_try_begin() == 0) {
        sink = 10 / zero;
        w32_try_end();
        printf("SEH-NOT-CAUGHT\n");
        failures++;
    } else {
        unsigned code = (unsigned)w32_exception_code();
        if (code == W32_EXCEPTION_INT_DIVIDE_BY_ZERO) {
            printf("SEH-DIV0-CAUGHT\n");
        } else {
            printf("SEH-WRONG-CODE 0x%08x\n", code);
            failures++;
        }
    }

    /* 2. a SECOND fault must also be caught.  This is the mask test: with
     *    plain longjmp instead of siglongjmp, SIGFPE would still be blocked
     *    here and this would kill the process. */
    if (w32_try_begin() == 0) {
        sink = 20 / zero;
        w32_try_end();
        printf("SEH-SECOND-NOT-CAUGHT\n");
        failures++;
    } else {
        printf("SEH-SECOND-CAUGHT\n");
    }

    /* 3. an access violation is distinguished from a divide by zero */
    if (w32_try_begin() == 0) {
        sink = *bad_ptr;
        w32_try_end();
        printf("SEH-AV-NOT-CAUGHT\n");
        failures++;
    } else {
        unsigned code = (unsigned)w32_exception_code();
        if (code == W32_EXCEPTION_ACCESS_VIOLATION) printf("SEH-AV-CAUGHT\n");
        else { printf("SEH-AV-WRONG-CODE 0x%08x\n", code); failures++; }
    }

    /* 4. the non-faulting path must leave the nesting stack balanced.
     *    Running more __try blocks than the stack is deep proves pop()
     *    actually happens -- a shim that only pushed would run out here. */
    static volatile int i;
    for (i = 0; i < W32_SEH_MAX_DEPTH * 4; i++) {
        if (w32_try_begin() == 0) {
            sink = i;
            w32_try_end();
        } else {
            printf("SEH-SPURIOUS-CATCH\n");
            failures++;
            break;
        }
    }
    printf("SEH-BALANCED\n");

    /* 5. nesting: the INNER handler must get the fault, not the outer one. */
    if (w32_try_begin() == 0) {
        if (w32_try_begin() == 0) {
            sink = 30 / zero;
            w32_try_end();
            printf("SEH-INNER-NOT-CAUGHT\n");
            failures++;
        } else {
            printf("SEH-INNER-CAUGHT\n");
        }
        w32_try_end();
    } else {
        printf("SEH-OUTER-STOLE-IT\n");
        failures++;
    }

    if (failures == 0) {
        printf("W32-SEH-OK\n");
        return 44;
    }
    printf("W32-SEH-FAIL %d\n", failures);
    return 1;
}
