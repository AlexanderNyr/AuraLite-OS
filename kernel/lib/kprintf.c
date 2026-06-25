/* kprintf.c — minimal freestanding formatted output for the kernel. */

#include <stdint.h>
#include <stdarg.h>
#include "kernel/lib/kprintf.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/klog.h"
#include "drivers/uart/uart.h"
#include "drivers/framebuffer/fb.h"

/* SMP-safe output: a spinlock ensures only one CPU prints at a time. cli/sti
 * alone is not sufficient under SMP (it's per-CPU). */
static spinlock_t print_lock;

/* Output sink: each character is duplicated to every console. */
void kputchar(char c) {
    uart_putchar(c);
    fb_putchar(c);
    klog_putchar(c);
}

void kputs(const char *s) {
    while (*s) {
        kputchar(*s++);
    }
}

static void print_str(const char *s) {
    if (s == NULL) {
        s = "(null)";
    }
    while (*s) {
        kputchar(*s++);
    }
}

/*
 * Print an unsigned integer in `base`, optionally left-padded to `width`.
 * When `zero_pad` is set the pad character is '0', otherwise ' '.
 */
static void print_uint(uint64_t value, unsigned base, int upper,
                       int width, int zero_pad) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[32];
    int i = 0;

    if (value == 0) {
        buf[i++] = '0';
    }
    while (value != 0 && i < (int)sizeof(buf)) {
        buf[i++] = digits[value % base];
        value /= base;
    }
    int pad = width - i;
    char padc = zero_pad ? '0' : ' ';
    while (pad-- > 0) {
        kputchar(padc);
    }
    while (i-- > 0) {
        kputchar(buf[i]);
    }
}

static void kvprintf(const char *fmt, va_list ap) {
    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            kputchar(*fmt);
            continue;
        }
        fmt++;

        /* Flags: '-' (left align, accepted) and '0' (zero pad). */
        int zero_pad = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '0') {
                zero_pad = 1;
            }
            fmt++;
        }

        /* Minimum field width. */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Precision (accepted and ignored). */
        if (*fmt == '.') {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') {
                fmt++;
            }
        }

        /* Length modifiers (integers are read as 64-bit regardless). */
        int is_long = 0;
        while (*fmt == 'l') {
            is_long++;
            fmt++;
        }
        if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') {
            is_long = 2;
            fmt++;
        }
        while (*fmt == 'h') {
            fmt++;
        }
        (void)is_long;

        switch (*fmt) {
        case '%':
            kputchar('%');
            break;
        case 'c':
            kputchar((char)va_arg(ap, int));
            break;
        case 's':
            print_str(va_arg(ap, const char *));
            break;
        case 'd': {
            int64_t v;
            if (is_long >= 2) {
                v = va_arg(ap, int64_t);
            } else if (is_long == 1) {
                v = va_arg(ap, long);
            } else {
                v = va_arg(ap, int);
            }
            if (v < 0) {
                kputchar('-');
                print_uint((uint64_t)(-(v + 1)) + 1, 10, 0, width, zero_pad);
            } else {
                print_uint((uint64_t)v, 10, 0, width, zero_pad);
            }
            break;
        }
        case 'u': {
            uint64_t v = (is_long >= 1) ? va_arg(ap, uint64_t)
                                        : (uint64_t)va_arg(ap, unsigned int);
            print_uint(v, 10, 0, width, zero_pad);
            break;
        }
        case 'x': {
            uint64_t v = (is_long >= 1) ? va_arg(ap, uint64_t)
                                        : (uint64_t)va_arg(ap, unsigned int);
            print_uint(v, 16, 0, width, zero_pad);
            break;
        }
        case 'X': {
            uint64_t v = (is_long >= 1) ? va_arg(ap, uint64_t)
                                        : (uint64_t)va_arg(ap, unsigned int);
            print_uint(v, 16, 1, width, zero_pad);
            break;
        }
        case 'p':
            kputchar('0');
            kputchar('x');
            print_uint((uint64_t)va_arg(ap, void *), 16, 0, 16, 1);
            break;
        case '\0':
            fmt--;            /* trailing % with no conversion */
            break;
        default:              /* unknown: emit verbatim */
            kputchar('%');
            kputchar(*fmt);
            break;
        }
    }
}

/*
 * Atomic output: disable interrupts for the duration of the formatted print
 * so that preempted threads cannot interleave characters on the console.
 * If interrupts were already off (e.g. inside an IRQ handler), they stay off.
 */
void kprintf(const char *fmt, ...) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
    spinlock_acquire(&print_lock);
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    spinlock_release(&print_lock);
    if (rflags & 0x200ULL) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

/* Simple snprintf for kernel strings. */
static void buf_putchar(char **buf, size_t *size, char c) {
    if (*size > 1) {
        *(*buf)++ = c;
        (*size)--;
    }
}
static void buf_puts(char **buf, size_t *size, const char *s) {
    while (*s) {
        buf_putchar(buf, size, *s++);
    }
}
static void buf_print_uint(char **buf, size_t *size, uint64_t value, unsigned base, int width, int zero_pad) {
    const char *digits = "0123456789abcdef";
    char tmp[32];
    int i = 0;
    if (value == 0) tmp[i++] = '0';
    while (value != 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = digits[value % base];
        value /= base;
    }
    int pad = width - i;
    char padc = zero_pad ? '0' : ' ';
    while (pad-- > 0) buf_putchar(buf, size, padc);
    while (i-- > 0) buf_putchar(buf, size, tmp[i]);
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    if (!buf || size == 0) return 0;
    char *orig_buf = buf;
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            buf_putchar(&buf, &size, *fmt);
            continue;
        }
        fmt++;
        int zero_pad = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '0') zero_pad = 1;
            fmt++;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == '.') {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') fmt++;
        }
        int is_long = 0;
        while (*fmt == 'l') { is_long++; fmt++; }
        if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { is_long = 2; fmt++; }
        while (*fmt == 'h') fmt++;

        switch (*fmt) {
        case '%': buf_putchar(&buf, &size, '%'); break;
        case 'c': buf_putchar(&buf, &size, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            buf_puts(&buf, &size, s ? s : "(null)");
            break;
        }
        case 'd': {
            int64_t v = (is_long >= 2) ? va_arg(ap, int64_t) : (is_long == 1) ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0) {
                buf_putchar(&buf, &size, '-');
                buf_print_uint(&buf, &size, (uint64_t)(-(v + 1)) + 1, 10, width, zero_pad);
            } else {
                buf_print_uint(&buf, &size, (uint64_t)v, 10, width, zero_pad);
            }
            break;
        }
        case 'u': {
            uint64_t v = (is_long >= 1) ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
            buf_print_uint(&buf, &size, v, 10, width, zero_pad);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t v = (is_long >= 1) ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
            buf_print_uint(&buf, &size, v, 16, width, zero_pad);
            break;
        }
        case 'p':
            buf_puts(&buf, &size, "0x");
            buf_print_uint(&buf, &size, (uint64_t)va_arg(ap, void *), 16, 16, 1);
            break;
        case '\0': fmt--; break;
        default: buf_putchar(&buf, &size, '%'); buf_putchar(&buf, &size, *fmt); break;
        }
    }
    va_end(ap);
    if (size > 0) *buf = '\0';
    return (int)(buf - orig_buf);
}

