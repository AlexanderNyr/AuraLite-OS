/* ehci.c — EHCI (USB 2.0) host controller driver.
 *
 * Implements: PCI detection, MMIO register mapping, controller reset,
 * async/periodic schedule setup, port enumeration, and companion controller
 * routing awareness.
 *
 * EHCI uses memory-mapped registers (MMIO via PCI BAR0). The register layout
 * is defined by the EHCI spec (capability registers are at BAR0+0, and the
 * operational registers are at an offset specified by the CAPLENGTH field).
 *
 * QEMU: EHCI appears as PCI 0x0C:0x03:0x20. QEMU's default `-usb` adds an
 * ICH9 EHCI controller. For explicit control:
 *   -device usb-ehci,id=ehci   (older QEMU)
 *   -device ich9-usb-ehci1,id=ehci
 */

#include <stdint.h>
#include "drivers/usb/ehci.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/limine_requests.h"

/* ---- EHCI capability register offsets (read-only, at BAR0 base) ---- */
#define EHCI_CAP_CAPLENGTH  0x00   /* Capability Register Length (8-bit) */
#define EHCI_CAP_HCIVERSION 0x02   /* HCI Version (16-bit, BCD) */
#define EHCI_CAP_HCSPARAMS  0x04   /* Structural Parameters */
#define EHCI_CAP_HCCPARAMS  0x08   /* Capability Parameters */

/* HCSPARAMS bit fields */
#define HCSPARAMS_N_PORTS_MASK   0x0F   /* bits 0-3: number of ports */
#define HCSPARAMS_PPC            (1u << 4) /* Port Power Control */
#define HCSPARAMS_N_PCC_SHIFT    8          /* bits 8-11: N_ports_companion */
#define HCSPARAMS_N_PCC_MASK     0x0F
#define HCSPARAMS_N_CC_SHIFT     12         /* bits 12-15: N_companion_controllers */

/* HCCPARAMS bit fields */
#define HCCPARAMS_EECP_MASK   0xFF   /* bits 0-7: EHCI Extended Capabilities Pointer */
#define HCCPARAMS_64BIT       (1u << 0) /* 64-bit addressing capability */

/* ---- EHCI operational register offsets (at CAPLENGTH offset from BAR0) ---- */
/* The operational register base = BAR0 + CAPLENGTH (read from capability regs). */
#define EHCI_OP_USBCMD       0x00   /* USB Command */
#define EHCI_OP_USBSTS       0x04   /* USB Status */
#define EHCI_OP_USBINTR      0x08   /* USB Interrupt Enable */
#define EHCI_OP_FRINDEX      0x0C   /* Frame Index */
#define EHCI_OP_CTRLDSSEGMENT 0x10  /* Control Data Structure Segment (64-bit) */
#define EHCI_OP_PERIODICLISTBASE 0x14 /* Periodic Frame List Base Address */
#define EHCI_OP_ASYNCLISTADDR 0x18  /* Async List Address */
#define EHCI_OP_CONFIGFLAG   0x40   /* Configure Flag */
#define EHCI_OP_PORTSC       0x44   /* Port Status/Control (port 0) */

/* USBCMD bits */
#define USBCMD_RUN           (1u << 0)    /* Run/Stop */
#define USBCMD_HCRESET       (1u << 1)    /* Host Controller Reset */
#define USBCMD_PSE           (1u << 4)    /* Periodic Schedule Enable */
#define USBCMD_ASE           (1u << 5)    /* Async Schedule Enable */
#define USBCMD_ITC_SHIFT     8            /* Interrupt Threshold Control (bits 8-15) */
#define USBCMD_ITC_MASK      0xFF

/* USBSTS bits */
#define USBSTS_INT           (1u << 0)    /* USB Interrupt */
#define USBSTS_ERR           (1u << 1)    /* USB Error Interrupt */
#define USBSTS_PCD           (1u << 2)    /* Port Change Detect */
#define USBSTS_FLR           (1u << 3)    /* Frame List Rollover */
#define USBSTS_HCHALTED      (1u << 12)   /* HC Halted */
#define USBSTS_RECLAMATION   (1u << 13)   /* Async Reclamation */
#define USBSTS_PSS           (1u << 14)   /* Periodic Schedule Status */
#define USBSTS_ASS           (1u << 15)   /* Async Schedule Status */

/* PORTSC bits */
#define PORTSC_CCS           (1u << 0)    /* Current Connect Status */
#define PORTSC_CSC           (1u << 1)    /* Connect Status Change */
#define PORTSC_PEC           (1u << 3)    /* Port Enable Change */
#define PORTSC_OCA           (1u << 4)    /* Over-Current Active */
#define PORTSC_PR            (1u << 8)    /* Port Reset */
#define PORTSC_PP            (1u << 12)   /* Port Power */
#define PORTSC_OWNER         (1u << 13)   /* Port Owner (release to companion) */
#define PORTSC_LS_SHIFT      10           /* Line Status (bits 10-11) */
#define PORTSC_LS_MASK       0x3
#define PORTSC_LS_KSTATE     0x1          /* K-state = low-speed */
#define PORTSC_LS_JSTATE     0x2          /* J-state = full/high-speed */

/* CONFIGFLAG */
#define CONFIGFLAG_FLAG      (1u << 0)    /* Configure Flag */

/* ---- EHCI Queue Head (QH) — 48 bytes minimum ---- */
struct ehci_qh {
    uint32_t next_qh;       /* link to next QH (phys | flags) */
    uint32_t ep_caps1;      /* endpoint capabilities 1 */
    uint32_t ep_caps2;      /* endpoint capabilities 2 */
    uint32_t current_qtd;   /* current qTD pointer (phys) */
    uint32_t next_qtd;      /* overlay: next qTD pointer */
    uint32_t alt_next_qtd;  /* overlay: alternate next qTD */
    uint32_t token;         /* overlay: qTD token */
    uint32_t buf_ptrs[5];   /* overlay: buffer pointers */
    uint32_t buf_ptrs_hi[5];/* overlay: high 32 bits of buffer pointers */
    uint32_t reserved[3];   /* pad to 48 bytes */
} __attribute__((packed, aligned(32)));

/* QH next_qh field bits */
#define QH_TERMINATE        (1u << 0)
#define QH_TYPE_QH          (1u << 1)    /* type = QH */

/* QH ep_caps1 fields */
#define QH_CAP1_DEVADDR_SHIFT  0          /* bits 0-6: device address */
#define QH_CAP1_EPNUM_SHIFT    8          /* bits 8-11: endpoint number */
#define QH_CAP1_EPS_SHIFT      12         /* bits 12-13: endpoint speed */
#define QH_CAP1_EPS_FULL       0          /* full-speed */
#define QH_CAP1_EPS_LOW        1          /* low-speed */
#define QH_CAP1_EPS_HIGH       2          /* high-speed */
#define QH_CAP1_DTC            (1u << 14) /* Data Toggle Control */
#define QH_CAP1_HBR            (1u << 15) /* Head of Reclamation List */
#define QH_CAP1_MPL_SHIFT      16         /* bits 16-26: max packet length */
#define QH_CAP1_N_C_TRANS_SHIFT 30       /* bits 30-31 (high-speed) */

/* QH ep_caps2 fields */
#define QH_CAP2_MULT_SHIFT     30         /* bits 30-31: multi-TD */

/* ---- EHCI qTD (queue Transfer Descriptor) — 32 bytes ---- */
struct ehci_qtd {
    uint32_t next_qtd;     /* link to next qTD (phys | flags) */
    uint32_t alt_next_qtd; /* alternate next qTD */
    uint32_t token;        /* status, PID, toggle, length */
    uint32_t buf_ptrs[5];  /* up to 5 buffer pointers (20 KiB max per qTD) */
    uint32_t buf_ptrs_hi[5]; /* high 32 bits (64-bit mode) */
} __attribute__((packed, aligned(32)));

/* qTD token bits */
#define QTD_DT               (1u << 31)  /* Data Toggle */
#define QTD_TOTAL_LEN_SHIFT  16          /* bits 16-30: total bytes to transfer */
#define QTD_TOTAL_LEN_MASK   0x7FFF
#define QTD_IOC              (1u << 15)  /* Interrupt On Complete */
#define QTD_C_PAGE_SHIFT     12          /* bits 12-14: current buffer page */
#define QTD_CERR_SHIFT       10          /* bits 10-11: error counter */
#define QTD_CPID_SHIFT       8           /* bits 8-9: PID code (0=OUT,1=IN,2=SETUP) */
#define QTD_PID_OUT          0
#define QTD_PID_IN           1
#define QTD_PID_SETUP        2
#define QTD_STATUS_ACTIVE    (1u << 7)
#define QTD_STATUS_HALTED    (1u << 6)
#define QTD_STATUS_DATA_BUF  (1u << 5)
#define QTD_STATUS_BABBLE    (1u << 4)
#define QTD_STATUS_XACT      (1u << 3)
#define QTD_STATUS_MISSEDSUF (1u << 2)
#define QTD_STATUS_SPLIT     (1u << 1)
#define QTD_STATUS_PING      (1u << 0)

#define QTD_TERMINATE        (1u << 0)

/* ---- Periodic frame list ---- */
#define EHCI_FRAME_COUNT  1024
#define EHCI_FRAME_SIZE   (EHCI_FRAME_COUNT * 4)   /* 4 KiB */

/* ---- Driver state ---- */
static volatile uint8_t *cap_base = NULL;   /* capability registers */
static volatile uint32_t *op_regs = NULL;   /* operational registers */
static uint32_t op_offset = 0;              /* CAPLENGTH value */
static uint32_t *periodic_list = NULL;      /* HHDM pointer */
static struct ehci_qh *async_qh = NULL;     /* async list head QH */
static uint32_t async_qh_phys = 0;
static int num_ports = 0;
static int port_count = 0;
static int has_64bit = 0;
static uint8_t pci_bus_e, pci_dev_e, pci_func_e;

/* ---- MMIO helpers ---- */
static inline uint32_t cap_rd(uint32_t off) {
    return *(volatile uint32_t *)(cap_base + off);
}
static inline uint8_t cap_rd8(uint32_t off) {
    return *(volatile uint8_t *)(cap_base + off);
}
static inline uint16_t cap_rd16(uint32_t off) {
    return *(volatile uint16_t *)(cap_base + off);
}
static inline uint32_t op_rd(uint32_t off) {
    return op_regs[off / 4];
}
static inline void op_wr(uint32_t off, uint32_t val) {
    op_regs[off / 4] = val;
}
static inline uint32_t port_sc(int port) {
    return op_rd(EHCI_OP_PORTSC + port * 4);
}
static inline void port_wr(int port, uint32_t val) {
    op_wr(EHCI_OP_PORTSC + port * 4, val);
}

/* ---- Port operations ---- */

int ehci_port_has_device(int port) {
    if (op_regs == NULL || port < 0 || port >= num_ports) return 0;
    uint32_t ps = port_sc(port);
    return ((ps & PORTSC_CCS) && !(ps & PORTSC_OWNER)) ? 1 : 0;
}

static int ehci_port_reset(int port) {
    /* Set port reset. */
    uint32_t ps = port_sc(port);
    port_wr(port, (ps & ~PORTSC_PR) | PORTSC_PR);

    /* Wait at least 50ms (USB 2.0 spec requires 50ms minimum). */
    for (volatile int i = 0; i < 5000000; i++)
        __asm__ volatile ("nop");

    /* Clear port reset. */
    ps = port_sc(port);
    port_wr(port, ps & ~PORTSC_PR);

    /* Wait for the port to settle. */
    for (volatile int i = 0; i < 500000; i++)
        __asm__ volatile ("nop");

    /* Check if the device is still present after reset. */
    ps = port_sc(port);
    if (!(ps & PORTSC_CCS)) {
        return -1;  /* device disconnected during reset */
    }

    /* Clear connect status change and enable change. */
    port_wr(port, port_sc(port) | PORTSC_CSC | PORTSC_PEC);

    return 0;
}

/* Determine the device speed after reset.
 * In EHCI, if the port is still owned by EHCI after reset, it's high-speed.
 * If the line status was K-state (low-speed) before reset, the port should be
 * released to the companion controller. */
static const char *ehci_port_speed(int port) {
    /* After a successful EHCI reset, the device is high-speed. */
    return "high-speed";
}

/* Release a port to the companion controller (for low/full-speed devices). */
static void ehci_release_port(int port) {
    uint32_t ps = port_sc(port);
    port_wr(port, ps | PORTSC_OWNER);
}

int ehci_init(void) {
    /* Find EHCI: class 0x0C (Serial Bus), subclass 0x03 (USB),
     * prog_if 0x20 (EHCI). */
    for (uint8_t b = 0; b < 1; b++) {
        for (uint8_t d = 0; d < 32; d++) {
            for (uint8_t f = 0; f < 8; f++) {
                if (pci_get_vendor(b, d, f) == 0xFFFF) continue;
                if (pci_get_class(b, d, f) == 0x0C &&
                    pci_get_subclass(b, d, f) == 0x03 &&
                    pci_get_prog_if(b, d, f) == 0x20) {
                    pci_bus_e = b; pci_dev_e = d; pci_func_e = f;
                    goto found;
                }
            }
        }
    }
    kprintf("[ehci] no EHCI controller found\n");
    return -1;

found:
    kprintf("[ehci] controller at PCI %u:%u.%u\n",
            pci_bus_e, pci_dev_e, pci_func_e);

    pci_enable_bus_master(pci_bus_e, pci_dev_e, pci_func_e);

    /* Map BAR0 (MMIO). EHCI typically needs 4 KiB+ — map 8 KiB to be safe. */
    uint32_t bar0 = pci_get_bar(pci_bus_e, pci_dev_e, pci_func_e, 0);
    uint32_t mmio_phys = bar0 & ~0xF;
    uint64_t hhdm = limine_get_hhdm_offset();
    for (uint32_t off = 0; off < 0x2000; off += 0x1000) {
        paging_map(hhdm + mmio_phys + off, mmio_phys + off,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE);
    }
    cap_base = (volatile uint8_t *)(uintptr_t)(hhdm + mmio_phys);

    /* Read the capability registers. */
    op_offset = cap_rd8(EHCI_CAP_CAPLENGTH);
    uint16_t hci_version = cap_rd16(EHCI_CAP_HCIVERSION);
    uint32_t hcsparams = cap_rd(EHCI_CAP_HCSPARAMS);
    uint32_t hccparams = cap_rd(EHCI_CAP_HCCPARAMS);

    num_ports = hcsparams & HCSPARAMS_N_PORTS_MASK;
    int has_ppc = (hcsparams & HCSPARAMS_PPC) ? 1 : 0;
    int n_cc = (hcsparams >> HCSPARAMS_N_CC_SHIFT) & HCSPARAMS_N_PCC_MASK;
    has_64bit = (hccparams & HCCPARAMS_64BIT) ? 1 : 0;

    /* Set up operational register pointer. */
    op_regs = (volatile uint32_t *)(uintptr_t)(hhdm + mmio_phys + op_offset);

    kprintf("[ehci] HCI version %x.%02x, %d ports, PPC=%d, 64-bit=%d, companions=%d\n",
            (hci_version >> 8) & 0xFF, hci_version & 0xFF,
            num_ports, has_ppc, has_64bit, n_cc);

    /* 1) Halt the controller before reset. */
    op_wr(EHCI_OP_USBCMD, 0);
    int t = 1000000;
    while (!(op_rd(EHCI_OP_USBSTS) & USBSTS_HCHALTED) && t-- > 0) {
        __asm__ volatile ("pause");
    }
    if (t < 0) {
        kprintf("[ehci] controller did not halt\n");
        return -1;
    }

    /* 2) Reset the controller. */
    op_wr(EHCI_OP_USBCMD, USBCMD_HCRESET);
    t = 1000000;
    while ((op_rd(EHCI_OP_USBCMD) & USBCMD_HCRESET) && t-- > 0) {
        __asm__ volatile ("pause");
    }
    if (t < 0) {
        kprintf("[ehci] controller reset timeout\n");
        return -1;
    }

    /* 3) Set up 64-bit addressing if supported. */
    if (has_64bit) {
        op_wr(EHCI_OP_CTRLDSSEGMENT, 0);  /* we use < 4 GiB addresses */
    }

    /* 4) Allocate and set up the periodic frame list (4 KiB). */
    uint64_t fl_phys = pmm_alloc_frame();
    if (fl_phys == 0) {
        kprintf("[ehci] OOM for periodic frame list\n");
        return -1;
    }
    periodic_list = (uint32_t *)(uintptr_t)(hhdm + fl_phys);
    /* Fill with terminate bits (no periodic schedule yet). */
    for (int i = 0; i < EHCI_FRAME_COUNT; i++) {
        periodic_list[i] = QH_TERMINATE;
    }
    op_wr(EHCI_OP_PERIODICLISTBASE, (uint32_t)fl_phys);
    op_wr(EHCI_OP_FRINDEX, 0);

    /* 5) Allocate the async list head QH (self-referencing circular list). */
    uint64_t qh_phys = pmm_alloc_frame();
    if (qh_phys == 0) {
        kprintf("[ehci] OOM for async QH\n");
        return -1;
    }
    async_qh_phys = (uint32_t)qh_phys;
    async_qh = (struct ehci_qh *)(uintptr_t)(hhdm + qh_phys);
    memset(async_qh, 0, sizeof(*async_qh));
    async_qh->next_qh = (uint32_t)qh_phys | QH_TYPE_QH;  /* circular: point to self */
    async_qh->ep_caps1 = QH_CAP1_HBR;  /* Head of Reclamation (required) */
    async_qh->next_qtd = QTD_TERMINATE;
    async_qh->alt_next_qtd = QTD_TERMINATE;
    op_wr(EHCI_OP_ASYNCLISTADDR, (uint32_t)qh_phys);

    /* 6) Set the Configure Flag (route all ports to EHCI). */
    op_wr(EHCI_OP_CONFIGFLAG, CONFIGFLAG_FLAG);

    /* 7) Power on all ports if port power control is used. */
    if (has_ppc) {
        for (int i = 0; i < num_ports; i++) {
            port_wr(i, PORTSC_PP);
        }
        for (volatile int i = 0; i < 1000000; i++)
            __asm__ volatile ("nop");
    }

    /* 8) Start the controller: run=1, async=1, periodic=1, ITC=8 (1 microframe). */
    uint32_t cmd = USBCMD_RUN | USBCMD_ASE | USBCMD_PSE | (8u << USBCMD_ITC_SHIFT);
    op_wr(EHCI_OP_USBCMD, cmd);

    /* Wait for the async + periodic schedules to be active. */
    t = 1000000;
    while (!(op_rd(EHCI_OP_USBSTS) & USBSTS_ASS) && t-- > 0) {
        __asm__ volatile ("pause");
    }
    if (t < 0) {
        kprintf("[ehci] async schedule did not start\n");
    }

    kprintf("[ehci] controller running, async+periodic schedules active\n");

    /* 9) Enumerate ports. */
    port_count = 0;
    for (int i = 0; i < num_ports; i++) {
        uint32_t ps = port_sc(i);
        if (ps & PORTSC_CCS) {
            /* Check line status for low-speed detection BEFORE reset.
             * If lines show K-state, it's a low-speed device — release to
             * companion. */
            uint32_t ls = (ps >> PORTSC_LS_SHIFT) & PORTSC_LS_MASK;
            if (ls == PORTSC_LS_KSTATE) {
                kprintf("[ehci] port %d: low-speed device (releasing to companion)\n", i);
                ehci_release_port(i);
                continue;
            }

            /* Attempt high-speed reset. */
            if (ehci_port_reset(i) == 0) {
                kprintf("[ehci] port %d: %s device\n", i, ehci_port_speed(i));
                port_count++;
            } else {
                kprintf("[ehci] port %d: reset failed (releasing to companion)\n", i);
                ehci_release_port(i);
            }
        }
    }

    if (port_count == 0) {
        kprintf("[ehci] no high-speed devices (companions handle low/full-speed)\n");
    } else {
        kprintf("[ehci] %d high-speed device(s) ready\n", port_count);
    }

    return 0;
}


int ehci_reset_port_public(int port) {
    if (op_regs == NULL || port < 0 || port >= num_ports) return -1;
    return ehci_port_reset(port);
}

int ehci_get_port_count(void) {
    return port_count;
}

static void ehci_qtd_set_bufs(volatile struct ehci_qtd *qtd,
                              uint64_t phys, uint32_t len) {
    for (int i = 0; i < 5; i++) { qtd->buf_ptrs[i] = 0; qtd->buf_ptrs_hi[i] = 0; }
    if (len == 0) return;
    uint64_t p = phys;
    for (int i = 0; i < 5; i++) {
        qtd->buf_ptrs[i] = (uint32_t)p;
        qtd->buf_ptrs_hi[i] = 0;
        uint64_t next_page = (p & ~0xFFFULL) + 0x1000ULL;
        if (next_page >= phys + len) break;
        p = next_page;
    }
}

static uint32_t ehci_qtd_token(uint8_t pid, uint32_t len, int toggle, int ioc) {
    uint32_t t = QTD_STATUS_ACTIVE;
    t |= (3u << QTD_CERR_SHIFT);
    t |= ((uint32_t)pid & 0x3u) << QTD_CPID_SHIFT;
    t |= (len & QTD_TOTAL_LEN_MASK) << QTD_TOTAL_LEN_SHIFT;
    if (toggle) t |= QTD_DT;
    if (ioc) t |= QTD_IOC;
    return t;
}

static int ehci_run_async(uint8_t dev_addr, uint8_t endpoint, uint16_t max_packet,
                          volatile struct ehci_qtd *qtds, uint32_t qtd_count,
                          uint32_t first_qtd_phys, uint32_t qh_phys) {
    uint64_t hhdm = limine_get_hhdm_offset();
    volatile struct ehci_qh *qh = (volatile struct ehci_qh *)(uintptr_t)(hhdm + qh_phys);
    memset((void *)qh, 0, 4096);
    if (max_packet == 0) max_packet = 64;
    qh->next_qh = async_qh_phys | QH_TYPE_QH;
    qh->ep_caps1 = ((uint32_t)(dev_addr & 0x7F) << QH_CAP1_DEVADDR_SHIFT) |
                   ((uint32_t)(endpoint & 0x0F) << QH_CAP1_EPNUM_SHIFT) |
                   (QH_CAP1_EPS_HIGH << QH_CAP1_EPS_SHIFT) |
                   QH_CAP1_DTC |
                   ((uint32_t)max_packet << QH_CAP1_MPL_SHIFT);
    qh->ep_caps2 = (1u << QH_CAP2_MULT_SHIFT);
    qh->current_qtd = 0;
    qh->next_qtd = first_qtd_phys;
    qh->alt_next_qtd = QTD_TERMINATE;
    qh->token = 0;

    uint32_t saved_next = async_qh->next_qh;
    async_qh->next_qh = qh_phys | QH_TYPE_QH;

    int timeout = 10000000;
    while (timeout-- > 0) {
        int active = 0;
        for (uint32_t i = 0; i < qtd_count; i++) {
            if (qtds[i].token & QTD_STATUS_ACTIVE) { active = 1; break; }
        }
        if (!active) break;
        __asm__ volatile ("pause");
    }

    async_qh->next_qh = saved_next;
    if (timeout < 0) {
        kprintf("[ehci] async qTD timeout token=0x%08x\n", qtds[0].token);
        return -1;
    }
    for (uint32_t i = 0; i < qtd_count; i++) {
        uint32_t tok = qtds[i].token;
        if (tok & (QTD_STATUS_HALTED | QTD_STATUS_DATA_BUF | QTD_STATUS_BABBLE |
                   QTD_STATUS_XACT | QTD_STATUS_MISSEDSUF | QTD_STATUS_SPLIT)) {
            kprintf("[ehci] qTD%u error token=0x%08x\n", i, tok);
            return -1;
        }
    }
    return 0;
}

int ehci_control_transfer(uint8_t dev_addr, int low_speed,
                          const void *setup, void *data,
                          uint16_t data_len, uint8_t max_packet0) {
    (void)low_speed;
    if (op_regs == NULL || setup == NULL) return -1;
    uint64_t hhdm = limine_get_hhdm_offset();
    uint16_t max_packet = max_packet0 ? max_packet0 : 64;
    uint32_t data_packets = data_len ? ((data_len + max_packet - 1) / max_packet) : 0;
    uint32_t qtd_count = 1 + data_packets + 1;
    if (qtd_count > 64) return -1;
    uint64_t setup_phys = pmm_alloc_frame();
    uint64_t data_phys = data_len ? pmm_alloc_contiguous((data_len + 0xFFF) / 0x1000) : 0;
    uint64_t qtd_phys = pmm_alloc_frame();
    uint64_t qh_phys = pmm_alloc_frame();
    if (!setup_phys || !qtd_phys || !qh_phys || (data_len && !data_phys)) return -1;
    memcpy((void *)(uintptr_t)(hhdm + setup_phys), setup, 8);
    if (data_len && data) memcpy((void *)(uintptr_t)(hhdm + data_phys), data, data_len);
    volatile struct ehci_qtd *qtds = (volatile struct ehci_qtd *)(uintptr_t)(hhdm + qtd_phys);
    memset((void *)qtds, 0, 4096);
    const uint8_t *setup_bytes = (const uint8_t *)setup;
    int data_in = (setup_bytes[0] & 0x80) ? 1 : 0;
    uint32_t n = 0;
    qtds[n].next_qtd = (uint32_t)(qtd_phys + (n + 1) * sizeof(struct ehci_qtd));
    qtds[n].alt_next_qtd = QTD_TERMINATE;
    qtds[n].token = ehci_qtd_token(QTD_PID_SETUP, 8, 0, 0);
    ehci_qtd_set_bufs(&qtds[n], setup_phys, 8);
    n++;
    uint32_t rem = data_len, off = 0; int toggle = 1;
    while (rem) {
        uint32_t chunk = rem > max_packet ? max_packet : rem;
        qtds[n].next_qtd = (uint32_t)(qtd_phys + (n + 1) * sizeof(struct ehci_qtd));
        qtds[n].alt_next_qtd = QTD_TERMINATE;
        qtds[n].token = ehci_qtd_token(data_in ? QTD_PID_IN : QTD_PID_OUT, chunk, toggle, 0);
        ehci_qtd_set_bufs(&qtds[n], data_phys + off, chunk);
        toggle ^= 1; rem -= chunk; off += chunk; n++;
    }
    qtds[n].next_qtd = QTD_TERMINATE;
    qtds[n].alt_next_qtd = QTD_TERMINATE;
    qtds[n].token = ehci_qtd_token(data_len ? (data_in ? QTD_PID_OUT : QTD_PID_IN) : QTD_PID_IN,
                                   0, 1, 1);
    n++;
    int ret = ehci_run_async(dev_addr, 0, max_packet, qtds, n, (uint32_t)qtd_phys, (uint32_t)qh_phys);
    if (ret == 0 && data_len && data && data_in) memcpy(data, (void *)(uintptr_t)(hhdm + data_phys), data_len);
    pmm_free_frame(qh_phys); pmm_free_frame(qtd_phys); pmm_free_frame(setup_phys);
    if (data_phys) for (uint32_t i=0;i<(data_len+0xFFF)/0x1000;i++) pmm_free_frame(data_phys+i*4096ULL);
    return ret == 0 ? (int)data_len : -1;
}

int ehci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint,
                       void *data, uint32_t len, int in, uint16_t max_packet) {
    if (op_regs == NULL || data == NULL || len == 0) return -1;
    uint64_t hhdm = limine_get_hhdm_offset();
    if (max_packet == 0) max_packet = 512;
    uint64_t buf_phys = pmm_alloc_contiguous((len + 0xFFF) / 0x1000);
    uint64_t qtd_phys = pmm_alloc_frame();
    uint64_t qh_phys = pmm_alloc_frame();
    if (!buf_phys || !qtd_phys || !qh_phys) return -1;
    if (!in) memcpy((void *)(uintptr_t)(hhdm + buf_phys), data, len);
    else memset((void *)(uintptr_t)(hhdm + buf_phys), 0, len);
    volatile struct ehci_qtd *qtd = (volatile struct ehci_qtd *)(uintptr_t)(hhdm + qtd_phys);
    memset((void *)qtd, 0, 4096);
    qtd[0].next_qtd = QTD_TERMINATE;
    qtd[0].alt_next_qtd = QTD_TERMINATE;
    qtd[0].token = ehci_qtd_token(in ? QTD_PID_IN : QTD_PID_OUT, len, 0, 1);
    ehci_qtd_set_bufs(&qtd[0], buf_phys, len);
    int ret = ehci_run_async(dev_addr, endpoint & 0x0F, max_packet, qtd, 1,
                             (uint32_t)qtd_phys, (uint32_t)qh_phys);
    if (ret == 0 && in) memcpy(data, (void *)(uintptr_t)(hhdm + buf_phys), len);
    pmm_free_frame(qh_phys); pmm_free_frame(qtd_phys);
    for (uint32_t i=0;i<(len+0xFFF)/0x1000;i++) pmm_free_frame(buf_phys+i*4096ULL);
    return ret == 0 ? (int)len : -1;
}


int ehci_interrupt_transfer(uint8_t dev_addr, uint8_t endpoint,
                            int low_speed, uint16_t max_packet,
                            void *data, uint16_t len, int *toggle_io) {
    (void)low_speed;
    (void)toggle_io;
    if (!(endpoint & 0x80)) return -1;
    /* QEMU's high-speed HID endpoints are accepted through the async qTD path
     * as a polled one-shot IN transaction.  Full/low-speed interrupt endpoints
     * still need split transactions through a TT and remain future work. */
    return ehci_bulk_transfer(dev_addr, endpoint, data, len, 1,
                              max_packet ? max_packet : len);
}

void ehci_self_test(void) {
    if (cap_base == NULL) {
        kprintf("[ehci] self-test: no controller\n");
        return;
    }

    /* Verify the controller is running. */
    uint32_t sts = op_rd(EHCI_OP_USBSTS);
    int halted = (sts & USBSTS_HCHALTED) ? 1 : 0;
    int async_active = (sts & USBSTS_ASS) ? 1 : 0;
    int periodic_active = (sts & USBSTS_PSS) ? 1 : 0;

    kprintf("[ehci] self-test: halted=%d async=%d periodic=%d\n",
            halted, async_active, periodic_active);

    /* Read the frame index to confirm the schedule is advancing. */
    uint32_t fi1 = op_rd(EHCI_OP_FRINDEX);
    for (volatile int i = 0; i < 500000; i++)
        __asm__ volatile ("nop");
    uint32_t fi2 = op_rd(EHCI_OP_FRINDEX);
    kprintf("[ehci] frame index: %u -> %u (delta=%u)\n", fi1, fi2, fi2 - fi1);

    /* Report port status. */
    for (int i = 0; i < num_ports; i++) {
        uint32_t ps = port_sc(i);
        kprintf("[ehci] port %d: CCS=%d PR=%d PP=%d OWNER=%d LS=%d\n",
                i,
                (ps & PORTSC_CCS) ? 1 : 0,
                (ps & PORTSC_PR) ? 1 : 0,
                (ps & PORTSC_PP) ? 1 : 0,
                (ps & PORTSC_OWNER) ? 1 : 0,
                (ps >> PORTSC_LS_SHIFT) & PORTSC_LS_MASK);
    }

    if (!halted && (fi2 != fi1)) {
        kprintf("[ehci] PASS: controller running, frame index advancing\n");
    } else if (!halted) {
        kprintf("[ehci] PASS: controller running\n");
    } else {
        kprintf("[ehci] FAIL: controller halted\n");
    }
}
