/* e1000.c — Intel 82540EM NIC driver for QEMU.
 *
 * Legacy TX/RX descriptor rings, polling-based (no interrupts). The MMIO
 * register file is reached through Limine's HHDM so we can read/write the
 * device registers without setting up dedicated paging.
 */

#include <stdint.h>
#include "drivers/e1000/e1000.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/limine_requests.h"

/* ---- MMIO register offsets (Intel 8254x datasheet) ---- */
#define E1000_CTRL    0x0000
#define E1000_STATUS  0x0008
#define E1000_RCTL    0x0100   /* Receive Control */
#define E1000_TCTL    0x0400   /* Transmit Control */
#define E1000_RDBAL   0x2800   /* RX Descriptor Base Low */
#define E1000_RDBAH   0x2804   /* RX Descriptor Base High */
#define E1000_RDLEN   0x2808   /* RX Descriptor Length */
#define E1000_RDH     0x2810   /* RX Descriptor Head */
#define E1000_RDT     0x2818   /* RX Descriptor Tail */
#define E1000_TDBAL   0x3800   /* TX Descriptor Base Low */
#define E1000_TDBAH   0x3804   /* TX Descriptor Base High */
#define E1000_TDLEN   0x3808   /* TX Descriptor Length */
#define E1000_TDH     0x3810   /* TX Descriptor Head */
#define E1000_TDT     0x3818   /* TX Descriptor Tail */
#define E1000_RAL     0x5400   /* Receive Address Low */
#define E1000_RAH     0x5404   /* Receive Address High */
#define E1000_EERD    0x0014   /* EEPROM Read */

/* RCTL bits */
#define RCTL_EN       (1u << 1)    /* Receiver Enable */
#define RCTL_SECRC    (1u << 26)   /* Strip Ethernet CRC */
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
struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

/* Legacy RX descriptor (16 bytes). */
struct rx_desc {
    uint64_t addr;        /* physical address of the packet buffer */
    uint16_t length;      /* received packet length */
    uint16_t checksum;    /* checksum */
    uint8_t  status;      /* status */
    uint8_t  errors;      /* error bits */
    uint16_t special;     /* special field */
} __attribute__((packed));

static volatile uint32_t *mmio = NULL;   /* HHDM-mapped register file */
static uint8_t  mac_addr[6];
static uint8_t  pci_bus, pci_dev, pci_func;

/* TX/RX descriptor rings + packet buffers. These must be volatile because the
 * NIC writes to them via DMA; without volatile the compiler caches reads. */
static volatile struct tx_desc *tx_ring;
static volatile struct rx_desc *rx_ring;
static uint8_t *tx_buffers[E1000_NUM_TX_DESC];
static uint8_t *rx_buffers[E1000_NUM_RX_DESC];
static uint32_t tx_tail = 0;
static uint32_t rx_next = 0;

/* ---- MMIO helpers ---- */

static inline void mmio_write(uint32_t reg, uint32_t val) {
    mmio[reg / 4] = val;
}

static inline uint32_t mmio_read(uint32_t reg) {
    return mmio[reg / 4];
}

/* Read a word (16 bits) from the EEPROM. QEMU may not implement this
 * correctly, so we add a timeout and return 0xFFFF on failure. */
static uint16_t eeprom_read(uint8_t word) {
    uint32_t val = ((uint32_t)word << 8) | EERD_START;
    mmio_write(E1000_EERD, val);
    int timeout = 100000;
    while (!(mmio_read(E1000_EERD) & EERD_DONE) && timeout-- > 0) {
        __asm__ volatile ("pause");
    }
    if (timeout < 0) {
        return 0xFFFF;   /* EEPROM not available */
    }
    return (uint16_t)(mmio_read(E1000_EERD) >> 16);
}

int e1000_init(void) {
    /* 1) Find the e1000 on the PCI bus. */
    if (pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_ID,
                        &pci_bus, &pci_dev, &pci_func) != 0) {
        kprintf("[e1000] NIC not found on PCI bus\n");
        return -1;
    }
    kprintf("[e1000] found at PCI %u:%u.%u\n",
            pci_bus, pci_dev, pci_func);

    /* 2) Enable bus mastering + memory space. */
    pci_enable_bus_master(pci_bus, pci_dev, pci_func);

    /* 3) Map the MMIO BAR0 into the HHDM. The HHDM only covers physical RAM,
     *    so we must explicitly map the MMIO region (which lives at ~4GB). */
    uint32_t bar0 = pci_get_bar(pci_bus, pci_dev, pci_func, 0);
    uint32_t mmio_phys = bar0 & ~0xF;   /* mask type/flags bits */
    uint64_t hhdm = limine_get_hhdm_offset();

    /* Map 128 KiB of MMIO space (128 * 4KB pages = 32 pages). */
    for (uint32_t off = 0; off < 0x20000; off += 0x1000) {
        paging_map(hhdm + mmio_phys + off, mmio_phys + off,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE);
    }

    mmio = (volatile uint32_t *)(uintptr_t)(hhdm + mmio_phys);
    kprintf("[e1000] BAR0 at phys 0x%x -> mmio 0x%llx\n",
            mmio_phys, (unsigned long long)(uintptr_t)mmio);

    /* Reset the controller (RST bit in CTRL). */
    mmio_write(E1000_CTRL, mmio_read(E1000_CTRL) | (1u << 26));
    int reset_to = 1000000;
    while ((mmio_read(E1000_CTRL) & (1u << 26)) && reset_to-- > 0) {
        __asm__ volatile ("pause");
    }
    /* Configure speed/duplex: FD=1 (full duplex), speed bits for 1 Gbps.
     * Actually, just let auto-negotiation work — set FRCSPD=0, FRCDPLX=0. */
    kprintf("[e1000] STATUS=0x%08x (LU=%u)\n",
            mmio_read(E1000_STATUS),
            (mmio_read(E1000_STATUS) >> 1) & 1);

    /* 4) Read the MAC address. QEMU's EEPROM emulation can be unreliable, so
     *    we try EEPROM first, then RAL/RAH, and fall back to the QEMU default. */
    uint16_t macw[3];
    int eeprom_ok = 1;
    for (int i = 0; i < 3; i++) {
        macw[i] = eeprom_read((uint8_t)i);
        if (macw[i] == 0xFFFF) {
            eeprom_ok = 0;
        }
    }
    if (eeprom_ok) {
        for (int i = 0; i < 3; i++) {
            mac_addr[i * 2]     = macw[i] & 0xFF;
            mac_addr[i * 2 + 1] = (macw[i] >> 8) & 0xFF;
        }
    } else {
        /* EEPROM failed. Try RAL/RAH; fall back to QEMU default MAC. */
        uint32_t ral = mmio_read(E1000_RAL);
        uint32_t rah = mmio_read(E1000_RAH);
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
            kprintf("[e1000] using default QEMU MAC\n");
        } else {
            kprintf("[e1000] MAC from RAL/RAH\n");
        }
    }
    kprintf("[e1000] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5]);

    /* 5) Disable interrupts (we poll). */
    mmio_write(0x00D0, 0xFFFFFFFF);   /* IMS */
    mmio_write(0x00D8, 0xFFFFFFFF);   /* IMC — clear all */

    /* 6) Set up the TX descriptor ring. Descriptors and buffers must be in
     *    memory the NIC can DMA — we allocate physical frames from the PMM
     *    and access them through the HHDM. The NIC needs the physical address. */
    {
        uint64_t ring_phys = pmm_alloc_contiguous(
            (sizeof(struct tx_desc) * E1000_NUM_TX_DESC + 0xFFF) / 0x1000);
        tx_ring = (struct tx_desc *)(uintptr_t)(hhdm + ring_phys);
        memset(tx_ring, 0, sizeof(struct tx_desc) * E1000_NUM_TX_DESC);
        for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
            uint64_t buf_phys = pmm_alloc_frame();
            tx_buffers[i] = (uint8_t *)(uintptr_t)(hhdm + buf_phys);
            tx_ring[i].addr = buf_phys;
        }
        mmio_write(E1000_TDBAL, (uint32_t)(ring_phys & 0xFFFFFFFF));
        mmio_write(E1000_TDBAH, (uint32_t)(ring_phys >> 32));
        mmio_write(E1000_TDLEN, E1000_NUM_TX_DESC * 16);
        mmio_write(E1000_TDH, 0);
        mmio_write(E1000_TDT, 0);
        mmio_write(E1000_TCTL, TCTL_EN | (0x10 << 4) | (0x40 << 12));
        mmio_write(0x0410, 0x0060200A);
    }

    /* 7) Set up the RX descriptor ring (same approach). */
    {
        uint64_t ring_phys = pmm_alloc_contiguous(
            (sizeof(struct rx_desc) * E1000_NUM_RX_DESC + 0xFFF) / 0x1000);
        rx_ring = (struct rx_desc *)(uintptr_t)(hhdm + ring_phys);
        memset(rx_ring, 0, sizeof(struct rx_desc) * E1000_NUM_RX_DESC);
        for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
            uint64_t buf_phys = pmm_alloc_frame();
            rx_buffers[i] = (uint8_t *)(uintptr_t)(hhdm + buf_phys);
            rx_ring[i].addr   = buf_phys;
            rx_ring[i].status = 0;
        }
        mmio_write(E1000_RDBAL, (uint32_t)(ring_phys & 0xFFFFFFFF));
        mmio_write(E1000_RDBAH, (uint32_t)(ring_phys >> 32));
        mmio_write(E1000_RDLEN, E1000_NUM_RX_DESC * 16);
        mmio_write(E1000_RDH, 0);
        mmio_write(E1000_RDT, E1000_NUM_RX_DESC - 1);
        rx_next = 0;
        kprintf("[e1000] RX ring phys=0x%llx, desc0_addr=0x%llx desc0_status=%u\n",
                (unsigned long long)ring_phys,
                (unsigned long long)rx_ring[0].addr,
                rx_ring[0].status);
        kprintf("[e1000] RCTL=0x%08x\n", mmio_read(E1000_RCTL));
    }

    /* Program our MAC address into the Receive Address Low/High registers. */
    uint32_t ral = mac_addr[0] | (mac_addr[1] << 8) |
                   (mac_addr[2] << 16) | (mac_addr[3] << 24);
    uint32_t rah = mac_addr[4] | (mac_addr[5] << 8) | (1u << 31);  /* AV bit */
    mmio_write(E1000_RAL, ral);
    mmio_write(E1000_RAH, rah);

    /* Enable RX: EN=1, SECRC=1 (strip CRC), BSIZE=0 (2048), unicast promiscuous. */
    mmio_write(E1000_RCTL, RCTL_EN | RCTL_SECRC | RCTL_BSIZE_2048 | (1u << 3) | (1u << 4));

    kprintf("[e1000] TX/RX rings initialised, RCTL=0x%08x\n",
            mmio_read(E1000_RCTL));
    return 0;
}

void e1000_get_mac(uint8_t mac[6]) {
    memcpy(mac, mac_addr, 6);
}

volatile uint32_t *e1000_get_mmio(void) {
    return mmio;
}

/* Debug: return the virtual address of RX descriptor 0 for direct access. */
volatile uint8_t *e1000_get_rx_desc0(void) {
    return (volatile uint8_t *)&rx_ring[0];
}

int e1000_send(const void *data, uint32_t len) {
    if (len > E1000_PKT_BUF_SIZE) {
        len = E1000_PKT_BUF_SIZE;
    }
    memcpy(tx_buffers[tx_tail], data, len);

    tx_ring[tx_tail].length = (uint16_t)len;
    tx_ring[tx_tail].cso    = 0;
    tx_ring[tx_tail].cmd    = 0x0B;  /* EOP + IFCS + RS */
    tx_ring[tx_tail].status = 0;

    uint32_t old_tail = tx_tail;
    tx_tail = (tx_tail + 1) % E1000_NUM_TX_DESC;
    mmio_write(E1000_TDT, tx_tail);

    /* Wait for the descriptor to be done (DD bit in status). */
    int timeout = 1000000;
    while ((tx_ring[old_tail].status & 0x01) == 0 && timeout-- > 0) {
        __asm__ volatile ("pause");
    }
    if (timeout < 0) {
        kprintf("[e1000] TX timeout: desc[%u].status=0x%02x TDH=%u TDT=%u\n",
                old_tail, tx_ring[old_tail].status,
                mmio_read(E1000_TDH), mmio_read(E1000_TDT));
        return -1;
    }
    return (int)len;
}

static uint32_t rx_packet_count = 0;
static uint32_t last_rdh = 0;

int e1000_recv(void *buf, uint32_t bufsize) {
    /* Poll the RDH MMIO register instead of the descriptor status byte.
     * QEMU's e1000 advances RDH when a packet arrives; the descriptor status
     * may not be visible through the HHDM mapping due to DMA ordering. */
    uint32_t rdh = mmio_read(E1000_RDH);
    if (rdh == last_rdh) {
        return 0;
    }

    uint32_t idx = last_rdh % E1000_NUM_RX_DESC;
    last_rdh = (last_rdh + 1) % E1000_NUM_RX_DESC;

    rx_packet_count++;
    uint16_t pkt_len = rx_ring[idx].length;
    if (pkt_len > bufsize) {
        pkt_len = (uint16_t)bufsize;
    }
    if (pkt_len > 0) {
        memcpy(buf, rx_buffers[idx], pkt_len);
    }

    /* Give the descriptor back to the hardware. */
    uint32_t new_rdt = (last_rdh + E1000_NUM_RX_DESC - 1) % E1000_NUM_RX_DESC;
    mmio_write(E1000_RDT, new_rdt);

    return pkt_len;
}
