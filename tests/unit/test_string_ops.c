/* test_string_ops.c — host gate for the O1 fast string backend
 * (OPT_PLAN.md O1; kernel/arch/x86_64/string_fast.c).
 *
 * test_string.c already covers the PORTABLE bodies in
 * kernel/lib/string.c (which the host build still compiles, because
 * ARCH_X86_64 is a kernel-build define).  This test covers what that one
 * cannot: the rep-string backend the x86_64 kernel actually links, and
 * the word-wide memcmp/strlen now shared by both builds — across the
 * alignment × size × overlap matrix where rep/word tricks break if they
 * are going to break at all:
 *
 *   - sizes straddling the 64-byte scalar/rep crossover (63, 64, 65);
 *   - every src/dst misalignment in 0..8;
 *   - memmove overlap in both directions at distances 1..9 (backward
 *     8-byte chunks read the tail first — distance < 8 is the trap);
 *   - strlen at every alignment and length 0..24 (the aligned word scan
 *     must not report a NUL early or late);
 *   - memcmp with the difference planted at every offset of a word.
 *
 * Everything is checked against a naive byte-loop reference, and the
 * canary bytes around each destination are checked after every call —
 * a copy that writes one byte past n is a bug the return value hides.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull in the fast backend under renamed symbols. */
#define memcpy   fast_memcpy
#define memset   fast_memset
#define memmove  fast_memmove
#include "../../kernel/arch/x86_64/string_fast.c"
#undef memcpy
#undef memset
#undef memmove

/* Pull in the portable word-wide memcmp/strlen the same way. */
#define memset   port_memset
#define memcpy   port_memcpy
#define memmove  port_memmove
#define memcmp   port_memcmp
#define strlen   port_strlen
#define strncpy  port_strncpy
#define strcmp   port_strcmp
#define strncmp  port_strncmp
#define strcpy   port_strcpy
#define strcat   port_strcat
#define strchr   port_strchr
#define strrchr  port_strrchr
#define strstr   port_strstr
#include "../../kernel/lib/string.c"
#undef memset
#undef memcpy
#undef memmove
#undef memcmp
#undef strlen
#undef strncpy
#undef strcmp
#undef strncmp
#undef strcpy
#undef strcat
#undef strchr
#undef strrchr
#undef strstr

static int passed = 0;
static int failed = 0;

#define CHECK(cond, ...) do {                       \
        if (cond) { passed++; }                     \
        else {                                      \
            failed++;                               \
            printf("FAIL: ");                       \
            printf(__VA_ARGS__);                    \
            printf("\n");                           \
        }                                           \
    } while (0)

#define GUARD 16
#define ARENA 4096

static unsigned char src_arena[ARENA + 2 * GUARD];
static unsigned char dst_arena[ARENA + 2 * GUARD];
static unsigned char ref_arena[ARENA + 2 * GUARD];

static void fill_pattern(unsigned char *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(seed + i * 7 + 3);
}

static int guards_intact(const unsigned char *arena) {
    for (int i = 0; i < GUARD; i++) {
        if (arena[i] != 0xEE) return 0;
        if (arena[ARENA + GUARD + i] != 0xEE) return 0;
    }
    return 1;
}

static void reset_arenas(void) {
    memset(src_arena, 0xEE, sizeof(src_arena));
    memset(dst_arena, 0xEE, sizeof(dst_arena));
    memset(ref_arena, 0xEE, sizeof(ref_arena));
}

static void test_memcpy_matrix(void) {
    static const size_t sizes[] = { 0, 1, 3, 7, 8, 9, 15, 16, 31,
                                    63, 64, 65, 127, 128, 255, 1024 };
    for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        for (unsigned so = 0; so <= 8; so++) {
            for (unsigned dofs = 0; dofs <= 8; dofs++) {
                size_t n = sizes[si];
                reset_arenas();
                unsigned char *s = src_arena + GUARD + so;
                unsigned char *d = dst_arena + GUARD + dofs;
                unsigned char *r = ref_arena + GUARD + dofs;
                fill_pattern(s, n, so * 31u + dofs);
                for (size_t i = 0; i < n; i++) r[i] = s[i];
                void *ret = fast_memcpy(d, s, n);
                int ok = (ret == d) && memcmp(d, r, n) == 0 &&
                         guards_intact(dst_arena);
                CHECK(ok, "memcpy n=%zu so=%u do=%u", n, so, dofs);
                if (!ok) return;   /* one report per shape is enough */
            }
        }
    }
}

static void test_memset_matrix(void) {
    static const size_t sizes[] = { 0, 1, 7, 63, 64, 65, 256, 1024 };
    static const int vals[] = { 0, 0x5A, 0xFF, -1 };
    for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        for (unsigned vi = 0; vi < sizeof(vals) / sizeof(vals[0]); vi++) {
            for (unsigned dofs = 0; dofs <= 8; dofs++) {
                size_t n = sizes[si];
                reset_arenas();
                unsigned char *d = dst_arena + GUARD + dofs;
                void *ret = fast_memset(d, vals[vi], n);
                int ok = (ret == d) && guards_intact(dst_arena);
                for (size_t i = 0; ok && i < n; i++) {
                    if (d[i] != (unsigned char)vals[vi]) ok = 0;
                }
                CHECK(ok, "memset n=%zu v=%d do=%u", n, vals[vi], dofs);
                if (!ok) return;
            }
        }
    }
}

static void test_memmove_overlap(void) {
    /* Overlap distances 1..9 in both directions, sizes across the
     * chunking boundaries. */
    static const size_t sizes[] = { 1, 7, 8, 9, 16, 63, 64, 65, 256 };
    for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        for (unsigned dist = 1; dist <= 9; dist++) {
            size_t n = sizes[si];
            /* Backward-overlap case: dst = src + dist. */
            reset_arenas();
            unsigned char *base = dst_arena + GUARD;
            unsigned char *rbase = ref_arena + GUARD;
            fill_pattern(base, n + dist, (unsigned)(n + dist));
            for (size_t i = 0; i < n + dist; i++) rbase[i] = base[i];
            fast_memmove(base + dist, base, n);
            for (size_t i = n; i > 0; i--) rbase[dist + i - 1] = rbase[i - 1];
            CHECK(memcmp(base + dist, rbase + dist, n) == 0 &&
                  guards_intact(dst_arena),
                  "memmove backward n=%zu dist=%u", n, dist);

            /* Forward-overlap case: dst = src - dist. */
            reset_arenas();
            fill_pattern(base, n + dist, (unsigned)(n * 3 + dist));
            for (size_t i = 0; i < n + dist; i++) rbase[i] = base[i];
            fast_memmove(base, base + dist, n);
            for (size_t i = 0; i < n; i++) rbase[i] = rbase[dist + i];
            CHECK(memcmp(base, rbase, n) == 0 && guards_intact(dst_arena),
                  "memmove forward n=%zu dist=%u", n, dist);
        }
    }
    /* dst == src is a defined no-op. */
    reset_arenas();
    unsigned char *base = dst_arena + GUARD;
    fill_pattern(base, 64, 5);
    fast_memmove(base, base, 64);
    CHECK(base[0] == (unsigned char)(5 + 3) && guards_intact(dst_arena),
          "memmove self is a no-op");
}

static void test_memcmp_wordwide(void) {
    unsigned char a[64], b[64];
    for (unsigned diff_at = 0; diff_at < 40; diff_at++) {
        for (unsigned ofs = 0; ofs <= 8 && ofs + 40 < sizeof(a); ofs++) {
            fill_pattern(a, sizeof(a), 9);
            fill_pattern(b, sizeof(b), 9);
            CHECK(port_memcmp(a + ofs, b + ofs, 40) == 0,
                  "memcmp equal ofs=%u", ofs);
            b[ofs + diff_at] = (unsigned char)(b[ofs + diff_at] + 1);
            int got = port_memcmp(a + ofs, b + ofs, 40);
            int want = (int)a[ofs + diff_at] - (int)b[ofs + diff_at];
            CHECK((got < 0) == (want < 0) && got != 0,
                  "memcmp sign diff_at=%u ofs=%u", diff_at, ofs);
            /* The difference one byte past n must be invisible. */
            fill_pattern(b, sizeof(b), 9);
            b[ofs + 40] = 0xAA;
            CHECK(port_memcmp(a + ofs, b + ofs, 40) == 0,
                  "memcmp stops at n (ofs=%u)", ofs);
        }
    }
}

static void test_strlen_wordwide(void) {
    /* Every alignment × every length 0..24; poison after the NUL so an
     * over-read that changes the answer is caught. */
    static unsigned char buf[64];
    for (unsigned ofs = 0; ofs <= 8; ofs++) {
        for (unsigned len = 0; len <= 24; len++) {
            memset(buf, 'x', sizeof(buf));
            char *s = (char *)buf + ofs;
            for (unsigned i = 0; i < len; i++) s[i] = (char)('a' + i % 26);
            s[len] = '\0';
            /* Poison a NUL right after — the scan must not have needed it. */
            CHECK(port_strlen(s) == len, "strlen len=%u ofs=%u", len, ofs);
        }
    }
}

int main(void) {
    printf("=== O1 string-ops test suite (fast backend + word-wide) ===\n\n");

    test_memcpy_matrix();
    test_memset_matrix();
    test_memmove_overlap();
    test_memcmp_wordwide();
    test_strlen_wordwide();

    printf("\n%d passed, %d failed\n", passed, failed);
    if (failed == 0) printf("=== ALL TESTS PASSED ===\n");
    return failed ? 1 : 0;
}
