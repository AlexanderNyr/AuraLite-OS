/* insttest.c — in-OS probe for the executable-installation policy.
 *
 * Phase F1 of FSLAYOUT_PLAN.md.  The host unit test
 * (tests/unit/test_execpolicy.c) covers the predicate exhaustively.  This
 * program covers what a pure predicate test cannot: that the rule is actually
 * wired into open() and chmod() on a running kernel, that a refusal is an
 * EPERM rather than some other failure, and — the part most likely to be got
 * wrong — that ordinary non-executable file creation still works everywhere.
 *
 * A restriction that also breaks normal writing is not a working restriction.
 * Half the checks here exist to prove nothing else was taken away.
 */

#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "errno.h"
#include "sys/stat.h"

static int checks = 0, failures = 0;

static void ok(const char *what) {
    checks++;
    printf("INSTTEST PASS: %s\n", what);
    fflush(stdout);
}

static void bad(const char *what, const char *detail) {
    checks++;
    failures++;
    printf("INSTTEST FAIL: %s (%s)\n", what, detail);
    fflush(stdout);
}

/* Creating an executable here must be refused, and refused BY THE POLICY.
 *
 * The errno matters.  An earlier run of this probe "passed" every refusal
 * while the policy was not being consulted at all: /evil was refused with
 * EROFS because the initrd is read-only, and /disk/evil with ENOENT because
 * no disk was attached.  Both are refusals; neither is this rule.  Requiring
 * EPERM is what makes the check about the thing it claims to test. */
static void expect_refused(const char *path) {
    errno = 0;
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (fd >= 0) {
        close(fd);
        unlink(path);
        bad("refuse exec create", path);
        return;
    }
    if (errno != EPERM) {
        printf("INSTTEST FAIL: %s refused with errno %d, wanted EPERM %d\n",
               path, errno, EPERM);
        checks++;
        failures++;
        return;
    }
    ok("refuse exec create");
}

/* Creating an executable here must succeed. */
static void expect_allowed(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (fd < 0) {
        bad("allow exec create", path);
        return;
    }
    write(fd, "x", 1);
    close(fd);
    ok("allow exec create");
    unlink(path);
}

/* Creating a plain data file must keep working — and in particular must not
 * fail with EPERM, which would mean the policy had caught something it has no
 * business catching.  A filesystem that is absent or read-only is a different
 * matter and is reported as a skip, not a failure: whether a disk is attached
 * is a property of the QEMU invocation, not of this rule. */
static void expect_data_ok(const char *path) {
    errno = 0;
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        if (errno == EPERM) {
            bad("data create wrongly refused by policy", path);
        } else {
            printf("INSTTEST SKIP: %s not writable here (errno %d)\n",
                   path, errno);
        }
        return;
    }
    write(fd, "data", 4);
    close(fd);
    ok("allow data create");
    unlink(path);
}

int main(void) {
    printf("INSTTEST: begin\n");
    fflush(stdout);

    /* --- installation is permitted where the policy says it is --- */
    expect_allowed("/opt/probe");
    expect_allowed("/tmp/probe");

    /* --- and refused everywhere else --- */
    expect_refused("/evil");
    expect_refused("/disk/evil");

    /* --- the traversal bypass, from a real process against a real VFS --- */
    expect_refused("/opt/../evil");
    expect_refused("/tmp/../evil");

    /* --- nothing else was taken away --- */
    expect_data_ok("/tmp/plain.txt");
    expect_data_ok("/opt/plain.txt");
    expect_data_ok("/disk/plain.txt");   /* skipped when no disk is attached */

    /* --- chmod cannot be used to go around open() --- */
    {
        int fd = open("/tmp/chmodme", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            bad("create file for chmod probe", "/tmp/chmodme");
        } else {
            close(fd);
            /* /tmp is on the allowlist, so this one must succeed. */
            if (chmod("/tmp/chmodme", 0755) == 0) ok("chmod +x inside allowlist");
            else bad("chmod +x inside allowlist", "/tmp/chmodme");
            unlink("/tmp/chmodme");
        }
    }
    {
        /* A file outside the allowlist must not be able to acquire the bit.
         *
         * /hello is in the initrd: it exists on every boot, it is outside the
         * allowlist, and its stored mode has no execute bits.  The initrd is
         * read-only, but chmod is a VFS-level operation on the vnode, so the
         * policy is what has to refuse it — and if it does not, the vnode's
         * mode really does change for the rest of the session. */
        errno = 0;
        if (chmod("/bin/hello", 0755) == 0) {
            bad("refuse chmod +x outside allowlist", "/bin/hello");
        } else if (errno != EPERM) {
            printf("INSTTEST FAIL: chmod /bin/hello refused with errno %d, "
                   "wanted EPERM %d\n", errno, EPERM);
            checks++;
            failures++;
        } else {
            ok("refuse chmod +x outside allowlist");
        }
    }

    /* NOTE ON TRAVERSAL, recorded because it changes what the checks above
     * actually prove.
     *
     * The VFS does not canonicalise paths: "/tmp/../evil" is split at the
     * "/tmp" mount and the remainder "../evil" is handed to tmpfs, which
     * rejects any name containing a slash.  So a traversal fails today even
     * with no policy at all — the refusals above are the policy answering
     * first, not the only thing standing in the way.
     *
     * That is worth having rather than relying on: the moment the VFS learns
     * to canonicalise, the incidental protection disappears and the policy's
     * is what remains.  The host unit test proves the predicate itself
     * handles the case; this program proves the predicate is consulted. */

    /* --- a chmod that does not ADD an execute bit must not be refused --- */
    {
        int fd = open("/tmp/perm.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            bad("create file for mode-change probe", "/tmp/perm.txt");
        } else {
            close(fd);
            if (chmod("/tmp/perm.txt", 0600) == 0) {
                ok("chmod without +x still allowed");
            } else {
                bad("chmod without +x", "/tmp/perm.txt");
            }
            unlink("/tmp/perm.txt");
        }
    }

    printf("INSTTEST: %d checks, %d failures\n", checks, failures);
    printf("INSTTEST: %s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    fflush(stdout);
    return failures == 0 ? 0 : 1;
}
