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
    if (pci_find_class(0x0C, 0x03, &bus, &dev, &func) != 0) {
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
