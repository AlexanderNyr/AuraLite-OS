/* tests/unit/test_ao_formula.c — OFFICE_PLAN OF4 formula gate.
 *
 * Expression -> value for every operator and every function the plan
 * names, graded against libaodoc/aocalc.c (the shipping source).
 * Precedence, string coercion, error propagation, a 1 000-cell SUM and
 * the self-referential cell that must read #CYCLE!, never hang.
 */
#include "aocalc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t mem[8 * 1024 * 1024];
static ao_arena_t arena;
static ao_book_t *book;
static ao_sheet_t *sh;
static int total, fails;

static ao_value_t ev(const char *f) {
    ao_value_t v;
    if (ao_formula_eval(book, sh, f, &v) != AO_CALC_OK && v.kind != AO_VAL_ERR) {
        v = ao_val_err(AO_ERR_VALUE);
    }
    return v;
}
static void expect_num(const char *name, const char *f, double want) {
    total++;
    ao_value_t v = ev(f);
    if (v.kind != AO_VAL_NUM || fabs(v.num - want) > 1e-9) {
        char b[128];
        ao_val_to_text(book, v, b, sizeof b);
        printf("AO FORMULA FAIL: %s: %s -> %s, want %.12g\n", name, f, b, want);
        fails++;
    }
}
static void expect_text(const char *name, const char *f, const char *want) {
    total++;
    ao_value_t v = ev(f);
    if (v.kind != AO_VAL_TEXT || !v.text || strcmp(v.text, want) != 0) {
        char b[128];
        ao_val_to_text(book, v, b, sizeof b);
        printf("AO FORMULA FAIL: %s: %s -> %s, want text \"%s\"\n", name, f, b, want);
        fails++;
    }
}
static void expect_err(const char *name, const char *f, ao_err_t want) {
    total++;
    ao_value_t v = ev(f);
    if (v.kind != AO_VAL_ERR || v.err != want) {
        char b[128];
        ao_val_to_text(book, v, b, sizeof b);
        printf("AO FORMULA FAIL: %s: %s -> %s, want %s\n", name, f, b, ao_err_str(want));
        fails++;
    }
}
static void expect_bool(const char *name, const char *f, int want) {
    total++;
    ao_value_t v = ev(f);
    if (v.kind != AO_VAL_BOOL || (v.num != 0) != want) {
        printf("AO FORMULA FAIL: %s: %s -> kind %d num %g, want bool %d\n",
               name, f, v.kind, v.num, want);
        fails++;
    }
}

int main(void) {
    ao_arena_init(&arena, mem, sizeof mem);
    book = ao_book_new(&arena);
    if (!book) { printf("AO FORMULA: FAIL (no book)\n"); return 1; }
    sh = ao_book_sheet(book, "S", 1);
    ao_book_set_now(book, 45000.0);   /* a fixed "today" for TODAY()/NOW() */

    /* Operators and precedence. */
    expect_num("add", "1+2", 3);
    expect_num("precedence", "1+2*3", 7);
    expect_num("parens", "(1+2)*3", 9);
    expect_num("sub-assoc", "10-3-2", 5);
    expect_num("div", "7/2", 3.5);
    expect_num("pow", "2^10", 1024);
    expect_num("pow-neg", "-2^2", 4);          /* Excel: unary binds first */
    expect_num("percent", "50%", 0.5);
    expect_num("percent-expr", "200*5%", 10);
    expect_num("unary", "--5", 5);
    expect_text("concat", "\"n=\"&42", "n=42");
    expect_text("concat-cells", "\"a\"&\"b\"&1+2", "ab3");
    expect_bool("eq", "1+1=2", 1);
    expect_bool("ne", "1<>2", 1);
    expect_bool("lt", "1<2", 1);
    expect_bool("ge-text", "\"b\">=\"a\"", 1);
    expect_bool("blank-eq-0", "Z9=0", 1);

    /* String coercion and its limits. */
    expect_num("coerce", "\"3\"*\"4\"", 12);
    expect_num("coerce-pct", "\"50%\"+1", 1.5);
    expect_err("coerce-bad", "\"x\"+1", AO_ERR_VALUE);
    expect_err("div0", "1/0", AO_ERR_DIV0);
    expect_err("ref-sheet", "Nope!A1", AO_ERR_REF);
    expect_err("name", "WHAT(1)", AO_ERR_NAME);
    expect_err("sqrt-neg", "SQRT(-1)", AO_ERR_NUM);

    /* Functions: aggregates over a fixture range. */
    ao_sheet_set(book, sh, 0, 0, "10");   /* A1 */
    ao_sheet_set(book, sh, 1, 0, "20");   /* A2 */
    ao_sheet_set(book, sh, 2, 0, "30");   /* A3 */
    ao_sheet_set(book, sh, 3, 0, "text"); /* A4 */
    ao_sheet_set(book, sh, 0, 1, "5");    /* B1 */
    ao_sheet_set(book, sh, 1, 1, "5");    /* B2 */
    ao_sheet_set(book, sh, 2, 1, "15");   /* B3 */
    expect_num("SUM", "SUM(A1:A3)", 60);
    expect_num("SUM-multi", "SUM(A1:A3,B1:B3)", 85);
    expect_num("AVERAGE", "AVERAGE(A1:A3)", 20);
    expect_num("MIN", "MIN(A1:B3)", 5);
    expect_num("MAX", "MAX(A1:B3)", 30);
    expect_num("COUNT", "COUNT(A1:A4)", 3);
    expect_num("COUNTA", "COUNTA(A1:A4)", 4);
    expect_num("PRODUCT", "PRODUCT(B1:B2)", 25);
    expect_num("MEDIAN-even", "MEDIAN(A1:A3,B1)", 15);
    expect_num("MEDIAN-odd", "MEDIAN(A1:A3)", 20);
    /* STDEV of 10,20,30 = sqrt(200/2) = 10 */
    expect_num("STDEV", "STDEV(A1:A3)", 10);
    expect_num("COUNTIF", "COUNTIF(B1:B3,\">=5\")", 3);
    expect_num("COUNTIF-text", "COUNTIF(A1:A4,\"text\")", 1);
    expect_num("SUMIF", "SUMIF(B1:B3,\">5\",A1:A3)", 30);
    expect_num("AVERAGEIF", "AVERAGEIF(B1:B3,\"=5\",A1:A3)", 15);
    expect_num("SUMPRODUCT", "SUMPRODUCT(A1:A3,B1:B3)", 600);

    /* Logic. */
    expect_num("IF", "IF(A1>5,111,222)", 111);
    expect_num("IF-false", "IF(A1>50,111,222)", 222);
    expect_num("IF-lazy", "IF(TRUE,1,1/0)", 1);
    expect_bool("AND", "AND(A1>5,B1=5)", 1);
    expect_bool("OR", "OR(A1>50,B1=5)", 1);
    expect_bool("NOT", "NOT(FALSE)", 1);

    /* Math. */
    expect_num("ABS", "ABS(-7)", 7);
    expect_num("ROUND", "ROUND(3.567,2)", 3.57);
    expect_num("ROUND-half", "ROUND(2.5,0)", 3);
    expect_num("ROUNDUP", "ROUNDUP(3.541,1)", 3.6);
    expect_num("ROUNDDOWN", "ROUNDDOWN(3.549,1)", 3.5);
    expect_num("INT-neg", "INT(-1.5)", -2);
    expect_num("MOD", "MOD(10,3)", 1);
    expect_num("MOD-neg", "MOD(-3,2)", 1);
    expect_num("POWER", "POWER(2,0.5)*POWER(2,0.5)", 2);
    expect_num("SQRT", "SQRT(144)", 12);
    expect_num("EXP-LN", "LN(EXP(2))", 2);
    expect_num("LOG", "LOG(1000)", 3);
    expect_num("LOG-base", "LOG(8,2)", 3);
    expect_num("SIN", "SIN(0)", 0);
    expect_num("COS", "COS(0)", 1);
    expect_num("TAN", "TAN(0)", 0);
    expect_num("PI", "PI()", 3.14159265358979);

    /* Text. */
    expect_text("CONCATENATE", "CONCATENATE(\"a\",1,\"b\")", "a1b");
    expect_text("LEFT", "LEFT(\"hello\",2)", "he");
    expect_text("RIGHT", "RIGHT(\"hello\",2)", "lo");
    expect_text("MID", "MID(\"hello\",2,3)", "ell");
    expect_num("LEN", "LEN(\"hello\")", 5);
    expect_text("UPPER", "UPPER(\"aBc\")", "ABC");
    expect_text("LOWER", "LOWER(\"aBc\")", "abc");
    expect_text("TRIM", "TRIM(\"  a   b  \")", "a b");
    expect_num("VALUE", "VALUE(\"42.5\")", 42.5);
    expect_text("TEXT-pct", "TEXT(0.25,\"0%\")", "25%");
    expect_text("TEXT-fixed", "TEXT(1234.5,\"#,##0.00\")", "1,234.50");

    /* Dates pinned by ao_book_set_now(45000) = 2023-03-15. */
    expect_num("TODAY", "TODAY()", 45000);
    expect_text("TEXT-date", "TEXT(45000,\"yyyy-mm-dd\")", "2023-03-15");

    /* Lookup family over a small table in D:F. */
    ao_sheet_set(book, sh, 0, 3, "1");      /* D1 */
    ao_sheet_set(book, sh, 0, 4, "one");    /* E1 */
    ao_sheet_set(book, sh, 1, 3, "2");      /* D2 */
    ao_sheet_set(book, sh, 1, 4, "two");    /* E2 */
    ao_sheet_set(book, sh, 2, 3, "3");      /* D3 */
    ao_sheet_set(book, sh, 2, 4, "three");  /* E3 */
    expect_text("VLOOKUP", "VLOOKUP(2,D1:E3,2,FALSE)", "two");
    expect_err("VLOOKUP-miss", "VLOOKUP(9,D1:E3,2,FALSE)", AO_ERR_NA);
    expect_text("INDEX", "INDEX(D1:E3,3,2)", "three");
    expect_num("MATCH", "MATCH(3,D1:D3,0)", 3);
    expect_err("MATCH-miss", "MATCH(9,D1:D3,0)", AO_ERR_NA);
    expect_num("RANK", "RANK(20,A1:A3)", 2);
    /* HLOOKUP over a horizontal table in row 5. */
    ao_sheet_set(book, sh, 4, 0, "7");
    ao_sheet_set(book, sh, 4, 1, "8");
    ao_sheet_set(book, sh, 4, 2, "9");
    ao_sheet_set(book, sh, 5, 0, "seven");
    ao_sheet_set(book, sh, 5, 1, "eight");
    ao_sheet_set(book, sh, 5, 2, "nine");
    expect_text("HLOOKUP", "HLOOKUP(8,A5:C6,2,FALSE)", "eight");

    /* Ranges and errors inside them. */
    ao_sheet_set(book, sh, 0, 6, "=1/0");   /* G1 */
    expect_err("range-err", "SUM(G1:G2)", AO_ERR_DIV0);
    expect_err("scalar-range", "A1:A2", AO_ERR_VALUE);

    /* A 1 000-cell SUM, built and summed through the engine. */
    ao_sheet_t *big = ao_book_sheet(book, "Big", 1);
    for (int i = 0; i < 1000; i++) {
        char in[16];
        snprintf(in, sizeof in, "%d", i + 1);
        ao_sheet_set(book, big, (uint32_t)i, 0, in);
    }
    ao_sheet_set(book, big, 0, 1, "=SUM(A1:A1000)");
    ao_value_t v = ao_sheet_get(book, big, 0, 1);
    total++;
    if (v.kind != AO_VAL_NUM || v.num != 500500.0) {
        printf("AO FORMULA FAIL: SUM-1000 -> kind %d num %.1f, want 500500\n", v.kind, v.num);
        fails++;
    }

    /* The self-referential cell: #CYCLE!, not a hang. */
    ao_sheet_t *cy = ao_book_sheet(book, "Cyc", 1);
    ao_sheet_set(book, cy, 0, 0, "=A1+1");
    ao_sheet_set(book, cy, 0, 1, "=A1*2");
    if (ao_book_recalc(book) != AO_CALC_OK) {
        printf("AO FORMULA FAIL: recalc refused the cyclic book\n");
        total++; fails++;
    } else {
        total++;
    }
    v = ao_sheet_get(book, cy, 0, 0);
    total++;
    if (v.kind != AO_VAL_ERR || v.err != AO_ERR_CYCLE) {
        printf("AO FORMULA FAIL: A1=A1+1 -> err %d, want #CYCLE!\n", v.err);
        fails++;
    }
    v = ao_sheet_get(book, cy, 0, 1);
    total++;
    if (v.kind != AO_VAL_ERR || v.err != AO_ERR_CYCLE) {
        printf("AO FORMULA FAIL: dependent of cycle -> err %d, want #CYCLE!\n", v.err);
        fails++;
    }

    printf("AO FORMULA: %s (%d checks, arena %zu bytes)\n",
           fails ? "FAIL" : "PASS", total, ao_arena_used(&arena));
    return fails ? 1 : 0;
}
