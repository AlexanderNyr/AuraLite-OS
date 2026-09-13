/* w32/tests/w32a2_common.h — shared bits for the W32A-2 guest fixtures.
 *
 * W32APP_PLAN.md phase W32A-2.  Each fixture is one mingw-w64 TU linked
 * -nostdlib with --entry=winstart (no CRT, no main): it asserts REAL
 * behaviour plus every refusal by name, prints markers, and exits 55 on
 * success, 1 on failure.  tests/integration/cases/test_w32a2_kernel32.sh
 * runs all seven under QEMU and greps the markers.
 *
 * Everything here is non-static on purpose: each fixture is a single TU,
 * so globals are defined once, and -Wall never flags an unused global
 * (a subset some file does not need would warn as an unused static).
 */
#include <windows.h>
#include <stdint.h>

/* The compiler may emit calls to these for struct copies and fixed
 * buffers even under -nostdlib; the volatile pointers keep it from
 * recognising its own loops as calls back into these same functions. */
void *memset(void *d, int c, unsigned long long n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    while (n-- > 0)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned long long n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    volatile const unsigned char *q = (volatile const unsigned char *)s;
    while (n-- > 0)
        *p++ = *q++;
    return d;
}

int memcmp(const void *a, const void *b, unsigned long long n) {
    volatile const unsigned char *p = (volatile const unsigned char *)a;
    volatile const unsigned char *q = (volatile const unsigned char *)b;
    while (n-- > 0) {
        if (*p != *q)
            return (int)*p - (int)*q;
        p++;
        q++;
    }
    return 0;
}

HANDLE w32a2_out;
DWORD w32a2_written;
int w32a2_fails;

void say(const char *s) {
    DWORD n = 0;
    while (s[n])
        n++;
    WriteFile(w32a2_out, s, n, &w32a2_written, NULL);
}

void mark_fail(const char *mark) {
    say("FAIL-");
    say(mark);
    say("\r\n");
    w32a2_fails++;
}

void w32a2_done(const char *group) {
    if (w32a2_fails == 0) {
        say("W32A2-");
        say(group);
        say("-OK\r\n");
        ExitProcess(55);
    }
    say("W32A2-");
    say(group);
    say("-FAIL\r\n");
    ExitProcess(1);
}

/* Narrow-expected compare: every WCHAR must equal the ASCII byte. */
int weq(const WCHAR *w, const char *a) {
    DWORD i = 0;
    for (;; i++) {
        if (w[i] != (WCHAR)(unsigned char)a[i])
            return 0;
        if (a[i] == 0)
            return 1;
    }
}

int wsubstr(const WCHAR *hay, const WCHAR *needle) {
    DWORD i, j;
    if (needle[0] == 0)
        return 1;
    for (i = 0; hay[i] != 0; i++) {
        for (j = 0; needle[j] != 0 && hay[i + j] == needle[j]; j++)
            ;
        if (needle[j] == 0)
            return 1;
    }
    return 0;
}

/* dir (backslash-terminated or not) + leaf -> out. */
void wjoin(const WCHAR *dir, const WCHAR *leaf, WCHAR *out) {
    DWORD i = 0, j = 0;
    while (dir[i] != 0) {
        out[i] = dir[i];
        i++;
    }
    if (i > 0 && out[i - 1] != L'\\')
        out[i++] = L'\\';
    while (leaf[j] != 0)
        out[i++] = leaf[j++];
    out[i] = 0;
}

#define CHECKX(cond, mark) do { if (!(cond)) mark_fail(mark); } while (0)
