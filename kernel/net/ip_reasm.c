/*
 * ip_reasm.c — pure IPv4 fragment reassembly engine (X4).
 * See ip_reasm.h for the contract and the security policy.
 */
#include "kernel/net/ip_reasm.h"

static void bm_set(uint8_t *bm, uint16_t bit)  { bm[bit >> 3] |= (uint8_t)(1u << (bit & 7)); }
static int  bm_get(const uint8_t *bm, uint16_t bit) { return (bm[bit >> 3] >> (bit & 7)) & 1; }

void ipreasm_init(ipreasm_t *t) {
    memset(t, 0, sizeof(*t));
}

int ipreasm_entries(const ipreasm_t *t) {
    int n = 0;
    for (int i = 0; i < IPREASM_MAX_ENTRIES; i++)
        if (t->e[i].used) n++;
    return n;
}

static int key_eq(const ipreasm_key_t *a, const ipreasm_key_t *b) {
    return a->src == b->src && a->dst == b->dst &&
           a->proto == b->proto && a->id == b->id;
}

static int entry_expired(const ipreasm_entry_t *e, uint32_t now_ms,
                         uint32_t timeout_ms) {
    return (now_ms - e->first_seen_ms) > timeout_ms;
}

int ipreasm_sweep(ipreasm_t *t, uint32_t now_ms, uint32_t timeout_ms) {
    int dropped = 0;
    for (int i = 0; i < IPREASM_MAX_ENTRIES; i++) {
        if (t->e[i].used && entry_expired(&t->e[i], now_ms, timeout_ms)) {
            t->e[i].used = 0;
            t->n_timeout++;
            dropped++;
        }
    }
    return dropped;
}

int ipreasm_input(ipreasm_t *t, uint32_t now_ms, uint32_t timeout_ms,
                  const ipreasm_key_t *key, uint16_t expected_len_or_0,
                  uint16_t off_bytes, const uint8_t *payload, uint16_t plen,
                  uint8_t *out, uint16_t out_cap, uint16_t *out_len) {
    /* Basic sanity: empty fragments and non-8-aligned offsets are not ours. */
    if (plen == 0) return IPREASM_REFUSED;
    if ((off_bytes & 7) != 0) return IPREASM_REFUSED;

    /* Lazy expiry: an expired entry with this key must not absorb fragments
     * — drop it and treat the fragment as the start of a new datagram. */
    ipreasm_sweep(t, now_ms, timeout_ms);

    /* Memory cap, enforced before anything is stored. */
    if ((uint32_t)off_bytes + plen > IPREASM_CAP) {
        /* A beyond-cap fragment of a tracked key kills the entry: either a
         * stranger trying to grow the datagram past the cap, or corruption.
         * Either way the safe thing is to refuse and forget. */
        for (int i = 0; i < IPREASM_MAX_ENTRIES; i++)
            if (t->e[i].used && key_eq(&t->e[i].key, key))
                t->e[i].used = 0;
        t->n_cap_refused++;
        return IPREASM_REFUSED;
    }
    if (expected_len_or_0 > IPREASM_CAP ||
        (expected_len_or_0 != 0 &&
         (uint32_t)off_bytes + plen != expected_len_or_0)) {
        /* Bogus "last fragment": wrong size or claims a cap-breaking total. */
        t->n_cap_refused++;
        return IPREASM_REFUSED;
    }

    /* Find (or make) the entry. */
    ipreasm_entry_t *e = 0;
    for (int i = 0; i < IPREASM_MAX_ENTRIES; i++)
        if (t->e[i].used && key_eq(&t->e[i].key, key)) { e = &t->e[i]; break; }

    if (!e) {
        /* Free slot, else evict the oldest entry (bounded LRU). */
        int free_i = -1, oldest_i = 0;
        uint32_t oldest_age = 0;
        for (int i = 0; i < IPREASM_MAX_ENTRIES; i++) {
            if (!t->e[i].used) { free_i = i; break; }
            uint32_t age = now_ms - t->e[i].first_seen_ms;
            if (age >= oldest_age) { oldest_age = age; oldest_i = i; }
        }
        if (free_i >= 0) e = &t->e[free_i];
        else { e = &t->e[oldest_i]; t->n_evicted++; }
        memset(e, 0, sizeof(*e));
        e->used = 1;
        e->key = *key;
        e->first_seen_ms = now_ms;
    }

    /* A "last fragment" pins the expected length; a conflicting second view
     * of the total is refused (first wins — we never move the goalposts). */
    if (expected_len_or_0 != 0) {
        if (e->expected_len != 0 && e->expected_len != expected_len_or_0)
            return IPREASM_REFUSED;
        e->expected_len = expected_len_or_0;
    }
    /* A fragment that reaches past the pinned total is garbage. */
    if (e->expected_len != 0 &&
        (uint32_t)off_bytes + plen > e->expected_len)
        return IPREASM_REFUSED;

    /* Copy in the new bytes: first writer wins; a conflicting re-write of
     * any previously received byte refuses the whole fragment. */
    for (uint16_t i = 0; i < plen; i++) {
        uint16_t pos = (uint16_t)(off_bytes + i);
        if (bm_get(e->bitmap, pos)) {
            if (e->data[pos] != payload[i]) {
                t->n_overlap_refused++;
                return IPREASM_REFUSED;      /* attack shape: refuse it */
            }
            continue;                        /* identical retransmission */
        }
    }
    for (uint16_t i = 0; i < plen; i++) {
        uint16_t pos = (uint16_t)(off_bytes + i);
        if (!bm_get(e->bitmap, pos)) {
            bm_set(e->bitmap, pos);
            e->data[pos] = payload[i];
            e->have_bytes++;
        }
    }

    /* Done? */
    if (e->expected_len != 0 && e->have_bytes == e->expected_len) {
        uint16_t total = e->expected_len;
        uint16_t ncopy = total > out_cap ? out_cap : total;
        if (out && ncopy) memcpy(out, e->data, ncopy);
        if (out_len) *out_len = total;
        e->used = 0;
        t->n_complete++;
        return IPREASM_COMPLETE;
    }
    return IPREASM_PENDING;
}
