/*
 * test_kprintf.c — unit tests for format string output.
 *
 * Tests the user-space printf format parsing by capturing output into a
 * buffer. Validates %s %d %u %x %c %% with various values, widths, and
 * padding.
 *
 * 35+ test cases.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static int passed = 0;
static int failed = 0;
static int test_num = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    int before = failed; name(); test_num++; \
    if (failed == before) passed++; \
} while(0)

#define CHECK(cond) do { \
    if (!(cond)) { printf("  FAIL: %d: %s\n", __LINE__, #cond); failed++; } \
} while(0)
#define CHECK_STREQ(actual, expected) do { \
    if (strcmp(actual, expected) != 0) { \
        printf("  FAIL: %d: got \"%s\", expected \"%s\"\n", \
               __LINE__, actual, expected); failed++; \
    } \
} while(0)

/* Minimal printf implementation (copied from libc/src/libc.c for standalone
 * testing — captures output into a buffer). */
static char capture_buf[256];
static int capture_pos;

static void cap_putchar(int c) {
    if (capture_pos < (int)sizeof(capture_buf) - 1)
        capture_buf[capture_pos++] = (char)c;
}

static void cap_print_uint(uint64_t val, unsigned base, int upper,
                           int width, int zero) {
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[32]; int i = 0;
    if (val == 0) buf[i++] = '0';
    while (val) { buf[i++] = d[val % base]; val /= base; }
    int pad = width - i;
    while (pad-- > 0) cap_putchar(zero ? '0' : ' ');
    while (i--) cap_putchar(buf[i]);
}

static void cap_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    capture_pos = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') { cap_putchar(*fmt); continue; }
        fmt++;
        int zero = 0, width = 0;
        while (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        switch (*fmt) {
        case '%': cap_putchar('%'); break;
        case 'c': cap_putchar(va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            while (*s) cap_putchar(*s++);
            break;
        }
        case 'd': {
            int64_t v = va_arg(ap, int);
            if (v < 0) { cap_putchar('-'); cap_print_uint((uint64_t)(-(v+1))+1, 10, 0, width, zero); }
            else cap_print_uint((uint64_t)v, 10, 0, width, zero);
            break;
        }
        case 'u': cap_print_uint(va_arg(ap, uint32_t), 10, 0, width, zero); break;
        case 'x': cap_print_uint(va_arg(ap, uint32_t), 16, 0, width, zero); break;
        case 'X': cap_print_uint(va_arg(ap, uint32_t), 16, 1, width, zero); break;
        case '\0': fmt--; break;
        default: cap_putchar('%'); cap_putchar(*fmt); break;
        }
    }
    capture_buf[capture_pos] = 0;
    va_end(ap);
}

#define PRINTF_AND_CHECK(fmt, expected, ...) \
    cap_printf(fmt, ##__VA_ARGS__); \
    CHECK_STREQ(capture_buf, expected)

/* ---- %s tests ---- */
TEST(test_s_basic) { PRINTF_AND_CHECK("hello", "hello"); }
TEST(test_s_arg) { PRINTF_AND_CHECK("%s", "world", "world"); }
TEST(test_s_empty) { PRINTF_AND_CHECK("[%s]", "[]", ""); }
TEST(test_s_null) { PRINTF_AND_CHECK("%s", "(null)", (char*)0); }
TEST(test_s_embedded) { PRINTF_AND_CHECK("a%sb", "aXb", "X"); }

/* ---- %d tests ---- */
TEST(test_d_zero) { PRINTF_AND_CHECK("%d", "0", 0); }
TEST(test_d_pos) { PRINTF_AND_CHECK("%d", "42", 42); }
TEST(test_d_neg) { PRINTF_AND_CHECK("%d", "-7", -7); }
TEST(test_d_max) { PRINTF_AND_CHECK("%d", "2147483647", 2147483647); }
TEST(test_d_min) { PRINTF_AND_CHECK("%d", "-2147483648", -2147483648); }
TEST(test_d_width) { PRINTF_AND_CHECK("%5d", "   42", 42); }
TEST(test_d_zero_pad) { PRINTF_AND_CHECK("%05d", "00042", 42); }
TEST(test_d_neg_width) { PRINTF_AND_CHECK("%5d", "-    7", -7); }

/* ---- %u tests ---- */
TEST(test_u_zero) { PRINTF_AND_CHECK("%u", "0", 0u); }
TEST(test_u_pos) { PRINTF_AND_CHECK("%u", "12345", 12345u); }
TEST(test_u_max) { PRINTF_AND_CHECK("%u", "4294967295", 4294967295u); }
TEST(test_u_width) { PRINTF_AND_CHECK("%8u", "     100", 100u); }

/* ---- %x tests ---- */
TEST(test_x_zero) { PRINTF_AND_CHECK("%x", "0", 0); }
TEST(test_x_small) { PRINTF_AND_CHECK("%x", "ff", 0xff); }
TEST(test_x_large) { PRINTF_AND_CHECK("%x", "deadbeef", 0xdeadbeef); }
TEST(test_x_width) { PRINTF_AND_CHECK("%8x", "    dead", 0xdead); }
TEST(test_x_zero_pad) { PRINTF_AND_CHECK("%08x", "0000dead", 0xdead); }
TEST(test_x_upper) { PRINTF_AND_CHECK("%X", "DEADBEEF", 0xdeadbeef); }

/* ---- %c tests ---- */
TEST(test_c_basic) { PRINTF_AND_CHECK("%c", "A", 'A'); }
TEST(test_c_zero) { PRINTF_AND_CHECK("[%c]", "[\0]", 0); /* null char */ }
TEST(test_c_newline) { cap_printf("%c", '\n'); CHECK(capture_buf[0] == '\n'); }

/* ---- %% tests ---- */
TEST(test_percent_alone) { PRINTF_AND_CHECK("%%", "%"); }
TEST(test_percent_embedded) { PRINTF_AND_CHECK("100%%", "100%"); }

/* ---- Combined format tests ---- */
TEST(test_combined_sd) {
    PRINTF_AND_CHECK("%s=%d", "answer=42", "answer", 42);
}
TEST(test_combined_multi) {
    PRINTF_AND_CHECK("[%s] %d/%u/0x%x", "[test] 10/10/0xa", "test", 10, 10u, 0xa);
}
TEST(test_combined_hex_addr) {
    PRINTF_AND_CHECK("0x%04x", "0x0abc", 0xabc);
}
TEST(test_combined_signed_unsigned) {
    int v = -1;
    PRINTF_AND_CHECK("%d/%u", "-1/4294967295", v, (unsigned)v);
}
TEST(test_combined_chars) {
    PRINTF_AND_CHECK("%c%c%c", "ABC", 'A', 'B', 'C');
}

/* ---- Edge cases ---- */
TEST(test_trailing_percent) {
    cap_printf("test%");
    /* Should just print "test%" */
    CHECK(capture_pos == 4);
}
TEST(test_empty_format) {
    cap_printf("");
    CHECK(capture_pos == 0);
}
TEST(test_no_specifiers) {
    PRINTF_AND_CHECK("hello world", "hello world");
}
TEST(test_long_string) {
    PRINTF_AND_CHECK("%s",
        "This is a very long string for testing buffer handling in the printf implementation",
        "This is a very long string for testing buffer handling in the printf implementation");
}

int main(void) {
    printf("=== Printf Format Tests ===\n\n");

    printf("--- %%s ---\n");
    RUN(test_s_basic); RUN(test_s_arg); RUN(test_s_empty);
    RUN(test_s_null); RUN(test_s_embedded);

    printf("--- %%d ---\n");
    RUN(test_d_zero); RUN(test_d_pos); RUN(test_d_neg);
    RUN(test_d_max); RUN(test_d_min); RUN(test_d_width);
    RUN(test_d_zero_pad); RUN(test_d_neg_width);

    printf("--- %%u ---\n");
    RUN(test_u_zero); RUN(test_u_pos); RUN(test_u_max); RUN(test_u_width);

    printf("--- %%x ---\n");
    RUN(test_x_zero); RUN(test_x_small); RUN(test_x_large);
    RUN(test_x_width); RUN(test_x_zero_pad); RUN(test_x_upper);

    printf("--- %%c ---\n");
    RUN(test_c_basic); RUN(test_c_zero); RUN(test_c_newline);

    printf("--- %%%% ---\n");
    RUN(test_percent_alone); RUN(test_percent_embedded);

    printf("--- combined ---\n");
    RUN(test_combined_sd); RUN(test_combined_multi);
    RUN(test_combined_hex_addr); RUN(test_combined_signed_unsigned);
    RUN(test_combined_chars);

    printf("--- edge cases ---\n");
    RUN(test_trailing_percent); RUN(test_empty_format);
    RUN(test_no_specifiers); RUN(test_long_string);

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           passed, test_num, failed);
    return failed ? 1 : 0;
}
