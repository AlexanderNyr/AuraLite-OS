/* kernel/arch/aarch64/vnet_a64.c -- virtio-net over mmio (ARM64_PLAN
 * A7; vnet_rv.c's shape over the PROMOTED transport).
 *
 * Two queues (0 = RX, 1 = TX), every frame prefixed by the 10-byte
 * virtio-net header (no features negotiated => the legacy 10-byte
 * layout, no mrg_rxbuf).  The PROTOCOLS above the frames are
 * kernel/net/miniproto.c -- the THIRD consumer (net32.c, vnet_rv.c,
 * now this file); the packets that satisfied SLIRP for two tenants
 * satisfy it for the fourth, and only the ops table is new.
 *
 * The transport keeps vmmio_probe's one-queue setup for queue 0 and
 * repeats the dance manually for queue 1 (the probe helper stays
 * simple; two call sites would not).  Log strings match vnet_rv.c's
 * byte for byte except the ping payload, which carries this arch's
 * name -- each tenant pings as itself so a crossed wire is visible.
 */

#include <stdint.h>

#include "kernel/arch/aarch64/vnet_a64.h"
#include "kernel/drivers/virtio_mmio.h"
#include "kernel/arch/aarch64/vmmio_a64.h"
#include "kernel/arch/aarch64/paging_a64.h"
#include "kernel/arch/aarch64/pmm_a64.h"
#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/trap_a64.h"
#include "kernel/arch/aarch64/irqflags.h"
#include "kernel/net/miniproto.h"

#define VNET_HDR_LEN 10
#define RX_BUFS 8
#define BUF_LEN 2048

static struct vmmio_dev rxq;           /* queue 0 on the same window */
static int ready;
static uint8_t our_mac[6];

/* TX ring state (queue 1, driven bare through the same window). */
static struct vring_desc  *txd;
static struct vring_avail *txa;
static struct vring_used  *txu;
static uint16_t tx_qsize, tx_last_used;

static uint64_t rxbuf_phys[RX_BUFS];
static uint8_t *rxbuf[RX_BUFS];
static uint64_t txbuf_phys;
static uint8_t *txbuf;

static inline uint32_t rd32(volatile uint8_t *b, uint32_t off)
{
    return *(volatile uint32_t *)(b + off);
}
static inline void wr32(volatile uint8_t *b, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(b + off) = v;
}

static inline void fence_a64(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void put_hexb_(uint8_t v)
{
    static const char hex[] = "0123456789abcdef";
    pl011_putc(hex[v >> 4]);
    pl011_putc(hex[v & 0xF]);
}

/* Queue-1 setup, the legacy contiguous-vring dance again. */
static int setup_tx_queue(void)
{
    volatile uint8_t *b = rxq.base;
    wr32(b, VM_QUEUE_SEL, 1);
    uint32_t maxq = rd32(b, VM_QUEUE_NUM_MAX);
    if (maxq == 0)
        return -1;
    tx_qsize = (uint16_t)(maxq < 128 ? maxq : 128);

    uint32_t desc_bytes  = 16u * tx_qsize;
    uint32_t avail_bytes = 6u + 2u * tx_qsize;
    uint32_t used_off    = (desc_bytes + avail_bytes + PAGE_SIZE_A64 - 1)
                           & ~(PAGE_SIZE_A64 - 1);
    uint32_t total       = used_off + 6u + 8u * tx_qsize;
    uint32_t npages      = (total + PAGE_SIZE_A64 - 1) / PAGE_SIZE_A64;

    uint64_t first = pmm_a64_alloc_frame();
    if (!first)
        return -1;
    uint64_t prev = first;
    for (uint32_t i = 1; i < npages; i++) {
        uint64_t f = pmm_a64_alloc_frame();
        if (f != prev + PAGE_SIZE_A64)
            return -1;
        prev = f;
    }
    uint8_t *ring = (uint8_t *)p2v_a64(first);
    for (uint32_t i = 0; i < total; i++)
        ring[i] = 0;

    txd = (struct vring_desc *)ring;
    txa = (struct vring_avail *)(ring + desc_bytes);
    txu = (struct vring_used *)(ring + used_off);

    if (rxq.version == 1) {
        wr32(b, VM_QUEUE_NUM, tx_qsize);
        wr32(b, VM_QUEUE_ALIGN, PAGE_SIZE_A64);
        wr32(b, VM_QUEUE_PFN, (uint32_t)(first / PAGE_SIZE_A64));
    } else {
        uint64_t desc_phys  = first;
        uint64_t avail_phys = first + desc_bytes;
        uint64_t used_phys  = first + used_off;
        wr32(b, VM_QUEUE_NUM, tx_qsize);
        wr32(b, VM_QUEUE_DESC_LO, (uint32_t)desc_phys);
        wr32(b, VM_QUEUE_DESC_HI, (uint32_t)(desc_phys >> 32));
        wr32(b, VM_QUEUE_AVAIL_LO, (uint32_t)avail_phys);
        wr32(b, VM_QUEUE_AVAIL_HI, (uint32_t)(avail_phys >> 32));
        wr32(b, VM_QUEUE_USED_LO, (uint32_t)used_phys);
        wr32(b, VM_QUEUE_USED_HI, (uint32_t)(used_phys >> 32));
        wr32(b, VM_QUEUE_READY, 1);
    }
    tx_last_used = txu->idx;
    return 0;
}

/* Keep the RX queue stocked: every buffer is one descriptor (header+
 * data share the buffer; no features => device writes the 10-byte
 * header first). */
static void rx_post(int slot)
{
    rxq.desc[slot].addr  = rxbuf_phys[slot];
    rxq.desc[slot].len   = BUF_LEN;
    rxq.desc[slot].flags = VRING_DESC_F_WRITE;
    rxq.desc[slot].next  = 0;
    rxq.avail->ring[rxq.avail->idx % rxq.qsize] = (uint16_t)slot;
    fence_a64();
    rxq.avail->idx++;
    fence_a64();
    wr32((volatile uint8_t *)rxq.base, VM_QUEUE_NOTIFY, 0);
}

int vnet_a64_init(const fdt_platform_t *plat)
{
    uint64_t bases[FDT_MAX_VIRTIO];
    for (uint32_t i = 0; i < plat->virtio_count; i++)
        bases[i] = (uint64_t)p2v_a64(plat->virtio_base[i]);

    if (vmmio_probe(&rxq, vmmio_a64_ops(), bases, plat->virtio_count,
                    VM_DEV_NET) != 0) {
        pl011_puts("[net]  no virtio-net device on the mmio windows "
                   "(pass -netdev/-device to attach one)\n");
        return -1;
    }
    if (setup_tx_queue() != 0)
        return -1;

    /* MAC from config space (offset 0..5, valid even without the
     * VIRTIO_NET_F_MAC bit on QEMU -- it always fills it). */
    for (int i = 0; i < 6; i++)
        our_mac[i] = vmmio_cfg8(&rxq, (uint32_t)i);

    for (int i = 0; i < RX_BUFS; i++) {
        rxbuf_phys[i] = pmm_a64_alloc_frame();
        if (!rxbuf_phys[i])
            return -1;
        rxbuf[i] = (uint8_t *)p2v_a64(rxbuf_phys[i]);
        rx_post(i);
    }
    txbuf_phys = pmm_a64_alloc_frame();
    if (!txbuf_phys)
        return -1;
    txbuf = (uint8_t *)p2v_a64(txbuf_phys);

    pl011_puts("[net]  virtio-net over mmio, MAC ");
    for (int i = 0; i < 6; i++) {
        put_hexb_(our_mac[i]);
        if (i < 5)
            pl011_putc(':');
    }
    pl011_puts("\n");
    ready = 1;
    return 0;
}

/* ---- the miniproto ops ---------------------------------------------------- */

static void net_send_a64(const uint8_t *frame, uint32_t len)
{
    if (!ready || len + VNET_HDR_LEN > PAGE_SIZE_A64)
        return;
    for (int i = 0; i < VNET_HDR_LEN; i++)
        txbuf[i] = 0;
    for (uint32_t i = 0; i < len; i++)
        txbuf[VNET_HDR_LEN + i] = frame[i];

    uint16_t before = tx_last_used;
    txd[0].addr  = txbuf_phys;
    txd[0].len   = VNET_HDR_LEN + len;
    txd[0].flags = 0;
    txd[0].next  = 0;
    txa->ring[txa->idx % tx_qsize] = 0;
    fence_a64();
    txa->idx++;
    fence_a64();
    volatile uint8_t *b = rxq.base;
    wr32(b, VM_QUEUE_NOTIFY, 1);

    uint64_t deadline = a64_cntvct() + vmmio_a64_ops()->ticks_per_sec;
    while (txu->idx == before && a64_cntvct() < deadline)
        fence_a64();
    tx_last_used = txu->idx;

    uint32_t is = rd32(b, VM_INT_STATUS);
    if (is)
        wr32(b, VM_INT_ACK, is);
}

static uint32_t net_poll_a64(uint8_t *out, uint32_t cap)
{
    if (!ready || rxq.used->idx == rxq.last_used_idx)
        return 0;
    fence_a64();

    struct vring_used_elem *e =
        &rxq.used->ring[rxq.last_used_idx % rxq.qsize];
    int slot = (int)e->id;
    uint32_t len = e->len;
    rxq.last_used_idx++;

    uint32_t flen = 0;
    if (len > VNET_HDR_LEN) {
        flen = len - VNET_HDR_LEN;
        if (flen > cap)
            flen = cap;
        for (uint32_t i = 0; i < flen; i++)
            out[i] = rxbuf[slot][VNET_HDR_LEN + i];
    }

    rx_post(slot);                     /* restock */
    volatile uint8_t *b = rxq.base;
    uint32_t is = rd32(b, VM_INT_STATUS);
    if (is)
        wr32(b, VM_INT_ACK, is);
    return flen;
}

static uint64_t ticks_a64_(void) { return timer_ticks_a64(); }
static void relax_a64_(void) { arch_cpu_relax(); }   /* A6's contract */

static const struct miniproto_ops vnet_ops = {
    .send    = net_send_a64,
    .poll    = net_poll_a64,
    .ticks   = ticks_a64_,
    .relax   = relax_a64_,
    .tick_hz = 100,
    .mac     = our_mac,
};

int vnet_a64_selftest(void)
{
    uint32_t our_ip = 0, gw_ip = 0;
    uint8_t gw_mac[6];
    static const uint8_t payload[] = "auralite-a64-ping";

    pl011_puts("[net]  self-test: DHCP -> ARP -> ICMP echo "
               "(shared miniproto)...\n");
    if (miniproto_dhcp(&vnet_ops, &our_ip, &gw_ip) != 0) {
        pl011_puts("[net]  FAIL: no DHCP lease\n");
        return -1;
    }
    pl011_puts("[net]  DHCP lease: ");
    for (int i = 0; i < 4; i++) {
        uint64_t oct = (our_ip >> (8 * i)) & 0xFF;
        char buf[4]; int n = 0;
        do { buf[n++] = (char)('0' + oct % 10); oct /= 10; } while (oct);
        while (n--) pl011_putc(buf[n]);
        if (i < 3) pl011_putc('.');
    }
    pl011_puts("\n");

    if (miniproto_arp_gw(&vnet_ops, our_ip, gw_ip, gw_mac) != 0) {
        pl011_puts("[net]  FAIL: gateway ARP unanswered\n");
        return -1;
    }
    pl011_puts("[net]  ARP: gateway resolved\n");

    if (miniproto_icmp_ping(&vnet_ops, our_ip, gw_ip, gw_mac,
                            payload, sizeof(payload)) != 0) {
        pl011_puts("[net]  FAIL: no ICMP echo reply\n");
        return -1;
    }
    pl011_puts("[net]  PASS: lease + ARP + echo reply (payload verified)\n");
    return 0;
}
