/*
 * libc.c — C library implementations for AuraLite OS user programs.
 *
 * Compiled with the host compiler and linked into user binaries. Provides
 * syscall wrappers, string functions, stdlib, and printf.
 */

#include <stdint.h>
#include <stdarg.h>
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* ---- Syscall wrappers ---- */

ssize_t write(int fd, const void *buf, size_t count) {
    return syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, (uint64_t)count,
                   0, 0, 0);
}

ssize_t read(int fd, void *buf, size_t count) {
    return syscall(SYS_READ, (uint64_t)fd, (uint64_t)buf, (uint64_t)count,
                   0, 0, 0);
}

int open(const char *path) {
    return (int)syscall(SYS_OPEN, (uint64_t)path, 0, 0, 0, 0, 0);
}

int close(int fd) {
    return (int)syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0);
}

void _exit(int code) {
    syscall(SYS_EXIT, (uint64_t)code, 0, 0, 0, 0, 0);
}

pid_t getpid(void) {
    return (pid_t)syscall(SYS_GETPID, 0, 0, 0, 0, 0, 0);
}

void listdir(const char *path) {
    syscall(SYS_LISTDIR, (uint64_t)path, 0, 0, 0, 0, 0);
}

uint32_t dns_resolve(const char *hostname) {
    return (uint32_t)syscall(SYS_DNS, (uint64_t)hostname, 0, 0, 0, 0, 0);
}

int net_connect(uint32_t ip, uint16_t port) {
    return (int)syscall(SYS_NET_CONNECT, ip, port, 0, 0, 0, 0);
}

int net_send(const void *data, uint32_t len) {
    return (int)syscall(SYS_NET_SEND, (uint64_t)data, len, 0, 0, 0, 0);
}

int net_recv(void *buf, uint32_t bufsize) {
    return (int)syscall(SYS_NET_RECV, (uint64_t)buf, bufsize, 0, 0, 0, 0);
}

int net_close(void) {
    return (int)syscall(SYS_NET_CLOSE, 0, 0, 0, 0, 0, 0);
}

int net_ping(uint32_t ip) {
    return (int)syscall(SYS_NET_PING, ip, 0, 0, 0, 0, 0);
}

pid_t fork(void) {
    return (pid_t)syscall(SYS_FORK, 0, 0, 0, 0, 0, 0);
}

int execve(const char *path) {
    return (int)syscall(SYS_EXECVE, (uint64_t)path, 0, 0, 0, 0, 0);
}

pid_t wait(int *status) {
    return (pid_t)syscall(SYS_WAIT4, (uint64_t)status, 0, 0, 0, 0, 0);
}

pid_t spawn(const char *path) {
    return (pid_t)syscall(SYS_SPAWN, (uint64_t)path, 0, 0, 0, 0, 0);
}

/* ---- stdlib ---- */

int atoi(const char *s) {
    int sign = 1, result = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result * sign;
}

long strtol(const char *s, char **end, int base) {
    long result = 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    }
    for (;;) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        s++;
    }
    if (end) *end = (char *)s;
    return result * sign;
}

static unsigned int rng_seed = 1;

void srand(unsigned int seed) {
    rng_seed = seed ? seed : 1;
}

int rand(void) {
    rng_seed ^= rng_seed << 13;
    rng_seed ^= rng_seed >> 17;
    rng_seed ^= rng_seed << 5;
    return (int)(rng_seed & 0x7FFFFFFF);
}

/* ---- String functions ---- */

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}

static char *strtok_save = 0;

char *strtok(char *s, const char *delim) {
    if (s) strtok_save = s;
    if (!strtok_save || !*strtok_save) return 0;
    char *p = strtok_save;
    while (*p) {
        const char *d = delim;
        while (*d) { if (*p == *d) break; d++; }
        if (!*d) break;
        p++;
    }
    if (!*p) { strtok_save = p; return 0; }
    char *tok = p;
    while (*p) {
        const char *d = delim;
        while (*d) { if (*p == *d) { *p = '\0'; strtok_save = p + 1; return tok; } d++; }
        p++;
    }
    strtok_save = p;
    return tok;
}

/* ---- stdio ---- */

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int puts(const char *s) {
    write(1, s, strlen(s));
    putchar('\n');
    return 0;
}

static void print_uint(uint64_t val, unsigned base, int upper, int width, int zero) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[32];
    int i = 0;
    if (val == 0) buf[i++] = '0';
    while (val) { buf[i++] = digits[val % base]; val /= base; }
    int pad = width - i;
    while (pad-- > 0) putchar(zero ? '0' : ' ');
    while (i--) putchar(buf[i]);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { putchar(*fmt); continue; }
        fmt++;
        int zero = 0, width = 0;
        while (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        /* Parse length modifiers (l, ll, h). */
        int is_long = 0;
        while (*fmt == 'l') { is_long++; fmt++; }
        while (*fmt == 'h') { fmt++; }
        switch (*fmt) {
        case '%': putchar('%'); break;
        case 'c': putchar(va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            write(1, s, strlen(s));
            break;
        }
        case 'd': {
            int64_t v = is_long ? va_arg(ap, int64_t)
                                : (int64_t)(int)va_arg(ap, int);
            if (v < 0) { putchar('-'); print_uint((uint64_t)(-(v+1))+1, 10, 0, width, zero); }
            else print_uint((uint64_t)v, 10, 0, width, zero);
            break;
        }
        case 'u': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)(unsigned)va_arg(ap, unsigned);
            print_uint(v, 10, 0, width, zero);
            break;
        }
        case 'x': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)(unsigned)va_arg(ap, unsigned);
            print_uint(v, 16, 0, width, zero);
            break;
        }
        case 'X': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)(unsigned)va_arg(ap, unsigned);
            print_uint(v, 16, 1, width, zero);
            break;
        }
        case '\0': fmt--; break;
        default: putchar('%'); putchar(*fmt); break;
        }
    }
    va_end(ap);
    return 0;
}
