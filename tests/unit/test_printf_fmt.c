/*
 * test_printf_fmt.c — conversion-specifier tests for AuraLite's printf.
 *
 * WHY THIS TEST EXISTS
 *
 * The '-' flag was not parsed at all. A format like "%-12s" fell through the
 * specifier parser and was printed LITERALLY, so every column-aligned table
 * in every program came out as `%-12s %-8s %-12s`. That shipped, unnoticed,
 * because nothing tested formatting against expected output — `apm`'s package
 * listing is what finally surfaced it, and only because someone read it.
 *
 * The real vsnprintf() from libc/src/libc.c is compiled in and its output
 * compared byte for byte, so a specifier that is silently ignored fails here
 * rather than being read in a log by a person.
 */

#include <stdio.h>
#include <string.h>

/* The unit under test is LINKED IN, not #included.
 *
 * libc.c declares `extern int main(int, char**, char**)` for its
 * __libc_start_main, which collides with this file's own main() if the source
 * is textually included. It is compiled separately and linked instead; the
 * Makefile rule supplies the stubs its freestanding half expects.
 *
 * The name is aliased because the host's <stdio.h> declares snprintf() too,
 * and comparing AuraLite's output against the host's would test nothing. */
/* AuraLite's snprintf, reached under a different name so the host's
 * declaration in <stdio.h> cannot shadow it. */
extern int snprintf_auralite(char *buf, unsigned long size, const char *fmt, ...)
    __asm__("snprintf");
#define aura_snprintf snprintf_auralite

/* libc.c references two symbols that live in assembly files (crt/sigreturn.asm
 * and stdlib_extra.c).  Neither is reachable from snprintf; they exist here
 * only so the object links. */
void __sigreturn(void);
void __run_atexit(void);
void __sigreturn(void) { }
void __run_atexit(void) { }

static int tn = 0, passed = 0, failed = 0;

static void check(const char *what, const char *got, const char *want) {
    tn++;
    if (strcmp(got, want) == 0) {
        passed++;
    } else {
        failed++;
        printf("  FAIL: %s\n        got  \"%s\"\n        want \"%s\"\n",
               what, got, want);
    }
}

#define FMT(want, ...) do {                        \
    char b[128];                                   \
    aura_snprintf(b, sizeof(b), __VA_ARGS__);      \
    check(#__VA_ARGS__, b, want);                  \
} while (0)

int main(void) {
    printf("test_printf_fmt: conversion specifiers\n");

    /* --- plain --- */
    FMT("hello",        "%s", "hello");
    FMT("42",           "%d", 42);
    FMT("-42",          "%d", -42);
    FMT("2a",           "%x", 42);
    FMT("2A",           "%X", 42);
    FMT("%",            "%%");
    FMT("c",            "%c", 'c');

    /* --- right-justified (this always worked) --- */
    FMT("     hello",   "%10s", "hello");
    FMT("        42",   "%10d", 42);
    FMT("0000000042",   "%010d", 42);

    /* --- THE regression: left-justified --- */
    FMT("hello     ",   "%-10s", "hello");
    FMT("42        ",   "%-10d", 42);
    FMT("x         ",   "%-10s", "x");
    FMT("-42       ",   "%-10d", -42);

    /* POSIX: '0' is ignored when '-' is present. */
    FMT("42        ",   "%-010d", 42);

    /* A width shorter than the value must not truncate it. */
    FMT("hello",        "%-3s", "hello");
    FMT("hello",        "%3s", "hello");
    FMT("12345",        "%-3d", 12345);

    /* Exact-width: no padding either side. */
    FMT("hello",        "%-5s", "hello");
    FMT("hello",        "%5s", "hello");

    /* --- the table apm actually prints --- */
    FMT("  matrix       1.0      [available]  rain",
        "  %-12s %-8s %-12s %s", "matrix", "1.0", "[available]", "rain");

    /* --- dynamic width / precision: tcc's diagnostic path --- */
    FMT("   abc",       "%*s", 6, "abc");
    FMT("abc   ",       "%*s", -6, "abc");  /* negative width => '-' */
    FMT("    42",       "%*d", 6, 42);
    FMT("abc",          "%.3s", "abcdef");
    FMT("abc",          "%.*s", 3, "abcdef");
    FMT("abcdef",       "%.*s", -1, "abcdef"); /* negative => omitted */
    FMT("   abc",       "%*.*s", 6, 3, "abcdef");
    FMT("007",          "%.*d", 3, 7);

    /* --- widths on other conversions --- */
    FMT("2a        ",   "%-10x", 42);
    FMT("        2a",   "%10x", 42);

    /* --- edge cases --- */
    FMT("(null)",       "%s", (char *)0);
    FMT("",             "%s", "");
    FMT("          ",   "%-10s", "");
    FMT("0",            "%d", 0);
    FMT("0         ",   "%-10d", 0);

    printf("  %d/%d passed, %d failed\n", passed, tn, failed);
    return failed == 0 ? 0 : 1;
}
