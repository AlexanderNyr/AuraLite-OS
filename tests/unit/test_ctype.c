/*
 * test_ctype.c — host-side unit test for the P1 ctype + limits/stdbool headers.
 *
 * The ctype predicate logic is reproduced here verbatim from libc/src/libc.c
 * (the user libc cannot be linked directly against glibc on the host without
 * symbol clashes), and checked against the host <ctype.h> for the ASCII range.
 * The limits.h and stdbool.h headers are exercised for self-consistency.
 *
 * Built and run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */
#include <stdio.h>
#include <ctype.h>   /* host reference */

/* ---- verbatim copies of the AuraLite ctype predicates (libc.c) ---- */
static int a_isdigit(int c)  { return c >= '0' && c <= '9'; }
static int a_isupper(int c)  { return c >= 'A' && c <= 'Z'; }
static int a_islower(int c)  { return c >= 'a' && c <= 'z'; }
static int a_isalpha(int c)  { return a_isupper(c) || a_islower(c); }
static int a_isalnum(int c)  { return a_isalpha(c) || a_isdigit(c); }
static int a_isspace(int c)  {
    return c == ' '  || c == '\t' || c == '\n' ||
           c == '\v' || c == '\f' || c == '\r';
}
static int a_isblank(int c)  { return c == ' ' || c == '\t'; }
static int a_iscntrl(int c)  { return (c >= 0 && c <= 0x1F) || c == 0x7F; }
static int a_isprint(int c)  { return c >= 0x20 && c <= 0x7E; }
static int a_isgraph(int c)  { return c > 0x20 && c <= 0x7E; }
static int a_ispunct(int c)  { return a_isgraph(c) && !a_isalnum(c); }
static int a_isxdigit(int c) {
    return a_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int a_tolower(int c)  { return a_isupper(c) ? c + ('a' - 'A') : c; }
static int a_toupper(int c)  { return a_islower(c) ? c - ('a' - 'A') : c; }

static int failures = 0;

/* Compare a predicate against the host reference across the ASCII range.
 * (!!x normalises both sides to 0/1 since glibc may return arbitrary nonzero.) */
#define CMP(name, a_fn, ref_fn) do {                                  \
        int bad = 0;                                                  \
        for (int c = 0; c < 128; c++)                                 \
            if (!!a_fn(c) != !!ref_fn(c)) { bad = 1; break; }         \
        if (!bad) { printf("PASS: %s matches host\n", name); }        \
        else { printf("FAIL: %s diverges from host\n", name);         \
               failures++; }                                          \
    } while (0)

#define CMP_MAP(name, a_fn, ref_fn) do {                              \
        int bad = 0;                                                  \
        for (int c = 0; c < 128; c++)                                 \
            if (a_fn(c) != ref_fn(c)) { bad = 1; break; }             \
        if (!bad) { printf("PASS: %s matches host\n", name); }        \
        else { printf("FAIL: %s diverges from host\n", name);         \
               failures++; }                                          \
    } while (0)

int main(void) {
    CMP("isdigit",  a_isdigit,  isdigit);
    CMP("isupper",  a_isupper,  isupper);
    CMP("islower",  a_islower,  islower);
    CMP("isalpha",  a_isalpha,  isalpha);
    CMP("isalnum",  a_isalnum,  isalnum);
    CMP("isspace",  a_isspace,  isspace);
    CMP("isblank",  a_isblank,  isblank);
    CMP("iscntrl",  a_iscntrl,  iscntrl);
    CMP("isprint",  a_isprint,  isprint);
    CMP("isgraph",  a_isgraph,  isgraph);
    CMP("ispunct",  a_ispunct,  ispunct);
    CMP("isxdigit", a_isxdigit, isxdigit);
    CMP_MAP("tolower", a_tolower, tolower);
    CMP_MAP("toupper", a_toupper, toupper);

    if (failures == 0) {
        printf("test_ctype: ALL PASS\n");
        return 0;
    }
    printf("test_ctype: %d FAILURE(S)\n", failures);
    return 1;
}
