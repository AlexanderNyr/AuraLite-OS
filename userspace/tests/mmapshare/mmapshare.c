/* mmapshare.c — MATURITY_PLAN M4 gate program.
 *
 * Proves that MAP_SHARED|MAP_ANONYMOUS really shares one physical page
 * across fork, and that MAP_PRIVATE really does not.
 *
 * The second half matters as much as the first.  A mapping that shares
 * everything would pass a "child sees the parent's write" test while being
 * badly broken, so the private case is the control: the same sequence over
 * MAP_PRIVATE must show copy-on-write isolation instead.
 *
 * Added by AUDIT_A1.  M4's integration gate (test_mmap_shared.sh) existed
 * but called an API that does not exist, so it had never run and there was
 * no program for it to run either -- it only ever asked the shell to
 * execute the generic selftest.
 */

#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "sys/wait.h"
#include "sys/mman.h"

#define PAGE 4096

static int fails = 0;

static void check(const char *what, int ok) {
    printf("  %s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    printf("== mmapshare: M4 MAP_SHARED across fork ==\n");

    /* ---- 1) MAP_SHARED|MAP_ANONYMOUS: the child must see the parent ---- */
    char *shared = mmap(0, PAGE, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == (void *)-1 || !shared) {
        printf("  mmap(MAP_SHARED|MAP_ANONYMOUS) FAIL: could not map\n");
        printf("== %d/%d passed ==\n", 0, 4);
        return 1;
    }
    check("mmap(MAP_SHARED|MAP_ANONYMOUS)", 1);

    memset(shared, 0, 64);
    strcpy(shared, "PARENT-WROTE-THIS");

    int pid = fork();
    if (pid == 0) {
        /* Child: read what the parent wrote, then answer in the same page. */
        if (strcmp(shared, "PARENT-WROTE-THIS") == 0) {
            strcpy(shared + 32, "CHILD-SAW-IT");
        } else {
            strcpy(shared + 32, "CHILD-SAW-NOTHING");
        }
        _exit(0);
    }

    int status = 0;
    wait(&status);

    check("child observed the parent's write through MAP_SHARED",
          strcmp(shared + 32, "CHILD-SAW-IT") == 0);

    /* The parent must also see what the child wrote -- sharing is two-way. */
    check("parent observed the child's write back",
          strcmp(shared + 32, "CHILD-SAW-IT") == 0);

    /* ---- 2) The control: MAP_PRIVATE must NOT share ---- */
    char *priv = mmap(0, PAGE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (priv == (void *)-1 || !priv) {
        printf("  mmap(MAP_PRIVATE|MAP_ANONYMOUS) FAIL: could not map\n");
        fails++;
    } else {
        memset(priv, 0, 64);
        strcpy(priv, "PARENT-PRIVATE");

        int pid2 = fork();
        if (pid2 == 0) {
            strcpy(priv, "CHILD-OVERWROTE");
            _exit(0);
        }
        wait(&status);

        /* Copy-on-write: the parent's copy must be untouched. */
        check("MAP_PRIVATE stayed private across fork (control)",
              strcmp(priv, "PARENT-PRIVATE") == 0);
    }

    printf("== %d/%d passed ==\n", 4 - fails, 4);
    return fails ? 1 : 0;
}
