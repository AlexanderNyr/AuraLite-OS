/* tests/unit/test_stdio_seek.c — DOOM_PLAN.md D1.
 *
 * fseek/ftell/rewind, memmove and abs — the libc gaps found while scoping
 * the DOOM port.
 *
 * These are AuraLite's OWN implementations, compiled into this test rather
 * than the host's. That distinction matters: test_stdio_ext.c links the
 * host libc because the functions it covers exist everywhere, which is
 * fine for checking an API surface but would test nothing here.
 *
 * The interesting case throughout is the BUFFER. AuraLite's FILE stages
 * bytes in an internal buffer, so the fd's offset runs ahead of the
 * position the program believes it is at. A naive ftell() returns the
 * read-ahead position and a naive fseek() leaves stale bytes in the
 * buffer; both produce silently wrong data rather than an error, which is
 * exactly how a WAD file ends up loading garbage.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

static int passed = 0, failed = 0, tn = 0;

#define CHECK(cond, msg) do {                                              \
        if (cond) { passed++; }                                            \
        else { failed++; printf("    FAIL L%d: %s\n", __LINE__, (msg)); }  \
    } while (0)

#define RUN(f) do { tn++; printf("  [%d] %s\n", tn, #f); f(); } while (0)

/* ---- the code under test ------------------------------------------------
 *
 * AuraLite's fseek/ftell/rewind are reimplemented here against the host's
 * FILE rather than #included, because stdio_extra.c reaches into AuraLite's
 * own FILE layout (f->bufcap, f->readpos, f->ungot) which the host's stdio
 * does not have. What IS shared is the logic, so the port of it below is
 * kept line-for-line equivalent and the semantics are what get asserted.
 *
 * memmove() and abs() have no such dependency and are included directly
 * from the real sources — those are the actual shipped implementations.
 */

/* A minimal stand-in for AuraLite's buffered FILE, with the same fields the
 * real fseek/ftell manipulate. */
typedef struct {
    int   fd;
    int   flags;
    int   bufpos;    /* bytes staged for writing        */
    int   bufcap;    /* valid bytes in the read buffer  */
    int   readpos;   /* read cursor within the buffer   */
    int   dir;       /* 0 none, 1 reading, 2 writing    */
    int   ungot;     /* pushed-back char, or -1         */
} AFILE;

#define AFILE_EOF 0x01
#define AFILE_ERR 0x02

/* Mirrors lib/libc/src/stdio_extra.c:ftell(). */
static long a_ftell(AFILE *f) {
    if (!f) { errno = EINVAL; return -1; }
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) { f->flags |= AFILE_ERR; return -1; }
    if (f->dir == 1) {
        pos -= (off_t)(f->bufcap - f->readpos);
        if (f->ungot >= 0) pos -= 1;
    } else if (f->dir == 2) {
        pos += (off_t)f->bufpos;
    }
    return (long)pos;
}

/* Mirrors lib/libc/src/stdio_extra.c:fseek(). */
static int a_fseek(AFILE *f, long offset, int whence) {
    if (!f) { errno = EINVAL; return -1; }
    if (whence == SEEK_CUR) {
        long here = a_ftell(f);
        if (here < 0) return -1;
        offset = here + offset;
        whence = SEEK_SET;
    }
    off_t pos = lseek(f->fd, (off_t)offset, whence);
    if (pos < 0) { f->flags |= AFILE_ERR; return -1; }
    f->bufpos = 0; f->bufcap = 0; f->readpos = 0;
    f->ungot = -1; f->dir = 0;
    f->flags &= ~AFILE_EOF;
    return 0;
}

/* The real implementations, compiled in.
 *
 * They are called through these ALIASES rather than by their standard
 * names, and that is not cosmetic: GCC recognises memmove() and abs() as
 * builtins and replaces the calls with its own inline expansion, so a test
 * written against the plain names silently exercises the compiler instead
 * of the code under test. Caught by mutation -- breaking memmove's
 * direction test changed nothing until the calls went through these. */
void *auralite_memmove(void *dst, const void *src, size_t n);
int   auralite_abs(int v);

/* ---- fixtures ----------------------------------------------------------- */

static int make_temp_file(const char *content, size_t len) {
    char path[] = "/tmp/auralite_seek_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);              /* anonymous: cleaned up on close */
    if (write(fd, content, len) != (ssize_t)len) { close(fd); return -1; }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

/* ---- the buffered-position tests ---------------------------------------- */

/* The core case. The stream has read 64 bytes into its buffer but the
 * program has only consumed 10, so the fd sits at 64 while the logical
 * position is 10. ftell() must say 10. */
static void ftell_accounts_for_the_read_buffer(void) {
    char data[128];
    for (int i = 0; i < 128; i++) data[i] = (char)i;
    int fd = make_temp_file(data, sizeof data);
    CHECK(fd >= 0, "temp file created");
    if (fd < 0) return;

    lseek(fd, 64, SEEK_SET);          /* pretend 64 bytes were buffered */
    AFILE f = { fd, 0, 0, 64, 10, 1, -1 };

    CHECK(a_ftell(&f) == 10,
          "ftell reports the logical position, not the fd's read-ahead");

    /* A pushed-back character puts the logical position one further back. */
    f.ungot = 'x';
    CHECK(a_ftell(&f) == 9, "a pushed-back character is accounted for");

    close(fd);
}

/* The write side is the mirror image: staged bytes have not reached the fd,
 * so the logical position is AHEAD of it. */
static void ftell_accounts_for_staged_writes(void) {
    int fd = make_temp_file("", 0);
    CHECK(fd >= 0, "temp file created");
    if (fd < 0) return;

    AFILE f = { fd, 0, 25, 0, 0, 2, -1 };   /* 25 bytes staged, fd at 0 */
    CHECK(a_ftell(&f) == 25, "ftell adds bytes still staged for writing");
    close(fd);
}

/* SEEK_CUR must be relative to the LOGICAL position. Resolving it against
 * the fd instead lands wherever the read-ahead happened to stop — the bug
 * this test exists for. */
static void seek_cur_is_relative_to_the_logical_position(void) {
    char data[128];
    for (int i = 0; i < 128; i++) data[i] = (char)i;
    int fd = make_temp_file(data, sizeof data);
    if (fd < 0) { CHECK(0, "temp file"); return; }

    lseek(fd, 64, SEEK_SET);
    AFILE f = { fd, 0, 0, 64, 10, 1, -1 };   /* logical 10, fd at 64 */

    CHECK(a_fseek(&f, 5, SEEK_CUR) == 0, "SEEK_CUR succeeds");
    CHECK(a_ftell(&f) == 15,
          "SEEK_CUR moved 5 from the logical position (10), not from 64");

    /* And the next byte read must be data[15]. */
    unsigned char b = 0;
    CHECK(read(fd, &b, 1) == 1 && b == 15,
          "the next byte read is the one at the new position");
    close(fd);
}

/* fseek must discard buffered read state, or the next read returns bytes
 * from the old position. */
static void seek_discards_the_buffer(void) {
    char data[128];
    for (int i = 0; i < 128; i++) data[i] = (char)i;
    int fd = make_temp_file(data, sizeof data);
    if (fd < 0) { CHECK(0, "temp file"); return; }

    AFILE f = { fd, AFILE_EOF, 0, 64, 10, 1, 'z' };
    CHECK(a_fseek(&f, 100, SEEK_SET) == 0, "seek succeeds");

    CHECK(f.bufcap == 0 && f.readpos == 0, "the read buffer is dropped");
    CHECK(f.ungot == -1, "the pushed-back character is dropped");
    CHECK(f.dir == 0, "the direction is reset");
    /* C89: a successful fseek clears end-of-file. */
    CHECK(!(f.flags & AFILE_EOF), "EOF is cleared by a successful seek");
    CHECK(a_ftell(&f) == 100, "and the position is where we asked");
    close(fd);
}

static void seek_end_and_back(void) {
    char data[64];
    memset(data, 'A', sizeof data);
    int fd = make_temp_file(data, sizeof data);
    if (fd < 0) { CHECK(0, "temp file"); return; }
    AFILE f = { fd, 0, 0, 0, 0, 0, -1 };

    CHECK(a_fseek(&f, 0, SEEK_END) == 0, "seek to end");
    CHECK(a_ftell(&f) == 64, "end is the file size");

    /* Seeking to a negative offset from the end is how a WAD reader finds
     * its directory. */
    CHECK(a_fseek(&f, -16, SEEK_END) == 0, "seek back from the end");
    CHECK(a_ftell(&f) == 48, "landed 16 bytes before the end");

    CHECK(a_fseek(&f, 0, SEEK_SET) == 0, "rewind to the start");
    CHECK(a_ftell(&f) == 0, "back at zero");
    close(fd);
}

static void seek_on_a_bad_stream_is_refused(void) {
    CHECK(a_fseek(NULL, 0, SEEK_SET) == -1, "NULL stream refused");
    CHECK(a_ftell(NULL) == -1, "ftell on NULL refused");

    /* A closed fd must report an error rather than pretend to work. */
    AFILE f = { 9999, 0, 0, 0, 0, 0, -1 };
    CHECK(a_fseek(&f, 0, SEEK_SET) == -1, "seek on a bad fd fails");
    CHECK(f.flags & AFILE_ERR, "and the error flag is set");
}

/* ---- memmove ------------------------------------------------------------ */

static void memmove_handles_overlap(void) {
    char buf[16];

    /* Forward overlap: destination inside the source. A plain memcpy would
     * clobber bytes it has not read yet. */
    memcpy(buf, "0123456789ABCDEF", 16);
    auralite_memmove(buf + 2, buf, 8);
    CHECK(memcmp(buf, "0101234567ABCDEF", 16) == 0,
          "overlapping forward move copies backwards");

    /* Backward overlap: source inside the destination. */
    memcpy(buf, "0123456789ABCDEF", 16);
    auralite_memmove(buf, buf + 2, 8);
    CHECK(memcmp(buf, "23456789" "89ABCDEF", 16) == 0,
          "overlapping backward move copies forwards");

    /* Exact overlap and zero length must be no-ops, not crashes. */
    memcpy(buf, "0123456789ABCDEF", 16);
    CHECK(auralite_memmove(buf, buf, 16) == buf, "self-move returns the destination");
    CHECK(memcmp(buf, "0123456789ABCDEF", 16) == 0, "self-move changes nothing");
    CHECK(auralite_memmove(buf, buf + 4, 0) == buf, "zero length returns the destination");
    CHECK(memcmp(buf, "0123456789ABCDEF", 16) == 0, "zero length changes nothing");

    /* Non-overlapping still has to work. */
    char dst[8];
    auralite_memmove(dst, "abcdefg", 8);
    CHECK(strcmp(dst, "abcdefg") == 0, "non-overlapping move is a plain copy");
}

/* ---- abs ---------------------------------------------------------------- */

static void abs_basics(void) {
    CHECK(auralite_abs(5) == 5, "abs of a positive");
    CHECK(auralite_abs(-5) == 5, "abs of a negative");
    CHECK(auralite_abs(0) == 0, "abs of zero");

    /* abs(INT_MIN) is undefined in C because -INT_MIN overflows. The
     * contract here is that it returns the argument rather than trapping or
     * letting the optimiser assume the case cannot happen. */
    CHECK(auralite_abs(INT_MIN) == INT_MIN, "abs(INT_MIN) returns INT_MIN, not UB");
    CHECK(auralite_abs(INT_MAX) == INT_MAX, "abs of INT_MAX");
}

int main(void) {
    printf("== test_stdio_seek (DOOM_PLAN D1) ==\n");
    RUN(ftell_accounts_for_the_read_buffer);
    RUN(ftell_accounts_for_staged_writes);
    RUN(seek_cur_is_relative_to_the_logical_position);
    RUN(seek_discards_the_buffer);
    RUN(seek_end_and_back);
    RUN(seek_on_a_bad_stream_is_refused);
    RUN(memmove_handles_overlap);
    RUN(abs_basics);
    printf("== %d passed, %d failed ==\n", passed, failed);
    return failed ? 1 : 0;
}
