/* tests/unit/test_ao_slide.c — OFFICE_PLAN OF6 deck gate.
 *
 * Model invariants, z-order operations, snapping arithmetic, undo across
 * slide reorder, serialize -> parse round-trips both directions.  All on
 * the host; QEMU stays for the integration gate.
 */
#include "aodeck.h"
#include "aomodel.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static ao_deck_t deck;                 /* ~450 KB: static, not stack */
static int total, fails;

static void check(const char *name, int cond) {
    total++;
    if (!cond) { printf("AO SLIDE FAIL: %s\n", name); fails++; }
}

static void reset(void) { ao_deck_init(&deck); }

/* Give slide 0 a recognisable identity for reorder tests. */
static void tag(ao_deck_t *d, uint32_t s, const char *note) {
    snprintf(d->slides[s].notes, AO_DECK_NOTES_MAX, "%s", note);
}

static int model_text_eq(const ao_model_t *m, const char *s) {
    size_t n = strlen(s);
    return m->text_len == n && memcmp(m->text, s, n) == 0;
}

static int deck_eq_text(ao_deck_t *d, uint32_t s, uint32_t i, const char *text) {
    if (s >= d->slide_len || i >= d->slides[s].obj_len) return 0;
    const ao_deck_obj_t *o = &d->slides[s].objs[i];
    if (o->kind != AO_OBJ_TEXT) return 0;
    return model_text_eq(&o->md, text);
}

int main(void) {
    /* ================= init & invariants ================= */
    reset();
    check("init: one slide", deck.slide_len == 1);
    check("init: no objects", deck.slides[0].obj_len == 0);
    check("init: master defaults (white, ink, 18pt, page numbers)",
          deck.master_bg == 0x00FFFFFF && deck.master_color == 0x00101010 &&
          deck.master_size == 18 && deck.master_page_numbers == 1);
    {
        const char *why = (const char *)0xDEADBEEF;
        check("init: deck is valid", ao_deck_valid(&deck, &why) == AO_DECK_OK && why == NULL);
    }
    check("init: notes start empty", deck.slides[0].notes[0] == '\0');
    check("init: guides default on", deck.guides_on == 1);
    check("names: text", ao_deck_kind_from_name("text") == AO_OBJ_TEXT);
    check("names: rect", ao_deck_kind_from_name("rect") == AO_OBJ_RECT);
    check("names: rounded", ao_deck_kind_from_name("rounded") == AO_OBJ_ROUNDED);
    check("names: ellipse", ao_deck_kind_from_name("ellipse") == AO_OBJ_ELLIPSE);
    check("names: line", ao_deck_kind_from_name("line") == AO_OBJ_LINE);
    check("names: polyline", ao_deck_kind_from_name("polyline") == AO_OBJ_POLYLINE);
    check("names: image", ao_deck_kind_from_name("image") == AO_OBJ_IMAGE);
    check("names: bogus is refused", ao_deck_kind_from_name("bogus") == AO_OBJ_NONE);
    check("names: kind->name round trip",
          !strcmp(ao_deck_kind_name(AO_OBJ_TEXT), "text") &&
          !strcmp(ao_deck_kind_name(AO_OBJ_IMAGE), "image"));
    check("undo: starts empty", deck.undo_len == 0 && deck.undo_total == 0);

    /* ================= slide ops ================= */
    reset();
    tag(&deck, 0, "zero");
    uint32_t idx;
    check("slide add after 0 gives index 1",
          ao_deck_slide_add(&deck, 0, &idx) == AO_DECK_OK && idx == 1 && deck.slide_len == 2);
    check("slide add produces an empty slide",
          deck.slides[1].obj_len == 0 && deck.slides[1].notes[0] == '\0');
    tag(&deck, 1, "one");
    check("slide add at the end gives index 2",
          ao_deck_slide_add(&deck, 1, &idx) == AO_DECK_OK && idx == 2 && deck.slide_len == 3);
    tag(&deck, 2, "two");
    check("slide add past the range is refused",
          ao_deck_slide_add(&deck, 9, &idx) == AO_DECK_E_RANGE);
    reset();
    for (uint32_t i = 1; i < AO_DECK_MAX_SLIDES; i++)
        ao_deck_slide_add(&deck, i - 1, &idx);
    check("deck fills to the cap and then refuses",
          deck.slide_len == AO_DECK_MAX_SLIDES &&
          ao_deck_slide_add(&deck, AO_DECK_MAX_SLIDES - 1, &idx) == AO_DECK_E_FULL);

    reset();
    tag(&deck, 0, "zero");
    ao_deck_slide_add(&deck, 0, &idx); tag(&deck, 1, "one");
    ao_deck_slide_add(&deck, 1, &idx); tag(&deck, 2, "two");
    check("slide move 0->2 reorders with contents",
          ao_deck_slide_move(&deck, 0, 2) == AO_DECK_OK &&
          !strcmp(deck.slides[0].notes, "one") &&
          !strcmp(deck.slides[1].notes, "two") &&
          !strcmp(deck.slides[2].notes, "zero"));
    check("slide move to itself is a no-op",
          ao_deck_slide_move(&deck, 1, 1) == AO_DECK_OK);
    check("slide move out of range is refused",
          ao_deck_slide_move(&deck, 0, 7) == AO_DECK_E_RANGE);
    check("slide remove closes the gap",
          ao_deck_slide_remove(&deck, 0) == AO_DECK_OK &&
          !strcmp(deck.slides[0].notes, "two") && deck.slide_len == 2);
    reset();
    tag(&deck, 0, "only");
    check("the last slide cannot be removed",
          ao_deck_slide_remove(&deck, 0) == AO_DECK_E_RANGE && deck.slide_len == 1);
    check("slide remove out of range is refused",
          ao_deck_slide_remove(&deck, 5) == AO_DECK_E_RANGE);

    /* duplicate: contents copied, independence preserved */
    reset();
    ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx);
    deck.slides[0].objs[idx].x = 333;
    ao_model_insert(&deck.slides[0].objs[idx].md, 0, "copy me", 7);  /* not a TEXT obj: refuse path */
    ao_deck_obj_add(&deck, 0, AO_OBJ_TEXT, &idx);
    ao_model_insert(&deck.slides[0].objs[idx].md, 0, "source text", 11);
    tag(&deck, 0, "orig note");
    uint32_t dup;
    check("slide dup duplicates at the next position",
          ao_deck_slide_dup(&deck, 0, &dup) == AO_DECK_OK && dup == 1 &&
          deck.slide_len == 2);
    check("slide dup carries objects and notes",
          deck.slides[1].obj_len == 2 &&
          deck.slides[1].objs[0].x == 333 &&
          deck_eq_text(&deck, 1, 1, "source text") &&
          !strcmp(deck.slides[1].notes, "orig note"));
    ao_deck_obj_t *cp = &deck.slides[1].objs[1];
    (void)ao_model_insert(&cp->md, ao_model_text_len(&cp->md), " edit", 5);
    check("slide dup is independent of the source",
          deck_eq_text(&deck, 0, 1, "source text") &&
          deck_eq_text(&deck, 1, 1, "source text edit"));
    reset();
    for (uint32_t i = 1; i < AO_DECK_MAX_SLIDES; i++) ao_deck_slide_add(&deck, i - 1, &idx);
    check("slide dup at the cap is refused",
          ao_deck_slide_dup(&deck, 0, &dup) == AO_DECK_E_FULL);

    /* ================= object ops and z-order ================= */
    reset();
    check("obj add TEXT grows the object list",
          ao_deck_obj_add(&deck, 0, AO_OBJ_TEXT, &idx) == AO_DECK_OK &&
          idx == 0 && deck.slides[0].obj_len == 1 &&
          deck.slides[0].objs[0].kind == AO_OBJ_TEXT);
    check("obj add refuses a kind outside the enum",
          ao_deck_obj_add(&deck, 0, 99, &idx) == AO_DECK_E_ARG &&
          ao_deck_obj_add(&deck, 0, 0, &idx) == AO_DECK_E_ARG);
    check("obj add refuses a slide outside the deck",
          ao_deck_obj_add(&deck, 4, AO_OBJ_RECT, &idx) == AO_DECK_E_RANGE);
    reset();
    ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx);
    check("rect defaults are the wire values",
          deck.slides[0].objs[idx].x == 120 && deck.slides[0].objs[idx].y == 200 &&
          deck.slides[0].objs[idx].w == 240 && deck.slides[0].objs[idx].h == 140 &&
          deck.slides[0].objs[idx].fill == 0x002F60C0 && deck.slides[0].objs[idx].fill_on == 1);
    reset();
    ao_deck_obj_add(&deck, 0, AO_OBJ_ELLIPSE, &idx);
    check("ellipse defaults", deck.slides[0].objs[idx].fill == 0x00B2543C);
    reset();
    ao_deck_obj_add(&deck, 0, AO_OBJ_LINE, &idx);
    check("line defaults are endpoints in x,y,w,h",
          deck.slides[0].objs[idx].x == 120 && deck.slides[0].objs[idx].y == 400 &&
          deck.slides[0].objs[idx].w == 840 && deck.slides[0].objs[idx].h == 400 &&
          deck.slides[0].objs[idx].fill_on == 0 && deck.slides[0].objs[idx].stroke_on == 1);
    reset();
    ao_deck_obj_add(&deck, 0, AO_OBJ_POLYLINE, &idx);
    check("polyline starts empty", deck.slides[0].objs[idx].npts == 0);
    reset();
    ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx);
    deck.slides[0].objs[idx].x = 100; deck.slides[0].objs[idx].y = 100;
    deck.slides[0].objs[idx].w = 400; deck.slides[0].objs[idx].h = 300;
    uint32_t top;
    ao_deck_obj_add(&deck, 0, AO_OBJ_TEXT, &top);
    deck.slides[0].objs[top].x = 150; deck.slides[0].objs[top].y = 150;
    deck.slides[0].objs[top].w = 100; deck.slides[0].objs[top].h = 50;
    check("obj_at returns the topmost object",
          ao_deck_obj_at(&deck, 0, 200, 175) == (int32_t)top);
    check("obj_at returns -1 off every object",
          ao_deck_obj_at(&deck, 0, 10, 10) == -1);
    check("obj raise/lower swaps neighbours",
          ao_deck_obj_move(&deck, 0, top, -1) == AO_DECK_OK &&
          deck.slides[0].objs[0].kind == AO_OBJ_TEXT &&
          deck.slides[0].objs[1].kind == AO_OBJ_RECT);
    check("obj_to_front lifts the back object",
          ao_deck_obj_to_front(&deck, 0, 0) == AO_DECK_OK &&
          deck.slides[0].objs[1].kind == AO_OBJ_TEXT);
    check("obj_to_back sinks the front object",
          ao_deck_obj_to_back(&deck, 0, 1) == AO_DECK_OK &&
          deck.slides[0].objs[0].kind == AO_OBJ_TEXT);
    check("obj raise at the front boundary is refused",
          ao_deck_obj_move(&deck, 0, 1, +1) == AO_DECK_E_RANGE);
    reset();
    ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx);
    ao_deck_obj_add(&deck, 0, AO_OBJ_ELLIPSE, &idx);
    ao_deck_obj_add(&deck, 0, AO_OBJ_TEXT, &idx);
    ao_deck_obj_to_front(&deck, 0, 0);
    check("to_front preserves the order of the rest",
          deck.slides[0].objs[0].kind == AO_OBJ_ELLIPSE &&
          deck.slides[0].objs[1].kind == AO_OBJ_TEXT &&
          deck.slides[0].objs[2].kind == AO_OBJ_RECT);
    reset();
    ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx);
    ao_deck_obj_add(&deck, 0, AO_OBJ_TEXT, &idx);
    ao_deck_obj_remove(&deck, 0, 0);
    check("obj remove closes the gap",
          deck.slides[0].obj_len == 1 && deck.slides[0].objs[0].kind == AO_OBJ_TEXT);
    check("obj remove out of range is refused",
          ao_deck_obj_remove(&deck, 0, 9) == AO_DECK_E_RANGE);
    reset();
    for (uint32_t i = 0; i < AO_DECK_MAX_OBJECTS; i++) ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx);
    check("object list fills to the cap and then refuses",
          deck.slides[0].obj_len == AO_DECK_MAX_OBJECTS &&
          ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx) == AO_DECK_E_FULL);

    /* ============ serialize / parse round-trips ============ */
    char xml[64 * 1024];
    size_t xcap = 0;
    static ao_deck_t deck2;
    ao_deck_init(&deck2);

    /* Build a deck with one of everything. */
    reset();
    deck.master_size = 11; deck.master_bold = 1;
    deck.master_page_numbers = 0; deck.guides_on = 0;
    tag(&deck, 0, "заметки: слайд один");
    ao_deck_obj_add(&deck, 0, AO_OBJ_TEXT, &idx);
    ao_model_insert(&deck.slides[0].objs[idx].md, 0, "Заголовок", (uint32_t)sizeof("Заголовок") - 1);
    (void)ao_model_set_para_style(&deck.slides[0].objs[idx].md, 0, AO_PARA_H1);
    (void)ao_model_set_align(&deck.slides[0].objs[idx].md, 0, AO_ALIGN_CENTER);
    ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx);
    deck.slides[0].objs[idx].fill = 0x00DEADBE;
    ao_deck_obj_add(&deck, 0, AO_OBJ_ELLIPSE, &idx);
    ao_deck_obj_add(&deck, 0, AO_OBJ_POLYLINE, &idx);
    {
        static const int16_t PX[6] = { 100, 260, 420, 580, 740, 880 };
        static const int16_t PY[6] = { 430, 240, 430, 240, 430, 300 };
        ao_deck_obj_t *po = &deck.slides[0].objs[idx];
        po->npts = 6;
        memcpy(po->pts_x, PX, sizeof PX);
        memcpy(po->pts_y, PY, sizeof PY);
    }
    ao_deck_obj_add(&deck, 0, AO_OBJ_IMAGE, &idx);
    snprintf(deck.slides[0].objs[idx].img_path, AO_DECK_PATH_MAX, "/lib/my chart.png");
    ao_deck_obj_add(&deck, 0, AO_OBJ_TEXT, &idx);
    ao_model_insert(&deck.slides[0].objs[idx].md, 0, "a\n\nb", 4);   /* empty line */
    ao_deck_slide_add(&deck, 0, &idx);
    tag(&deck, 1, "second");

    check("serialize: writes the xml declaration",
          ao_deck_serialize(&deck, xml, sizeof xml, &xcap) == AO_DECK_OK &&
          xcap > 20 && !memcmp(xml, "<?xml", 5));
    check("round-trip: parse of the serialized deck succeeds",
          ao_deck_parse(&deck2, xml, xcap) == AO_DECK_OK);
    check("round-trip: slide count preserved", deck2.slide_len == 2);
    check("round-trip: object kinds preserved in order",
          deck2.slides[0].obj_len == 6 &&
          deck2.slides[0].objs[0].kind == AO_OBJ_TEXT &&
          deck2.slides[0].objs[1].kind == AO_OBJ_RECT &&
          deck2.slides[0].objs[2].kind == AO_OBJ_ELLIPSE &&
          deck2.slides[0].objs[3].kind == AO_OBJ_POLYLINE &&
          deck2.slides[0].objs[4].kind == AO_OBJ_IMAGE &&
          deck2.slides[0].objs[5].kind == AO_OBJ_TEXT);
    check("round-trip: rect geometry and fill preserved",
          deck2.slides[0].objs[1].x == 120 && deck2.slides[0].objs[1].fill == 0x00DEADBE);
    check("round-trip: heading text content preserved",
          model_text_eq(&deck2.slides[0].objs[0].md, "Заголовок"));
    check("round-trip: empty line inside text is preserved",
          model_text_eq(&deck2.slides[0].objs[5].md, "a\n\nb"));
    check("round-trip: heading paragraph style survives (AO_PARA_H1)",
          (deck2.slides[0].objs[0].md.paras[0].style & AO_PARA_KIND_MASK) == AO_PARA_H1);
    check("round-trip: paragraph alignment survives",
          deck2.slides[0].objs[0].md.paras[0].align == AO_ALIGN_CENTER);
    check("round-trip: notes with Cyrillic survive byte for byte",
          strcmp(deck2.slides[0].notes, "заметки: слайд один") == 0);
    check("round-trip: image path with a space survives",
          strcmp(deck2.slides[0].objs[4].img_path, "/lib/my chart.png") == 0);
    check("round-trip: polyline points survive",
          deck2.slides[0].objs[3].npts == 6 &&
          deck2.slides[0].objs[3].pts_x[5] == 880 &&
          deck2.slides[0].objs[3].pts_y[3] == 240);
    check("round-trip: master fields survive (size 11, bold, no page numbers, no guides)",
          deck2.master_size == 11 && deck2.master_bold == 1 &&
          deck2.master_page_numbers == 0 && deck2.guides_on == 0);
    ao_deck_init(&deck2);
    check("parse: garbage is refused and the deck is left empty",
          ao_deck_parse(&deck2, "<aodeck version=\"1\"><slide><obj", 40) == AO_DECK_E_FMT &&
          deck2.slide_len == 1 && deck2.slides[0].obj_len == 0);
    check("parse: unknown object type is refused",
          ao_deck_parse(&deck2,
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<aodeck version=\"1\">\n"
              "<slide>\n<obj type=\"squiggle\" x=\"0\" y=\"0\" w=\"1\" h=\"1\"/>\n</slide>\n</aodeck>\n",
              200) == AO_DECK_E_FMT);
    check("parse: a missing master attribute is refused",
          ao_deck_parse(&deck2,
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<aodeck version=\"1\">\n"
              "<master bg=\"#FFFFFF\" color=\"#101010\" size=\"18\" bold=\"0\"/>\n<slide>\n</slide>\n</aodeck>\n",
              300) == AO_DECK_E_FMT);

    /* ================= undo ================= */
    reset();
    ao_deck_undo_push(&deck);
    ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx);
    check("undo: returns the pre-mutation state",
          deck.slides[0].obj_len == 1 && ao_deck_undo(&deck) == 1 &&
          deck.slides[0].obj_len == 0);
    reset();
    tag(&deck, 0, "zero");
    ao_deck_slide_add(&deck, 0, &idx); tag(&deck, 1, "one");
    ao_deck_slide_add(&deck, 1, &idx); tag(&deck, 2, "two");
    ao_deck_undo_push(&deck);
    ao_deck_slide_move(&deck, 0, 2);                  /* zero one two -> one two zero */
    check("undo across slide reorder restores the old order",
          !strcmp(deck.slides[0].notes, "one") && ao_deck_undo(&deck) == 1 &&
          !strcmp(deck.slides[0].notes, "zero") &&
          !strcmp(deck.slides[1].notes, "one") &&
          !strcmp(deck.slides[2].notes, "two"));
    check("redo re-applies the reorder",
          ao_deck_redo(&deck) == 1 && !strcmp(deck.slides[0].notes, "one") &&
          !strcmp(deck.slides[1].notes, "two") &&
          !strcmp(deck.slides[2].notes, "zero"));
    ao_deck_undo_push(&deck);
    ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx);
    check("a new edit clears the redo ring", ao_deck_redo(&deck) == 0);
    {
        static ao_deck_t blank;
        ao_deck_init(&blank);
        check("undo on an empty stack returns 0", ao_deck_undo(&blank) == 0);
    }
    reset();
    for (int i = 0; i < 12; i++) {
        ao_deck_undo_push(&deck);
        ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx);
    }
    {
        int undos = 0;
        while (ao_deck_undo(&deck)) undos++;
        check("undo depth is capped at the ring size",
              undos == AO_DECK_UNDO_DEPTH);
    }
    reset();
    ao_deck_undo_push(&deck);
    ao_deck_slide_add(&deck, 0, &idx);
    check("undo across slide add works",
          deck.slide_len == 2 && ao_deck_undo(&deck) == 1 && deck.slide_len == 1);
    reset();
    ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx);
    ao_deck_undo_push(&deck);
    ao_deck_obj_remove(&deck, 0, 0);
    check("undo across obj remove restores the object",
          deck.slides[0].obj_len == 0 && ao_deck_undo(&deck) == 1 &&
          deck.slides[0].obj_len == 1);
    reset();
    ao_deck_obj_add(&deck, 0, AO_OBJ_TEXT, &idx);
    ao_model_insert(&deck.slides[0].objs[idx].md, 0, "keep me", 7);
    ao_deck_undo_push(&deck);
    uint32_t di;
    ao_deck_slide_dup(&deck, 0, &di);
    check("undo across slide dup removes the copy",
          deck.slide_len == 2 && ao_deck_undo(&deck) == 1 && deck.slide_len == 1);
    deck.master_size = 33;
    ao_deck_undo_push(&deck);
    deck.guides_on = 0;
    check("undo restores a changed master/view field",
          deck.guides_on == 0 && ao_deck_undo(&deck) == 1 && deck.guides_on == 1);
    reset();
    ao_deck_undo_push(&deck);
    ao_deck_obj_add(&deck, 0, AO_OBJ_RECT, &idx);
    ao_deck_undo(&deck); ao_deck_undo(&deck);   /* second one is a no-op */
    {
        const char *why = NULL;
        check("deck is valid after undo traffic",
              ao_deck_valid(&deck, &why) == AO_DECK_OK);
    }
    check("public parse clears the undo stack",
          ao_deck_parse(&deck2, xml, xcap) == AO_DECK_OK &&
          ao_deck_undo(&deck2) == 0);
    {
        char xml2[64 * 1024];
        size_t x2 = 0;
        ao_deck_serialize(&deck2, xml2, sizeof xml2, &x2);
        check("parse->serialize round-trip is byte-identical",
              x2 == xcap && memcmp(xml2, xml, x2) == 0);
        const char *why = NULL;
        check("deck is valid after building one of everything",
              ao_deck_valid(&deck, &why) == AO_DECK_OK && why == NULL);
        check("deck is valid after parsing the round-trip",
              ao_deck_valid(&deck2, &why) == AO_DECK_OK && why == NULL);
    }

    /* ================= snapping ================= */
    {
        int32_t guides[2] = { 462, 470 };
        static const int32_t vguides[3] = { 48, 480, 912 };
        static const int32_t hguides[3] = { 40, 270, 500 };
        int32_t snapped, ox, oy;
        uint32_t gi;
        check("snap: nearest guide within tolerance wins",
              ao_snap_axis(468, guides, 2, 6, &snapped, &gi) == 470 &&
              snapped == 1 && gi == 1);
        check("snap: outside tolerance is left alone",
              ao_snap_axis(500, guides, 2, 6, &snapped, &gi) == 500 && snapped == 0);
        check("snap: the tolerance edge is inclusive",
              ao_snap_axis(476, guides, 2, 6, &snapped, &gi) == 470 && snapped == 1);
        check("snap: one past the tolerance is not snapped",
              ao_snap_axis(477, guides, 2, 6, &snapped, &gi) == 477 && snapped == 0);
        check("snap: exactly at a guide snaps with distance zero",
              ao_snap_axis(480, vguides, 3, 6, &snapped, &gi) == 480 && snapped == 1);
        check("snap: a tie goes to the earlier guide",
              ao_snap_axis(466, (int32_t[]){464, 468}, 2, 6, &snapped, &gi) == 464);
        int32_t g1[2] = { 460, 470 };
        check("snap: between two in-tolerance guides the nearer wins",
              ao_snap_axis(468, g1, 2, 8, &snapped, &gi) == 470);
        check("snap: no guides means no snapping",
              ao_snap_axis(468, NULL, 0, 6, &snapped, &gi) == 468 && snapped == 0);
        check("snap: a negative coordinate snaps to the origin guide",
              ao_snap_axis(-3, (int32_t[]){0}, 1, 6, &snapped, &gi) == 0 && snapped == 1);
        uint32_t fl = ao_snap_box(472, 100, 16, 20, vguides, 3, hguides, 3, 6, &ox, &oy);
        check("snap box: the centre anchor snaps to the centre guide",
              (fl & AO_SNAP_V_GUIDE) != 0 && ox == 472 && oy == 100);
        fl = ao_snap_box(44, 250, 200, 44, vguides, 3, hguides, 3, 6, &ox, &oy);
        check("snap box: the left/top anchors snap",
              (fl & AO_SNAP_V_GUIDE) != 0 && (fl & AO_SNAP_H_GUIDE) != 0);
        fl = ao_snap_box(100, 100, 50, 50, vguides, 3, hguides, 3, 6, &ox, &oy);
        check("snap box: nothing near means no flags and no movement",
              fl == 0 && ox == 100 && oy == 100);
    }

    printf("AO SLIDE: %s (%d checks, 0 failures)\n", fails ? "FAIL" : "PASS", total);
    return fails ? 1 : 0;
}
