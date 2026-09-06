/*
 * vmxnet3.c — VMware paravirtual NIC driver, revision 1 (RESIDUE2 T6,
 * ledger RES-46's NIC half; e1000.c is the plan's reference driver).
 *
 * Device model (per Linux drivers/net/vmxnet3/vmxnet3_defs.h, which is
 * the spec QEMU implements): two 4 KiB MMIO BARs and one shared memory
 * block the device DMAs through.
 *
 *   BAR1 "VD" (device config): VRRS @0x00, DSAL/DSAH @0x10/0x14 (shared
 *        block PA), CMD @0x20, MACL/MACH @0x28/0x30, ECR @0x40.
 *   BAR0 "PT" (passthrough):  IMR @0x00, TXPROD @0x600, RXPROD @0x800.
 *
 * The shared block carries the driver→device config (MiscConf, interrupt
 * and filter config) plus one TxQueueDesc and one RxQueueDesc, each
 * pointing at three rings: TX descriptors (driver→device, one per
 * packet, EOP|CQ), TX completions (device→driver, reclaim), RX
 * descriptors (buffer addresses the device fills) and RX completions
 * (packet arrivals).  Every ring uses the vmxnet3 generation-bit
 * convention: bit flips when the index wraps past the ring size.
 *
 * Data path: INTx with autoMask — the device masks interrupt 0 when it
 * raises; the handler drains RX completions into the software queue,
 * wakes the RX wait queue and rearms by writing IMR=0.  TX is
 * synchronous: reclaim-by-poll with a bounded wait (a full ring under
 * QEMU drains in microseconds).
 *
 * The netdev surface (send/recv/recv_wait/get_mac/link_up plus the
 * RESIDUE2 T5 rx_pop_idle idle-drain hook) matches e1000.c exactly, so
 * the IPv4/ARP/DHCP/UDP/TCP stack in net.c cannot tell the two apart.
 */

#include <stdint.h>
#include "drivers/vmxnet3/vmxnet3.h"
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

/* ---- PCI identity -------------------------------------------------- */

#define VMXNET3_VENDOR_ID   0x15AD      /* VMware */
#define VMXNET3_DEVICE_ID   0x07B0      /* vmxnet3 virtual NIC */

/* ---- BAR1 (VD) register offsets, in bytes --------------------------- */

#define VMXNET3_VD_VRRS     0x00        /* revision select: write 1 to start */
#define VMXNET3_VD_UVRS     0x08
#define VMXNET3_VD_DSAL     0x10        /* shared block PA low */
#define VMXNET3_VD_DSAH     0x18        /* shared block PA high */
#define VMXNET3_VD_CMD      0x20        /* command register (read = ret) */
#define VMXNET3_VD_MACL     0x28
#define VMXNET3_VD_MACH     0x30
#define VMXNET3_VD_ECR      0x40        /* event cause; write-back acks */

/* ---- BAR0 (PT) register offsets, in bytes --------------------------- */

#define VMXNET3_PT_IMR      0x00        /* interrupt mask: 0=unmasked */
#define VMXNET3_PT_TXPROD   0x600       /* TX producer index doorbell */
#define VMXNET3_PT_RXPROD   0x800       /* RX producer index (ring 1) */

/* ---- commands (VD_CMD) ---------------------------------------------- */

#define VMXNET3_CMD_ACTIVATE_DEV        0xCAFE0000u
#define VMXNET3_CMD_UPDATE_RX_MODE      0xCAFE0003u
#define VMXNET3_CMD_UPDATE_MAC_FILTERS  0xCAFE0004u
#define VMXNET3_CMD_GET_LINK            0xF00D0002u

/* ---- events (VD_ECR) ------------------------------------------------ */

#define VMXNET3_ECR_LINK    (1u << 2)

/* ---- ring geometry --------------------------------------------------- */

#define VMXNET3_TX_RING_SIZE    128     /* multiple of 32, per the spec */
#define VMXNET3_TX_COMP_SIZE    128
#define VMXNET3_RX_RING_SIZE    128
#define VMXNET3_RX_COMP_SIZE    256
#define VMXNET3_PKT_BUF_SIZE    2048    /* ring-1 head buffer; MTU 1500 */

/* ---- shared-memory structures (little endian, x86 native) ------------- */

struct vmxnet3_tx_desc {
    uint64_t addr;
    uint32_t dw1;               /* len:14 gen:14 oco:15 dtype:15 ext1:16  */
    uint32_t dw2;               /* hlen:10 om:12 eop:13 cq:14 ... tci:16  */
};

#define TXD_LEN(x)       ((uint32_t)(x) & 0x3FFFu)
#define TXD_GEN          (1u << 14)
#define TXD_EOP          (1u << 12)
#define TXD_CQ           (1u << 13)

struct vmxnet3_tx_comp {
    uint32_t dw[3];             /* txdIdx:12, ... device-written */
    uint32_t hdr;               /* rsvd:24 type:7 gen:1 */
};

#define TCD_TYPE_SHIFT   24
#define TCD_GEN          (1u << 31)

struct vmxnet3_rx_desc {
    uint64_t addr;
    uint32_t dw1;               /* len:14 btype:14 dtype:15 rsvd:15 gen:31 */
    uint32_t ext1;
};

#define RXD_LEN(x)       ((uint32_t)(x) & 0x3FFFu)
#define RXD_GEN          (1u << 31)

struct vmxnet3_rx_comp {
    uint32_t dw1;               /* rxdIdx:12 ext1:14 eop:15 sop:16 rqID:20 */
    uint32_t rss_hash;
    uint32_t dw3;               /* len:14 err:14 ts:15 tci:16 */
    uint32_t hdr;               /* csum:16 ... type:24 gen:31 */
};


#define RCD_EOP          (1u << 15)
#define RCD_SOP          (1u << 16)

#define RCD_ERR          (1u << 14)
#define CDTYPE_RXCOMP    3

/* Driver info / misc config — offsets matter, sizes are pinned by
 * _Static_assert below against the values Linux's layout requires. */

struct vmxnet3_misc_conf {
    uint32_t version;
    uint32_t gos;               /* gosBits:2 gosType:4 ... */
    uint32_t vmxnet3_rev_spt;
    uint32_t upt_ver_spt;
    uint64_t upt_features;
    uint64_t dd_pa;
    uint64_t queue_desc_pa;
    uint32_t dd_len;
    uint32_t queue_desc_len;
    uint32_t mtu;
    uint16_t max_num_rx_sg;
    uint8_t  num_tx_queues;
    uint8_t  num_rx_queues;
    uint32_t reserved[4];
};

struct vmxnet3_intr_conf {
    uint8_t  auto_mask;
    uint8_t  num_intrs;
    uint8_t  event_intr_idx;
    uint8_t  mod_levels[25];
    uint32_t intr_ctrl;
    uint32_t reserved[2];
};

struct vmxnet3_rx_filter_conf {
    uint32_t rx_mode;
    uint16_t mf_table_len;
    uint16_t pad1;
    uint64_t mf_table_pa;
    uint32_t vf_table[128];
};

struct vmxnet3_varlen_conf {
    uint32_t conf_ver;
    uint32_t conf_len;
    uint64_t conf_pa;
};

struct vmxnet3_ds_dev_read {
    struct vmxnet3_misc_conf      misc;
    struct vmxnet3_intr_conf      intr_conf;
    struct vmxnet3_rx_filter_conf rx_filter_conf;
    struct vmxnet3_varlen_conf    rss_conf;
    struct vmxnet3_varlen_conf    pm_conf;
    struct vmxnet3_varlen_conf    plugin_conf;
};

/* Rx filter modes */
#define VMXNET3_RXM_UCAST   0x01
#define VMXNET3_RXM_MCAST   0x02
#define VMXNET3_RXM_BCAST   0x04
#define VMXNET3_RXM_ALL_MULTI 0x08

struct vmxnet3_tx_queue_conf {
    uint64_t tx_ring_base_pa;
    uint64_t data_ring_base_pa;
    uint64_t comp_ring_base_pa;
    uint64_t dd_pa;
    uint64_t reserved;
    uint32_t tx_ring_size;
    uint32_t data_ring_size;
    uint32_t comp_ring_size;
    uint32_t dd_len;
    uint8_t  intr_idx;
    uint8_t  pad1;
    uint16_t tx_data_ring_desc_size;
    uint32_t pad2;
};

struct vmxnet3_rx_queue_conf {
    uint64_t rx_ring_base_pa[2];
    uint64_t comp_ring_base_pa;
    uint64_t dd_pa;
    uint64_t rx_data_ring_base_pa;
    uint32_t rx_ring_size[2];
    uint32_t comp_ring_size;
    uint32_t dd_len;
    uint8_t  intr_idx;
    uint8_t  pad1;
    uint16_t rx_data_ring_desc_size;
    uint32_t pad2;
};

struct vmxnet3_queue_status {
    uint8_t  stopped;
    uint8_t  pad[3];
    uint32_t error;
};

struct vmxnet3_tx_queue_ctrl {
    uint32_t tx_num_deferred;
    uint32_t tx_threshold;
    uint64_t reserved;
};

struct vmxnet3_rx_queue_ctrl {
    uint8_t  update_rx_prod;
    uint8_t  pad[7];
    uint64_t reserved;
};

/* Opaque UPT1 stat block (10 x u64) + timestamp-ring conf (16 B). */
struct vmxnet3_upt_stats { uint64_t c[10]; };

struct vmxnet3_tx_queue_desc {
    struct vmxnet3_tx_queue_ctrl  ctrl;   /*   0 */
    struct vmxnet3_tx_queue_conf  conf;   /*  16 */
    struct vmxnet3_queue_status   status; /*  80 */
    struct vmxnet3_upt_stats      stats;  /*  88 */
    uint8_t                       ts_conf[16];
    uint8_t                       pad[72];/* 184 .. 256 */
};

struct vmxnet3_rx_queue_desc {
    struct vmxnet3_rx_queue_ctrl  ctrl;   /*   0 */
    struct vmxnet3_rx_queue_conf  conf;   /*  16 */
    struct vmxnet3_queue_status   status; /*  80 */
    struct vmxnet3_upt_stats      stats;  /*  88 */
    uint8_t                       ts_conf[16];
    uint8_t                       pad[72];/* 184 .. 256 */
};

#define VMXNET3_REV1_MAGIC  3133079265u

struct vmxnet3_driver_shared {
    uint32_t magic;
    uint32_t size;
    struct vmxnet3_ds_dev_read dev_read;  /* 8 */
    uint32_t ecr;                         /* device→driver events */
    uint32_t reserved;
    uint64_t cmd_info[2];
    uint8_t  tail[96];                    /* devReadExt + slack */
};

_Static_assert(sizeof(struct vmxnet3_tx_desc) == 16, "tx desc");
_Static_assert(sizeof(struct vmxnet3_tx_comp) == 16, "tx comp");
_Static_assert(sizeof(struct vmxnet3_rx_desc) == 16, "rx desc");
_Static_assert(sizeof(struct vmxnet3_rx_comp) == 16, "rx comp");
_Static_assert(sizeof(struct vmxnet3_misc_conf) == 72, "misc conf");
_Static_assert(sizeof(struct vmxnet3_intr_conf) == 40, "intr conf");
_Static_assert(sizeof(struct vmxnet3_rx_filter_conf) == 528, "filter conf");
_Static_assert(sizeof(struct vmxnet3_tx_queue_desc) == 256, "tx qdesc");
_Static_assert(sizeof(struct vmxnet3_rx_queue_desc) == 256, "rx qdesc");

/* ---- driver state ---------------------------------------------------- */

static volatile uint32_t *vd_regs;   /* BAR1 */
static volatile uint32_t *pt_regs;   /* BAR0 */
static uint8_t  pci_bus_v, pci_dev_v, pci_func_v;
static uint8_t  vmx_irq_line = 0xFF;
static int      vmx_active;
static uint8_t  mac_addr[6];
static int      link_is_up;

static struct vmxnet3_driver_shared *shared;
static struct vmxnet3_tx_queue_desc *tq_desc;
static struct vmxnet3_rx_queue_desc *rq_desc;
static struct vmxnet3_tx_desc *tx_ring;
static struct vmxnet3_tx_comp *tx_comp;
static struct vmxnet3_rx_desc *rx_ring;
static struct vmxnet3_rx_comp *rx_comp;
static uint8_t *tx_buffers[VMXNET3_TX_RING_SIZE];
static uint8_t *rx_buffers[VMXNET3_RX_RING_SIZE];
static uint64_t tx_buf_phys[VMXNET3_TX_RING_SIZE];
static uint64_t rx_buf_phys[VMXNET3_RX_RING_SIZE];

static uint32_t tx_produce;      /* next desc slot to fill            */
static uint32_t tx_outstanding;  /* submitted, not yet reclaimed      */
static uint32_t tx_comp_next;    /* consumer index in the comp ring   */
static uint8_t  tx_comp_gen;
static uint32_t rx_fill_count;   /* total refills ever                */
static uint32_t rx_comp_next;
static uint8_t  rx_comp_gen;
static uint32_t rx_dropped, rx_errors;

/* Software RX queue + wait queue: the e1000.c contract (RESIDUE2 T5:
 * idle drain + blocking recv with the waiter count under rxq_lock). */

#define VMX3_SW_RX_QUEUE_LEN 64
struct vmx3_sw_packet {
    uint16_t len;
    uint8_t  data[VMXNET3_PKT_BUF_SIZE];
};

static struct vmx3_sw_packet sw_rx_queue[VMX3_SW_RX_QUEUE_LEN];
static uint32_t sw_rx_head, sw_rx_tail, sw_rx_count, sw_rx_drops;
static volatile int rx_waiters;
static uint32_t idle_drained;
static spinlock_t rxq_lock = SPINLOCK_UNLOCKED;
static struct wait_queue vmx_rx_wq;

/* ---- MMIO helpers ---------------------------------------------------- */

static inline void vd_write(uint32_t off, uint32_t val) {
    vd_regs[off / 4] = val;
}
static inline uint32_t vd_read(uint32_t off) {
    return vd_regs[off / 4];
}
static inline void pt_write(uint32_t off, uint32_t val) {
    pt_regs[off / 4] = val;
}
static inline uint32_t pt_read(uint32_t off) {
    return pt_regs[off / 4];
}

/* Issue a GET command; the device writes the return value into CMD. */
static uint32_t vmx_cmd(uint32_t cmd) {
    vd_write(VMXNET3_VD_CMD, cmd);
    return vd_read(VMXNET3_VD_CMD);
}

/* SET commands have no readable status (reading one back only draws a
 * QEMU "unknown command" warning) — issue and move on. */
static void vmx_cmd_set(uint32_t cmd) {
    vd_write(VMXNET3_VD_CMD, cmd);
}

static int vmxnet3_link_up(void);

/* ---- software RX queue ----------------------------------------------- */

static int sw_rx_push(const void *data, uint16_t len) {
    if (!data || len == 0) return 0;
    if (len > VMXNET3_PKT_BUF_SIZE) len = VMXNET3_PKT_BUF_SIZE;

    uint64_t flags = spinlock_acquire_irqsave(&rxq_lock);
    if (sw_rx_count >= VMX3_SW_RX_QUEUE_LEN) {
        sw_rx_drops++;
        spinlock_release_irqrestore(&rxq_lock, flags);
        return 0;
    }
    struct vmx3_sw_packet *p = &sw_rx_queue[sw_rx_tail];
    memcpy(p->data, data, len);
    p->len = len;
    sw_rx_tail = (sw_rx_tail + 1) % VMX3_SW_RX_QUEUE_LEN;
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
    struct vmx3_sw_packet *p = &sw_rx_queue[sw_rx_head];
    uint16_t len = p->len;
    if (len > bufsize) len = (uint16_t)bufsize;
    memcpy(buf, p->data, len);
    p->len = 0;
    sw_rx_head = (sw_rx_head + 1) % VMX3_SW_RX_QUEUE_LEN;
    sw_rx_count--;
    spinlock_release_irqrestore(&rxq_lock, flags);
    return (int)len;
}

/* RESIDUE2 T5 (idle RX drain): atomic check-and-pop mirroring
 * e1000_rx_pop_idle — never steals a frame from a blocking consumer. */
static int vmxnet3_rx_pop_idle(void *buf, uint32_t bufsize) {
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
    struct vmx3_sw_packet *p = &sw_rx_queue[sw_rx_head];
    uint16_t len = p->len;
    if (len > bufsize) len = (uint16_t)bufsize;
    memcpy(buf, p->data, len);
    p->len = 0;
    sw_rx_head = (sw_rx_head + 1) % VMX3_SW_RX_QUEUE_LEN;
    sw_rx_count--;
    spinlock_release_irqrestore(&rxq_lock, flags);

    idle_drained++;
    if (idle_drained == 1 || (idle_drained & 0xFF) == 0) {
        kprintf("[vmxnet3] idle drain: %u unsolicited frame(s) consumed\n",
                idle_drained);
    }
    return (int)len;
}

/* ---- RX completion processing ---------------------------------------- */

/* Refill one consumed RX descriptor and ring the doorbell.  Generation
 * follows the fill count: gen is 1 for the first pass over the ring and
 * flips each time the ring wraps (the device's expectation stays in
 * lockstep because completions arrive in ring order). */
static void rx_refill(uint32_t idx) {
    uint8_t gen = (uint8_t)(((rx_fill_count / VMXNET3_RX_RING_SIZE) & 1u) ^ 1u);
    rx_ring[idx].addr = rx_buf_phys[idx];
    rx_ring[idx].dw1 = RXD_LEN(VMXNET3_PKT_BUF_SIZE) | ((uint32_t)gen << 31);
    rx_fill_count++;
    pt_write(VMXNET3_PT_RXPROD, idx);
}

/* Walk the RX completion ring; called from the IRQ handler and from
 * init.  Returns the number of packets delivered to the sw queue. */
static int vmxnet3_rx_drain(void) {
    int delivered = 0;
    for (;;) {
        /* NOTE: QEMU's vmxnet3 never writes the completion-type field of
         * RX completions (it memsets the descriptor and fills rxdIdx/len/
         * sop/eop/gen only), so a type check would discard every frame.
         * Ring membership + the generation bit are the contract. */
        volatile struct vmxnet3_rx_comp *rcd = &rx_comp[rx_comp_next];
        uint32_t hdr = rcd->hdr;
        if (!!(hdr & TCD_GEN) != !!rx_comp_gen) break;   /* not ours yet */

        uint32_t dw1 = rcd->dw1;
        uint32_t len = rcd->dw3 & 0x3FFFu;
        int eop = (int)(dw1 & (1u << 14));
        int sop = (int)(dw1 & (1u << 15));
        int err = (rcd->dw3 & RCD_ERR) ? 1 : 0;

        if (eop && sop && !err && len <= VMXNET3_PKT_BUF_SIZE) {
            uint32_t idx = dw1 & 0xFFFu;
            if (idx < VMXNET3_RX_RING_SIZE &&
                    sw_rx_push(rx_buffers[idx], (uint16_t)len)) {
                delivered++;
            }
            rx_refill(idx);
        } else if (eop) {
            /* Multi-descriptor or errored frame: the head descriptor was
             * already refilled when its SOP completion passed through
             * (or never consumed a buffer at all) — just account it. */
            if (err) rx_errors++;
            else rx_dropped++;
        } else {
            /* SOP-only completion (frame spanning buffers): drop the
             * frame; the matching EOP completion takes the other arm. */
            rx_dropped++;
        }

        rx_comp_next++;
        if (rx_comp_next == VMXNET3_RX_COMP_SIZE) {
            rx_comp_next = 0;
            rx_comp_gen ^= 1;
        }
    }
    return delivered;
}

/* ---- TX reclaim / send ------------------------------------------------ */

static void tx_reclaim(void) {
    for (;;) {
        volatile struct vmxnet3_tx_comp *tcd = &tx_comp[tx_comp_next];
        uint32_t hdr = tcd->hdr;
        if (!!(hdr & TCD_GEN) != !!tx_comp_gen) break;
        if (tx_outstanding == 0) break;

        tx_outstanding--;
        tx_comp_next++;
        if (tx_comp_next == VMXNET3_TX_COMP_SIZE) {
            tx_comp_next = 0;
            tx_comp_gen ^= 1;
        }
    }
}

static int vmxnet3_send(const void *data, uint32_t len) {
    if (!vmx_active || !data || len == 0 || len > VMXNET3_PKT_BUF_SIZE)
        return -1;

    /* Make sure a descriptor is free; completions land within
     * microseconds under QEMU, so poll with a bounded wait. */
    uint64_t give_up = timer_get_ticks() + 100;   /* ~1 s at 100 Hz */
    for (;;) {
        tx_reclaim();
        if (tx_outstanding < VMXNET3_TX_RING_SIZE) break;
        if (timer_get_ticks() >= give_up) {
            kprintf("[vmxnet3] TX ring stuck (outstanding=%u)\n",
                    tx_outstanding);
            return -1;
        }
        arch_cpu_relax();
    }

    uint32_t idx = tx_produce % VMXNET3_TX_RING_SIZE;
    uint8_t gen = (uint8_t)(((tx_produce / VMXNET3_TX_RING_SIZE) & 1u) ^ 1u);

    memcpy(tx_buffers[idx], data, len);
    arch_compiler_barrier();
    tx_ring[idx].addr = tx_buf_phys[idx];
    tx_ring[idx].dw1 = TXD_LEN(len) | ((uint32_t)gen << 14);
    tx_ring[idx].dw2 = TXD_EOP | TXD_CQ;
    arch_compiler_barrier();
    pt_write(VMXNET3_PT_TXPROD, idx);

    tx_produce++;
    tx_outstanding++;
    return (int)len;
}

/* ---- netdev surface --------------------------------------------------- */

static int vmxnet3_recv(void *buf, uint32_t bufsize) {
    if (!vmx_active) return 0;
    vmxnet3_rx_drain();
    return sw_rx_pop(buf, bufsize);
}

static int vmxnet3_recv_wait(void *buf, uint32_t bufsize,
                             uint64_t timeout_ticks) {
    if (!vmx_active) return -1;
    uint64_t start = timer_get_ticks();
    uint64_t deadline = timeout_ticks ? start + timeout_ticks : 0;
    tcb_t *cur = sched_current();
    uint64_t old_sleep_deadline = cur ? cur->sleep_deadline : 0;

    /* RESIDUE2 T5: mark this consumer before the first pop, under the
     * same lock the idle drain checks (see vmxnet3_rx_pop_idle). */
    {
        uint64_t flags = spinlock_acquire_irqsave(&rxq_lock);
        rx_waiters++;
        spinlock_release_irqrestore(&rxq_lock, flags);
    }

    int n;
    for (;;) {
        vmxnet3_rx_drain();
        n = sw_rx_pop(buf, bufsize);
        if (n != 0) break;
        if (!vmxnet3_link_up()) { n = -1; break; }
        if (deadline && timer_get_ticks() >= deadline) { n = 0; break; }

        if (!cur) {
            arch_cpu_relax();
            continue;
        }
        if (deadline) cur->sleep_deadline = deadline;
        wq_wait(&vmx_rx_wq, NULL);
        if (deadline) cur->sleep_deadline = old_sleep_deadline;
    }

    {
        uint64_t flags = spinlock_acquire_irqsave(&rxq_lock);
        rx_waiters--;
        spinlock_release_irqrestore(&rxq_lock, flags);
    }
    return n;
}

static void vmxnet3_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = mac_addr[i];
}

static int vmxnet3_link_up(void) {
    if (!vmx_active) return 0;
    /* GET_LINK returns speed<<16 | status; bit 0 is up (QEMU reports
     * 1000 Mbps, ESXi up to 10 Gbps — do not compare the speed field). */
    uint32_t r = vmx_cmd(VMXNET3_CMD_GET_LINK);
    link_is_up = (r & 1u) ? 1 : 0;
    return link_is_up;
}

/* ---- interrupt --------------------------------------------------------- */

static void vmxnet3_irq_handler(struct registers *regs) {
    (void)regs;
    if (!vmx_active) return;

    /* Ack device events (link changes); write-back clears them. */
    uint32_t ecr = vd_read(VMXNET3_VD_ECR);
    if (ecr) vd_write(VMXNET3_VD_ECR, ecr);

    int delivered = vmxnet3_rx_drain();
    tx_reclaim();
    if (delivered > 0) wq_wake_all(&vmx_rx_wq);

    /* autoMask raised the mask when the interrupt fired: rearm. */
    pt_write(VMXNET3_PT_IMR, 0);
}

/* ---- init --------------------------------------------------------------- */

static int vmxnet3_map_bar(int bar, volatile uint32_t **out) {
    uint32_t raw = pci_get_bar(pci_bus_v, pci_dev_v, pci_func_v, bar);
    if ((raw & 0x6) == 0x4) return -1;         /* 64-bit BAR: unsupported */
    uint64_t phys = raw & ~0xFu;
    uint64_t hhdm = boot_get_hhdm_offset();
    for (uint32_t off = 0; off < 0x1000; off += 0x1000) {
        paging_map(hhdm + phys + off, phys + off, PAGE_FLAGS_MMIO);
    }
    *out = (volatile uint32_t *)(uintptr_t)(hhdm + phys);
    return 0;
}

int vmxnet3_init(void) {
    if (pci_find_device(VMXNET3_VENDOR_ID, VMXNET3_DEVICE_ID,
                        &pci_bus_v, &pci_dev_v, &pci_func_v) != 0) {
        return -1;
    }
    kprintf("[vmxnet3] found at %02x:%02x.%x\n",
            pci_bus_v, pci_dev_v, pci_func_v);

    pci_enable_bus_master(pci_bus_v, pci_dev_v, pci_func_v);
    uint32_t cmd = pci_config_read(pci_bus_v, pci_dev_v, pci_func_v, 0x04);
    pci_config_write(pci_bus_v, pci_dev_v, pci_func_v, 0x04,
                     cmd & ~(1u << 10));       /* INTx enabled */

    /* BAR0 = PT (doorbells/IMR), BAR1 = VD (config). */
    if (vmxnet3_map_bar(0, &pt_regs) != 0) return -1;
    if (vmxnet3_map_bar(1, &vd_regs) != 0) return -1;

    /* Revision handshake: writing 1 selects device revision 1 and
     * resets the emulation; read-back proves the device is alive. */
    vd_write(VMXNET3_VD_VRRS, 1);
    uint32_t rev = vd_read(VMXNET3_VD_VRRS);
    if (rev < 1) {
        kprintf("[vmxnet3] FAIL: VRRS readback %u\n", rev);
        return -1;
    }

    /* Mask interrupts while the rings come up. */
    pt_write(VMXNET3_PT_IMR, 1);

    /* Shared block: driver shared + queue descriptors + all four rings
     * carved out of one physically-contiguous PMM run (the device DMAs
     * through these — HHDM access, physical addresses in the config). */
    uint64_t hhdm = boot_get_hhdm_offset();
    enum { PG_SHARED = 0, PG_QDESC, PG_TXR, PG_TXC, PG_RXR, PG_RXC, PG_TOTAL };
    uint64_t base = pmm_alloc_contiguous(PG_TOTAL);
    if (!base) return -1;
    memset((void *)(uintptr_t)(hhdm + base), 0, PG_TOTAL * 0x1000);

    shared  = (struct vmxnet3_driver_shared *)(uintptr_t)(hhdm + base);
    tq_desc = (struct vmxnet3_tx_queue_desc *)(uintptr_t)(hhdm + base +
                                                          PG_QDESC * 0x1000);
    rq_desc = (struct vmxnet3_rx_queue_desc *)(uintptr_t)(hhdm + base +
                                                          PG_QDESC * 0x1000 +
                                                          256);
    tx_ring = (struct vmxnet3_tx_desc *)(uintptr_t)(hhdm + base +
                                                    PG_TXR * 0x1000);
    tx_comp = (struct vmxnet3_tx_comp *)(uintptr_t)(hhdm + base +
                                                    PG_TXC * 0x1000);
    rx_ring = (struct vmxnet3_rx_desc *)(uintptr_t)(hhdm + base +
                                                    PG_RXR * 0x1000);
    rx_comp = (struct vmxnet3_rx_comp *)(uintptr_t)(hhdm + base +
                                                    PG_RXC * 0x1000);

    /* Packet buffers (one frame each, PMM-backed). */
    for (int i = 0; i < VMXNET3_TX_RING_SIZE; i++) {
        tx_buf_phys[i] = pmm_alloc_frame();
        tx_buffers[i] = (uint8_t *)(uintptr_t)(hhdm + tx_buf_phys[i]);
    }
    for (int i = 0; i < VMXNET3_RX_RING_SIZE; i++) {
        rx_buf_phys[i] = pmm_alloc_frame();
        rx_buffers[i] = (uint8_t *)(uintptr_t)(hhdm + rx_buf_phys[i]);
    }

    /* Driver-shared config. */
    shared->magic = VMXNET3_REV1_MAGIC;
    shared->size = (uint32_t)sizeof(*shared);
    shared->dev_read.misc.version = 1;
    shared->dev_read.misc.gos = 2 | (1u << 2);   /* 64-bit, Linux-ish GOS */
    shared->dev_read.misc.vmxnet3_rev_spt = 1;
    shared->dev_read.misc.upt_ver_spt = 1;
    shared->dev_read.misc.dd_pa = base;
    shared->dev_read.misc.dd_len = (uint32_t)sizeof(*shared);
    shared->dev_read.misc.queue_desc_pa = base + PG_QDESC * 0x1000;
    shared->dev_read.misc.queue_desc_len = 512;
    shared->dev_read.misc.mtu = 1500;
    shared->dev_read.misc.num_tx_queues = 1;
    shared->dev_read.misc.num_rx_queues = 1;

    shared->dev_read.intr_conf.auto_mask = 1;
    shared->dev_read.intr_conf.num_intrs = 1;
    shared->dev_read.intr_conf.event_intr_idx = 0;
    shared->dev_read.intr_conf.mod_levels[0] = 0;   /* no moderation */
    shared->dev_read.intr_conf.intr_ctrl = 0;

    /* ALL_MULTI keeps IPv6 NDP group traffic flowing, like e1000's MPE. */
    shared->dev_read.rx_filter_conf.rx_mode =
        VMXNET3_RXM_UCAST | VMXNET3_RXM_BCAST | VMXNET3_RXM_ALL_MULTI;

    tq_desc->conf.tx_ring_base_pa = base + PG_TXR * 0x1000;
    tq_desc->conf.data_ring_base_pa = base + PG_TXR * 0x1000; /* unused */
    tq_desc->conf.comp_ring_base_pa = base + PG_TXC * 0x1000;
    tq_desc->conf.dd_pa = base;
    tq_desc->conf.dd_len = 512;
    tq_desc->conf.tx_ring_size = VMXNET3_TX_RING_SIZE;
    tq_desc->conf.data_ring_size = 0;
    tq_desc->conf.comp_ring_size = VMXNET3_TX_COMP_SIZE;
    tq_desc->conf.intr_idx = 0;
    tq_desc->conf.tx_data_ring_desc_size = 128;

    rq_desc->ctrl.update_rx_prod = 1;
    rq_desc->conf.rx_ring_base_pa[0] = base + PG_RXR * 0x1000;
    rq_desc->conf.comp_ring_base_pa = base + PG_RXC * 0x1000;
    rq_desc->conf.dd_pa = base;
    rq_desc->conf.dd_len = 512;
    rq_desc->conf.rx_ring_size[0] = VMXNET3_RX_RING_SIZE;
    rq_desc->conf.comp_ring_size = VMXNET3_RX_COMP_SIZE;
    rq_desc->conf.intr_idx = 0;

    vd_write(VMXNET3_VD_DSAL, (uint32_t)(base & 0xFFFFFFFFu));
    vd_write(VMXNET3_VD_DSAH, (uint32_t)(base >> 32));

    /* Completion rings start at generation 1 (VMXNET3_INIT_GEN). */
    tx_comp_gen = 1;
    rx_comp_gen = 1;

    /* Fill every RX descriptor once (pass 0: gen=1), doorbell = last. */
    rx_fill_count = 0;
    for (uint32_t i = 0; i < VMXNET3_RX_RING_SIZE; i++) rx_refill(i);

    /* Read-back: 0 means the device came up active, 1 means refused. */
    uint32_t act = vmx_cmd(VMXNET3_CMD_ACTIVATE_DEV);
    if (act != 0) {
        kprintf("[vmxnet3] FAIL: activate refused (%u)\n", act);
        return -1;
    }
    kprintf("[vmxnet3] device activated (rev %u)\n", rev);
    vmx_cmd_set(VMXNET3_CMD_UPDATE_RX_MODE);
    vmx_cmd_set(VMXNET3_CMD_UPDATE_MAC_FILTERS);

    uint32_t macl = vd_read(VMXNET3_VD_MACL);
    uint32_t mach = vd_read(VMXNET3_VD_MACH) & 0xFFFF;
    mac_addr[0] = (uint8_t)macl;
    mac_addr[1] = (uint8_t)(macl >> 8);
    mac_addr[2] = (uint8_t)(macl >> 16);
    mac_addr[3] = (uint8_t)(macl >> 24);
    mac_addr[4] = (uint8_t)mach;
    mac_addr[5] = (uint8_t)(mach >> 8);
    kprintf("[vmxnet3] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5]);

    /* vmx_active is still 0 here, so query the command directly. */
    link_is_up = (int)(vmx_cmd(VMXNET3_CMD_GET_LINK) & 1u);
    kprintf("[vmxnet3] link %s\n", link_is_up ? "up" : "down");

    wq_init(&vmx_rx_wq);
    vmx_irq_line = pci_get_interrupt_line(pci_bus_v, pci_dev_v, pci_func_v);
    irq_register_handler((int)vmx_irq_line, vmxnet3_irq_handler);
    kprintf("[vmxnet3] IRQ line %u enabled\n", vmx_irq_line);
    pt_write(VMXNET3_PT_IMR, 0);

    kprintf("[vmxnet3] TX/RX rings initialised (tx=%u/%u rx=%u/%u)\n",
            VMXNET3_TX_RING_SIZE, VMXNET3_TX_COMP_SIZE,
            VMXNET3_RX_RING_SIZE, VMXNET3_RX_COMP_SIZE);
    vmx_active = 1;
    return 0;
}

/* ---- netdev registration ---------------------------------------------- */

static const struct netdev vmxnet3_netdev = {
    .name        = "vmxnet3",
    .send        = vmxnet3_send,
    .recv        = vmxnet3_recv,
    .recv_wait   = vmxnet3_recv_wait,
    .get_mac     = vmxnet3_get_mac,
    .link_up     = vmxnet3_link_up,
    .rx_pop_idle = vmxnet3_rx_pop_idle,
};

void vmxnet3_register_netdev(void) {
    netdev_register(&vmxnet3_netdev);
}
