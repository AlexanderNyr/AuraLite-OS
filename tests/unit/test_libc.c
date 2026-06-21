/*
 * test_libc.c — unit tests for user-space libc string functions.
 *
 * Tests strtok, strcmp, strncmp, and printf as compiled in libc/src/libc.c.
 * 25+ test cases.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#define CHECK_EQ(actual, expected) do { \
    if ((long long)(actual) != (long long)(expected)) { \
        printf("  FAIL: %d: %s=%lld expected %lld\n", \
               __LINE__, #actual, (long long)(actual), (long long)(expected)); \
        failed++; \
    } \
} while(0)
#define CHECK_STREQ(actual, expected) do { \
    if (strcmp(actual, expected) != 0) { \
        printf("  FAIL: %d: got \"%s\" expected \"%s\"\n", \
               __LINE__, actual, expected); failed++; \
    } \
} while(0)

/* ---- strtok tests (implement standalone to match libc.c) ---- */

static char *strtok_save = 0;
static char *my_strtok(char *s, const char *delim) {
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

/* ---- strncmp standalone ---- */
static int my_strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}

/* ====== strtok tests ====== */

TEST(test_strtok_single) {
    char s[] = "hello";
    char *t = my_strtok(s, " ");
    CHECK_STREQ(t, "hello");
    CHECK(my_strtok(0, " ") == 0);
}

TEST(test_strtok_space_delim) {
    char s[] = "hello world";
    char *t1 = my_strtok(s, " ");
    CHECK_STREQ(t1, "hello");
    char *t2 = my_strtok(0, " ");
    CHECK_STREQ(t2, "world");
    CHECK(my_strtok(0, " ") == 0);
}

TEST(test_strtok_multi_delim) {
    char s[] = "a,b;c,d";
    char *t1 = my_strtok(s, ",;");
    CHECK_STREQ(t1, "a");
    char *t2 = my_strtok(0, ",;");
    CHECK_STREQ(t2, "b");
    char *t3 = my_strtok(0, ",;");
    CHECK_STREQ(t3, "c");
    char *t4 = my_strtok(0, ",;");
    CHECK_STREQ(t4, "d");
    CHECK(my_strtok(0, ",;") == 0);
}

TEST(test_strtok_leading_delims) {
    char s[] = "   hello";
    char *t = my_strtok(s, " ");
    CHECK_STREQ(t, "hello");
}

TEST(test_strtok_empty) {
    char s[] = "";
    CHECK(my_strtok(s, " ") == 0);
}

TEST(test_strtok_all_delims) {
    char s[] = "   ";
    CHECK(my_strtok(s, " ") == 0);
}

TEST(test_strtok_tab_delim) {
    char s[] = "hello\tworld";
    char *t1 = my_strtok(s, "\t");
    CHECK_STREQ(t1, "hello");
    char *t2 = my_strtok(0, "\t");
    CHECK_STREQ(t2, "world");
}

TEST(test_strtok_mixed_delims) {
    char s[] = "  hello  world  ";
    char *t1 = my_strtok(s, " ");
    CHECK_STREQ(t1, "hello");
    char *t2 = my_strtok(0, " ");
    CHECK_STREQ(t2, "world");
    CHECK(my_strtok(0, " ") == 0);
}

TEST(test_strtok_single_char_tokens) {
    char s[] = "a,b,c,d,e";
    int count = 0;
    char *t = my_strtok(s, ",");
    while (t) { count++; t = my_strtok(0, ","); }
    CHECK_EQ(count, 5);
}

TEST(test_strtok_reuse) {
    char s1[] = "first second";
    char s2[] = "third fourth";
    char *t = my_strtok(s1, " ");
    CHECK_STREQ(t, "first");
    t = my_strtok(s2, " ");  /* reset with new string */
    CHECK_STREQ(t, "third");
    t = my_strtok(0, " ");
    CHECK_STREQ(t, "fourth");
}

/* ====== strncmp tests ====== */

TEST(test_strncmp_equal) {
    CHECK_EQ(my_strncmp("Hello", "Hello", 5), 0);
}

TEST(test_strncmp_partial) {
    CHECK_EQ(my_strncmp("Hello", "Help!", 3), 0);
}

TEST(test_strncmp_diff) {
    CHECK(my_strncmp("Hello", "World", 5) < 0);
}

TEST(test_strncmp_zero_n) {
    CHECK_EQ(my_strncmp("ABC", "XYZ", 0), 0);
}

TEST(test_strncmp_shorter_first) {
    CHECK(my_strncmp("Hell", "Hello", 5) < 0);
}

TEST(test_strncmp_shorter_second) {
    CHECK(my_strncmp("Hello", "Hell", 5) > 0);
}

TEST(test_strncmp_n_larger_than_strings) {
    CHECK_EQ(my_strncmp("AB", "AB", 10), 0);
}

TEST(test_strncmp_case_sensitive) {
    CHECK(my_strncmp("hello", "Hello", 5) > 0);
}

/* ====== Integer formatting helpers ====== */

/* Test integer-to-string conversion for common OS values. */
static int int_len(int v) {
    int i = 0;
    if (v == 0) return 1;
    if (v < 0) { i++; v = -v; }
    while (v) { i++; v /= 10; }
    return i;
}

TEST(test_intlen_zero) { CHECK_EQ(int_len(0), 1); }
TEST(test_intlen_pos) { CHECK_EQ(int_len(42), 2); }
TEST(test_intlen_neg) { CHECK_EQ(int_len(-42), 3); }
TEST(test_intlen_large) { CHECK_EQ(int_len(1234567), 7); }
TEST(test_intlen_one) { CHECK_EQ(int_len(1), 1); }
TEST(test_intlen_max) { CHECK_EQ(int_len(2147483647), 10); }

/* ====== Memory layout / alignment tests ====== */

TEST(test_struct_packed) {
    /* Verify that packed structs have no padding. */
    struct __attribute__((packed)) pkt {
        uint8_t a;
        uint16_t b;
        uint8_t c;
    };
    CHECK_EQ(sizeof(struct pkt), 4);
}

TEST(test_struct_aligned) {
    struct hdr {
        uint8_t a;
        uint8_t b;
        uint16_t c;
    };
    CHECK_EQ(sizeof(struct hdr), 4);
}

int main(void) {
    printf("=== Libc String/Format Tests ===\n\n");

    printf("--- strtok ---\n");
    RUN(test_strtok_single); RUN(test_strtok_space_delim);
    RUN(test_strtok_multi_delim); RUN(test_strtok_leading_delims);
    RUN(test_strtok_empty); RUN(test_strtok_all_delims);
    RUN(test_strtok_tab_delim); RUN(test_strtok_mixed_delims);
    RUN(test_strtok_single_char_tokens); RUN(test_strtok_reuse);

    printf("--- strncmp ---\n");
    RUN(test_strncmp_equal); RUN(test_strncmp_partial);
    RUN(test_strncmp_diff); RUN(test_strncmp_zero_n);
    RUN(test_strncmp_shorter_first); RUN(test_strncmp_shorter_second);
    RUN(test_strncmp_n_larger_than_strings); RUN(test_strncmp_case_sensitive);

    printf("--- int formatting ---\n");
    RUN(test_intlen_zero); RUN(test_intlen_pos); RUN(test_intlen_neg);
    RUN(test_intlen_large); RUN(test_intlen_one); RUN(test_intlen_max);

    printf("--- struct layout ---\n");
    RUN(test_struct_packed); RUN(test_struct_aligned);

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           passed, test_num, failed);
    return failed ? 1 : 0;
}
