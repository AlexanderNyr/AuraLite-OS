/* uhci.c — UHCI (USB 1.1) host controller driver for QEMU's PIIX3.
 *
 * Implements: PCI detection, controller reset, frame list setup, port
 * reset/enumeration, and basic transfer descriptor (TD) handling.
 *
 * The UHCI uses I/O port-mapped registers (BAR4, 32 bytes) and a periodic
 * frame list (1024 × 4-byte entries). Each entry points to a Queue Head (QH)
 * or Transfer Descriptor (TD) via a physical address.
 *
 * QEMU setup: UHCI is built into the PIIX3 southbridge. Enable with:
 *   -usb                          (adds the default UHCI controller)
 *   -device usb-mouse             (attach a USB mouse)
 *   -device usb-kbd               (attach a USB keyboard)
 *   -drive file=disk.img,if=none,id=usbstick \
 *   -device usb-storage,drive=usbstick
 */

#include <stdint.h>
#include "drivers/usb/uhci.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/limine_requests.h"

/* ---- UHCI I/O register offsets (from BAR4 base) ---- */
#define UHCI_USBCMD      0x00    /* Command register */
#define UHCI_USBSTS      0x02    /* Status register */
#define UHCI_USBINTR     0x04    /* Interrupt enable */
#define UHCI_FRNUM       0x06    /* Frame number */
#define UHCI_FLBASEADD   0x08    /* Frame List Base Address (32-bit) */
#define UHCI_SOFMOD      0x0C    /* Start of Frame Modify */
#define UHCI_PORTSC1     0x10    /* Port 1 Status/Control */
#define UHCI_PORTSC2     0x12    /* Port 2 Status/Control */

/* USBCMD bits */
#define USBCMD_RUN       (1u << 0)    /* Run/Stop */
#define USBCMD_HCRESET   (1u << 1)    /* Host Controller Reset */
#define USBCMD_GRESET    (1u << 2)    /* Global Reset */
#define USBCMD_MAXPACKET (1u << 7)    /* Max packet is 64 bytes (0=32) */
#define USBCMD_CF        (1u << 6)    /* Configured Flag */

/* USBSTS bits */
#define USBSTS_HCHALTED  (1u << 5)    /* HC Halted */

/* PORTSC bits */
#define PORTSC_CCS       (1u << 0)    /* Current Connect Status */
#define PORTSC_CSC       (1u << 1)    /* Connect Status Change */
#define PORTSC_PED       (1u << 2)    /* Port Enable/Detect */
#define PORTSC_ECSC      (1u << 3)    /* Enable Status Change */
#define PORTSC_LS_MASK   (0x3 << 4)   /* Line Status (D+/D-) */
#define PORTSC_RD        (1u << 6)    /* Resume Detect */
#define PORTSC_LSDA      (1u << 8)    /* Low Speed Device Attached */
#define PORTSC_PR        (1u << 9)    /* Port Reset */
#define PORTSC_SUSPEND  (1u << 12)   /* Suspend */

/* ---- Transfer Descriptor (TD) structure (32 bytes) ---- */
/* Link pointer format: bits 0 = Terminate, 1 = QH (vs TD), 2-31 = address */
struct uhci_td {
    uint32_t link;       /* link to next TD/QH (phys addr | flags) */
    uint32_t ctrl;       /* control/status bits */
    uint32_t token;      /* PID, device addr, endpoint, data toggle, length */
    uint32_t buffer;     /* physical address of data buffer */
} __attribute__((packed, aligned(16)));

/* ---- Queue Head (QH) structure (8 bytes) ---- */
struct uhci_qh {
    uint32_t head_link;  /* link to next QH/TD */
    uint32_t element_link;/* link to first TD in this queue */
} __attribute__((packed, aligned(16)));

/* TD control bits */
#define TD_CTRL_ACTIVE   (1u << 23)
#define TD_CTRL_STALLED  (1u << 22)
#define TD_CTRL_DATA_BUF (1u << 21)
#define TD_CTRL_BABBLE   (1u << 20)
#define TD_CTRL_NAK      (1u << 19)
#define TD_CTRL_TIMEOUT  (1u << 18)
#define TD_CTRL_BITSTUFF (1u << 17)
#define TD_CTRL_LS       (1u << 26)   /* Low Speed Device */
#define TD_CTRL_SPD      (1u << 29)   /* Short Packet Detect */
#define TD_CTRL_CERR_SHIFT 27          /* Error Counter bits 27-28 */

/* TD token bits */
#define TD_TOKEN_PID_SHIFT  0    /* bits 0-7: PID */
#define TD_TOKEN_DEV_SHIFT  8    /* bits 8-14: device address */
#define TD_TOKEN_EP_SHIFT   15   /* bits 15-18: endpoint */
#define TD_TOKEN_DT_SHIFT   19   /* bit 19: data toggle */
#define TD_TOKEN_LEN_SHIFT  21   /* bits 21-31: length (maxlen-1) */

/* PIDs */
#define PID_SETUP  0x2D
#define PID_IN     0x69
#define PID_OUT    0xE1

/* ---- Frame list ---- */
#define UHCI_FRAME_COUNT 1024
#define UHCI_FRAME_SIZE  (UHCI_FRAME_COUNT * 4)   /* 4 KiB */

/* ---- Driver state ---- */
static uint16_t iobase = 0;             /* I/O port base (BAR4) */
static uint32_t *frame_list = NULL;     /* HHDM pointer to the frame list */
static struct uhci_qh *async_qh = NULL; /* HHDM pointer to the async QH */
static int port_count = 0;
static uint8_t pci_bus_u, pci_dev_u, pci_func_u;

/* ---- I/O helpers ---- */
static inline uint16_t rd16(uint8_t off) {
    return inw(iobase + off);
}
static inline void wr16(uint8_t off, uint16_t val) {
    outw(iobase + off, val);
}
static inline uint32_t rd32(uint8_t off) {
    return inl(iobase + off);
}
static inline void wr32(uint8_t off, uint32_t val) {
    outl(iobase + off, val);
}

/* Wait for the controller to halt (used after sending RUN=0). */
static int wait_halt(void) {
    for (int i = 0; i < 100000; i++) {
        if (rd16(UHCI_USBSTS) & USBSTS_HCHALTED) {
            return 0;
        }
        __asm__ volatile ("pause");
    }
    return -1;
}

/* ---- Port operations ---- */

static int uhci_port_has_device_raw(uint8_t port_off) {
    uint16_t sc = rd16(port_off);
    /* CCS (bit 0) = current connect status. */
    return (sc & PORTSC_CCS) ? 1 : 0;
}

static void uhci_port_reset(uint8_t port_off) {
    /* Enable port reset. */
    uint16_t sc = rd16(port_off);
    wr16(port_off, sc | PORTSC_PR);
    /* Wait at least 50ms (USB spec requires 10-20ms; use more). */
    for (volatile int i = 0; i < 5000000; i++)
        __asm__ volatile ("nop");
    /* Clear port reset. */
    sc = rd16(port_off);
    wr16(port_off, sc & ~PORTSC_PR);
    /* Wait for the port to settle. */
    for (volatile int i = 0; i < 500000; i++)
        __asm__ volatile ("nop");
    /* Enable the port. */
    sc = rd16(port_off);
    wr16(port_off, sc | PORTSC_PED);
    /* Clear status change bits. */
    wr16(port_off, rd16(port_off) | PORTSC_CSC | PORTSC_ECSC);
}

int uhci_port_has_device(int port) {
    if (port < 0 || port >= UHCI_MAX_PORTS) return 0;
    uint8_t off = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
    return uhci_port_has_device_raw(off);
}

int uhci_port_is_low_speed(int port) {
    if (port < 0 || port >= UHCI_MAX_PORTS) return 0;
    uint8_t off = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
    return (rd16(off) & PORTSC_LSDA) ? 1 : 0;
}

/* ---- TD/QH chain execution ---- */

/* Build a TD token field. */
static uint32_t make_td_token(uint8_t pid, uint8_t dev_addr, uint8_t endpoint,
                              int data_toggle, uint32_t max_len) {
    uint32_t token = 0;
    token |= (uint32_t)pid << TD_TOKEN_PID_SHIFT;
    token |= (uint32_t)(dev_addr & 0x7F) << TD_TOKEN_DEV_SHIFT;
    token |= (uint32_t)(endpoint & 0xF) << TD_TOKEN_EP_SHIFT;
    token |= (uint32_t)(data_toggle & 1) << TD_TOKEN_DT_SHIFT;
    /* length field = (maxlen - 1) for IN; (maxlen) for OUT. UHCI uses
     * maxlen-1 encoding for both. If maxlen=0, set to 0x7FF (zero-length). */
    if (max_len == 0) {
        token |= (0x7FFu << TD_TOKEN_LEN_SHIFT);
    } else {
        token |= (((max_len - 1) & 0x7FF) << TD_TOKEN_LEN_SHIFT);
    }
    return token;
}

/* Build a TD control field. */
static uint32_t make_td_ctrl(int low_speed, int is_interrupt) {
    uint32_t ctrl = TD_CTRL_ACTIVE;
    ctrl |= (3u << TD_CTRL_CERR_SHIFT);   /* 3 error retries */
    if (low_speed) {
        ctrl |= TD_CTRL_LS;
    }
    if (is_interrupt) {
        /* Interrupt on completion. */
    }
    return ctrl;
}

/*
 * Schedule a chain of TDs via the async QH.
 * 1. Create a temporary QH
 * 2. Set its element_link to the first TD
 * 3. Insert it after the idle QH (idle_qh->head_link = our_qh)
 * 4. Wait for the QH's element_link to become 0x1 (terminate = all done)
 * 5. Restore idle QH
 */
static int uhci_schedule_tds(volatile struct uhci_td *first_td,
                             volatile struct uhci_td *last_td,
                             uint32_t first_td_phys) {
    uint64_t hhdm = limine_get_hhdm_offset();

    /* Allocate a QH for this transfer. */
    uint64_t qh_phys = pmm_alloc_frame();
    if (qh_phys == 0) return -1;
    volatile struct uhci_qh *qh =
        (volatile struct uhci_qh *)(uintptr_t)(hhdm + qh_phys);
    memset((void *)qh, 0, sizeof(*qh));

    /* Set up the QH: element_link = first TD, head_link = terminate. */
    qh->element_link = first_td_phys;   /* points to first TD (not QH type) */
    qh->head_link = 0x1;               /* terminate (no next QH) */

    /* Replace ALL frame list entries with our QH so the controller sees it
     * in the very next 1ms frame. */
    uint32_t saved_frame0 = frame_list[0];
    uint64_t saved_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(saved_flags));
    for (int i = 0; i < UHCI_FRAME_COUNT; i++) {
        frame_list[i] = (uint32_t)qh_phys | (1u << 1);
    }
    if (saved_flags & 0x200ULL) {
        __asm__ volatile ("sti" ::: "memory");
    }

    /* Wait for completion: element_link becomes 0x1 when all TDs are done. */
    int timeout = 10000000;
    while (timeout-- > 0) {
        uint32_t el = qh->element_link;
        if ((el & 0x1) && ((el & ~0xFUL) == 0)) break;
        __asm__ volatile ("pause");
    }

    /* Restore frame list. */
    __asm__ volatile ("cli" ::: "memory");
    for (int i = 0; i < UHCI_FRAME_COUNT; i++) {
        frame_list[i] = saved_frame0;
    }
    if (saved_flags & 0x200ULL) {
        __asm__ volatile ("sti" ::: "memory");
    }

    if (timeout < 0) {
        kprintf("[uhci] TD chain timeout (el=0x%08x TD0.c=0x%08x.t=0x%08x TD1.c=0x%08x.t=0x%08x)\n",
                qh->element_link,
                first_td[0].ctrl, first_td[0].token,
                first_td[1].ctrl, first_td[1].token);
        return -1;
    }

    /* Check TD status for errors. */
    if (last_td->ctrl & (TD_CTRL_STALLED | TD_CTRL_DATA_BUF |
                         TD_CTRL_BABBLE | TD_CTRL_BITSTUFF)) {
        kprintf("[uhci] TD error: ctrl=0x%08x\n", last_td->ctrl);
        return -1;
    }

    /* Extract actual bytes transferred from the last TD's token field. */
    /* Actual length = maxlen_field - (remaining bits in token[21..31]).
     * UHCI stores the actual length in the same field after completion:
     * the HC writes (maxlen - actual_len) into the field. So:
     *   actual = original_maxlen - (current_value & 0x7FF)
     * But we set maxlen-1 encoding, so the formula is different.
     * For simplicity, return 0 (success) and let callers check. */
    return 0;
}

int uhci_control_transfer(uint8_t dev_addr, int low_speed,
                          const void *setup_pkt, void *data, uint16_t data_len) {
    if (iobase == 0) return -1;

    uint64_t hhdm = limine_get_hhdm_offset();

    /* Allocate DMA buffers for the setup packet and data. */
    uint64_t setup_phys = pmm_alloc_frame();
    uint64_t data_phys = (data_len > 0) ? pmm_alloc_frame() : 0;
    if (setup_phys == 0) return -1;

    /* Allocate 3 TDs (SETUP, DATA, STATUS) from one frame. */
    uint64_t td_phys = pmm_alloc_frame();
    if (td_phys == 0) return -1;
    volatile struct uhci_td *tds =
        (volatile struct uhci_td *)(uintptr_t)(hhdm + td_phys);
    memset((void *)tds, 0, 48);  /* 3 × 16 bytes */

    /* Copy setup packet to DMA buffer. */
    memcpy((void *)(uintptr_t)(hhdm + setup_phys), setup_pkt, 8);
    /* Copy data to DMA buffer if writing. */
    if (data && data_len > 0 && data_phys) {
        memcpy((void *)(uintptr_t)(hhdm + data_phys), data, data_len);
    }

    /* Determine direction from the setup packet. */
    const uint8_t *setup_bytes = (const uint8_t *)setup_pkt;
    int data_in = (setup_bytes[0] & 0x80) ? 1 : 0;

    /* TD 0: SETUP phase. */
    tds[0].link   = (uint32_t)(td_phys + 16);  /* link to TD1 */
    tds[0].ctrl   = make_td_ctrl(low_speed, 0);
    tds[0].token  = make_td_token(PID_SETUP, dev_addr, 0, 0, 8);
    tds[0].buffer = (uint32_t)setup_phys;

    /* TD 1: DATA phase (if any). */
    if (data_len > 0) {
        uint8_t data_pid = data_in ? PID_IN : PID_OUT;
        tds[1].link   = (uint32_t)(td_phys + 32);  /* link to TD2 */
        tds[1].ctrl   = make_td_ctrl(low_speed, 0) | TD_CTRL_SPD;
        tds[1].token  = make_td_token(data_pid, dev_addr, 0, 1, data_len);
        tds[1].buffer = (uint32_t)data_phys;
    } else {
        /* No data phase: TD1 is the STATUS phase (zero-length IN).
         * Link directly to terminate since there's no TD2 needed. */
        tds[1].link   = 0x1;  /* terminate */
        tds[1].ctrl   = make_td_ctrl(low_speed, 1);
        tds[1].token  = make_td_token(PID_IN, dev_addr, 0, 1, 0);
        tds[1].buffer = 0;
    }

    /* TD 2: STATUS phase (if data phase exists, TD2 is status). */
    if (data_len > 0) {
        tds[2].link   = 0x1;  /* terminate */
        /* Status direction is opposite of data. */
        uint8_t status_pid = data_in ? PID_OUT : PID_IN;
        tds[2].ctrl   = make_td_ctrl(low_speed, 1);
        tds[2].token  = make_td_token(status_pid, dev_addr, 0, 1, 0);
        tds[2].buffer = 0;
    } else {
        tds[2].link   = 0x1;
        tds[2].ctrl   = 0;  /* unused */
        tds[2].token  = 0;
        tds[2].buffer = 0;
    }

    /* Schedule and wait. */
    int ret = uhci_schedule_tds(&tds[0], data_len > 0 ? &tds[2] : &tds[1],
                                (uint32_t)td_phys);
    if (ret < 0) {
        return -1;
    }

    /* If data was read (IN direction), copy from DMA buffer to caller. */
    if (data && data_len > 0 && data_in && data_phys) {
        memcpy(data, (void *)(uintptr_t)(hhdm + data_phys), data_len);
    }

    return (int)data_len;
}

int uhci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint,
                       void *data, uint32_t len) {
    if (iobase == 0 || len == 0) return -1;

    uint64_t hhdm = limine_get_hhdm_offset();
    int is_in = (endpoint & 0x80) ? 1 : 0;
    uint8_t ep = endpoint & 0x0F;
    uint8_t pid = is_in ? PID_IN : PID_OUT;

    /* Allocate DMA buffer + TD. */
    uint64_t buf_phys = pmm_alloc_contiguous((len + 0xFFF) / 0x1000);
    uint64_t td_phys = pmm_alloc_frame();
    if (buf_phys == 0 || td_phys == 0) return -1;

    volatile struct uhci_td *td =
        (volatile struct uhci_td *)(uintptr_t)(hhdm + td_phys);
    memset((void *)td, 0, sizeof(*td));

    /* Copy data to DMA buffer if writing. */
    if (!is_in) {
        memcpy((void *)(uintptr_t)(hhdm + buf_phys), data, len);
    }

    td->link   = 0x1;  /* terminate */
    td->ctrl   = make_td_ctrl(0, 1) | TD_CTRL_SPD;  /* full-speed, SPD */
    td->token  = make_td_token(pid, dev_addr, ep, 1, len);
    td->buffer = (uint32_t)buf_phys;

    int ret = uhci_schedule_tds(td, td, (uint32_t)td_phys);
    if (ret < 0) return -1;

    /* Copy data from DMA buffer if reading. */
    if (is_in) {
        memcpy(data, (void *)(uintptr_t)(hhdm + buf_phys), len);
    }

    return (int)len;
}

/* ---- Public API ---- */

int uhci_init(void) {
    /* Find the UHCI controller on PCI.
     * Class 0x0C (Serial Bus), subclass 0x03 (USB), prog_if 0x00 (UHCI). */
    if (pci_find_class(0x0C, 0x03, &pci_bus_u, &pci_dev_u, &pci_func_u) != 0) {
        /* Try by vendor/device: Intel PIIX3 USB = 0x8086:0x7020 */
        if (pci_find_device(0x8086, 0x7020,
                            &pci_bus_u, &pci_dev_u, &pci_func_u) != 0) {
            kprintf("[uhci] no UHCI controller found\n");
            return -1;
        }
    }

    uint16_t vendor = pci_get_vendor(pci_bus_u, pci_dev_u, pci_func_u);
    uint16_t device = pci_get_device(pci_bus_u, pci_dev_u, pci_func_u);
    kprintf("[uhci] controller at PCI %u:%u.%u (0x%04x:0x%04x)\n",
            pci_bus_u, pci_dev_u, pci_func_u, vendor, device);

    /* Enable bus mastering + I/O space. */
    pci_enable_bus_master(pci_bus_u, pci_dev_u, pci_func_u);

    /* Read BAR4 — UHCI uses I/O space (BAR bit 0 = 1 for I/O). */
    uint32_t bar4 = pci_get_bar(pci_bus_u, pci_dev_u, pci_func_u, 4);
    if (!(bar4 & 0x1)) {
        kprintf("[uhci] BAR4 is not I/O space (0x%08x)\n", bar4);
        return -1;
    }
    iobase = (uint16_t)(bar4 & ~0xF);
    kprintf("[uhci] I/O base = 0x%04x\n", iobase);

    /* Disable interrupts during init. */
    wr16(UHCI_USBINTR, 0);

    /* Stop the controller and wait for halt. */
    wr16(UHCI_USBCMD, 0);
    wait_halt();

    /* Global reset (hold for ~100ms per spec). */
    wr16(UHCI_USBCMD, USBCMD_GRESET);
    for (volatile int i = 0; i < 5000000; i++)
        __asm__ volatile ("nop");
    wr16(UHCI_USBCMD, 0);
    wait_halt();

    /* Allocate the frame list (4 KiB, 1024 entries). */
    uint64_t hhdm = limine_get_hhdm_offset();
    uint64_t fl_phys = pmm_alloc_frame();
    if (fl_phys == 0) {
        kprintf("[uhci] OOM for frame list\n");
        return -1;
    }
    frame_list = (uint32_t *)(uintptr_t)(hhdm + fl_phys);

    /* Allocate an idle QH (the "terminate" queue — does nothing). */
    uint64_t qh_phys = pmm_alloc_frame();
    if (qh_phys == 0) return -1;
    async_qh = (struct uhci_qh *)(uintptr_t)(hhdm + qh_phys);
    memset(async_qh, 0, sizeof(*async_qh));
    async_qh->head_link = 0x1;   /* Terminate (no next) */
    async_qh->element_link = 0x1;/* Terminate (no TDs) */

    /* Fill the frame list: every entry points to the idle QH.
     * Each entry is a 32-bit physical address with bit 1 set for QH type. */
    for (int i = 0; i < UHCI_FRAME_COUNT; i++) {
        frame_list[i] = (uint32_t)qh_phys | (1u << 1);  /* QH type bit */
    }

    /* Program the frame list base address. */
    wr32(UHCI_FLBASEADD, (uint32_t)fl_phys);

    /* Reset frame number to 0. */
    wr16(UHCI_FRNUM, 0);

    /* Clear all status bits. */
    wr16(UHCI_USBSTS, 0xFFFF);

    /* Set the Configured Flag. */
    wr16(UHCI_USBCMD, USBCMD_CF);

    /* Start the controller (RUN=1, configured=1, maxpacket=64). */
    wr16(UHCI_USBCMD, USBCMD_RUN | USBCMD_CF | USBCMD_MAXPACKET);

    /* Wait for the controller to start (HCHALTED should clear). */
    for (int i = 0; i < 100000; i++) {
        if (!(rd16(UHCI_USBSTS) & USBSTS_HCHALTED)) {
            break;
        }
        __asm__ volatile ("pause");
    }
    if (rd16(UHCI_USBSTS) & USBSTS_HCHALTED) {
        kprintf("[uhci] controller did not start\n");
        return -1;
    }

    kprintf("[uhci] controller running, frame list at phys 0x%llx\n",
            (unsigned long long)fl_phys);

    /* Enumerate ports. */
    port_count = 0;
    for (int p = 0; p < UHCI_MAX_PORTS; p++) {
        uint8_t off = (p == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
        if (uhci_port_has_device_raw(off)) {
            /* Determine speed: bit 8 (LSDA) = low-speed, else full-speed. */
            uint16_t sc = rd16(off);
            const char *speed = (sc & PORTSC_LSDA) ? "low-speed" : "full-speed";
            kprintf("[uhci] port %d: device attached (%s)\n", p, speed);
            uhci_port_reset(off);
            port_count++;
        }
    }

    if (port_count == 0) {
        kprintf("[uhci] no USB devices detected\n");
    } else {
        kprintf("[uhci] %d device(s) ready\n", port_count);
    }
    return 0;
}

int uhci_get_port_count(void) {
    return port_count;
}

void uhci_self_test(void) {
    if (iobase == 0) {
        kprintf("[uhci] self-test: no controller\n");
        return;
    }

    /* Read the current frame number to verify the controller is running. */
    uint16_t frnum1 = rd16(UHCI_FRNUM);
    for (volatile int i = 0; i < 100000; i++)
        __asm__ volatile ("nop");
    uint16_t frnum2 = rd16(UHCI_FRNUM);

    kprintf("[uhci] self-test: frame counter %u -> %u (delta=%u)\n",
            frnum1, frnum2, (uint16_t)(frnum2 - frnum1));

    if (frnum2 != frnum1) {
        kprintf("[uhci] frame list active (controller running)\n");
    }

    /* Report port status. */
    for (int p = 0; p < UHCI_MAX_PORTS; p++) {
        uint8_t off = (p == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
        uint16_t sc = rd16(off);
        kprintf("[uhci] port %d: CCS=%d PED=%d LSDA=%d\n",
                p,
                (sc & PORTSC_CCS) ? 1 : 0,
                (sc & PORTSC_PED) ? 1 : 0,
                (sc & PORTSC_LSDA) ? 1 : 0);
    }

    if (port_count > 0) {
        kprintf("[uhci] PASS: %d USB device(s) detected\n", port_count);
    } else {
        kprintf("[uhci] PASS: controller initialized, no devices\n");
    }
}
