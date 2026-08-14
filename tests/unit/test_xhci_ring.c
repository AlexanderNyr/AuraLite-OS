/* test_xhci_ring.c — USB_PLAN U1 gate: event-ring cycle/wrap arithmetic.
 *
 * The xHCI event ring is consumed by comparing each TRB's Cycle bit against
 * a software Consumer Cycle State that inverts on every wrap.  That rule is
 * pure arithmetic, it is where the classic "works once, then hangs" xHCI
 * bug lives, and it does not need a controller to test — so it is tested
 * here, on the host, in the manner of test_w32_pe.c.
 *
 * This mirrors the algorithm in drivers/usb/xhci.c (xhci_ev_dequeue,
 * xhci_ev_advance).  The driver is freestanding kernel code that cannot be
 * linked into a host binary, so the model below is kept deliberately small
 * and its invariants are the ones the driver must also satisfy.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define RING_TRBS 256
#define TRB_CYCLE 1u
#define ERDP_BUSY (1u << 3)

struct trb {
    uint32_t param, status, control, flags;
};

/* ---- model of the consumer ---- */
typedef struct {
    struct trb ring[RING_TRBS];
    int idx;            /* dequeue index */
    int ccs;            /* consumer cycle state */
    uint64_t base;      /* physical base of the segment */
    uint64_t erdp;      /* last value written to ERDP */
    int erdp_writes;
} consumer_t;

static void cons_init(consumer_t *c, uint64_t base) {
    memset(c, 0, sizeof(*c));
    c->ccs = 1;
    c->base = base;
}

static void cons_advance(consumer_t *c) {
    c->idx++;
    if (c->idx >= RING_TRBS) {
        c->idx = 0;
        c->ccs ^= 1;
    }
    c->erdp = (c->base + (uint64_t)c->idx * sizeof(struct trb)) | ERDP_BUSY;
    c->erdp_writes++;
}

static int cons_dequeue(consumer_t *c, struct trb *out) {
    struct trb *t = &c->ring[c->idx];
    if ((t->flags & TRB_CYCLE) != (uint32_t)c->ccs) return -1;
    struct trb copy = *t;
    cons_advance(c);
    if (out) *out = copy;
    return 0;
}

/* ---- model of the producer (the controller) ---- */
typedef struct {
    int idx;
    int pcs;            /* producer cycle state */
} producer_t;

static void prod_init(producer_t *p) { p->idx = 0; p->pcs = 1; }

static void prod_post(producer_t *p, consumer_t *c, uint32_t payload) {
    struct trb *t = &c->ring[p->idx];
    t->param = payload;
    t->status = 0;
    t->control = 0;
    /* type 33 (Command Completion), cc=1 in the top byte of control */
    t->control = (1u << 24);
    t->flags = (33u << 10) | (uint32_t)p->pcs;
    p->idx++;
    if (p->idx >= RING_TRBS) {
        p->idx = 0;
        p->pcs ^= 1;
    }
}

/* ---- harness ---- */
static int checks = 0, failures = 0;

static void ok(int cond, const char *what) {
    checks++;
    if (cond) {
        printf("  PASS: %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    printf("== xHCI event-ring cycle/wrap arithmetic (USB_PLAN U1) ==\n");

    /* 1. An empty ring yields nothing: every TRB is zeroed, so cycle 0 !=
     *    ccs 1.  A consumer that ignored the cycle bit would happily
     *    "consume" 256 zero TRBs -- the bug this rule exists to prevent. */
    {
        consumer_t c; cons_init(&c, 0x100000);
        struct trb ev;
        ok(cons_dequeue(&c, &ev) == -1, "empty ring yields no event");
        ok(c.idx == 0, "empty ring does not advance the dequeue pointer");
        ok(c.erdp_writes == 0, "empty ring writes no ERDP");
    }

    /* 2. One posted event is consumed exactly once. */
    {
        consumer_t c; cons_init(&c, 0x100000);
        producer_t p; prod_init(&p);
        struct trb ev;
        prod_post(&p, &c, 0xAAAA);
        ok(cons_dequeue(&c, &ev) == 0, "posted event is consumed");
        ok(ev.param == 0xAAAA, "event payload is intact");
        ok(cons_dequeue(&c, &ev) == -1, "the same event is not consumed twice");
        ok(c.idx == 1, "dequeue pointer advanced by one");
    }

    /* 3. ERDP is written back after each consume, with EHB set (RW1C) and
     *    pointing at the NEXT TRB.  Omitting this stalls real hardware. */
    {
        consumer_t c; cons_init(&c, 0x200000);
        producer_t p; prod_init(&p);
        struct trb ev;
        prod_post(&p, &c, 1);
        cons_dequeue(&c, &ev);
        ok((c.erdp & ERDP_BUSY) != 0, "ERDP write clears EHB (writes it as 1)");
        ok((c.erdp & ~(uint64_t)ERDP_BUSY) == 0x200000 + sizeof(struct trb),
           "ERDP points at the next TRB");
    }

    /* 4. The wrap: exactly RING_TRBS events fill the segment, and the
     *    consumer's cycle must invert.  This is the case a driver that
     *    only issues a few commands never reaches. */
    {
        consumer_t c; cons_init(&c, 0x300000);
        producer_t p; prod_init(&p);
        struct trb ev;
        for (int i = 0; i < RING_TRBS; i++) prod_post(&p, &c, (uint32_t)i);
        int consumed = 0;
        while (cons_dequeue(&c, &ev) == 0) {
            if (ev.param != (uint32_t)consumed) break;
            consumed++;
        }
        ok(consumed == RING_TRBS, "all 256 events consumed in order");
        ok(c.idx == 0, "dequeue pointer wrapped to 0");
        ok(c.ccs == 0, "consumer cycle state inverted on wrap");
        ok(p.pcs == 0, "producer cycle state inverted on wrap");
    }

    /* 5. Past the wrap the producer writes cycle 0, and a consumer that
     *    failed to invert would now reject every TRB. */
    {
        consumer_t c; cons_init(&c, 0x400000);
        producer_t p; prod_init(&p);
        struct trb ev;
        for (int i = 0; i < RING_TRBS; i++) prod_post(&p, &c, (uint32_t)i);
        while (cons_dequeue(&c, &ev) == 0) { }
        prod_post(&p, &c, 0xBEEF);
        ok(cons_dequeue(&c, &ev) == 0, "event after the wrap is consumed");
        ok(ev.param == 0xBEEF, "post-wrap payload is intact");
        ok((c.ring[0].flags & TRB_CYCLE) == 0, "post-wrap TRB carries cycle 0");
    }

    /* 6. Two full laps: 512 events must all arrive, in order.  This is the
     *    256-No-Op guest gate expressed off-hardware. */
    {
        consumer_t c; cons_init(&c, 0x500000);
        producer_t p; prod_init(&p);
        struct trb ev;
        int got = 0;
        for (int i = 0; i < 2 * RING_TRBS; i++) {
            prod_post(&p, &c, (uint32_t)i);
            if (cons_dequeue(&c, &ev) == 0 && ev.param == (uint32_t)i) got++;
        }
        ok(got == 2 * RING_TRBS, "512 events across two laps, all in order");
        ok(c.ccs == 1, "cycle state back to 1 after two laps");
        ok(c.erdp_writes == 2 * RING_TRBS, "one ERDP write per consumed event");
    }

    /* 7. Interleaved produce/consume never desynchronises: the consumer
     *    must stop exactly when it catches up with the producer. */
    {
        consumer_t c; cons_init(&c, 0x600000);
        producer_t p; prod_init(&p);
        struct trb ev;
        int mismatches = 0, total = 0;
        for (int burst = 1; burst <= 40; burst++) {
            for (int i = 0; i < burst; i++) prod_post(&p, &c, (uint32_t)(total + i));
            int drained = 0;
            while (cons_dequeue(&c, &ev) == 0) {
                if (ev.param != (uint32_t)(total + drained)) mismatches++;
                drained++;
            }
            if (drained != burst) mismatches++;
            total += burst;
        }
        ok(mismatches == 0, "820 events in 40 ragged bursts, none lost or reordered");
        ok(c.idx == p.idx, "consumer caught up with the producer");
        ok(c.ccs == p.pcs, "cycle states agree after interleaving");
    }

    /* 8. Negative control: drop the `ccs ^= 1` on wrap and the consumer
     *    breaks — which is how the assertions above are known to test
     *    anything.
     *
     *    The failure is worse than a stall, and worth stating precisely.
     *    After one lap of 256 events the dequeue index returns to 0, where
     *    the *stale* TRB from lap 1 still carries cycle 1.  A consumer
     *    whose ccs is also still 1 accepts it again, and keeps going
     *    forever: it re-delivers events the controller already retired.
     *    That is a silent data-corruption bug, not a hang, and it is why
     *    the cycle bit — not an index comparison — is the ownership rule.
     *
     *    (The first draft of this test asserted a stall after 256, then
     *    posted 512 events into a 256-entry ring so every slot had been
     *    overwritten with cycle 0 and the broken consumer stopped at once.
     *    Both the expectation and the setup were wrong; running it is what
     *    exposed that.) */
    {
        consumer_t c; cons_init(&c, 0x700000);
        producer_t p; prod_init(&p);
        for (int i = 0; i < RING_TRBS; i++) prod_post(&p, &c, (uint32_t)i);

        int consumed = 0;
        const int cap = 4 * RING_TRBS;      /* bound the runaway */
        while (consumed < cap) {
            struct trb *t = &c.ring[c.idx];
            if ((t->flags & TRB_CYCLE) != (uint32_t)c.ccs) break;
            c.idx++;
            if (c.idx >= RING_TRBS) c.idx = 0;   /* deliberately no ccs ^= 1 */
            consumed++;
        }
        ok(consumed == cap,
           "negative control: a non-inverting consumer re-delivers stale events forever");

        /* The correct consumer, same ring, stops after exactly one lap. */
        consumer_t good; cons_init(&good, 0x700000);
        memcpy(good.ring, c.ring, sizeof(good.ring));
        struct trb ev;
        int n = 0;
        while (cons_dequeue(&good, &ev) == 0 && n < cap) n++;
        ok(n == RING_TRBS,
           "the real consumer stops after exactly one lap on the same ring");
    }

    printf("== %d/%d passed ==\n", checks - failures, checks);
    return failures ? 1 : 0;
}
