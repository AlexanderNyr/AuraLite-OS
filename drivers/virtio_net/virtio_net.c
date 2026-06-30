/* virtio_net.c — modern virtio-net PCI driver (data path).
 *
 * Brings up a modern virtio-net PCI device (1af4:1041, or the transitional
 * 1af4:1000 advertised through modern capabilities), negotiates
 * VIRTIO_F_VERSION_1, and configures two virtqueues: queue 0 (RX) and queue 1
 * (TX).  RX is prefilled with receive buffers up front.  Frames are exchanged
 * with a 10-byte virtio_net_hdr (MRG_RXBUF is intentionally not negotiated so
 * the header size is fixed).
 *
 * This is a polling data path matching the boot-time networking stack: there is
 * no allocation and no protocol parsing in IRQ context.  The MAC address is read
 * from the device configuration space when VIRTIO_NET_F_MAC is offered.
 */

#include <stdint.h>
#include <stddef.h>
#include "drivers/virtio_net/virtio_net.h"
#include "drivers/pci/pci.h"
#include "kernel/net/netdev.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/limine_requests.h"
#include "kernel/mm/pmm.h"
#include "drivers/timer/pit.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define VIRTIO_VENDOR_ID         0x1AF4
#define VIRTIO_NET_MODERN_DEVICE 0x1041
#define VIRTIO_NET_TRANS_DEVICE  0x1000

/* PCI capability IDs. */
#define PCI_STATUS_CAP_LIST 0x10
#define PCI_CAP_VENDOR      0x09

/* virtio PCI capability cfg_type values. */
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

/* Device status bits. */
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED       128

/* Feature bits. */
#define VIRTIO_NET_F_MAC        5
#define VIRTIO_NET_F_MRG_RXBUF  15
#define VIRTIO_F_VERSION_1      32

/* Virtqueue descriptor flags. */
#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

/* Queue assignment for a device without MQ/CTRL_VQ. */
#define VNET_RXQ 0
#define VNET_TXQ 1

#define VNET_Q_SIZE   16    /* ring slots we will use (clamped to queue_size) */
#define VNET_BUF_SIZE 2048  /* per-buffer size: 12-byte hdr + up to 1518 frame */
/* Under VIRTIO_F_VERSION_1 the virtio_net_hdr always includes the num_buffers
 * field, so its size is 12 bytes even when MRG_RXBUF is not negotiated. */
#define VNET_HDR_LEN  12

struct virtio_pci_common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __attribute__((packed));

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VNET_Q_SIZE];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VNET_Q_SIZE];
} __attribute__((packed));

/* virtio_net_hdr.  Under VIRTIO_F_VERSION_1 num_buffers is always present
 * (12 bytes total), independent of MRG_RXBUF, for both TX and RX. */
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} __attribute__((packed));

/* Per-queue state. */
struct vnet_queue {
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint64_t            desc_phys, avail_phys, used_phys;
    uint16_t            qsize;
    uint16_t            last_used_idx;
    uint16_t            notify_off;
    /* Backing buffers: one page-aligned slab per descriptor. */
    uint8_t            *buf[VNET_Q_SIZE];
    uint64_t            buf_phys[VNET_Q_SIZE];
};

static int init_attempted;
static int init_result = -1;
static int present;

static uint8_t pci_bus, pci_dev, pci_func;
static volatile struct virtio_pci_common_cfg *common_cfg;
static volatile uint8_t *notify_base;
static uint32_t notify_multiplier;
static volatile uint8_t *isr_cfg;
static volatile uint8_t *device_cfg;

static struct vnet_queue rxq;
static struct vnet_queue txq;
static uint8_t mac[6];
static int have_mac;

static uint16_t pci_read16_at(uint8_t off) {
    uint32_t v = pci_config_read(pci_bus, pci_dev, pci_func, off & 0xFC);
    return (uint16_t)(v >> ((off & 2) * 8));
}

static uint8_t pci_read8_at(uint8_t off) {
    uint32_t v = pci_config_read(pci_bus, pci_dev, pci_func, off & 0xFC);
    return (uint8_t)(v >> ((off & 3) * 8));
}

static uint64_t map_bar_region(uint8_t bar, uint32_t offset, uint32_t length) {
    uint32_t raw = pci_get_bar(pci_bus, pci_dev, pci_func, bar);
    if (raw == 0 || raw == 0xFFFFFFFF || (raw & 1)) return 0;
    uint64_t phys = (uint64_t)(raw & ~0xFULL) + offset;
    uint64_t hhdm = limine_get_hhdm_offset();
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = (phys + length + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t p = start; p < end; p += 0x1000) {
        paging_map(hhdm + p, p, PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE);
    }
    return hhdm + phys;
}

static int parse_virtio_caps(void) {
    uint16_t status = pci_read16_at(0x06);
    if (!(status & PCI_STATUS_CAP_LIST)) return -1;
    uint8_t cap = pci_read8_at(0x34) & 0xFC;
    int guard = 0;
    while (cap && guard++ < 64) {
        uint8_t cap_vndr = pci_read8_at(cap + 0);
        uint8_t cap_next = pci_read8_at(cap + 1) & 0xFC;
        if (cap_vndr == PCI_CAP_VENDOR) {
            uint8_t cfg_type = pci_read8_at(cap + 3);
            uint8_t bar = pci_read8_at(cap + 4);
            uint32_t off = pci_config_read(pci_bus, pci_dev, pci_func, cap + 8);
            uint32_t len = pci_config_read(pci_bus, pci_dev, pci_func, cap + 12);
            uint64_t va = map_bar_region(bar, off, len ? len : 0x1000);
            if (va) {
                if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                    common_cfg = (volatile struct virtio_pci_common_cfg *)(uintptr_t)va;
                } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    notify_base = (volatile uint8_t *)(uintptr_t)va;
                    notify_multiplier = pci_config_read(pci_bus, pci_dev, pci_func, cap + 16);
                } else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG) {
                    isr_cfg = (volatile uint8_t *)(uintptr_t)va;
                } else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                    device_cfg = (volatile uint8_t *)(uintptr_t)va;
                }
            }
        }
        cap = cap_next;
    }
    return (common_cfg && notify_base) ? 0 : -1;
}

static uint64_t alloc_zero_page(void **virt_out) {
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    void *virt = (void *)(uintptr_t)(limine_get_hhdm_offset() + phys);
    memset(virt, 0, 4096);
    if (virt_out) *virt_out = virt;
    return phys;
}

static uint64_t read_device_features(void) {
    common_cfg->device_feature_select = 0;
    uint32_t lo = common_cfg->device_feature;
    common_cfg->device_feature_select = 1;
    uint32_t hi = common_cfg->device_feature;
    return ((uint64_t)hi << 32) | lo;
}

static void write_driver_features(uint64_t f) {
    common_cfg->driver_feature_select = 0;
    common_cfg->driver_feature = (uint32_t)f;
    common_cfg->driver_feature_select = 1;
    common_cfg->driver_feature = (uint32_t)(f >> 32);
}

/* Allocate the three ring structures plus per-descriptor backing buffers. */
static int setup_queue(struct vnet_queue *q, uint16_t qidx) {
    memset(q, 0, sizeof(*q));
    common_cfg->queue_select = qidx;
    uint16_t qsz = common_cfg->queue_size;
    if (qsz == 0) return -1;
    if (qsz > VNET_Q_SIZE) qsz = VNET_Q_SIZE;
    q->qsize = qsz;

    q->desc_phys  = alloc_zero_page((void **)&q->desc);
    q->avail_phys = alloc_zero_page((void **)&q->avail);
    q->used_phys  = alloc_zero_page((void **)&q->used);
    if (!q->desc_phys || !q->avail_phys || !q->used_phys) return -1;

    /* One page per backing buffer keeps each buffer physically contiguous and
     * page-aligned (VNET_BUF_SIZE <= 4096). */
    for (uint16_t i = 0; i < qsz; i++) {
        q->buf_phys[i] = alloc_zero_page((void **)&q->buf[i]);
        if (!q->buf_phys[i]) return -1;
    }

    common_cfg->queue_select = qidx;
    common_cfg->queue_size = qsz;
    common_cfg->queue_desc = q->desc_phys;
    common_cfg->queue_driver = q->avail_phys;
    common_cfg->queue_device = q->used_phys;
    common_cfg->queue_enable = 1;
    q->notify_off = common_cfg->queue_notify_off;
    q->last_used_idx = q->used->idx;
    return 0;
}

static void notify_queue(struct vnet_queue *q, uint16_t qidx) {
    volatile uint16_t *qn = (volatile uint16_t *)(uintptr_t)
        (notify_base + (uint32_t)q->notify_off * notify_multiplier);
    *qn = qidx;
}

/* Prefill the RX queue: every descriptor points at a device-writable buffer. */
static void rx_fill(void) {
    for (uint16_t i = 0; i < rxq.qsize; i++) {
        rxq.desc[i].addr = rxq.buf_phys[i];
        rxq.desc[i].len = VNET_BUF_SIZE;
        rxq.desc[i].flags = VRING_DESC_F_WRITE;
        rxq.desc[i].next = 0;
        rxq.avail->ring[i] = i;
    }
    __asm__ volatile ("mfence" ::: "memory");
    rxq.avail->idx = rxq.qsize;
    __asm__ volatile ("mfence" ::: "memory");
    notify_queue(&rxq, VNET_RXQ);
}

int virtio_net_init(void) {
    if (init_attempted) return init_result;
    init_attempted = 1;
    init_result = -1;

    uint16_t devid = VIRTIO_NET_MODERN_DEVICE;
    if (pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_NET_MODERN_DEVICE,
                        &pci_bus, &pci_dev, &pci_func) != 0) {
        if (pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_NET_TRANS_DEVICE,
                            &pci_bus, &pci_dev, &pci_func) != 0) {
            return -1;
        }
        devid = VIRTIO_NET_TRANS_DEVICE;
    }
    present = 1;
    kprintf("[virtio-net] found device %04x:%04x at PCI %u:%u.%u\n",
            VIRTIO_VENDOR_ID, devid, pci_bus, pci_dev, pci_func);

    pci_enable_bus_master(pci_bus, pci_dev, pci_func);
    if (parse_virtio_caps() != 0) {
        kprintf("[virtio-net] modern PCI capabilities not found\n");
        return -1;
    }

    common_cfg->device_status = 0;
    __asm__ volatile ("mfence" ::: "memory");
    common_cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    common_cfg->device_status |= VIRTIO_STATUS_DRIVER;

    uint64_t dev_features = read_device_features();
    if (!(dev_features & (1ULL << VIRTIO_F_VERSION_1))) {
        kprintf("[virtio-net] device lacks VIRTIO_F_VERSION_1\n");
        common_cfg->device_status |= VIRTIO_STATUS_FAILED;
        return -1;
    }
    /* Negotiate VERSION_1 and MAC only.  Explicitly avoid MRG_RXBUF so the RX
     * header stays a fixed 10 bytes. */
    uint64_t wanted = (1ULL << VIRTIO_F_VERSION_1);
    if (dev_features & (1ULL << VIRTIO_NET_F_MAC)) {
        wanted |= (1ULL << VIRTIO_NET_F_MAC);
    }
    write_driver_features(wanted);

    common_cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
    if (!(common_cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("[virtio-net] FEATURES_OK rejected\n");
        common_cfg->device_status |= VIRTIO_STATUS_FAILED;
        return -1;
    }

    /* MAC lives at the start of the virtio-net device config space. */
    if ((wanted & (1ULL << VIRTIO_NET_F_MAC)) && device_cfg) {
        for (int i = 0; i < 6; i++) mac[i] = device_cfg[i];
        have_mac = 1;
    } else {
        /* Fallback: a locally-administered address. */
        mac[0] = 0x52; mac[1] = 0x54; mac[2] = 0x00;
        mac[3] = 0x12; mac[4] = 0x34; mac[5] = 0x56;
        have_mac = 1;
    }

    if (setup_queue(&rxq, VNET_RXQ) != 0) {
        kprintf("[virtio-net] RX queue setup failed\n");
        common_cfg->device_status |= VIRTIO_STATUS_FAILED;
        return -1;
    }
    if (setup_queue(&txq, VNET_TXQ) != 0) {
        kprintf("[virtio-net] TX queue setup failed\n");
        common_cfg->device_status |= VIRTIO_STATUS_FAILED;
        return -1;
    }

    common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;

    /* RX must be filled after DRIVER_OK so the device may consume buffers. */
    rx_fill();

    kprintf("[virtio-net] ready: MAC %02x:%02x:%02x:%02x:%02x:%02x rxq=%u txq=%u\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            rxq.qsize, txq.qsize);

    init_result = 0;
    return init_result;
}

int virtio_net_available(void) {
    return init_attempted && init_result == 0;
}

void virtio_net_get_mac(uint8_t out[6]) {
    if (have_mac) memcpy(out, mac, 6);
    else memset(out, 0, 6);
}

int virtio_net_link_up(void) {
    /* Without VIRTIO_NET_F_STATUS we assume the link is up once DRIVER_OK. */
    return virtio_net_available();
}

int virtio_net_send(const void *data, uint32_t len) {
    if (!virtio_net_available()) return -1;
    if (len == 0 || len > VNET_BUF_SIZE - VNET_HDR_LEN) return -1;

    /* Round-robin through the TX descriptors. */
    static uint16_t tx_next;
    uint16_t slot = tx_next % txq.qsize;
    tx_next = (uint16_t)(tx_next + 1);

    uint8_t *buf = txq.buf[slot];
    struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)buf;
    memset(hdr, 0, VNET_HDR_LEN);
    memcpy(buf + VNET_HDR_LEN, data, len);

    txq.desc[slot].addr = txq.buf_phys[slot];
    txq.desc[slot].len = VNET_HDR_LEN + len;
    txq.desc[slot].flags = 0; /* device-readable */
    txq.desc[slot].next = 0;

    uint16_t aidx = txq.avail->idx;
    txq.avail->ring[aidx % txq.qsize] = slot;
    __asm__ volatile ("mfence" ::: "memory");
    txq.avail->idx = (uint16_t)(aidx + 1);
    __asm__ volatile ("mfence" ::: "memory");
    notify_queue(&txq, VNET_TXQ);

    /* Wait for the device to consume the descriptor (TX completion). */
    int timeout = 10000000;
    while (txq.used->idx == txq.last_used_idx && timeout-- > 0) {
        __asm__ volatile ("pause");
    }
    if (timeout <= 0) {
        kprintf("[virtio-net] TX timeout\n");
        return -1;
    }
    __asm__ volatile ("mfence" ::: "memory");
    txq.last_used_idx = txq.used->idx;
    if (isr_cfg) (void)*isr_cfg;
    return (int)len;
}

/* Non-blocking RX: pop one completed buffer from the used ring, strip the
 * virtio_net_hdr, copy the frame out, and recycle the descriptor. */
int virtio_net_recv(void *out, uint32_t bufsize) {
    if (!virtio_net_available()) return 0;

    __asm__ volatile ("mfence" ::: "memory");
    if (rxq.used->idx == rxq.last_used_idx) return 0;

    struct vring_used_elem *e =
        &rxq.used->ring[rxq.last_used_idx % rxq.qsize];
    uint16_t id = (uint16_t)(e->id % rxq.qsize);
    uint32_t total = e->len;

    int frame_len = 0;
    if (total > VNET_HDR_LEN) {
        uint32_t flen = total - VNET_HDR_LEN;
        if (flen > bufsize) flen = bufsize;
        memcpy(out, rxq.buf[id] + VNET_HDR_LEN, flen);
        frame_len = (int)flen;
    }

    /* Recycle this descriptor back onto the avail ring. */
    rxq.desc[id].addr = rxq.buf_phys[id];
    rxq.desc[id].len = VNET_BUF_SIZE;
    rxq.desc[id].flags = VRING_DESC_F_WRITE;
    rxq.desc[id].next = 0;
    uint16_t aidx = rxq.avail->idx;
    rxq.avail->ring[aidx % rxq.qsize] = id;
    __asm__ volatile ("mfence" ::: "memory");
    rxq.avail->idx = (uint16_t)(aidx + 1);
    __asm__ volatile ("mfence" ::: "memory");

    rxq.last_used_idx = (uint16_t)(rxq.last_used_idx + 1);
    notify_queue(&rxq, VNET_RXQ);
    if (isr_cfg) (void)*isr_cfg;
    return frame_len;
}

int virtio_net_recv_wait(void *out, uint32_t bufsize, uint64_t timeout_ticks) {
    if (!virtio_net_available()) return -1;
    uint64_t start = timer_get_ticks();
    uint64_t deadline = timeout_ticks ? start + timeout_ticks : 0;

    for (;;) {
        int n = virtio_net_recv(out, bufsize);
        if (n != 0) return n;
        if (!virtio_net_link_up()) return -1;
        if (deadline && timer_get_ticks() >= deadline) return 0;
        __asm__ volatile ("pause");
    }
}

/* ---- netdev backend registration ---------------------------------------- */

static const struct netdev virtio_net_netdev = {
    .name      = "virtio-net",
    .send      = virtio_net_send,
    .recv      = virtio_net_recv,
    .recv_wait = virtio_net_recv_wait,
    .get_mac   = virtio_net_get_mac,
    .link_up   = virtio_net_link_up,
};

void virtio_net_register_netdev(void) {
    netdev_register(&virtio_net_netdev);
}
