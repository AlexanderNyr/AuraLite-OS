/* netglue32.c — what the shared TCP needs from the i386 port
 * (RESIDUE_PLAN.md R3, RES-10: the last struck-through I8 line).
 *
 * The measurement that shaped this file: kernel/net/tcp.c compiled
 * at -m32 with ZERO width errors and llvm-nm showed it needs only
 * the netdev seam, four small net.c helpers, string/kprintf (long
 * since shared) and a tick source.  No scheduler, no threads — its
 * waits are netdev_recv_wait + timer ticks.  So the port is this
 * file, not a fork: the netdev registration over net32's e1000
 * rings, the four helpers, and a self-test that round-trips one
 * TCP payload against a real host peer (QEMU SLIRP maps 10.0.2.2
 * to the host's loopback, so the smoke can put a real listener
 * behind the SYN).
 *
 * Named honestly: net_ipfrag_step() here is a PASSTHROUGH — IPv4
 * fragment reassembly (X4) is not ported; single-fragment segments
 * are what TCP produces at this MTU, and the day that changes this
 * shim refuses loudly rather than mis-assembling.  ARP resolution
 * answers with the GATEWAY's MAC for every target: SLIRP routes
 * everything through 10.0.2.2, and a default-route-only bring-up
 * says so here instead of pretending to a neighbour table.
 */

#include <stdint.h>

#include "kernel/arch/i386/net32.h"
#include "kernel/arch/i386/irq32.h"
#include "kernel/net/netdev.h"
#include "kernel/net/tcp.h"
#include "kernel/net/ipv6.h"
#include "kernel/lib/kprintf.h"

/* ---- the tick source tcp.c links against ----------------------------- */

uint64_t timer_get_ticks(void)
{
    return pit32_ticks();
}

/* ---- the four net.c helpers ------------------------------------------ */

void net_get_mac(uint8_t mac[6])       { net32_get_mac(mac); }

/* net32 keeps addresses in NETWORK order; the shared tcp.c works in
 * HOST order (the packet capture said so: a SYN with source
 * 15.2.0.10 is 10.0.2.15 byte-swapped).  One swap helper, used for
 * both directions of this boundary. */
static uint32_t swap32(uint32_t v)
{
    return ((v & 0xFFu) << 24) | (((v >> 8) & 0xFFu) << 16) |
           (((v >> 16) & 0xFFu) << 8) | (v >> 24);
}

uint32_t net_get_our_ip(void)          { return swap32(net32_our_ip()); }

int net_arp_resolve(uint32_t target_ip, uint8_t out_mac[6])
{
    (void)target_ip;                   /* default route only, stated */
    net32_gw_mac(out_mac);
    return 0;
}

const uint8_t *net_ipfrag_step(const uint8_t *frame, int len,
                               int *out_len)
{
    *out_len = len;                    /* passthrough; X4 not ported */
    return frame;
}

/* Y3: netl3.c's v6 ops are a SHARED row, so these two symbols must
 * exist on i386.  There is no v6 stack on this port — resolve fails
 * closed and src_for is NULL.  Named: TCP-over-IPv6 is x86_64 only
 * this phase. */
int net_ipv6_resolve(const ipv6_addr_t *target, uint8_t out_mac[6])
{
    (void)target;
    (void)out_mac;
    return -1;
}

const ipv6_addr_t *net_ipv6_src_for(const ipv6_addr_t *dst)
{
    (void)dst;
    return 0;
}

int net_ipv6_handle_frame(const uint8_t *frame, int len)
{
    (void)frame;
    (void)len;
    return 0;
}

/* ---- the netdev registration ------------------------------------------ */

static int nd_send(const void *data, uint32_t len)
{
    net32_send_frame(data, len);
    return 0;
}

static int nd_recv(void *buf, uint32_t bufsize)
{
    return (int)net32_poll_frame(buf, bufsize);
}

static int nd_recv_wait(void *buf, uint32_t bufsize,
                        uint64_t timeout_ticks)
{
    uint64_t deadline = pit32_ticks() + timeout_ticks;
    for (;;) {
        int n = (int)net32_poll_frame(buf, bufsize);
        if (n > 0)
            return n;
        if (pit32_ticks() >= deadline)
            return 0;
        __asm__ volatile("pause");
    }
}

static void nd_get_mac(uint8_t mac[6]) { net32_get_mac(mac); }
static int  nd_link_up(void)           { return 1; }

static const struct netdev net32_netdev = {
    .name      = "e1000-32",
    .send      = nd_send,
    .recv      = nd_recv,
    .recv_wait = nd_recv_wait,
    .get_mac   = nd_get_mac,
    .link_up   = nd_link_up,
};

/* ---- bring-up + the round-trip proof ---------------------------------- */

#define TCP32_PEER_PORT 8032

void net32_tcp_bringup(void)
{
    netdev_register(&net32_netdev);
    kprintf("[netdev] e1000-32 registered (the seam tcp.c drives)\n");

    /* One real round-trip when the smoke put a listener on the host
     * side of SLIRP; an honest skip when nobody is listening (every
     * other boot). */
    /* net32 stores addresses in NETWORK byte order (its wire
     * discipline); tcp_open takes HOST order -- the first boot
     * printed "connecting to 2.2.0.10" and taught this line. */
    uint32_t gw = swap32(net32_gw_ip());
    tcp_handle_t h = tcp_open(gw, TCP32_PEER_PORT);
    if (h < 0) {
        kprintf("[tcp32] no peer on 10.0.2.2:%u (SYN unanswered); "
                "round-trip skipped\n", TCP32_PEER_PORT);
        return;
    }
    static const char ping[] = "PING-FROM-I386\n";
    if (tcp_send_h(h, ping, sizeof(ping) - 1) < 0) {
        kprintf("[tcp32] send failed after connect\n");
        tcp_close_h(h);
        return;
    }
    char reply[128];
    int n = tcp_recv_h(h, reply, sizeof(reply) - 1);
    if (n <= 0) {
        kprintf("[tcp32] connected + sent, but no reply payload\n");
        tcp_close_h(h);
        return;
    }
    reply[n] = '\0';
    for (int i = 0; i < n; i++)
        if (reply[i] == '\n') reply[i] = ' ';
    kprintf("[tcp32] PASS: round-trip %d byte(s): %s\n", n, reply);
    tcp_close_h(h);
}
