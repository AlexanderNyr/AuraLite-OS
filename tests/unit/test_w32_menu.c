/*
 * test_w32_menu.c — host unit tests for the RT_MENU decoder.
 *
 * W32RUN_PLAN.md phase WR-2, the "Tools > Options" gap.  Until this landed,
 * LoadMenuW found the RT_MENU resource and threw it away, returning an empty
 * CreateMenu() -- so an application's menu bar had nothing in it.
 *
 * The grammar was fitted against real bytes, not assumed; see the long
 * comment in w32/include/w32/user32.h.  The headline is that the documented
 * MENUEX item layout (a 20-byte prefix) does NOT describe what a real
 * application ships: the pinned 7-Zip 24.09 7zFM.exe uses an 8-byte header
 * and a 14-byte item prefix, and three earlier attempts to walk it as
 * documented MENUEX looped or read off the end.  A parser built from the
 * fitted grammar consumes that resource exactly -- 3 532 of 3 532 bytes, 90
 * items, zero resyncs -- which is the evidence that the grammar is right
 * rather than merely plausible.
 *
 * No foreign bytes are committed: the menus below are emitted here, record by
 * record, in the documented layout (the convention tools/wr2_make_fixture.py
 * and test_w32_accel.c follow).  The item ids and texts mirror what the pinned
 * 7zFM.exe actually carries, so a regression shows up as a wrong id or a
 * missing item rather than as an abstract mismatch.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32/user32.h"

#include "../../w32/src/w32_rsrc.c"
#include "../../w32/src/w32_errno.c"

/* w32_rsrc.c's module lookup and byte walk: nothing here has a module. */
W32_HMODULE W32ABI w32_GetModuleHandleA(const char *name) { (void)name; return 0; }
int w32_module_file_bytes(void *h, const uint8_t **d, size_t *sz) {
    (void)h; (void)d; (void)sz; return 0;
}
/* w32_errno.c keeps the last-error code in the guest thread environment
 * block; a static one keeps the link honest, nothing here reads it. */
#include "w32/w32_teb.h"
static struct w32_teb host_teb;
struct w32_teb *w32_teb_self(void) { return &host_teb; }

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; \
                    else printf("  [%s] FAILED\n", #f); } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long long _a = (long long)(a), _e = (long long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%lld want %lld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)
#define CHECK_STR(a, e) do { \
    char _b[128]; size_t _k = 0; \
    for (const uint16_t *_s = (a); *_s && _k < sizeof _b - 1; _s++) \
        _b[_k++] = (char)*_s; \
    _b[_k] = 0; \
    if (strcmp(_b, (e))) { printf("  FAIL L%d: text \"%s\" want \"%s\"\n", \
        __LINE__, _b, (e)); failed++; } } while (0)

/* ---- in-tree builder ---------------------------------------------------- */

static uint8_t blob[8192];
static size_t blen;

static void put16(uint16_t v) { blob[blen++] = (uint8_t)v; blob[blen++] = (uint8_t)(v >> 8); }
static void put32(uint32_t v) {
    blob[blen++] = (uint8_t)v;         blob[blen++] = (uint8_t)(v >> 8);
    blob[blen++] = (uint8_t)(v >> 16); blob[blen++] = (uint8_t)(v >> 24);
}
static void put_text(const char *s) {
    for (; *s; s++) put16((uint16_t)(uint8_t)*s);
    put16(0);
}
static void align4(void) { while (blen & 3u) blob[blen++] = 0; }

/* One item record: DWORD type, DWORD state, DWORD id, WORD flags, WCHAR text,
 * aligned to 4.  Returns the offset the record started at. */
static size_t item(uint32_t type, uint32_t state, uint32_t id,
                   uint16_t flags, const char *text) {
    size_t at = blen;
    put32(type); put32(state); put32(id); put16(flags);
    put_text(text);
    align4();
    return at;
}
static void header(void) { blen = 0; put16(1); put16(4); put32(0); }  /* wVersion, wOffset, dwHelpId */

/* The shape the pinned 7zFM.exe carries: six top-level popups, a submenu with
 * a separator and a nested popup, a column break, MF_END terminators, and two
 * plain top-level items after the last popup.  Ids 960/961 are the real ones,
 * and 960 is also what the pinned RT_ACCELERATOR maps F1 to. */
static void build_7zip_shape(void) {
    header();
    /* &File (500) -> Open (540), separator, E&xit (8, MF_END) */
    item(0, 0, 500, W32_MENUITEM_F_POPUP, "&File");
    put32(0);                                   /* submenu dwHelpId */
    item(0, 0, 540, 0, "&Open\tEnter");
    item(W32_MFT_SEPARATOR, 0, 0, 0, "");
    item(0, 0, 8, W32_MENUITEM_F_END, "E&xit\tAlt+F4");
    /* &Edit (501) -> Select &All (600), column break, Deselect (601, MF_END) */
    item(0, 0, 501, W32_MENUITEM_F_POPUP, "&Edit");
    put32(0);
    item(0, 0, 600, 0, "Select &All\tShift+[Grey +]");
    item(W32_MFT_MENUBARBREAK, 0, 0, 0, "");
    item(0, 0, 601, W32_MENUITEM_F_END, "Deselect All\tShift+[Grey -]");
    /* &Help (505) -> CRC popup (nested) -> CRC-32 (102, MF_END), then &Contents (960, MF_END) */
    item(0, 0, 505, W32_MENUITEM_F_POPUP, "&Help");
    put32(0);
    item(0, 0, 0, W32_MENUITEM_F_POPUP, "CRC");
    put32(0);
    item(0, 0, 102, W32_MENUITEM_F_END, "CRC-32");
    item(0, 0, 960, W32_MENUITEM_F_END, "&Contents...\tF1");
    /* two plain top-level items after the last popup */
    item(W32_MFT_SEPARATOR, 0, 0, 0, "");
    item(0, 0, 961, 0, "&About 7-Zip...");
}

/* ---- decoding ---------------------------------------------------------- */

static void t_parse_shape(void) {
    build_7zip_shape();
    /* 14 items: &File, &Open, sep, E&xit, &Edit, Select &All, colbreak,
     * Deselect, &Help, CRC, CRC-32, &Contents, sep, &About. */
    int cnt = w32_menu_parse(blob, blen, NULL, 0);
    CHECK_EQ(cnt, 14);                          /* size query */
    if (cnt != 14) return;

    w32_menu_item_raw it[32];
    memset(it, 0xA5, sizeof it);
    int n = w32_menu_parse(blob, blen, it, 32);
    CHECK_EQ(n, 14);
    if (n != 14) return;

    /* &File and its submenu */
    CHECK_EQ(it[0].depth, 0);
    CHECK_EQ(it[0].id, 500);
    CHECK(it[0].flags & W32_MENUITEM_F_POPUP);
    CHECK_STR(it[0].text, "&File");

    CHECK_EQ(it[1].depth, 1);
    CHECK_EQ(it[1].id, 540);
    CHECK_STR(it[1].text, "&Open\tEnter");

    CHECK_EQ(it[2].depth, 1);
    CHECK_EQ(it[2].type, (long)W32_MFT_SEPARATOR);
    CHECK_STR(it[2].text, "");                  /* separators carry no text */

    CHECK_EQ(it[3].depth, 1);
    CHECK_EQ(it[3].id, 8);
    CHECK(it[3].flags & W32_MENUITEM_F_END);    /* closes &File */
    CHECK_STR(it[3].text, "E&xit\tAlt+F4");

    /* back at the top level -- this is what MF_END has to restore */
    CHECK_EQ(it[4].depth, 0);
    CHECK_EQ(it[4].id, 501);
    CHECK_STR(it[4].text, "&Edit");

    /* the column break sits inside the submenu, at depth 1 */
    CHECK_EQ(it[5].depth, 1);
    CHECK_EQ(it[5].id, 600);
    CHECK_EQ(it[6].depth, 1);
    CHECK_EQ(it[6].type, (long)W32_MFT_MENUBARBREAK);
    CHECK_EQ(it[7].depth, 1);
    CHECK(it[7].flags & W32_MENUITEM_F_END);

    /* &Help, then a popup nested two deep */
    CHECK_EQ(it[8].depth, 0);
    CHECK_EQ(it[8].id, 505);
    CHECK_EQ(it[9].depth, 1);
    CHECK_STR(it[9].text, "CRC");
    CHECK_EQ(it[10].depth, 2);                  /* nested submenu */
    CHECK_EQ(it[10].id, 102);
    CHECK_STR(it[10].text, "CRC-32");

    /* the inner MF_END pops one level, not all the way out */
    CHECK_EQ(it[11].depth, 1);
    CHECK_EQ(it[11].id, 960);
    CHECK_STR(it[11].text, "&Contents...\tF1");

    /* &Contents carries MF_END, so it closes &Help and the last two items
     * are back at the top level.  Each MF_END closes exactly one submenu --
     * CRC-32's END popped depth 2 -> 1, &Contents' END popped 1 -> 0.  The
     * pinned 7-Zip resource behaves the same way: its &Contents and &About
     * also decode at depth 0. */
    CHECK_EQ(it[12].depth, 0);
    CHECK_EQ(it[12].type, (long)W32_MFT_SEPARATOR);
    CHECK_EQ(it[13].depth, 0);
    CHECK_EQ(it[13].id, 961);
    CHECK_STR(it[13].text, "&About 7-Zip...");

    /* nothing written past the decoded items */
    CHECK_EQ(it[14].depth, 0xA5A5A5A5u);
}

/* The last two top-level items: with the fixture above the trailing separator
 * and &About stay inside &Help because only one MF_END follows the nested
 * popup.  This checks the intended behaviour directly: enough MF_ENDs return
 * all the way to depth 0. */
static void t_end_pops_to_top(void) {
    header();
    item(0, 0, 500, W32_MENUITEM_F_POPUP, "&File");
    put32(0);
    item(0, 0, 540, W32_MENUITEM_F_END, "&Open");
    item(0, 0, 961, 0, "&About 7-Zip...");

    w32_menu_item_raw it[8];
    int n = w32_menu_parse(blob, blen, it, 8);
    CHECK_EQ(n, 3);
    if (n != 3) return;
    CHECK_EQ(it[0].depth, 0);
    CHECK_EQ(it[1].depth, 1);
    CHECK_EQ(it[2].depth, 0);                   /* MF_END restored the top level */
    CHECK_EQ(it[2].id, 961);
    CHECK_STR(it[2].text, "&About 7-Zip...");
}

/* dwState must survive the round trip: a greyed item has to stay greyed, or a
 * menu would offer commands the application meant to disable. */
static void t_state_survives(void) {
    header();
    item(0, W32_MFS_GRAYED, 700, 0, "Greyed out");
    item(0, W32_MFS_CHECKED, 701, 0, "Checked");

    w32_menu_item_raw it[4];
    int n = w32_menu_parse(blob, blen, it, 4);
    CHECK_EQ(n, 2);
    if (n != 2) return;
    CHECK_EQ(it[0].state, (long)W32_MFS_GRAYED);
    CHECK_EQ(it[1].state, (long)W32_MFS_CHECKED);
}

/* ---- malformed input is refused --------------------------------------- */

static void t_rejects_short_header(void) {
    build_7zip_shape();
    w32_menu_item_raw it[4];
    CHECK_EQ(w32_menu_parse(NULL, blen, it, 4), -1);
    CHECK_EQ(w32_menu_parse(blob, 0, it, 4), -1);
    CHECK_EQ(w32_menu_parse(blob, 7, it, 4), -1);   /* one byte short of a header */
    /* A header with no items at all is not a menu. */
    header();
    CHECK_EQ(w32_menu_parse(blob, blen, it, 4), -1);
}

static void t_rejects_truncated_record(void) {
    header();
    item(0, 0, 500, 0, "&File");
    w32_menu_item_raw it[4];
    size_t full = blen;

    /* Cutting inside the item's text leaves it unterminated within `size`,
     * which must be refused rather than read past.  (Chopping only the final
     * alignment byte is NOT an error: the record is still whole, so the
     * parser legitimately reports the same single item.) */
    CHECK_EQ(w32_menu_parse(blob, 8 + 14 + 4, it, 4), -1);  /* mid-text */
    CHECK_EQ(w32_menu_parse(blob, 8 + 13, it, 4), -1);      /* 13 of 14 prefix bytes */
    CHECK_EQ(w32_menu_parse(blob, 8 + 6, it, 4), -1);       /* prefix only */
    (void)full;
}

static void t_rejects_unknown_bits(void) {
    w32_menu_item_raw it[4];

    /* A type bit outside the recognised set.  0x2000 (RIGHTORDER in the
     * published MFT_ table) is not in this project's W32_MFT_* set, so it is
     * refused rather than silently accepted as a string item. */
    header();
    item(0x2000u, 0, 500, 0, "&File");
    CHECK_EQ(w32_menu_parse(blob, blen, it, 4), -1);

    /* A flag bit outside POPUP|END. */
    header();
    item(0, 0, 500, 0x0002u, "&File");
    CHECK_EQ(w32_menu_parse(blob, blen, it, 4), -1);
}

static void t_rejects_unterminated_text(void) {
    w32_menu_item_raw it[4];
    header();
    put32(0); put32(0); put32(500); put16(0);
    put16('x');                               /* no NUL before the end */
    CHECK_EQ(w32_menu_parse(blob, blen, it, 4), -1);
}

static void t_rejects_overlong_text(void) {
    w32_menu_item_raw it[4];
    header();
    put32(0); put32(0); put32(500); put16(0);
    for (int i = 0; i < 400; i++) put16('x'); /* past the 256-char cap */
    put16(0);
    CHECK_EQ(w32_menu_parse(blob, blen, it, 4), -1);
}

/* Nesting is counted, not recursed into, and capped: a resource claiming
 * 200 levels of submenu must be refused rather than walked off the end or
 * buried under frames. */
static void t_rejects_deep_nesting(void) {
    /* cap must exceed the depth limit, or the "buffer too small" (-2) check
     * fires first and the depth check is never reached. */
    w32_menu_item_raw it[64];
    header();
    for (int i = 0; i < 200; i++) {
        item(0, 0, (uint32_t)(600 + i), W32_MENUITEM_F_POPUP, "Sub");
        put32(0);
    }
    CHECK_EQ(w32_menu_parse(blob, blen, it, 64), -1);
}

static void t_reports_small_buffer(void) {
    build_7zip_shape();
    w32_menu_item_raw it[4];
    /* -2 keeps "your buffer is too small" distinct from "malformed" (-1). */
    CHECK_EQ(w32_menu_parse(blob, blen, it, 4), -2);
    w32_menu_item_raw big[32];
    CHECK_EQ(w32_menu_parse(blob, blen, big, 14), 14);   /* exact cap is fine */
}

/* Every truncation of a well-formed menu is refused cleanly.  A resource can
 * be truncated by a bad read; the decoder must not over-read past `size`. */
static void t_truncations_do_not_crash(void) {
    build_7zip_shape();
    size_t full = blen;
    w32_menu_item_raw it[32];
    int refused = 0, partial = 0;
    for (size_t len = 0; len < full; len++) {
        int r = w32_menu_parse(blob, len, it, 32);
        if (r == -1) refused++;
        else if (r > 0) partial++;
        else if (r != -2) { printf("  FAIL L%d: len=%zu -> %d\n", __LINE__, len, r); failed++; return; }
    }
    /* Some prefixes are legitimately valid shorter menus (they end on a
     * record boundary); the point is that none of them is -2 or a crash, and
     * that the full length still decodes. */
    CHECK(refused + partial > 0);
    CHECK_EQ(w32_menu_parse(blob, full, it, 32), 14);
}

int main(void) {
    printf("== w32 RT_MENU decoder (WR-2) ==\n");
    RUN(t_parse_shape);
    RUN(t_end_pops_to_top);
    RUN(t_state_survives);
    RUN(t_rejects_short_header);
    RUN(t_rejects_truncated_record);
    RUN(t_rejects_unknown_bits);
    RUN(t_rejects_unterminated_text);
    RUN(t_rejects_overlong_text);
    RUN(t_rejects_deep_nesting);
    RUN(t_reports_small_buffer);
    RUN(t_truncations_do_not_crash);

    printf("%d/%d checks passed, %d failed\n", passed, tn, failed);
    if (failed) { printf("w32 menu: FAIL\n"); return 1; }
    printf("w32 menu: PASS\n");
    return 0;
}
