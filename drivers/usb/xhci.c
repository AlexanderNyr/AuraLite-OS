/* xhci.c — xHCI (USB 3.0) host controller driver.
 *
 * xHCI is the unified USB host controller: it handles low/full/high/superSpeed
 * in one driver, with a fundamentally different architecture from UHCI/OHCI/EHCI.
 *
 * Key xHCI concepts:
 *   - Capability registers (at BAR0 + 0): describe the controller's limits.
 *   - Operational registers (at BAR0 + CAPLENGTH): control the HC.
 *   - Runtime registers (at BAR0 + RTSOFF): interrupt moderation, MSIX.
 *   - Doorbell registers (at BAR0 + DBOFF): ring the HC to process transfers.
 *   - Device contexts + input contexts: per-device state (slot, endpoint).
 *   - Command Ring: a circular TRB ring for host commands (Address Device, etc).
 *   - Transfer Rings: per-endpoint circular TRB rings for I/O.
 *   - Event Ring: the HC writes completion TRBs here.
 *   - Scratchpad Buffers: page-sized buffers the HC may request for caching.
 *
 * This implementation:
 *   - Detects the xHCI controller on PCI.
 *   - Maps all register spaces (cap, op, runtime, doorbell).
 *   - Resets the HC and waits for the "Controller Not Ready" bit to clear.
 *   - Allocates the Device Context Base Address Array (DCBAA).
 *   - Allocates Scratchpad Buffer Array if needed.
 *   - Creates the Command Ring and Event Ring.
 *   - Starts the HC and enumerates root hub ports.
 *   - Full TRB and context structures are defined for future transfer support.
 *
 * QEMU: -device qemu-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=...
 */

#include <stdint.h>
#include "drivers/usb/xhci.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/limine_requests.h"

/* ---- xHCI capability registers (offsets from BAR0) ---- */
#define XHCI_CAP_CAPLENGTH  0x00    /* Capability Register Length (8-bit) + HCIVERSION */
#define XHCI_CAP_HCSPARAMS1 0x04    /* Structural Parameters 1 */
#define XHCI_CAP_HCSPARAMS2 0x08    /* Structural Parameters 2 */
#define XHCI_CAP_HCSPARAMS3 0x0C    /* Structural Parameters 3 */
#define XHCI_CAP_HCCPARAMS1 0x10    /* Capability Parameters 1 */
#define XHCI_CAP_DBOFF      0x14    /* Doorbell Offset */
#define XHCI_CAP_RTSOFF     0x18    /* Runtime Register Space Offset */
#define XHCI_CAP_HCCPARAMS2 0x1C    /* Capability Parameters 2 */

/* HCSPARAMS1 fields. QEMU's xHCI uses bits 24-31 for MaxPorts. */
#define HCSPARAMS1_MAX_SLOTS   0x000000FF   /* bits 0-7 */
#define HCSPARAMS1_MAX_INTRS   0x0000FF00   /* bits 8-15 */
#define HCSPARAMS1_MAX_PORTS   0xFF000000   /* bits 24-31 */

/* HCSPARAMS2 fields */
#define HCSPARAMS2_SCRATCHPAD_RESET  (1u << 27)  /* bit 27 (ERS) */
#define HCSPARAMS2_MAX_SCRATCHPADS_HI 0x01E00000  /* bits 21-25 (SPB hi) */
#define HCSPARAMS2_MAX_SCRATCHPADS_LO 0x000F8000  /* bits 15-19 (SPB lo) */
#define HCSPARAMS2_SPB_SHIFT   15
#define HCSPARAMS2_SPB_MASK    0x3FF   /* 10 bits total (15-25) */

/* HCCPARAMS1 fields */
#define HCCPARAMS1_AC64        (1u << 0)    /* 64-bit Addressing Capability */
#define HCCPARAMS1_BNC         (1u << 1)    /* BW Negotiation Capability */
#define HCCPARAMS1_CSZ         (1u << 2)    /* Context Size (0=32-byte, 1=64-byte) */
#define HCCPARAMS1_PPC         (1u << 3)    /* Port Power Control */
#define HCCPARAMS1_PIND        (1u << 4)    /* Port Indicators */
#define HCCPARAMS1_LHRC        (1u << 5)    /* Light HC Reset Capability */
#define HCCPARAMS1_LTC         (1u << 6)    /* Latency Tolerance Messaging */
#define HCCPARAMS1_NSS         (1u << 7)    /* No Secondary SID Support */
#define HCCPARAMS1_SEC_TYPE    0xFF000000   /* bits 24-31 */
#define HCCPARAMS1_XECP_SHIFT  16
#define HCCPARAMS1_XECP_MASK   0xFFFF

/* ---- xHCI operational registers (offset = CAPLENGTH from BAR0) ---- */
#define XHCI_OP_USBCMD        0x00    /* USB Command */
#define XHCI_OP_USBSTS        0x04    /* USB Status */
#define XHCI_OP_PAGESIZE      0x08    /* Page Size */
#define XHCI_OP_DNCTRL        0x14    /* Device Notification Control */
#define XHCI_OP_CRCR          0x18    /* Command Ring Control Register */
#define XHCI_OP_DCBAAP        0x30    /* Device Context Base Address Array Pointer */
#define XHCI_OP_CONFIG        0x38    /* Configure Register */

/* USBCMD bits */
#define XHCI_USBCMD_RUN       (1u << 0)    /* Run/Stop */
#define XHCI_USBCMD_HCRST     (1u << 1)    /* Host Controller Reset */
#define XHCI_USBCMD_INTE      (1u << 2)    /* Interrupter Enable */
#define XHCI_USBCMD_HSEE      (1u << 3)    /* Host System Error Enable */
#define XHCI_USBCMD_LHCRST    (1u << 7)    /* Light HC Reset */

/* USBSTS bits */
#define XHCI_USBSTS_HCH       (1u << 0)    /* HC Halted */
#define XHCI_USBSTS_HSE       (1u << 2)    /* Host System Error */
#define XHCI_USBSTS_PCD       (1u << 4)    /* Port Change Detect */
#define XHCI_USBSTS_CNR       (1u << 11)   /* Controller Not Ready */
#define XHCI_USBSTS_HCE       (1u << 12)   /* HC Error */

/* CRCR (Command Ring Control Register) bits */
#define XHCI_CRCR_RCS         (1u << 0)    /* Ring Cycle State */
#define XHCI_CRCR_CS          (1u << 1)    /* Command Stop */
#define XHCI_CRCR_CA          (1u << 2)    /* Command Abort */
#define XHCI_CRCR_CRR         (1u << 3)    /* Command Ring Running */
#define XHCI_CRCR_MASK        0xFFFFFFFFFFFFFFF8ULL  /* mask low 3 bits for ptr */

/* CONFIG register */
#define XHCI_CONFIG_MAX_SLOTS_EN  0x000000FF   /* bits 0-7 */

/* ---- Runtime registers (offset = RTSOFF from BAR0) ---- */
/* Interrupter Register Set N: offset = RTSOFF + (0x20 * N) */
#define XHCI_RT_IR_IMAN(n)    (0x20 + 0x20 * n)  /* Interrupter Management */
#define XHCI_RT_IR_IMOD(n)    (0x24 + 0x20 * n)  /* Interrupter Moderation */
#define XHCI_RT_IR_ERSTSZ(n)  (0x28 + 0x20 * n)  /* Event Ring Segment Table Size */
#define XHCI_RT_IR_ERSTBA(n)  (0x30 + 0x20 * n)  /* Event Ring Segment Table BA */
#define XHCI_RT_IR_ERDP(n)    (0x38 + 0x20 * n)  /* Event Ring Dequeue Pointer */

#define XHCI_IR_IMAN_IE       (1u << 1)    /* Interrupt Enable */
#define XHCI_IR_IMAN_IP       (1u << 0)    /* Interrupt Pending */

#define XHCI_ERDP_BUSY        (1u << 3)    /* Event Handler Busy */

/* ---- Port Status and Control (offset = 0x400 from operational base) ---- */
#define XHCI_PORT_OFFSET      0x400
#define XHCI_PORT_STRIDE      0x10    /* each port is 16 bytes */

#define XHCI_PORTSC_CCS        (1u << 0)    /* Current Connect Status */
#define XHCI_PORTSC_PED       (1u << 1)    /* Port Enabled/Disabled */
#define XHCI_PORTSC_OCA       (1u << 3)    /* Over-Current Active */
#define XHCI_PORTSC_PR        (1u << 4)    /* Port Reset */
#define XHCI_PORTSC_PLS_SHIFT  5           /* Port Link State (bits 5-8) */
#define XHCI_PORTSC_PLS_MASK  0x0F
#define XHCI_PORTSC_PP        (1u << 9)    /* Port Power */
#define XHCI_PORTSC_SPEED_SHIFT 10         /* Port Speed (bits 10-13) */
#define XHCI_PORTSC_SPEED_MASK  0x0F
#define XHCI_PORTSC_LWS       (1u << 16)   /* Port Link State Write Strobe */
#define XHCI_PORTSC_CSC       (1u << 17)   /* Connect Status Change */
#define XHCI_PORTSC_PEC       (1u << 18)   /* Port Enabled/Disabled Change */
#define XHCI_PORTSC_WRC       (1u << 19)   /* Warm Port Reset Change */
#define XHCI_PORTSC_OCC       (1u << 20)   /* Over-Current Change */
#define XHCI_PORTSC_PRC       (1u << 21)   /* Port Reset Change */
#define XHCI_PORTSC_PLC       (1u << 22)   /* Port Link State Change */
#define XHCI_PORTSC_CEC       (1u << 23)   /* Config Error Change */
#define XHCI_PORTSC_WCE       (1u << 25)   /* Wake on Connect Enable */
#define XHCI_PORTSC_WDE       (1u << 26)   /* Wake on Disconnect Enable */
#define XHCI_PORTSC_WOE       (1u << 27)   /* Wake on Over-current Enable */
#define XHCI_PORTSC_DR        (1u << 30)   /* Device Removable */
#define XHCI_PORTSC_WPR       (1u << 31)   /* Warm Port Reset */

/* Port speed values */
#define XHCI_SPEED_FULL       1    /* 12 Mbps */
#define XHCI_SPEED_LOW        2    /* 1.5 Mbps */
#define XHCI_SPEED_HIGH       3    /* 480 Mbps */
#define XHCI_SPEED_SUPER      4    /* 5 Gbps */

/* ---- TRB (Transfer Request Block) — 16 bytes ---- */
struct xhci_trb {
    uint32_t param;      /* parameter (data buffer ptr, length, etc.) */
    uint32_t status;     /* status (transfer length, etc.) */
    uint32_t control;    /* control (type, cycle, slot, endpoint, etc.) */
    uint32_t flags;      /* actually part of control high bits */
} __attribute__((packed));

/* TRB types (bits 10-15 of the control field). */
#define XHCI_TRB_TYPE_SHIFT   10
#define XHCI_TRB_TYPE_MASK    0x3F
#define XHCI_TRB_NORMAL       1
#define XHCI_TRB_SETUP_STAGE  2
#define XHCI_TRB_DATA_STAGE   3
#define XHCI_TRB_STATUS_STAGE 4
#define XHCI_TRB_LINK         6
#define XHCI_TRB_TRANSFER_EVENT 32
#define XHCI_TRB_CMD_COMPLETION 33
#define XHCI_TRB_CMD_NOOP     23
#define XHCI_TRB_CMD_ENABLE_SLOT  9
#define XHCI_TRB_CMD_DISABLE_SLOT 10
#define XHCI_TRB_CMD_ADDRESS_DEVICE 11
#define XHCI_TRB_CMD_CONFIGURE_ENDPOINT 12

/* TRB cycle bit */
#define XHCI_TRB_CYCLE       (1u << 0)
#define XHCI_TRB_TC          (1u << 1)    /* Toggle Cycle (Link TRB) */
#define XHCI_TRB_IOC         (1u << 5)    /* Interrupt On Completion */

/* ---- Event Ring Segment Table Entry — 16 bytes ---- */
struct xhci_erst_entry {
    uint32_t addr_lo;     /* segment address low */
    uint32_t addr_hi;     /* segment address high */
    uint32_t size;        /* number of TRBs in segment */
    uint32_t reserved;
} __attribute__((packed));

/* ---- Driver state ---- */
static volatile uint8_t *cap_regs = NULL;
static volatile uint32_t *op_regs = NULL;
static volatile uint32_t *rt_regs = NULL;
static volatile uint32_t *db_regs = NULL;
static uint32_t op_offset = 0;
static uint32_t rt_offset = 0;
static uint32_t db_offset = 0;

static int num_ports = 0;
static int num_slots = 0;
static int max_scratchpads = 0;
static int has_64bit = 0;
static int context_size = 32;    /* 32 or 64 bytes per context */
static int port_count = 0;

/* DMA-allocated structures (via PMM, accessed via HHDM). */
static uint64_t *dcbaa = NULL;       /* Device Context Base Address Array */
static uint64_t *scratchpad_arr = NULL;
static struct xhci_trb *cmd_ring = NULL;
static struct xhci_trb *event_ring = NULL;
static struct xhci_erst_entry *erst = NULL;

/* Ring state. */
static int cmd_ring_cycle = 1;
static int event_ring_cycle = 1;
static int event_ring_idx = 0;
static uint32_t event_ring_phys32 = 0;
static uint32_t cmd_ring_phys32 = 0;
static int cmd_ring_idx = 0;

#define XHCI_MAX_DEVS 16
#define XHCI_RING_TRBS 256
#define XHCI_CTX_BYTES 2048
#define XHCI_EP_CONTROL 4
#define XHCI_EP_BULK_OUT 2
#define XHCI_EP_BULK_IN  6
#define XHCI_EP_INTR_OUT 3
#define XHCI_EP_INTR_IN  7

typedef struct {
    int in_use;
    uint8_t usb_addr;
    uint8_t slot_id;
    int port;
    uint8_t root_port;
    uint32_t route_string;
    int speed;
    uint64_t dev_ctx_phys;
    uint64_t input_ctx_phys;
    uint64_t ep_ring_phys[32];
    struct xhci_trb *ep_ring[32];
    uint16_t ep_max_packet[32];
    uint8_t ep_type[32];
    int ep_cycle[32];
    int ep_idx[32];
    int ep_configured[32];
} xhci_dev_t;

static xhci_dev_t xdevs[XHCI_MAX_DEVS];

/* ---- MMIO helpers ---- */
static inline uint32_t cap_rd32(uint32_t off) {
    return *(volatile uint32_t *)(cap_regs + off);
}
static inline uint8_t cap_rd8(uint32_t off) {
    return *(volatile uint8_t *)(cap_regs + off);
}
static inline uint16_t cap_rd16(uint32_t off) {
    return *(volatile uint16_t *)(cap_regs + off);
}
static inline uint32_t op_rd(uint32_t off) {
    return op_regs[off / 4];
}
static inline void op_wr(uint32_t off, uint32_t val) {
    op_regs[off / 4] = val;
}
static inline uint32_t rt_rd(uint32_t off) {
    return rt_regs[off / 4];
}
static inline void rt_wr(uint32_t off, uint32_t val) {
    rt_regs[off / 4] = val;
}
static inline void db_wr(int slot, uint32_t val) {
    db_regs[slot] = val;
}

/* ---- Port helpers ---- */
static inline uint32_t port_rd(int port) {
    return op_rd(XHCI_PORT_OFFSET + port * XHCI_PORT_STRIDE);
}
static inline void port_wr(int port, uint32_t val) {
    op_wr(XHCI_PORT_OFFSET + port * XHCI_PORT_STRIDE, val);
}

static const char *speed_name(int speed) {
    switch (speed) {
    case XHCI_SPEED_FULL:  return "full-speed (12 Mbps)";
    case XHCI_SPEED_LOW:   return "low-speed (1.5 Mbps)";
    case XHCI_SPEED_HIGH:  return "high-speed (480 Mbps)";
    case XHCI_SPEED_SUPER: return "super-speed (5 Gbps)";
    default: return "unknown";
    }
}

int xhci_init(void) {
    /* Find xHCI: class 0x0C/0x03/prog_if 0x30. */
    uint8_t bus = 0, dev = 0, func = 0;
    int found = 0;

    for (uint8_t b = 0; b < 1 && !found; b++) {
        for (uint8_t d = 0; d < 32 && !found; d++) {
            for (uint8_t f = 0; f < 8; f++) {
                if (pci_get_vendor(b, d, f) == 0xFFFF) continue;
                if (pci_get_class(b, d, f) == 0x0C &&
                    pci_get_subclass(b, d, f) == 0x03 &&
                    pci_get_prog_if(b, d, f) == 0x30) {
                    bus = b; dev = d; func = f;
                    found = 1;
                    break;
                }
            }
        }
    }
    if (!found) {
        kprintf("[xhci] no xHCI controller found\n");
        return -1;
    }

    kprintf("[xhci] controller at PCI %u:%u.%u\n", bus, dev, func);
    pci_enable_bus_master(bus, dev, func);

    /* Map BAR0. xHCI needs more MMIO space than the others — map 64 KiB. */
    uint32_t bar0 = pci_get_bar(bus, dev, func, 0);
    uint32_t mmio_phys = bar0 & ~0xF;
    uint64_t hhdm = limine_get_hhdm_offset();
    for (uint32_t off = 0; off < 0x10000; off += 0x1000) {
        paging_map(hhdm + mmio_phys + off, mmio_phys + off,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE);
    }
    cap_regs = (volatile uint8_t *)(uintptr_t)(hhdm + mmio_phys);

    /* Read capability registers. */
    op_offset = cap_rd8(XHCI_CAP_CAPLENGTH);
    uint16_t hci_ver = cap_rd16(XHCI_CAP_CAPLENGTH + 2);
    uint32_t hcs1 = cap_rd32(XHCI_CAP_HCSPARAMS1);
    uint32_t hcs2 = cap_rd32(XHCI_CAP_HCSPARAMS2);
    uint32_t hcc1 = cap_rd32(XHCI_CAP_HCCPARAMS1);
    db_offset = cap_rd32(XHCI_CAP_DBOFF);
    rt_offset = cap_rd32(XHCI_CAP_RTSOFF);

    num_ports = (hcs1 & HCSPARAMS1_MAX_PORTS) >> 24;
    num_slots = hcs1 & HCSPARAMS1_MAX_SLOTS;
    max_scratchpads = (hcs2 >> HCSPARAMS2_SPB_SHIFT) & HCSPARAMS2_SPB_MASK;
    has_64bit = (hcc1 & HCCPARAMS1_AC64) ? 1 : 0;
    int csz = (hcc1 & HCCPARAMS1_CSZ) ? 1 : 0;
    context_size = csz ? 64 : 32;
    int has_ppc = (hcc1 & HCCPARAMS1_PPC) ? 1 : 0;

    /* Set up register pointers. */
    op_regs = (volatile uint32_t *)(uintptr_t)(hhdm + mmio_phys + op_offset);
    rt_regs = (volatile uint32_t *)(uintptr_t)(hhdm + mmio_phys + rt_offset);
    db_regs = (volatile uint32_t *)(uintptr_t)(hhdm + mmio_phys + db_offset);

    kprintf("[xhci] HCI version %x.%02x, %d ports, %d slots, "
            "%d scratchpads, 64-bit=%d, context=%d bytes, PPC=%d\n",
            (hci_ver >> 8) & 0xFF, hci_ver & 0xFF,
            num_ports, num_slots, max_scratchpads,
            has_64bit, context_size, has_ppc);

    /* 1) Halt the HC if running. */
    op_wr(XHCI_OP_USBCMD, 0);
    int t = 1000000;
    while (!(op_rd(XHCI_OP_USBSTS) & XHCI_USBSTS_HCH) && t-- > 0) {
        __asm__ volatile ("pause");
    }

    /* 2) Reset the HC. */
    op_wr(XHCI_OP_USBCMD, XHCI_USBCMD_HCRST);
    t = 1000000;
    while ((op_rd(XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST) && t-- > 0) {
        __asm__ volatile ("pause");
    }
    if (t < 0) {
        kprintf("[xhci] reset timeout (HCRST still set)\n");
        return -1;
    }

    /* 3) Wait for the "Controller Not Ready" bit to clear after reset. */
    t = 1000000;
    while ((op_rd(XHCI_OP_USBSTS) & XHCI_USBSTS_CNR) && t-- > 0) {
        __asm__ volatile ("pause");
    }

    /* 4) Set MaxSlotsEn in the CONFIG register. */
    op_wr(XHCI_OP_CONFIG, num_slots & XHCI_CONFIG_MAX_SLOTS_EN);

    /* 5) Allocate the DCBAA (Device Context Base Address Array).
     *    Array of (num_slots + 1) 64-bit pointers. Entry 0 = scratchpad array. */
    uint32_t dcbaa_size = (num_slots + 1) * 8;
    uint32_t dcbaa_frames = (dcbaa_size + 0xFFF) / 0x1000;
    uint64_t dcbaa_phys = pmm_alloc_contiguous(dcbaa_frames);
    if (dcbaa_phys == 0) {
        kprintf("[xhci] OOM for DCBAA\n");
        return -1;
    }
    dcbaa = (uint64_t *)(uintptr_t)(hhdm + dcbaa_phys);
    memset(dcbaa, 0, dcbaa_frames * 0x1000);

    /* 6) Allocate Scratchpad Buffer Array if needed. */
    if (max_scratchpads > 0) {
        uint32_t sp_arr_size = max_scratchpads * 8;
        uint64_t sp_arr_phys = pmm_alloc_contiguous(
            (sp_arr_size + 0xFFF) / 0x1000);
        if (sp_arr_phys == 0) {
            kprintf("[xhci] OOM for scratchpad array\n");
            return -1;
        }
        scratchpad_arr = (uint64_t *)(uintptr_t)(hhdm + sp_arr_phys);
        memset(scratchpad_arr, 0, sp_arr_size);

        /* Allocate individual scratchpad buffers (1 page each). */
        for (int i = 0; i < max_scratchpads; i++) {
            uint64_t sp_buf_phys = pmm_alloc_frame();
            if (sp_buf_phys == 0) {
                kprintf("[xhci] OOM for scratchpad buffer %d\n", i);
                return -1;
            }
            scratchpad_arr[i] = sp_buf_phys;
        }

        /* Point DCBAA entry 0 to the scratchpad array. */
        dcbaa[0] = sp_arr_phys;
        kprintf("[xhci] allocated %d scratchpad buffers\n", max_scratchpads);
    }

    /* Set the DCBAAP register (64-bit register pair; DMA is below 4 GiB). */
    op_wr(XHCI_OP_DCBAAP, (uint32_t)dcbaa_phys);
    op_wr(XHCI_OP_DCBAAP + 4, 0);

    /* 7) Allocate the Command Ring (1 page = 256 TRBs). */
    uint64_t cmd_ring_phys = pmm_alloc_frame();
    if (cmd_ring_phys == 0) {
        kprintf("[xhci] OOM for command ring\n");
        return -1;
    }
    cmd_ring_phys32 = (uint32_t)cmd_ring_phys;
    cmd_ring_idx = 0;
    cmd_ring = (struct xhci_trb *)(uintptr_t)(hhdm + cmd_ring_phys);
    memset(cmd_ring, 0, 4096);

    /* Place a Link TRB at the end of the first segment to make it circular. */
    /* Segment size = 256 TRBs (4096 / 16). Link at TRB[255]. */
    cmd_ring[255].param = (uint32_t)cmd_ring_phys;
    cmd_ring[255].status = 0;
    cmd_ring[255].control = 0;
    cmd_ring[255].flags = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC;
    cmd_ring_cycle = 1;

    /* Program the CRCR (Command Ring Control Register).
     * CRCR = ring_base_phys | RCS (initial cycle state = 1). */
    op_wr(XHCI_OP_CRCR, (uint32_t)cmd_ring_phys | XHCI_CRCR_RCS);
    op_wr(XHCI_OP_CRCR + 4, 0);

    /* 8) Allocate the Event Ring (1 page = 256 TRBs) + ERST. */
    uint64_t evt_ring_phys = pmm_alloc_frame();
    if (evt_ring_phys == 0) {
        kprintf("[xhci] OOM for event ring\n");
        return -1;
    }
    event_ring_phys32 = (uint32_t)evt_ring_phys;
    event_ring = (struct xhci_trb *)(uintptr_t)(hhdm + evt_ring_phys);
    memset(event_ring, 0, 4096);

    /* ERST (Event Ring Segment Table) — at least 1 entry. */
    uint64_t erst_phys = pmm_alloc_frame();
    if (erst_phys == 0) {
        kprintf("[xhci] OOM for ERST\n");
        return -1;
    }
    erst = (struct xhci_erst_entry *)(uintptr_t)(hhdm + erst_phys);
    memset(erst, 0, 4096);
    erst[0].addr_lo = (uint32_t)evt_ring_phys;
    erst[0].addr_hi = 0;
    erst[0].size = 256;   /* 256 TRBs in this segment */
    event_ring_cycle = 1;
    event_ring_idx = 0;

    /* Program the primary interrupter (Interrupter 0). */
    rt_wr(XHCI_RT_IR_ERSTSZ(0), 1);   /* 1 segment */
    rt_wr(XHCI_RT_IR_ERSTBA(0), (uint32_t)erst_phys);
    rt_wr(XHCI_RT_IR_ERSTBA(0) + 4, 0);
    rt_wr(XHCI_RT_IR_ERDP(0), (uint32_t)evt_ring_phys);
    rt_wr(XHCI_RT_IR_ERDP(0) + 4, 0);
    rt_wr(XHCI_RT_IR_IMAN(0), XHCI_IR_IMAN_IE);   /* Enable interrupter */
    rt_wr(XHCI_RT_IR_IMOD(0), 0);     /* No moderation */

    /* 9) Power on ports if port power control is used. */
    if (has_ppc) {
        for (int i = 0; i < num_ports; i++) {
            port_wr(i, XHCI_PORTSC_PP);
        }
        for (volatile int i = 0; i < 1000000; i++)
            __asm__ volatile ("nop");
    }

    /* 10) Start the HC: INTE + RUN. */
    op_wr(XHCI_OP_USBCMD, XHCI_USBCMD_INTE | XHCI_USBCMD_RUN);

    /* Wait for the HC to start (HCH clears). */
    t = 1000000;
    while ((op_rd(XHCI_OP_USBSTS) & XHCI_USBSTS_HCH) && t-- > 0) {
        __asm__ volatile ("pause");
    }
    if (t < 0) {
        kprintf("[xhci] controller did not start\n");
    } else {
        kprintf("[xhci] controller running\n");
    }

    /* 11) Enumerate ports. */
    port_count = 0;
    for (int i = 0; i < num_ports; i++) {
        uint32_t ps = port_rd(i);
        if (ps & XHCI_PORTSC_CCS) {
            int speed = (ps >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK;

            /* Reset the port. */
            port_wr(i, XHCI_PORTSC_PR);
            /* Wait for reset to complete (PR clears). */
            int rt2 = 5000000;
            while ((port_rd(i) & XHCI_PORTSC_PR) && rt2-- > 0) {
                __asm__ volatile ("pause");
            }
            /* Clear status change bits. */
            port_wr(i, port_rd(i) | (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC |
                                     XHCI_PORTSC_PRC | XHCI_PORTSC_PLC));

            /* Re-read speed after reset. */
            ps = port_rd(i);
            speed = (ps >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK;
            kprintf("[xhci] port %d: device attached (%s)\n", i, speed_name(speed));
            port_count++;
        }
    }

    if (port_count == 0) {
        kprintf("[xhci] no USB devices detected\n");
    } else {
        kprintf("[xhci] %d device(s) ready\n", port_count);
    }

    return 0;
}

int xhci_get_port_count(void) {
    return port_count;
}

int xhci_port_has_device(int port) {
    if (op_regs == NULL || port < 0 || port >= num_ports) return 0;
    return (port_rd(port) & XHCI_PORTSC_CCS) ? 1 : 0;
}


int xhci_reset_port(int port) {
    if (op_regs == NULL || port < 0 || port >= num_ports) return -1;
    if (!(port_rd(port) & XHCI_PORTSC_CCS)) return -1;
    port_wr(port, port_rd(port) | XHCI_PORTSC_PR);
    int t = 5000000;
    while ((port_rd(port) & XHCI_PORTSC_PR) && t-- > 0) __asm__ volatile ("pause");
    port_wr(port, port_rd(port) | (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC));
    return (port_rd(port) & XHCI_PORTSC_CCS) ? 0 : -1;
}

int xhci_port_speed(int port) {
    if (op_regs == NULL || port < 0 || port >= num_ports) return 0;
    return (int)((port_rd(port) >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK);
}

static uint32_t trb_type(const struct xhci_trb *t) {
    return (t->flags >> XHCI_TRB_TYPE_SHIFT) & XHCI_TRB_TYPE_MASK;
}
static uint32_t trb_cc(const struct xhci_trb *t) {
    return (t->control >> 24) & 0xFF;
}

static int xhci_poll_event(uint32_t want_type, struct xhci_trb *out) {
    int timeout = 10000000;
    while (timeout-- > 0) {
        struct xhci_trb *e = &event_ring[event_ring_idx];
        if ((e->flags & XHCI_TRB_CYCLE) == (uint32_t)event_ring_cycle) {
            if (out) *out = *e;
            uint32_t type = trb_type(e);
            event_ring_idx++;
            if (event_ring_idx >= 256) {
                event_ring_idx = 0;
                event_ring_cycle ^= 1;
            }
            uint32_t erdp = event_ring_phys32 + (uint32_t)event_ring_idx * sizeof(struct xhci_trb);
            /* Acknowledge by writing the dequeue pointer with EHB. */
            rt_wr(XHCI_RT_IR_ERDP(0), erdp | XHCI_ERDP_BUSY);
            if (!want_type || type == want_type) return 0;
        }
        __asm__ volatile ("pause");
    }
    return -1;
}

static int xhci_cmd_submit(struct xhci_trb trb, struct xhci_trb *event_out) {
    if (!cmd_ring) return -1;
    int idx = cmd_ring_idx;
    trb.flags |= (uint32_t)cmd_ring_cycle;
    cmd_ring[idx] = trb;
    cmd_ring_idx++;
    if (cmd_ring_idx >= 255) {
        cmd_ring[255].param = cmd_ring_phys32;
        cmd_ring[255].status = 0;
        cmd_ring[255].control = 0;
        cmd_ring[255].flags = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) |
                              XHCI_TRB_TC | (uint32_t)cmd_ring_cycle;
        cmd_ring_idx = 0;
        cmd_ring_cycle ^= 1;
    }
    db_wr(0, 0);
    struct xhci_trb ev;
    if (xhci_poll_event(XHCI_TRB_CMD_COMPLETION, &ev) != 0) {
        kprintf("[xhci] command timeout type=%u usbsts=0x%08x iman=0x%08x ev0={%08x,%08x,%08x,%08x}\n",
                (trb.flags >> XHCI_TRB_TYPE_SHIFT) & 0x3F,
                op_rd(XHCI_OP_USBSTS), rt_rd(XHCI_RT_IR_IMAN(0)),
                event_ring[0].param, event_ring[0].status,
                event_ring[0].control, event_ring[0].flags);
        return -1;
    }
    uint32_t cc = trb_cc(&ev);
    if (cc != 1) {
        kprintf("[xhci] command completion cc=%u type=%u slot=%u\n",
                cc, (trb.flags >> XHCI_TRB_TYPE_SHIFT) & 0x3F, ev.flags >> 24);
        return -1;
    }
    if (event_out) *event_out = ev;
    return 0;
}

static uint32_t *ctx_ptr(void *base, int ctx_index) {
    return (uint32_t *)((uint8_t *)base + (uint32_t)ctx_index * (uint32_t)context_size);
}

static xhci_dev_t *find_xdev(uint8_t usb_addr) {
    for (int i = 0; i < XHCI_MAX_DEVS; i++)
        if (xdevs[i].in_use && xdevs[i].usb_addr == usb_addr) return &xdevs[i];
    return 0;
}

static xhci_dev_t *alloc_xdev(uint8_t usb_addr) {
    for (int i = 0; i < XHCI_MAX_DEVS; i++) {
        if (!xdevs[i].in_use) {
            memset(&xdevs[i], 0, sizeof(xdevs[i]));
            xdevs[i].in_use = 1;
            xdevs[i].usb_addr = usb_addr;
            return &xdevs[i];
        }
    }
    return 0;
}

static uint16_t xhci_default_max_packet(int speed, uint8_t mps0) {
    if (mps0) return mps0;
    if (speed == XHCI_SPEED_SUPER) return 512;
    if (speed == XHCI_SPEED_HIGH) return 64;
    return 8;
}

/* Decode usb_core's pseudo-port encoding for devices behind hubs.
 * Root devices use port=0..N-1. Hub children use ((root+1)<<4)|route_nibbles,
 * where route_nibbles is the xHCI route string (1 nibble per hub depth). */
static void xhci_decode_port_route(int port, uint8_t *root_port, uint32_t *route) {
    if (port >= 16) {
        uint32_t enc = (uint32_t)port;
        uint32_t root = (enc >> 4) & 0x0F;
        *root_port = root ? (uint8_t)root : 1;
        /* Current usb_core encoding stores the root port in the high nibble and
         * the first downstream hub port in the low nibble.  For xHCI the route
         * string contains only downstream hub ports, not the root port. */
        *route = enc & 0x0Fu;
    } else {
        *root_port = (uint8_t)(port + 1);
        *route = 0;
    }
}

static int xhci_alloc_ep_ring(xhci_dev_t *xd, int ep_id) {
    if (xd->ep_ring[ep_id]) return 0;
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return -1;
    uint64_t hhdm = limine_get_hhdm_offset();
    xd->ep_ring_phys[ep_id] = phys;
    xd->ep_ring[ep_id] = (struct xhci_trb *)(uintptr_t)(hhdm + phys);
    memset(xd->ep_ring[ep_id], 0, 4096);
    xd->ep_ring[ep_id][255].param = (uint32_t)phys;
    xd->ep_ring[ep_id][255].status = 0;
    xd->ep_ring[ep_id][255].control = 0;
    xd->ep_ring[ep_id][255].flags = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC | 1;
    xd->ep_cycle[ep_id] = 1;
    xd->ep_idx[ep_id] = 0;
    return 0;
}

int xhci_address_device(uint8_t usb_addr, int port, int speed, uint8_t max_packet0) {
    if (!op_regs || !dcbaa) return -1;
    xhci_dev_t *xd = alloc_xdev(usb_addr);
    if (!xd) return -1;
    xd->port = port;
    xhci_decode_port_route(port, &xd->root_port, &xd->route_string);
    xd->speed = speed;

    struct xhci_trb cmd = {0}, ev;
    cmd.flags = (XHCI_TRB_CMD_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT);
    if (xhci_cmd_submit(cmd, &ev) != 0) return -1;
    uint8_t slot = (uint8_t)(ev.flags >> 24);
    if (!slot) return -1;
    xd->slot_id = slot;

    uint64_t hhdm = limine_get_hhdm_offset();
    uint64_t dev_ctx_phys = pmm_alloc_contiguous((XHCI_CTX_BYTES + 0xFFF) / 0x1000);
    uint64_t in_ctx_phys  = pmm_alloc_contiguous((XHCI_CTX_BYTES + 0xFFF) / 0x1000);
    if (!dev_ctx_phys || !in_ctx_phys) return -1;
    xd->dev_ctx_phys = dev_ctx_phys;
    xd->input_ctx_phys = in_ctx_phys;
    memset((void *)(uintptr_t)(hhdm + dev_ctx_phys), 0, XHCI_CTX_BYTES);
    memset((void *)(uintptr_t)(hhdm + in_ctx_phys), 0, XHCI_CTX_BYTES);
    dcbaa[slot] = dev_ctx_phys;

    if (xhci_alloc_ep_ring(xd, 1) != 0) return -1;
    xd->ep_max_packet[1] = xhci_default_max_packet(speed, max_packet0);
    xd->ep_type[1] = XHCI_EP_CONTROL;
    xd->ep_configured[1] = 1;

    void *inctx = (void *)(uintptr_t)(hhdm + in_ctx_phys);
    uint32_t *icc = ctx_ptr(inctx, 0);
    icc[0] = 0;
    icc[1] = 0x3; /* add slot + ep0 */
    uint32_t *slot_ctx = ctx_ptr(inctx, 1);
    slot_ctx[0] = (xd->route_string & 0xFFFFFu) | ((uint32_t)(speed & 0xF) << 20) | (1u << 27); /* ContextEntries=1 */
    slot_ctx[1] = ((uint32_t)xd->root_port << 16);
    uint32_t *ep0 = ctx_ptr(inctx, 2);
    ep0[0] = 0;
    ep0[1] = (3u << 1) | (XHCI_EP_CONTROL << 3) |
             ((uint32_t)xd->ep_max_packet[1] << 16);
    ep0[2] = (uint32_t)xd->ep_ring_phys[1] | 1u;
    ep0[3] = 0;
    ep0[4] = 8;

    memset(&cmd, 0, sizeof(cmd));
    cmd.param = (uint32_t)in_ctx_phys;
    cmd.status = 0;
    cmd.flags = (XHCI_TRB_CMD_ADDRESS_DEVICE << XHCI_TRB_TYPE_SHIFT) |
                ((uint32_t)slot << 24);
    if (xhci_cmd_submit(cmd, &ev) != 0) return -1;
    kprintf("[xhci] addressed device: usb_addr=%u slot=%u port=%d speed=%d mps0=%u\n",
            usb_addr, slot, port, speed, xd->ep_max_packet[1]);
    return 0;
}

static int xhci_configure_ep(xhci_dev_t *xd, uint8_t endpoint, uint16_t max_packet, int forced_type) {
    int ep_num = endpoint & 0x0F;
    int is_in = endpoint & 0x80;
    int ep_id = ep_num * 2 + (is_in ? 1 : 0);
    if (ep_id <= 1 || ep_id >= 32) return -1;
    if (xd->ep_configured[ep_id]) return 0;
    if (xhci_alloc_ep_ring(xd, ep_id) != 0) return -1;
    if (!max_packet) max_packet = (xd->speed == XHCI_SPEED_HIGH || xd->speed == XHCI_SPEED_SUPER) ? 512 : 64;
    xd->ep_max_packet[ep_id] = max_packet;
    xd->ep_type[ep_id] = forced_type ? (uint8_t)forced_type : (is_in ? XHCI_EP_BULK_IN : XHCI_EP_BULK_OUT);

    uint64_t hhdm = limine_get_hhdm_offset();
    void *inctx = (void *)(uintptr_t)(hhdm + xd->input_ctx_phys);
    memset(inctx, 0, XHCI_CTX_BYTES);
    uint32_t *icc = ctx_ptr(inctx, 0);
    icc[1] = (1u << ep_id) | 0x1; /* add ep + slot */
    uint32_t *slot_ctx = ctx_ptr(inctx, 1);
    uint32_t entries = (uint32_t)ep_id;
    slot_ctx[0] = (xd->route_string & 0xFFFFFu) | ((uint32_t)(xd->speed & 0xF) << 20) | (entries << 27);
    slot_ctx[1] = ((uint32_t)xd->root_port << 16);
    uint32_t *ep = ctx_ptr(inctx, 1 + ep_id);
    ep[0] = 0;
    ep[1] = (3u << 1) | ((uint32_t)xd->ep_type[ep_id] << 3) |
            ((uint32_t)max_packet << 16);
    ep[2] = (uint32_t)xd->ep_ring_phys[ep_id] | 1u;
    ep[3] = 0;
    ep[4] = max_packet;

    struct xhci_trb cmd = {0}, ev;
    cmd.param = (uint32_t)xd->input_ctx_phys;
    cmd.status = 0;
    cmd.flags = (XHCI_TRB_CMD_CONFIGURE_ENDPOINT << XHCI_TRB_TYPE_SHIFT) |
                ((uint32_t)xd->slot_id << 24);
    if (xhci_cmd_submit(cmd, &ev) != 0) return -1;
    xd->ep_configured[ep_id] = 1;
    kprintf("[xhci] configured ep 0x%02x slot=%u ep_id=%d maxpkt=%u\n",
            endpoint, xd->slot_id, ep_id, max_packet);
    return 0;
}

static int xhci_ring_enqueue(xhci_dev_t *xd, int ep_id, struct xhci_trb trb) {
    int idx = xd->ep_idx[ep_id];
    trb.flags |= (uint32_t)xd->ep_cycle[ep_id];
    xd->ep_ring[ep_id][idx] = trb;
    xd->ep_idx[ep_id]++;
    if (xd->ep_idx[ep_id] >= 255) {
        xd->ep_ring[ep_id][255].param = (uint32_t)xd->ep_ring_phys[ep_id];
        xd->ep_ring[ep_id][255].status = 0;
        xd->ep_ring[ep_id][255].control = 0;
        xd->ep_ring[ep_id][255].flags = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) |
            XHCI_TRB_TC | (uint32_t)xd->ep_cycle[ep_id];
        xd->ep_idx[ep_id] = 0;
        xd->ep_cycle[ep_id] ^= 1;
    }
    return 0;
}

static int xhci_wait_transfer(uint8_t slot, int ep_id, int silent_timeout) {
    db_wr(slot, (uint32_t)ep_id);
    struct xhci_trb ev;
    if (xhci_poll_event(XHCI_TRB_TRANSFER_EVENT, &ev) != 0) {
        if (!silent_timeout) kprintf("[xhci] transfer timeout slot=%u ep=%d\n", slot, ep_id);
        return -1;
    }
    uint32_t cc = trb_cc(&ev);
    if (cc != 1 && cc != 13) { /* 13 short packet is acceptable */
        kprintf("[xhci] transfer event cc=%u slot=%u ep=%u\n",
                cc, ev.flags >> 24, (ev.flags >> 16) & 0x1F);
        return -1;
    }
    return 0;
}

int xhci_control_transfer(uint8_t dev_addr, int low_speed,
                          const void *setup, void *data,
                          uint16_t data_len, uint8_t max_packet0) {
    (void)low_speed; (void)max_packet0;
    if (op_regs == NULL || setup == NULL) return -1;
    const uint8_t *sb = (const uint8_t *)setup;
    /* USB SET_ADDRESS is handled by xhci_address_device(), not sent as a
     * normal control transfer. */
    if (sb[1] == 5) return 0;
    xhci_dev_t *xd = find_xdev(dev_addr);
    if (!xd) return -1;
    uint64_t hhdm = limine_get_hhdm_offset();
    uint64_t setup_phys = pmm_alloc_frame();
    uint64_t data_phys = data_len ? pmm_alloc_contiguous((data_len + 0xFFF) / 0x1000) : 0;
    if (!setup_phys || (data_len && !data_phys)) return -1;
    memcpy((void *)(uintptr_t)(hhdm + setup_phys), setup, 8);
    if (data_len && data) memcpy((void *)(uintptr_t)(hhdm + data_phys), data, data_len);
    int data_in = (sb[0] & 0x80) ? 1 : 0;
    struct xhci_trb trb;
    memset(&trb, 0, sizeof(trb));
    uint32_t *sp = (uint32_t *)(uintptr_t)(hhdm + setup_phys);
    trb.param = sp[0]; trb.status = sp[1]; trb.control = 8;
    uint32_t trt = data_len ? (data_in ? 3u : 2u) : 0u;
    trb.flags = (XHCI_TRB_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) | (1u << 6) | (trt << 16);
    xhci_ring_enqueue(xd, 1, trb);
    if (data_len) {
        memset(&trb, 0, sizeof(trb));
        trb.param = (uint32_t)data_phys; trb.status = 0; trb.control = data_len;
        trb.flags = (XHCI_TRB_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) | (data_in ? (1u << 16) : 0);
        xhci_ring_enqueue(xd, 1, trb);
    }
    memset(&trb, 0, sizeof(trb));
    trb.flags = (XHCI_TRB_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC |
                ((data_len == 0 || !data_in) ? (1u << 16) : 0);
    xhci_ring_enqueue(xd, 1, trb);
    int ret = xhci_wait_transfer(xd->slot_id, 1, 0);
    if (ret == 0 && data_len && data && data_in) memcpy(data, (void *)(uintptr_t)(hhdm + data_phys), data_len);
    if (data_phys) for (uint32_t i=0;i<(data_len+0xFFF)/0x1000;i++) pmm_free_frame(data_phys+i*4096ULL);
    pmm_free_frame(setup_phys);
    return ret == 0 ? (int)data_len : -1;
}

int xhci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint,
                       void *data, uint32_t len, int in, uint16_t max_packet) {
    if (op_regs == NULL || data == NULL || len == 0) return -1;
    xhci_dev_t *xd = find_xdev(dev_addr);
    if (!xd) return -1;
    int ep_num = endpoint & 0x0F;
    int ep_id = ep_num * 2 + ((endpoint & 0x80) ? 1 : 0);
    if (xhci_configure_ep(xd, endpoint, max_packet, in ? XHCI_EP_BULK_IN : XHCI_EP_BULK_OUT) != 0) return -1;
    uint64_t hhdm = limine_get_hhdm_offset();
    uint64_t buf_phys = pmm_alloc_contiguous((len + 0xFFF) / 0x1000);
    if (!buf_phys) return -1;
    if (!in) memcpy((void *)(uintptr_t)(hhdm + buf_phys), data, len);
    else memset((void *)(uintptr_t)(hhdm + buf_phys), 0, len);
    struct xhci_trb trb;
    memset(&trb, 0, sizeof(trb));
    trb.param = (uint32_t)buf_phys;
    trb.status = 0;
    trb.control = len;
    trb.flags = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
    xhci_ring_enqueue(xd, ep_id, trb);
    int ret = xhci_wait_transfer(xd->slot_id, ep_id, 0);
    if (ret == 0 && in) memcpy(data, (void *)(uintptr_t)(hhdm + buf_phys), len);
    for (uint32_t i=0;i<(len+0xFFF)/0x1000;i++) pmm_free_frame(buf_phys+i*4096ULL);
    return ret == 0 ? (int)len : -1;
}


int xhci_interrupt_transfer(uint8_t dev_addr, uint8_t endpoint,
                            int low_speed, uint16_t max_packet,
                            void *data, uint16_t len, int *toggle_io) {
    (void)low_speed; (void)toggle_io;
    if (op_regs == NULL || data == NULL || len == 0 || !(endpoint & 0x80)) return -1;
    xhci_dev_t *xd = find_xdev(dev_addr);
    if (!xd) return -1;
    int ep_num = endpoint & 0x0F;
    int ep_id = ep_num * 2 + 1;
    if (xhci_configure_ep(xd, endpoint, max_packet ? max_packet : len, XHCI_EP_INTR_IN) != 0) return -1;
    uint64_t hhdm = limine_get_hhdm_offset();
    uint64_t buf_phys = pmm_alloc_frame();
    if (!buf_phys) return -1;
    memset((void *)(uintptr_t)(hhdm + buf_phys), 0, len);
    struct xhci_trb trb;
    memset(&trb, 0, sizeof(trb));
    trb.param = (uint32_t)buf_phys;
    trb.status = 0;
    trb.control = len;
    trb.flags = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
    xhci_ring_enqueue(xd, ep_id, trb);
    int ret = xhci_wait_transfer(xd->slot_id, ep_id, 1);
    if (ret == 0) {
        memcpy(data, (void *)(uintptr_t)(hhdm + buf_phys), len);
        ret = (int)len;
    }
    pmm_free_frame(buf_phys);
    return ret;
}

void xhci_self_test(void) {
    if (cap_regs == NULL) {
        kprintf("[xhci] self-test: no controller\n");
        return;
    }

    uint32_t sts = op_rd(XHCI_OP_USBSTS);
    int halted = (sts & XHCI_USBSTS_HCH) ? 1 : 0;
    int cnr = (sts & XHCI_USBSTS_CNR) ? 1 : 0;

    kprintf("[xhci] self-test: halted=%d CNR=%d\n", halted, cnr);

    /* Verify the CRCR is valid (CRR=0 means not running, which is OK if idle). */
    uint32_t crcr = op_rd(XHCI_OP_CRCR);
    kprintf("[xhci] CRCR=0x%08x (RCS=%d CRR=%d)\n",
            crcr, crcr & 1, (crcr >> 3) & 1);

    /* Report port status. */
    for (int i = 0; i < num_ports && i < 8; i++) {
        uint32_t ps = port_rd(i);
        int speed = (ps >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK;
        kprintf("[xhci] port %d: CCS=%d PED=%d PP=%d speed=%d PLS=%d\n",
                i,
                (ps & XHCI_PORTSC_CCS) ? 1 : 0,
                (ps & XHCI_PORTSC_PED) ? 1 : 0,
                (ps & XHCI_PORTSC_PP) ? 1 : 0,
                speed,
                (ps >> XHCI_PORTSC_PLS_SHIFT) & XHCI_PORTSC_PLS_MASK);
    }

    if (!halted && !cnr) {
        kprintf("[xhci] PASS: controller running, %d device(s)\n", port_count);
    } else {
        kprintf("[xhci] FAIL: controller not operational\n");
    }
}
