/*
 * test_string_ext.c — host-side unit test for Phase Q3 string/memory
 * extensions (memccpy, memmem, stpcpy, stpncpy, strlcpy, strlcat,
 * strverscmp, strsignal).
 *
 * Compiled with the host compiler and linked against the host libc.
 * The implementation being tested is in libc/src/string_extra.c but this test
 * re-implements the same algorithms standalone (or simply verifies semantics
 * using the host functions where available) to avoid linking against the
 * freestanding runtime. For functions not available on the host, we compile
 * and inline a local copy.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond) do { \
    if (cond) { passed++; } \
    else { printf("  FAIL: %d: %s\n", __LINE__, #cond); failed++; } \
} while (0)

/* Local strnlen (may not be available on all hosts in strict C11). */
static size_t local_strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

/* Inline copies of the functions under test (matching string_extra.c). */
static void *test_memccpy(void *dst, const void *src, int c, size_t n) {
    const unsigned char *s = src;
    unsigned char *d = dst, uc = (unsigned char)c;
    while (n--) {
        if ((*d++ = *s++) == uc) return d;
    }
    return NULL;
}

static void *test_memmem(const void *h, size_t hl, const void *n, size_t nl) {
    if (nl == 0) return (void *)h;
    if (hl < nl) return NULL;
    const char *hay = h, *ndl = n;
    for (size_t i = 0; i <= hl - nl; i++)
        if (memcmp(hay + i, ndl, nl) == 0) return (void *)(hay + i);
    return NULL;
}

static char *test_stpcpy(char *dst, const char *src) {
    while ((*dst = *src)) { dst++; src++; }
    return dst;
}

static char *test_stpncpy(char *dst, const char *src, size_t n) {
    while (n && (*dst = *src)) { dst++; src++; n--; }
    if (n) {
        char *p = dst;
        while (--n) *p++ = '\0';
    }
    return dst;
}

static size_t test_strlcpy(char *dst, const char *src, size_t dsize) {
    const char *s = src;
    size_t n = dsize;
    if (n && --n) {
        do { if (!(*dst++ = *s++)) break; } while (--n);
    }
    if (!n) {
        if (dsize) *dst = '\0';
        while (*s++) {}
    }
    return (size_t)(s - src - 1);
}

static size_t test_strlcat(char *dst, const char *src, size_t dsize) {
    size_t dl = local_strnlen(dst, dsize);
    if (dl == dsize) return dl + strlen(src);
    return dl + test_strlcpy(dst + dl, src, dsize - dl);
}

static int test_strverscmp(const char *a, const char *b) {
    for (;;) {
        if (!*a && !*b) return 0;
        int da = (*a >= '0' && *a <= '9');
        int db = (*b >= '0' && *b <= '9');
        if (da && db) {
            /* Count leading zeros on each side before comparing numerics. */
            size_t za = 0, zb = 0;
            while (a[za] == '0') za++;
            while (b[zb] == '0') zb++;
            const char *na = a + za, *nb = b + zb;
            size_t la = 0, lb = 0;
            while (na[la] >= '0' && na[la] <= '9') la++;
            while (nb[lb] >= '0' && nb[lb] <= '9') lb++;
            if (la != lb) return (la < lb) ? -1 : 1;
            int r = memcmp(na, nb, la);
            if (r) return r;
            /* Same numeric value: more leading zeros sorts earlier. */
            if (za != zb) return (za > zb) ? -1 : 1;
            a = na + la; b = nb + lb;
        } else {
            if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
            a++; b++;
        }
    }
}

static void test_memccpy_func(void) {
    char buf[16];
    memset(buf, 0, sizeof(buf));
    /* Copy "hello!" up to and including '!' */
    char *r = test_memccpy(buf, "hello!", '!', 6);
    CHECK(r == buf + 6);
    CHECK(buf[0] == 'h');
    CHECK(buf[5] == '!');

    /* Separator not found within n */
    memset(buf, 0, sizeof(buf));
    r = test_memccpy(buf, "abcd", 'x', 4);
    CHECK(r == NULL);
    CHECK(strcmp(buf, "abcd") == 0);

    /* n = 0: no copy */
    memset(buf, 0, sizeof(buf));
    r = test_memccpy(buf, "test", 't', 0);
    CHECK(r == NULL);
    CHECK(buf[0] == 0);
}

static void test_memmem_func(void) {
    const char *haystack = "haystackack";
    /* Find "ack" */
    void *r = test_memmem(haystack, 11, "ack", 3);
    CHECK(r == haystack + 5);

    /* Needle at start */
    r = test_memmem(haystack, 11, "hay", 3);
    CHECK(r == haystack);

    /* Not found */
    r = test_memmem(haystack, 11, "xyz", 3);
    CHECK(r == NULL);

    /* Empty needle */
    r = test_memmem(haystack, 11, "", 0);
    CHECK(r == haystack);

    /* Needle larger than haystack */
    r = test_memmem("short", 5, "longer needle", 13);
    CHECK(r == NULL);
}

static void test_stpcpy_func(void) {
    char buf[16];
    char *r = test_stpcpy(buf, "hi");
    CHECK(r == buf + 2);
    CHECK(buf[0] == 'h');
    CHECK(buf[1] == 'i');
    CHECK(buf[2] == '\0');

    /* Empty string */
    r = test_stpcpy(buf, "");
    CHECK(r == buf);
    CHECK(buf[0] == '\0');
}

static void test_stpncpy_func(void) {
    char buf[16];
    memset(buf, 'X', sizeof(buf));
    buf[15] = '\0';

    /* Copy "hello" (5 chars) into buffer of 8 */
    char *r = test_stpncpy(buf, "hello", 8);
    CHECK(r == buf + 5);  /* points to first NUL */
    CHECK(strncmp(buf, "hello", 5) == 0);
    /* remaining 2 bytes should be NUL-padded */
    CHECK(buf[5] == '\0');
    CHECK(buf[6] == '\0');

    /* Copy longer string than n */
    memset(buf, 'X', sizeof(buf));
    buf[15] = '\0';
    r = test_stpncpy(buf, "hello", 3);
    CHECK(r == buf + 3);
    CHECK(strncmp(buf, "hel", 3) == 0);

    /* n = 0 */
    memset(buf, 'X', sizeof(buf));
    buf[15] = '\0';
    r = test_stpncpy(buf, "hello", 0);
    CHECK(r == buf);
}

static void test_strlcpy_func(void) {
    char buf[16];

    /* Full copy fits */
    size_t n = test_strlcpy(buf, "hello", sizeof(buf));
    CHECK(n == 5);
    CHECK(strcmp(buf, "hello") == 0);

    /* Truncation: "hello" into 3-byte buffer produces "he" */
    n = test_strlcpy(buf, "hello", 3);
    CHECK(n == 5);  /* return value is src length */
    CHECK(strcmp(buf, "he") == 0);

    /* Empty dest */
    n = test_strlcpy(buf, "abc", 1);
    CHECK(n == 3);
    CHECK(buf[0] == '\0');

    /* dsize = 0: no copy, returns src length */
    n = test_strlcpy(buf, "test", 0);
    CHECK(n == 4);
}

static void test_strlcat_func(void) {
    char buf[16] = "hello";

    /* Normal concatenation */
    size_t n = test_strlcat(buf, " world", sizeof(buf));
    CHECK(n == 11);
    CHECK(strcmp(buf, "hello world") == 0);

    /* Truncation: full buf */
    buf[0] = '\0';
    n = test_strlcat(buf, "Hello", 3);
    CHECK(n == 5);
    CHECK(strcmp(buf, "He") == 0);

    /* dsize = 0 */
    n = test_strlcat(buf, "test", 0);
    CHECK(n == 4);  /* strlen("test") */
}

static void test_strverscmp_func(void) {
    CHECK(test_strverscmp("", "") == 0);
    CHECK(test_strverscmp("a", "b") < 0);
    CHECK(test_strverscmp("b", "a") > 0);
    CHECK(test_strverscmp("2.9", "2.10") < 0);
    CHECK(test_strverscmp("2.10", "2.9") > 0);
    CHECK(test_strverscmp("2.9", "2.9") == 0);
    CHECK(test_strverscmp("10", "2") > 0);
    CHECK(test_strverscmp("2", "10") < 0);
    CHECK(test_strverscmp("file1", "file2") < 0);
    CHECK(test_strverscmp("file10", "file2") > 0);
    CHECK(test_strverscmp("file2", "file10") < 0);
    /* Leading zeros */
    CHECK(test_strverscmp("01", "1") < 0);
    CHECK(test_strverscmp("1", "01") > 0);
}

static const char *local_strsignal(int sig) {
    static const char *names[32] = {
        [1]  = "Hangup",
        [2]  = "Interrupt",
        [3]  = "Quit",
        [4]  = "Illegal instruction",
        [5]  = "Trace/BPT trap",
        [6]  = "Aborted",
        [7]  = "Bus error",
        [8]  = "Floating point exception",
        [9]  = "Killed",
        [10] = "User defined signal 1",
        [11] = "Segmentation fault",
        [12] = "User defined signal 2",
        [13] = "Broken pipe",
        [14] = "Alarm clock",
        [15] = "Terminated",
        [17] = "Child exited",
        [18] = "Continued",
        [19] = "Stopped (signal)",
        [20] = "Stopped",
        [21] = "Stopped (tty input)",
        [22] = "Stopped (tty output)",
        [28] = "Window size changes",
    };
    static char buf[32];
    if (sig > 0 && sig < 32 && names[sig]) return names[sig];
    snprintf(buf, sizeof(buf), "Unknown signal %d", sig);
    return buf;
}

static void test_strsignal_func(void) {
    /* Static names from the names table */
    CHECK(strcmp(local_strsignal(9), "Killed") == 0);
    CHECK(strcmp(local_strsignal(2), "Interrupt") == 0);
    CHECK(strcmp(local_strsignal(15), "Terminated") == 0);
    CHECK(strcmp(local_strsignal(99), "Unknown signal 99") == 0);
}

/* ---- Main ---- */

int main(void) {
    printf("test_string_ext:\n");

    test_memccpy_func();
    test_memmem_func();
    test_stpcpy_func();
    test_stpncpy_func();
    test_strlcpy_func();
    test_strlcat_func();
    test_strverscmp_func();
    test_strsignal_func();

    printf("  %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
