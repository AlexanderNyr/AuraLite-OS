#ifndef AURALITE_NET_DUALSTACK_H
#define AURALITE_NET_DUALSTACK_H

/*
 * dualstack.h — Y3 address-selection core (REALINTERNET2).
 *
 * Pure C, host-tested.  No happy-eyeballs racing (plan §4 named
 * non-goal): pick once, then fall back serially.  v6 is preferred
 * when a global address exists AND an AAAA was learned — the R9
 * source-selection floor.  Otherwise v4.  A lone AAAA still wins
 * over nothing.
 */

#define DS_NONE 0
#define DS_V4   4
#define DS_V6   6

static inline int dualstack_pick(int have_global, int have_aaaa, int have_a) {
    if (have_global && have_aaaa) return DS_V6;
    if (have_a) return DS_V4;
    if (have_aaaa) return DS_V6;
    return DS_NONE;
}

#endif /* AURALITE_NET_DUALSTACK_H */
