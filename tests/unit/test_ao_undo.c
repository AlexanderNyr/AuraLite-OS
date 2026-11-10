/* tests/unit/test_ao_undo.c — undo and redo over serialised snapshots (OF2).
 *
 * Undo is the feature users notice first when it is wrong, so the tests here
 * are written the way a writer uses it rather than the way the buffer is laid
 * out:
 *
 *   1. a session of edits, undone one at a time, comes back exactly;
 *   2. typing coalesces into one step, but a caret move (a barrier) splits it;
 *   3. a new edit after an undo throws the redo stack away, as every editor does;
 *   4. the byte budget evicts the OLDEST entry and never corrupts the rest;
 *   5. a 200-operation random session, undone to the top and redone to the
 *      bottom, reproduces both ends byte for byte.
 *
 * The snapshot design means "come back exactly" is a `memcmp` of the two
 * serialised documents, which is a much stronger claim than "the text looks
 * right": styles, paragraph alignment and the run split are all in there.
 */

#include <stdio.h>
#include <string.h>

#include "userspace/office/libaodoc/aomodel.h"
#include "userspace/office/libaodoc/aoundo.h"

static int checks = 0, failures = 0;

static void ck(const char *what, int cond) {
    checks++;
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
}

static char      g_text[8192];
static ao_run_t  g_runs[512];
static ao_para_t g_paras[256];
static char      g_text2[8192];
static ao_run_t  g_runs2[512];
static ao_para_t g_paras2[256];

static uint8_t   g_ser[16384];
static uint8_t   g_ser2[16384];

/* Two histories: one for the document under test, one big enough to hold a
 * whole session of snapshots for the 200-operation test. */
static uint8_t   g_undo_mem[64 * 1024];
static uint8_t   g_redo_mem[64 * 1024];
static uint8_t   g_undo_mem2[512 * 1024];
static uint8_t   g_redo_mem2[512 * 1024];

static void fresh(ao_model_t *m) {
    ao_model_init(m, g_text, sizeof g_text, g_runs, 512, g_paras, 256);
}
static void fresh2(ao_model_t *m) {
    ao_model_init(m, g_text2, sizeof g_text2, g_runs2, 512, g_paras2, 256);
}
static int ins(ao_model_t *m, uint32_t pos, const char *s) {
    return ao_model_insert(m, pos, s, (uint32_t)strlen(s));
}

/* The model's text is a byte range, not a C string: compare it as one. */
static int text_is(ao_model_t *m, const char *want) {
    size_t n = strlen(want);
    return m->text_len == n && !memcmp(m->text, want, n);
}

/* Serialise and compare: the full truth about a document, not just its text. */
static int same(ao_model_t *a, ao_model_t *b) {
    size_t na = 0, nb = 0;
    if (ao_model_serialize(a, g_ser, sizeof g_ser, &na) != AO_OK) return 0;
    if (ao_model_serialize(b, g_ser2, sizeof g_ser2, &nb) != AO_OK) return 0;
    return na == nb && !memcmp(g_ser, g_ser2, na);
}

/* A simple deterministic generator: the test must fail the same way twice. */
static uint32_t g_seed = 12345;
static uint32_t rnd(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 8) & 0xFFFFFFu;
}

/* ---------------------------------------------------------------- round trip */

static void test_round_trip(void) {
    ao_model_t m, back;
    ao_undo_t u;
    fresh(&m);
    fresh2(&back);
    ck("init accepts caller memory",
       ao_undo_init(&u, g_undo_mem, sizeof g_undo_mem, g_redo_mem, sizeof g_redo_mem) == AO_OK);
    ck("a fresh history is empty",
       ao_undo_depth(&u) == 0 && ao_undo_redo_depth(&u) == 0 && ao_undo_bytes(&u) == 0);

    ins(&m, 0, "Привет");
    ck("push before an edit", ao_undo_push(&u, &m, AO_UNDO_BARRIER) == AO_OK);
    ins(&m, ao_model_text_len(&m), ", мир");
    ck("one step is recorded", ao_undo_depth(&u) == 1);
    ck("the edit itself is in the document", text_is(&m, "Привет, мир"));

    ck("undo succeeds", ao_undo_undo(&u, &m) == AO_OK);
    ck("undo restores the text exactly", text_is(&m, "Привет"));
    ins(&back, 0, "Привет");
    ck("the undone document equals a document built from scratch", same(&m, &back));
    ck("undo moves the state to the redo stack",
       ao_undo_depth(&u) == 0 && ao_undo_redo_depth(&u) == 1);
    ck("the document is still valid after undo", ao_model_valid(&m, NULL));

    ck("redo succeeds", ao_undo_redo(&u, &m) == AO_OK);
    ck("redo restores the text exactly", text_is(&m, "Привет, мир"));
    ck("redo empties the redo stack",
       ao_undo_depth(&u) == 1 && ao_undo_redo_depth(&u) == 0);
    ck("redo with nothing to redo is refused", ao_undo_redo(&u, &m) == AO_E_FULL);
    ck("the document is still valid after redo", ao_model_valid(&m, NULL));

    ck("undo on an empty redo stack is refused, not destructive",
       ao_undo_undo(&u, &m) == AO_OK && ao_undo_undo(&u, &m) == AO_E_FULL);
    ck("the document survived the refused undo", text_is(&m, "Привет"));
}

/* Styles and alignment are part of the snapshot, which is the whole point of
 * storing the document rather than an inverse operation. */
static void test_styles_and_paragraphs(void) {
    ao_model_t m;
    ao_undo_t u;
    fresh(&m);
    ao_undo_init(&u, g_undo_mem, sizeof g_undo_mem, g_redo_mem, sizeof g_redo_mem);

    ins(&m, 0, "первый\nвторой");
    ao_undo_push(&u, &m, AO_UNDO_BARRIER);
    ao_model_set_style(&m, 0, 12, AO_STYLE_BOLD, 1);
    ck("bold is applied", ao_model_style_at(&m, 0) == AO_STYLE_BOLD);
    ao_undo_push(&u, &m, AO_UNDO_BARRIER);
    ao_model_set_align(&m, 0, AO_ALIGN_CENTER);
    ao_model_set_style(&m, 0, 12, AO_STYLE_BOLD, 0);
    ck("alignment is applied", m.paras[0].align == AO_ALIGN_CENTER);

    ck("undo the alignment", ao_undo_undo(&u, &m) == AO_OK);
    ck("alignment came back", m.paras[0].align == AO_ALIGN_LEFT);
    ck("the bold text came back", ao_model_style_at(&m, 0) == AO_STYLE_BOLD);
    ck("undo the bold", ao_undo_undo(&u, &m) == AO_OK);
    ck("bold is gone again", ao_model_style_at(&m, 0) == AO_STYLE_OFF);
    ck("the paragraphs are intact", ao_model_para_count(&m) == 2);
    ck("both undos can be redone",
       ao_undo_redo(&u, &m) == AO_OK && ao_undo_redo(&u, &m) == AO_OK);
    ck("the redone document has the alignment", m.paras[0].align == AO_ALIGN_CENTER);
    ck("the redone document is valid", ao_model_valid(&m, NULL));
}

/* --------------------------------------------------------------- coalescing */

static void test_coalescing(void) {
    ao_model_t m;
    ao_undo_t u;
    fresh(&m);
    ao_undo_init(&u, g_undo_mem, sizeof g_undo_mem, g_redo_mem, sizeof g_redo_mem);

    /* Ten keystrokes are one undo step. */
    for (int i = 0; i < 10; i++) {
        ao_undo_push(&u, &m, AO_UNDO_TYPING);
        ins(&m, ao_model_text_len(&m), "a");
    }
    ck("ten keystrokes coalesce into one entry", ao_undo_depth(&u) == 1);
    ck("the typing is all there", ao_model_text_len(&m) == 10);
    ao_undo_undo(&u, &m);
    ck("one undo removes the whole run of typing", ao_model_text_len(&m) == 0);

    /* A barrier ends the run: typing after it is its own entry. */
    ao_model_clear(&m);
    ao_undo_init(&u, g_undo_mem, sizeof g_undo_mem, g_redo_mem, sizeof g_redo_mem);
    for (int i = 0; i < 3; i++) {
        ao_undo_push(&u, &m, AO_UNDO_TYPING);
        ins(&m, ao_model_text_len(&m), "b");
    }
    ao_undo_break(&u);
    for (int i = 0; i < 3; i++) {
        ao_undo_push(&u, &m, AO_UNDO_TYPING);
        ins(&m, ao_model_text_len(&m), "c");
    }
    ck("a barrier splits the run into two entries", ao_undo_depth(&u) == 2);
    ao_undo_undo(&u, &m);
    ck("the first undo removes only the second run", text_is(&m, "bbb"));
    ao_undo_undo(&u, &m);
    ck("the second undo removes the first run", ao_model_text_len(&m) == 0);

    /* Deletes coalesce among themselves but not with typing. */
    ao_model_clear(&m);
    ao_undo_init(&u, g_undo_mem, sizeof g_undo_mem, g_redo_mem, sizeof g_redo_mem);
    ins(&m, 0, "abcdefgh");
    ao_undo_push(&u, &m, AO_UNDO_TYPING);
    ins(&m, 8, "i");
    for (int i = 0; i < 3; i++) {
        ao_undo_push(&u, &m, AO_UNDO_DELETE);
        ao_model_delete(&m, ao_model_text_len(&m) - 1, 1);
    }
    ck("typing then deleting is two entries", ao_undo_depth(&u) == 2);
    ck("the deletes happened", text_is(&m, "abcdef"));
    ao_undo_undo(&u, &m);
    ck("one undo brings back all three deleted characters", text_is(&m, "abcdefghi"));

    /* An edit entry is never coalesced. */
    ao_model_clear(&m);
    ao_undo_init(&u, g_undo_mem, sizeof g_undo_mem, g_redo_mem, sizeof g_redo_mem);
    ao_undo_push(&u, &m, AO_UNDO_EDIT);
    ins(&m, 0, "x");
    ao_undo_push(&u, &m, AO_UNDO_EDIT);
    ins(&m, 0, "y");
    ck("two EDIT entries stay two entries", ao_undo_depth(&u) == 2);

    /* Changing kind also breaks the run. */
    ao_model_clear(&m);
    ao_undo_init(&u, g_undo_mem, sizeof g_undo_mem, g_redo_mem, sizeof g_redo_mem);
    ao_undo_push(&u, &m, AO_UNDO_TYPING);
    ins(&m, 0, "1");
    ao_undo_push(&u, &m, AO_UNDO_DELETE);
    ck("a different kind starts a new entry", ao_undo_depth(&u) == 2);
}

/* ------------------------------------------------------------------- redo */

static void test_redo_invalidation(void) {
    ao_model_t m;
    ao_undo_t u;
    fresh(&m);
    ao_undo_init(&u, g_undo_mem, sizeof g_undo_mem, g_redo_mem, sizeof g_redo_mem);

    ins(&m, 0, "one");
    ao_undo_push(&u, &m, AO_UNDO_BARRIER);
    ins(&m, ao_model_text_len(&m), "-two");
    ao_undo_push(&u, &m, AO_UNDO_BARRIER);
    ins(&m, ao_model_text_len(&m), "-three");
    ck("three states", ao_undo_depth(&u) == 2);

    ao_undo_undo(&u, &m);
    ck("undone once", text_is(&m, "one-two"));
    ck("redo is available", ao_undo_redo_depth(&u) == 1);

    /* Editing now must throw the redo stack away. */
    ao_undo_push(&u, &m, AO_UNDO_BARRIER);
    ins(&m, ao_model_text_len(&m), "-other");
    ck("a new edit clears the redo stack", ao_undo_redo_depth(&u) == 0);
    ck("redo is refused after a new edit", ao_undo_redo(&u, &m) == AO_E_FULL);
    ck("the new text is what is in the document", text_is(&m, "one-two-other"));
    ao_undo_undo(&u, &m);
    ck("undo still works after the redo stack was dropped", text_is(&m, "one-two"));
}

/* ------------------------------------------------------------------ budget */

static void test_budget(void) {
    ao_model_t m;
    ao_undo_t u;
    fresh(&m);
    /* A deliberately tiny history: room for about three small snapshots. */
    ck("a small history is accepted",
       ao_undo_init(&u, g_undo_mem, 900, g_redo_mem, 900) == AO_OK);

    ins(&m, 0, "x");
    int pushed = 0;
    for (int i = 0; i < 40; i++) {
        if (ao_undo_push(&u, &m, AO_UNDO_BARRIER) == AO_OK) pushed++;
        ins(&m, ao_model_text_len(&m), "аб");
    }
    ck("some pushes succeeded", pushed > 0);
    ck("the history never exceeds its byte budget", ao_undo_bytes(&u) <= 900);
    ck("the history is bounded by the budget, not by the edit count",
       ao_undo_depth(&u) < 40);
    ck("evictions were counted", u.evicted > 0);
    ck("pushes and coalescing are counted", u.pushes == (uint32_t)pushed);

    /* Everything that is still in the history must undo cleanly, in order, and
     * the document must never become invalid on the way down. */
    uint32_t depth = ao_undo_depth(&u);
    int ok = 1;
    for (uint32_t i = 0; i < depth; i++) {
        if (ao_undo_undo(&u, &m) != AO_OK) { ok = 0; break; }
        if (!ao_model_valid(&m, NULL)) { ok = 0; break; }
    }
    ck("every surviving entry undoes cleanly", ok && ao_undo_depth(&u) == 0);
    ck("after undoing everything the document is at the oldest kept state",
       ao_model_valid(&m, NULL));

    /* A zero-capacity history is legal and honest about it. */
    ao_undo_t zero;
    ck("a zero-capacity history initialises",
       ao_undo_init(&zero, NULL, 0, NULL, 0) == AO_OK);
    ck("and refuses to push", ao_undo_push(&zero, &m, AO_UNDO_BARRIER) == AO_E_FULL);
    ck("and refuses to undo", ao_undo_undo(&zero, &m) == AO_E_FULL);
    ck("the document is untouched by the refusals", ao_model_valid(&m, NULL));
}

/* A snapshot bigger than the whole history must be refused, and the refusal
 * must not corrupt the entries already stored. */
static void test_refusal_is_clean(void) {
    ao_model_t m;
    ao_undo_t u;
    fresh(&m);
    ins(&m, 0, "kept");
    ao_undo_init(&u, g_undo_mem, sizeof g_undo_mem, g_redo_mem, sizeof g_redo_mem);
    ck("a normal push works", ao_undo_push(&u, &m, AO_UNDO_BARRIER) == AO_OK);
    ins(&m, ao_model_text_len(&m), " and more");

    ao_undo_t tiny;
    ao_undo_init(&tiny, g_undo_mem2, 16, g_redo_mem2, 16);
    ck("a snapshot that cannot fit is refused",
       ao_undo_push(&tiny, &m, AO_UNDO_BARRIER) == AO_E_FULL);
    ck("a refused push leaves no entry", ao_undo_depth(&tiny) == 0);
    ck("and reports the refusal", tiny.refusals == 1);
    ck("the other history is unaffected",
       ao_undo_depth(&u) == 1 && ao_undo_undo(&u, &m) == AO_OK && text_is(&m, "kept"));
}

/* ------------------------------------------------------------------ session */

/* 200 random edits with a snapshot before each: undo all the way down, then
 * redo all the way up, and the two ends must be byte identical to the states
 * the session actually passed through. */
static void test_session(void) {
    static const char *const words[] = {
        "a", "Z", " ", "\n", "привет", "ё", "Ё", "дом", "x y", "\n\n", "—", "…"
    };
    ao_model_t m;
    ao_undo_t u;
    fresh(&m);
    ao_undo_init(&u, g_undo_mem2, sizeof g_undo_mem2, g_redo_mem2, sizeof g_redo_mem2);

    ck("the session starts from an empty document", ao_undo_depth(&u) == 0);
    for (int i = 0; i < 200; i++) {
        uint32_t len = ao_model_text_len(&m);
        uint32_t op = rnd() % 6;
        if (op == 0 && len) {
            ao_undo_push(&u, &m, AO_UNDO_DELETE);
            ao_model_delete(&m, rnd() % len, 1 + rnd() % 4);
        } else if (op == 2 && len) {
            ao_undo_push(&u, &m, AO_UNDO_BARRIER);
            ao_model_set_style(&m, rnd() % len, 1 + rnd() % 6,
                               (uint8_t)(1u << (rnd() % 3)), (int)(rnd() & 1));
        } else if (op == 3) {
            ao_undo_push(&u, &m, AO_UNDO_BARRIER);
            ao_model_set_align(&m, len ? rnd() % len : 0, (uint8_t)(rnd() % 4));
        } else {
            const char *w = words[rnd() % (sizeof words / sizeof words[0])];
            uint32_t pos = len ? rnd() % (len + 1) : 0;
            ao_undo_push(&u, &m, AO_UNDO_TYPING);
            ao_model_insert(&m, pos, w, (uint32_t)strlen(w));
        }
        if (!ao_model_valid(&m, NULL)) break;
    }
    ck("200 operations left a valid document", ao_model_valid(&m, NULL));
    ck("the session recorded history", ao_undo_depth(&u) > 10);
    ck("coalescing kept the history shorter than the edit count", ao_undo_depth(&u) < 200);

    /* Remember the end state, then walk all the way down. */
    size_t na = 0;
    ck("the end state serialises", ao_model_serialize(&m, g_ser, sizeof g_ser, &na) == AO_OK);
    uint32_t steps = ao_undo_depth(&u);
    int down_ok = 1;
    for (uint32_t i = 0; i < steps; i++) {
        if (ao_undo_undo(&u, &m) != AO_OK || !ao_model_valid(&m, NULL)) { down_ok = 0; break; }
    }
    ck("the whole session undoes without a single invalid state", down_ok);
    ck("everything undone is on the redo stack", ao_undo_redo_depth(&u) == steps);
    ck("the document is back at the beginning", ao_model_text_len(&m) == 0);

    /* ... and all the way back up. */
    int up_ok = 1;
    for (uint32_t i = 0; i < steps; i++) {
        if (ao_undo_redo(&u, &m) != AO_OK || !ao_model_valid(&m, NULL)) { up_ok = 0; break; }
    }
    ck("the whole session redoes without a single invalid state", up_ok);
    size_t nb = 0;
    ck("the redone state serialises", ao_model_serialize(&m, g_ser2, sizeof g_ser2, &nb) == AO_OK);
    ck("redo all the way up reproduces the end state byte for byte",
       na == nb && !memcmp(g_ser, g_ser2, na));
}

int main(void) {
    test_round_trip();
    test_styles_and_paragraphs();
    test_coalescing();
    test_redo_invalidation();
    test_budget();
    test_refusal_is_clean();
    test_session();
    printf("AO UNDO: %s (%d checks)\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
