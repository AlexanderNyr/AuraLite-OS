/* tests/unit/test_ao_sheet.c — OFFICE_PLAN OF4 sheet gate.
 *
 * Edit -> recalc correctness (a 1 000-cell dependent chain), deletion,
 * the practical cell cap, sheet copy/rename, the .aosheet round trip,
 * and CSV round trips against hostile input (quotes, CRLF, embedded
 * newlines, a 1 MiB field).  Allocation is the arena's: freed by reset,
 * counted by ao_arena_used(), printed at the end.
 */
#include "aocalc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t mem[64 * 1024 * 1024];
static ao_arena_t arena;
static ao_book_t *book;
static int total, fails;

static void check(const char *name, int cond) {
    total++;
    if (!cond) { printf("AO SHEET FAIL: %s\n", name); fails++; }
}
static double getn(ao_sheet_t *s, uint32_t r, uint32_t c) {
    ao_value_t v = ao_sheet_get(book, s, r, c);
    return v.kind == AO_VAL_NUM ? v.num : -1e99;
}

typedef struct { char *buf; size_t len, cap; } sink_t;
static void sink_emit(void *ctx, const char *data, size_t n) {
    sink_t *sk = ctx;
    if (sk->len + n + 1 > sk->cap) {
        sk->cap = (sk->len + n + 1) * 2;
        sk->buf = realloc(sk->buf, sk->cap);
    }
    memcpy(sk->buf + sk->len, data, n);
    sk->len += n;
    sk->buf[sk->len] = 0;
}

int main(void) {
    ao_arena_init(&arena, mem, sizeof mem);
    book = ao_book_new(&arena);
    if (!book) { printf("AO SHEET: FAIL (no book)\n"); return 1; }
    ao_sheet_t *s = ao_book_sheet(book, "Main", 1);

    /* Basic set/get, literals and a formula. */
    ao_sheet_set(book, s, 0, 0, "21");
    ao_sheet_set(book, s, 0, 1, "=A1*2");
    check("formula", getn(s, 0, 1) == 42);
    ao_sheet_set(book, s, 0, 0, "100");
    check("edit recalc", getn(s, 0, 1) == 200);
    check("blank reads blank", ao_sheet_get(book, s, 9, 9).kind == AO_VAL_BLANK);
    ao_sheet_set(book, s, 1, 0, "hello");
    ao_value_t v = ao_sheet_get(book, s, 1, 0);
    check("text literal", v.kind == AO_VAL_TEXT && strcmp(v.text, "hello") == 0);
    ao_sheet_set(book, s, 2, 0, "TRUE");
    v = ao_sheet_get(book, s, 2, 0);
    check("bool literal", v.kind == AO_VAL_BOOL && v.num == 1);

    /* A chain of 1 000 dependents: edit the head, the tail must follow. */
    ao_sheet_t *ch = ao_book_sheet(book, "Chain", 1);
    ao_sheet_set(book, ch, 0, 0, "1");
    for (int i = 1; i < 1000; i++) {
        char f[32];
        snprintf(f, sizeof f, "=A%d+1", i);
        ao_sheet_set(book, ch, (uint32_t)i, 0, f);
    }
    ao_book_recalc(book);
    check("chain tail", getn(ch, 999, 0) == 1000);
    ao_sheet_set(book, ch, 0, 0, "41");
    ao_book_recalc(book);
    check("chain re-edit", getn(ch, 999, 0) == 1040);

    /* Cross-sheet references and ranges. */
    ao_sheet_t *o = ao_book_sheet(book, "Other", 1);
    ao_sheet_set(book, o, 0, 0, "=Chain!A1000");
    ao_sheet_set(book, o, 0, 1, "=SUM(Chain!A1:A10)");
    check("cross-sheet", getn(o, 0, 0) == 1040);
    check("cross-range", getn(o, 0, 1) == 410 + 45);   /* 41..50: (41+50)*10/2 */

    /* Deletion: the cell goes away, dependents fall back to blank=0. */
    ao_sheet_set(book, s, 5, 5, "7");
    ao_sheet_set(book, s, 5, 6, "=F6+1");
    check("pre-delete", getn(s, 5, 6) == 8);
    ao_sheet_set(book, s, 5, 5, "");
    check("deleted is blank", ao_sheet_get(book, s, 5, 5).kind == AO_VAL_BLANK);
    check("dependent after delete", getn(s, 5, 6) == 1);
    check("count after delete", ao_sheet_cell_count(s) == 5);

    /* Sheet copy + rename: the copy computes on its own. */
    check("copy rc", ao_book_sheet_copy(book, "Main", "Copy") == AO_CALC_OK);
    ao_sheet_t *cp = ao_book_sheet(book, "Copy", 0);
    check("copy exists", cp != NULL);
    if (cp) {
        ao_sheet_set(book, cp, 0, 0, "3");
        check("copy independent", getn(cp, 0, 1) == 6 && getn(s, 0, 1) == 200);
    }
    check("rename rc", ao_book_sheet_rename(book, "Copy", "Renamed") == AO_CALC_OK);
    check("renamed lookup", ao_book_sheet(book, "Renamed", 0) != NULL);
    check("old name gone", ao_book_sheet(book, "Copy", 0) == NULL);

    /* .aosheet round trip: save, load into a fresh book, same values. */
    sink_t sk = { NULL, 0, 0 };
    check("save rc", ao_book_save(book, sink_emit, &sk) == AO_CALC_OK);
    check("save has formula", sk.buf && strstr(sk.buf, "=A1*2") != NULL);
    check("save has sheets", sk.buf && strstr(sk.buf, "[Main]") != NULL);
    static uint8_t mem2[8 * 1024 * 1024];
    ao_arena_t ar2;
    ao_arena_init(&ar2, mem2, sizeof mem2);
    ao_book_t *b2 = ao_book_new(&ar2);
    check("load rc", ao_book_load(b2, sk.buf, sk.len) == AO_CALC_OK);
    ao_sheet_t *m2 = ao_book_sheet(b2, "Main", 0);
    check("loaded sheet", m2 != NULL);
    if (m2) {
        ao_value_t w = ao_sheet_get(b2, m2, 0, 1);
        check("loaded value", w.kind == AO_VAL_NUM && w.num == 200);
    }

    /* CSV: hostile round trip. */
    static char csv_in[1500];
    snprintf(csv_in, sizeof csv_in,
             "a,\"b, with comma\",\"say \"\"hi\"\"\",42\r\n"
             "\"line one\nline two\",plain,=1+2,3.5\n"
             "x;y;z\n");
    ao_sheet_t *cs = ao_book_sheet(book, "Csv", 1);
    check("csv import", ao_csv_import(book, cs, csv_in, strlen(csv_in), 1, ',') == AO_CALC_OK);
    v = ao_sheet_get(book, cs, 0, 1);
    check("csv quoted comma", v.kind == AO_VAL_TEXT && strcmp(v.text, "b, with comma") == 0);
    v = ao_sheet_get(book, cs, 0, 2);
    check("csv escaped quotes", v.kind == AO_VAL_TEXT && strcmp(v.text, "say \"hi\"") == 0);
    v = ao_sheet_get(book, cs, 1, 0);
    check("csv embedded newline", v.kind == AO_VAL_TEXT && strcmp(v.text, "line one\nline two") == 0);
    check("csv formula field", getn(cs, 1, 2) == 3);
    sink_t sk2 = { NULL, 0, 0 };
    check("csv export", ao_csv_export(book, cs, ',', sink_emit, &sk2) == AO_CALC_OK);
    /* Export into a fresh book must reproduce the values. */
    static uint8_t mem3[8 * 1024 * 1024];
    ao_arena_t ar3;
    ao_arena_init(&ar3, mem3, sizeof mem3);
    ao_book_t *b3 = ao_book_new(&ar3);
    ao_sheet_t *cs3 = ao_book_sheet(b3, "Csv", 1);
    check("csv re-import", ao_csv_import(b3, cs3, sk2.buf, sk2.len, 1, ',') == AO_CALC_OK);
    ao_value_t w = ao_sheet_get(b3, cs3, 0, 1);
    check("csv round trip", w.kind == AO_VAL_TEXT && strcmp(w.text, "b, with comma") == 0);

    /* A 1 MiB field: bounded only by the arena, not the parser. */
    size_t big_len = 1024 * 1024;
    char *bigcsv = malloc(big_len + 64);
    char *want = malloc(big_len + 1);
    memset(want, 'x', big_len);
    want[big_len] = 0;
    snprintf(bigcsv, big_len + 64, "\"%s\",1\n", want);
    ao_sheet_t *bs = ao_book_sheet(book, "Big", 1);
    check("csv 1MiB import", ao_csv_import(book, bs, bigcsv, strlen(bigcsv), 0, ',') == AO_CALC_OK);
    v = ao_sheet_get(book, bs, 0, 0);
    check("csv 1MiB field", v.kind == AO_VAL_TEXT && v.text &&
          strlen(v.text) == big_len && memcmp(v.text, want, big_len) == 0);
    free(bigcsv); free(want);

    /* Tab-sniffed import. */
    ao_sheet_t *ts = ao_book_sheet(book, "Tabs", 1);
    const char *tsv = "1\t2\t3\n4\t5\t6\n";
    check("tsv import", ao_csv_import(book, ts, tsv, strlen(tsv), 1, ',') == AO_CALC_OK);
    check("tsv value", getn(ts, 1, 2) == 6);

    /* Number formatting. */
    char fb[128];
    ao_format_value(book, ao_val_num(1234567.891), "#,##0.00", fb, sizeof fb);
    check("fmt thousands", strcmp(fb, "1,234,567.89") == 0);
    ao_format_value(book, ao_val_num(0.125), "0.0%", fb, sizeof fb);
    check("fmt percent", strcmp(fb, "12.5%") == 0);
    ao_format_value(book, ao_val_num(1234.5), "$#,##0.00", fb, sizeof fb);
    check("fmt currency", strcmp(fb, "$1,234.50") == 0);
    ao_format_value(book, ao_val_num(45000), "yyyy-mm-dd", fb, sizeof fb);
    check("fmt date", strcmp(fb, "2023-03-15") == 0);
    ao_format_value(book, ao_val_num(42), "0", fb, sizeof fb);
    check("fmt fixed", strcmp(fb, "42") == 0);

    /* References: parse/format are inverse on the corners of the grid. */
    uint32_t r, c;
    check("ref parse", ao_ref_parse("XFD1048576", &r, &c, NULL, NULL) == 0 &&
          r == 1048575 && c == 16383);
    ao_ref_name(1048575, 16383, fb, sizeof fb);
    check("ref format", strcmp(fb, "XFD1048576") == 0);
    check("ref rejects", ao_ref_parse("A0", &r, &c, NULL, NULL) != 0 &&
          ao_ref_parse("1A", &r, &c, NULL, NULL) != 0);

    /* The recalculation budget refuses a pathological graph. */
    ao_sheet_t *pb = ao_book_sheet(book, "Path", 1);
    ao_sheet_set(book, pb, 0, 0, "1");
    for (int i = 1; i < 200; i++) {
        char f[48];
        snprintf(f, sizeof f, "=SUM($A$1:A%d)", i);
        ao_sheet_set(book, pb, (uint32_t)i, 0, f);
    }
    ao_book_set_budget(book, 50);   /* far below what this needs */
    int rc = ao_book_recalc(book);
    check("budget refuses", rc == AO_CALC_ERR_BUDGET);
    ao_book_set_budget(book, AO_RECALC_BUDGET);
    check("budget restores", ao_book_recalc(book) == AO_CALC_OK);

    /* The practical cap refuses a 65 537th distinct cell. */
    ao_sheet_t *cap = ao_book_sheet(book, "Cap", 1);
    char num[16];
    int cap_hit = 0;
    for (uint32_t i = 0; i < AO_SHEET_MAX_CELLS + 5; i++) {
        snprintf(num, sizeof num, "%u", i);
        int rc2 = ao_sheet_set(book, cap, i / 256, i % 256, num);
        if (rc2 == AO_CALC_ERR_CAP) { cap_hit = 1; break; }
        if (rc2 != AO_CALC_OK) break;
    }
    check("cell cap refuses", cap_hit);
    check("cell cap exact", ao_sheet_cell_count(cap) == AO_SHEET_MAX_CELLS);

    /* Sorted iteration is row-major. */
    ao_sheet_set(book, s, 7, 0, "late");
    ao_cell_t *first = ao_sheet_cell_sorted(book, s, 0);
    uint32_t fr, fc;
    ao_cell_pos(first, &fr, &fc);
    check("sorted first", fr == 0 && fc == 0);

    free(sk.buf); free(sk2.buf);
    printf("AO SHEET: %s (%d checks, arena %zu bytes)\n",
           fails ? "FAIL" : "PASS", total, ao_arena_used(&arena));
    return fails ? 1 : 0;
}
