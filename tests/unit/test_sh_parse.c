/*
 * test_sh_parse.c — host-side unit test for the shell's quote-aware tokenizer
 * (SELFHOST SH6b).
 *
 * Includes userspace/system/init/sh_parse.h and calls the SHIPPED body.  As
 * with test_sh_expand.c there is no local copy of the logic: a re-implementation
 * would prove nothing about what the guest shell runs.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../userspace/system/init/sh_parse.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond) do { \
    if (cond) { passed++; } \
    else { printf("  FAIL: %d: %s\n", __LINE__, #cond); failed++; } \
} while (0)

#define MAXTOK 32
static struct sh_tok tk[MAXTOK];

/* Tokenize and return the count, or a negative error. */
static int tok(const char *s)
{
    return sh_tokenize(s, strlen(s), tk, MAXTOK);
}

/* Assert the nth token is a word equal to `want` (quotes still present). */
static void is_word(int n, const char *want)
{
    char got[256];
    if (n < 0 || n >= MAXTOK || tk[n].len >= sizeof got) {
        printf("  FAIL: token %d out of range\n", n);
        failed++;
        return;
    }
    memcpy(got, tk[n].text, tk[n].len);
    got[tk[n].len] = '\0';
    if (tk[n].type != SH_TOK_WORD || strcmp(got, want) != 0) {
        printf("  FAIL: token %d: want word \"%s\", got type %d \"%s\"\n",
               n, want, tk[n].type, got);
        failed++;
    } else {
        passed++;
    }
}

static void is_op(int n, int type)
{
    if (tk[n].type != type) {
        printf("  FAIL: token %d: want op %d, got %d\n", n, type, tk[n].type);
        failed++;
    } else {
        passed++;
    }
}

static void test_plain_words(void) {
    CHECK(tok("echo hello world") == 3);
    is_word(0, "echo");
    is_word(1, "hello");
    is_word(2, "world");

    CHECK(tok("") == 0);
    CHECK(tok("   ") == 0);
    CHECK(tok("\t \n") == 0);

    /* A long tcc link line: the shape that broke on MAX_ARGS 8 (ledger SH-14). */
    /* tcc, -nostdlib, -static, -o, /tmp/x, a.o, b.o == 7 */
    CHECK(tok("tcc -nostdlib -static -o /tmp/x /tmp/a.o /tmp/b.o") == 7);
    is_word(6, "/tmp/b.o");
}

static void test_quotes(void) {
    /* The whole point: a `>` inside quotes is text, not an operator. */
    CHECK(tok("echo \"a > b\"") == 2);
    is_word(0, "echo");
    is_word(1, "\"a > b\"");

    CHECK(tok("echo 'a > b'") == 2);
    is_word(1, "'a > b'");

    /* Spaces inside quotes do not split. */
    CHECK(tok("write /tmp/f \"hello world\"") == 3);
    is_word(2, "\"hello world\"");

    /* An empty quoted argument is still an argument. */
    CHECK(tok("echo \"\" x") == 3);
    is_word(1, "\"\"");
    is_word(2, "x");

    /* Quotes glued to bare text form one word. */
    CHECK(tok("echo a\"b c\"d") == 2);
    is_word(1, "a\"b c\"d");

    /* Both quote styles in one word. */
    CHECK(tok("echo \"it's\" 'say \"hi\"'") == 3);
    is_word(1, "\"it's\"");
    is_word(2, "'say \"hi\"'");

    /* A backslash escapes the next character outside quotes. */
    CHECK(tok("echo a\\>b") == 2);
    is_word(1, "a\\>b");
}

static void test_unterminated(void) {
    CHECK(tok("echo \"unterminated") == SH_PARSE_QUOTE);
    CHECK(tok("echo 'unterminated") == SH_PARSE_QUOTE);
    /* Closed is fine; a second opener is not. */
    CHECK(tok("echo \"ok\" \"nope") == SH_PARSE_QUOTE);
}

static void test_redirects(void) {
    CHECK(tok("echo hi > /tmp/o.txt") == 4);
    is_word(0, "echo");
    is_word(1, "hi");
    is_op(2, SH_TOK_GT);
    is_word(3, "/tmp/o.txt");

    CHECK(tok("echo hi >> /tmp/o.txt") == 4);
    is_op(2, SH_TOK_GGT);

    CHECK(tok("cat < /tmp/in.txt") == 3);
    is_op(1, SH_TOK_LT);
    is_word(2, "/tmp/in.txt");

    /* Two redirects on one line. */
    CHECK(tok("sort < /tmp/a > /tmp/b") == 5);
    is_op(1, SH_TOK_LT);
    is_op(3, SH_TOK_GT);

    /* No space around the operator: still three tokens, as in POSIX. */
    CHECK(tok("echo hi>/tmp/o") == 4);
    is_word(1, "hi");
    is_op(2, SH_TOK_GT);
    is_word(3, "/tmp/o");

    /* `>>>` is `>>` then `>`; the parser does not invent a third operator. */
    CHECK(tok("echo x >>> f") == 5);
    is_op(2, SH_TOK_GGT);
    is_op(3, SH_TOK_GT);

    /* A redirect with nothing after it is refused, not silently dropped. */
    CHECK(tok("echo hi >") == SH_PARSE_NOTARGET);
    CHECK(tok("echo hi >   ") == SH_PARSE_NOTARGET);
    CHECK(tok("cat <") == SH_PARSE_NOTARGET);

    /* A quoted redirect target is a filename that happens to contain
     * characters the tokenizer would otherwise treat specially. */
    CHECK(tok("echo hi > \"/tmp/a b\"") == 4);
    is_op(2, SH_TOK_GT);
    is_word(3, "\"/tmp/a b\"");
}

static void test_background(void) {
    CHECK(tok("run calc &") == 3);
    is_word(0, "run");
    is_word(1, "calc");
    is_op(2, SH_TOK_AMP);

    /* Glued form, which the old strtok path handled by stripping a trailing
     * '&'; the tokenizer must keep producing the same two things. */
    CHECK(tok("run calc&") == 3);
    is_word(1, "calc");
    is_op(2, SH_TOK_AMP);

    /* A redirect and a background marker together. */
    CHECK(tok("run calc > /tmp/log &") == 5);
    is_op(2, SH_TOK_GT);
    is_op(4, SH_TOK_AMP);
}

static void test_limits(void) {
    struct sh_tok small[3];
    /* Exactly three tokens fit. */
    CHECK(sh_tokenize("a b c", 5, small, 3) == 3);
    /* Four do not -- and the caller must be told, not silently truncated. */
    CHECK(sh_tokenize("a b c d", 7, small, 3) == SH_PARSE_TOOMANY);

    CHECK(sh_tokenize(NULL, 0, small, 3) == SH_PARSE_TOOMANY);
    CHECK(sh_tokenize("a", 1, NULL, 3) == SH_PARSE_TOOMANY);
    CHECK(sh_tokenize("a", 1, small, 0) == SH_PARSE_TOOMANY);
}

static void test_realistic_lines(void) {
    /* Lines of the shape build.sh will contain once SH6f lands. */
    CHECK(tok("tcc -c -I/src/libc/include -o /tmp/o/kernel.o /src/kernel.c > /tmp/log") == 8);
    is_op(6, SH_TOK_GT);
    is_word(7, "/tmp/log");

    CHECK(tok("echo \"[selfhost] build PASS: kernel\" >> /fat/build.log") == 4);
    is_word(1, "\"[selfhost] build PASS: kernel\"");
    is_op(2, SH_TOK_GGT);

    /* A path with a redirect character in a quoted argument must survive. */
    /* `2>` is the word `2` then an operator: fd-specific redirects are out
     * of scope (the shell has one output stream), and this shows the parse
     * is at least predictable rather than silently swallowing the `2`. */
    CHECK(tok("echo \"2 > 1\" and 2> /tmp/x") == 6);
    is_word(1, "\"2 > 1\"");
    is_word(2, "and");
    is_word(3, "2");
    is_op(4, SH_TOK_GT);
    is_word(5, "/tmp/x");
}

static void test_list_operators(void) {
    /* SH6c: | ; && || are operators, recognised in the same pass as > < &. */
    CHECK(tok("a | b") == 3);
    is_word(0, "a");
    is_op(1, SH_TOK_PIPE);
    is_word(2, "b");

    CHECK(tok("a|b") == 3);
    is_op(1, SH_TOK_PIPE);

    CHECK(tok("a | b | c") == 5);
    is_op(1, SH_TOK_PIPE);
    is_op(3, SH_TOK_PIPE);

    CHECK(tok("a ; b") == 3);
    is_op(1, SH_TOK_SEMI);
    is_word(2, "b");

    /* && is an AND, not a background &. */
    CHECK(tok("a && b") == 3);
    is_op(1, SH_TOK_ANDAND);
    is_word(2, "b");
    CHECK(tok("a&&b") == 3);
    is_op(1, SH_TOK_ANDAND);

    /* A lone & is still background. */
    CHECK(tok("a & b") == 3);
    is_op(1, SH_TOK_AMP);

    /* || is an OR, not two pipes. */
    CHECK(tok("a || b") == 3);
    is_op(1, SH_TOK_OROR);
    is_word(2, "b");
    CHECK(tok("a||b") == 3);
    is_op(1, SH_TOK_OROR);

    /* Every list operator on one line. */
    CHECK(tok("a && b | c || d ; e & f") == 11);
    is_word(0, "a");
    is_op(1, SH_TOK_ANDAND);
    is_word(2, "b");
    is_op(3, SH_TOK_PIPE);
    is_word(4, "c");
    is_op(5, SH_TOK_OROR);
    is_word(6, "d");
    is_op(7, SH_TOK_SEMI);
    is_word(8, "e");
    is_op(9, SH_TOK_AMP);
    is_word(10, "f");

    /* A `|` inside quotes is text, not a stage join. */
    CHECK(tok("echo \"a | b\"") == 2);
    is_word(0, "echo");
    is_word(1, "\"a | b\"");
    CHECK(tok("echo 'a ; b'") == 2);
    is_word(1, "'a ; b'");
    CHECK(tok("echo \"x && y\" | cat") == 4);
    is_word(1, "\"x && y\"");
    is_op(2, SH_TOK_PIPE);

    /* An operator glued to a word ends the word, as with >. */
    CHECK(tok("a|b;c") == 5);
    is_word(0, "a");
    is_op(1, SH_TOK_PIPE);
    is_word(2, "b");
    is_op(3, SH_TOK_SEMI);
    is_word(4, "c");

    /* | && || cannot end a line: no command where one is due. */
    CHECK(tok("a |") == SH_PARSE_NOCOMMAND);
    CHECK(tok("a |   ") == SH_PARSE_NOCOMMAND);
    CHECK(tok("a &&") == SH_PARSE_NOCOMMAND);
    CHECK(tok("a ||") == SH_PARSE_NOCOMMAND);
    CHECK(tok("a ||  ") == SH_PARSE_NOCOMMAND);

    /* ; and & MAY end a line. */
    CHECK(tok("a ;") == 2);
    is_op(1, SH_TOK_SEMI);
    CHECK(tok("a &") == 2);
    is_op(1, SH_TOK_AMP);
}

static void test_tok_name(void) {
    CHECK(strcmp(sh_tok_name(SH_TOK_GT), ">") == 0);
    CHECK(strcmp(sh_tok_name(SH_TOK_GGT), ">>") == 0);
    CHECK(strcmp(sh_tok_name(SH_TOK_LT), "<") == 0);
    CHECK(strcmp(sh_tok_name(SH_TOK_AMP), "&") == 0);
    CHECK(strcmp(sh_tok_name(SH_TOK_PIPE), "|") == 0);
    CHECK(strcmp(sh_tok_name(SH_TOK_SEMI), ";") == 0);
    CHECK(strcmp(sh_tok_name(SH_TOK_ANDAND), "&&") == 0);
    CHECK(strcmp(sh_tok_name(SH_TOK_OROR), "||") == 0);
    CHECK(strcmp(sh_tok_name(SH_TOK_WORD), "word") == 0);
}

int main(void) {
    printf("test_sh_parse:\n");

    test_plain_words();
    test_quotes();
    test_unterminated();
    test_redirects();
    test_background();
    test_limits();
    test_realistic_lines();
    test_list_operators();
    test_tok_name();

    printf("  %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
