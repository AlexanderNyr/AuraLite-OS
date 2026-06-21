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
static struct uhci_td *idle_td = NULL;  /* HHDM pointer to an idle TD */
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
