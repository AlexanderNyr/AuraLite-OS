/*
 * test_w32_accel.c — host unit tests for the RT_ACCELERATOR decoder.
 *
 * W32RUN_PLAN.md phase WR-2, gap "Tools > Options".  Until this landed,
 * LoadAcceleratorsW found the RT_ACCELERATOR resource and threw it away,
 * returning an empty table -- so no application hot key ever fired.  The
 * format knowledge is now in w32/src/w32_rsrc.c as w32_accel_parse() and
 * w32_accel_match(); this file pins both.
 *
 * The grammar is measured, not assumed.  Walking the .rsrc section of the
 * pinned 7-Zip 24.09 7zFM.exe (sha256 dc4fdcd9…bf58) finds exactly one
 * RT_ACCELERATOR, id 72, 16 bytes, i.e. two 8-byte records:
 *
 *   FVIRTKEY|FNOINVERT      (0x0003)  VK_F1  (0x70)  -> 960
 *   FVIRTKEY|FNOINVERT|FALT (0x0093)  VK_F12 (0x7b)  -> 736
 *
 * No foreign bytes are committed to the tree: the table below is emitted
 * here, record by record, in the documented layout -- the same convention
 * tools/wr2_make_fixture.py uses for the ZIP fixture.
 *
 * A unit test must not depend on the pinned binary being present, which it
 * never is in CI (the legal boundary keeps it outside the tree).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32/user32.h"

/* The decoder under test.  Including the .c directly is the established
 * pattern here (test_w32_pe.c, test_w32_a6.c, test_w32_a8.c): the guest
 * build compiles the same file, so the test cannot drift from the shipped
 * parser the way a re-implementation would. */
#include "../../w32/src/w32_rsrc.c"
#include "../../w32/src/w32_errno.c"   /* w32_set_last_error, the cross-TU
                                          symbol the decoder's file needs */

/* w32_errno.c stores the last-error code in the thread environment block,
 * which is a guest construct.  A static one is enough here: nothing under
 * test reads it, the stub only keeps the link honest. */
#include "w32/w32_teb.h"
static struct w32_teb host_teb;
struct w32_teb *w32_teb_self(void) { return &host_teb; }

/* w32_rsrc.c's module lookup and byte walk: no module table on the host,
 * and nothing here has PE bytes to walk.  Same stubs test_w32_a8.c provides. */
W32_HMODULE W32ABI w32_GetModuleHandleA(const char *name) { (void)name; return 0; }
int w32_module_file_bytes(void *h, const uint8_t **d, size_t *sz) {
    (void)h; (void)d; (void)sz; return 0;
}
W32ABI void SetLastError(W32_DWORD c) { w32_set_last_error(c); }

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; \
                    else printf("  [%s] FAILED\n", #f); } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long long _a = (long long)(a), _e = (long long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%lld want %lld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* ---- the 7-Zip table, rebuilt from its measured records ---------------- */

#define ACC_F1_CMD   960u
#define ACC_F12_CMD  736u

/* Append one 8-byte record: fVirt, key, cmd (little-endian, unpadded). */
static size_t acc_rec(uint8_t *p, uint16_t fv, uint16_t key, uint32_t cmd) {
    p[0] = (uint8_t)(fv & 0xFFu);       p[1] = (uint8_t)(fv >> 8);
    p[2] = (uint8_t)(key & 0xFFu);      p[3] = (uint8_t)(key >> 8);
    p[4] = (uint8_t)(cmd & 0xFFu);      p[5] = (uint8_t)((cmd >> 8) & 0xFFu);
    p[6] = (uint8_t)((cmd >> 16) & 0xFFu); p[7] = (uint8_t)((cmd >> 24) & 0xFFu);
    return 8u;
}

/* The exact 16 bytes 7zFM.exe carries, so the decoder is pinned against the
 * resource a real application ships rather than against a shape I invented. */
static uint8_t zip7_table[16];
static void build_7zip_table(void) {
    size_t n = 0;
    n += acc_rec(zip7_table + n,
                 (uint16_t)(W32_FVIRTKEY | W32_FNOINVERT), 0x70u /*VK_F1*/,
                 ACC_F1_CMD);
    n += acc_rec(zip7_table + n,
                 (uint16_t)(W32_FVIRTKEY | W32_FNOINVERT | W32_FALT),
                 0x7bu /*VK_F12*/, ACC_F12_CMD);
    CHECK_EQ(n, 16u);                   /* matches the measured resource size */
}

/* ---- decoding the real table ------------------------------------------ */

static void t_parse_7zip_table(void) {
    build_7zip_table();
    W32_ACCEL out[8];
    memset(out, 0xA5, sizeof out);

    int n = w32_accel_parse(zip7_table, sizeof zip7_table, out, 8);
    CHECK_EQ(n, 2);
    if (n != 2) return;

    /* Entry 0: F1 -> 960. */
    CHECK_EQ(out[0].flags, (long)(W32_FVIRTKEY | W32_FNOINVERT));
    CHECK_EQ(out[0].key, 0x70);
    CHECK_EQ(out[0].cmd, ACC_F1_CMD);

    /* Entry 1: Alt+F12 -> 736.  The FALT bit must survive the round trip:
     * dropping it is the bug that makes Alt+F12 fire on a bare F12. */
    CHECK_EQ(out[1].flags, (long)(W32_FVIRTKEY | W32_FNOINVERT | W32_FALT));
    CHECK_EQ(out[1].key, 0x7b);
    CHECK_EQ(out[1].cmd, ACC_F12_CMD);

    /* Nothing was written past the records decoded. */
    CHECK_EQ(out[2].flags, 0xA5);
}

static void t_parse_size_query(void) {
    build_7zip_table();
    /* out == NULL asks for the count without writing anywhere. */
    CHECK_EQ(w32_accel_parse(zip7_table, sizeof zip7_table, NULL, 0), 2);
}

/* ---- malformed input is refused, not partially trusted ---------------- */

static void t_parse_rejects_malformed(void) {
    build_7zip_table();
    W32_ACCEL out[8];

    CHECK_EQ(w32_accel_parse(NULL, 16, out, 8), -1);
    CHECK_EQ(w32_accel_parse(zip7_table, 0, out, 8), -1);
    /* 4 and 12 bytes are not a whole number of 8-byte records.  Accepting
     * them would read a half record, i.e. garbage bytes off the end. */
    CHECK_EQ(w32_accel_parse(zip7_table, 4, out, 8), -1);
    CHECK_EQ(w32_accel_parse(zip7_table, 12, out, 8), -1);
}

static void t_parse_rejects_reserved_flags(void) {
    build_7zip_table();
    W32_ACCEL out[8];

    uint8_t bad[16];
    memcpy(bad, zip7_table, sizeof bad);
    /* 0x0020 is not a documented F* bit.  A table carrying it is not a
     * table this decoder understands, so the whole resource is refused
     * rather than the bit being silently dropped. */
    bad[0] = (uint8_t)((W32_FVIRTKEY | W32_FNOINVERT) | 0x20u);
    CHECK_EQ(w32_accel_parse(bad, sizeof bad, out, 8), -1);
}

static void t_parse_rejects_oversized_cmd(void) {
    build_7zip_table();
    W32_ACCEL out[8];

    uint8_t bad[16];
    memcpy(bad, zip7_table, sizeof bad);
    /* WM_COMMAND ids are a WORD; a cmd above 0xFFFF means the field was
     * misread, so the layout is wrong. */
    bad[6] = 0x01u;                     /* cmd = 0x000103C0 */
    CHECK_EQ(w32_accel_parse(bad, sizeof bad, out, 8), -1);
}

static void t_parse_reports_small_buffer(void) {
    build_7zip_table();
    W32_ACCEL out[1];

    /* Well-formed, but two records do not fit in one.  -2 keeps this
     * distinct from "malformed" (-1), so a caller can grow and retry. */
    CHECK_EQ(w32_accel_parse(zip7_table, sizeof zip7_table, out, 1), -2);
    /* cap exactly equal to the record count succeeds. */
    W32_ACCEL out2[2];
    CHECK_EQ(w32_accel_parse(zip7_table, sizeof zip7_table, out2, 2), 2);
}

/* Every truncation of the real blob that is not a whole record is refused,
 * and none of them crashes.  A resource can be truncated by a bad disk read;
 * the decoder must not over-read past the bytes it was given.
 *
 * Note the one length that *is* accepted: 8 bytes is exactly one record, so
 * it decodes as a valid single-entry table rather than being an error.  The
 * first pass at this test called that a failure; it is correct behaviour and
 * is now asserted as such. */
static void t_parse_truncations(void) {
    build_7zip_table();
    W32_ACCEL out[8];

    int refused = 0, accepted = 0;
    for (size_t len = 1; len < sizeof zip7_table; len++) {
        if (w32_accel_parse(zip7_table, len, out, 8) == -1) refused++;
        else accepted++;
    }
    /* 15 candidate lengths; only len == 8 is a whole record. */
    CHECK_EQ(refused, 14);
    CHECK_EQ(accepted, 1);

    /* That one accepted truncation decodes as entry 0 and nothing more. */
    W32_ACCEL one[8];
    memset(one, 0xA5, sizeof one);
    int n = w32_accel_parse(zip7_table, 8, one, 8);
    CHECK_EQ(n, 1);
    if (n != 1) return;
    CHECK_EQ(one[0].flags, (long)(W32_FVIRTKEY | W32_FNOINVERT));
    CHECK_EQ(one[0].key, 0x70);
    CHECK_EQ(one[0].cmd, ACC_F1_CMD);
    CHECK_EQ(one[1].flags, 0xA5);       /* second record was not touched */
}

/* ---- matching: modifiers must match exactly --------------------------- */

static W32_ACCEL mk(uint8_t flags, uint16_t key, uint16_t cmd) {
    W32_ACCEL a; a.flags = flags; a.key = key; a.cmd = cmd; return a;
}

static void t_match_plain_vkey(void) {
    W32_ACCEL f1 = mk((uint8_t)(W32_FVIRTKEY | W32_FNOINVERT), 0x70, ACC_F1_CMD);

    CHECK_EQ(w32_accel_match(&f1, 0x70, 0, 0, 0), 1);   /* F1 alone fires */
    CHECK_EQ(w32_accel_match(&f1, 0x71, 0, 0, 0), 0);   /* F2 does not */
    /* Negative control: the pre-fix TranslateAcceleratorW compared only the
     * key, so it would answer 1 to all three of these. */
    CHECK_EQ(w32_accel_match(&f1, 0x70, 1, 0, 0), 0);   /* Shift+F1 */
    CHECK_EQ(w32_accel_match(&f1, 0x70, 0, 1, 0), 0);   /* Ctrl+F1 */
    CHECK_EQ(w32_accel_match(&f1, 0x70, 0, 0, 1), 0);   /* Alt+F1 */
    CHECK_EQ(w32_accel_match(NULL, 0x70, 0, 0, 0), 0);  /* no entry, no crash */
}

static void t_match_alt_vkey(void) {
    W32_ACCEL altf12 = mk((uint8_t)(W32_FVIRTKEY | W32_FNOINVERT | W32_FALT),
                          0x7b, ACC_F12_CMD);

    CHECK_EQ(w32_accel_match(&altf12, 0x7b, 0, 0, 1), 1);  /* Alt+F12 fires */
    /* This is the concrete regression: a bare F12 must NOT fire an FALT
     * entry.  The old code posted WM_COMMAND 736 for it. */
    CHECK_EQ(w32_accel_match(&altf12, 0x7b, 0, 0, 0), 0);
    CHECK_EQ(w32_accel_match(&altf12, 0x7b, 1, 0, 1), 0);  /* Shift+Alt+F12 */
    CHECK_EQ(w32_accel_match(&altf12, 0x7b, 0, 1, 1), 0);  /* Ctrl+Alt+F12 */
    CHECK_EQ(w32_accel_match(&altf12, 0x7a, 0, 0, 1), 0);  /* Alt+F11 */
}

static void t_match_character_entry(void) {
    /* A non-FVIRTKEY entry stores an uppercase ANSI character. */
    W32_ACCEL ctlc = mk((uint8_t)(W32_FCONTROL | W32_FNOINVERT), 'C', 900u);

    CHECK_EQ(w32_accel_match(&ctlc, 'c', 0, 1, 0), 1);  /* lowercase typed */
    CHECK_EQ(w32_accel_match(&ctlc, 'C', 0, 1, 0), 1);  /* uppercase typed */
    CHECK_EQ(w32_accel_match(&ctlc, 'c', 0, 0, 0), 0);  /* bare 'c' */
    CHECK_EQ(w32_accel_match(&ctlc, 'x', 0, 1, 0), 0);  /* Ctrl+X */
}

static void t_match_table_dispatch(void) {
    /* The two 7-Zip entries together: F1 and Alt+F12 must resolve to
     * different command ids, and no bare chord may resolve to 736. */
    build_7zip_table();
    W32_ACCEL t[8];
    int n = w32_accel_parse(zip7_table, sizeof zip7_table, t, 8);
    CHECK_EQ(n, 2);
    if (n != 2) return;

    int hit_f1 = -1, hit_f12 = -1, hit_bare_f12 = -1;
    for (int i = 0; i < n; i++) {
        if (w32_accel_match(&t[i], 0x70, 0, 0, 0)) hit_f1 = t[i].cmd;
        if (w32_accel_match(&t[i], 0x7b, 0, 0, 1)) hit_f12 = t[i].cmd;
        if (w32_accel_match(&t[i], 0x7b, 0, 0, 0)) hit_bare_f12 = t[i].cmd;
    }
    CHECK_EQ(hit_f1, (long)ACC_F1_CMD);
    CHECK_EQ(hit_f12, (long)ACC_F12_CMD);
    CHECK_EQ(hit_bare_f12, -1);         /* nothing fires on a bare F12 */
}

int main(void) {
    printf("== w32 RT_ACCELERATOR decoder (WR-2) ==\n");
    RUN(t_parse_7zip_table);
    RUN(t_parse_size_query);
    RUN(t_parse_rejects_malformed);
    RUN(t_parse_rejects_reserved_flags);
    RUN(t_parse_rejects_oversized_cmd);
    RUN(t_parse_reports_small_buffer);
    RUN(t_parse_truncations);
    RUN(t_match_plain_vkey);
    RUN(t_match_alt_vkey);
    RUN(t_match_character_entry);
    RUN(t_match_table_dispatch);

    printf("%d/%d checks passed, %d failed\n", passed, tn, failed);
    if (failed) { printf("w32 accel: FAIL\n"); return 1; }
    printf("w32 accel: PASS\n");
    return 0;
}
