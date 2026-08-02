/* hostile argv probe: the cases a normal run never produces. */
#include "stdio.h"
#include "unistd.h"
#include "errno.h"

int main(void) {
    printf("HOSTILE: begin\n"); fflush(stdout);

    /* 1. argv pointer into kernel space */
    char *const *bad = (char *const *)0xFFFF800000000000ULL;
    pid_t r = spawnv("/bin/hello", bad);
    printf("HOSTILE: kernel-pointer argv -> %lld\n", (long long)r); fflush(stdout);

    /* 2. a vector whose first entry points into kernel space */
    char *v2[2]; v2[0] = (char *)0xFFFF800000000000ULL; v2[1] = 0;
    r = spawnv("/bin/hello", v2);
    printf("HOSTILE: kernel-pointer string -> %lld\n", (long long)r); fflush(stdout);

    /* 3. A vector with no terminator of its own.
     *
     * Honest note on what this does and does not prove: v3 is in BSS, which
     * is zero-filled, so the kernel's walk finds a NULL just past the end and
     * stops.  The spawn therefore SUCCEEDS.  What is being checked here is
     * that walking off the end of a vector does not fault the kernel or run
     * away -- EXEC_MAX_ARGS bounds it at 256 entries either way.  A vector
     * that genuinely never terminates before unmapped memory is case 2. */
    static char *v3[64];
    for (int i = 0; i < 64; i++) v3[i] = (char *)"x";
    r = spawnv("/bin/hello", v3);
    printf("HOSTILE: unterminated argv -> %s\n",
           r > 0 ? "bounded, spawned" : "bounded, refused"); fflush(stdout);

    /* 4. a NULL vector must behave exactly like spawn() */
    r = spawnv("/bin/hello", 0);
    printf("HOSTILE: null argv -> %s\n", r > 0 ? "spawned" : "failed"); fflush(stdout);

    printf("HOSTILE: survived\n"); fflush(stdout);
    return 0;
}
