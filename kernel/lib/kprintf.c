/* kprintf.c — minimal freestanding formatted output for the kernel. */

#include <stdint.h>
#include <stdarg.h>
#include "kernel/lib/kprintf.h"
#include "drivers/uart/uart.h"
#include "drivers/framebuffer/fb.h"

/* Output sink: each character is duplicated to every console. */
void kputchar(char c) {
    uart_putchar(c);
    fb_putchar(c);
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
            int64_t v = va_arg(ap, int64_t);
            if (v < 0) {
                kputchar('-');
                /* (uint64_t)(-(v+1)) + 1 avoids overflow on INT64_MIN. */
                print_uint((uint64_t)(-(v + 1)) + 1, 10, 0, width, zero_pad);
            } else {
                print_uint((uint64_t)v, 10, 0, width, zero_pad);
            }
            break;
        }
        case 'u':
            print_uint(va_arg(ap, uint64_t), 10, 0, width, zero_pad);
            break;
        case 'x':
            print_uint(va_arg(ap, uint64_t), 16, 0, width, zero_pad);
            break;
        case 'X':
            print_uint(va_arg(ap, uint64_t), 16, 1, width, zero_pad);
            break;
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

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}
