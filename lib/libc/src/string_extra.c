/* libc/src/string_extra.c — дополнительные строковые функции (P10 / Q3) */

#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == 0) return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    char *token;
    if (str) *saveptr = str;
    if (!*saveptr) return NULL;

    token = *saveptr;
    while (*token && strchr(delim, *token)) token++;
    if (!*token) { *saveptr = NULL; return NULL; }

    char *end = token;
    while (*end && !strchr(delim, *end)) end++;
    if (*end) {
        *end = '\0';
        *saveptr = end + 1;
    } else {
        *saveptr = NULL;
    }
    return token;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        if (strchr(accept, *s)) return (char *)s;
        s++;
    }
    return NULL;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *d = malloc(len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = '\0';
    }
    return d;
}

int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = tolower(*s1), c2 = tolower(*s2);
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return tolower(*s1) - tolower(*s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s2) {
        int c1 = tolower((unsigned char)*s1), c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++; s2++; n--;
    }
    if (n == 0) return 0;
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

/* ---- POSIX.1-2024 extended string/memory functions (Phase Q3) ---- */

void *memccpy(void *dst, const void *src, int c, size_t n) {
    const unsigned char *s = src;
    unsigned char *d = dst, uc = (unsigned char)c;
    while (n--) {
        if ((*d++ = *s++) == uc) return d;
    }
    return NULL;
}

void *memmem(const void *h, size_t hl, const void *n, size_t nl) {
    if (nl == 0) return (void *)h;
    if (hl < nl) return NULL;
    const char *hay = h, *ndl = n;
    for (size_t i = 0; i <= hl - nl; i++)
        if (memcmp(hay + i, ndl, nl) == 0) return (void *)(hay + i);
    return NULL;
}

char *stpcpy(char *dst, const char *src) {
    while ((*dst = *src)) { dst++; src++; }
    return dst;
}

char *stpncpy(char *dst, const char *src, size_t n) {
    while (n && (*dst = *src)) { dst++; src++; n--; }
    if (n) {
        char *p = dst;
        while (--n) *p++ = '\0';
    }
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t dsize) {
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

size_t strlcat(char *dst, const char *src, size_t dsize) {
    size_t dl = strnlen(dst, dsize);
    if (dl == dsize) return dl + strlen(src);
    return dl + strlcpy(dst + dl, src, dsize - dl);
}

int strverscmp(const char *a, const char *b) {
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

char *strsignal(int sig) {
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
    if (sig > 0 && sig < 32 && names[sig]) return (char *)names[sig];
    snprintf(buf, sizeof(buf), "Unknown signal %d", sig);
    return buf;
}
/* memmove — overlap-safe copy.
 *
 * A genuine gap until now: <string.h> declared memcpy() but not memmove(),
 * and bcopy() in compat.c carried a comment saying so and open-coded the
 * copy itself.  DOOM needs it (the renderer and the zone allocator both
 * shuffle overlapping regions), and so does any code that moves data within
 * one buffer, so it belongs here rather than in the port.
 *
 * The direction test is the whole function: copying forwards when the
 * destination overlaps the tail of the source overwrites bytes that have
 * not been read yet.
 */
void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) return dst;

    if (d < s) {
        /* Forward: overlap-safe in this direction.  8-byte chunks via
         * __builtin_memcpy (one mov each), NOT a call to memcpy() — this
         * function is extracted standalone by tools/extract_libc_impls.py
         * for the host stdio tests, where an unresolved memcpy symbol
         * would be a build error (OPT_PLAN O1). */
        size_t i = 0;
        while (i + 8 <= n) {
            uint64_t w;
            __builtin_memcpy(&w, s + i, 8);
            __builtin_memcpy(d + i, &w, 8);
            i += 8;
        }
        for (; i < n; i++) d[i] = s[i];
        return dst;
    }
    /* Backwards, so the overlapping tail is read before it is clobbered.
     * 8-byte tail-first chunks (OPT_PLAN O1), then the byte remainder;
     * see kernel/arch/x86_64/string_fast.c for why not std/rep. */
    while (n >= 8) {
        n -= 8;
        uint64_t w;
        __builtin_memcpy(&w, s + n, 8);
        __builtin_memcpy(d + n, &w, 8);
    }
    while (n--) d[n] = s[n];
    return dst;
}
