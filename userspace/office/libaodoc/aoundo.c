/* userspace/office/libaodoc/aoundo.c — undo/redo (OFFICE_PLAN.md OF2).
 *
 * Layout of the shared buffer:
 *
 *   [ undo entry ][ undo entry ] ........... [ redo entry ][ redo entry ]
 *   0                                      ^ tail          cap
 *
 * An entry is a 8-byte header (u32 payload length, u8 kind, u8 pad, u16 pad2)
 * followed by the serialised document.  Undo records are appended at
 * undo_used; redo records are appended DOWNWARDS from cap.  Both stacks are
 * LIFO, so "pop" is a length subtraction and "clear" is a field assignment.
 */

#include "aoundo.h"

#include <string.h>

#define HDR 8u

typedef struct {
    uint32_t len;
    uint8_t  kind;
    uint8_t  _pad;
    uint16_t _pad2;
} hdr_t;

static void write_hdr(uint8_t *p, uint32_t len, uint8_t kind) {
    hdr_t h;
    memset(&h, 0, sizeof h);
    h.len = len;
    h.kind = kind;
    memcpy(p, &h, HDR);
}

static void read_hdr(const uint8_t *p, hdr_t *h) { memcpy(h, p, HDR); }

int ao_undo_init(ao_undo_t *u, void *undo_mem, size_t undo_cap,
                 void *redo_mem, size_t redo_cap) {
    if (!u) return AO_E_ARG;
    memset(u, 0, sizeof(*u));
    if ((undo_cap && !undo_mem) || (redo_cap && !redo_mem)) return AO_E_ARG;
    u->undo_base = (uint8_t *)undo_mem;
    u->undo_cap = undo_mem ? undo_cap : 0;
    u->redo_base = (uint8_t *)redo_mem;
    u->redo_cap = redo_mem ? redo_cap : 0;
    return AO_OK;
}

void ao_undo_reset(ao_undo_t *u) {
    if (!u) return;
    u->undo_used = u->redo_used = 0;
    u->undo_count = u->redo_count = 0;
    u->last_kind = AO_UNDO_BARRIER;
    u->coalesce_blocked = 0;
}

void ao_undo_break(ao_undo_t *u) {
    if (!u) return;
    /* The next push starts a new entry even if it is the same kind: used by a
     * caret move, a save, or anything that should not merge with the typing
     * that preceded it. */
    u->coalesce_blocked = 1;
}

uint32_t ao_undo_depth(const ao_undo_t *u) { return u ? u->undo_count : 0; }
uint32_t ao_undo_redo_depth(const ao_undo_t *u) { return u ? u->redo_count : 0; }
size_t   ao_undo_bytes(const ao_undo_t *u) {
    return u ? (u->undo_used + u->redo_used) : 0;
}

/* Push a serialised document onto one of the two stacks.  Entries are variable
 * length, so a stack is a byte array plus a header per entry; the LIFO order is
 * implied by the byte order, and the top is found by walking (the budget bounds
 * how many entries there can be). */
static int stack_push(uint8_t *base, size_t cap, size_t *used, uint32_t *count,
                      const ao_model_t *m, uint8_t kind, int evict,
                      uint32_t *evicted) {
    size_t need = HDR + ao_model_serialized_size(m);
    while (*used + need > cap) {
        if (!*count || !evict) return AO_E_FULL;
        /* Evict the OLDEST entry, at the bottom. */
        hdr_t h;
        read_hdr(base, &h);
        size_t drop = HDR + h.len;
        if (drop > *used) drop = *used;
        memmove(base, base + drop, *used - drop);
        *used -= drop;
        (*count)--;
        if (evicted) (*evicted)++;
    }
    size_t len = 0;
    int rc = ao_model_serialize(m, base + *used + HDR, cap - *used - HDR, &len);
    if (rc != AO_OK) return AO_E_FULL;
    write_hdr(base + *used, (uint32_t)len, kind);
    *used += HDR + len;
    (*count)++;
    return AO_OK;
}

/* The last (top) entry of a stack. */
static int stack_top(const uint8_t *base, size_t used, uint32_t count,
                     size_t *off_out, hdr_t *h_out) {
    if (!count || used < HDR) return AO_E_FULL;
    size_t off = 0, last = 0;
    uint32_t found = 0;
    while (off + HDR <= used) {
        hdr_t h;
        read_hdr(base + off, &h);
        if (off + HDR + h.len > used) break;
        last = off;
        found++;
        off += HDR + h.len;
    }
    if (found != count) return AO_E_FORMAT;
    *off_out = last;
    read_hdr(base + last, h_out);
    return AO_OK;
}

int ao_undo_push(ao_undo_t *u, const ao_model_t *m, uint8_t kind) {
    if (!u || !m) return AO_E_ARG;
    if (u->undo_cap == 0) { u->refusals++; return AO_E_FULL; }

    /* Coalescing: a keystroke that follows another keystroke does not need its
     * own entry -- the entry already on the stack is the state before the whole
     * run, which is exactly what one undo should return to. */
    if (u->undo_count && !u->coalesce_blocked && u->last_kind == kind &&
        (kind == AO_UNDO_TYPING || kind == AO_UNDO_DELETE)) {
        u->coalesced++;
        return AO_OK;
    }

    /* Any new edit invalidates the redo stack: this makes "type, undo, type"
     * unable to resurrect a future that no longer follows from the text. */
    u->redo_used = 0;
    u->redo_count = 0;

    int rc = stack_push(u->undo_base, u->undo_cap, &u->undo_used, &u->undo_count,
                        m, kind, 1, &u->evicted);
    if (rc != AO_OK) { u->refusals++; return rc; }
    u->last_kind = kind;
    u->coalesce_blocked = 0;
    u->pushes++;
    return AO_OK;
}

int ao_undo_undo(ao_undo_t *u, ao_model_t *m) {
    if (!u || !m) return AO_E_ARG;
    size_t off = 0;
    hdr_t h;
    int rc = stack_top(u->undo_base, u->undo_used, u->undo_count, &off, &h);
    if (rc != AO_OK) return rc;

    /* The entry is consumed whatever happens next, so remember where it starts. */
    const uint8_t *payload = u->undo_base + off + HDR;
    uint32_t len = h.len;
    u->undo_used = off;
    u->undo_count--;

    rc = stack_push(u->redo_base, u->redo_cap, &u->redo_used, &u->redo_count,
                    m, AO_UNDO_EDIT, 0, NULL);
    if (rc != AO_OK) {
        /* No room to remember how to redo: the step is refused rather than taken
         * without a way back.  Put the entry back where it was. */
        u->undo_used = off + HDR + len;
        u->undo_count++;
        return AO_E_FULL;
    }
    rc = ao_model_deserialize(m, payload, len);
    if (rc != AO_OK) return AO_E_FORMAT;    /* refused, never half-applied */
    u->coalesce_blocked = 1;
    u->last_kind = AO_UNDO_BARRIER;
    return AO_OK;
}

int ao_undo_redo(ao_undo_t *u, ao_model_t *m) {
    if (!u || !m) return AO_E_ARG;
    size_t off = 0;
    hdr_t h;
    int rc = stack_top(u->redo_base, u->redo_used, u->redo_count, &off, &h);
    if (rc != AO_OK) return rc;

    const uint8_t *payload = u->redo_base + off + HDR;
    uint32_t len = h.len;
    u->redo_used = off;
    u->redo_count--;

    rc = stack_push(u->undo_base, u->undo_cap, &u->undo_used, &u->undo_count,
                    m, AO_UNDO_EDIT, 0, NULL);
    if (rc != AO_OK) {
        u->redo_used = off + HDR + len;
        u->redo_count++;
        return AO_E_FULL;
    }
    rc = ao_model_deserialize(m, payload, len);
    if (rc != AO_OK) return AO_E_FORMAT;
    u->coalesce_blocked = 1;
    u->last_kind = AO_UNDO_BARRIER;
    return AO_OK;
}
