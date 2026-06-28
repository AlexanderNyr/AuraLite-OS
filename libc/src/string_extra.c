/* libc/src/string_extra.c — дополнительные строковые функции (P10) */

#include <string.h>
#include <ctype.h>

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