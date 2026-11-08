/* nvme.c — NVMe over PCIe, polled, one namespace (RESIDUE3 C7b).
 *
 * Read drivers/nvme/nvme.h first: it states the scope, and the omissions are
 * deliberate.  This file is the hardware half — MMIO, DMA frames, doorbells,
 * a bounded poll — while the encoding lives in nvme_proto.h where a host test
 * can pin it.
 *
 * THE ORDER OF BRING-UP IS THE SPEC'S ORDER, and it is not optional:
 *
 *   1. CC.EN = 0, wait CSTS.RDY = 0        (reset before touching queues)
 *   2. allocate and zero the admin SQ/CQ, write AQA/ASQ/ACQ
 *   3. CC.EN = 1, wait CSTS.RDY = 1        (controller is now using the queues)
 *   4. Identify Controller, Identify Namespace
 *   5. Create one I/O CQ, then one I/O SQ bound to it
 *   6. only then may an I/O command be submitted
 *
 * A step that times out reports WHICH step and what it saw.  A driver that
 * prints "NVMe init failed" tells a reader nothing at 3 a.m.
 */

#include "drivers/nvme/nvme.h"

#include <stdint.h>

#include "drivers/nvme/nvme_proto.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/arch.h"
#include "kernel/boot_info.h"
#include "kernel/fs/blkdev.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/paddr.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/atomic_compat.h"   /* SELFHOST: __sync_synchronize */
#include "kernel/lib/string.h"
#include "kernel/mm/pmm.h"

/* ---- Constants -------------------------------------------------------- */

#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_NVME      0x08
#define PCI_PROGIF_NVME        0x02

#define NVME_ADMIN_QID   0u
#define NVME_IO_QID      1u
#define NVME_ADMIN_QSIZE 8u
#define NVME_IO_QSIZE    8u

#define NVME_PAGE        4096u
#define NVME_BOUNCE      NVME_PAGE
#define NVME_MAX_SECTORS_PER_CMD (NVME_BOUNCE / BLKDEV_SECTOR_SIZE)

/* Poll budget.  QEMU answers in microseconds; 2,000,000 iterations of a
 * volatile read is generous enough that a timeout means "this controller is
 * not coming back", not "the host is slow". */
#define NVME_POLL_LIMIT  2000000u
#define NVME_READY_LIMIT 2000000u

/* ---- State ----------------------------------------------------------- */

typedef struct {
    uint8_t  up;
    uint8_t  bus, dev, func;
    volatile uint8_t *regs;          /* BAR0 through the HHDM */
    uint64_t cap;
    uint32_t dstrd;
    uint32_t mqes;

    /* Admin queue pair. */
    uint8_t *asq;  uint64_t asq_phys;
    uint8_t *acq;  uint64_t acq_phys;
    uint32_t asq_tail, acq_head;
    uint8_t  acq_phase;

    /* I/O queue pair. */
    uint8_t *iosq; uint64_t iosq_phys;
    uint8_t *iocq; uint64_t iocq_phys;
    uint32_t iosq_tail, iocq_head;
    uint8_t  iocq_phase;

    uint8_t *bounce; uint64_t bounce_phys;

    uint16_t cid;

    /* Identity of namespace 1. */
    uint8_t  ns_active;
    uint64_t ns_blocks;
    uint32_t lba_bytes;
    char     model[48];
    char     serial[24];
    char     firmware[16];
    int      blkdev_id;
} nvme_t;

static nvme_t nv;
static spinlock_t nvme_lock;

/* ---- MMIO helpers ---------------------------------------------------- */

static inline uint32_t nvme_read32(uint32_t off) {
    return *(volatile uint32_t *)(nv.regs + off);
}
static inline void nvme_write32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(nv.regs + off) = val;
}
static inline uint64_t nvme_read64(uint32_t off) {
    /* Two 32-bit reads rather than one 64-bit access: on a weakly ordered
     * machine a single 64-bit MMIO read can be split anyway, and the low
     * half is the one that matters for the CAP fields this driver uses. */
    uint64_t lo = nvme_read32(off);
    uint64_t hi = nvme_read32(off + 4u);
    return lo | (hi << 32);
}
static inline void nvme_write64(uint32_t off, uint64_t val) {
    nvme_write32(off, (uint32_t)(val & 0xFFFFFFFFu));
    nvme_write32(off + 4u, (uint32_t)(val >> 32));
}
static inline void nvme_dma_barrier(void) {
    __sync_synchronize();
}

/* Poll CSTS against `want_mask` for `want` until the budget runs out.
 * Returns 1 on match, 0 on timeout. */
static int nvme_wait_csts(uint32_t want_mask, uint32_t want) {
    for (uint32_t i = 0; i < NVME_READY_LIMIT; i++) {
        uint32_t csts = nvme_read32(NVME_REG_CSTS);
        if ((csts & want_mask) == want) return 1;
        if (csts & NVME_CSTS_CFS) {
            kprintf("[nvme] controller fatal status during wait "
                    "(CSTS=0x%08x)\n", csts);
            return 0;
        }
    }
    return 0;
}

/* ---- Submission and completion --------------------------------------- */

/* Point the submission entry at a 64-byte slot in the given queue. */
static uint32_t *sqe_slot(int admin, uint32_t tail) {
    uint8_t *base = admin ? nv.asq : nv.iosq;
    return (uint32_t *)(base + (tail * 64u));
}

/* Submit whatever is in slot `tail` and wait for its completion.
 *
 * The queue is one-command-in-flight: the tail is advanced by one, the
 * doorbell is rung, and this call blocks (bounded) until the phase bit of the
 * next completion entry flips.  That is enough for identify + sequential
 * block I/O and it makes the failure mode unambiguous.
 *
 * Returns 0 on a good completion, negative on a controller-reported error
 * (-4) or a timeout (-3). */
static int nvme_submit_wait(int admin, uint16_t cid, uint32_t *out_dw0) {
    uint8_t *cq = admin ? nv.acq : nv.iocq;
    uint32_t qid = admin ? NVME_ADMIN_QID : NVME_IO_QID;
    uint32_t qsize = admin ? NVME_ADMIN_QSIZE : NVME_IO_QSIZE;

    /* The doorbell carries the NEW tail (the next free slot), not the slot
     * that was just filled: ringing the old value tells the controller there
     * is nothing new to fetch, and the command then times out with every
     * register looking perfectly healthy.  Measured exactly once. */
    uint32_t next = nvme_ring_next(admin ? nv.asq_tail : nv.iosq_tail, qsize);
    if (admin) nv.asq_tail = next; else nv.iosq_tail = next;
    nvme_dma_barrier();                      /* entry visible before the bell */
    nvme_write32(nvme_doorbell_sq(nv.dstrd, qid), next);

    uint8_t phase = admin ? nv.acq_phase : nv.iocq_phase;
    uint32_t head = admin ? nv.acq_head : nv.iocq_head;

    uint32_t *cqe = (uint32_t *)(cq + head * 16u);
    for (uint32_t i = 0; i < NVME_POLL_LIMIT; i++) {
        nvme_dma_barrier();                  /* read the entry after the DMA */
        uint32_t cdw2 = cqe[2];
        uint32_t cdw3 = cqe[3];
        if (!nvme_cqe_plausible(cdw2, cdw3, phase, cid)) continue;

        nvme_status_t st = nvme_cqe_status(cdw3);
        if (out_dw0) *out_dw0 = cqe[0];

        /* Advance the head and tell the controller it may reuse the slot. */
        uint32_t nh = nvme_ring_next(head, qsize);
        if (admin) { nv.acq_head = nh; if (nh == 0) nv.acq_phase ^= 1u; }
        else       { nv.iocq_head = nh; if (nh == 0) nv.iocq_phase ^= 1u; }
        nvme_write32(nvme_doorbell_cq(nv.dstrd, qid), nh);

        if (st.status_code != 0) {
            kprintf("[nvme] command failed: status type %u code 0x%02x%s\n",
                    (unsigned)st.status_type, (unsigned)st.status_code,
                    st.do_not_retry ? " (do-not-retry)" : "");
            return -4;
        }
        return 0;
    }
    kprintf("[nvme] completion timeout on %s queue (phase=%u head=%u)\n",
            admin ? "admin" : "I/O", (unsigned)phase, head);
    /* A timeout with a healthy CSTS is a queue-plumbing bug, so dump the raw
     * material a reader needs: the entry being watched (with its CID and its
     * status half separately) and the controller's own registers. */
    {
        uint32_t *sq = (uint32_t *)((admin ? nv.asq : nv.iosq));
        kprintf("[nvme]   CQ[%u]: result=%08x sqhd/sqid=%08x cid=%u status=0x%04x\n",
                head, cqe[0], cqe[2],
                (unsigned)nvme_cqe_cid(cqe[3]),
                (unsigned)nvme_cqe_status16(cqe[3]));
        kprintf("[nvme]   waiting for cid=%u; SQ slot0=%08x slot1=%08x\n",
                (unsigned)cid, sq[0], sq[16]);
        kprintf("[nvme]   CSTS=0x%08x CC=0x%08x AQA=0x%08x dstrd=%u tail=%u\n",
                nvme_read32(NVME_REG_CSTS), nvme_read32(NVME_REG_CC),
                nvme_read32(NVME_REG_AQA), nv.dstrd,
                admin ? nv.asq_tail : nv.iosq_tail);
    }
    return -3;
}

/* Build + submit one admin command.  `prp1` is a physical address or 0. */
static int nvme_admin_cmd(uint32_t opcode, uint32_t nsid, uint64_t prp1,
                          uint32_t cdw10, uint32_t cdw11, uint32_t cdw12) {
    uint32_t cid = ++nv.cid;
    uint32_t *e = sqe_slot(1, nv.asq_tail);
    memset(e, 0, 63);                        /* the last byte is the CID's LSB */
    e[0] = nvme_cdw0(opcode, cid);
    e[1] = nsid;
    e[6] = (uint32_t)(prp1 & 0xFFFFFFFFu);
    e[7] = (uint32_t)(prp1 >> 32);
    e[10] = cdw10;
    e[11] = cdw11;
    e[12] = cdw12;
    return nvme_submit_wait(1, (uint16_t)cid, NULL);
}

/* Build + submit one I/O read or write. */
static int nvme_io_cmd(uint32_t opcode, uint64_t lba, uint32_t blocks,
                       uint64_t prp1) {
    uint32_t cid = ++nv.cid;
    uint32_t *e = sqe_slot(0, nv.iosq_tail);
    memset(e, 0, 63);
    e[0] = nvme_cdw0(opcode, cid);
    e[1] = 1u;                               /* NSID 1 */
    e[6] = (uint32_t)(prp1 & 0xFFFFFFFFu);
    e[7] = (uint32_t)(prp1 >> 32);
    e[10] = (uint32_t)(lba & 0xFFFFFFFFu);
    e[11] = (uint32_t)(lba >> 32);
    e[12] = nvme_cdw12_nlb(blocks);
    return nvme_submit_wait(0, (uint16_t)cid, NULL);
}

/* ---- DMA frames ------------------------------------------------------ */

/* One page, zeroed, with its physical address recorded.  The HHDM makes the
 * virtual address free: pages are contiguous in the direct map. */
static int dma_page(uint8_t **out_virt, uint64_t *out_phys) {
    paddr_t phys = pmm_alloc_frame();
    if (!phys) return -1;
    uint8_t *virt = (uint8_t *)(uintptr_t)(boot_get_hhdm_offset() + phys);
    memset(virt, 0, NVME_PAGE);
    *out_virt = virt;
    *out_phys = (paddr_t)phys;
    return 0;
}

/* ---- Bring-up -------------------------------------------------------- */

static int nvme_map_bar(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t bar0 = pci_get_bar(bus, dev, func, 0);
    if (bar0 == 0 || bar0 == 0xFFFFFFFFu) {
        kprintf("[nvme] BAR0 reads 0x%08x — no memory window\n", bar0);
        return -1;
    }
    paddr_t bar_phys = (paddr_t)(bar0 & ~0xFu);
    /* A 64-bit memory BAR keeps its upper half in BAR1. */
    if (((bar0 >> 1) & 0x3u) == 0x2u) {
        uint32_t bar1 = pci_get_bar(bus, dev, func, 1);
        bar_phys |= ((paddr_t)bar1 << 32);
    }
    uint64_t hhdm = boot_get_hhdm_offset();
    /* Two pages: registers + the doorbell array for two queue pairs. */
    for (uint32_t off = 0; off < 0x2000u; off += NVME_PAGE)
        paging_map(hhdm + bar_phys + off, bar_phys + off, PAGE_FLAGS_MMIO);

    nv.regs = (volatile uint8_t *)(uintptr_t)(hhdm + bar_phys);
    kprintf("[nvme] BAR0 phys=0x%llx mapped at 0x%llx (2 pages)\n",
            (unsigned long long)bar_phys,
            (unsigned long long)(hhdm + bar_phys));
    return 0;
}

static int nvme_reset_and_arm(void) {
    /* 1. Reset: disable, then wait for the controller to acknowledge. */
    uint32_t cc = nvme_read32(NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        nvme_write32(NVME_REG_CC, cc & ~NVME_CC_EN);
        if (!nvme_wait_csts(NVME_CSTS_RDY, 0)) {
            kprintf("[nvme] reset: CSTS.RDY never cleared (CSTS=0x%08x)\n",
                    nvme_read32(NVME_REG_CSTS));
            return -1;
        }
        kprintf("[nvme] reset: CC.EN=0 -> CSTS.RDY=0\n");
    }

    /* 2. Admin queues.  AQA's two size fields are 0-based. */
    if (dma_page(&nv.asq, &nv.asq_phys) != 0) return -1;
    if (dma_page(&nv.acq, &nv.acq_phys) != 0) return -1;
    nv.asq_tail = 0; nv.acq_head = 0; nv.acq_phase = 1;

    nvme_write32(NVME_REG_AQA, nvme_aqa_value(NVME_ADMIN_QSIZE, NVME_ADMIN_QSIZE));
    nvme_write64(NVME_REG_ASQ, nv.asq_phys);
    nvme_write64(NVME_REG_ACQ, nv.acq_phys);
    kprintf("[nvme] admin queues: AQA=0x%08x ASQ=0x%llx ACQ=0x%llx (%u entries)\n",
            nvme_read32(NVME_REG_AQA),
            (unsigned long long)nv.asq_phys, (unsigned long long)nv.acq_phys,
            NVME_ADMIN_QSIZE);

    /* 3. Enable: CSS=NVM, IOSQES=6, IOCQES=4 — entry sizes are part of the
     *    configuration, not something the controller guesses. */
    nvme_write32(NVME_REG_CC, nvme_cc_value(1));
    if (!nvme_wait_csts(NVME_CSTS_RDY, NVME_CSTS_RDY)) {
        kprintf("[nvme] CC.EN=1 but CSTS.RDY never set (CSTS=0x%08x)\n",
                nvme_read32(NVME_REG_CSTS));
        return -1;
    }
    kprintf("[nvme] reset: CC.EN=1 -> CSTS.RDY=1 (controller ready)\n");
    return 0;
}

static int nvme_identify(uint32_t cns, uint32_t nsid, uint8_t *out) {
    memset(nv.bounce, 0, NVME_BOUNCE);
    if (nvme_admin_cmd(NVME_ADMIN_IDENTIFY, nsid, nv.bounce_phys,
                       nvme_identify_cdw10(cns), 0, 0) != 0)
        return -1;
    memcpy(out, nv.bounce, NVME_PAGE);
    return 0;
}

static int nvme_create_io_queues(void) {
    if (dma_page(&nv.iosq, &nv.iosq_phys) != 0) return -1;
    if (dma_page(&nv.iocq, &nv.iocq_phys) != 0) return -1;
    nv.iosq_tail = 0; nv.iocq_head = 0; nv.iocq_phase = 1;

    /* Completion queue FIRST: a submission queue must name an existing CQ. */
    if (nvme_admin_cmd(NVME_ADMIN_CREATE_CQ, 0, nv.iocq_phys,
                       nvme_create_cq_cdw10(NVME_IO_QID, NVME_IO_QSIZE),
                       nvme_create_cq_cdw11(1, 0), 0) != 0) {
        kprintf("[nvme] Create I/O Completion Queue refused\n");
        return -1;
    }
    if (nvme_admin_cmd(NVME_ADMIN_CREATE_SQ, 0, nv.iosq_phys,
                       nvme_create_sq_cdw10(NVME_IO_QID, NVME_IO_QSIZE),
                       nvme_create_sq_cdw11(1, NVME_IO_QID, 0), 0) != 0) {
        kprintf("[nvme] Create I/O Submission Queue refused\n");
        return -1;
    }
    kprintf("[nvme] I/O queue pair ready (qid %u, %u entries, physically "
            "contiguous, no interrupts — completion is polled)\n",
            NVME_IO_QID, NVME_IO_QSIZE);
    return 0;
}

/* ---- Block I/O through the bounce buffer ----------------------------- */

/* The bounce buffer keeps every command inside PRP1: a 4 KiB page needs no
 * PRP list, so the driver is honest about doing one memcpy per 4 KiB rather
 * than pretending an arbitrary kernel buffer is physically contiguous. */
static int nvme_transfer(uint64_t lba, uint32_t count, void *buf, int write) {
    if (!nv.ns_active || count == 0 || !buf) return -1;
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > NVME_MAX_SECTORS_PER_CMD) chunk = NVME_MAX_SECTORS_PER_CMD;

        if (write) {
            memcpy(nv.bounce, p, chunk * BLKDEV_SECTOR_SIZE);
            nvme_dma_barrier();
        }
        int rc = nvme_io_cmd(write ? NVME_IO_WRITE : NVME_IO_READ,
                             lba + done, chunk, nv.bounce_phys);
        if (rc != 0) return rc;
        if (!write) {
            nvme_dma_barrier();
            memcpy(p, nv.bounce, chunk * BLKDEV_SECTOR_SIZE);
        }
        p += chunk * BLKDEV_SECTOR_SIZE;
        done += chunk;
    }
    return 0;
}

static int nvme_bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    spinlock_acquire(&nvme_lock);
    int rc = nvme_transfer(lba, count, buf, 0);
    spinlock_release(&nvme_lock);
    return rc;
}

static int nvme_bd_write(void *ctx, uint64_t lba, uint32_t count,
                         const void *buf) {
    (void)ctx;
    spinlock_acquire(&nvme_lock);
    int rc = nvme_transfer(lba, count, (void *)buf, 1);
    spinlock_release(&nvme_lock);
    return rc;
}

static uint64_t nvme_bd_sectors(void *ctx) {
    (void)ctx;
    return nv.ns_blocks;
}

static const struct blkdev_ops nvme_bd_ops = {
    .read = nvme_bd_read,
    .write = nvme_bd_write,
    .sector_count = nvme_bd_sectors,
};

/* ---- Public API ------------------------------------------------------ */

int nvme_init(void) {
    memset(&nv, 0, sizeof(nv));
    nv.blkdev_id = -1;
    spinlock_init(&nvme_lock);

    uint8_t bus = 0, dev = 0, func = 0;
    if (pci_find_class(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_NVME,
                       &bus, &dev, &func) != 0) {
        /* Not an error: most machines have no NVMe controller.  Say so once,
         * quietly, so a boot log shows the probe ran. */
        return -1;
    }
    if (pci_get_prog_if(bus, dev, func) != PCI_PROGIF_NVME) {
        kprintf("[nvme] class 01/08 device at %u:%u.%u has prog-if 0x%02x, "
                "not NVMe (0x02); skipping\n",
                bus, dev, func, pci_get_prog_if(bus, dev, func));
        return -1;
    }

    nv.bus = bus; nv.dev = dev; nv.func = func;
    kprintf("[nvme] controller at PCI %u:%u.%u\n", bus, dev, func);
    pci_enable_bus_master(bus, dev, func);

    if (nvme_map_bar(bus, dev, func) != 0) return -3;

    nv.cap = nvme_read64(NVME_REG_CAP);
    nv.dstrd = nvme_cap_dstrd(nv.cap);
    nv.mqes = nvme_cap_mqes(nv.cap);
    kprintf("[nvme] VS=0x%08x CAP=0x%llx (MQES=%u, DSTRD=%u, MPSMIN=%u)\n",
            nvme_read32(NVME_REG_VS), (unsigned long long)nv.cap,
            nv.mqes, nv.dstrd, nvme_cap_mpsmin(nv.cap));
    if (nvme_cap_mpsmin(nv.cap) != 0) {
        kprintf("[nvme] MPSMIN=%u: this driver only speaks 4 KiB pages\n",
                nvme_cap_mpsmin(nv.cap));
        return -3;
    }

    if (nvme_reset_and_arm() != 0) return -3;
    if (dma_page(&nv.bounce, &nv.bounce_phys) != 0) return -3;

    /* 4. Identity.  These are the controller's own bytes. */
    uint8_t id[4096];
    if (nvme_identify(NVME_IDENTIFY_CTRL, 0, id) != 0) {
        kprintf("[nvme] Identify Controller failed\n");
        return -3;
    }
    nvme_id_string(id, NVME_ID_CTRL_MN, 40, nv.model, sizeof(nv.model));
    nvme_id_string(id, NVME_ID_CTRL_SN, 20, nv.serial, sizeof(nv.serial));
    nvme_id_string(id, NVME_ID_CTRL_FR, 8, nv.firmware, sizeof(nv.firmware));
    kprintf("[nvme] identify controller: model=\"%s\" serial=\"%s\" "
            "firmware=\"%s\"\n", nv.model, nv.serial, nv.firmware);

    if (nvme_identify(NVME_IDENTIFY_NSID_0, 1, id) != 0) {
        kprintf("[nvme] Identify Namespace 1 failed (is a namespace attached "
                "to the controller?)\n");
        return -3;
    }
    nv.ns_blocks = nvme_le64(id + NVME_ID_NS_SIZE);
    nv.lba_bytes = nvme_ns_lba_bytes(id);
    if (nv.ns_blocks == 0) {
        kprintf("[nvme] namespace 1 reports zero blocks; nothing to register\n");
        return -3;
    }
    if (nv.lba_bytes != BLKDEV_SECTOR_SIZE) {
        kprintf("[nvme] namespace 1 uses %u-byte logical blocks; this driver "
                "registers only %u-byte LBAs (the blkdev seam is 512-only) — "
                "refused by name, not divided by\n",
                nv.lba_bytes, BLKDEV_SECTOR_SIZE);
        return -2;
    }
    nv.ns_active = 1;
    kprintf("[nvme] namespace 1: %llu blocks x %u bytes = %llu MiB\n",
            (unsigned long long)nv.ns_blocks, nv.lba_bytes,
            (unsigned long long)((nv.ns_blocks * nv.lba_bytes) >> 20));

    /* 5. I/O queues, then the block device. */
    if (nvme_create_io_queues() != 0) return -3;

    nv.blkdev_id = blkdev_register("nvme0", &nvme_bd_ops, &nv, BLKDEV_SECTOR_SIZE);
    if (nv.blkdev_id < 0) {
        kprintf("[nvme] blkdev_register refused (rc=%d)\n", nv.blkdev_id);
        return -3;
    }
    kprintf("[blkdev] registered 'nvme0' as disk %d (%llu sectors)\n",
            nv.blkdev_id, (unsigned long long)nv.ns_blocks);

    nv.up = 1;
    kprintf("[nvme] PASS: namespace 1 is block device %d\n", nv.blkdev_id);
    return 0;
}

int nvme_namespace_count(void) {
    return (nv.up && nv.ns_active) ? 1 : 0;
}

void nvme_self_test(void) {
    if (!nv.up) return;

    kprintf("[nvme] self-test: reading LBA 0 through the driver\n");
    uint8_t direct[512];
    memset(direct, 0, sizeof(direct));
    if (nvme_transfer(0, 1, direct, 0) != 0) {
        kprintf("[nvme] FAIL: read of LBA 0 through the driver\n");
        return;
    }
    uint32_t sum_direct = 0;
    for (int i = 0; i < 512; i++) sum_direct += direct[i];

    kprintf("[nvme] self-test: reading LBA 0 through the blkdev seam\n");
    uint8_t viaseam[512];
    memset(viaseam, 0, sizeof(viaseam));
    if (blkdev_read(nv.blkdev_id, 0, 1, viaseam) != 0) {
        kprintf("[nvme] FAIL: read of LBA 0 through blkdev\n");
        return;
    }
    uint32_t sum_seam = 0;
    for (int i = 0; i < 512; i++) sum_seam += viaseam[i];

    kprintf("[nvme] read LBA 0: first bytes %02x %02x %02x %02x, "
            "checksum %u (driver) vs %u (seam)\n",
            direct[0], direct[1], direct[2], direct[3], sum_direct, sum_seam);
    if (sum_direct != sum_seam || memcmp(direct, viaseam, 512) != 0) {
        kprintf("[nvme] FAIL: the two paths disagree about LBA 0\n");
        return;
    }
    kprintf("[nvme] PASS: %llu blocks reachable, both paths agree on LBA 0\n",
            (unsigned long long)nv.ns_blocks);
}
