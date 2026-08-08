/* auxvtest.c -- M5 (MATURITY_PLAN.md) auxiliary-vector gate.
 *
 * Until M5 the kernel built an auxv containing only an AT_NULL terminator, so
 * getauxval() returned 0 for every type and there was no AT_RANDOM (the
 * stack-canary source crt0 wants), no AT_PAGESZ, no AT_EXECFN.  This program
 * reads the auxv via getauxval() and checks the entries a real program (or a
 * future dynamic loader) depends on.
 *
 * Prints "AUXV PASS" on success.
 */

#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "sys/auxv.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fails = 0;

    unsigned long pagesz = getauxval(AT_PAGESZ);
    unsigned long entry  = getauxval(AT_ENTRY);
    unsigned long phnum  = getauxval(AT_PHNUM);
    unsigned long phent  = getauxval(AT_PHENT);
    unsigned long random = getauxval(AT_RANDOM);
    unsigned long execfn = getauxval(AT_EXECFN);

    if (pagesz != 4096) { printf("[auxv] FAIL pagesz=%lu (want 4096)\n", pagesz); fails++; }
    if (phent  != 56)   { printf("[auxv] FAIL phent=%lu (want 56)\n", phent); fails++; }
    if (entry  == 0)    { printf("[auxv] FAIL AT_ENTRY missing\n"); fails++; }
    if (phnum  == 0)    { printf("[auxv] FAIL AT_PHNUM missing\n"); fails++; }

    /* AT_RANDOM: a valid pointer to 16 non-all-zero seed bytes. */
    if (random == 0) {
        printf("[auxv] FAIL AT_RANDOM missing\n"); fails++;
    } else {
        unsigned char *r = (unsigned char *)random;
        int any = 0;
        for (int i = 0; i < 16; i++) any |= r[i];
        if (!any) { printf("[auxv] FAIL AT_RANDOM all zero\n"); fails++; }
    }

    /* AT_EXECFN: the path the program was run with (must name this binary). */
    if (execfn == 0) {
        printf("[auxv] FAIL AT_EXECFN missing\n"); fails++;
    } else {
        const char *fn = (const char *)execfn;
        if (strstr(fn, "auxvtest") == NULL) {
            printf("[auxv] FAIL AT_EXECFN='%s'\n", fn); fails++;
        }
    }

    printf("[auxv] pagesz=%lu entry=0x%lx phnum=%lu random=0x%lx execfn ok=%d\n",
           pagesz, entry, phnum, random, execfn != 0);
    fflush(stdout);

    if (fails == 0) { printf("AUXV PASS\n"); return 0; }
    printf("AUXV FAIL\n");
    return 1;
}
