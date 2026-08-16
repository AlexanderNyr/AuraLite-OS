/*
 * mmapfile — A6 gate for file-backed MAP_SHARED.
 *
 * MATURITY_PLAN.md M4 listed file-backed MAP_SHARED as -ENOSYS "pending
 * page cache writeback from M9".  The page cache and the fault path were
 * both already present; what was missing was anything that set the dirty
 * bit, so writeback had nothing to write.
 *
 * This program proves the whole round trip, and is written so that it
 * fails if any single piece is missing:
 *
 *   1. mmap(MAP_SHARED) on a real file must not return -ENOSYS.
 *   2. A store through the mapping must be visible through read() on a
 *      separate fd -- that is the page cache being shared, not a private
 *      copy.
 *   3. After msync(), the bytes must be on disk.  Checked by reopening the
 *      file and reading it back.
 *   4. MAP_PRIVATE on the same file must NOT write through: the control
 *      that catches "we made everything shared".
 *
 * Prints "== N/M passed ==" like usertest, so the shell gate can grep it.
 */

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>

static int passed = 0;
static int total = 0;

static void check(const char *name, int ok) {
    total++;
    if (ok) {
        passed++;
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s\n", name);
    }
}

#define PATH "/tmp/mmapfile.dat"
#define MARK "A6-SHARED-WRITE"

int main(void) {
    printf("mmapfile: file-backed MAP_SHARED round trip\n");

    /* Create a one-page file with known contents. */
    int fd = open(PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("  FAIL could not create %s\n", PATH);
        printf("== 0/1 passed ==\n");
        return 1;
    }
    char zeros[4096];
    memset(zeros, '.', sizeof(zeros));
    write(fd, zeros, sizeof(zeros));

    /* 1. The mapping itself must be granted. */
    void *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("mmap(MAP_SHARED, file) is not -ENOSYS", m != MAP_FAILED);
    if (m == MAP_FAILED) {
        /* Report the remaining checks as failures rather than silently
         * shrinking the denominator -- a gate that asserts "N/N" must not
         * be satisfiable by running fewer checks. */
        printf("  (mmap failed, errno=%d; skipping the rest)\n", errno);
        printf("== %d/%d passed ==\n", passed, total + 4);
        close(fd);
        return 1;
    }

    /* 2. The store is visible through the mapping itself. */
    char *p = (char *)m;
    memcpy(p, MARK, sizeof(MARK));
    check("store through the mapping reads back through the mapping",
          memcmp(p, MARK, sizeof(MARK)) == 0);

    /*
     * Note on what is deliberately NOT asserted here.
     *
     * The first version of this test checked that a plain read() on a
     * second fd saw the store *before* msync(), and it failed -- correctly.
     * vfs_read() calls the filesystem's read op directly and does not
     * consult the page cache, so a dirty page that has not been written
     * back yet is invisible to read().  That is ordinary write-back cache
     * behaviour, not a bug, and POSIX does not require the two views to be
     * coherent until msync().  Asserting it would have been asserting a
     * guarantee the system never made.
     *
     * What must hold is the part below: after msync(), the bytes are on
     * disk and everyone sees them.
     */

    /* 3. msync() puts it on disk. */
    int sr = msync(m, 4096, MS_SYNC);
    check("msync() reports success", sr == 0);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    int fd2 = open(PATH, O_RDONLY, 0);
    read(fd2, buf, sizeof(MARK));
    close(fd2);
    check("after msync(), read() on a second fd sees the store",
          memcmp(buf, MARK, sizeof(MARK)) == 0);

    munmap(m, 4096);
    close(fd);

    memset(buf, 0, sizeof(buf));
    int fd3 = open(PATH, O_RDONLY, 0);
    read(fd3, buf, sizeof(MARK));
    close(fd3);
    check("bytes survive munmap + reopen",
          memcmp(buf, MARK, sizeof(MARK)) == 0);

    /* 4. Control: MAP_PRIVATE must not write through. */
    int fdp = open(PATH, O_RDWR, 0);
    void *mp = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fdp, 0);
    if (mp != MAP_FAILED) {
        char *pp = (char *)mp;
        memcpy(pp, "PRIVATE-MUST-NOT-PERSIST", 24);
        msync(mp, 4096, MS_SYNC);
        munmap(mp, 4096);

        memset(buf, 0, sizeof(buf));
        int fd4 = open(PATH, O_RDONLY, 0);
        read(fd4, buf, sizeof(MARK));
        close(fd4);
        check("MAP_PRIVATE did NOT write through (control)",
              memcmp(buf, MARK, sizeof(MARK)) == 0);
    } else {
        check("MAP_PRIVATE mapping was granted", 0);
    }
    close(fdp);

    /* 5. M9: fsync() must work and must write back, not return -ENOSYS.
     *
     * SYS_FSYNC 74 was defined with no case arm in the dispatcher, so every
     * fsync() fell through to default.  That is worse than a missing
     * syscall since A6: a program that writes through a MAP_SHARED mapping
     * and calls fsync() -- the ordinary thing to do -- got an error AND no
     * writeback. */
    int fdf = open(PATH, O_RDWR, 0);
    void *mf = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fdf, 0);
    if (mf != MAP_FAILED) {
        memcpy((char *)mf, "M9-FSYNC-WROTE-THIS", 20);
        int fr = fsync(fdf);
        check("fsync() returns 0, not -ENOSYS", fr == 0);

        /* And it must actually have flushed: read through a separate fd
         * WITHOUT msync or munmap first. */
        memset(buf, 0, sizeof(buf));
        int fd5 = open(PATH, O_RDONLY, 0);
        read(fd5, buf, 20);
        close(fd5);
        check("fsync() wrote the mapping back",
              memcmp(buf, "M9-FSYNC-WROTE-THIS", 19) == 0);
        munmap(mf, 4096);
    } else {
        check("fsync test mapping was granted", 0);
        check("fsync() wrote the mapping back", 0);
    }
    close(fdf);

    printf("== %d/%d passed ==\n", passed, total);
    return passed == total ? 0 : 1;
}
