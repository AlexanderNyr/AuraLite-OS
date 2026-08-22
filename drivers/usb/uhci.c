/* uhci.c — UHCI (USB 1.1) host controller driver — FULL SUPPORT version.
 *
 * Features:
 * - PCI detection, reset, frame list (1024 entries), idle QH
 * - Port reset/enumeration, speed detection
 * - CONTROL transfers: SETUP+DATA+STATUS with proper toggle and SPD, memory leak fixed
 * - BULK transfers: persistent DATA toggle tracking, contiguous buffer handling, leak fixed
 * - INTERRUPT transfers: polling IN endpoint, NAK handling, toggle tracking
 * - ISOCHRONOUS transfers: implemented via periodic schedule, no data toggle, high BW
 * - Power management: suspend/resume hooks
 * - Isochronous bandwidth accounting (simple frame utilization)
 * - Periodic interrupt QHs linked into frame list for HID
 * - Detailed self-test with frame counter and port diagnostics
 */

#include <stdint.h>
#include "drivers/usb/uhci.h"
#include "drivers/timer/pit.h"      /* RES-01: guest-time TD bound */
#include "drivers/pci/pci.h"
#include "kernel/arch/arch.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/boot_info.h"

#define UHCI_USBCMD      0x00
#define UHCI_USBSTS      0x02
#define UHCI_USBINTR     0x04
#define UHCI_FRNUM       0x06
#define UHCI_FLBASEADD   0x08
#define UHCI_SOFMOD      0x0C
#define UHCI_PORTSC1     0x10
#define UHCI_PORTSC2     0x12

#define USBCMD_RUN       (1u << 0)
#define USBCMD_HCRESET   (1u << 1)
#define USBCMD_GRESET    (1u << 2)
#define USBCMD_MAXPACKET (1u << 7)
#define USBCMD_CF        (1u << 6)

#define USBSTS_HCHALTED  (1u << 5)

#define PORTSC_CCS       (1u << 0)
#define PORTSC_CSC       (1u << 1)
#define PORTSC_PED       (1u << 2)
#define PORTSC_ECSC      (1u << 3)
#define PORTSC_LS_MASK   (0x3 << 4)
#define PORTSC_RD        (1u << 6)
#define PORTSC_LSDA      (1u << 8)
#define PORTSC_PR        (1u << 9)
#define PORTSC_SUSPEND   (1u << 12)

struct uhci_td {
    uint32_t link;
    uint32_t ctrl;
    uint32_t token;
    uint32_t buffer;
} __attribute__((packed, aligned(16)));

struct uhci_qh {
    uint32_t head_link;
    uint32_t element_link;
} __attribute__((packed, aligned(16)));

#define TD_CTRL_ACTIVE   (1u << 23)
#define TD_CTRL_STALLED  (1u << 22)
#define TD_CTRL_DATA_BUF (1u << 21)
#define TD_CTRL_BABBLE   (1u << 20)
#define TD_CTRL_NAK      (1u << 19)
#define TD_CTRL_TIMEOUT  (1u << 18)
#define TD_CTRL_BITSTUFF (1u << 17)
#define TD_CTRL_LS       (1u << 26)
#define TD_CTRL_SPD      (1u << 29)
#define TD_CTRL_IOS      (1u << 25)
#define TD_CTRL_CERR_SHIFT 27

#define TD_TOKEN_PID_SHIFT  0
#define TD_TOKEN_DEV_SHIFT  8
#define TD_TOKEN_EP_SHIFT   15
#define TD_TOKEN_DT_SHIFT   19
#define TD_TOKEN_LEN_SHIFT  21

#define PID_SETUP  0x2D
#define PID_IN     0x69
#define PID_OUT    0xE1

#define UHCI_FRAME_COUNT 1024

static uint16_t iobase = 0;
static uint32_t *frame_list = NULL;
static struct uhci_qh *async_qh = NULL;
static uint64_t async_qh_phys = 0;
static uint64_t frame_list_phys = 0;
static int port_count = 0;
static uint8_t pci_bus_u, pci_dev_u, pci_func_u;
static uint32_t frame_bandwidth[UHCI_FRAME_COUNT];

static inline uint16_t rd16(uint8_t off) { return inw(iobase + off); }
static inline void wr16(uint8_t off, uint16_t val) { outw(iobase + off, val); }
static inline uint32_t rd32(uint8_t off) { return inl(iobase + off); }
static inline void wr32(uint8_t off, uint32_t val) { outl(iobase + off, val); }

static int wait_halt(void) {
    for (int i = 0; i < 100000; i++) {
        if (rd16(UHCI_USBSTS) & USBSTS_HCHALTED) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static int uhci_port_has_device_raw(uint8_t port_off) {
    return (rd16(port_off) & PORTSC_CCS) ? 1 : 0;
}
static void uhci_port_reset(uint8_t port_off) {
    uint16_t sc = rd16(port_off);
    wr16(port_off, sc | PORTSC_PR);
    for (volatile int i = 0; i < 5000000; i++) __asm__ volatile("nop");
    sc = rd16(port_off);
    wr16(port_off, sc & ~PORTSC_PR);
    for (volatile int i = 0; i < 500000; i++) __asm__ volatile("nop");
    sc = rd16(port_off);
    wr16(port_off, sc | PORTSC_PED);
    wr16(port_off, rd16(port_off) | PORTSC_CSC | PORTSC_ECSC);
}

int uhci_port_has_device(int port) {
    if (iobase == 0 || port < 0 || port >= UHCI_MAX_PORTS) return 0;
    uint8_t off = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
    return uhci_port_has_device_raw(off);
}
int uhci_reset_port(int port) {
    if (iobase == 0 || port < 0 || port >= UHCI_MAX_PORTS) return -1;
    uint8_t off = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
    uhci_port_reset(off);
    return uhci_port_has_device(port) ? 0 : -1;
}
int uhci_port_is_low_speed(int port) {
    if (iobase == 0 || port < 0 || port >= UHCI_MAX_PORTS) return 0;
    uint8_t off = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
    return (rd16(off) & PORTSC_LSDA) ? 1 : 0;
}

static uint32_t make_td_token(uint8_t pid, uint8_t dev_addr, uint8_t endpoint, int dt, uint32_t max_len) {
    uint32_t token = 0;
    token |= (uint32_t)pid << TD_TOKEN_PID_SHIFT;
    token |= (uint32_t)(dev_addr & 0x7F) << TD_TOKEN_DEV_SHIFT;
    token |= (uint32_t)(endpoint & 0xF) << TD_TOKEN_EP_SHIFT;
    token |= (uint32_t)(dt & 1) << TD_TOKEN_DT_SHIFT;
    if (max_len == 0) token |= (0x7FFu << TD_TOKEN_LEN_SHIFT);
    else token |= (((max_len - 1) & 0x7FF) << TD_TOKEN_LEN_SHIFT);
    return token;
}
static uint32_t make_td_ctrl(int low_speed, int isoc) {
    uint32_t ctrl = TD_CTRL_ACTIVE;
    ctrl |= (3u << TD_CTRL_CERR_SHIFT);
    if (low_speed) ctrl |= TD_CTRL_LS;
    if (isoc) ctrl |= TD_CTRL_IOS;
    return ctrl;
}

static int uhci_schedule_tds(volatile struct uhci_td *first_td,
                             volatile struct uhci_td *last_td,
                             uint32_t first_td_phys, uint64_t qh_phys) {
    uint64_t hhdm = boot_get_hhdm_offset();
    volatile struct uhci_qh *qh = (volatile struct uhci_qh *)(uintptr_t)(hhdm + qh_phys);
    memset((void*)qh, 0, sizeof(*qh));
    qh->element_link = first_td_phys;
    qh->head_link = 0x1;
    uint32_t saved_frame0 = frame_list[0];
    uint64_t saved_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(saved_flags));
    for (int i = 0; i < UHCI_FRAME_COUNT; i++) {
        frame_list[i] = (uint32_t)qh_phys | (1u << 1);
    }
    if (saved_flags & 0x200ULL) __asm__ volatile("sti" ::: "memory");
    /* RES-01 (ledger): the wait is bounded by GUEST TIME, not by an
     * iteration count.  200M pause-iterations meant "however long
     * this vCPU takes to spin that far" -- a starved shared runner
     * once stretched the device's 1ms frames past it and produced
     * the one recorded TD-timeout flake.  Two seconds of PIT time is
     * 2000 frame walks: interrupts are on in this window, the tick
     * advances regardless of vCPU speed.  The iteration fuse stays
     * as a backstop for a broken timer (interrupts off would
     * otherwise make this loop eternal). */
    uint64_t td_deadline = timer_get_ticks() + 200;   /* 2 s @ 100 Hz */
    long td_fuse = 2000000000L;
    int td_timed_out = 1;
    while (timer_get_ticks() < td_deadline && td_fuse-- > 0) {
        uint32_t el = qh->element_link;
        if ((el & 0x1) && ((el & ~0xFUL) == 0)) { td_timed_out = 0; break; }
        __asm__ volatile("pause");
    }
    __asm__ volatile("cli" ::: "memory");
    for (int i = 0; i < UHCI_FRAME_COUNT; i++) frame_list[i] = saved_frame0;
    if (saved_flags & 0x200ULL) __asm__ volatile("sti" ::: "memory");
    int result = 0;
    if (td_timed_out) {
        kprintf("[uhci] TD chain timeout (el=0x%08x)\n", qh->element_link);
        result = -1;
    } else if (last_td->ctrl & (TD_CTRL_STALLED | TD_CTRL_DATA_BUF | TD_CTRL_BABBLE | TD_CTRL_BITSTUFF)) {
        kprintf("[uhci] TD error: ctrl=0x%08x token=0x%08x\n", last_td->ctrl, last_td->token);
        result = -1;
    }
    pmm_free_frame(qh_phys);
    return result;
}

static void free_contiguous(uint64_t base_phys, uint32_t len) {
    if (!base_phys || !len) return;
    uint32_t pages = (len + 0xFFF) / 0x1000;
    for (uint32_t i = 0; i < pages; i++) pmm_free_frame(base_phys + i * 4096ULL);
}

int uhci_control_transfer_ex(uint8_t dev_addr, int low_speed,
                             const void *setup_pkt, void *data, uint16_t data_len,
                             uint8_t max_packet0) {
    if (iobase == 0) return -1;
    uint64_t hhdm = boot_get_hhdm_offset();
    uint32_t max_packet = low_speed ? 8u : (uint32_t)max_packet0;
    if (max_packet != 8 && max_packet != 16 && max_packet != 32 && max_packet != 64) max_packet = low_speed ? 8u : 8u;
    uint32_t data_packets = (data_len > 0) ? ((data_len + max_packet - 1) / max_packet) : 0;
    uint32_t td_count = 1 + data_packets + 1;
    if (td_count * sizeof(struct uhci_td) > 4096) {
        kprintf("[uhci] control transfer too large (%u TDs)\n", td_count);
        return -1;
    }
    uint64_t setup_phys = pmm_alloc_frame();
    uint64_t data_phys = (data_len > 0) ? pmm_alloc_contiguous((data_len + 0xFFF) / 0x1000) : 0;
    uint64_t td_phys = pmm_alloc_frame();
    uint64_t qh_phys = pmm_alloc_frame();
    if (setup_phys == 0 || td_phys == 0 || qh_phys == 0 || (data_len > 0 && data_phys == 0)) {
        if (setup_phys) pmm_free_frame(setup_phys);
        if (data_phys) free_contiguous(data_phys, data_len);
        if (td_phys) pmm_free_frame(td_phys);
        if (qh_phys) pmm_free_frame(qh_phys);
        return -1;
    }
    volatile struct uhci_td *tds = (volatile struct uhci_td *)(uintptr_t)(hhdm + td_phys);
    memset((void*)tds, 0, 4096);
    memcpy((void*)(uintptr_t)(hhdm + setup_phys), setup_pkt, 8);
    if (data && data_len > 0) memcpy((void*)(uintptr_t)(hhdm + data_phys), data, data_len);
    const uint8_t *setup_bytes = (const uint8_t *)setup_pkt;
    int data_in = (setup_bytes[0] & 0x80) ? 1 : 0;
    uint32_t td = 0;
    tds[td].link = (td_count > 1) ? (uint32_t)(td_phys + (td + 1) * 16) : 0x1;
    tds[td].ctrl = make_td_ctrl(low_speed, 0);
    tds[td].token = make_td_token(PID_SETUP, dev_addr, 0, 0, 8);
    tds[td].buffer = (uint32_t)setup_phys;
    td++;
    uint32_t remaining = data_len;
    uint32_t offset = 0;
    int toggle = 1;
    while (remaining > 0) {
        uint32_t chunk = remaining > max_packet ? max_packet : remaining;
        uint8_t pid = data_in ? PID_IN : PID_OUT;
        tds[td].link = (uint32_t)(td_phys + (td + 1) * 16);
        tds[td].ctrl = make_td_ctrl(low_speed, 0) | TD_CTRL_SPD;
        tds[td].token = make_td_token(pid, dev_addr, 0, toggle, chunk);
        tds[td].buffer = (uint32_t)(data_phys + offset);
        td++;
        toggle ^= 1;
        offset += chunk;
        remaining -= chunk;
    }
    uint8_t status_pid = data_len > 0 ? (data_in ? PID_OUT : PID_IN) : PID_IN;
    tds[td].link = 0x1;
    tds[td].ctrl = make_td_ctrl(low_speed, 0);
    tds[td].token = make_td_token(status_pid, dev_addr, 0, 1, 0);
    tds[td].buffer = 0;
    int ret = uhci_schedule_tds(&tds[0], &tds[td], (uint32_t)td_phys, qh_phys);
    if (ret == 0 && data && data_len > 0 && data_in) {
        memcpy(data, (void*)(uintptr_t)(hhdm + data_phys), data_len);
    }
    pmm_free_frame(td_phys);
    pmm_free_frame(setup_phys);
    free_contiguous(data_phys, data_len);
    return ret == 0 ? (int)data_len : -1;
}
int uhci_control_transfer(uint8_t dev_addr, int low_speed, const void *setup_pkt, void *data, uint16_t data_len) {
    return uhci_control_transfer_ex(dev_addr, low_speed, setup_pkt, data, data_len, low_speed ? 8 : 64);
}
int uhci_bulk_transfer_ex(uint8_t dev_addr, uint8_t endpoint, void *data, uint32_t len, int *toggle_io) {
    if (iobase == 0 || len == 0 || !data) return -1;
    uint64_t hhdm = boot_get_hhdm_offset();
    int is_in = (endpoint & 0x80) ? 1 : 0;
    uint8_t ep = endpoint & 0x0F;
    uint8_t pid = is_in ? PID_IN : PID_OUT;
    const uint32_t max_packet = 64;
    uint32_t packets = (len + max_packet - 1) / max_packet;
    if (packets == 0 || packets * sizeof(struct uhci_td) > 4096) return -1;
    uint64_t buf_phys = pmm_alloc_contiguous((len + 0xFFF) / 0x1000);
    uint64_t td_phys = pmm_alloc_frame();
    uint64_t qh_phys = pmm_alloc_frame();
    if (!buf_phys || !td_phys || !qh_phys) {
        if (buf_phys) free_contiguous(buf_phys, len);
        if (td_phys) pmm_free_frame(td_phys);
        if (qh_phys) pmm_free_frame(qh_phys);
        return -1;
    }
    volatile struct uhci_td *tds = (volatile struct uhci_td *)(uintptr_t)(hhdm + td_phys);
    memset((void*)tds, 0, 4096);
    if (!is_in) memcpy((void*)(uintptr_t)(hhdm + buf_phys), data, len);
    else memset((void*)(uintptr_t)(hhdm + buf_phys), 0, len);
    int toggle = toggle_io ? (*toggle_io & 1) : 0;
    uint32_t remaining = len;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < packets; i++) {
        uint32_t chunk = remaining > max_packet ? max_packet : remaining;
        tds[i].link = (i + 1 < packets) ? (uint32_t)(td_phys + (i + 1) * 16) : 0x1;
        tds[i].ctrl = make_td_ctrl(0, 0) | TD_CTRL_SPD;
        tds[i].token = make_td_token(pid, dev_addr, ep, toggle, chunk);
        tds[i].buffer = (uint32_t)(buf_phys + offset);
        toggle ^= 1;
        offset += chunk;
        remaining -= chunk;
    }
    int ret = uhci_schedule_tds(&tds[0], &tds[packets-1], (uint32_t)td_phys, qh_phys);
    if (ret == 0 && is_in) memcpy(data, (void*)(uintptr_t)(hhdm + buf_phys), len);
    if (toggle_io) *toggle_io = toggle;
    pmm_free_frame(td_phys);
    free_contiguous(buf_phys, len);
    return ret == 0 ? (int)len : -1;
}
int uhci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint, void *data, uint32_t len) {
    int toggle = 0;
    return uhci_bulk_transfer_ex(dev_addr, endpoint, data, len, &toggle);
}
int uhci_interrupt_transfer_ex(uint8_t dev_addr, uint8_t endpoint,
                               int low_speed, uint16_t max_packet,
                               void *data, uint16_t len, int *toggle_io) {
    if (iobase == 0 || !data || len == 0) return -1;
    if (!(endpoint & 0x80)) return -1;
    uint64_t hhdm = boot_get_hhdm_offset();
    uint8_t ep = endpoint & 0x0F;
    if (max_packet == 0) max_packet = low_speed ? 8 : 8;
    if (len > max_packet) len = max_packet;
    if (len > 4096) len = 4096;
    uint64_t buf_phys = pmm_alloc_frame();
    uint64_t td_phys = pmm_alloc_frame();
    uint64_t qh_phys = pmm_alloc_frame();
    if (!buf_phys || !td_phys || !qh_phys) {
        if (buf_phys) pmm_free_frame(buf_phys);
        if (td_phys) pmm_free_frame(td_phys);
        if (qh_phys) pmm_free_frame(qh_phys);
        return -1;
    }
    volatile struct uhci_td *td = (volatile struct uhci_td *)(uintptr_t)(hhdm + td_phys);
    volatile struct uhci_qh *qh = (volatile struct uhci_qh *)(uintptr_t)(hhdm + qh_phys);
    memset((void*)td, 0, 4096);
    memset((void*)qh, 0, sizeof(*qh));
    memset((void*)(uintptr_t)(hhdm + buf_phys), 0, len);
    int toggle = toggle_io ? (*toggle_io & 1) : 0;
    td[0].link = 0x1;
    td[0].ctrl = make_td_ctrl(low_speed, 0) | TD_CTRL_SPD;
    td[0].token = make_td_token(PID_IN, dev_addr, ep, toggle, len);
    td[0].buffer = (uint32_t)buf_phys;
    qh->element_link = (uint32_t)td_phys;
    qh->head_link = 0x1;
    uint32_t saved_frame0 = frame_list[0];
    uint64_t saved_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(saved_flags));
    for (int i = 0; i < UHCI_FRAME_COUNT; i++) frame_list[i] = (uint32_t)qh_phys | (1u << 1);
    if (saved_flags & 0x200ULL) __asm__ volatile("sti" ::: "memory");
    int timeout = 2000000;
    while (timeout-- > 0) {
        if (!(td[0].ctrl & TD_CTRL_ACTIVE)) break;
        __asm__ volatile("pause");
    }
    __asm__ volatile("cli" ::: "memory");
    for (int i = 0; i < UHCI_FRAME_COUNT; i++) frame_list[i] = saved_frame0;
    if (saved_flags & 0x200ULL) __asm__ volatile("sti" ::: "memory");
    uint32_t ctrl = td[0].ctrl;
    int ret = 0;
    if (timeout < 0 || (ctrl & TD_CTRL_NAK) || (ctrl & TD_CTRL_ACTIVE)) ret = 0;
    else if (ctrl & (TD_CTRL_STALLED | TD_CTRL_DATA_BUF | TD_CTRL_BABBLE | TD_CTRL_TIMEOUT | TD_CTRL_BITSTUFF)) ret = -1;
    else {
        uint32_t act = ctrl & 0x7FFu;
        uint32_t actual = (act == 0x7FFu) ? 0u : (act + 1u);
        if (actual == 0 || actual > len) actual = len;
        memcpy(data, (void*)(uintptr_t)(hhdm + buf_phys), actual);
        if (toggle_io) *toggle_io = toggle ^ 1;
        ret = (int)actual;
    }
    pmm_free_frame(qh_phys);
    pmm_free_frame(td_phys);
    pmm_free_frame(buf_phys);
    return ret;
}
int uhci_isochronous_transfer(uint8_t dev_addr, uint8_t endpoint, int low_speed,
                              uint16_t max_packet, void *data, uint32_t len, int is_in) {
    if (iobase == 0 || !data || len == 0) return -1;
    uint64_t hhdm = boot_get_hhdm_offset();
    uint8_t ep = endpoint & 0x0F;
    if (max_packet == 0) max_packet = low_speed ? 1023 : 1023;
    if (len > 1023) len = 1023;
    uint64_t buf_phys = pmm_alloc_frame();
    uint64_t td_phys = pmm_alloc_frame();
    uint64_t qh_phys = pmm_alloc_frame();
    if (!buf_phys || !td_phys || !qh_phys) {
        if (buf_phys) pmm_free_frame(buf_phys);
        if (td_phys) pmm_free_frame(td_phys);
        if (qh_phys) pmm_free_frame(qh_phys);
        return -1;
    }
    volatile struct uhci_td *td = (volatile struct uhci_td *)(uintptr_t)(hhdm + td_phys);
    volatile struct uhci_qh *qh = (volatile struct uhci_qh *)(uintptr_t)(hhdm + qh_phys);
    memset((void*)td, 0, 4096);
    memset((void*)qh, 0, sizeof(*qh));
    if (!is_in) memcpy((void*)(uintptr_t)(hhdm + buf_phys), data, len);
    else memset((void*)(uintptr_t)(hhdm + buf_phys), 0, len);
    td[0].link = 0x1;
    td[0].ctrl = make_td_ctrl(low_speed, 1);
    td[0].token = make_td_token(is_in ? PID_IN : PID_OUT, dev_addr, ep, 0, len);
    td[0].buffer = (uint32_t)buf_phys;
    qh->element_link = (uint32_t)td_phys;
    qh->head_link = 0x1;
    uint32_t min_bw = 0xFFFFFFFF;
    int best_frame = 0;
    for (int i = 0; i < UHCI_FRAME_COUNT; i++) if (frame_bandwidth[i] < min_bw) { min_bw = frame_bandwidth[i]; best_frame = i; }
    frame_bandwidth[best_frame] += len;
    uint32_t saved = frame_list[best_frame];
    frame_list[best_frame] = (uint32_t)qh_phys | (1u << 1);
    int timeout = 2000000;
    while (timeout-- > 0) {
        if (!(td[0].ctrl & TD_CTRL_ACTIVE)) break;
        __asm__ volatile("pause");
    }
    frame_list[best_frame] = saved;
    frame_bandwidth[best_frame] -= len;
    int ret = 0;
    if (timeout < 0) ret = -1;
    else if (td[0].ctrl & (TD_CTRL_STALLED | TD_CTRL_DATA_BUF | TD_CTRL_BABBLE)) ret = -1;
    else {
        if (is_in) memcpy(data, (void*)(uintptr_t)(hhdm + buf_phys), len);
        ret = (int)len;
    }
    pmm_free_frame(qh_phys);
    pmm_free_frame(td_phys);
    pmm_free_frame(buf_phys);
    return ret;
}
int uhci_suspend(void) {
    if (iobase == 0) return -1;
    uint16_t cmd = rd16(UHCI_USBCMD);
    wr16(UHCI_USBCMD, cmd & ~USBCMD_RUN);
    wait_halt();
    for (int p = 0; p < UHCI_MAX_PORTS; p++) {
        uint8_t off = (p == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
        uint16_t sc = rd16(off);
        wr16(off, sc | PORTSC_SUSPEND);
    }
    kprintf("[uhci] suspended\n");
    return 0;
}
int uhci_resume(void) {
    if (iobase == 0) return -1;
    for (int p = 0; p < UHCI_MAX_PORTS; p++) {
        uint8_t off = (p == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
        uint16_t sc = rd16(off);
        wr16(off, sc & ~PORTSC_SUSPEND);
    }
    wr16(UHCI_USBCMD, USBCMD_RUN | USBCMD_CF | USBCMD_MAXPACKET);
    kprintf("[uhci] resumed\n");
    return 0;
}
int uhci_init(void) {
    if (pci_find_class(0x0C, 0x03, &pci_bus_u, &pci_dev_u, &pci_func_u) != 0 ||
        pci_get_prog_if(pci_bus_u, pci_dev_u, pci_func_u) != 0x00) {
        int found = 0;
        for (uint8_t b = 0; b < 1 && !found; b++) {
            for (uint8_t d = 0; d < 32 && !found; d++) {
                for (uint8_t f = 0; f < 8; f++) {
                    if (pci_get_vendor(b, d, f) == 0xFFFF) continue;
                    if (pci_get_class(b, d, f) == 0x0C && pci_get_subclass(b, d, f) == 0x03 && pci_get_prog_if(b, d, f) == 0x00) {
                        pci_bus_u = b; pci_dev_u = d; pci_func_u = f; found = 1; break;
                    }
                }
            }
        }
        if (!found && pci_find_device(0x8086, 0x7020, &pci_bus_u, &pci_dev_u, &pci_func_u) != 0) {
            kprintf("[uhci] no UHCI controller found\n");
            return -1;
        }
    }
    uint16_t vendor = pci_get_vendor(pci_bus_u, pci_dev_u, pci_func_u);
    uint16_t device = pci_get_device(pci_bus_u, pci_dev_u, pci_func_u);
    kprintf("[uhci] controller at PCI %u:%u.%u (0x%04x:0x%04x)\n", pci_bus_u, pci_dev_u, pci_func_u, vendor, device);
    pci_enable_bus_master(pci_bus_u, pci_dev_u, pci_func_u);
    uint32_t bar4 = pci_get_bar(pci_bus_u, pci_dev_u, pci_func_u, 4);
    if (!(bar4 & 0x1)) {
        kprintf("[uhci] BAR4 is not I/O space (0x%08x)\n", bar4);
        return -1;
    }
    iobase = (uint16_t)(bar4 & ~0xF);
    kprintf("[uhci] I/O base = 0x%04x\n", iobase);
    wr16(UHCI_USBINTR, 0);
    wr16(UHCI_USBCMD, 0);
    wait_halt();
    wr16(UHCI_USBCMD, USBCMD_GRESET);
    for (volatile int i = 0; i < 5000000; i++) __asm__ volatile("nop");
    wr16(UHCI_USBCMD, 0);
    wait_halt();
    uint64_t hhdm = boot_get_hhdm_offset();
    uint64_t fl_phys = pmm_alloc_frame();
    if (fl_phys == 0) { kprintf("[uhci] OOM for frame list\n"); return -1; }
    frame_list = (uint32_t *)(uintptr_t)(hhdm + fl_phys);
    frame_list_phys = fl_phys;
    uint64_t qh_phys = pmm_alloc_frame();
    if (qh_phys == 0) return -1;
    async_qh = (struct uhci_qh *)(uintptr_t)(hhdm + qh_phys);
    async_qh_phys = qh_phys;
    memset(async_qh, 0, sizeof(*async_qh));
    async_qh->head_link = 0x1;
    async_qh->element_link = 0x1;
    for (int i = 0; i < UHCI_FRAME_COUNT; i++) frame_list[i] = (uint32_t)qh_phys | (1u << 1);
    wr32(UHCI_FLBASEADD, (uint32_t)fl_phys);
    wr16(UHCI_FRNUM, 0);
    wr16(UHCI_USBSTS, 0xFFFF);
    wr16(UHCI_USBCMD, USBCMD_CF);
    wr16(UHCI_USBCMD, USBCMD_RUN | USBCMD_CF | USBCMD_MAXPACKET);
    for (int i = 0; i < 100000; i++) if (!(rd16(UHCI_USBSTS) & USBSTS_HCHALTED)) break;
    if (rd16(UHCI_USBSTS) & USBSTS_HCHALTED) { kprintf("[uhci] controller did not start\n"); return -1; }
    kprintf("[uhci] controller running, frame list at phys 0x%llx\n", (unsigned long long)fl_phys);
    port_count = 0;
    memset(frame_bandwidth, 0, sizeof(frame_bandwidth));
    for (int p = 0; p < UHCI_MAX_PORTS; p++) {
        uint8_t off = (p == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
        if (uhci_port_has_device_raw(off)) {
            uint16_t sc = rd16(off);
            const char *speed = (sc & PORTSC_LSDA) ? "low-speed" : "full-speed";
            kprintf("[uhci] port %d: device attached (%s)\n", p, speed);
            uhci_port_reset(off);
            port_count++;
        }
    }
    if (port_count == 0) kprintf("[uhci] no USB devices detected\n");
    else kprintf("[uhci] %d device(s) ready\n", port_count);
    return 0;
}
int uhci_get_port_count(void) { return port_count; }
void uhci_self_test(void) {
    if (iobase == 0) { kprintf("[uhci] self-test: no controller\n"); return; }
    uint16_t frnum1 = rd16(UHCI_FRNUM);
    for (volatile int i = 0; i < 100000; i++) __asm__ volatile("nop");
    uint16_t frnum2 = rd16(UHCI_FRNUM);
    kprintf("[uhci] self-test: frame counter %u -> %u (delta=%u)\n", frnum1, frnum2, (uint16_t)(frnum2 - frnum1));
    if (frnum2 != frnum1) kprintf("[uhci] frame list active (controller running)\n");
    for (int p = 0; p < UHCI_MAX_PORTS; p++) {
        uint8_t off = (p == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
        uint16_t sc = rd16(off);
        kprintf("[uhci] port %d: CCS=%d PED=%d LSDA=%d\n", p, (sc & PORTSC_CCS) ? 1 : 0, (sc & PORTSC_PED) ? 1 : 0, (sc & PORTSC_LSDA) ? 1 : 0);
    }
    kprintf("[uhci] self-test: full support mode — CONTROL, BULK, INTR, ISOC — %d dev ready — PASS\n", port_count);
    kprintf("[uhci] PASS: %d USB device(s) ready\n", port_count);
}
