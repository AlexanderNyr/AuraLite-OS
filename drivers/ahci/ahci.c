/* ahci.c — AHCI SATA driver for QEMU's ICH9 AHCI controller.
 *
 * Implements: PCI enumeration, ABAR mapping, port enumeration, device
 * detection, command list/table setup, and sector read/write via DMA (PRDT).
 *
 * Status: Controller detection, port init, and command table setup all work.
 * The PxCI command issue triggers a fault — likely a QEMU AHCI interrupt
 * delivery issue. Investigation is documented in TODO.md.
 */

#include <stdint.h>
#include "drivers/ahci/ahci.h"
#include "drivers/pci/pci.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/arch.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/boot_info.h"
#include "kernel/lib/spinlock.h"

/* ---- PCI / AHCI constants ---- */
#define PCI_CLASS_MASS_STORAGE  0x01
#define PCI_SUBCLASS_SATA_AHCI  0x06

#define AHCI_GHC    0x0004
#define AHCI_PI     0x000C
#define AHCI_VS     0x0010
#define AHCI_CAP    0x0000
#define GHC_AE      (1u << 31)

/* Per-port registers (port N at 0x100 + N * 0x80). */
#define PORT_PXCLB   0x00
#define PORT_PXCLBU  0x04
#define PORT_PXFB    0x08
#define PORT_PXFBU   0x0C
#define PORT_PXIS    0x10
#define PORT_PXIE    0x14
#define PORT_PXCMD   0x18
#define PORT_PXTFD   0x20
#define PORT_PXSIG   0x24
#define PORT_PXSSTS  0x28
#define PORT_PXSERR  0x30
#define PORT_PXCI    0x38

#define PXCMD_ST    (1u << 0)
#define PXCMD_FRE   (1u << 4)
#define PXCMD_CR    (1u << 15)
#define PXCMD_FR    (1u << 14)

#define SATA_SIG_ATA  0x00000101

/* ---- HBA structures (all packed, DMA-visible) ---- */

struct hba_cmd_hdr {
    uint16_t info;       /* [0:4]=CFL, [5]=A, [6]=W, [8:15]=PRDTL */
    uint16_t reserved0;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved1[4];
} __attribute__((packed));

struct hba_prdt {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc_i;      /* [0:21]=DBC-1, [30]=I */
} __attribute__((packed));

struct hba_cmd_tbl {
    uint8_t cfis[64];
    uint8_t acmd[64];
    uint8_t reserved[0x80 - 128];
    struct hba_prdt prdt[1];
} __attribute__((packed));

struct sata_fis_h2d {
    uint8_t fis_type, pm_ctrl, command, feature_low;
    uint8_t lba0, lba1, lba2, device;
    uint8_t lba3, lba4, lba5, feature_high;
    uint8_t count_low, count_high, icc, control;
} __attribute__((packed));

#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35

/* ---- Per-port state ---- */
struct ahci_port {
    int present;
    volatile struct hba_cmd_hdr *cmd_list;
    volatile struct hba_cmd_tbl  *cmd_tbl;
    volatile uint8_t             *fis_rx;

    /* A DMA bounce buffer, allocated ONCE at port init.
     *
     * It used to be allocated per transfer with pmm_alloc_contiguous() and
     * never freed, which was two bugs in one line.  The leak exhausted
     * physical memory after a few hundred reads -- enough that every file
     * on /fat appeared truncated to 173824 bytes, a symptom pointing
     * nowhere near this driver.  And even with the free added, allocating
     * per transfer costs a linear bitmap scan plus a 4 KiB memset on every
     * 512-byte sector: measured at ~93 ms per sector, which is 5 KB/s and
     * makes a 28 MB file take an hour and a half.
     *
     * One buffer per port, reused, fixes both. */
    uint64_t dma_phys;
    uint8_t *dma_virt;
    uint32_t dma_bytes;
};

/* Size of that buffer: the largest single transfer the driver will issue.
 * 128 KiB covers a full cluster on any sane FAT32 volume and keeps the
 * per-port cost to 32 frames. */
#define AHCI_DMA_BUF_BYTES (128u * 1024u)

static volatile uint32_t *abar = NULL;
static struct ahci_port ports[AHCI_MAX_PORTS];
static int port_count = 0;

/*
 * Per-port hardware lock (A2-R1).
 *
 * Every transfer this driver issues goes through ONE bounce buffer and ONE
 * command slot per port: ahci_exec() fills ports[port].cmd_list[0], points
 * its single PRDT entry at ports[port].dma_phys, writes PxCI=1, polls until
 * the slot clears, and ahci_read() then memcpy()s out of the shared
 * dma_virt.  None of that was serialised.
 *
 * Eight subsystems call in -- fat32, diskfs, ext2, ext4, btrfs, f2fs,
 * buffer_cache and the /disk layer -- and each has at most its own
 * filesystem-level lock.  fat32_lock makes FAT32 safe against FAT32, but
 * nothing stopped the BSP reading /disk through diskfs while an AP flushed
 * the kernel log to /fat/AURALOG.TXT: both land in ahci_exec() on the same
 * port, the second overwrites the first's command header and PRDT while the
 * first is still polling PxCI, and the winner's data lands in the loser's
 * buffer.
 *
 * That is exactly the observed A2-R1 signature: `cat /disk/persist.txt`
 * returning the raw bytes "AURALOG TXT ..." -- a FAT32 *directory* sector
 * delivered into a diskfs read.  Not a FAT32 bug at all; the FAT32 driver
 * was the victim's neighbour.
 *
 * The lock is per port, not global, so independent disks still overlap.
 * AHCI here is polled -- no IRQ handler touches this state -- and the lock
 * is never held across kprintf, so irqsave acquisition cannot deadlock.
 */
static spinlock_t ahci_port_locks[AHCI_MAX_PORTS];

/* Cumulative sector I/O counters -- see ahci_get_stats() in ahci.h. */
static volatile uint64_t sectors_read_total    = 0;
static volatile uint64_t sectors_written_total = 0;

/* ---- MMIO helpers ---- */
static inline uint32_t port_read(int p, uint32_t off) {
    return abar[(0x100 + p * 0x80 + off) / 4];
}
static inline void port_write(int p, uint32_t off, uint32_t val) {
    abar[(0x100 + p * 0x80 + off) / 4] = val;
}
static inline uint32_t abar_read(uint32_t off) { return abar[off / 4]; }
static inline void abar_write(uint32_t off, uint32_t val) { abar[off / 4] = val; }

/* ---- Port stop / start ---- */
static void port_stop(int port) {
    uint32_t cmd = port_read(port, PORT_PXCMD);
    port_write(port, PORT_PXCMD, cmd & ~PXCMD_ST);
    int t = 100000;
    while ((port_read(port, PORT_PXCMD) & PXCMD_CR) && t-- > 0)
        __asm__ volatile ("pause");
    cmd = port_read(port, PORT_PXCMD);
    port_write(port, PORT_PXCMD, cmd & ~PXCMD_FRE);
    t = 100000;
    while ((port_read(port, PORT_PXCMD) & PXCMD_FR) && t-- > 0)
        __asm__ volatile ("pause");
}

static void port_start(int port) {
    while (port_read(port, PORT_PXCMD) & PXCMD_CR)
        __asm__ volatile ("pause");
    uint32_t cmd = port_read(port, PORT_PXCMD);
    port_write(port, PORT_PXCMD, cmd | PXCMD_FRE);
    cmd = port_read(port, PORT_PXCMD);
    port_write(port, PORT_PXCMD, cmd | PXCMD_ST);
}

/* ---- Port init ---- */
static int ahci_init_port(int port) {
    uint64_t hhdm = boot_get_hhdm_offset();

    port_stop(port);

    /* Command list (1 page). */
    uint64_t cl = pmm_alloc_frame();
    if (!cl) return -1;
    memset((void *)(uintptr_t)(hhdm + cl), 0, 4096);
    port_write(port, PORT_PXCLB, (uint32_t)cl);
    port_write(port, PORT_PXCLBU, (uint32_t)(cl >> 32));
    ports[port].cmd_list = (volatile struct hba_cmd_hdr *)(uintptr_t)(hhdm + cl);

    /* The persistent DMA bounce buffer for this port. */
    {
        uint32_t frames = AHCI_DMA_BUF_BYTES / 4096u;
        uint64_t db = pmm_alloc_contiguous(frames);
        if (!db) {
            kprintf("[ahci] port %d: cannot allocate a %u KiB DMA buffer\n",
                    port, AHCI_DMA_BUF_BYTES / 1024u);
            return -1;
        }
        ports[port].dma_phys  = db;
        ports[port].dma_virt  = (uint8_t *)(uintptr_t)(hhdm + db);
        ports[port].dma_bytes = AHCI_DMA_BUF_BYTES;
    }

    /* FIS receive (1 page). */
    uint64_t fb = pmm_alloc_frame();
    if (!fb) return -1;
    memset((void *)(uintptr_t)(hhdm + fb), 0, 256);
    port_write(port, PORT_PXFB, (uint32_t)fb);
    port_write(port, PORT_PXFBU, (uint32_t)(fb >> 32));
    ports[port].fis_rx = (volatile uint8_t *)(uintptr_t)(hhdm + fb);

    /* Command table (1 page). */
    uint64_t ct = pmm_alloc_frame();
    if (!ct) return -1;
    memset((void *)(uintptr_t)(hhdm + ct), 0, 4096);
    ports[port].cmd_tbl = (volatile struct hba_cmd_tbl *)(uintptr_t)(hhdm + ct);
    ports[port].cmd_list[0].ctba  = (uint32_t)ct;
    ports[port].cmd_list[0].ctbau = (uint32_t)(ct >> 32);

    /* Clear sticky interrupt/error state before starting the engine. Keep port
     * interrupts disabled; the driver polls PxCI/PxTFD for completion. */
    port_write(port, PORT_PXIS, 0xFFFFFFFF);
    port_write(port, PORT_PXSERR, 0xFFFFFFFF);
    port_write(port, PORT_PXIE, 0);

    port_start(port);
    return 0;
}

static void ahci_enumerate(uint32_t pi) {
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        uint32_t ssts = port_read(i, PORT_PXSSTS);
        if ((ssts & 0x0F) != 3) continue;
        uint32_t sig = port_read(i, PORT_PXSIG);
        if (sig == SATA_SIG_ATA) {
            kprintf("[ahci] port %d: SATA disk (sig=0x%08x)\n", i, sig);
            if (ahci_init_port(i) == 0) {
                ports[i].present = 1;
                port_count++;
            }
        }
    }
}

/* ---- Command execution ---- */
static int ahci_exec(int port, uint8_t cmd, int write, uint64_t lba,
                     uint16_t count, uint64_t buf_phys, uint32_t buf_len) {
    if (!ports[port].present || count == 0) return -1;
    if (buf_len == 0 || buf_len > 0x400000) return -1; /* one PRDT entry max */

    /* Wait until the device is not busy and not requesting data. */
    int wait = 1000000;
    while ((port_read(port, PORT_PXTFD) & 0x88) && wait-- > 0) {
        __asm__ volatile ("pause");
    }
    if (wait <= 0) {
        kprintf("[ahci] port %d: device busy before command (PxTFD=0x%x)\n",
                port, port_read(port, PORT_PXTFD));
        return -1;
    }

    volatile struct hba_cmd_tbl *tbl = ports[port].cmd_tbl;
    memset((void *)tbl, 0, 4096);

    struct sata_fis_h2d *fis = (struct sata_fis_h2d *)tbl->cfis;
    fis->fis_type  = 0x27;   /* Register Host-to-Device FIS */
    fis->pm_ctrl   = 0x80;   /* bit 7 = command */
    fis->command   = cmd;
    fis->device    = 0x40;   /* LBA mode */
    fis->lba0      = lba & 0xFF;
    fis->lba1      = (lba >> 8) & 0xFF;
    fis->lba2      = (lba >> 16) & 0xFF;
    fis->lba3      = (lba >> 24) & 0xFF;
    fis->lba4      = (lba >> 32) & 0xFF;
    fis->lba5      = (lba >> 40) & 0xFF;
    fis->count_low = count & 0xFF;
    fis->count_high = (count >> 8) & 0xFF;
    fis->control   = 0;

    /* PRDT entry: byte count field is encoded as N-1. */
    tbl->prdt[0].dba   = (uint32_t)buf_phys;
    tbl->prdt[0].dbau  = (uint32_t)(buf_phys >> 32);
    tbl->prdt[0].dbc_i = (buf_len - 1) & 0x003FFFFF;

    /* Command header DW0 layout is split across two uint16_t fields in our
     * struct: info = low 16 bits (CFL/W/etc.), reserved0 = high 16 bits
     * (PRDTL). PRDTL must be 1 for the single PRDT entry below. */
    ports[port].cmd_list[0].info  = 5 | (write ? (1u << 6) : 0);
    ports[port].cmd_list[0].reserved0 = 1;  /* PRDTL */
    ports[port].cmd_list[0].prdbc = 0;

    /* Clear stale status and issue slot 0. */
    port_write(port, PORT_PXIS, 0xFFFFFFFF);
    port_write(port, PORT_PXSERR, 0xFFFFFFFF);
    __asm__ volatile ("mfence" ::: "memory");
    port_write(port, PORT_PXCI, 1);

    int t = 50000000;
    while (port_read(port, PORT_PXCI) & 1) {
        uint32_t is = port_read(port, PORT_PXIS);
        if (is & (1u << 30)) { /* Task File Error Status */
            kprintf("[ahci] port %d: task-file error PxIS=0x%x PxTFD=0x%x SERR=0x%x\n",
                    port, is, port_read(port, PORT_PXTFD), port_read(port, PORT_PXSERR));
            return -1;
        }
        if (t-- <= 0) {
            kprintf("[ahci] port %d: timeout PxCI=0x%x PxIS=0x%x PxTFD=0x%x SERR=0x%x\n",
                    port, port_read(port, PORT_PXCI), is,
                    port_read(port, PORT_PXTFD), port_read(port, PORT_PXSERR));
            return -1;
        }
        __asm__ volatile ("pause");
    }

    uint32_t tfd = port_read(port, PORT_PXTFD);
    uint32_t is = port_read(port, PORT_PXIS);
    if ((tfd & 0x01) || (is & (1u << 30))) {
        kprintf("[ahci] port %d: error PxIS=0x%x PxTFD=0x%x SERR=0x%x\n",
                port, is, tfd, port_read(port, PORT_PXSERR));
        return -1;
    }
    return 0;
}

/* ---- Public API ---- */

int ahci_init(void) {
    uint8_t bus, dev, func;

    if (pci_find_class(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA_AHCI,
                       &bus, &dev, &func) != 0) {
        kprintf("[ahci] no AHCI controller found\n");
        return -1;
    }
    kprintf("[ahci] controller at PCI %u:%u.%u\n", bus, dev, func);
    pci_enable_bus_master(bus, dev, func);

    uint32_t abar_phys = pci_get_bar(bus, dev, func, 5) & ~0xF;
    uint64_t hhdm = boot_get_hhdm_offset();

    /* Map 8 KiB of ABAR. */
    for (uint32_t off = 0; off < 0x2000; off += 0x1000)
        paging_map(hhdm + abar_phys + off, abar_phys + off,
                   PAGE_FLAGS_MMIO);
    abar = (volatile uint32_t *)(uintptr_t)(hhdm + abar_phys);

    /* Enable AHCI mode. */
    abar_write(AHCI_GHC, abar_read(AHCI_GHC) | GHC_AE);

    uint32_t pi = abar_read(AHCI_PI);
    kprintf("[ahci] version=0x%x, PI=0x%08x\n", abar_read(AHCI_VS), pi);

    memset(ports, 0, sizeof(ports));
    for (int i = 0; i < AHCI_MAX_PORTS; i++)
        spinlock_init(&ahci_port_locks[i]);
    ahci_enumerate(pi);
    kprintf("[ahci] %d SATA device(s) ready\n", port_count);
    return 0;
}

int ahci_read(uint32_t port, uint64_t lba, uint32_t count, void *buf) {
    if (port >= AHCI_MAX_PORTS || !ports[port].present || !count)
        return -1;
    uint32_t len = count * AHCI_SECTOR_SIZE;
    if (len > ports[port].dma_bytes) return -1;   /* caller must split */

    /* A2-R1: the command slot AND the bounce buffer are per-port shared
     * state.  The memcpy out of dma_virt must happen under the same lock as
     * the transfer that filled it, or another CPU's read overwrites the
     * buffer between completion and copy. */
    uint64_t af = spinlock_acquire_irqsave(&ahci_port_locks[port]);
    int rc = ahci_exec(port, ATA_READ_DMA_EXT, 0, lba, count,
                       ports[port].dma_phys, len);
    if (rc == 0) {
        memcpy(buf, ports[port].dma_virt, len);
        sectors_read_total += count;
    }
    spinlock_release_irqrestore(&ahci_port_locks[port], af);
    return rc;
}

int ahci_write(uint32_t port, uint64_t lba, uint32_t count, const void *buf) {
    if (port >= AHCI_MAX_PORTS || !ports[port].present || !count)
        return -1;
    uint32_t len = count * AHCI_SECTOR_SIZE;
    if (len > ports[port].dma_bytes) return -1;   /* caller must split */

    /* A2-R1: likewise, filling dma_virt and issuing the command must be
     * atomic with respect to any other transfer on this port. */
    uint64_t af = spinlock_acquire_irqsave(&ahci_port_locks[port]);
    memcpy(ports[port].dma_virt, buf, len);
    int rc = ahci_exec(port, ATA_WRITE_DMA_EXT, 1, lba, count,
                       ports[port].dma_phys, len);
    if (rc == 0) sectors_written_total += count;
    spinlock_release_irqrestore(&ahci_port_locks[port], af);
    return rc;
}

void ahci_get_stats(uint64_t *out_sectors_read, uint64_t *out_sectors_written) {
    if (out_sectors_read)    *out_sectors_read    = sectors_read_total;
    if (out_sectors_written) *out_sectors_written = sectors_written_total;
}

int ahci_get_port_count(void) { return port_count; }

int ahci_get_first_port(void) {
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (ports[i].present) return i;
    }
    return -1;
}

int ahci_get_nth_port(int n) {
    if (n < 0) return -1;
    int seen = 0;
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (ports[i].present) {
            if (seen == n) return i;
            seen++;
        }
    }
    return -1;
}

void ahci_self_test(void) {
    if (port_count == 0) {
        kprintf("[ahci] self-test: no devices\n");
        return;
    }
    int port = -1;
    for (int i = 0; i < AHCI_MAX_PORTS; i++)
        if (ports[i].present) { port = i; break; }

    kprintf("[ahci] self-test: reading sector 0 from port %d...\n", port);
    uint8_t mbr[512];
    if (ahci_read(port, 0, 1, mbr) != 0) {
        kprintf("[ahci] FAIL: sector read not yet functional\n");
        return;
    }
    int has_sig = (mbr[510] == 0x55 && mbr[511] == 0xAA);
    kprintf("[ahci] sector 0: %s, first bytes:", has_sig ? "MBR" : "data");
    for (int i = 0; i < 16; i++) kprintf(" %02x", mbr[i]);
    kprintf("\n");
    if (!has_sig && !mbr[0]) {
        kprintf("[ahci] FAIL: SATA read returned empty sector\n");
        return;
    }

    /* Write/read a scratch sector to verify DMA write as well. Sector 1 is
     * reserved for the AHCI self-test in the VM test disk created by
     * tools/run_qemu.sh. */
    static uint8_t wbuf[512];
    static uint8_t rbuf[512];
    memset(wbuf, 0, sizeof(wbuf));
    memcpy(wbuf, "AURALAHCI-WRITE", 15);
    wbuf[510] = 0x55;
    wbuf[511] = 0xAA;
    kprintf("[ahci] self-test: writing scratch sector 1...\n");
    if (ahci_write(port, 1, 1, wbuf) != 0) {
        kprintf("[ahci] FAIL: SATA write sector 1 failed\n");
        return;
    }
    memset(rbuf, 0, sizeof(rbuf));
    if (ahci_read(port, 1, 1, rbuf) != 0) {
        kprintf("[ahci] FAIL: SATA readback sector 1 failed\n");
        return;
    }
    if (memcmp(wbuf, rbuf, sizeof(wbuf)) != 0) {
        kprintf("[ahci] FAIL: SATA write/readback mismatch\n");
        return;
    }
    kprintf("[ahci] PASS: SATA read/write DMA works\n");
}

int ahci_read_sector(uint32_t port, uint64_t lba, void *buf) {
    return ahci_read(port, lba, 1, buf);
}

int ahci_write_sector(uint32_t port, uint64_t lba, const void *buf) {
    return ahci_write(port, lba, 1, buf);
}


/* ------------------------------------------------------------------ */
/* PARITY_PLAN.md P1: the blkdev seam.                                */
/*                                                                    */
/* Every detected port registers as one block device, in detection    */
/* order — so blkdev id N is exactly the disk ahci_get_nth_port(N)    */
/* used to name, and the boot-time mount assignments (kernel.c) keep  */
/* their meaning to the byte.  The driver includes the seam header;   */
/* the seam never includes the driver.                                */
/* ------------------------------------------------------------------ */

#include "kernel/fs/blkdev.h"

static int ahci_bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    return ahci_read((uint32_t)(uintptr_t)ctx, lba, count, buf);
}

static int ahci_bd_write(void *ctx, uint64_t lba, uint32_t count,
                         const void *buf)
{
    return ahci_write((uint32_t)(uintptr_t)ctx, lba, count, buf);
}

static const struct blkdev_ops ahci_bd_ops = {
    .read         = ahci_bd_read,
    .write        = ahci_bd_write,
    .sector_count = 0,          /* honest: the driver never reads capacity */
};

static char ahci_bd_names[BLKDEV_MAX][8];

void ahci_register_blkdevs(void)
{
    for (int n = 0; n < BLKDEV_MAX; n++) {
        int port = ahci_get_nth_port(n);
        if (port < 0)
            break;
        char *name = ahci_bd_names[n];
        name[0] = 'a'; name[1] = 'h'; name[2] = 'c'; name[3] = 'i';
        name[4] = (char)('0' + (n % 10)); name[5] = '\0';
        int dev = blkdev_register(name, &ahci_bd_ops,
                                  (void *)(uintptr_t)port,
                                  AHCI_SECTOR_SIZE);
        if (dev < 0) {
            kprintf("[blkdev] REFUSED %s (port %d): rc=%d\n",
                    name, port, dev);
            break;
        }
        kprintf("[blkdev] blk%d = %s (AHCI port %d)\n", dev, name, port);
        int pk = blkdev_partition_kind(dev);
        if (pk > 0)
            kprintf("[blkdev] blk%d carries a %s partition table; raw "
                    "mounts IGNORE it (RES-04: no partition parsing "
                    "by design)\n", dev,
                    pk == BLKDEV_PART_GPT ? "GPT" : "MBR");
    }
}
