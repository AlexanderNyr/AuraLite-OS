/* execvetest.c — prove execvpe() and fexecve() in-tree (RESIDUE2 T4).
 *
 * The prototypes existed since P10, but a prototype is not an
 * implementation receipt: nothing in the tree ever CALLED either function,
 * so a stub that returned -ENOSYS would have passed every existing gate.
 * This program is the proof, four lanes deep:
 *
 *   execvetest fexec    fexecve(open("/tests/argv_echo")) replaces the
 *                       image; the successor's ARGV_ECHO markers carry the
 *                       argv/envp THIS call marshalled (XEC_FEXEC).
 *   execvetest vpe      execvpe("argv_echo", ...) with envp that sets
 *                       PATH=/tests:/bin — proves the search reads the
 *                       CHILD's environment, not the global one.
 *   execvetest vpe-cwd  PATH=":" — the empty first segment means the
 *                       current directory; run from /tests it must find
 *                       ./argv_echo before the /bin fallback.
 *   execvetest vpe-miss execvpe of a name that is nowhere must RETURN -1
 *                       with ENOENT (the exec must not succeed, and the
 *                       loop must not spin forever on the empty segment).
 *
 * The first three lanes never return on success; their "pass" output is
 * printed by the successor image.  A lane that returns prints FAILED with
 * errno so the integration case can tell exactly which one broke.
 */

#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "errno.h"

static int lane_fexec(void) {
    int fd = open("/tests/argv_echo", O_RDONLY);
    if (fd < 0) {
        printf("XEC fexec open FAILED errno=%d\n", errno);
        return 1;
    }
    char *argv[] = { "argv_echo", "XEC_FEXEC", 0 };
    char *envp[] = { "XEC=1", "SHELL=/init", 0 };
    printf("XEC fexecve calling (fd=%d)\n", fd);
    int r = fexecve(fd, argv, envp);
    /* Reaching here at all is the failure path. */
    printf("XEC fexecve FAILED errno=%d ret=%d\n", errno, r);
    close(fd);
    return 1;
}

static int lane_vpe(void) {
    char *argv[] = { "argv_echo", "XEC_VPE", 0 };
    /* PATH names /tests FIRST: the global environment does not carry it,
     * so resolving proves execvpe searched envp's PATH. */
    char *envp[] = { "PATH=/tests:/bin", "XEC=2", 0 };
    printf("XEC execvpe calling (PATH=/tests:/bin)\n");
    int r = execvpe("argv_echo", argv, envp);
    printf("XEC execvpe FAILED errno=%d ret=%d\n", errno, r);
    return 1;
}

static int lane_vpe_cwd(void) {
    char *argv[] = { "argv_echo", "XEC_CWD", 0 };
    /* Empty leading segment: POSIX resolves it as the current directory.
     * The shell starts in /, so chdir to /tests first. */
    if (chdir("/tests") != 0) {
        printf("XEC vpe-cwd chdir FAILED errno=%d\n", errno);
        return 1;
    }
    char *envp[] = { "PATH=:/bin", "XEC=3", 0 };
    printf("XEC execvpe cwd-segment calling (PATH=:/bin)\n");
    int r = execvpe("argv_echo", argv, envp);
    printf("XEC execvpe cwd-segment FAILED errno=%d ret=%d\n", errno, r);
    return 1;
}

static int lane_vpe_miss(void) {
    char *argv[] = { "xec_not_anywhere", 0 };
    char *envp[] = { "PATH=/bin:/apps:/demos", 0 };
    int r = execvpe("xec_not_anywhere", argv, envp);
    if (r == -1 && errno == ENOENT) {
        printf("XEC_VPE_MISS_OK ENOENT\n");
        return 0;
    }
    printf("XEC vpe-miss FAILED ret=%d errno=%d (wanted -1/ENOENT)\n", r, errno);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: execvetest fexec|vpe|vpe-cwd|vpe-miss\n");
        return 1;
    }
    if (strcmp(argv[1], "fexec") == 0)    return lane_fexec();
    if (strcmp(argv[1], "vpe") == 0)      return lane_vpe();
    if (strcmp(argv[1], "vpe-cwd") == 0)  return lane_vpe_cwd();
    if (strcmp(argv[1], "vpe-miss") == 0) return lane_vpe_miss();
    printf("XEC unknown lane '%s'\n", argv[1]);
    return 1;
}
