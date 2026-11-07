/* aspace.c — refcounted user address spaces.  See aspace.h for the contract
 * and for the two defects that made it necessary.
 *
 * Implemented with __sync builtins only (no spinlock.h, no paging.h) so the
 * host unit test links this translation unit as-is.
 */

#include <stddef.h>

#include "kernel/mm/aspace.h"

typedef struct {
    uint64_t pml4;      /* 0 = slot free */
    uint32_t refs;
} aslot_t;

static aslot_t  slots[ASPACE_SLOTS];
static volatile uint8_t table_lock;

static uint64_t shared_spaces;
static uint64_t untracked_puts;
static uint64_t last_puts;
static uint64_t double_puts;

/* Tombstones: the roots whose last owner this module has already released.
 * A put of a root that is not in the table but IS in this ring is a second
 * release of the same address space (a TCB reaped twice, an exit path that
 * drops the space and then the exec path drops it again) — it must NOT
 * authorise a second free of page tables that are now somebody else's. */
#define ASPACE_TOMBSTONES 32
static uint64_t tombstones[ASPACE_TOMBSTONES];
static unsigned tomb_next;

static void tombstone_remember(uint64_t pml4) {
    tombstones[tomb_next] = pml4;
    tomb_next = (tomb_next + 1) % ASPACE_TOMBSTONES;
}

static int tombstone_has(uint64_t pml4) {
    for (unsigned i = 0; i < ASPACE_TOMBSTONES; i++) {
        if (tombstones[i] == pml4) return 1;
    }
    return 0;
}

static void aspace_lock(void)   { while (__sync_lock_test_and_set(&table_lock, 1)) { } }
static void aspace_unlock(void) { __sync_lock_release(&table_lock); }

static unsigned home_slot(uint64_t pml4) {
    /* The low 12 bits of a PML4 root are flags/zero; fold the frame number. */
    uint64_t f = pml4 >> 12;
    f ^= f >> 9;
    return (unsigned)(f % ASPACE_SLOTS);
}

/* Returns the slot holding @pml4, or NULL.  Caller holds the table lock. */
static aslot_t *find_locked(uint64_t pml4) {
    unsigned h = home_slot(pml4);
    for (unsigned i = 0; i < 8; i++) {
        aslot_t *s = &slots[(h + i) % ASPACE_SLOTS];
        if (s->pml4 == pml4) return s;
        if (s->pml4 == 0) return NULL;      /* empty slot ends the probe run */
    }
    return NULL;
}

void aspace_reset(void) {
    aspace_lock();
    for (unsigned i = 0; i < ASPACE_SLOTS; i++) {
        slots[i].pml4 = 0;
        slots[i].refs = 0;
    }
    shared_spaces  = 0;
    untracked_puts = 0;
    last_puts      = 0;
    double_puts    = 0;
    tomb_next      = 0;
    for (unsigned i = 0; i < ASPACE_TOMBSTONES; i++) tombstones[i] = 0;
    aspace_unlock();
}

uint64_t aspace_get(uint64_t pml4) {
    if (pml4 == 0) return 0;                /* kernel address space: untracked */

    aspace_lock();
    aslot_t *s = find_locked(pml4);
    if (s) {
        s->refs++;
        if (s->refs == 2) shared_spaces++;
        uint64_t r = s->refs;
        aspace_unlock();
        return r;
    }
    /* Register: first owner.  Insert at the home slot's probe window. */
    unsigned h = home_slot(pml4);
    for (unsigned i = 0; i < 8; i++) {
        aslot_t *c = &slots[(h + i) % ASPACE_SLOTS];
        if (c->pml4 == 0) {
            c->pml4 = pml4;
            c->refs = 1;
            uint64_t r = 1;
            aspace_unlock();
            return r;
        }
    }
    /* Table full for this hash window: keep the pre-C2 behaviour.  The put
     * that follows frees on first drop, which is exactly what the tree did
     * before this module existed — a precision loss, never a leak. */
    aspace_unlock();
    return 1;
}

int aspace_put(uint64_t pml4) {
    if (pml4 == 0) return 0;                /* never freed through here */

    aspace_lock();
    aslot_t *s = find_locked(pml4);
    if (!s) {
        if (tombstone_has(pml4)) {
            /* Second release of a space this module already freed. */
            double_puts++;
            aspace_unlock();
            return 0;
        }
        /* Unregistered space: the caller may free it, as before. */
        untracked_puts++;
        aspace_unlock();
        return 1;
    }
    if (s->refs == 0) {
        /* Double put: refuse to authorise a second free. */
        aspace_unlock();
        return 0;
    }
    s->refs--;
    if (s->refs == 0) {
        s->pml4 = 0;                        /* free the slot; probe runs stop here */
        tombstone_remember(pml4);
        last_puts++;
        aspace_unlock();
        return 1;
    }
    if (s->refs == 1) shared_spaces--;
    aspace_unlock();
    return 0;
}

int aspace_refs(uint64_t pml4) {
    if (pml4 == 0) return 0;
    aspace_lock();
    aslot_t *s = find_locked(pml4);
    int r = s ? (int)s->refs : 0;
    aspace_unlock();
    return r;
}

uint64_t aspace_shared_spaces(void) {
    aspace_lock();
    uint64_t v = shared_spaces;
    aspace_unlock();
    return v;
}

uint64_t aspace_untracked_puts(void) {
    aspace_lock();
    uint64_t v = untracked_puts;
    aspace_unlock();
    return v;
}

uint64_t aspace_last_puts(void) {
    aspace_lock();
    uint64_t v = last_puts;
    aspace_unlock();
    return v;
}

uint64_t aspace_double_puts(void) {
    aspace_lock();
    uint64_t v = double_puts;
    aspace_unlock();
    return v;
}

uint64_t aspace_live_spaces(void) {
    uint64_t n = 0;
    aspace_lock();
    for (unsigned i = 0; i < ASPACE_SLOTS; i++) {
        if (slots[i].pml4) n++;
    }
    aspace_unlock();
    return n;
}
