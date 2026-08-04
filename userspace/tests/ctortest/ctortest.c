/* ctortest — FIX_R5 gate: .init_array / .fini_array actually run, in order.
 *
 * Three __attribute__((constructor)) functions and three
 * __attribute__((destructor)) functions append tokens to a global log
 * buffer.  Both verdicts are judged in-program from that buffer, so order
 * is checked end to end, not only by the presence of output lines:
 *
 *   - main() must already see "123": constructors ran BEFORE main, in link
 *     order (the runtime walks .init_array forwards);
 *   - the last destructor to run must see "123m321": destructors ran AFTER
 *     main returned, in reverse link order (the runtime walks .fini_array
 *     backwards).
 *
 * Pre-R5 the arrays were linked but never walked, so main() sees an empty
 * log and both verdicts FAIL — which is exactly the gate's "reverting the
 * runtime walk makes the test fail".
 */

#include "stdio.h"
#include "string.h"

static char ctor_log[32];

/* ---- constructors: must run in definition/link order 1, 2, 3 ---- */

__attribute__((constructor)) static void ctor_1(void) { strcat(ctor_log, "1"); }
__attribute__((constructor)) static void ctor_2(void) { strcat(ctor_log, "2"); }
__attribute__((constructor)) static void ctor_3(void) {
    strcat(ctor_log, "3");
    printf("CTORTEST ctors ran (log=%s)\n", ctor_log);
}

/* ---- destructors: must run in reverse 3, 2, 1 ---- */

__attribute__((destructor)) static void dtor_1(void) {
    strcat(ctor_log, "1");
    /* dtor_1 unwinds last and sees the whole trace. */
    if (strcmp(ctor_log, "123m321") == 0) {
        printf("CTORTEST PASS dtors after main, reverse order (log=%s)\n",
               ctor_log);
    } else {
        printf("CTORTEST FAIL dtor order (log=%s)\n", ctor_log);
    }
    fflush(stdout);
}
__attribute__((destructor)) static void dtor_2(void) { strcat(ctor_log, "2"); }
__attribute__((destructor)) static void dtor_3(void) { strcat(ctor_log, "3"); }

int main(void) {
    if (strcmp(ctor_log, "123") == 0) {
        printf("CTORTEST PASS ctors before main, link order (log=%s)\n",
               ctor_log);
    } else {
        printf("CTORTEST FAIL ctor order (log=%s)\n", ctor_log[0] ? ctor_log : "<empty>");
    }
    strcat(ctor_log, "m");
    fflush(stdout);
    return 0;
}
