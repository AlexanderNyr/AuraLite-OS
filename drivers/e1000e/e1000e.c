/* e1000e.c — Intel 82574L (e1000e) PCI-e NIC driver (RESIDUE2 T6,
 * ledger RES-46's NIC half together with vmxnet3).
 *
 * The 82574L keeps the 8254x register file (CTRL/STATUS/RCTL/TCTL,
 * RDBA/TDBA ring bases at 0x2800/0x3800, ICR/IMS) AND the legacy
 * 16-byte descriptor formats: with RFCTL.EXTEN left clear the device
 * uses legacy RX descriptors, and legacy TX descriptors are always
 * accepted (QEMU's e1000e models exactly this — e1000e_core.c's
 * e1000e_rx_use_legacy_descriptor / process_tx_desc).  So the driver
 * is the e1000e.c data path pointed at 8086:10D3, minus the EEPROM:
 * this adapter has no EERD word interface in QEMU, but the reset
 * already loads RAL/RAH with the permanent MAC, which we read instead.
 *
 * Same netdev contract as e1000e.c, including the RESIDUE2 T5 hooks
 * (idle RX drain, blocking recv_wait with the waiter count).
 */

#include <stdint.h>
#include "drivers/e1000e/e1000e.h"
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

/* ---- MMIO register offsets (Intel 8254x datasheet) ---- */
#define E1000E_CTRL    0x0000
#define E1000E_STATUS  0x0008
#define E1000E_RCTL    0x0100   /* Receive Control */
#define E1000E_TCTL    0x0400   /* Transmit Control */
#define E1000E_RDBAL   0x2800   /* RX Descriptor Base Low */
#define E1000E_RDBAH   0x2804   /* RX Descriptor Base High */
#define E1000E_RDLEN   0x2808   /* RX Descriptor Length */
#define E1000E_RDH     0x2810   /* RX Descriptor Head */
#define E1000E_RDT     0x2818   /* RX Descriptor Tail */
#define E1000E_TDBAL   0x3800   /* TX Descriptor Base Low */
#define E1000E_TDBAH   0x3804   /* TX Descriptor Base High */
#define E1000E_TDLEN   0x3808   /* TX Descriptor Length */
#define E1000E_TDH     0x3810   /* TX Descriptor Head */
#define E1000E_TDT     0x3818   /* TX Descriptor Tail */
#define E1000E_RAL     0x5400   /* Receive Address Low */
#define E1000E_RAH     0x5404   /* Receive Address High */
#define E1000E_EERD    0x0014   /* EEPROM Read */
#define E1000E_ICR     0x00C0   /* Interrupt Cause Read (read clears) */
#define E1000E_ICS     0x00C8   /* Interrupt Cause Set */
#define E1000E_IMS     0x00D0   /* Interrupt Mask Set/Read */
#define E1000E_IMC     0x00D8   /* Interrupt Mask Clear */

/* CTRL bits */
#define CTRL_FD       (1u << 0)    /* Full Duplex */
#define CTRL_SLU      (1u << 6)    /* Set Link Up (useful on emulated NICs) */

/* STATUS bits */
#define STATUS_LU     (1u << 1)    /* Link Up */

/* Interrupt cause bits. */
#define ICR_TXDW      (1u << 0)    /* TX descriptor written back */
#define ICR_LSC       (1u << 2)    /* Link status change */
#define ICR_RXDMT0    (1u << 4)    /* RX descriptor minimum threshold */
#define ICR_RXO       (1u << 6)    /* RX overrun */
#define ICR_RXT0      (1u << 7)    /* RX timer expired */

/* RCTL bits */
#define RCTL_EN       (1u << 1)    /* Receiver Enable */
#define RCTL_SECRC    (1u << 26)   /* Strip Ethernet CRC */
#define RCTL_BAM      (1u << 15)   /* Broadcast Accept Mode (accept broadcasts) */
#define RCTL_MPE      (1u << 4)    /* Multicast Promiscuous Enable -- R9: IPv6
                                    * RAs ride 33:33:00:00:00:01 and the MTA
                                    * hash table is unprogrammed; without MPE
                                    * the NIC ate every Router Advertisement
                                    * (the virtio NIC does not filter, which
                                    * is why only the e1000e lane missed SLAAC
                                    * -- measured, both NICs, same boot). */
#define RCTL_BSIZE_2048  0          /* BSIZE=00 -> 2048 bytes */

/* TCTL bits */
#define TCTL_EN       (1u << 1)    /* Transmitter Enable */

/* EEPROM bit masks */
#define EERD_DONE     (1u << 4)    /* Read done */
#define EERD_START    (1u << 0)    /* Start read */

/* Legacy TX descriptor (16 bytes). Layout per Intel 8254x datasheet:
 *   bytes 0-7:  buffer address
 *   bytes 8-9:  length
 *   byte  10:   CSO (checksum offset)
 *   byte  11:   CMD (command: bit0=EOP, bit1=IFCS, bit3=RS)
 *   byte  12:   status (bit0=DD descriptor done)
 *   byte  13:   CSS (checksum start)
 *   bytes 14-15: special */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* Legacy RX descriptor (16 bytes). */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct rx_desc {
    uint64_t addr;        /* physical address of the packet buffer */
    uint16_t length;      /* received packet length */
    uint16_t checksum;    /* checksum */
    uint8_t  status;      /* status */
    uint8_t  errors;      /* error bits */
    uint16_t special;     /* special field */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

static volatile uint32_t *mmio = NULL;   /* HHDM-mapped register file */
static uint8_t  mac_addr[6];
static uint8_t  pci_bus, pci_dev, pci_func;

/* TX/RX descriptor rings + packet buffers. These must be volatile because the
 * NIC writes to them via DMA; without volatile the compiler caches reads. */
static volatile struct tx_desc *tx_ring;
static volatile struct rx_desc *rx_ring;
static uint8_t *tx_buffers[E1000E_NUM_TX_DESC];
static uint8_t *rx_buffers[E1000E_NUM_RX_DESC];
static uint32_t tx_tail = 0;

#define E1000E_SW_RX_QUEUE_LEN 64
struct sw_rx_packet {
    uint16_t len;
    uint8_t  data[E1000E_PKT_BUF_SIZE];
};

static struct sw_rx_packet sw_rx_queue[E1000E_SW_RX_QUEUE_LEN];
static uint32_t sw_rx_head = 0;
static uint32_t sw_rx_tail = 0;
static uint32_t sw_rx_count = 0;
static uint32_t sw_rx_drops = 0;
/* RESIDUE2 T5 (the idle RX drain): how many e1000e_recv_wait() callers are
 * in flight.  Guarded by rxq_lock so the idle drain's check-and-pop
 * (e1000e_rx_pop_idle) is atomic against a waiter appearing: a drain can
 * never steal a frame a blocking consumer is about to claim. */
static volatile int rx_waiters = 0;
/* RESIDUE2 T5: frames consumed by the idle drain (receipt counter). */
static uint32_t idle_drained = 0;
static spinlock_t rxq_lock = SPINLOCK_UNLOCKED;
static spinlock_t hw_rx_lock = SPINLOCK_UNLOCKED;
static struct wait_queue e1000e_rx_wq;
static struct wait_queue e1000e_tx_wq;
static uint8_t e1000e_irq_line = 0xFF;
static uint32_t rx_packet_count = 0;
static uint32_t last_rdh = 0;

static int sw_rx_push(const void *data, uint16_t len) {
    if (!data || len == 0) return 0;
    if (len > E1000E_PKT_BUF_SIZE) len = E1000E_PKT_BUF_SIZE;

    uint64_t flags = spinlock_acquire_irqsave(&rxq_lock);
    if (sw_rx_count >= E1000E_SW_RX_QUEUE_LEN) {
        sw_rx_drops++;
        spinlock_release_irqrestore(&rxq_lock, flags);
        return 0;
    }
    struct sw_rx_packet *p = &sw_rx_queue[sw_rx_tail];
    memcpy(p->data, data, len);
    p->len = len;
    sw_rx_tail = (sw_rx_tail + 1) % E1000E_SW_RX_QUEUE_LEN;
    sw_rx_count++;
    spinlock_release_irqrestore(&rxq_lock, flags);
    return 1;
}

static int sw_rx_pop(void *buf, uint32_t bufsize) {
    if (!buf || bufsize == 0) return -1;

    uint64_t flags = spinlock_acquire_irqsave(&rxq_lock);
    if (sw_rx_count == 0) {
        spinlock_release_irqrestore(&rxq_lock, flags);
        return 0;
    }
    struct sw_rx_packet *p = &sw_rx_queue[sw_rx_head];
    uint16_t len = p->len;
    if (len > bufsize) len = (uint16_t)bufsize;
    memcpy(buf, p->data, len);
    p->len = 0;
    sw_rx_head = (sw_rx_head + 1) % E1000E_SW_RX_QUEUE_LEN;
    sw_rx_count--;
    spinlock_release_irqrestore(&rxq_lock, flags);
    return (int)len;
}

/* RESIDUE2 T5 (the idle RX drain): atomic check-and-pop for
 * netdev_passive_drain().  The waiter check and the pop share rxq_lock,
 * so a recv_wait() that appears between two drained frames takes the
 * queue back immediately (return -1) and nothing is ever stolen from
 * under it.  Prints the receipt line at a 2^n cadence — one line when
 * the drain first proves itself, then every 256 frames: readable under
 * sustained unsolicited traffic, never a flood. */
static int e1000e_rx_pop_idle(void *buf, uint32_t bufsize) {
    if (!buf || bufsize == 0) return 0;

    uint64_t flags = spinlock_acquire_irqsave(&rxq_lock);
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
    sw_rx_head = (sw_rx_head + 1) % E1000E_SW_RX_QUEUE_LEN;
    sw_rx_count--;
    spinlock_release_irqrestore(&rxq_lock, flags);

    idle_drained++;
    if (idle_drained == 1 || (idle_drained & 0xFF) == 0) {
        kprintf("[e1000e] idle drain: %u unsolicited frame(s) consumed "
                "(queue kept empty; ARP/NDP answered)\n", idle_drained);
    }
    return (int)len;
}

/* ---- MMIO helpers ---- */

static inline void mmio_write(uint32_t reg, uint32_t val) {
    mmio[reg / 4] = val;
}

static inline uint32_t mmio_read(uint32_t reg) {
    return mmio[reg / 4];
}

static const char *e1000e_device_name(uint16_t device_id) {
    return (device_id == E1000E_DEVICE_82574L) ? "82574L" : "e1000e";
}

static int e1000e_hw_rx_drain(void) {
    if (!mmio || !rx_ring) return 0;

    int drained = 0;
    uint64_t flags = spinlock_acquire_irqsave(&hw_rx_lock);
    uint32_t rdh = mmio_read(E1000E_RDH);
    while (rdh != last_rdh) {
        uint32_t idx = last_rdh % E1000E_NUM_RX_DESC;
        uint16_t pkt_len = rx_ring[idx].length;
        if (pkt_len > E1000E_PKT_BUF_SIZE) pkt_len = E1000E_PKT_BUF_SIZE;
        if (pkt_len > 0) {
            if (sw_rx_push(rx_buffers[idx], pkt_len)) {
                drained++;
                rx_packet_count++;
            }
        }

        rx_ring[idx].status = 0;
        last_rdh = (last_rdh + 1) % E1000E_NUM_RX_DESC;
        uint32_t new_rdt = (last_rdh + E1000E_NUM_RX_DESC - 1) % E1000E_NUM_RX_DESC;
        mmio_write(E1000E_RDT, new_rdt);
        rdh = mmio_read(E1000E_RDH);
    }
    spinlock_release_irqrestore(&hw_rx_lock, flags);
    return drained;
}

static void e1000e_irq_handler(struct registers *regs) {
    (void)regs;
    if (!mmio) return;

    uint32_t icr = mmio_read(E1000E_ICR); /* read clears pending causes */
    if (icr == 0) return;

    if (icr & (ICR_RXT0 | ICR_RXDMT0 | ICR_RXO)) {
        int drained = e1000e_hw_rx_drain();
        if (drained > 0) {
            wq_wake_all(&e1000e_rx_wq);
        }
        if (icr & ICR_RXO) {
            /* RESIDUE2 T5: rate-limit the overrun line.  A queue that
             * overruns once a burst tends to overrun every burst — the
             * old per-interrupt print was the serial-log flood the TODO
             * named.  At most one line per ~5 s of ticks. */
            static uint64_t last_overrun_print = 0;
            uint64_t now = timer_get_ticks();
            if (now - last_overrun_print >= 500) {   /* PIT @ 100 Hz */
                last_overrun_print = now;
                kprintf("[e1000e] RX overrun (drops=%u)\n", sw_rx_drops);
            }
        }
    }

    if (icr & (ICR_TXDW | ICR_LSC)) {
        wq_wake_all(&e1000e_tx_wq);
    }
}

static int e1000e_find_supported_device(uint8_t *out_bus, uint8_t *out_dev,
                                       uint8_t *out_func, uint16_t *out_id) {
    static const uint16_t supported[] = {
        E1000E_DEVICE_82574L,   /* QEMU -device e1000e; real 82574L */
    };

    for (uint32_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (pci_find_device(E1000E_VENDOR_ID, supported[i],
                            out_bus, out_dev, out_func) == 0) {
            if (out_id) *out_id = supported[i];
            return 0;
        }
    }
    return -1;
}

int e1000e_init(void) {
    /* 1) Find an e1000e-compatible NIC on the PCI bus. QEMU, VirtualBox and
     * VMware expose slightly different 8254x device IDs, so accept the common
     * legacy variants instead of only QEMU's 82540EM (0x100E). */
    uint16_t device_id = 0;
    if (e1000e_find_supported_device(&pci_bus, &pci_dev, &pci_func,
                                    &device_id) != 0) {
        kprintf("[e1000e] NIC not found on PCI bus "
                "(supported: 82540EM/82545EM/82543GC)\n");
        return -1;
    }
    kprintf("[e1000e] found %s (0x%04x) at PCI %u:%u.%u\n",
            e1000e_device_name(device_id), device_id,
            pci_bus, pci_dev, pci_func);

    wq_init(&e1000e_rx_wq);
    wq_init(&e1000e_tx_wq);
    sw_rx_head = sw_rx_tail = sw_rx_count = sw_rx_drops = 0;
    last_rdh = 0;
    rx_packet_count = 0;

    /* 2) Enable bus mastering + memory space. */
    pci_enable_bus_master(pci_bus, pci_dev, pci_func);
    /* Make sure legacy INTx is enabled in PCI Command (bit 10 disables INTx). */
    uint32_t pci_cmd = pci_config_read(pci_bus, pci_dev, pci_func, 0x04);
    pci_config_write(pci_bus, pci_dev, pci_func, 0x04, pci_cmd & ~(1u << 10));

    /* 3) Map the MMIO BAR0 into the HHDM. The HHDM only covers physical RAM,
     *    so we must explicitly map the MMIO region (which lives at ~4GB). */
    uint32_t bar0 = pci_get_bar(pci_bus, pci_dev, pci_func, 0);
    uint32_t mmio_phys = bar0 & ~0xF;   /* mask type/flags bits */
    uint64_t hhdm = boot_get_hhdm_offset();

    /* Map 128 KiB of MMIO space (128 * 4KB pages = 32 pages). */
    for (uint32_t off = 0; off < 0x20000; off += 0x1000) {
        paging_map(hhdm + mmio_phys + off, mmio_phys + off,
                   PAGE_FLAGS_MMIO);
    }

    mmio = (volatile uint32_t *)(uintptr_t)(hhdm + mmio_phys);
    kprintf("[e1000e] BAR0 at phys 0x%x -> mmio 0x%llx\n",
            mmio_phys, (unsigned long long)(uintptr_t)mmio);

    /* Reset the controller (RST bit in CTRL). */
    mmio_write(E1000E_CTRL, mmio_read(E1000E_CTRL) | (1u << 26));
    int reset_to = 1000000;
    while ((mmio_read(E1000E_CTRL) & (1u << 26)) && reset_to-- > 0) {
        arch_cpu_relax();
    }

    /* Force the emulated link up where supported. VirtualBox/VMware legacy
     * e1000e adapters sometimes report LU=0 until CTRL.SLU/FD is asserted; if
     * the VM cable is genuinely disconnected this will still remain down. */
    mmio_write(E1000E_CTRL, mmio_read(E1000E_CTRL) | CTRL_FD | CTRL_SLU);
    int link_wait = 100000;
    while (!(mmio_read(E1000E_STATUS) & STATUS_LU) && link_wait-- > 0) {
        arch_cpu_relax();
    }
    kprintf("[e1000e] STATUS=0x%08x (LU=%u)\n",
            mmio_read(E1000E_STATUS),
            (mmio_read(E1000E_STATUS) & STATUS_LU) ? 1 : 0);

    /* 4) Read the MAC address. The 82574L has no EERD word interface
     *    under QEMU, but the device reset already loaded RAL/RAH with
     *    the permanent MAC; fall back to the QEMU default if unpopulated. */
    {
        uint32_t ral = mmio_read(E1000E_RAL);
        uint32_t rah = mmio_read(E1000E_RAH);
        mac_addr[0] = ral & 0xFF;
        mac_addr[1] = (ral >> 8) & 0xFF;
        mac_addr[2] = (ral >> 16) & 0xFF;
        mac_addr[3] = (ral >> 24) & 0xFF;
        mac_addr[4] = rah & 0xFF;
        mac_addr[5] = (rah >> 8) & 0xFF;
        if (mac_addr[0] == 0 && mac_addr[5] == 0) {
            /* RAL/RAH not populated — use the QEMU default. */
            uint8_t default_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
            memcpy(mac_addr, default_mac, 6);
            kprintf("[e1000e] using default QEMU MAC\n");
        } else {
            kprintf("[e1000e] MAC from RAL/RAH\n");
        }
    }
    kprintf("[e1000e] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5]);

    /* 5) Keep interrupts masked while rings are being initialised. */
    mmio_write(E1000E_IMC, 0xFFFFFFFF);
    (void)mmio_read(E1000E_ICR);       /* clear any stale cause bits */

    /* 6) Set up the TX descriptor ring. Descriptors and buffers must be in
     *    memory the NIC can DMA — we allocate physical frames from the PMM
     *    and access them through the HHDM. The NIC needs the physical address. */
    {
        uint64_t ring_phys = pmm_alloc_contiguous(
            (sizeof(struct tx_desc) * E1000E_NUM_TX_DESC + 0xFFF) / 0x1000);
        tx_ring = (volatile struct tx_desc *)(uintptr_t)(hhdm + ring_phys);
        memset((void *)tx_ring, 0, sizeof(struct tx_desc) * E1000E_NUM_TX_DESC);
        for (int i = 0; i < E1000E_NUM_TX_DESC; i++) {
            uint64_t buf_phys = pmm_alloc_frame();
            tx_buffers[i] = (uint8_t *)(uintptr_t)(hhdm + buf_phys);
            tx_ring[i].addr = buf_phys;
        }
        mmio_write(E1000E_TDBAL, (uint32_t)(ring_phys & 0xFFFFFFFF));
        mmio_write(E1000E_TDBAH, (uint32_t)(ring_phys >> 32));
        mmio_write(E1000E_TDLEN, E1000E_NUM_TX_DESC * 16);
        mmio_write(E1000E_TDH, 0);
        mmio_write(E1000E_TDT, 0);
        mmio_write(E1000E_TCTL, TCTL_EN | (0x10 << 4) | (0x40 << 12));
        mmio_write(0x0410, 0x0060200A);
    }

    /* 7) Set up the RX descriptor ring (same approach). */
    {
        uint64_t ring_phys = pmm_alloc_contiguous(
            (sizeof(struct rx_desc) * E1000E_NUM_RX_DESC + 0xFFF) / 0x1000);
        rx_ring = (volatile struct rx_desc *)(uintptr_t)(hhdm + ring_phys);
        memset((void *)rx_ring, 0, sizeof(struct rx_desc) * E1000E_NUM_RX_DESC);
        for (int i = 0; i < E1000E_NUM_RX_DESC; i++) {
            uint64_t buf_phys = pmm_alloc_frame();
            rx_buffers[i] = (uint8_t *)(uintptr_t)(hhdm + buf_phys);
            rx_ring[i].addr   = buf_phys;
            rx_ring[i].status = 0;
        }
        mmio_write(E1000E_RDBAL, (uint32_t)(ring_phys & 0xFFFFFFFF));
        mmio_write(E1000E_RDBAH, (uint32_t)(ring_phys >> 32));
        mmio_write(E1000E_RDLEN, E1000E_NUM_RX_DESC * 16);
        mmio_write(E1000E_RDH, 0);
        mmio_write(E1000E_RDT, E1000E_NUM_RX_DESC - 1);
        kprintf("[e1000e] RX ring phys=0x%llx, desc0_addr=0x%llx desc0_status=%u\n",
                (unsigned long long)ring_phys,
                (unsigned long long)rx_ring[0].addr,
                rx_ring[0].status);
        kprintf("[e1000e] RCTL=0x%08x\n", mmio_read(E1000E_RCTL));
    }

    /* Program our MAC address into the Receive Address Low/High registers. */
    uint32_t ral = mac_addr[0] | (mac_addr[1] << 8) |
                   (mac_addr[2] << 16) | (mac_addr[3] << 24);
    uint32_t rah = mac_addr[4] | (mac_addr[5] << 8) | (1u << 31);  /* AV bit */
    mmio_write(E1000E_RAL, ral);
    mmio_write(E1000E_RAH, rah);

    /* Enable RX: EN=1, SECRC=1 (strip CRC), BSIZE=0 (2048), BAM=1 (accept
     * broadcasts — needed for DHCP), UPE=1 (unicast promiscuous). */
    /* R9: open the multicast table too -- IPv6 NDP rides 33:33:xx
     * group addresses.  MTA[0..127] = all-ones accepts every hash
     * bucket; belt to MPE's braces (measured: this QEMU's e1000e
     * still dropped the RA with MPE alone). */
    for (int mta = 0; mta < 128; mta++)
        mmio_write(0x5200 + mta * 4, 0xFFFFFFFFu);
    mmio_write(E1000E_RCTL, RCTL_EN | RCTL_SECRC | RCTL_BSIZE_2048 | RCTL_BAM |
                           RCTL_MPE | (1u << 3));

    e1000e_irq_line = pci_get_interrupt_line(pci_bus, pci_dev, pci_func);
    if (e1000e_irq_line < 16) {
        irq_register_handler((int)e1000e_irq_line, e1000e_irq_handler);
        (void)mmio_read(E1000E_ICR);
        mmio_write(E1000E_IMS, ICR_RXT0 | ICR_RXDMT0 | ICR_RXO | ICR_TXDW | ICR_LSC);
        kprintf("[e1000e] IRQ line %u enabled (IMS=0x%08x)\n",
                e1000e_irq_line, mmio_read(E1000E_IMS));
    } else {
        kprintf("[e1000e] no valid PCI INTx line (0x%02x); polling fallback only\n",
                e1000e_irq_line);
    }

    kprintf("[e1000e] TX/RX rings initialised, RCTL=0x%08x\n",
            mmio_read(E1000E_RCTL));
    return 0;
}

void e1000e_get_mac(uint8_t mac[6]) {
    memcpy(mac, mac_addr, 6);
}

int e1000e_link_up(void) {
    return (mmio != NULL && (mmio_read(E1000E_STATUS) & STATUS_LU)) ? 1 : 0;
}

volatile uint32_t *e1000e_get_mmio(void) {
    return mmio;
}

/* Debug: return the virtual address of RX descriptor 0 for direct access. */
volatile uint8_t *e1000e_get_rx_desc0(void) {
    return (volatile uint8_t *)&rx_ring[0];
}

int e1000e_send(const void *data, uint32_t len) {
    if (!e1000e_link_up()) {
        return -1;
    }
    if (len > E1000E_PKT_BUF_SIZE) {
        len = E1000E_PKT_BUF_SIZE;
    }
    memcpy(tx_buffers[tx_tail], data, len);

    tx_ring[tx_tail].length = (uint16_t)len;
    tx_ring[tx_tail].cso    = 0;
    tx_ring[tx_tail].cmd    = 0x0B;  /* EOP + IFCS + RS */
    tx_ring[tx_tail].status = 0;

    uint32_t old_tail = tx_tail;
    tx_tail = (tx_tail + 1) % E1000E_NUM_TX_DESC;
    mmio_write(E1000E_TDT, tx_tail);

    /* Wait for the descriptor to be done (DD bit in status). */
    int timeout = 100000;
    while ((tx_ring[old_tail].status & 0x01) == 0 && timeout-- > 0) {
        arch_cpu_relax();
    }
    if (timeout < 0) {
        kprintf("[e1000e] TX timeout: desc[%u].status=0x%02x TDH=%u TDT=%u\n",
                old_tail, tx_ring[old_tail].status,
                mmio_read(E1000E_TDH), mmio_read(E1000E_TDT));
        return -1;
    }
    return (int)len;
}

int e1000e_recv(void *buf, uint32_t bufsize) {
    int n = sw_rx_pop(buf, bufsize);
    if (n != 0) return n;

    /* Preserve legacy non-blocking polling semantics: if no IRQ has queued a
     * packet yet, opportunistically drain the hardware ring and try again. */
    if (e1000e_hw_rx_drain() > 0) {
        n = sw_rx_pop(buf, bufsize);
        if (n != 0) return n;
    }
    return 0;
}

int e1000e_recv_wait(void *buf, uint32_t bufsize, uint64_t timeout_ticks) {
    uint64_t start = timer_get_ticks();
    uint64_t deadline = timeout_ticks ? start + timeout_ticks : 0;
    tcb_t *cur = sched_current();
    uint64_t old_sleep_deadline = cur ? cur->sleep_deadline : 0;

    /* RESIDUE2 T5: mark this consumer BEFORE the first pop attempt (under
     * the same lock e1000e_rx_pop_idle checks), so the idle drain stops
     * touching the queue the moment a waiter exists. */
    {
        uint64_t flags = spinlock_acquire_irqsave(&rxq_lock);
        rx_waiters++;
        spinlock_release_irqrestore(&rxq_lock, flags);
    }

    int n;
    for (;;) {
        n = e1000e_recv(buf, bufsize);
        if (n != 0) break;
        if (!e1000e_link_up()) { n = -1; break; }
        if (deadline && timer_get_ticks() >= deadline) { n = 0; break; }

        if (!cur) {
            arch_cpu_relax();
            continue;
        }

        if (deadline) cur->sleep_deadline = deadline;
        wq_wait(&e1000e_rx_wq, NULL);
        if (deadline) cur->sleep_deadline = old_sleep_deadline;
    }

    {
        uint64_t flags = spinlock_acquire_irqsave(&rxq_lock);
        rx_waiters--;
        spinlock_release_irqrestore(&rxq_lock, flags);
    }
    return n;
}

int e1000e_recv_blocking(void *buf, uint32_t bufsize) {
    return e1000e_recv_wait(buf, bufsize, 0);
}

/* ---- netdev backend registration ---------------------------------------- */

static const struct netdev e1000e_netdev = {
    .name        = "e1000e",
    .send        = e1000e_send,
    .recv        = e1000e_recv,
    .recv_wait   = e1000e_recv_wait,
    .get_mac     = e1000e_get_mac,
    .link_up     = e1000e_link_up,
    .rx_pop_idle = e1000e_rx_pop_idle,   /* RESIDUE2 T5: the idle drain */
};

void e1000e_register_netdev(void) {
    netdev_register(&e1000e_netdev);
}
