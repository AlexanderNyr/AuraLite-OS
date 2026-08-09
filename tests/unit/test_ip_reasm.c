/*
 * test_ip_reasm.c — host unit tests for REALINTERNET_PLAN phase X4 (IPv4
 * fragment reassembly).  Drives the pure engine kernel/net/ip_reasm.c with
 * an injected clock: reassembly byte-exactness, out-of-order delivery,
 * duplicates, overlapping-fragment attacks (first wins, conflict refused),
 * memory cap, table eviction, timeouts, and key isolation.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AURALITE_IPREASM_HOST_TEST 1
#include "kernel/net/ip_reasm.c"   /* test the implementation directly */

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long _a = (long)(a), _e = (long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%ld want %ld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

#define TIMEOUT 10000u   /* 10 s in ms, matching the kernel policy */

static ipreasm_t T;
static uint8_t outb[IPREASM_CAP];
static uint32_t now;

static ipreasm_key_t K(uint32_t src, uint32_t dst, uint8_t proto, uint16_t id) {
    ipreasm_key_t k; k.src = src; k.dst = dst; k.proto = proto; k.id = id;
    return k;
}

/* Build a patterned "datagram" payload. */
static void pattern(uint8_t *b, int n, int seed) {
    for (int i = 0; i < n; i++) b[i] = (uint8_t)(seed + i * 31 + (i >> 5));
}

/* Feed fragment [off, off+len) of dg; expected=0 unless MF=0. */
static int feed(ipreasm_key_t k, uint16_t exp, uint16_t off,
                const uint8_t *dg, uint16_t len, uint16_t *out_len) {
    return ipreasm_input(&T, now, TIMEOUT, &k, exp, off, dg + off, len,
                         outb, sizeof(outb), out_len);
}

static void reset(void) { ipreasm_init(&T); now = 100000; }

/* 1: basic two-fragment reassembly, byte-identical. */
static void t_two_frag(void) {
    reset();
    uint8_t dg[2000]; pattern(dg, 2000, 3);
    uint16_t olen = 0;
    ipreasm_key_t k = K(0x0A000203, 0x0A00020F, 17, 0x1234);
    CHECK_EQ(feed(k, 0, 0, dg, 1480, &olen), IPREASM_PENDING);
    CHECK_EQ(feed(k, 2000, 1480, dg, 520, &olen), IPREASM_COMPLETE);
    CHECK_EQ(olen, 2000);
    CHECK(memcmp(outb, dg, 2000) == 0);
    CHECK_EQ(T.n_complete, 1);
    CHECK_EQ(ipreasm_entries(&T), 0);         /* slot freed on completion */
}

/* 2: three fragments delivered last-first, byte-identical. */
static void t_out_of_order(void) {
    reset();
    uint8_t dg[3000]; pattern(dg, 3000, 7);
    uint16_t olen = 0;
    ipreasm_key_t k = K(1, 2, 6, 0xBEEF);
    CHECK_EQ(feed(k, 3000, 2960, dg, 40, &olen), IPREASM_PENDING);  /* tail first */
    CHECK_EQ(T.e[0].expected_len, 3000);
    CHECK_EQ(feed(k, 0, 1480, dg, 1480, &olen), IPREASM_PENDING);
    CHECK_EQ(feed(k, 0, 0, dg, 1480, &olen), IPREASM_COMPLETE);
    CHECK_EQ(olen, 3000);
    CHECK(memcmp(outb, dg, 3000) == 0);
}

/* 3: duplicate retransmission of a fragment is benign. */
static void t_duplicate(void) {
    reset();
    uint8_t dg[1600]; pattern(dg, 1600, 11);
    uint16_t olen = 0;
    ipreasm_key_t k = K(5, 6, 17, 42);
    CHECK_EQ(feed(k, 0, 0, dg, 800, &olen), IPREASM_PENDING);
    CHECK_EQ(feed(k, 0, 0, dg, 800, &olen), IPREASM_PENDING);   /* dup */
    CHECK_EQ(T.e[0].have_bytes, 800);                          /* counted once */
    CHECK_EQ(feed(k, 1600, 800, dg, 800, &olen), IPREASM_COMPLETE);
    CHECK(memcmp(outb, dg, 1600) == 0);
}

/* 4: overlapping conflict refused; first fragment wins, entry survives. */
static void t_overlap_conflict(void) {
    reset();
    uint8_t dg[2000]; pattern(dg, 2000, 13);
    uint8_t atk[2000]; memcpy(atk, dg, 2000); atk[100] ^= 0xFF;
    uint16_t olen = 0;
    ipreasm_key_t k = K(7, 8, 17, 777);
    CHECK_EQ(feed(k, 0, 0, dg, 1480, &olen), IPREASM_PENDING);
    /* same range, different bytes -> refused */
    CHECK_EQ(ipreasm_input(&T, now, TIMEOUT, &k, 0, 0, atk, 1480,
                           outb, sizeof(outb), &olen), IPREASM_REFUSED);
    CHECK_EQ(T.n_overlap_refused, 1);
    CHECK_EQ(ipreasm_entries(&T), 1);                  /* entry kept */
    /* partial-overlap attack shape (off 64) with bad bytes -> refused too */
    atk[64] ^= 0xFF;
    CHECK_EQ(ipreasm_input(&T, now, TIMEOUT, &k, 0, 64, atk + 64, 400,
                           outb, sizeof(outb), &olen), IPREASM_REFUSED);
    /* original completest byte-exact: first won */
    CHECK_EQ(feed(k, 2000, 1480, dg, 520, &olen), IPREASM_COMPLETE);
    CHECK(memcmp(outb, dg, 2000) == 0);
}

/* 5: benign overlap with identical bytes is accepted. */
static void t_overlap_benign(void) {
    reset();
    uint8_t dg[1200]; pattern(dg, 1200, 17);
    uint16_t olen = 0;
    ipreasm_key_t k = K(9, 10, 17, 55);
    CHECK_EQ(feed(k, 0, 0, dg, 800, &olen), IPREASM_PENDING);
    CHECK_EQ(feed(k, 0, 64, dg, 800, &olen), IPREASM_PENDING);  /* overlaps 64..800 */
    CHECK_EQ(T.e[0].have_bytes, 864);
    CHECK_EQ(feed(k, 1200, 864, dg, 336, &olen), IPREASM_COMPLETE);
    CHECK(memcmp(outb, dg, 1200) == 0);
}

/* 6: memory cap — datagram past IPREASM_CAP refused, entry killed. */
static void t_cap(void) {
    reset();
    uint8_t dg[7000]; pattern(dg, 7000, 19);
    uint16_t olen = 0;
    ipreasm_key_t k = K(11, 12, 17, 99);
    CHECK_EQ(feed(k, 0, 0, dg, 1480, &olen), IPREASM_PENDING);
    /* fragment that lands past the 8 KiB cap: off 7000 */
    CHECK_EQ(ipreasm_input(&T, now, TIMEOUT, &k, 0, 7000, dg + 7000 - 2000, 1480,
                           outb, sizeof(outb), &olen), IPREASM_REFUSED);
    CHECK_EQ(T.n_cap_refused, 1);
    CHECK_EQ(ipreasm_entries(&T), 0);                  /* poisoned entry killed */
    /* a wrong "last fragment" (size mismatch) is refused */
    reset();
    CHECK_EQ(feed(k, 0, 0, dg, 1480, &olen), IPREASM_PENDING);
    CHECK_EQ(feed(k, 999, 1480, dg, 500, &olen), IPREASM_REFUSED); /* 1480+500!=999 */
    CHECK_EQ(T.n_cap_refused, 1);
}

/* 7: timeout — incomplete reassembly dropped by sweep, not held. */
static void t_timeout(void) {
    reset();
    uint8_t dg[2000]; pattern(dg, 2000, 23);
    uint16_t olen = 0;
    ipreasm_key_t k = K(13, 14, 17, 123);
    CHECK_EQ(feed(k, 0, 0, dg, 1480, &olen), IPREASM_PENDING);
    now += TIMEOUT;                      /* exactly at the boundary: still held */
    CHECK_EQ(ipreasm_sweep(&T, now, TIMEOUT), 0);
    now += 1;                            /* one ms past */
    CHECK_EQ(ipreasm_sweep(&T, now, TIMEOUT), 1);
    CHECK_EQ(T.n_timeout, 1);
    CHECK_EQ(ipreasm_entries(&T), 0);
    /* a late second fragment starts a NEW datagram (no old state) */
    CHECK_EQ(feed(k, 2000, 1480, dg, 520, &olen), IPREASM_PENDING);
    CHECK_EQ(ipreasm_entries(&T), 1);
}

/* 8: lazy expiry via input path (no sweep needed). */
static void t_lazy_timeout(void) {
    reset();
    uint8_t dg[2000]; pattern(dg, 2000, 29);
    uint16_t olen = 0;
    ipreasm_key_t k = K(15, 16, 17, 321);
    CHECK_EQ(feed(k, 0, 0, dg, 1480, &olen), IPREASM_PENDING);
    now += TIMEOUT + 5;                  /* any input call drops the stale entry */
    CHECK_EQ(feed(k, 2000, 1480, dg, 520, &olen), IPREASM_PENDING);
    CHECK_EQ(T.n_timeout, 1);
    /* and the new entry does not falsely complete from the old state */
    CHECK_EQ(ipreasm_entries(&T), 1);
    CHECK_EQ(feed(k, 0, 0, dg, 1480, &olen), IPREASM_COMPLETE);
    CHECK(memcmp(outb, dg, 2000) == 0);
}

/* 9: same fragment id from different sources must not cross-talk. */
static void t_key_isolation(void) {
    reset();
    uint8_t dga[1200], dgb[1200];
    pattern(dga, 1200, 31); pattern(dgb, 1200, 71);
    uint16_t olen = 0;
    ipreasm_key_t a = K(100, 1, 17, 9), b = K(200, 1, 17, 9);
    CHECK_EQ(feed(a, 0, 0, dga, 800, &olen), IPREASM_PENDING);
    CHECK_EQ(feed(b, 0, 0, dgb, 800, &olen), IPREASM_PENDING);
    CHECK_EQ(ipreasm_entries(&T), 2);
    CHECK_EQ(feed(b, 1200, 800, dgb, 400, &olen), IPREASM_COMPLETE);
    CHECK(olen == 1200 && memcmp(outb, dgb, 1200) == 0);
    CHECK_EQ(feed(a, 1200, 800, dga, 400, &olen), IPREASM_COMPLETE);
    CHECK(olen == 1200 && memcmp(outb, dga, 1200) == 0);
}

/* 10: table full — oldest entry evicted for a new datagram (bounded). */
static void t_eviction(void) {
    reset();
    uint8_t dg[1000]; pattern(dg, 1000, 37);
    uint16_t olen = 0;
    ipreasm_key_t keys[IPREASM_MAX_ENTRIES + 1];
    for (int i = 0; i < IPREASM_MAX_ENTRIES; i++) {
        keys[i] = K(1000 + i, 1, 17, (uint16_t)i);
        CHECK_EQ(feed(keys[i], 0, 0, dg, 800, &olen), IPREASM_PENDING);
        now += 10;
    }
    CHECK_EQ(ipreasm_entries(&T), IPREASM_MAX_ENTRIES);
    keys[IPREASM_MAX_ENTRIES] = K(9999, 1, 17, 77);
    CHECK_EQ(feed(keys[IPREASM_MAX_ENTRIES], 0, 0, dg, 800, &olen), IPREASM_PENDING);
    CHECK_EQ(ipreasm_entries(&T), IPREASM_MAX_ENTRIES);  /* still bounded */
    CHECK_EQ(T.n_evicted, 1);
    /* the oldest datagram lost its slot; its last fragment restarts it */
    CHECK_EQ(feed(keys[0], 1000, 800, dg, 200, &olen), IPREASM_PENDING);
    /* the newest survivor completes fine */
    CHECK_EQ(feed(keys[IPREASM_MAX_ENTRIES], 1000, 800, dg, 200, &olen),
             IPREASM_COMPLETE);
    CHECK(memcmp(outb, dg, 1000) == 0);
}

/* 11: malformed inputs refused. */
static void t_malformed(void) {
    reset();
    uint8_t dg[1000]; pattern(dg, 1000, 41);
    uint16_t olen = 0;
    ipreasm_key_t k = K(1, 1, 17, 1);
    CHECK_EQ(ipreasm_input(&T, now, TIMEOUT, &k, 0, 0, dg, 0, outb,
                           sizeof(outb), &olen), IPREASM_REFUSED);      /* empty */
    CHECK_EQ(ipreasm_input(&T, now, TIMEOUT, &k, 0, 12/* not x8 */, dg + 12, 100,
                           outb, sizeof(outb), &olen), IPREASM_REFUSED);
    CHECK_EQ(ipreasm_entries(&T), 0);
}

int main(void) {
    RUN(t_two_frag);
    RUN(t_out_of_order);
    RUN(t_duplicate);
    RUN(t_overlap_conflict);
    RUN(t_overlap_benign);
    RUN(t_cap);
    RUN(t_timeout);
    RUN(t_lazy_timeout);
    RUN(t_key_isolation);
    RUN(t_eviction);
    RUN(t_malformed);

    printf("test_ip_reasm: %d/%d scenarios passed\n", passed, tn);
    if (failed == 0) { printf("PASS: 0 failures\n"); return 0; }
    printf("FAIL: %d check(s) failed\n", failed);
    return 1;
}
