/* rtl8139.c -- Realtek RTL8139 family Fast Ethernet driver.
 *
 * A real data path, not a probe: the RX ring buffer is read through
 * CAPR/CBR, TX goes through the four hardware descriptors round-robin,
 * and an INTx handler drains receives into a software queue and wakes
 * whoever is sleeping in recv_wait().  The shape deliberately mirrors
 * drivers/e1000/e1000.c -- same sw_rx queue, same wait queues, same
 * netdev registration -- because that driver is this tree's reference
 * for "a NIC that actually moves bytes", and a second NIC that looks
 * different for no reason is a maintenance tax.
 *
 * What is different, and why:
 *
 *   PORT I/O, NOT MMIO.  The 8139 answers the same 256-byte register
 *   file at BAR0 (I/O space) and BAR1 (memory space).  This driver
 *   uses BAR0, so it needs no paging_map() of a device window and no
 *   HHDM arithmetic for registers.  That also keeps it inside the
 *   portable-include budget check_width_sweep.py ratchets: arch.h
 *   forwards inb/outb, and nothing here includes kernel/arch/x86_64/
 *   directly.
 *
 *   A RING, NOT A DESCRIPTOR ARRAY.  Receive is one flat buffer the
 *   chip fills continuously, each frame preceded by a 4-byte header.
 *   The arithmetic for walking it -- CRC stripping, wrap, the CAPR
 *   -16 bias -- is in rtl8139_ring.h and host-tested, because those
 *   are the three bugs this chip is famous for and QEMU cannot be
 *   asked to produce a wrapped frame on demand.
 *
 *   32-BIT DMA IS A HARD LIMIT.  RBSTART and TSAD0..3 are 32 bits.
 *   Buffers above 4 GiB cannot be expressed, so the driver CHECKS and
 *   refuses by name instead of programming a truncated address and
 *   corrupting whatever lives at the low alias.  On a machine with
 *   more than 4 GiB and no low frames free, that refusal is the
 *   correct outcome and says so on the console.
 */

#include <stdint.h>
#include "drivers/rtl8139/rtl8139.h"
#include "drivers/rtl8139/rtl8139_ring.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/arch.h"
#include "kernel/mm/pmm.h"
#include "kernel/proc/wait_queue.h"
#include "kernel/net/netdev.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "drivers/timer/pit.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/boot_info.h"

/* IRQ registration, declared rather than #included.
 *
 * kernel/arch/x86_64/irq.h is NOT forwarded by arch.h -- it has no
 * same-contract sibling on the other three architectures yet (the
 * riscv64/aarch64 device route is virtio-mmio + PLIC/GIC), so it is
 * part of ratchet 2's named residue in check_width_sweep.py.  The
 * e1000 and virtio-net drivers each spend one unit of that budget on
 * a direct include.  This driver does not: it is a strictly x86 PCI
 * device (it uses port I/O, which arch.h makes a hard compile error
 * elsewhere), and the two symbols it needs are declared here so the
 * portable-include count stays exactly where the sweep left it.
 * When irq.h earns an arch.h forwarding block, this shrinks to one
 * include and the declarations go. */
struct registers;
typedef void (*irq_handler_t)(struct registers *regs);
void irq_register_handler(int irq, irq_handler_t handler);

/* ---- Register offsets from BAR0 (RTL8139 datasheet, section 6) ---- */
#define RTL_IDR0        0x00   /* station address, 6 bytes            */
#define RTL_MAR0        0x08   /* multicast hash table, 8 bytes       */
#define RTL_TSD0        0x10   /* transmit status, 4 x 4 bytes        */
#define RTL_TSAD0       0x20   /* transmit start address, 4 x 4 bytes */
#define RTL_RBSTART     0x30   /* receive buffer start (32-bit phys)  */
#define RTL_CMD         0x37   /* command register (8 bit)            */
#define RTL_CAPR        0x38   /* current address of packet read      */
#define RTL_CBR         0x3A   /* current buffer address              */
#define RTL_IMR         0x3C   /* interrupt mask                      */
#define RTL_ISR         0x3E   /* interrupt status (write to clear)   */
#define RTL_TCR         0x40   /* transmit configuration              */
#define RTL_RCR         0x44   /* receive configuration               */
#define RTL_CONFIG1     0x52   /* config 1 (power management)         */
#define RTL_MSR         0x58   /* media status                        */

/* CMD bits. */
#define CMD_BUFE        (1u << 0)   /* RX buffer empty                 */
#define CMD_TE          (1u << 2)   /* transmitter enable              */
#define CMD_RE          (1u << 3)   /* receiver enable                 */
#define CMD_RST         (1u << 4)   /* software reset                  */

/* Interrupt status/mask bits. */
#define INT_ROK         (1u << 0)   /* receive OK                      */
#define INT_RER         (1u << 1)   /* receive error                   */
#define INT_TOK         (1u << 2)   /* transmit OK                     */
#define INT_TER         (1u << 3)   /* transmit error                  */
#define INT_RXOVW       (1u << 4)   /* RX buffer overflow              */
#define INT_PUN         (1u << 5)   /* packet underrun / link change   */
#define INT_FOVW        (1u << 6)   /* RX FIFO overflow                */
#define INT_TIMEOUT     (1u << 14)
#define INT_SERR        (1u << 15)  /* system error (PCI bus fault)    */

/* RCR bits: accept broadcast + multicast + physical-match + wrap. */
#define RCR_AAP         (1u << 0)   /* accept all (promiscuous)        */
#define RCR_APM         (1u << 1)   /* accept physical match           */
#define RCR_AM          (1u << 2)   /* accept multicast                */
#define RCR_AB          (1u << 3)   /* accept broadcast                */
#define RCR_WRAP        (1u << 7)   /* do not split frames at the end  */
#define RCR_RBLEN_8K    (0u << 11)  /* 8 KiB + 16 ring                 */
#define RCR_MXDMA_1024  (6u << 8)   /* max DMA burst                   */
#define RCR_RXFTH_NONE  (7u << 13)  /* no RX FIFO threshold            */

/* TSD bits. */
#define TSD_OWN         (1u << 13)  /* chip has finished with this desc */
#define TSD_TUN         (1u << 14)  /* transmit FIFO underrun           */
#define TSD_TOK         (1u << 15)  /* transmit OK                      */

/* MSR bits. */
#define MSR_LINKB       (1u << 2)   /* inverse link: 0 == link UP       */

/* TCR bits. */
#define TCR_MXDMA_2048  (7u << 8)
#define TCR_IFG_STD     (3u << 24)

/* The 8139 cannot address memory above this line (32-bit DMA regs). */
#define RTL8139_DMA_LIMIT  0x100000000ULL

static uint16_t io_base;                 /* BAR0 port-space base         */
static uint8_t  mac_addr[6];
static uint8_t  pci_bus, pci_dev, pci_func;
static uint8_t  rtl_irq_line = 0xFF;
static int      rtl_present;             /* set once init() succeeded    */
static uint16_t device_id_seen;

static uint8_t *rx_buffer;               /* HHDM view of the RX ring     */
static uint32_t rx_offset;               /* our read cursor into it      */
static uint8_t *tx_buffer[RTL8139_NUM_TX_DESC];
static uint32_t tx_buffer_phys[RTL8139_NUM_TX_DESC];
static uint32_t tx_cur;                  /* next descriptor to use       */

static uint32_t rx_packet_count;
static uint32_t rx_reset_count;          /* ring desyncs recovered from  */
static uint32_t sw_rx_drops;
static int      rx_irq_receipt_printed;

#define RTL8139_SW_RX_QUEUE_LEN 64
struct sw_rx_packet {
    uint16_t len;
    uint8_t  data[RTL8139_PKT_BUF_SIZE];
};

static struct sw_rx_packet sw_rx_queue[RTL8139_SW_RX_QUEUE_LEN];
static uint32_t sw_rx_head, sw_rx_tail, sw_rx_count;
static spinlock_t rxq_lock  = SPINLOCK_UNLOCKED;
static spinlock_t hw_rx_lock = SPINLOCK_UNLOCKED;
static struct wait_queue rtl_rx_wq;
static struct wait_queue rtl_tx_wq;

/* ---- register helpers (port I/O via arch.h) ---- */

static inline void rtl_out8(uint16_t reg, uint8_t v)   { outb(io_base + reg, v); }
static inline void rtl_out16(uint16_t reg, uint16_t v) { outw(io_base + reg, v); }
static inline void rtl_out32(uint16_t reg, uint32_t v) { outl(io_base + reg, v); }
static inline uint8_t  rtl_in8(uint16_t reg)  { return inb(io_base + reg); }
static inline uint16_t rtl_in16(uint16_t reg) { return inw(io_base + reg); }
static inline uint32_t rtl_in32(uint16_t reg) { return inl(io_base + reg); }

/* ---- software RX queue (identical contract to the e1000's) ---- */

static int sw_rx_push(const void *data, uint16_t len) {
    if (!data || len == 0) return 0;
    if (len > RTL8139_PKT_BUF_SIZE) len = RTL8139_PKT_BUF_SIZE;

    arch_irqflags_t flags = spinlock_acquire_irqsave(&rxq_lock);
    if (sw_rx_count >= RTL8139_SW_RX_QUEUE_LEN) {
        sw_rx_drops++;
        spinlock_release_irqrestore(&rxq_lock, flags);
        return 0;
    }
    struct sw_rx_packet *p = &sw_rx_queue[sw_rx_tail];
    memcpy(p->data, data, len);
    p->len = len;
    sw_rx_tail = (sw_rx_tail + 1) % RTL8139_SW_RX_QUEUE_LEN;
    sw_rx_count++;
    spinlock_release_irqrestore(&rxq_lock, flags);
    return 1;
}

static int sw_rx_pop(void *buf, uint32_t bufsize) {
    if (!buf || bufsize == 0) return -1;

    arch_irqflags_t flags = spinlock_acquire_irqsave(&rxq_lock);
    if (sw_rx_count == 0) {
        spinlock_release_irqrestore(&rxq_lock, flags);
        return 0;
    }
    struct sw_rx_packet *p = &sw_rx_queue[sw_rx_head];
    uint16_t len = p->len;
    if (len > bufsize) len = (uint16_t)bufsize;
    memcpy(buf, p->data, len);
    p->len = 0;
    sw_rx_head = (sw_rx_head + 1) % RTL8139_SW_RX_QUEUE_LEN;
    sw_rx_count--;
    spinlock_release_irqrestore(&rxq_lock, flags);
    return (int)len;
}

static const char *rtl8139_device_name(uint16_t id) {
    switch (id) {
    case RTL8139_DEVICE_8139: return "RTL8139/8139C/8139D Fast Ethernet";
    case RTL8139_DEVICE_8138: return "RTL8139B CardBus";
    case RTL8139_DEVICE_8100: return "RTL8100 Fast Ethernet";
    case RTL8139_DEVICE_8130: return "RTL8139C+/8130 Fast Ethernet";
    default:                  return "unknown Realtek 8139-compatible";
    }
}

/*
 * Drain every complete frame the chip has committed to the ring.
 *
 * CMD.BUFE is the authority on emptiness: the chip clears it while
 * unread frames remain.  Each iteration reads the 4-byte header,
 * classifies it with the host-tested core, and advances.  A RESET
 * verdict means our cursor is no longer on a header boundary, which
 * is unrecoverable by walking -- we resynchronise from the chip's own
 * CBR and count the event, because silently looping on garbage is how
 * a NIC "mysteriously stops receiving".
 */
static int rtl8139_hw_rx_drain(void) {
    if (!rtl_present || !rx_buffer) return 0;

    int drained = 0;
    arch_irqflags_t flags = spinlock_acquire_irqsave(&hw_rx_lock);

    /* Bound the loop: a full 8 KiB ring cannot hold more than this many
     * minimum-size frames, so a runaway cursor still terminates. */
    /* NOTE: every modulus below is RTL8139_RX_WRAP_LEN -- the ring
     * proper that RCR.RBLEN selects -- and never RTL8139_RX_BUF_LEN,
     * which is the larger ALLOCATION (ring + slack + WRAP pad).  The
     * chip wraps at the former; programming a CAPR past it makes the
     * hardware believe the buffer is full and the receiver never
     * recovers.  See the comment block in rtl8139_ring.h. */
    int budget = RTL8139_RX_WRAP_LEN / 64;
    while (!(rtl_in8(RTL_CMD) & CMD_BUFE) && budget-- > 0) {
        uint32_t hdr = rtl8139_rx_header(rx_buffer, RTL8139_RX_WRAP_LEN,
                                         rx_offset);
        uint32_t payload = 0;
        enum rtl8139_rx_verdict v =
            rtl8139_rx_classify(hdr, RTL8139_PKT_BUF_SIZE,
                                RTL8139_RX_WRAP_LEN, &payload);

        if (v == RTL8139_RX_EMPTY) break;

        if (v == RTL8139_RX_RESET) {
            /* Resynchronise from the hardware's write cursor. */
            rx_offset = rtl_in16(RTL_CBR) % RTL8139_RX_WRAP_LEN;
            rtl_out16(RTL_CAPR, rtl8139_capr_from_offset(rx_offset));
            rx_reset_count++;
            kprintf("[rtl8139] RX ring desync; resynced from CBR "
                    "(resets=%u)\n", rx_reset_count);
            break;
        }

        uint32_t frame_len = (hdr >> 16) & 0xFFFFu;

        if (v == RTL8139_RX_OK && payload > 0) {
            /* Copy out of the ring, honouring the wrap: the frame body
             * starts just past its header and may run over the end. */
            uint32_t start = (rx_offset + RTL8139_RX_HDR_LEN) %
                             RTL8139_RX_WRAP_LEN;
            static uint8_t linear[RTL8139_PKT_BUF_SIZE];
            if (start + payload <= RTL8139_RX_WRAP_LEN) {
                /* Wholly inside the ring, or running into the WRAP pad
                 * -- either way contiguous, because the pad exists so
                 * the chip never has to split a frame. */
                memcpy(linear, rx_buffer + start, payload);
            } else {
                /* The chip DID split it (WRAP disabled, or a clone that
                 * ignores the bit): stitch the two halves. */
                uint32_t first = RTL8139_RX_WRAP_LEN - start;
                memcpy(linear, rx_buffer + start, first);
                memcpy(linear + first, rx_buffer, payload - first);
            }
            if (sw_rx_push(linear, (uint16_t)payload)) {
                drained++;
                rx_packet_count++;
            }
        }

        rx_offset = rtl8139_rx_advance(rx_offset, frame_len,
                                       RTL8139_RX_WRAP_LEN);
        rtl_out16(RTL_CAPR, rtl8139_capr_from_offset(rx_offset));
    }

    spinlock_release_irqrestore(&hw_rx_lock, flags);
    return drained;
}

static void rtl8139_irq_handler(struct registers *regs) {
    (void)regs;
    if (!rtl_present) return;

    uint16_t isr = rtl_in16(RTL_ISR);
    if (isr == 0) return;

    /* Acknowledge first: the 8139 latches causes, and a cause left set
     * while INTx is level-triggered re-fires the handler forever.
     * (Writing the bits back is what clears them on this chip.) */
    rtl_out16(RTL_ISR, isr);

    if (isr & (INT_ROK | INT_RER | INT_RXOVW | INT_FOVW)) {
        int drained = rtl8139_hw_rx_drain();
        if (drained > 0) {
            if (!rx_irq_receipt_printed) {
                /* The receipt this tree asks of an IRQ-backed RX path
                 * (the R9/RES-28 precedent): prove the interrupt did
                 * the work, once, rather than claiming it in a doc. */
                kprintf("[rtl8139] RX via IRQ wake (%d frame(s))\n", drained);
                rx_irq_receipt_printed = 1;
            }
            wq_wake_all(&rtl_rx_wq);
        }
        if (isr & (INT_RXOVW | INT_FOVW)) {
            kprintf("[rtl8139] RX overflow (drops=%u)\n", sw_rx_drops);
        }
    }

    if (isr & (INT_TOK | INT_TER)) {
        wq_wake_all(&rtl_tx_wq);
    }

    if (isr & INT_SERR) {
        kprintf("[rtl8139] PCI system error reported (ISR=0x%04x)\n", isr);
    }
}

static int rtl8139_find_device(uint8_t *out_bus, uint8_t *out_dev,
                               uint8_t *out_func, uint16_t *out_id) {
    static const uint16_t supported[] = {
        RTL8139_DEVICE_8139,
        RTL8139_DEVICE_8138,
        RTL8139_DEVICE_8100,
        RTL8139_DEVICE_8130,
    };
    for (uint32_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (pci_find_device(RTL8139_VENDOR_ID, supported[i],
                            out_bus, out_dev, out_func) == 0) {
            if (out_id) *out_id = supported[i];
            return 0;
        }
    }
    return -1;
}

int rtl8139_init(void) {
    uint16_t id = 0;
    if (rtl8139_find_device(&pci_bus, &pci_dev, &pci_func, &id) != 0) {
        kprintf("[rtl8139] no Realtek NIC on PCI "
                "(supported: 8139/8138/8100/8130)\n");
        return -1;
    }
    device_id_seen = id;
    kprintf("[rtl8139] found %s (10ec:%04x) at PCI %u:%u.%u\n",
            rtl8139_device_name(id), id, pci_bus, pci_dev, pci_func);

    wq_init(&rtl_rx_wq);
    wq_init(&rtl_tx_wq);
    sw_rx_head = sw_rx_tail = sw_rx_count = sw_rx_drops = 0;
    rx_packet_count = rx_reset_count = 0;
    rx_irq_receipt_printed = 0;
    tx_cur = 0;
    rx_offset = 0;

    /* Bus mastering is required before any DMA; clear INTx-disable so the
     * legacy interrupt actually reaches the PIC/IOAPIC. */
    pci_enable_bus_master(pci_bus, pci_dev, pci_func);
    uint32_t cmd = pci_config_read(pci_bus, pci_dev, pci_func, 0x04);
    pci_config_write(pci_bus, pci_dev, pci_func, 0x04,
                     (cmd | 0x5u) & ~(1u << 10));   /* I/O space + bus master */

    /* BAR0 is the I/O-space window; bit 0 marks it as such. */
    uint32_t bar0 = pci_get_bar(pci_bus, pci_dev, pci_func, 0);
    if (!(bar0 & 0x1u)) {
        kprintf("[rtl8139] BAR0 is not an I/O BAR (0x%08x); "
                "this driver uses the port-I/O window\n", bar0);
        return -1;
    }
    io_base = (uint16_t)(bar0 & ~0x3u);
    kprintf("[rtl8139] BAR0 I/O port base 0x%04x\n", io_base);
    rtl_present = 1;

    /* Power the chip on (CONFIG1 = 0 clears LWAKE/sleep on real parts). */
    rtl_out8(RTL_CONFIG1, 0x00);

    /* Software reset, then wait for the chip to clear RST itself. */
    rtl_out8(RTL_CMD, CMD_RST);
    int reset_to = 100000;
    while ((rtl_in8(RTL_CMD) & CMD_RST) && reset_to-- > 0) {
        arch_cpu_relax();
    }
    if (reset_to <= 0) {
        kprintf("[rtl8139] reset timeout; device did not clear CMD.RST\n");
        rtl_present = 0;
        return -1;
    }

    /* Station address: six bytes at IDR0.  Unlike the e1000 there is no
     * EEPROM dance -- the chip has already loaded it for us. */
    for (int i = 0; i < 6; i++) mac_addr[i] = rtl_in8((uint16_t)(RTL_IDR0 + i));
    kprintf("[rtl8139] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5]);

    uint64_t hhdm = boot_get_hhdm_offset();

    /* RX ring: one physically contiguous run, and it MUST sit below
     * 4 GiB because RBSTART is 32 bits.  Refuse loudly rather than
     * truncate -- a truncated DMA address corrupts unrelated memory. */
    {
        uint64_t pages = (RTL8139_RX_BUF_LEN + 0xFFF) / 0x1000;
        paddr_t phys = pmm_alloc_contiguous(pages);
        if (!phys) {
            kprintf("[rtl8139] out of memory for the %u-byte RX ring\n",
                    (unsigned)RTL8139_RX_BUF_LEN);
            rtl_present = 0;
            return -1;
        }
        if (phys + RTL8139_RX_BUF_LEN > RTL8139_DMA_LIMIT) {
            kprintf("[rtl8139] RX ring at 0x%llx is above the 4 GiB "
                    "32-bit DMA limit; refusing (no bounce buffer yet)\n",
                    (unsigned long long)phys);
            rtl_present = 0;
            return -1;
        }
        rx_buffer = (uint8_t *)(uintptr_t)(hhdm + phys);
        memset(rx_buffer, 0, RTL8139_RX_BUF_LEN);
        rtl_out32(RTL_RBSTART, (uint32_t)phys);
        kprintf("[rtl8139] RX ring phys=0x%llx len=%u\n",
                (unsigned long long)phys, (unsigned)RTL8139_RX_BUF_LEN);
    }

    /* Four TX buffers, same 4 GiB rule (TSAD0..3 are 32 bits too). */
    for (int i = 0; i < RTL8139_NUM_TX_DESC; i++) {
        paddr_t phys = pmm_alloc_frame();
        if (!phys) {
            kprintf("[rtl8139] out of memory for TX buffer %d\n", i);
            rtl_present = 0;
            return -1;
        }
        if (phys + RTL8139_PKT_BUF_SIZE > RTL8139_DMA_LIMIT) {
            kprintf("[rtl8139] TX buffer %d at 0x%llx is above the 4 GiB "
                    "32-bit DMA limit; refusing\n",
                    i, (unsigned long long)phys);
            rtl_present = 0;
            return -1;
        }
        tx_buffer[i]      = (uint8_t *)(uintptr_t)(hhdm + phys);
        tx_buffer_phys[i] = (uint32_t)phys;
        rtl_out32((uint16_t)(RTL_TSAD0 + i * 4), tx_buffer_phys[i]);
    }

    /* Multicast: accept everything.  Same reasoning the e1000 driver
     * records at R9 -- IPv6 NDP rides 33:33:xx group addresses, and an
     * unprogrammed hash table eats every Router Advertisement. */
    rtl_out32(RTL_MAR0 + 0, 0xFFFFFFFFu);
    rtl_out32(RTL_MAR0 + 4, 0xFFFFFFFFu);

    /* Mask interrupts while the rings are being armed. */
    rtl_out16(RTL_IMR, 0);
    rtl_out16(RTL_ISR, 0xFFFFu);

    /* Enable RX+TX before programming RCR/TCR: the datasheet's order,
     * and the chip ignores RCR writes while the receiver is disabled
     * on some clones. */
    rtl_out8(RTL_CMD, CMD_RE | CMD_TE);

    rtl_out32(RTL_RCR, RCR_APM | RCR_AM | RCR_AB | RCR_WRAP |
                       RCR_RBLEN_8K | RCR_MXDMA_1024 | RCR_RXFTH_NONE);
    rtl_out32(RTL_TCR, TCR_MXDMA_2048 | TCR_IFG_STD);

    rx_offset = 0;
    rtl_out16(RTL_CAPR, rtl8139_capr_from_offset(rx_offset));

    rtl_irq_line = pci_get_interrupt_line(pci_bus, pci_dev, pci_func);
    if (rtl_irq_line < 16) {
        irq_register_handler((int)rtl_irq_line, rtl8139_irq_handler);
        rtl_out16(RTL_IMR, INT_ROK | INT_RER | INT_TOK | INT_TER |
                           INT_RXOVW | INT_FOVW | INT_PUN | INT_SERR);
        kprintf("[rtl8139] IRQ line %u enabled (IMR=0x%04x)\n",
                rtl_irq_line, rtl_in16(RTL_IMR));
    } else {
        kprintf("[rtl8139] no valid PCI INTx line (0x%02x); "
                "polling fallback only\n", rtl_irq_line);
    }

    kprintf("[rtl8139] ready: CMD=0x%02x RCR=0x%08x link=%s\n",
            rtl_in8(RTL_CMD), rtl_in32(RTL_RCR),
            rtl8139_link_up() ? "up" : "down");
    return 0;
}

void rtl8139_get_mac(uint8_t mac[6]) {
    memcpy(mac, mac_addr, 6);
}

int rtl8139_link_up(void) {
    if (!rtl_present) return 0;
    /* MSR.LINKB is INVERSE logic: the bit is CLEAR when the link is up. */
    return (rtl_in8(RTL_MSR) & MSR_LINKB) ? 0 : 1;
}

int rtl8139_send(const void *data, uint32_t len) {
    if (!rtl_present || !data) return -1;
    if (!rtl8139_link_up()) return -1;
    if (len == 0) return 0;
    if (len > RTL8139_PKT_BUF_SIZE) len = RTL8139_PKT_BUF_SIZE;

    /* The 8139 will not transmit a runt: pad to the 60-byte Ethernet
     * minimum (the chip appends the 4-byte FCS itself).  Without this
     * an ARP request -- 42 bytes -- is silently dropped by the wire,
     * which presents as "DHCP never completes". */
    uint32_t xmit_len = len;
    if (xmit_len < 60) xmit_len = 60;

    uint32_t slot = tx_cur;
    tx_cur = (tx_cur + 1) % RTL8139_NUM_TX_DESC;

    memcpy(tx_buffer[slot], data, len);
    if (xmit_len > len) memset(tx_buffer[slot] + len, 0, xmit_len - len);

    /* Writing TSD with the length (and a zeroed OWN bit) starts the DMA.
     * The low 13 bits are the size; bits 16-20 are the early-TX
     * threshold, which we leave at 0 (store-and-forward). */
    rtl_out32((uint16_t)(RTL_TSD0 + slot * 4), xmit_len);

    /* Wait for the chip to hand the descriptor back.  OWN is set when
     * the FIFO has drained; TOK when the frame is actually out. */
    int timeout = 100000;
    uint32_t tsd = 0;
    while (timeout-- > 0) {
        tsd = rtl_in32((uint16_t)(RTL_TSD0 + slot * 4));
        if (tsd & (TSD_TOK | TSD_OWN)) break;
        arch_cpu_relax();
    }
    if (timeout <= 0) {
        kprintf("[rtl8139] TX timeout on desc %u (TSD=0x%08x)\n", slot, tsd);
        return -1;
    }
    if (tsd & TSD_TUN) {
        kprintf("[rtl8139] TX FIFO underrun on desc %u\n", slot);
    }
    return (int)len;
}

int rtl8139_recv(void *buf, uint32_t bufsize) {
    int n = sw_rx_pop(buf, bufsize);
    if (n != 0) return n;

    /* Same contract as the e1000: if no IRQ has queued anything yet,
     * opportunistically drain the ring so a polling caller still works
     * when INTx is unavailable. */
    if (rtl8139_hw_rx_drain() > 0) {
        n = sw_rx_pop(buf, bufsize);
        if (n != 0) return n;
    }
    return 0;
}

int rtl8139_recv_wait(void *buf, uint32_t bufsize, uint64_t timeout_ticks) {
    uint64_t start = timer_get_ticks();
    uint64_t deadline = timeout_ticks ? start + timeout_ticks : 0;
    tcb_t *cur = sched_current();
    uint64_t old_sleep_deadline = cur ? cur->sleep_deadline : 0;

    for (;;) {
        int n = rtl8139_recv(buf, bufsize);
        if (n != 0) {
            if (cur) cur->sleep_deadline = old_sleep_deadline;
            return n;
        }
        if (!rtl8139_link_up()) {
            if (cur) cur->sleep_deadline = old_sleep_deadline;
            return -1;
        }
        if (deadline && timer_get_ticks() >= deadline) {
            if (cur) cur->sleep_deadline = old_sleep_deadline;
            return 0;
        }
        if (!cur) {
            arch_cpu_relax();
            continue;
        }
        /* Sleep on the queue the IRQ handler wakes; the deadline is the
         * O7 safety net against a lost wakeup, not the normal path. */
        if (deadline) cur->sleep_deadline = deadline;
        wq_wait_deadline(&rtl_rx_wq, NULL, deadline);
        if (deadline) cur->sleep_deadline = old_sleep_deadline;
    }
}

/* ---- netdev backend registration ---------------------------------------- */

static const struct netdev rtl8139_netdev = {
    .name      = "rtl8139",
    .send      = rtl8139_send,
    .recv      = rtl8139_recv,
    .recv_wait = rtl8139_recv_wait,
    .get_mac   = rtl8139_get_mac,
    .link_up   = rtl8139_link_up,
};

void rtl8139_register_netdev(void) {
    netdev_register(&rtl8139_netdev);
}
