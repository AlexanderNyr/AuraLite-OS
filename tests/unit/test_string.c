/*
 * test_string.c — comprehensive unit tests for the kernel's freestanding
 * string/memory functions (kernel/lib/string.c).
 *
 * Compiled with the host compiler and linked against string.c directly.
 * 30+ test cases covering: memset, memcpy, memmove, memcmp, strlen,
 * strncpy, strcmp, strcpy.
 *
 * Run with: make test-unit
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Declare kernel functions (they use the same signatures as libc, but we
 * link against the kernel implementation, not libc). */
extern void  *kmemset(void *dst, int c, size_t n);
extern void  *kmemcpy(void *dst, const void *src, size_t n);
extern void  *kmemmove(void *dst, const void *src, size_t n);
extern int    kmemcmp(const void *a, const void *b, size_t n);
extern size_t kstrlen(const char *s);
extern char  *kstrncpy(char *dst, const char *src, size_t n);
extern int    kstrcmp(const char *a, const char *b);
extern char  *kstrcpy(char *dst, const char *src);

/* We rename the kernel functions to avoid clashing with libc. */
#define kmemset   aos_memset
#define kmemcpy   aos_memcpy
#define kmemmove  aos_memmove
#define kmemcmp   aos_memcmp
#define kstrlen   aos_strlen
#define kstrncpy  aos_strncpy
#define kstrcmp   aos_strcmp
#define kstrcpy   aos_strcpy

/* Include the kernel implementation directly with renames. */
#define memset   aos_memset
#define memcpy   aos_memcpy
#define memmove  aos_memmove
#define memcmp   aos_memcmp
#define strlen   aos_strlen
#define strncpy  aos_strncpy
#define strcmp   aos_strcmp
#define strcpy   aos_strcpy

#include "../../kernel/lib/string.c"

#undef memset
#undef memcpy
#undef memmove
#undef memcmp
#undef strlen
#undef strncpy
#undef strcmp
#undef strcpy

static int passed = 0;
static int failed = 0;
static int test_num = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    int before = failed; \
    name(); \
    test_num++; \
    if (failed == before) { passed++; } \
} while(0)

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: line %d: %s\n", __LINE__, #cond); \
        failed++; \
    } \
} while(0)

#define CHECK_EQ(actual, expected) do { \
    if ((long)(actual) != (long)(expected)) { \
        printf("  FAIL: line %d: %s = %ld, expected %ld\n", \
               __LINE__, #actual, (long)(actual), (long)(expected)); \
        failed++; \
    } \
} while(0)

/* ---- memset tests ---- */

TEST(test_memset_basic) {
    char buf[16] = {0};
    kmemset(buf, 'A', 10);
    for (int i = 0; i < 10; i++) CHECK_EQ(buf[i], 'A');
    CHECK_EQ(buf[10], 0);  /* not written */
}

TEST(test_memset_zero_len) {
    char buf[4] = {1, 2, 3, 4};
    kmemset(buf, 0, 0);
    CHECK_EQ(buf[0], 1);
    CHECK_EQ(buf[1], 2);
}

TEST(test_memset_full) {
    char buf[8];
    kmemset(buf, 0xFF, 8);
    for (int i = 0; i < 8; i++) CHECK_EQ((unsigned char)buf[i], 0xFF);
}

TEST(test_memset_value_0) {
    int buf[4];
    kmemset(buf, 0, sizeof(buf));
    for (int i = 0; i < 4; i++) CHECK_EQ(buf[i], 0);
}

TEST(test_memset_returns_dst) {
    char buf[8];
    void *ret = kmemset(buf, 'x', 4);
    CHECK(ret == buf);
}

TEST(test_memset_large) {
    static char big[4096];
    kmemset(big, 'Z', 4096);
    CHECK_EQ(big[0], 'Z');
    CHECK_EQ(big[4095], 'Z');
    CHECK_EQ(big[2048], 'Z');
}

TEST(test_memset_byte_value) {
    char buf[4];
    kmemset(buf, 0x80, 4);
    for (int i = 0; i < 4; i++) CHECK_EQ((unsigned char)buf[i], 0x80);
}

TEST(test_memset_overlapping_dst) {
    char buf[16] = "Hello, World!!!";
    kmemset(buf + 7, '*', 5);
    CHECK_EQ(kmemcmp(buf, "Hello, *****", 12), 0);
}

/* ---- memcpy tests ---- */

TEST(test_memcpy_basic) {
    const char *src = "Hello";
    char dst[8] = {0};
    kmemcpy(dst, src, 5);
    CHECK_EQ(kmemcmp(dst, "Hello", 5), 0);
}

TEST(test_memcpy_zero_len) {
    char dst[4] = {1, 2, 3, 4};
    char src[4] = {9, 9, 9, 9};
    kmemcpy(dst, src, 0);
    CHECK_EQ(dst[0], 1);
}

TEST(test_memcpy_returns_dst) {
    char dst[4], src[4] = {1, 2, 3, 4};
    void *ret = kmemcpy(dst, src, 4);
    CHECK(ret == dst);
}

TEST(test_memcpy_large) {
    static char src[4096], dst[4096];
    for (int i = 0; i < 4096; i++) src[i] = (char)(i & 0xFF);
    kmemcpy(dst, src, 4096);
    CHECK_EQ(kmemcmp(src, dst, 4096), 0);
}

TEST(test_memcpy_struct) {
    struct { int a; long b; char c; } src = {42, 999, 'X'}, dst;
    kmemcpy(&dst, &src, sizeof(dst));
    CHECK_EQ(dst.a, 42);
    CHECK_EQ(dst.b, 999);
    CHECK_EQ(dst.c, 'X');
}

/* ---- memmove tests ---- */

TEST(test_memmove_no_overlap) {
    char src[] = "ABCDEF";
    char dst[8] = {0};
    kmemmove(dst, src, 6);
    CHECK_EQ(kmemcmp(dst, "ABCDEF", 6), 0);
}

TEST(test_memmove_overlap_forward) {
    /* dst < src: copy forward */
    char buf[] = "ABCDEFGH";
    kmemmove(buf, buf + 2, 4);
    CHECK_EQ(kmemcmp(buf, "CDEF", 4), 0);
}

TEST(test_memmove_overlap_backward) {
    /* dst > src: copy backward */
    char buf[] = "ABCDEFGH";
    kmemmove(buf + 2, buf, 4);
    CHECK_EQ(kmemcmp(buf + 2, "ABCD", 4), 0);
}

TEST(test_memmove_zero_len) {
    char buf[4] = {1, 2, 3, 4};
    kmemmove(buf, buf, 0);
    CHECK_EQ(buf[0], 1);
}

TEST(test_memmove_self_copy) {
    char buf[] = "Hello";
    kmemmove(buf, buf, 5);
    CHECK_EQ(kmemcmp(buf, "Hello", 5), 0);
}

/* ---- memcmp tests ---- */

TEST(test_memcmp_equal) {
    CHECK_EQ(kmemcmp("Hello", "Hello", 5), 0);
}

TEST(test_memcmp_diff_first) {
    CHECK(kmemcmp("Aello", "Hello", 5) < 0);
}

TEST(test_memcmp_diff_last) {
    CHECK(kmemcmp("HellA", "HellB", 5) < 0);
}

TEST(test_memcmp_zero_len) {
    CHECK_EQ(kmemcmp("ABC", "XYZ", 0), 0);
}

TEST(test_memcmp_partial) {
    CHECK_EQ(kmemcmp("ABCDEF", "ABCXYZ", 3), 0);
    CHECK(kmemcmp("ABCDEF", "ABCXYZ", 4) < 0);
}

TEST(test_memcmp_sign) {
    unsigned char a[2] = {0x00, 0xFF};
    unsigned char b[2] = {0x01, 0x00};
    CHECK(kmemcmp(a, b, 2) < 0);
}

/* ---- strlen tests ---- */

TEST(test_strlen_basic) {
    CHECK_EQ(kstrlen("Hello"), 5);
}

TEST(test_strlen_empty) {
    CHECK_EQ(kstrlen(""), 0);
}

TEST(test_strlen_long) {
    CHECK_EQ(kstrlen("The quick brown fox jumps over the lazy dog"), 43);
}

TEST(test_strlen_with_null) {
    CHECK_EQ(kstrlen("\0hidden"), 0);
}

/* ---- strncpy tests ---- */

TEST(test_strncpy_basic) {
    char dst[8];
    kstrncpy(dst, "Hello", 8);
    CHECK_EQ(kstrcmp(dst, "Hello"), 0);
}

TEST(test_strncpy_truncate) {
    char dst[4];
    kstrncpy(dst, "Hello", 3);
    CHECK_EQ(dst[0], 'H');
    CHECK_EQ(dst[1], 'e');
    CHECK_EQ(dst[2], 'l');
    /* dst[3] is untouched */
}

TEST(test_strncpy_pads) {
    char dst[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    kstrncpy(dst, "Hi", 8);
    CHECK_EQ(dst[0], 'H');
    CHECK_EQ(dst[1], 'i');
    for (int i = 2; i < 8; i++) CHECK_EQ(dst[i], 0);
}

TEST(test_strncpy_exact) {
    char dst[6];
    kstrncpy(dst, "Hello", 5);
    CHECK_EQ(kmemcmp(dst, "Hello", 5), 0);
}

/* ---- strcmp tests ---- */

TEST(test_strcmp_equal) {
    CHECK_EQ(kstrcmp("Hello", "Hello"), 0);
}

TEST(test_strcmp_less) {
    CHECK(kstrcmp("Apple", "Banana") < 0);
}

TEST(test_strcmp_greater) {
    CHECK(kstrcmp("Banana", "Apple") > 0);
}

TEST(test_strcmp_prefix) {
    CHECK(kstrcmp("Hell", "Hello") < 0);
}

TEST(test_strcmp_empty) {
    CHECK_EQ(kstrcmp("", ""), 0);
    CHECK(kstrcmp("", "A") < 0);
    CHECK(kstrcmp("A", "") > 0);
}

/* ---- strcpy tests ---- */

TEST(test_strcpy_basic) {
    char dst[16];
    kstrcpy(dst, "Hello World");
    CHECK_EQ(kstrcmp(dst, "Hello World"), 0);
}

TEST(test_strcpy_empty) {
    char dst[4] = {'X', 'X', 'X', 'X'};
    kstrcpy(dst, "");
    CHECK_EQ(dst[0], 0);
}

TEST(test_strcpy_returns_dst) {
    char dst[16];
    char *ret = kstrcpy(dst, "test");
    CHECK(ret == dst);
}

int main(void) {
    printf("=== String/Memory Function Tests ===\n\n");

    printf("--- memset ---\n");
    RUN(test_memset_basic);
    RUN(test_memset_zero_len);
    RUN(test_memset_full);
    RUN(test_memset_value_0);
    RUN(test_memset_returns_dst);
    RUN(test_memset_large);
    RUN(test_memset_byte_value);
    RUN(test_memset_overlapping_dst);

    printf("--- memcpy ---\n");
    RUN(test_memcpy_basic);
    RUN(test_memcpy_zero_len);
    RUN(test_memcpy_returns_dst);
    RUN(test_memcpy_large);
    RUN(test_memcpy_struct);

    printf("--- memmove ---\n");
    RUN(test_memmove_no_overlap);
    RUN(test_memmove_overlap_forward);
    RUN(test_memmove_overlap_backward);
    RUN(test_memmove_zero_len);
    RUN(test_memmove_self_copy);

    printf("--- memcmp ---\n");
    RUN(test_memcmp_equal);
    RUN(test_memcmp_diff_first);
    RUN(test_memcmp_diff_last);
    RUN(test_memcmp_zero_len);
    RUN(test_memcmp_partial);
    RUN(test_memcmp_sign);

    printf("--- strlen ---\n");
    RUN(test_strlen_basic);
    RUN(test_strlen_empty);
    RUN(test_strlen_long);
    RUN(test_strlen_with_null);

    printf("--- strncpy ---\n");
    RUN(test_strncpy_basic);
    RUN(test_strncpy_truncate);
    RUN(test_strncpy_pads);
    RUN(test_strncpy_exact);

    printf("--- strcmp ---\n");
    RUN(test_strcmp_equal);
    RUN(test_strcmp_less);
    RUN(test_strcmp_greater);
    RUN(test_strcmp_prefix);
    RUN(test_strcmp_empty);

    printf("--- strcpy ---\n");
    RUN(test_strcpy_basic);
    RUN(test_strcpy_empty);
    RUN(test_strcpy_returns_dst);

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           passed, test_num, failed);
    return failed ? 1 : 0;
}
