/*
 * test_sh_expand.c — host-side unit test for the shell's positional-parameter
 * expander (SELFHOST SH6a).
 *
 * This includes userspace/system/init/sh_expand.h and calls the SHIPPED body.
 * It does not re-implement the expander: a local copy would prove nothing
 * about what the guest shell actually runs, which is precisely the mistake
 * the original memchr test made before SH5d rewired it to link the generated
 * impl.  The header is pure (no syscalls, no globals), so the host compiler
 * can build it directly.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../userspace/system/init/sh_expand.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond) do { \
    if (cond) { passed++; } \
    else { printf("  FAIL: %d: %s\n", __LINE__, #cond); failed++; } \
} while (0)

#define DSTSZ 256

/* Expand `src` with the given positional array and report the result.
 * Returns the expander's status; the text lands in `out`. */
static int xp(const char *src, char *out, size_t cap,
               char *const *pos, int npos, int last_status)
{
    return sh_expand_positional(src, src ? strlen(src) : 0, out, cap,
                                pos, npos, last_status);
}

/* The common case: a script invoked as `sh build.sh kernel`. */
static char *p_build[3] = { (char *)"build.sh", (char *)"kernel", NULL };

static void test_plain_text(void) {
    char out[DSTSZ];

    CHECK(xp("echo hello", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo hello") == 0);

    CHECK(xp("", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(out[0] == '\0');

    /* NULL source is an empty result, not a crash. */
    CHECK(xp(NULL, out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(out[0] == '\0');

    /* Punctuation the tokenizer cares about must survive untouched. */
    CHECK(xp("run tcc -c -o /tmp/a.o /src/a.c", out, sizeof out,
              p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "run tcc -c -o /tmp/a.o /src/a.c") == 0);
}

static void test_positional(void) {
    char out[DSTSZ];

    CHECK(xp("echo $0", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo build.sh") == 0);

    CHECK(xp("echo $1", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo kernel") == 0);

    /* The whole point of SH6a: the target reaches the script. */
    CHECK(xp("shmake $1", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "shmake kernel") == 0);

    /* An absent parameter expands to nothing, not to "$2". */
    CHECK(xp("echo [$2]", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo []") == 0);

    CHECK(xp("echo [$9]", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo []") == 0);

    /* $10 is $1 followed by a literal '0' — POSIX has no multi-digit
     * positional without braces, and braces are SH6b. */
    CHECK(xp("echo $10", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo kernel0") == 0);

    /* Several parameters on one line, in any order. */
    CHECK(xp("echo $1 $0 $1", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo kernel build.sh kernel") == 0);

    /* With no script at all, $0 and $# are empty/zero rather than garbage. */
    CHECK(xp("echo [$0][$#]", out, sizeof out, NULL, 0, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo [][0]") == 0);
}

static void test_hash_and_status(void) {
    char out[DSTSZ];

    /* $# counts arguments, excluding $0. */
    CHECK(xp("echo $#", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo 1") == 0);

    char *four[5] = { (char *)"s", (char *)"a", (char *)"b", (char *)"c", NULL };
    CHECK(xp("echo $#", out, sizeof out, four, 4, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo 3") == 0);

    /* Multi-digit, so a single-digit formatter cannot pass by accident. */
    char *twelve[12];
    for (int i = 0; i < 11; i++) twelve[i] = (char *)"x";
    twelve[11] = NULL;
    CHECK(xp("echo $#", out, sizeof out, twelve, 11, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo 10") == 0);

    CHECK(xp("echo $?", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo 0") == 0);

    CHECK(xp("echo $?", out, sizeof out, p_build, 2, 1) == SH_EXP_OK);
    CHECK(strcmp(out, "echo 1") == 0);

    CHECK(xp("echo $?", out, sizeof out, p_build, 2, 127) == SH_EXP_OK);
    CHECK(strcmp(out, "echo 127") == 0);

    /* A negative status is reported as its unsigned byte, like every shell:
     * a signal-killed child must not print as "-11". */
    CHECK(xp("echo $?", out, sizeof out, p_build, 2, -11) == SH_EXP_OK);
    CHECK(strcmp(out, "echo 245") == 0);
}

static void test_literal_dollar(void) {
    char out[DSTSZ];

    CHECK(xp("echo $$", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo $") == 0);

    /* A trailing '$' has nothing to name, so it stays. */
    CHECK(xp("echo cost$", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo cost$") == 0);

    /* Unknown names must survive verbatim until SH6b defines them; eating
     * them here would silently corrupt any script mentioning an env var. */
    CHECK(xp("echo $PATH", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo $PATH") == 0);

    CHECK(xp("echo ${CC}", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo ${CC}") == 0);

    CHECK(xp("echo $@ $* $-", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "echo $@ $* $-") == 0);
}

static void test_overflow(void) {
    char out[DSTSZ];
    char small[8];

    /* A result that does not fit must be reported, not truncated quietly:
     * silent truncation is exactly how SH-14's argv cap turned a tcc link
     * line into "unresolved reference to '__libc_start_main'". */
    CHECK(xp("echo $1 $1 $1 $1 $1 $1", small, sizeof small,
              p_build, 2, 0) == SH_EXP_OVERFLOW);
    CHECK(small[sizeof small - 1] == '\0');

    /* Still NUL-terminated at cap 1, the degenerate case. */
    char one[1];
    CHECK(xp("echo hi", one, sizeof one, p_build, 2, 0) == SH_EXP_OVERFLOW);
    CHECK(one[0] == '\0');

    CHECK(xp("echo hi", NULL, 0, p_build, 2, 0) == SH_EXP_OVERFLOW);

    /* Just enough room is fine, and the boundary is exact. */
    const char *src = "echo $1";                 /* -> "echo kernel" = 11 */
    char exact[12];
    CHECK(xp(src, exact, sizeof exact, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(exact, "echo kernel") == 0);

    char short1[11];
    CHECK(xp(src, short1, sizeof short1, p_build, 2, 0) == SH_EXP_OVERFLOW);

    /* Overflow from the numeric paths too, not just the string copy. */
    char tiny[2];
    CHECK(xp("$?", tiny, sizeof tiny, p_build, 2, 255) == SH_EXP_OVERFLOW);
    (void)out;
}

static void test_realistic_lines(void) {
    char out[DSTSZ];

    /* Lines of the shape build.sh will actually contain. */
    CHECK(xp("run tcc -c -I/src/libc/include -o /tmp/o/$1.o /src/$1.c",
              out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out,
        "run tcc -c -I/src/libc/include -o /tmp/o/kernel.o /src/kernel.c") == 0);

    CHECK(xp("echo [selfhost] build PASS: $1 built", out, sizeof out,
              p_build, 2, 0) == SH_EXP_OK);
    CHECK(strcmp(out, "[selfhost] build PASS: kernel built") != 0); /* echo prefix */
    CHECK(strstr(out, "[selfhost] build PASS: kernel built") != NULL);

    /* A failing step's status is visible to the next line. */
    CHECK(xp("echo status=$? after=$1", out, sizeof out,
              p_build, 2, 2) == SH_EXP_OK);
    CHECK(strcmp(out, "echo status=2 after=kernel") == 0);
}

int main(void) {
    printf("test_sh_expand:\n");

    test_plain_text();
    test_positional();
    test_hash_and_status();
    test_literal_dollar();
    test_overflow();
    test_realistic_lines();

    printf("  %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
