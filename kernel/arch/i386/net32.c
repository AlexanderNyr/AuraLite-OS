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
#include "kernel/net/miniproto.h"

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

static uint32_t our_ip, gw_ip;          /* network byte order */
static uint8_t  gw_mac[6];

/* The protocol bodies moved to kernel/net/miniproto.c (RISCV_PLAN V7
 * lifted them for the rv64 NIC -- second consumer, same rule as the
 * smallsh promotion).  The PACKETS are unchanged; this file keeps its
 * own log lines, which is what keeps the I8 smoke asserts byte-
 * identical across the refactor. */

static void ops_send(const uint8_t *frame, uint32_t len)
{
    net_send(frame, len);
}
static uint32_t ops_poll(uint8_t *out, uint32_t cap)
{
    return net_poll(out, cap);
}
static uint64_t ops_ticks(void)
{
    return pit32_ticks();
}
static void ops_relax(void)
{
    __asm__ volatile("pause");
}

static const struct miniproto_ops net32_ops = {
    .send    = ops_send,
    .poll    = ops_poll,
    .ticks   = ops_ticks,
    .relax   = ops_relax,
    .tick_hz = 100,
    .mac     = our_mac,
};

static int dhcp_run(void)
{
    if (miniproto_dhcp(&net32_ops, &our_ip, &gw_ip) != 0)
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
    if (miniproto_arp_gw(&net32_ops, our_ip, gw_ip, gw_mac) != 0)
        return -1;
    kprintf32("[net] ARP: gateway is %b:%b:%b:%b:%b:%b\n",
              gw_mac[0], gw_mac[1], gw_mac[2],
              gw_mac[3], gw_mac[4], gw_mac[5]);
    return 0;
}

static int icmp_ping_gw(void)
{
    static const uint8_t payload[] = "auralite-i386-ping";
    return miniproto_icmp_ping(&net32_ops, our_ip, gw_ip, gw_mac,
                               payload, sizeof(payload));
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
