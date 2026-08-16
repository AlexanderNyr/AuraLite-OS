#ifndef AURALITE_NET_TCP_M6E_H
#define AURALITE_NET_TCP_M6E_H

#include <stdint.h>

/*
 * MATURITY_PLAN.md M6e — the last three items: listen backlog,
 * SO_REUSEADDR, and keepalive.
 *
 * A bug found while scoping this, worth naming because it is the reason
 * the backlog matters at all: tcp_listen() never checked whether the port
 * was already in use.  Two listens on the same port both "succeeded", and
 * whichever call happened to poll first stole the SYN -- a silent,
 * order-dependent hijack rather than the EADDRINUSE the caller expected.
 * SO_REUSEADDR only means anything once that check exists, so the check
 * comes first and the option relaxes it.
 *
 * Policy lives here as pure functions over plain integers, the pattern
 * tcp_x5.h / tcp_m6*.h established, so it is testable without a NIC.
 */

/* ------------------------------------------------ listen backlog ----- */

/*
 * Completed connections waiting for accept().  Depth is deliberately
 * small: this is a hobby stack whose accept loop is a shell command, and
 * a deep queue would only let more peers time out while enqueued.
 */
#define TCPM6E_BACKLOG_MAX 8u

typedef struct {
    uint32_t peer_ip;
    uint16_t peer_port;
    uint32_t peer_seq;      /* their ISN, so accept() can build the ACK */
    uint8_t  in_use;
} tcpm6e_pending_t;

typedef struct {
    tcpm6e_pending_t q[TCPM6E_BACKLOG_MAX];
    uint32_t head;
    uint32_t count;
    uint32_t limit;         /* the backlog the caller asked for */
    uint32_t dropped;       /* SYNs refused because the queue was full */
} tcpm6e_backlog_t;

static inline void tcpm6e_backlog_init(tcpm6e_backlog_t *b, uint32_t limit) {
    for (uint32_t i = 0; i < TCPM6E_BACKLOG_MAX; i++) b->q[i].in_use = 0;
    b->head = 0;
    b->count = 0;
    b->dropped = 0;
    /* listen(fd, 0) is legal and means "one", per common practice; and no
     * caller may exceed the compiled-in ceiling. */
    if (limit == 0) limit = 1;
    b->limit = limit > TCPM6E_BACKLOG_MAX ? TCPM6E_BACKLOG_MAX : limit;
}

static inline int tcpm6e_backlog_full(const tcpm6e_backlog_t *b) {
    return b->count >= b->limit;
}

/* Returns 0 on success, -1 when the queue is full (the SYN is dropped, and
 * the peer will retransmit it -- that is the designed behaviour). */
static inline int tcpm6e_backlog_push(tcpm6e_backlog_t *b, uint32_t ip,
                                      uint16_t port, uint32_t seq) {
    if (tcpm6e_backlog_full(b)) {
        b->dropped++;
        return -1;
    }
    uint32_t idx = (b->head + b->count) % TCPM6E_BACKLOG_MAX;
    b->q[idx].peer_ip = ip;
    b->q[idx].peer_port = port;
    b->q[idx].peer_seq = seq;
    b->q[idx].in_use = 1;
    b->count++;
    return 0;
}

/* Oldest first: a backlog is FIFO, so the peer that waited longest is
 * served first.  Returns 0 when empty. */
static inline tcpm6e_pending_t *tcpm6e_backlog_pop(tcpm6e_backlog_t *b) {
    if (b->count == 0) return 0;
    tcpm6e_pending_t *p = &b->q[b->head];
    b->head = (b->head + 1) % TCPM6E_BACKLOG_MAX;
    b->count--;
    p->in_use = 0;
    return p;
}

/*
 * A duplicate SYN from a peer already queued must not take a second slot:
 * a retransmitted SYN (which is exactly what a peer does when its first
 * one is dropped) would otherwise fill the backlog with one client.
 */
static inline int tcpm6e_backlog_has(const tcpm6e_backlog_t *b, uint32_t ip,
                                     uint16_t port) {
    for (uint32_t i = 0; i < b->count; i++) {
        const tcpm6e_pending_t *p = &b->q[(b->head + i) % TCPM6E_BACKLOG_MAX];
        if (p->peer_ip == ip && p->peer_port == port) return 1;
    }
    return 0;
}

/* ------------------------------------------------- SO_REUSEADDR ------ */

/*
 * Whether a bind/listen on `port` may proceed.
 *
 * The rule this encodes: a port held by a LIVE listener is never available,
 * with or without SO_REUSEADDR.  A port held only by a connection in
 * TIME_WAIT is available when the caller set SO_REUSEADDR -- that is the
 * option's entire purpose, letting a server restart without waiting out
 * 2*MSL.
 *
 * `existing_state` uses the tcp_state_t values; the header cannot include
 * tcp.h (which includes this), so the caller passes an int and the two
 * states that matter are named below.
 */
#define TCPM6E_ST_FREE       0
#define TCPM6E_ST_LISTENING  1
#define TCPM6E_ST_TIME_WAIT  2
#define TCPM6E_ST_ACTIVE     3

typedef enum {
    TCPM6E_BIND_OK,
    TCPM6E_BIND_IN_USE,        /* -EADDRINUSE */
} tcpm6e_bind_t;

static inline tcpm6e_bind_t tcpm6e_can_bind(int existing_state,
                                            int reuseaddr) {
    switch (existing_state) {
    case TCPM6E_ST_FREE:
        return TCPM6E_BIND_OK;
    case TCPM6E_ST_TIME_WAIT:
        /* The whole point of SO_REUSEADDR. */
        return reuseaddr ? TCPM6E_BIND_OK : TCPM6E_BIND_IN_USE;
    case TCPM6E_ST_LISTENING:
    case TCPM6E_ST_ACTIVE:
    default:
        /* SO_REUSEADDR does NOT let two live listeners share a port.
         * (That is SO_REUSEPORT, which is a different option and is not
         * implemented here.)  Returning OK would resurrect exactly the
         * silent hijack this phase fixes. */
        return TCPM6E_BIND_IN_USE;
    }
}

/* --------------------------------------------------- keepalive ------- */

/*
 * RFC 1122 s4.2.3.6.  A connection idle for `idle` sends a probe; if
 * `count` probes go unanswered at `intvl` apart, the connection is dead.
 *
 * The RFC insists the default idle time be no less than two hours,
 * specifically so that keepalive cannot be mistaken for a heartbeat.  Two
 * hours is useless for a test gate, so the values are configurable per
 * connection and the default is stated rather than silently shortened.
 */
#define TCPM6E_KA_IDLE_MS   7200000u   /* 2 hours, per RFC 1122 */
#define TCPM6E_KA_INTVL_MS    75000u   /* 75 s between probes */
#define TCPM6E_KA_COUNT           9u   /* then declare it dead */

typedef struct {
    uint8_t  enabled;
    uint32_t idle_ms;
    uint32_t intvl_ms;
    uint32_t count;
    uint32_t probes_sent;
    uint32_t last_activity;   /* tick of the last segment either way */
    uint32_t last_probe;      /* tick of the last probe we sent */
} tcpm6e_ka_t;

static inline void tcpm6e_ka_init(tcpm6e_ka_t *k, uint32_t now) {
    k->enabled = 0;
    k->idle_ms = TCPM6E_KA_IDLE_MS;
    k->intvl_ms = TCPM6E_KA_INTVL_MS;
    k->count = TCPM6E_KA_COUNT;
    k->probes_sent = 0;
    k->last_activity = now;
    k->last_probe = 0;
}

/* Any traffic on the connection resets the idle timer and the probe run. */
static inline void tcpm6e_ka_activity(tcpm6e_ka_t *k, uint32_t now) {
    k->last_activity = now;
    k->probes_sent = 0;
}

typedef enum {
    TCPM6E_KA_IDLE_STILL,   /* nothing to do */
    TCPM6E_KA_SEND_PROBE,   /* send a keepalive probe now */
    TCPM6E_KA_DEAD          /* probes exhausted: drop the connection */
} tcpm6e_ka_action_t;

static inline tcpm6e_ka_action_t tcpm6e_ka_poll(tcpm6e_ka_t *k, uint32_t now,
                                                uint32_t ms_per_tick) {
    if (!k->enabled) return TCPM6E_KA_IDLE_STILL;

    if (k->probes_sent == 0) {
        uint32_t idle = (now - k->last_activity) * ms_per_tick;
        if (idle < k->idle_ms) return TCPM6E_KA_IDLE_STILL;
        return TCPM6E_KA_SEND_PROBE;
    }

    /* A probe run is under way. */
    if (k->probes_sent >= k->count) return TCPM6E_KA_DEAD;
    uint32_t since = (now - k->last_probe) * ms_per_tick;
    if (since < k->intvl_ms) return TCPM6E_KA_IDLE_STILL;
    return TCPM6E_KA_SEND_PROBE;
}

static inline void tcpm6e_ka_probe_sent(tcpm6e_ka_t *k, uint32_t now) {
    k->probes_sent++;
    k->last_probe = now;
}

#endif /* AURALITE_NET_TCP_M6E_H */
