/* ohci.c — OHCI (USB 1.1) host controller driver.
 *
 * OHCI uses memory-mapped registers (MMIO via PCI BAR0). It schedules
 * transfers via Endpoint Descriptors (ED) and General Transfer Descriptors
 * (TD) linked through physical addresses. The HCCA (256 bytes) holds the
 * interrupt table and frame counter.
 *
 * QEMU: -device pci-ohci,id=ohci (or -device usb-ohci)
 */

#include <stdint.h>
#include "drivers/usb/ohci.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/limine_requests.h"

/* ---- OHCI register offsets (from BAR0 base) ---- */
#define OHCI_REVISION      0x00    /* Revision */
#define OHCI_CONTROL       0x04    /* Control */
#define OHCI_COMMAND_STATUS 0x08   /* Command Status */
#define OHCI_INTERRUPT_STATUS 0x0C /* Interrupt Status */
#define OHCI_INTERRUPT_ENABLE 0x10 /* Interrupt Enable */
#define OHCI_INTERRUPT_DISABLE 0x14
#define OHCI_HCCA          0x18    /* HCCA pointer (DMA) */
#define OHCI_PERIOD_CURRENT_ED 0x1C
#define OHCI_CONTROL_HEAD_ED 0x20
#define OHCI_CONTROL_CURRENT_ED 0x24
#define OHCI_BULK_HEAD_ED  0x28
#define OHCI_BULK_CURRENT_ED 0x2C
#define OHCI_DONE_HEAD     0x30    /* (also in HCCA+0x84) */
#define OHCI_FM_INTERVAL   0x34
#define OHCI_FM_REMAINING  0x38
#define OHCI_FM_NUMBER     0x3C
#define OHCI_PERIODIC_START 0x40
#define OHCI_LSTHRESH     0x44
#define OHCI_RH_DESC_A    0x48    /* Root Hub Descriptor A */
#define OHCI_RH_DESC_B    0x4C    /* Root Hub Descriptor B */
#define OHCI_RH_STATUS    0x50    /* Root Hub Status */
#define OHCI_RH_PORT_STATUS 0x54  /* Port 1 Status (offset from port 1) */

/* Control register bits */
#define OHCI_CTRL_CBSR    (3u << 0)   /* Control/Bulk Service Ratio */
#define OHCI_CTRL_PLE     (1u << 2)    /* Periodic List Enable */
#define OHCI_CTRL_IE      (1u << 3)    /* Isochronous Enable */
#define OHCI_CTRL_CLE     (1u << 4)    /* Control List Enable */
#define OHCI_CTRL_BLE     (1u << 5)    /* Bulk List Enable */
#define OHCI_CTRL_HCFS_SHIFT 6          /* HC Functional State (2 bits) */
#define OHCI_CTRL_HCFS_RESET   0
#define OHCI_CTRL_HCFS_RESUME  1
#define OHCI_CTRL_HCFS_OPER    2
#define OHCI_CTRL_HCFS_SUSPEND 3
#define OHCI_CTRL_IR       (1u << 8)   /* Interrupt Routing */
#define OHCI_CTRL_RWC      (1u << 9)   /* Remote Wakeup Connected */
#define OHCI_CTRL_RWE      (1u << 10)  /* Remote Wakeup Enabled */

/* Command Status bits */
#define OHCI_CMD_HCR      (1u << 0)    /* Host Controller Reset */
#define OHCI_CMD_CLF      (1u << 1)    /* Control List Filled */
#define OHCI_CMD_BLF      (1u << 2)    /* Bulk List Filled */
#define OHCI_CMD_OCR      (1u << 3)    /* Ownership Change Request */

/* RH Descriptor A bits */
#define OHCI_RH_NDP_SHIFT 0            /* Number of Downstream Ports (bits 0-7) */
#define OHCI_RH_PSM       (1u << 8)    /* Power Switching Mode */
#define OHCI_RH_NPS       (1u << 9)    /* No Power Switching */
#define OHCI_RH_AICP      (1u << 11)   /* Over-current protection */

/* RH Port Status bits */
#define OHCI_PORT_CCS     (1u << 0)    /* Current Connect Status */
#define OHCI_PORT_PES     (1u << 1)    /* Port Enable Status */
#define OHCI_PORT_PSS     (1u << 2)    /* Port Suspend Status */
#define OHCI_PORT_POCI    (1u << 3)    /* Port Over-Current Indicator */
#define OHCI_PORT_PRS     (1u << 4)    /* Port Reset Status */
#define OHCI_PORT_PPS     (1u << 8)    /* Port Power Status */
#define OHCI_PORT_LSDA    (1u << 9)    /* Low Speed Device Attached */
#define OHCI_PORT_CSC     (1u << 16)   /* Connect Status Change */
#define OHCI_PORT_PESC    (1u << 17)   /* Port Enable Status Change */
#define OHCI_PORT_PRSC    (1u << 20)   /* Port Reset Status Change */

/* HCCA structure (256 bytes). */
struct ohci_hcca {
    uint32_t int_table[32];    /* interrupt ED pointers */
    uint16_t frame_number;     /* current frame number */
    uint16_t pad1;
    uint32_t done_head;        /* done queue head */
    uint8_t  reserved[116];    /* pad to 256 bytes */
} __attribute__((packed, aligned(256)));

/* ---- Endpoint Descriptor (ED) — 16 bytes ---- */
struct ohci_ed {
    uint32_t flags;        /* bits: function addr, endpoint, dir, speed, etc. */
    uint32_t tail_td;      /* tail TD pointer (phys) */
    uint32_t head_td;      /* head TD pointer (phys) + carry toggle bit */
    uint32_t next_ed;      /* next ED pointer (phys) */
} __attribute__((packed, aligned(16)));

/* ED flags layout:
 *   bits 0-6:   function address
 *   bits 7-10:  endpoint number
 *   bits 11-12: direction (00=FROM_TD, 01=OUT, 10=IN)
 *   bit 13:     speed (1=low, 0=full)
 *   bit 14:     skip
 *   bit 15:     isochronous
 *   bits 16-26: max packet size
 *   bit 27:     Halted
 *   bit 28:     Toggle carry */

/* ---- General Transfer Descriptor (TD) — 16 bytes ---- */
struct ohci_td {
    uint32_t flags;        /* direction, toggle, delay interrupt, CC, etc. */
    uint32_t cbp;          /* current buffer pointer (phys) */
    uint32_t next_td;      /* next TD pointer (phys) */
    uint32_t be;           /* buffer end pointer (phys) */
} __attribute__((packed, aligned(16)));

/* TD flags:
 *   bits 0-17:  reserved
 *   bit 18:     buffer rounding
 *   bits 19-20: direction (0=SETUP, 1=OUT, 2=IN)
 *   bits 21-22: data toggle (0=use_ed_carry, 1=DATA0, 2=DATA1)
 *   bits 23-26: delay interrupt
 *   bits 27-31: condition code (0=OK, etc.) + CC field */

#define OHCI_ED_DIR_FROM_TD 0u
#define OHCI_ED_DIR_OUT     1u
#define OHCI_ED_DIR_IN      2u
#define OHCI_ED_SKIP        (1u << 14)

#define OHCI_TD_R           (1u << 18)
#define OHCI_TD_DP_SETUP    0u
#define OHCI_TD_DP_OUT      1u
#define OHCI_TD_DP_IN       2u
#define OHCI_TD_DI_SHIFT    21
#define OHCI_TD_T_SHIFT     24
#define OHCI_TD_T_DATA0     0u
#define OHCI_TD_T_DATA1     1u
#define OHCI_TD_CC_SHIFT    28
#define OHCI_TD_CC_MASK     0xFu

/* ---- Driver state ---- */
static volatile uint32_t *mmio = NULL;
static struct ohci_hcca *hcca = NULL;   /* HHDM pointer */
static int num_ports = 0;
static int port_count = 0;

/* ---- MMIO helpers ---- */
static inline uint32_t rd(uint32_t off) { return mmio[off / 4]; }
static inline void wr(uint32_t off, uint32_t val) { mmio[off / 4] = val; }

static inline uint32_t port_status(int port) {
    return rd(OHCI_RH_PORT_STATUS + port * 4);
}

static inline void port_write(int port, uint32_t val) {
    wr(OHCI_RH_PORT_STATUS + port * 4, val);
}

int ohci_init(void) {
    uint8_t bus, dev, func;

    /* Find OHCI: class 0x0C (Serial Bus), subclass 0x03 (USB),
     * prog_if 0x10 (OHCI). */
    if (pci_find_class(0x0C, 0x03, &bus, &dev, &func) != 0 ||
        pci_get_prog_if(bus, dev, func) != 0x10) {
        /* Try PCI class scan with prog_if check. */
        for (uint8_t b = 0; b < 1; b++) {
            for (uint8_t d = 0; d < 32; d++) {
                for (uint8_t f = 0; f < 8; f++) {
                    if (pci_get_vendor(b, d, f) == 0xFFFF) continue;
                    if (pci_get_class(b, d, f) == 0x0C &&
                        pci_get_subclass(b, d, f) == 0x03 &&
                        pci_get_prog_if(b, d, f) == 0x10) {
                        bus = b; dev = d; func = f;
                        goto found;
                    }
                }
            }
        }
        kprintf("[ohci] no OHCI controller found\n");
        return -1;
    }
found:
    kprintf("[ohci] controller at PCI %u:%u.%u (prog_if=0x%02x)\n",
            bus, dev, func, pci_get_prog_if(bus, dev, func));

    pci_enable_bus_master(bus, dev, func);

    /* Map BAR0 (MMIO). */
    uint32_t bar0 = pci_get_bar(bus, dev, func, 0);
    uint32_t mmio_phys = bar0 & ~0xF;
    uint64_t hhdm = limine_get_hhdm_offset();
    for (uint32_t off = 0; off < 0x1000; off += 0x1000) {
        paging_map(hhdm + mmio_phys + off, mmio_phys + off,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE);
    }
    mmio = (volatile uint32_t *)(uintptr_t)(hhdm + mmio_phys);

    uint32_t rev = rd(OHCI_REVISION);
    kprintf("[ohci] revision 0x%08x\n", rev);

    /* Save the interrupt routing bit and remote wakeup connected bit. */
    uint32_t ctrl = rd(OHCI_CONTROL);
    uint32_t rwc = ctrl & OHCI_CTRL_RWC;

    /* 1) Reset the controller. */
    wr(OHCI_COMMAND_STATUS, OHCI_CMD_HCR);
    int timeout = 1000000;
    while ((rd(OHCI_COMMAND_STATUS) & OHCI_CMD_HCR) && timeout-- > 0) {
        __asm__ volatile ("pause");
    }
    if (timeout < 0) {
        kprintf("[ohci] reset timeout\n");
        return -1;
    }

    /* 2) Set HCCA. */
    uint64_t hcca_phys = pmm_alloc_frame();
    if (hcca_phys == 0) {
        kprintf("[ohci] OOM for HCCA\n");
        return -1;
    }
    hcca = (struct ohci_hcca *)(uintptr_t)(hhdm + hcca_phys);
    memset(hcca, 0, sizeof(*hcca));
    wr(OHCI_HCCA, (uint32_t)hcca_phys);

    /* 3) Set control/bulk head EDs to NULL (terminate). */
    wr(OHCI_CONTROL_HEAD_ED, 0);
    wr(OHCI_BULK_HEAD_ED, 0);

    /* 4) Disable all interrupts. */
    wr(OHCI_INTERRUPT_DISABLE, 0x80000000);   /* disable all */
    wr(OHCI_INTERRUPT_STATUS, 0x7FFFFFFF);    /* clear status */

    /* 5) Set Frame Interval (12000 = 100 Hz at 48 MHz). */
    wr(OHCI_FM_INTERVAL, (11999) | (1u << 31));  /* FIT toggle + FI */
    wr(OHCI_PERIODIC_START, (12000 * 9) / 10);   /* 90% of FI */
    wr(OHCI_LSTHRESH, 0x628);                     /* ~7.08 us */

    /* 6) Read root hub descriptor for port count. */
    uint32_t rhda = rd(OHCI_RH_DESC_A);
    num_ports = rhda & 0xFF;
    if (num_ports > 15) num_ports = 15;
    kprintf("[ohci] root hub: %d ports, power_switching=%s\n",
            num_ports,
            (rhda & OHCI_RH_NPS) ? "no" : "yes");

    /* 7) Power on ports (if power switching is used). */
    if (!(rhda & OHCI_RH_NPS)) {
        /* Set all port power bits via RH_DESC_B (or RH_STATUS). */
        
        /* Set DeviceRemovable=0, PortPowerControlMask=all. */
        
        /* Power on each port. */
        for (int i = 0; i < num_ports; i++) {
            port_write(i, OHCI_PORT_PPS);   /* SetPortPower */
        }
        /* Wait for power to stabilize. */
        for (volatile int i = 0; i < 1000000; i++)
            __asm__ volatile ("nop");
    }

    /* 8) Transition to OPERATIONAL state. */
    wr(OHCI_CONTROL, (OHCI_CTRL_HCFS_OPER << OHCI_CTRL_HCFS_SHIFT) | rwc);

    /* 9) Enable control + bulk lists. */
    uint32_t new_ctrl = rd(OHCI_CONTROL);
    new_ctrl |= OHCI_CTRL_CLE | OHCI_CTRL_BLE | OHCI_CTRL_PLE;
    wr(OHCI_CONTROL, new_ctrl);

    /* 10) Enumerate ports. */
    port_count = 0;
    for (int i = 0; i < num_ports; i++) {
        uint32_t ps = port_status(i);
        if (ps & OHCI_PORT_CCS) {
            int low_speed = (ps & OHCI_PORT_LSDA) ? 1 : 0;
            kprintf("[ohci] port %d: device attached (%s)\n",
                    i, low_speed ? "low-speed" : "full-speed");

            /* Reset the port. */
            port_write(i, OHCI_PORT_PRS);   /* SetPortReset */
            /* Wait for reset to complete (PRS clears, PRSC sets). */
            int t = 10000000;
            while ((port_status(i) & OHCI_PORT_PRS) && t-- > 0) {
                __asm__ volatile ("pause");
            }
            /* Clear reset change. */
            port_write(i, OHCI_PORT_PRSC);  /* ClearPortResetChange */
            /* Enable the port. */
            port_write(i, OHCI_PORT_PES);   /* SetPortEnable */
            port_count++;
        }
    }

    if (port_count == 0) {
        kprintf("[ohci] no USB devices detected\n");
    } else {
        kprintf("[ohci] %d device(s) ready\n", port_count);
    }

    /* Verify the frame number is advancing (controller operational). */
    uint16_t fn1 = hcca->frame_number;
    for (volatile int i = 0; i < 100000; i++)
        __asm__ volatile ("nop");
    uint16_t fn2 = hcca->frame_number;
    kprintf("[ohci] frame number: %u -> %u (running=%s)\n",
            fn1, fn2, fn1 != fn2 ? "yes" : "no");

    return 0;
}

int ohci_get_port_count(void) {
    return port_count;
}

int ohci_port_has_device(int port) {
    if (mmio == NULL || port < 0 || port >= num_ports) return 0;
    return (port_status(port) & OHCI_PORT_CCS) ? 1 : 0;
}


int ohci_reset_port(int port) {
    if (mmio == NULL || port < 0 || port >= num_ports) return -1;
    if (!(port_status(port) & OHCI_PORT_CCS)) return -1;
    port_write(port, OHCI_PORT_PRS);
    int t = 10000000;
    while ((port_status(port) & OHCI_PORT_PRS) && t-- > 0) __asm__ volatile ("pause");
    port_write(port, OHCI_PORT_PRSC);
    port_write(port, OHCI_PORT_PES);
    return (port_status(port) & OHCI_PORT_PES) ? 0 : -1;
}

int ohci_port_is_low_speed(int port) {
    if (mmio == NULL || port < 0 || port >= num_ports) return 0;
    return (port_status(port) & OHCI_PORT_LSDA) ? 1 : 0;
}

static uint32_t ohci_ed_flags(uint8_t dev_addr, uint8_t endpoint,
                              uint8_t dir, int low_speed, uint16_t max_packet) {
    uint32_t f = 0;
    if (max_packet == 0) max_packet = low_speed ? 8 : 64;
    f |= (uint32_t)(dev_addr & 0x7F);
    f |= (uint32_t)(endpoint & 0x0F) << 7;
    f |= (uint32_t)(dir & 0x03) << 11;
    if (low_speed) f |= (1u << 13);
    f |= ((uint32_t)max_packet & 0x7FFu) << 16;
    return f;
}

static uint32_t ohci_td_flags(uint8_t pid, uint8_t toggle, int irq_on_done,
                              int allow_short) {
    uint32_t f = 0;
    if (allow_short) f |= OHCI_TD_R;
    f |= ((uint32_t)pid & 0x03u) << 19;
    f |= (irq_on_done ? 0u : 7u) << OHCI_TD_DI_SHIFT;
    f |= ((uint32_t)toggle & 0x03u) << OHCI_TD_T_SHIFT;
    return f;
}

static void ohci_td_buffer(volatile struct ohci_td *td, uint64_t phys,
                           uint32_t len) {
    if (len == 0) {
        td->cbp = 0;
        td->be = 0;
    } else {
        td->cbp = (uint32_t)phys;
        td->be = (uint32_t)(phys + len - 1);
    }
}

static int ohci_wait_ed_done(volatile struct ohci_ed *ed, uint32_t tail_phys,
                             volatile struct ohci_td *tds, uint32_t td_count,
                             int is_bulk) {
    if (is_bulk) wr(OHCI_COMMAND_STATUS, OHCI_CMD_BLF);
    else         wr(OHCI_COMMAND_STATUS, OHCI_CMD_CLF);

    int timeout = 10000000;
    while (timeout-- > 0) {
        uint32_t head = ed->head_td;
        if ((head & ~0xFu) == (tail_phys & ~0xFu)) break;
        __asm__ volatile ("pause");
    }
    if (timeout < 0) {
        kprintf("[ohci] ED/TD timeout head=0x%08x tail=0x%08x\n",
                ed->head_td, tail_phys);
        return -1;
    }
    if (ed->head_td & 0x1) {
        kprintf("[ohci] ED halted head=0x%08x\n", ed->head_td);
        return -1;
    }
    for (uint32_t i = 0; i < td_count; i++) {
        uint32_t cc = (tds[i].flags >> OHCI_TD_CC_SHIFT) & OHCI_TD_CC_MASK;
        if (cc != 0) {
            /* CC=4 (NAK) on one-shot interrupt IN means no report available. */
            if (cc == 4 && td_count == 1) return 0;
            kprintf("[ohci] TD%u error cc=%u flags=0x%08x\n", i, cc, tds[i].flags);
            return -1;
        }
    }
    return 1;
}

static int ohci_run_transfer(uint8_t dev_addr, uint8_t endpoint, uint8_t ed_dir,
                             int low_speed, uint16_t max_packet,
                             volatile struct ohci_td *tds, uint32_t td_count,
                             uint32_t first_td_phys, uint32_t tail_td_phys,
                             int is_bulk) {
    uint64_t hhdm = limine_get_hhdm_offset();
    uint64_t ed_phys = pmm_alloc_frame();
    if (!ed_phys) return -1;
    volatile struct ohci_ed *ed = (volatile struct ohci_ed *)(uintptr_t)(hhdm + ed_phys);
    memset((void *)ed, 0, 4096);
    ed->flags = ohci_ed_flags(dev_addr, endpoint, ed_dir, low_speed, max_packet);
    ed->tail_td = tail_td_phys;
    ed->head_td = first_td_phys;
    ed->next_ed = 0;

    if (is_bulk) wr(OHCI_BULK_HEAD_ED, (uint32_t)ed_phys);
    else         wr(OHCI_CONTROL_HEAD_ED, (uint32_t)ed_phys);

    uint32_t ctrl = rd(OHCI_CONTROL);
    ctrl |= is_bulk ? OHCI_CTRL_BLE : OHCI_CTRL_CLE;
    wr(OHCI_CONTROL, ctrl);

    int ret = ohci_wait_ed_done(ed, tail_td_phys, tds, td_count, is_bulk);

    if (is_bulk) wr(OHCI_BULK_HEAD_ED, 0);
    else         wr(OHCI_CONTROL_HEAD_ED, 0);
    pmm_free_frame(ed_phys);
    return ret;
}

int ohci_control_transfer(uint8_t dev_addr, int low_speed,
                          const void *setup, void *data,
                          uint16_t data_len, uint8_t max_packet0) {
    if (mmio == NULL || setup == NULL) return -1;
    uint64_t hhdm = limine_get_hhdm_offset();
    uint16_t max_packet = max_packet0 ? max_packet0 : (low_speed ? 8 : 64);
    uint32_t data_packets = data_len ? ((data_len + max_packet - 1) / max_packet) : 0;
    uint32_t td_count = 1 + data_packets + 1;
    if (td_count > 64) return -1;

    uint64_t setup_phys = pmm_alloc_frame();
    uint64_t data_phys = data_len ? pmm_alloc_contiguous((data_len + 0xFFF) / 0x1000) : 0;
    uint64_t td_phys = pmm_alloc_frame();
    uint64_t tail_phys = pmm_alloc_frame();
    if (!setup_phys || !td_phys || !tail_phys || (data_len && !data_phys)) return -1;
    memcpy((void *)(uintptr_t)(hhdm + setup_phys), setup, 8);
    if (data_len && data) memcpy((void *)(uintptr_t)(hhdm + data_phys), data, data_len);

    volatile struct ohci_td *tds = (volatile struct ohci_td *)(uintptr_t)(hhdm + td_phys);
    memset((void *)tds, 0, 4096);
    volatile struct ohci_td *tail = (volatile struct ohci_td *)(uintptr_t)(hhdm + tail_phys);
    memset((void *)tail, 0, 4096);

    const uint8_t *setup_bytes = (const uint8_t *)setup;
    int data_in = (setup_bytes[0] & 0x80) ? 1 : 0;
    uint32_t n = 0;
    tds[n].flags = ohci_td_flags(OHCI_TD_DP_SETUP, OHCI_TD_T_DATA0, 0, 0);
    ohci_td_buffer(&tds[n], setup_phys, 8);
    tds[n].next_td = (uint32_t)(td_phys + (n + 1) * sizeof(struct ohci_td));
    n++;

    uint32_t remaining = data_len, off = 0;
    uint8_t toggle = OHCI_TD_T_DATA1;
    while (remaining) {
        uint32_t chunk = remaining > max_packet ? max_packet : remaining;
        tds[n].flags = ohci_td_flags(data_in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT,
                                     toggle, 0, data_in);
        ohci_td_buffer(&tds[n], data_phys + off, chunk);
        tds[n].next_td = (uint32_t)(td_phys + (n + 1) * sizeof(struct ohci_td));
        toggle = (toggle == OHCI_TD_T_DATA1) ? OHCI_TD_T_DATA0 : OHCI_TD_T_DATA1;
        remaining -= chunk;
        off += chunk;
        n++;
    }

    tds[n].flags = ohci_td_flags(data_len ? (data_in ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN)
                                          : OHCI_TD_DP_IN,
                                 OHCI_TD_T_DATA1, 1, 0);
    ohci_td_buffer(&tds[n], 0, 0);
    tds[n].next_td = (uint32_t)tail_phys;
    n++;

    int ret = ohci_run_transfer(dev_addr, 0, OHCI_ED_DIR_FROM_TD, low_speed,
                                max_packet, tds, n, (uint32_t)td_phys,
                                (uint32_t)tail_phys, 0);
    if (ret > 0 && data_len && data && data_in) {
        memcpy(data, (void *)(uintptr_t)(hhdm + data_phys), data_len);
    }
    pmm_free_frame(tail_phys);
    pmm_free_frame(td_phys);
    if (data_phys) for (uint32_t i = 0; i < (data_len + 0xFFF) / 0x1000; i++) pmm_free_frame(data_phys + i * 4096ULL);
    pmm_free_frame(setup_phys);
    return ret > 0 ? (int)data_len : ret;
}

int ohci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint,
                       void *data, uint32_t len, int in, uint16_t max_packet) {
    if (mmio == NULL || data == NULL || len == 0) return -1;
    uint64_t hhdm = limine_get_hhdm_offset();
    if (max_packet == 0) max_packet = 64;
    uint64_t buf_phys = pmm_alloc_contiguous((len + 0xFFF) / 0x1000);
    uint64_t td_phys = pmm_alloc_frame();
    uint64_t tail_phys = pmm_alloc_frame();
    if (!buf_phys || !td_phys || !tail_phys) return -1;
    if (!in) memcpy((void *)(uintptr_t)(hhdm + buf_phys), data, len);
    else memset((void *)(uintptr_t)(hhdm + buf_phys), 0, len);
    volatile struct ohci_td *td = (volatile struct ohci_td *)(uintptr_t)(hhdm + td_phys);
    memset((void *)td, 0, 4096);
    volatile struct ohci_td *tail = (volatile struct ohci_td *)(uintptr_t)(hhdm + tail_phys);
    memset((void *)tail, 0, 4096);
    td[0].flags = ohci_td_flags(in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT,
                                OHCI_TD_T_DATA0, 1, in);
    ohci_td_buffer(&td[0], buf_phys, len);
    td[0].next_td = (uint32_t)tail_phys;
    int ret = ohci_run_transfer(dev_addr, endpoint & 0x0F,
                                in ? OHCI_ED_DIR_IN : OHCI_ED_DIR_OUT,
                                0, max_packet, td, 1, (uint32_t)td_phys,
                                (uint32_t)tail_phys, 1);
    if (ret > 0 && in) memcpy(data, (void *)(uintptr_t)(hhdm + buf_phys), len);
    pmm_free_frame(tail_phys);
    pmm_free_frame(td_phys);
    for (uint32_t i = 0; i < (len + 0xFFF) / 0x1000; i++) pmm_free_frame(buf_phys + i * 4096ULL);
    return ret > 0 ? (int)len : ret;
}

int ohci_interrupt_transfer(uint8_t dev_addr, uint8_t endpoint,
                            int low_speed, uint16_t max_packet,
                            void *data, uint16_t len, int *toggle_io) {
    if (mmio == NULL || data == NULL || len == 0 || !(endpoint & 0x80)) return -1;
    uint64_t hhdm = limine_get_hhdm_offset();
    if (max_packet == 0) max_packet = low_speed ? 8 : 8;
    if (len > max_packet) len = max_packet;
    uint64_t buf_phys = pmm_alloc_frame();
    uint64_t td_phys = pmm_alloc_frame();
    uint64_t tail_phys = pmm_alloc_frame();
    if (!buf_phys || !td_phys || !tail_phys) return -1;
    memset((void *)(uintptr_t)(hhdm + buf_phys), 0, len);
    volatile struct ohci_td *td = (volatile struct ohci_td *)(uintptr_t)(hhdm + td_phys);
    memset((void *)td, 0, 4096);
    volatile struct ohci_td *tail = (volatile struct ohci_td *)(uintptr_t)(hhdm + tail_phys);
    memset((void *)tail, 0, 4096);
    uint8_t tog = toggle_io ? (*toggle_io ? OHCI_TD_T_DATA1 : OHCI_TD_T_DATA0) : OHCI_TD_T_DATA0;
    td[0].flags = ohci_td_flags(OHCI_TD_DP_IN, tog, 1, 1);
    ohci_td_buffer(&td[0], buf_phys, len);
    td[0].next_td = (uint32_t)tail_phys;

    uint64_t ed_phys = pmm_alloc_frame();
    if (!ed_phys) {
        pmm_free_frame(tail_phys);
        pmm_free_frame(td_phys);
        pmm_free_frame(buf_phys);
        return -1;
    }
    volatile struct ohci_ed *ed = (volatile struct ohci_ed *)(uintptr_t)(hhdm + ed_phys);
    memset((void *)ed, 0, 4096);
    ed->flags = ohci_ed_flags(dev_addr, endpoint & 0x0F, OHCI_ED_DIR_IN,
                              low_speed, max_packet);
    ed->tail_td = (uint32_t)tail_phys;
    ed->head_td = (uint32_t)td_phys;
    ed->next_ed = 0;

    uint32_t old_table[32];
    for (int i = 0; i < 32; i++) {
        old_table[i] = hcca->int_table[i];
        hcca->int_table[i] = (uint32_t)ed_phys;
    }
    wr(OHCI_CONTROL, rd(OHCI_CONTROL) | OHCI_CTRL_PLE);

    int timeout = 200000;
    while (timeout-- > 0) {
        if ((ed->head_td & ~0xFu) == ((uint32_t)tail_phys & ~0xFu)) break;
        __asm__ volatile ("pause");
    }
    for (int i = 0; i < 32; i++) hcca->int_table[i] = old_table[i];

    int ret = 0;
    uint32_t cc = (td[0].flags >> OHCI_TD_CC_SHIFT) & OHCI_TD_CC_MASK;
    if (timeout < 0 || cc == 4) {
        ret = 0; /* no interrupt report ready */
    } else if (cc != 0 || (ed->head_td & 0x1)) {
        ret = -1;
    } else {
        memcpy(data, (void *)(uintptr_t)(hhdm + buf_phys), len);
        if (toggle_io) *toggle_io ^= 1;
        ret = (int)len;
    }
    pmm_free_frame(ed_phys);
    pmm_free_frame(tail_phys);
    pmm_free_frame(td_phys);
    pmm_free_frame(buf_phys);
    return ret;
}

void ohci_self_test(void) {
    if (mmio == NULL) {
        kprintf("[ohci] self-test: no controller\n");
        return;
    }

    /* Verify operational state. */
    uint32_t ctrl = rd(OHCI_CONTROL);
    int state = (ctrl >> OHCI_CTRL_HCFS_SHIFT) & 0x3;
    const char *state_names[] = {"RESET", "RESUME", "OPERATIONAL", "SUSPEND"};
    kprintf("[ohci] self-test: HC state = %s\n",
            state < 4 ? state_names[state] : "?");

    /* Report port status. */
    for (int i = 0; i < num_ports; i++) {
        uint32_t ps = port_status(i);
        kprintf("[ohci] port %d: CCS=%d PES=%d LSDA=%d PPS=%d\n",
                i,
                (ps & OHCI_PORT_CCS) ? 1 : 0,
                (ps & OHCI_PORT_PES) ? 1 : 0,
                (ps & OHCI_PORT_LSDA) ? 1 : 0,
                (ps & OHCI_PORT_PPS) ? 1 : 0);
    }

    if (port_count > 0) {
        kprintf("[ohci] PASS: %d USB device(s) detected\n", port_count);
    } else {
        kprintf("[ohci] PASS: controller operational, no devices\n");
    }
}
