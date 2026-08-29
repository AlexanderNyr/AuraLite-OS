/*
 * test_sh_expand.c — host-side unit test for the shell's parameter and
 * variable expander (SELFHOST SH6a positional parameters, SH6b named
 * variables and quote awareness).
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

/* Report a mismatch with both strings; a bare strcmp failure gives no clue
 * what the expander actually produced. */
#define CHECK_STR(got, want) do { \
    if (strcmp((got), (want)) == 0) { passed++; } \
    else { printf("  FAIL: %d: want \"%s\", got \"%s\"\n", \
                  __LINE__, (want), (got)); failed++; } \
} while (0)

#define DSTSZ 256

/* The common case: a script invoked as `sh build.sh kernel`. */
static char *p_build[3] = { (char *)"build.sh", (char *)"kernel", NULL };

/* Expand with no shell variables defined. */
static int xp(const char *src, char *out, size_t cap,
              char *const *pos, int npos, int last_status)
{
    return sh_expand_word(src, src ? strlen(src) : 0, out, cap,
                          NULL, 0, pos, npos, last_status);
}

/* Expand with a variable table. */
static int xv(const char *src, char *out, size_t cap,
              const struct sh_var *vars, int nvars)
{
    return sh_expand_word(src, strlen(src), out, cap,
                          vars, nvars, p_build, 2, 0);
}

static void test_plain_text(void) {
    char out[DSTSZ];

    CHECK(xp("echo hello", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo hello");

    CHECK(xp("", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(out[0] == '\0');

    /* NULL source is an empty result, not a crash. */
    CHECK(xp(NULL, out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK(out[0] == '\0');

    /* Punctuation the tokenizer cares about must survive untouched. */
    CHECK(xp("tcc -c -o /tmp/a.o /src/a.c", out, sizeof out,
             p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "tcc -c -o /tmp/a.o /src/a.c");
}

static void test_positional(void) {
    char out[DSTSZ];

    CHECK(xp("echo $0", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo build.sh");

    CHECK(xp("echo $1", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo kernel");

    /* The point of SH6a: the target reaches the script. */
    CHECK(xp("shmake $1", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "shmake kernel");

    /* An absent parameter expands to nothing, not to "$2". */
    CHECK(xp("echo [$2]", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo []");

    CHECK(xp("echo [$9]", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo []");

    /* $10 is $1 followed by a literal '0' — POSIX has no multi-digit
     * positional without braces, and braces are still out of scope. */
    CHECK(xp("echo $10", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo kernel0");

    CHECK(xp("echo $1 $0 $1", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo kernel build.sh kernel");

    /* With no script at all, $0 and $# are empty/zero rather than garbage. */
    CHECK(xp("echo [$0][$#]", out, sizeof out, NULL, 0, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo [][0]");
}

static void test_hash_and_status(void) {
    char out[DSTSZ];

    /* $# counts arguments, excluding $0. */
    CHECK(xp("echo $#", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo 1");

    char *four[5] = { (char *)"s", (char *)"a", (char *)"b", (char *)"c", NULL };
    CHECK(xp("echo $#", out, sizeof out, four, 4, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo 3");

    /* Multi-digit, so a single-digit formatter cannot pass by accident. */
    char *twelve[12];
    for (int i = 0; i < 11; i++) twelve[i] = (char *)"x";
    twelve[11] = NULL;
    CHECK(xp("echo $#", out, sizeof out, twelve, 11, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo 10");

    CHECK(xp("echo $?", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo 0");

    CHECK(xp("echo $?", out, sizeof out, p_build, 2, 1) == SH_EXP_OK);
    CHECK_STR(out, "echo 1");

    CHECK(xp("echo $?", out, sizeof out, p_build, 2, 127) == SH_EXP_OK);
    CHECK_STR(out, "echo 127");

    /* A negative status is reported as its unsigned byte, like every shell:
     * a signal-killed child must not print as "-11". */
    CHECK(xp("echo $?", out, sizeof out, p_build, 2, -11) == SH_EXP_OK);
    CHECK_STR(out, "echo 245");
}

static void test_literal_dollar(void) {
    char out[DSTSZ];

    CHECK(xp("echo $$", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo $");

    /* A trailing '$' has nothing to name, so it stays. */
    CHECK(xp("echo cost$", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo cost$");

    /* Names the shell does not define yet must survive verbatim until a later
     * phase claims them; eating them here would corrupt any script that
     * mentions one.  '{' is not a valid name start, so ${CC} is untouched. */
    CHECK(xp("echo ${CC}", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo ${CC}");

    CHECK(xp("echo $@ $* $-", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "echo $@ $* $-");
}

/* ---- SH6b: named variables ---- */

static struct sh_var v_cc   = { "CC", "tcc" };
static struct sh_var v_out  = { "OUT", "/tmp/o" };
static struct sh_var v_empt = { "EMPTY", "" };
static struct sh_var v_long = { "MSG", "[selfhost] build PASS: kernel" };
static struct sh_var v_dol  = { "DOL", "a$b" };

static void test_variables(void) {
    char out[DSTSZ];
    struct sh_var one[1] = { v_cc };
    struct sh_var five[5] = { v_cc, v_out, v_empt, v_long, v_dol };

    CHECK(xv("$CC", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "tcc");

    /* A variable used as part of a path, the build.sh shape. */
    CHECK(xv("$OUT/kernel.o", out, sizeof out, five, 5) == SH_EXP_OK);
    CHECK_STR(out, "/tmp/o/kernel.o");

    /* The name is greedy: $CCx is the variable CCx, not $CC followed by x. */
    CHECK(xv("$CCx", out, sizeof out, five, 5) == SH_EXP_OK);
    CHECK_STR(out, "");

    /* A name stops at the first non-name character. */
    CHECK(xv("$CC-c", out, sizeof out, five, 5) == SH_EXP_OK);
    CHECK_STR(out, "tcc-c");

    CHECK(xv("[$CC]", out, sizeof out, five, 5) == SH_EXP_OK);
    CHECK_STR(out, "[tcc]");

    /* Set but empty, and unset, both expand to nothing — that is what makes
     * `$VERBOSE` usable as a flag. */
    CHECK(xv("[$EMPTY]", out, sizeof out, five, 5) == SH_EXP_OK);
    CHECK_STR(out, "[]");

    CHECK(xv("[$NOPE]", out, sizeof out, five, 5) == SH_EXP_OK);
    CHECK_STR(out, "[]");

    /* A value with a space stays ONE word: expansion runs per token, so a
     * value can never inject an extra argument or an operator.  This is the
     * injection bug that expanding the whole line first would reintroduce. */
    CHECK(xv("echo $MSG", out, sizeof out, five, 5) == SH_EXP_OK);
    CHECK_STR(out, "echo [selfhost] build PASS: kernel");

    /* A value containing '$' is not re-expanded — no second pass, so a value
     * cannot smuggle in a parameter reference. */
    CHECK(xv("echo $DOL", out, sizeof out, five, 5) == SH_EXP_OK);
    CHECK_STR(out, "echo a$b");

    /* Underscores and digits are valid in names. */
    struct sh_var ud[2] = { { "MY_VAR2", "ok" }, { "_x", "u" } };
    CHECK(xv("$MY_VAR2/$_x", out, sizeof out, ud, 2) == SH_EXP_OK);
    CHECK_STR(out, "ok/u");

    /* Prefix matching must be exact, not "starts with": $C is not $CC. */
    CHECK(xv("$C", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "");

    /* An empty table behaves like no variables at all. */
    CHECK(xv("[$CC]", out, sizeof out, NULL, 0) == SH_EXP_OK);
    CHECK_STR(out, "[]");

    /* Positional and named parameters coexist on one line. */
    CHECK(xv("$1 via $CC", out, sizeof out, five, 5) == SH_EXP_OK);
    CHECK_STR(out, "kernel via tcc");

    /* An unset name expands to nothing, which is the POSIX rule and the
     * change from SH6a, where an unknown name was left verbatim.  SH6a's
     * reason for that was to avoid pre-empting this phase; now that named
     * variables exist, "unset means empty" is the useful behaviour. */
    CHECK(xp("[$PATH]", out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "[]");
}

/* ---- SH6b: quotes ---- */

static void test_quotes(void) {
    char out[DSTSZ];
    struct sh_var one[1] = { v_cc };

    /* Single quotes suppress everything. */
    CHECK(xv("'$CC'", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "$CC");

    CHECK(xv("'a\\$b'", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "a\\$b");

    /* Double quotes allow expansion. */
    CHECK(xv("\"$CC\"", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "tcc");

    /* ...and a backslash still escapes the characters that matter. */
    CHECK(xv("\"\\$CC\"", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "$CC");

    CHECK(xv("\"a\\\"b\"", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "a\"b");

    /* A backslash before an ordinary character is literal inside "...". */
    CHECK(xv("\"a\\nb\"", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "a\\nb");

    /* Outside quotes a backslash escapes the next byte, so a literal dollar
     * sign does not need the $$ special case. */
    CHECK(xv("a\\$CC", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "a$CC");

    CHECK(xv("a\\>b", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "a>b");

    /* Quotes are removed, and adjacent quoted/unquoted runs join. */
    CHECK(xv("a\"b c\"d", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "ab cd");

    CHECK(xv("\"\"x", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "x");

    /* Mixed quote styles: inside "..." a single quote is ordinary, and
     * inside '...' a double quote is. */
    CHECK(xv("\"it's\"", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "it's");

    CHECK(xv("'say \"hi\"'", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "say \"hi\"");

    /* A quoted '$' followed by a positional still expands inside "...". */
    CHECK(xp("\"$1 and $?\"", out, sizeof out, p_build, 2, 7) == SH_EXP_OK);
    CHECK_STR(out, "kernel and 7");

    /* The redirect target the integration gate writes through. */
    CHECK(xv("\"/tmp/a b\"", out, sizeof out, one, 1) == SH_EXP_OK);
    CHECK_STR(out, "/tmp/a b");
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
    CHECK_STR(exact, "echo kernel");

    char short1[11];
    CHECK(xp(src, short1, sizeof short1, p_build, 2, 0) == SH_EXP_OVERFLOW);

    /* Overflow from the numeric paths too, not just the string copy. */
    char tiny[2];
    CHECK(xp("$?", tiny, sizeof tiny, p_build, 2, 255) == SH_EXP_OVERFLOW);

    /* A long variable value that does not fit is an error, not a half word. */
    char small2[8];
    struct sh_var lv[1] = { v_long };
    CHECK(xv("$MSG", small2, sizeof small2, lv, 1) == SH_EXP_OVERFLOW);
    CHECK(small2[sizeof small2 - 1] == '\0');

    /* Quote handling must not lose the overflow report either. */
    char small3[6];
    CHECK(xv("\"$MSG\"", small3, sizeof small3, lv, 1) == SH_EXP_OVERFLOW);
    (void)out;
}

static void test_realistic_lines(void) {
    char out[DSTSZ];
    struct sh_var three[3] = { v_cc, v_out, v_long };

    CHECK(xp("tcc -c -I/src/libc/include -o /tmp/o/$1.o /src/$1.c",
             out, sizeof out, p_build, 2, 0) == SH_EXP_OK);
    CHECK_STR(out, "tcc -c -I/src/libc/include -o /tmp/o/kernel.o /src/kernel.c");

    CHECK(xv("$CC -c -o $OUT/$1.o /src/$1.c", out, sizeof out, three, 3)
          == SH_EXP_OK);
    CHECK_STR(out, "tcc -c -o /tmp/o/kernel.o /src/kernel.c");

    CHECK(xv("echo $MSG >> /fat/build.log", out, sizeof out, three, 3)
          == SH_EXP_OK);
    CHECK_STR(out, "echo [selfhost] build PASS: kernel >> /fat/build.log");

    /* A failing step's status is visible to the next line. */
    CHECK(xp("echo status=$? after=$1", out, sizeof out,
             p_build, 2, 2) == SH_EXP_OK);
    CHECK_STR(out, "echo status=2 after=kernel");
}

int main(void) {
    printf("test_sh_expand:\n");

    test_plain_text();
    test_positional();
    test_hash_and_status();
    test_literal_dollar();
    test_variables();
    test_quotes();
    test_overflow();
    test_realistic_lines();

    printf("  %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
