#ifndef AURALITE_NET_IP_REASM_H
#define AURALITE_NET_IP_REASM_H

/*
 * ip_reasm.h — pure IPv4 fragment reassembly engine (REALINTERNET_PLAN
 * phase X4).
 *
 * Same discipline as dns_parse: no kernel includes, no global state, clock
 * injected.  The whole table lives in an ip_reasm_t owned by the caller, so
 * host unit tests drive it deterministically while the kernel instantiates
 * one static copy.
 *
 * Policy (per INTERNET_PLAN "no large allocation on a stranger's say-so"):
 *   - Bounded: at most IPREASM_MAX_ENTRIES concurrent datagrams, each capped
 *     at IPREASM_CAP bytes.  A fragment that would push the datagram past
 *     the cap is refused and poisons-kills its entry.
 *   - Timeout: entries older than the caller-supplied timeout are dropped
 *     (never held forever); expiry is checked lazily on input and eagerly
 *     by ipreasm_sweep().
 *   - Overlap: the first fragment wins for every byte.  A later fragment
 *     that covers the same byte with different content is dropped (RFC 1858
 *     family, e.g. teardrop, is refused); an identical retransmission is
 *     benign and ignored.
 *   - When the table is full, the oldest incomplete entry is evicted for a
 *     new first-fragment (bounded livelock-preventing LRU, counted).
 */

#include <stdint.h>

#ifdef AURALITE_IPREASM_HOST_TEST
#include <string.h>
#else
#include "kernel/lib/string.h"
#endif

#define IPREASM_MAX_ENTRIES 8
#define IPREASM_CAP         8192    /* max bytes per reassembled datagram */

/* Return codes of ipreasm_input(). */
#define IPREASM_PENDING   0   /* fragment stored, datagram still incomplete */
#define IPREASM_COMPLETE  1   /* datagram completed; copied to out */
#define IPREASM_REFUSED  (-1) /* fragment refused (overlap conflict, policy) */

typedef struct {
    uint32_t src;    /* values compared opaque; any consistent byte order */
    uint32_t dst;
    uint8_t  proto;
    uint16_t id;
} ipreasm_key_t;

typedef struct {
    uint8_t       used;
    ipreasm_key_t key;
    uint32_t      first_seen_ms;
    uint16_t      have_bytes;      /* distinct payload bytes received */
    uint16_t      expected_len;    /* 0 until the MF=0 fragment arrives */
    uint8_t       data[IPREASM_CAP];
    uint8_t       bitmap[IPREASM_CAP / 8];   /* 1 bit per byte position */
} ipreasm_entry_t;

typedef struct {
    ipreasm_entry_t e[IPREASM_MAX_ENTRIES];
    /* Diagnostics (monotonic counters since ipreasm_init). */
    uint32_t n_complete;
    uint32_t n_timeout;
    uint32_t n_overlap_refused;
    uint32_t n_cap_refused;
    uint32_t n_evicted;
} ipreasm_t;

void ipreasm_init(ipreasm_t *t);

/* Number of entries currently held (debug/tests). */
int ipreasm_entries(const ipreasm_t *t);

/*
 * Feed one fragment.
 *   key        — {src, dst, proto, id} from the IPv4 header
 *   expected_len_or_0 — pass 0 while MF is set; pass the true total payload
 *                  length when this is the last fragment (computed as
 *                  off_bytes + payload_len by the caller)
 *   off_bytes  — IPv4 fragment offset, bytes (field value * 8); must be a
 *                multiple of 8
 *   payload    — fragment payload bytes (IP payload of THIS fragment)
 *   out/out_cap — on IPREASM_COMPLETE the full datagram is written here
 *   out_len    — set to the datagram length on IPREASM_COMPLETE
 */
int ipreasm_input(ipreasm_t *t, uint32_t now_ms, uint32_t timeout_ms,
                  const ipreasm_key_t *key, uint16_t expected_len_or_0,
                  uint16_t off_bytes, const uint8_t *payload, uint16_t plen,
                  uint8_t *out, uint16_t out_cap, uint16_t *out_len);

/* Drop every entry older than timeout_ms. Returns the number dropped. */
int ipreasm_sweep(ipreasm_t *t, uint32_t now_ms, uint32_t timeout_ms);

#endif /* AURALITE_NET_IP_REASM_H */
