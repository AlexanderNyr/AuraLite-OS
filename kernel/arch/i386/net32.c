/* kernel/arch/i386/net32.c -- e1000 bring-up + DHCP/ARP/ICMP
 * (I386_PLAN I8).  See net32.h for scope.
 *
 * Ring geometry mirrors drivers/e1000/e1000.c's at minimum size:
 * 8 RX + 8 TX legacy descriptors, 2 KiB buffers, all allocated from
 * the PMM low region so the direct map reaches them and the NIC's
 * 32-bit DMA sees the same address (everything is below 896 MiB by
 * construction -- pmm32's horizon IS the direct map).
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/i386/net32.h"
#include "kernel/arch/i386/pmm32.h"
#include "kernel/arch/i386/paging32.h"
#include "kernel/arch/i386/kprintf32.h"
#include "kernel/arch/i386/irq32.h"
#include "drivers/pci/pci.h"

/* ---- MMIO ---------------------------------------------------------------- */

#define E1000_VENDOR 0x8086
#define E1000_82540EM 0x100E

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define CTRL_SLU   (1u << 6)

#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)

#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)

static volatile uint32_t *mmio;

static uint32_t rd32(uint32_t reg)            { return mmio[reg / 4]; }
static void     wr32(uint32_t reg, uint32_t v) { mmio[reg / 4] = v;    }

/* ---- rings ---------------------------------------------------------------- */

struct rx_desc {
    uint32_t addr_lo, addr_hi;      /* 64-bit on the wire (D6) */
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint32_t addr_lo, addr_hi;
    uint16_t length;
    uint8_t  cso, cmd, status, css;
    uint16_t special;
} __attribute__((packed));

#define NDESC    8
#define BUF_SIZE 2048

static struct rx_desc *rx_ring;
static struct tx_desc *tx_ring;
static uint8_t *rx_buf[NDESC];
static uint8_t *tx_buf[NDESC];
static uint32_t rx_next, tx_next;
static uint8_t our_mac[6];

/* ---- byte order ----------------------------------------------------------- */

static uint16_t htons16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t htonl32(uint32_t v)
{
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | (v >> 24);
}

/* ---- init ------------------------------------------------------------------ */

int net32_init(void)
{
    uint8_t bus, dev, fn;
    if (pci_find_device(E1000_VENDOR, E1000_82540EM, &bus, &dev, &fn) != 0) {
        kprintf32("[net] no e1000 (82540EM) on PCI; skipping\n");
        return -1;
    }

    uint32_t bar0 = pci_get_bar(bus, dev, fn, 0) & ~0xFu;
    pci_enable_bus_master(bus, dev, fn);

    /* BAR0 (0xFEBC0000-region on QEMU) is above the direct map: give
     * it 4 KiB-page mappings in the probe window.  128 KiB register
     * file = 32 pages. */
    for (uint32_t off = 0; off < 0x20000; off += PAGE_SIZE_32)
        paging32_map(0xFD000000u + off, bar0 + off, PAGE32_FLAG_WRITE);
    mmio = (volatile uint32_t *)0xFD000000u;

    /* MAC from RAL/RAH (QEMU pre-loads them; EEPROM read not needed). */
    uint32_t ral = rd32(REG_RAL0), rah = rd32(REG_RAH0);
    our_mac[0] = (uint8_t)ral;        our_mac[1] = (uint8_t)(ral >> 8);
    our_mac[2] = (uint8_t)(ral >> 16); our_mac[3] = (uint8_t)(ral >> 24);
    our_mac[4] = (uint8_t)rah;        our_mac[5] = (uint8_t)(rah >> 8);

    /* Rings + buffers: one 4 KiB frame carries both rings (8*16 = 128
     * bytes each); buffers get 2 KiB halves of frames. */
    uint32_t ring_frame = pmm32_alloc_frame();
    if (!ring_frame)
        return -1;
    rx_ring = (struct rx_desc *)p2v_32(ring_frame);
    tx_ring = (struct tx_desc *)p2v_32(ring_frame + 2048);

    for (int i = 0; i < NDESC; i++) {
        uint32_t f = pmm32_alloc_frame();
        if (!f)
            return -1;
        rx_buf[i] = (uint8_t *)p2v_32(f);
        rx_ring[i].addr_lo = f;
        rx_ring[i].addr_hi = 0;         /* explicit: D6, not struct luck */
        rx_ring[i].status  = 0;

        f = pmm32_alloc_frame();
        if (!f)
            return -1;
        tx_buf[i] = (uint8_t *)p2v_32(f);
        tx_ring[i].addr_lo = f;
        tx_ring[i].addr_hi = 0;
        tx_ring[i].status  = 1;         /* DD: free to use */
    }

    wr32(REG_CTRL, rd32(REG_CTRL) | CTRL_SLU);

    wr32(REG_RDBAL, ring_frame);
    wr32(REG_RDBAH, 0);
    wr32(REG_RDLEN, NDESC * 16);
    wr32(REG_RDH, 0);
    wr32(REG_RDT, NDESC - 1);
    wr32(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    wr32(REG_TDBAL, ring_frame + 2048);
    wr32(REG_TDBAH, 0);
    wr32(REG_TDLEN, NDESC * 16);
    wr32(REG_TDH, 0);
    wr32(REG_TDT, 0);
    wr32(REG_TCTL, TCTL_EN | TCTL_PSP);

    rx_next = 0;
    tx_next = 0;

    kprintf32("[net] e1000 82540EM at PCI %u:%u.%u, BAR0 %x, MAC "
              "%b:%b:%b:%b:%b:%b\n", bus, dev, fn, bar0,
              our_mac[0], our_mac[1], our_mac[2],
              our_mac[3], our_mac[4], our_mac[5]);
    return 0;
}

/* ---- raw send/poll --------------------------------------------------------- */

static void net_send(const uint8_t *frame, uint32_t len)
{
    struct tx_desc *d = &tx_ring[tx_next];
    while (!(d->status & 1)) { }        /* wait for DD */
    for (uint32_t i = 0; i < len; i++)
        tx_buf[tx_next][i] = frame[i];
    d->length = (uint16_t)len;
    d->cmd    = 0x0B;                   /* EOP | IFCS | RS */
    d->status = 0;
    tx_next = (tx_next + 1) % NDESC;
    wr32(REG_TDT, tx_next);
}

/* Returns length (>0) with the frame copied out, or 0 when idle. */
static uint32_t net_poll(uint8_t *out, uint32_t cap)
{
    struct rx_desc *d = &rx_ring[rx_next];
    if (!(d->status & 1))
        return 0;
    uint32_t len = d->length;
    if (len > cap)
        len = cap;
    for (uint32_t i = 0; i < len; i++)
        out[i] = rx_buf[rx_next][i];
    d->status = 0;
    wr32(REG_RDT, rx_next);
    rx_next = (rx_next + 1) % NDESC;
    return len;
}

/* ---- minimal protocols (self-test scope) ----------------------------------- */

static uint16_t ip_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
    if (len & 1)
        sum += (uint32_t)data[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint32_t our_ip, gw_ip;          /* network byte order */
static uint8_t  gw_mac[6];

/* Spin-poll with a PIT deadline; returns len or 0 on timeout. */
static uint32_t poll_until(uint8_t *buf, uint32_t cap, uint32_t ticks)
{
    uint32_t deadline = pit32_ticks() + ticks;
    while (pit32_ticks() < deadline) {
        uint32_t n = net_poll(buf, cap);
        if (n)
            return n;
        __asm__ volatile("pause");
    }
    return 0;
}

/* DHCP: fixed-format DISCOVER/REQUEST over UDP broadcast.  SLIRP's
 * server needs nothing fancier. */
static uint32_t build_dhcp(uint8_t *f, int request, uint32_t offered,
                           uint32_t server)
{
    static const uint8_t xid[4] = { 0xAA, 0x33, 0x55, 0x77 };
    uint32_t i;

    /* Ethernet: broadcast. */
    for (i = 0; i < 6; i++) f[i] = 0xFF;
    for (i = 0; i < 6; i++) f[6 + i] = our_mac[i];
    f[12] = 0x08; f[13] = 0x00;

    uint8_t *ip = f + 14;
    uint8_t *udp = ip + 20;
    uint8_t *bp  = udp + 8;

    /* BOOTP fixed part (236 bytes) + magic + options. */
    for (i = 0; i < 300; i++) bp[i] = 0;
    bp[0] = 1; bp[1] = 1; bp[2] = 6;                 /* op, htype, hlen */
    for (i = 0; i < 4; i++) bp[4 + i] = xid[i];
    bp[10] = 0x80;                                   /* BROADCAST flag */
    for (i = 0; i < 6; i++) bp[28 + i] = our_mac[i];
    bp[236] = 99; bp[237] = 130; bp[238] = 83; bp[239] = 99;  /* magic */

    uint32_t o = 240;
    bp[o++] = 53; bp[o++] = 1; bp[o++] = request ? 3 : 1;     /* msg type */
    if (request) {
        bp[o++] = 50; bp[o++] = 4;                            /* requested IP */
        bp[o++] = (uint8_t)(offered); bp[o++] = (uint8_t)(offered >> 8);
        bp[o++] = (uint8_t)(offered >> 16); bp[o++] = (uint8_t)(offered >> 24);
        bp[o++] = 54; bp[o++] = 4;                            /* server id */
        bp[o++] = (uint8_t)(server); bp[o++] = (uint8_t)(server >> 8);
        bp[o++] = (uint8_t)(server >> 16); bp[o++] = (uint8_t)(server >> 24);
    }
    bp[o++] = 255;
    uint32_t blen = o;

    uint32_t ulen = 8 + blen;
    udp[0] = 0; udp[1] = 68;            /* src 68 */
    udp[2] = 0; udp[3] = 67;            /* dst 67 */
    udp[4] = (uint8_t)(ulen >> 8); udp[5] = (uint8_t)ulen;
    udp[6] = 0; udp[7] = 0;             /* checksum optional over IPv4 */

    uint32_t iplen = 20 + ulen;
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (uint8_t)(iplen >> 8); ip[3] = (uint8_t)iplen;
    ip[4] = 0; ip[5] = 0; ip[6] = 0; ip[7] = 0;
    ip[8] = 64; ip[9] = 17;             /* TTL, UDP */
    ip[10] = 0; ip[11] = 0;
    for (i = 12; i < 16; i++) ip[i] = 0;          /* 0.0.0.0 */
    for (i = 16; i < 20; i++) ip[i] = 0xFF;       /* 255.255.255.255 */
    uint16_t cs = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(cs >> 8); ip[11] = (uint8_t)cs;

    return 14 + iplen;
}

static int dhcp_run(void)
{
    static uint8_t f[1536], r[1536];
    uint32_t offered = 0, server = 0;

    for (int attempt = 0; attempt < 3 && !our_ip; attempt++) {
        net_send(f, build_dhcp(f, 0, 0, 0));

        uint32_t until = pit32_ticks() + 100;      /* 1 s per attempt */
        while (pit32_ticks() < until) {
            uint32_t n = poll_until(r, sizeof(r), 20);
            if (n < 14 + 20 + 8 + 240)
                continue;
            /* UDP to port 68 with our xid? */
            const uint8_t *ip = r + 14, *udp = ip + 20, *bp = udp + 8;
            if (r[12] != 0x08 || r[13] != 0x00 || ip[9] != 17)
                continue;
            if (udp[2] != 0 || udp[3] != 68)
                continue;
            if (bp[4] != 0xAA || bp[5] != 0x33)
                continue;
            /* Option walk for message type + server id. */
            uint32_t o = 240;
            uint8_t mtype = 0;
            uint32_t sid = 0;
            uint32_t limit = n - 14 - 20 - 8;
            while (o + 1 < limit && bp[o] != 255) {
                uint8_t opt = bp[o], olen = bp[o + 1];
                if (opt == 53 && olen == 1)
                    mtype = bp[o + 2];
                if (opt == 54 && olen == 4)
                    sid = (uint32_t)bp[o + 2] | ((uint32_t)bp[o + 3] << 8) |
                          ((uint32_t)bp[o + 4] << 16) | ((uint32_t)bp[o + 5] << 24);
                o += 2 + olen;
            }
            uint32_t yiaddr = (uint32_t)bp[16] | ((uint32_t)bp[17] << 8) |
                              ((uint32_t)bp[18] << 16) | ((uint32_t)bp[19] << 24);
            if (mtype == 2 && !offered) {                  /* OFFER */
                offered = yiaddr; server = sid;
                net_send(f, build_dhcp(f, 1, offered, server));
            } else if (mtype == 5 && offered) {            /* ACK */
                our_ip = offered;
                gw_ip  = server;   /* SLIRP: server 10.0.2.2 IS the gw */
                break;
            }
        }
    }

    if (!our_ip)
        return -1;
    kprintf32("[net] DHCP lease: %u.%u.%u.%u (gw %u.%u.%u.%u)\n",
              our_ip & 0xFF, (our_ip >> 8) & 0xFF,
              (our_ip >> 16) & 0xFF, our_ip >> 24,
              gw_ip & 0xFF, (gw_ip >> 8) & 0xFF,
              (gw_ip >> 16) & 0xFF, gw_ip >> 24);
    return 0;
}

static int arp_resolve_gw(void)
{
    static uint8_t f[64], r[1536];
    uint32_t i;

    for (i = 0; i < 6; i++) f[i] = 0xFF;
    for (i = 0; i < 6; i++) f[6 + i] = our_mac[i];
    f[12] = 0x08; f[13] = 0x06;
    uint8_t *a = f + 14;
    a[0] = 0; a[1] = 1; a[2] = 8; a[3] = 0; a[4] = 6; a[5] = 4;
    a[6] = 0; a[7] = 1;                                /* request */
    for (i = 0; i < 6; i++) a[8 + i] = our_mac[i];
    a[14] = (uint8_t)our_ip; a[15] = (uint8_t)(our_ip >> 8);
    a[16] = (uint8_t)(our_ip >> 16); a[17] = (uint8_t)(our_ip >> 24);
    for (i = 0; i < 6; i++) a[18 + i] = 0;
    a[24] = (uint8_t)gw_ip; a[25] = (uint8_t)(gw_ip >> 8);
    a[26] = (uint8_t)(gw_ip >> 16); a[27] = (uint8_t)(gw_ip >> 24);

    for (int attempt = 0; attempt < 3; attempt++) {
        net_send(f, 42);
        uint32_t n = poll_until(r, sizeof(r), 50);
        if (n >= 42 && r[12] == 0x08 && r[13] == 0x06 &&
            r[14 + 7] == 2) {                          /* reply */
            for (i = 0; i < 6; i++)
                gw_mac[i] = r[14 + 8 + i];
            kprintf32("[net] ARP: gateway is %b:%b:%b:%b:%b:%b\n",
                      gw_mac[0], gw_mac[1], gw_mac[2],
                      gw_mac[3], gw_mac[4], gw_mac[5]);
            return 0;
        }
    }
    return -1;
}

static int icmp_ping_gw(void)
{
    static uint8_t f[128], r[1536];
    uint32_t i;

    for (i = 0; i < 6; i++) f[i] = gw_mac[i];
    for (i = 0; i < 6; i++) f[6 + i] = our_mac[i];
    f[12] = 0x08; f[13] = 0x00;

    uint8_t *ip = f + 14, *ic = ip + 20;
    static const uint8_t payload[] = "auralite-i386-ping";
    uint32_t plen = sizeof(payload);
    uint32_t iclen = 8 + plen;

    ic[0] = 8; ic[1] = 0; ic[2] = 0; ic[3] = 0;        /* echo request */
    ic[4] = 0x13; ic[5] = 0x86;                        /* id */
    ic[6] = 0; ic[7] = 1;                              /* seq */
    for (i = 0; i < plen; i++) ic[8 + i] = payload[i];
    uint16_t cs = ip_checksum(ic, iclen);
    ic[2] = (uint8_t)(cs >> 8); ic[3] = (uint8_t)cs;

    uint32_t iplen = 20 + iclen;
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (uint8_t)(iplen >> 8); ip[3] = (uint8_t)iplen;
    ip[4] = 0; ip[5] = 0; ip[6] = 0; ip[7] = 0;
    ip[8] = 64; ip[9] = 1;                             /* ICMP */
    ip[10] = 0; ip[11] = 0;
    ip[12] = (uint8_t)our_ip; ip[13] = (uint8_t)(our_ip >> 8);
    ip[14] = (uint8_t)(our_ip >> 16); ip[15] = (uint8_t)(our_ip >> 24);
    ip[16] = (uint8_t)gw_ip; ip[17] = (uint8_t)(gw_ip >> 8);
    ip[18] = (uint8_t)(gw_ip >> 16); ip[19] = (uint8_t)(gw_ip >> 24);
    cs = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(cs >> 8); ip[11] = (uint8_t)cs;

    for (int attempt = 0; attempt < 3; attempt++) {
        net_send(f, 14 + iplen);
        uint32_t n = poll_until(r, sizeof(r), 50);
        if (n >= 14 + 20 + 8 &&
            r[12] == 0x08 && r[13] == 0x00 &&
            r[14 + 9] == 1 &&                          /* ICMP */
            r[14 + 20] == 0 &&                         /* echo reply */
            r[14 + 20 + 4] == 0x13 && r[14 + 20 + 5] == 0x86) {
            /* Payload must round-trip byte for byte. */
            for (i = 0; i < plen; i++)
                if (r[14 + 20 + 8 + i] != payload[i])
                    return -1;
            return 0;
        }
    }
    return -1;
}

int net32_selftest(void)
{
    (void)htons16; (void)htonl32;

    kprintf32("[net] self-test: DHCP -> ARP -> ICMP echo...\n");
    if (dhcp_run() != 0) {
        kprintf32("[net] FAIL: no DHCP lease\n");
        return -1;
    }
    if (arp_resolve_gw() != 0) {
        kprintf32("[net] FAIL: gateway ARP unanswered\n");
        return -1;
    }
    if (icmp_ping_gw() != 0) {
        kprintf32("[net] FAIL: no ICMP echo reply\n");
        return -1;
    }
    kprintf32("[net] PASS: lease + ARP + echo reply (payload verified)\n");
    return 0;
}
