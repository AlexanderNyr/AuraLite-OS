/* r8169.c -- Realtek RTL8169/8168/8111 Gigabit Ethernet driver.
 *
 * The gigabit sibling of drivers/rtl8139/, and a DIFFERENT machine: the
 * 8169 uses descriptor rings with an OWN-bit ownership hand-off (not a
 * ring buffer walked through CAPR/CBR) and 64-bit descriptor/buffer
 * addresses (not the 8139's 32-bit RBSTART/TSAD).  The register map,
 * descriptor layouts and hand-off arithmetic live in r8169_desc.h
 * (RT1); the state machine that drives them lives in r8169_core.h and
 * is proved against a register-level model of the chip on the host --
 * QEMU has no 8169, so the data path cannot be gated under `-device`
 * the way the 8139's was (see docs/plans/REALTEK_PLAN.md).
 *
 * What this file is: the KERNEL glue.  It supplies the narrow
 * register-I/O seam (port I/O to BAR0), the PCI probe, the DMA buffer
 * allocation, the INTx handler that drains RX into a software queue and
 * wakes sleepers, and the netdev registration.  Every device access
 * goes through the seam -- the same one the host gate binds to the
 * model -- so the tested core and the shipped driver are one object.
 *
 *   PORT I/O, NOT MMIO.  The chip answers the same 256-byte register
 *   file at BAR0 (I/O space) and BAR1 (memory space); BAR0 keeps the
 *   driver inside the portable-include budget (the 8139 rule).
 *
 *   NO 4 GiB DMA WALL.  TNPDS/RDSAR and the descriptor address fields
 *   are 64-bit, so unlike the 8139 there is nothing to refuse by name.
 *   The analog the core enforces instead: both ring bases are 256-byte
 *   aligned and BOTH halves of each base register are programmed.
 */

#include <stdint.h>
#include "kernel/lib/string.h"
#include "drivers/r8169/r8169.h"
#include "drivers/r8169/r8169_core.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/arch.h"
#include "kernel/mm/pmm.h"
#include "kernel/proc/wait_queue.h"
#include "kernel/net/netdev.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "drivers/timer/pit.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/kprintf.h"
#include "kernel/boot_info.h"

/* IRQ registration, declared rather than #included -- the 8139 rule:
 * kernel/arch/x86_64/irq.h is not forwarded by arch.h, and the 8169 is
 * a strictly x86 PCI device (port I/O), so the two symbols are declared
 * here and the portable-include count stays exactly where the width
 * sweep left it. */
struct registers;
typedef void (*irq_handler_t)(struct registers *regs);
void irq_register_handler(int irq, irq_handler_t handler);

/* ---- the register-I/O seam (port I/O via arch.h, bound to BAR0) ---- */

static uint16_t r8169_io_base;

static uint8_t  r8169_io_rd8(struct r8169_io *io, uint16_t reg)  { (void)io; return inb(r8169_io_base + reg); }
static uint16_t r8169_io_rd16(struct r8169_io *io, uint16_t reg) { (void)io; return inw(r8169_io_base + reg); }
static uint32_t r8169_io_rd32(struct r8169_io *io, uint16_t reg) { (void)io; return inl(r8169_io_base + reg); }
static void r8169_io_wr8(struct r8169_io *io, uint16_t reg, uint8_t v)  { (void)io; outb(r8169_io_base + reg, v); }
static void r8169_io_wr16(struct r8169_io *io, uint16_t reg, uint16_t v) { (void)io; outw(r8169_io_base + reg, v); }
static void r8169_io_wr32(struct r8169_io *io, uint16_t reg, uint32_t v) { (void)io; outl(r8169_io_base + reg, v); }
static void r8169_io_relax(struct r8169_io *io) { (void)io; arch_cpu_relax(); }

static struct r8169_io r8169_kernel_io = {
    .rd8   = r8169_io_rd8,
    .rd16  = r8169_io_rd16,
    .rd32  = r8169_io_rd32,
    .wr8   = r8169_io_wr8,
    .wr16  = r8169_io_wr16,
    .wr32  = r8169_io_wr32,
    .relax = r8169_io_relax,
};

/* ---- static driver state ---- */

static struct r8169_state r8169_st;

static uint8_t  r8169_pci_bus, r8169_pci_dev, r8169_pci_func;
static uint8_t  r8169_irq_line = 0xFF;
static uint16_t device_id_seen;
static int      r8169_present_flag;     /* set once init() succeeded */

static uint32_t rx_packet_count;
static uint32_t sw_rx_drops;
static int      rx_irq_receipt_printed;

#define R8169_SW_RX_QUEUE_LEN 64
struct sw_rx_packet {
    uint16_t len;
    uint8_t  data[R8169_PKT_BUF_SIZE];
};

static struct sw_rx_packet sw_rx_queue[R8169_SW_RX_QUEUE_LEN];
static uint32_t sw_rx_head, sw_rx_tail, sw_rx_count;
static spinlock_t rxq_lock   = SPINLOCK_UNLOCKED;
static spinlock_t hw_rx_lock = SPINLOCK_UNLOCKED;
static struct wait_queue r8169_rx_wq;
static struct wait_queue r8169_tx_wq;
/* RESIDUE2 T5 (the idle RX drain, mirroring e1000/rtl8139). */
static volatile int rx_waiters = 0;
static uint32_t idle_drained = 0;

/* ---- software RX queue (identical contract to the e1000's) ---- */

static int sw_rx_push(const void *data, uint16_t len) {
    if (!data || len == 0) return 0;
    if (len > R8169_PKT_BUF_SIZE) len = R8169_PKT_BUF_SIZE;

    arch_irqflags_t flags = spinlock_acquire_irqsave(&rxq_lock);
    if (sw_rx_count >= R8169_SW_RX_QUEUE_LEN) {
        sw_rx_drops++;
        spinlock_release_irqrestore(&rxq_lock, flags);
        return 0;
    }
    struct sw_rx_packet *p = &sw_rx_queue[sw_rx_tail];
    memcpy(p->data, data, len);
    p->len = len;
    sw_rx_tail = (sw_rx_tail + 1) % R8169_SW_RX_QUEUE_LEN;
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
    sw_rx_head = (sw_rx_head + 1) % R8169_SW_RX_QUEUE_LEN;
    sw_rx_count--;
    spinlock_release_irqrestore(&rxq_lock, flags);
    return (int)len;
}

/* RESIDUE2 T5: atomic check-and-pop for netdev_passive_drain(). */
static int r8169_rx_pop_idle(void *buf, uint32_t bufsize) {
    if (!buf || bufsize == 0) return 0;

    arch_irqflags_t flags = spinlock_acquire_irqsave(&rxq_lock);
    if (rx_waiters != 0) {
        spinlock_release_irqrestore(&rxq_lock, flags);
        return -1;
    }
    if (sw_rx_count == 0) {
        spinlock_release_irqrestore(&rxq_lock, flags);
        return 0;
    }
    struct sw_rx_packet *p = &sw_rx_queue[sw_rx_head];
    uint16_t len = p->len;
    if (len > bufsize) len = (uint16_t)bufsize;
    memcpy(buf, p->data, len);
    p->len = 0;
    sw_rx_head = (sw_rx_head + 1) % R8169_SW_RX_QUEUE_LEN;
    sw_rx_count--;
    spinlock_release_irqrestore(&rxq_lock, flags);

    idle_drained++;
    if (idle_drained == 1 || (idle_drained & 0xFF) == 0) {
        kprintf("[r8169] idle drain: %u unsolicited frame(s) consumed "
                "(queue kept empty; ARP/NDP answered)\n", idle_drained);
    }
    return (int)len;
}

static const char *r8169_device_name(uint16_t id) {
    switch (id) {
    case R8169_DEVICE_8169: return "RTL8169 Gigabit Ethernet";
    case R8169_DEVICE_8168: return "RTL8168/8111 Gigabit Ethernet";
    default:                return "unknown Realtek 8169-family";
    }
}

/* Drain every complete frame the chip has handed back, into the
 * software queue.  The core classifies each descriptor (OWN hand-off,
 * error/fragment drop, FCS-stripped length, desync) -- the same core
 * the host gate drives against the model. */
static int r8169_hw_rx_drain(void) {
    if (!r8169_st.present) return 0;

    int drained = 0;
    arch_irqflags_t flags = spinlock_acquire_irqsave(&hw_rx_lock);

    /* Bound the loop: a full ring cannot hold more frames than it has
     * descriptors. */
    int budget = (int)R8169_NUM_RX_DESC;
    static uint8_t linear[R8169_PKT_BUF_SIZE];
    while (budget-- > 0) {
        int n = r8169_core_recv(&r8169_st, linear, sizeof(linear));
        if (n <= 0) break;
        if (sw_rx_push(linear, (uint16_t)n)) {
            drained++;
            rx_packet_count++;
        }
    }

    spinlock_release_irqrestore(&hw_rx_lock, flags);
    return drained;
}

static void r8169_irq_handler(struct registers *regs) {
    (void)regs;
    if (!r8169_st.present) return;

    uint16_t isr = r8169_core_isr_ack(&r8169_kernel_io);
    if (isr == 0) return;

    if (isr & (R8169_INT_RX_OK | R8169_INT_RX_ERR |
               R8169_INT_RX_FIFO_OVER | R8169_INT_RX_OVER)) {
        int drained = r8169_hw_rx_drain();
        if (drained > 0) {
            if (!rx_irq_receipt_printed) {
                /* The receipt this tree asks of an IRQ-backed RX path
                 * (the R9/RES-28 precedent): prove the interrupt did
                 * the work, once. */
                kprintf("[r8169] RX via IRQ wake (%d frame(s))\n", drained);
                rx_irq_receipt_printed = 1;
            }
            wq_wake_all(&r8169_rx_wq);
        }
        if (isr & (R8169_INT_RX_FIFO_OVER | R8169_INT_RX_OVER)) {
            kprintf("[r8169] RX overflow (drops=%u)\n", sw_rx_drops);
        }
    }

    if (isr & (R8169_INT_TX_OK | R8169_INT_TX_ERR)) {
        wq_wake_all(&r8169_tx_wq);
    }
}

static int r8169_find_device(uint8_t *out_bus, uint8_t *out_dev,
                             uint8_t *out_func, uint16_t *out_id) {
    static const uint16_t supported[] = {
        R8169_DEVICE_8169,
        R8169_DEVICE_8168,
    };
    for (uint32_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (pci_find_device(R8169_VENDOR_ID, supported[i],
                            out_bus, out_dev, out_func) == 0) {
            if (out_id) *out_id = supported[i];
            return 0;
        }
    }
    return -1;
}

int r8169_init(void) {
    uint16_t id = 0;
    if (r8169_find_device(&r8169_pci_bus, &r8169_pci_dev, &r8169_pci_func, &id) != 0) {
        kprintf("[r8169] no Realtek gigabit NIC on PCI "
                "(supported: 8169/8168/8111)\n");
        return -1;
    }
    device_id_seen = id;
    kprintf("[r8169] found %s (10ec:%04x) at PCI %u:%u.%u\n",
            r8169_device_name(id), id, r8169_pci_bus, r8169_pci_dev,
            r8169_pci_func);

    wq_init(&r8169_rx_wq);
    wq_init(&r8169_tx_wq);
    sw_rx_head = sw_rx_tail = sw_rx_count = sw_rx_drops = 0;
    rx_packet_count = 0;
    rx_irq_receipt_printed = 0;
    rx_waiters = 0;
    idle_drained = 0;
    memset(&r8169_st, 0, sizeof(r8169_st));

    /* Bus mastering is required before any DMA; clear INTx-disable. */
    pci_enable_bus_master(r8169_pci_bus, r8169_pci_dev, r8169_pci_func);
    uint32_t cmd = pci_config_read(r8169_pci_bus, r8169_pci_dev, r8169_pci_func, 0x04);
    pci_config_write(r8169_pci_bus, r8169_pci_dev, r8169_pci_func, 0x04,
                     (cmd | 0x5u) & ~(1u << 10));   /* I/O space + bus master */

    /* BAR0 is the I/O-space window; bit 0 marks it as such. */
    uint32_t bar0 = pci_get_bar(r8169_pci_bus, r8169_pci_dev, r8169_pci_func, 0);
    if (!(bar0 & 0x1u)) {
        kprintf("[r8169] BAR0 is not an I/O BAR (0x%08x); "
                "this driver uses the port-I/O window\n", bar0);
        return -1;
    }
    r8169_io_base = (uint16_t)(bar0 & ~0x3u);
    kprintf("[r8169] BAR0 I/O port base 0x%04x\n", r8169_io_base);

    /* Software reset, then wait for the chip to clear RST itself. */
    if (r8169_core_reset(&r8169_kernel_io, &r8169_st) != 0) {
        kprintf("[r8169] reset timeout; device did not clear CR.RST\n");
        return -1;
    }

    /* Station address: the chip loaded it at reset (IDR0-5), no EEPROM
     * dance needed -- exactly like the 8139. */
    r8169_core_load_mac(&r8169_kernel_io, &r8169_st);
    kprintf("[r8169] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            r8169_st.mac[0], r8169_st.mac[1], r8169_st.mac[2],
            r8169_st.mac[3], r8169_st.mac[4], r8169_st.mac[5]);

    uint64_t hhdm = boot_get_hhdm_offset();

    /* TX descriptor ring: 64 x 16 bytes, page-aligned (>= 256-aligned). */
    {
        paddr_t phys = pmm_alloc_contiguous(1);
        if (!phys) {
            kprintf("[r8169] out of memory for the TX ring\n");
            return -1;
        }
        r8169_st.tx_ring = (struct r8169_tx_desc *)(uintptr_t)(hhdm + phys);
        r8169_st.tx_ring_phys = phys;
        memset(r8169_st.tx_ring, 0, R8169_TX_RING_BYTES);
    }

    /* RX descriptor ring: 256 x 16 bytes, page-aligned. */
    {
        paddr_t phys = pmm_alloc_contiguous(1);
        if (!phys) {
            kprintf("[r8169] out of memory for the RX ring\n");
            return -1;
        }
        r8169_st.rx_ring = (struct r8169_rx_desc *)(uintptr_t)(hhdm + phys);
        r8169_st.rx_ring_phys = phys;
        memset(r8169_st.rx_ring, 0, R8169_RX_RING_BYTES);
    }

    /* TX bounce buffer (one frame in flight at a time). */
    {
        paddr_t phys = pmm_alloc_contiguous(1);
        if (!phys) {
            kprintf("[r8169] out of memory for the TX buffer\n");
            return -1;
        }
        r8169_st.tx_buf = (uint8_t *)(uintptr_t)(hhdm + phys);
        r8169_st.tx_buf_phys = phys;
    }

    /* RX packet buffers: one 16383-byte buffer per descriptor (the
     * armed size the 14-bit length field can express), carved from a
     * single contiguous 4 MiB block. */
    {
        const uint64_t pages = (R8169_NUM_RX_DESC * 16384u) / 0x1000u;
        paddr_t phys = pmm_alloc_contiguous(pages);
        if (!phys) {
            kprintf("[r8169] out of memory for the RX buffers\n");
            return -1;
        }
        uint8_t *base = (uint8_t *)(uintptr_t)(hhdm + phys);
        for (uint32_t i = 0; i < R8169_NUM_RX_DESC; i++) {
            r8169_st.rx_pkt[i]      = base + i * 16384u;
            r8169_st.rx_pkt_phys[i] = phys + i * 16384u;
        }
    }

    /* Program the chip and arm the rings (the core the host gate
     * proves).  Ring bases are page-aligned, so the 256-byte alignment
     * refusal cannot fire here -- it guards the model, and the metal
     * slot is where a misaligned allocator would be caught. */
    if (r8169_core_config(&r8169_kernel_io, &r8169_st) != 0) {
        kprintf("[r8169] ring base not 256-byte aligned; refusing\n");
        return -1;
    }

    r8169_irq_line = pci_get_interrupt_line(r8169_pci_bus, r8169_pci_dev,
                                            r8169_pci_func);
    if (r8169_irq_line < 16) {
        irq_register_handler((int)r8169_irq_line, r8169_irq_handler);
        r8169_io_wr16(&r8169_kernel_io, R8169_IMR,
                      R8169_INT_RX_OK | R8169_INT_RX_ERR |
                      R8169_INT_RX_FIFO_OVER | R8169_INT_RX_OVER |
                      R8169_INT_TX_OK | R8169_INT_TX_ERR |
                      R8169_INT_LINK_CHG);
        kprintf("[r8169] IRQ line %u enabled (IMR=0x%04x)\n",
                r8169_irq_line, r8169_io_rd16(&r8169_kernel_io, R8169_IMR));
    } else {
        kprintf("[r8169] no valid PCI INTx line (0x%02x); "
                "polling fallback only\n", r8169_irq_line);
    }

    r8169_present_flag = 1;
    kprintf("[r8169] ready: CR=0x%02x RCR=0x%08x link=%s\n",
            r8169_io_rd8(&r8169_kernel_io, R8169_CR),
            r8169_io_rd32(&r8169_kernel_io, R8169_RCR),
            r8169_link_up() ? "up" : "down");
    return 0;
}

void r8169_get_mac(uint8_t mac[6]) {
    r8169_core_get_mac(&r8169_st, mac);
}

int r8169_link_up(void) {
    if (!r8169_present_flag) return 0;
    return r8169_core_link_up(&r8169_kernel_io, &r8169_st);
}

int r8169_send(const void *data, uint32_t len) {
    if (!r8169_present_flag) return -1;
    return r8169_core_send(&r8169_kernel_io, &r8169_st, data, len);
}

int r8169_recv(void *buf, uint32_t bufsize) {
    int n = sw_rx_pop(buf, bufsize);
    if (n != 0) return n;

    /* Same contract as the e1000/8139: if no IRQ has queued anything,
     * opportunistically drain the ring so a polling caller still works
     * when INTx is unavailable. */
    if (r8169_hw_rx_drain() > 0) {
        n = sw_rx_pop(buf, bufsize);
        if (n != 0) return n;
    }
    return 0;
}

int r8169_recv_wait(void *buf, uint32_t bufsize, uint64_t timeout_ticks) {
    uint64_t start = timer_get_ticks();
    uint64_t deadline = timeout_ticks ? start + timeout_ticks : 0;
    tcb_t *cur = sched_current();
    uint64_t old_sleep_deadline = cur ? cur->sleep_deadline : 0;

    /* RESIDUE2 T5: claim the queue before the first pop. */
    {
        arch_irqflags_t flags = spinlock_acquire_irqsave(&rxq_lock);
        rx_waiters++;
        spinlock_release_irqrestore(&rxq_lock, flags);
    }

    int n;
    for (;;) {
        n = r8169_recv(buf, bufsize);
        if (n != 0) break;
        if (!r8169_link_up()) { n = -1; break; }
        if (deadline && timer_get_ticks() >= deadline) { n = 0; break; }
        if (!cur) {
            arch_cpu_relax();
            continue;
        }
        if (deadline) cur->sleep_deadline = deadline;
        wq_wait_deadline(&r8169_rx_wq, NULL, deadline);
        if (deadline) cur->sleep_deadline = old_sleep_deadline;
    }

    {
        arch_irqflags_t flags = spinlock_acquire_irqsave(&rxq_lock);
        rx_waiters--;
        spinlock_release_irqrestore(&rxq_lock, flags);
    }
    return n;
}

/* ---- netdev backend registration ---------------------------------------- */

static const struct netdev r8169_netdev = {
    .name        = "r8169",
    .send        = r8169_send,
    .recv        = r8169_recv,
    .recv_wait   = r8169_recv_wait,
    .get_mac     = r8169_get_mac,
    .link_up     = r8169_link_up,
    .rx_pop_idle = r8169_rx_pop_idle,   /* RESIDUE2 T5: the idle drain */
};

void r8169_register_netdev(void) {
    netdev_register(&r8169_netdev);
}
