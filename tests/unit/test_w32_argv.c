/* tests/unit/test_w32_argv.c — WIN32_PLAN.md phase W32-6.
 *
 * Command-line splitting.
 *
 * The valuable part of this file is documented_examples(): the exact table
 * from Microsoft's published "Parsing C command-line arguments" page, used
 * as the specification.  Those five rows are the behaviour every MSVC-built
 * program depends on, and they are precisely the cases a hand-rolled
 * splitter gets wrong -- so they are asserted verbatim rather than
 * paraphrased into cases that happen to pass.
 *
 * Using the published table is also the legally relevant choice: the rules
 * come from documentation, not from disassembling anyone's CRT (D3/D4).
 */

#define AURALITE_W32_HOST_TEST 1

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "w32/src/w32_argv.c"

static int passed = 0, failed = 0, tn = 0;

#define CHECK(cond, msg) do {                                              \
        if (cond) { passed++; }                                            \
        else { failed++; printf("    FAIL L%d: %s\n", __LINE__, (msg)); }  \
    } while (0)

#define RUN(f) do { tn++; printf("  [%d] %s\n", tn, #f); f(); } while (0)

/* Split @cmd and compare against a NULL-terminated list of expected
 * arguments.  Runs the measuring pass first and asserts it agrees with the
 * real pass, so a drift between the two is caught on every single case
 * rather than needing its own test. */
static void expect(const char *cmd, const char *const *want) {
    size_t want_n = 0;
    while (want[want_n]) want_n++;

    /* Measuring pass. */
    size_t m_argc = 0, m_bytes = 0;
    int rc = w32_cmdline_to_argv(cmd, NULL, 0, NULL, 0, &m_argc, &m_bytes);
    CHECK(rc == W32_ARGV_OK, "measuring pass should succeed");
    CHECK(m_argc == want_n, "measured argc matches expectation");

    /* Real pass. */
    char *argv[32];
    char buf[512];
    size_t argc = 0, bytes = 0;
    rc = w32_cmdline_to_argv(cmd, argv, 32, buf, sizeof buf, &argc, &bytes);
    CHECK(rc == W32_ARGV_OK, "real pass should succeed");
    CHECK(argc == m_argc, "real argc equals measured argc");
    CHECK(bytes == m_bytes, "real byte count equals measured byte count");

    if (argc != want_n) {
        printf("    (got argc=%zu, want %zu, cmdline <%s>)\n",
               argc, want_n, cmd);
        return;
    }
    for (size_t i = 0; i < want_n; i++) {
        if (strcmp(argv[i], want[i]) != 0) {
            failed++;
            printf("    FAIL: <%s> argv[%zu] = <%s>, want <%s>\n",
                   cmd, i, argv[i], want[i]);
        } else {
            passed++;
        }
    }
}

/* ---------------------------------------------------------------------- */

/* Microsoft's published examples, verbatim.
 *
 * C escaping makes these hard to read, so each case carries the command line
 * as it would actually be typed in a comment.  argv[0] is "prog" throughout;
 * the documented table starts at argv[1]. */
static void documented_examples(void) {
    /* prog "a b c" d e   ->   [a b c] [d] [e] */
    expect("prog \"a b c\" d e",
           (const char *const[]){ "prog", "a b c", "d", "e", NULL });

    /* prog "ab\"c" "\\" d   ->   [ab"c] [\] [d]
     * The first shows \" surviving inside quotes; the second shows that a
     * lone "\\" is one backslash, not an escaped quote. */
    expect("prog \"ab\\\"c\" \"\\\\\" d",
           (const char *const[]){ "prog", "ab\"c", "\\", "d", NULL });

    /* prog a\\\b d"e f"g h   ->   [a\\\b] [de fg] [h]
     * Backslashes NOT before a quote are literal -- all three survive. And
     * a quoted run can sit in the middle of an argument. */
    expect("prog a\\\\\\b d\"e f\"g h",
           (const char *const[]){ "prog", "a\\\\\\b", "de fg", "h", NULL });

    /* prog a\\\"b c d   ->   [a\"b] [c] [d]
     * 3 backslashes = 2n+1 with n=1: one backslash, then a literal quote,
     * and quoting does NOT toggle -- so the space still separates. */
    expect("prog a\\\\\\\"b c d",
           (const char *const[]){ "prog", "a\\\"b", "c", "d", NULL });

    /* prog a\\\\"b c" d e   ->   [a\\b c] [d] [e]
     * 4 backslashes = 2n with n=2: two backslashes, and the quote toggles,
     * so "b c" joins the argument including its space. */
    expect("prog a\\\\\\\\\"b c\" d e",
           (const char *const[]){ "prog", "a\\\\b c", "d", "e", NULL });

    /* prog a"b"" c d   ->   [ab" c d]
     * The "" rule: inside quotes, a doubled quote is one literal quote and
     * stays inside, so everything after it is one argument. */
    expect("prog a\"b\"\" c d",
           (const char *const[]){ "prog", "ab\" c d", NULL });
}

/* argv[0] plays by different rules: backslashes in it are ALWAYS literal,
 * because it is a path.  If the general rule leaked into argv[0], a program
 * in a directory ending in a backslash would see a mangled path. */
static void argv0_is_special(void) {
    expect("C:\\dir\\prog.exe a",
           (const char *const[]){ "C:\\dir\\prog.exe", "a", NULL });

    /* Quoted, with a space: quotes group but are not kept. */
    expect("\"C:\\my dir\\prog.exe\" a",
           (const char *const[]){ "C:\\my dir\\prog.exe", "a", NULL });

    /* The distinguishing case: \" in argv[0].  Under the general rule this
     * would be a literal quote; in argv[0] the backslash is literal and the
     * quote merely toggles grouping. */
    expect("a\\\"b c\" d",
           (const char *const[]){ "a\\b c", "d", NULL });
}

static void separators_and_empties(void) {
    /* Runs of spaces and tabs collapse; they do not make empty arguments. */
    expect("prog   a\t\tb",
           (const char *const[]){ "prog", "a", "b", NULL });

    /* Leading whitespace is skipped rather than producing an empty argv[0]. */
    expect("   prog a",
           (const char *const[]){ "prog", "a", NULL });

    /* An explicitly quoted empty string IS an argument -- this is how a
     * program is handed an empty parameter, and dropping it would silently
     * shift every later argument down one. */
    expect("prog \"\" x",
           (const char *const[]){ "prog", "", "x", NULL });

    /* A completely empty command line has no arguments at all, rather than
     * one empty one. */
    size_t argc = 99, bytes = 99;
    int rc = w32_cmdline_to_argv("", NULL, 0, NULL, 0, &argc, &bytes);
    CHECK(rc == W32_ARGV_OK, "empty command line is not an error");
    CHECK(argc == 0, "empty command line yields argc 0");
    CHECK(bytes == 0, "empty command line yields no bytes");

    /* Whitespace only is the same as empty. */
    argc = 99;
    rc = w32_cmdline_to_argv("  \t ", NULL, 0, NULL, 0, &argc, &bytes);
    CHECK(rc == W32_ARGV_OK && argc == 0, "whitespace-only yields argc 0");
}

/* An unterminated quote is not an error on Windows: everything to the end of
 * the line becomes the final argument.  A parser that instead rejected it
 * would break drag-and-drop invocations that lose a trailing quote. */
static void unterminated_quote(void) {
    expect("prog \"a b",
           (const char *const[]){ "prog", "a b", NULL });
}

/* Trailing backslashes with no quote after them stay literal, including at
 * the very end of the line where there is nothing left to escape. */
static void trailing_backslashes(void) {
    expect("prog a\\\\",
           (const char *const[]){ "prog", "a\\\\", NULL });
    expect("prog C:\\dir\\",
           (const char *const[]){ "prog", "C:\\dir\\", NULL });
}

/* The buffer contract: too small must be reported, not written past, and the
 * reported size must be the size that would have worked. */
static void overflow_is_reported(void) {
    size_t need_argc = 0, need_bytes = 0;
    w32_cmdline_to_argv("prog alpha beta", NULL, 0, NULL, 0,
                        &need_argc, &need_bytes);

    char small[4];
    char *argv[8];
    size_t argc = 0, bytes = 0;

    /* A canary immediately after the buffer catches an overrun even without
     * a sanitizer, so this test is meaningful in a plain build too. */
    struct { char buf[4]; char canary; } box;
    box.canary = 0x7f;
    memset(box.buf, 0, sizeof box.buf);
    (void)small;

    int rc = w32_cmdline_to_argv("prog alpha beta", argv, 8,
                                 box.buf, sizeof box.buf, &argc, &bytes);
    CHECK(rc == W32_ARGV_ENOSPACE, "small buffer reports ENOSPACE");
    CHECK(box.canary == 0x7f, "nothing written past the buffer");
    CHECK(bytes == need_bytes, "reported size is the size that would work");
    CHECK(argc == need_argc, "argc still accurate on overflow");

    /* And the retry with that exact size must succeed -- otherwise the
     * measuring pass is off by one and every caller allocates too little. */
    char exact[64];
    CHECK(need_bytes <= sizeof exact, "test buffer is big enough");
    rc = w32_cmdline_to_argv("prog alpha beta", argv, 8,
                             exact, need_bytes, &argc, &bytes);
    CHECK(rc == W32_ARGV_OK, "retry at the reported size succeeds");
}

static void too_many_args(void) {
    char *argv[2];
    char buf[64];
    size_t argc = 0, bytes = 0;
    int rc = w32_cmdline_to_argv("prog a b c", argv, 2, buf, sizeof buf,
                                 &argc, &bytes);
    CHECK(rc == W32_ARGV_ETOOMANY, "more args than slots reports ETOOMANY");
}

static void null_arguments_refused(void) {
    size_t argc = 0, bytes = 0;
    CHECK(w32_cmdline_to_argv(NULL, NULL, 0, NULL, 0, &argc, &bytes)
              == W32_ARGV_EINVAL, "NULL cmdline refused");
    CHECK(w32_cmdline_to_argv("x", NULL, 0, NULL, 0, NULL, &bytes)
              == W32_ARGV_EINVAL, "NULL out_argc refused");
    CHECK(w32_cmdline_to_argv("x", NULL, 0, NULL, 0, &argc, NULL)
              == W32_ARGV_EINVAL, "NULL out_bytes refused");
}

/* GetCommandLineA (W32-4) builds a command line by quoting any argument that
 * contains a space.  This splitter must invert that, or a program that reads
 * its own command line and re-splits it -- which is exactly what
 * mainCRTStartup does -- would not get back what it was given. */
static void round_trip_with_getcommandline(void) {
    expect("w32run.exe \"a b\" c",
           (const char *const[]){ "w32run.exe", "a b", "c", NULL });
}

int main(void) {
    printf("== test_w32_argv (WIN32_PLAN W32-6) ==\n");
    RUN(documented_examples);
    RUN(argv0_is_special);
    RUN(separators_and_empties);
    RUN(unterminated_quote);
    RUN(trailing_backslashes);
    RUN(overflow_is_reported);
    RUN(too_many_args);
    RUN(null_arguments_refused);
    RUN(round_trip_with_getcommandline);
    printf("== %d passed, %d failed ==\n", passed, failed);
    return failed ? 1 : 0;
}
