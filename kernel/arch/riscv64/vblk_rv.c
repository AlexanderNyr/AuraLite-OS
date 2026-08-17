/* kernel/arch/riscv64/vblk_rv.c -- virtio-blk over mmio (RISCV_PLAN V7).
 *
 * The request format is the PCI driver's exactly (virtio-blk is
 * transport-agnostic by design): a 16-byte header descriptor, a data
 * descriptor, a 1-byte status descriptor -- the same three-chain
 * drivers/virtio_blk/virtio_blk.c posts.  Capacity comes from the
 * mmio config space (le64 at offset 0).
 *
 * The self-test is ata32_selftest's shape verbatim: sector 0 of the
 * attached disk must carry known bytes, then write/readback/restore
 * on the LAST sector (on real hardware that disk is somebody's SD
 * card; leave it as found).
 */

#include <stdint.h>

#include "kernel/arch/riscv64/vblk_rv.h"
#include "kernel/arch/riscv64/virtio_mmio.h"
#include "kernel/arch/riscv64/paging_rv.h"
#include "kernel/arch/riscv64/pmm_rv.h"
#include "kernel/arch/riscv64/sbi.h"

#define VBLK_T_IN   0
#define VBLK_T_OUT  1
#define VBLK_S_OK   0

struct vblk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static struct vmmio_dev dev;
static int ready;
static uint64_t capacity;

/* One page holds header + data + status for a 512-byte op. */
static uint64_t dma_phys;
static uint8_t *dma;

static void put_udec_(uint64_t v)
{
    char buf[20]; int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) sbi_putc(buf[i]);
}

int vblk_rv_init(const fdt_platform_t *plat)
{
    uint64_t bases[FDT_MAX_VIRTIO];
    for (uint32_t i = 0; i < plat->virtio_count; i++)
        bases[i] = (uint64_t)p2v_rv(plat->virtio_base[i]);

    if (vmmio_probe(&dev, bases, plat->virtio_count, VM_DEV_BLK) != 0) {
        sbi_puts("[blk]  no virtio-blk device on the mmio windows "
                 "(pass -drive/-device to attach one)\n");
        return -1;
    }

    dma_phys = pmm_rv_alloc_frame();
    if (!dma_phys)
        return -1;
    dma = (uint8_t *)p2v_rv(dma_phys);

    /* Config space: capacity le64 at offset 0. */
    capacity = 0;
    for (int i = 7; i >= 0; i--)
        capacity = (capacity << 8) | vmmio_cfg8(&dev, (uint32_t)i);

    sbi_puts("[blk]  virtio-blk over mmio (legacy version ");
    put_udec_(dev.version);
    sbi_puts("): ");
    put_udec_(capacity);
    sbi_puts(" sectors (");
    put_udec_(capacity / 2048);
    sbi_puts(" MiB), queue size ");
    put_udec_(dev.qsize);
    sbi_puts("\n");
    ready = 1;
    return 0;
}

int vblk_rv_available(void) { return ready; }
uint64_t vblk_rv_sector_count(void) { return capacity; }

static int vblk_op(uint32_t type, uint64_t lba, uint8_t *buf512)
{
    if (!ready || lba >= capacity)
        return -1;

    struct vblk_req_hdr *hdr = (struct vblk_req_hdr *)dma;
    uint8_t *data   = dma + 512;
    uint8_t *status = dma + 1024;

    hdr->type = type; hdr->reserved = 0; hdr->sector = lba;
    *status = 0xFF;
    if (type == VBLK_T_OUT)
        for (int i = 0; i < 512; i++)
            data[i] = buf512[i];

    dev.desc[0].addr  = dma_phys;
    dev.desc[0].len   = sizeof(*hdr);
    dev.desc[0].flags = VRING_DESC_F_NEXT;
    dev.desc[0].next  = 1;
    dev.desc[1].addr  = dma_phys + 512;
    dev.desc[1].len   = 512;
    dev.desc[1].flags = VRING_DESC_F_NEXT |
                        (type == VBLK_T_IN ? VRING_DESC_F_WRITE : 0);
    dev.desc[1].next  = 2;
    dev.desc[2].addr  = dma_phys + 1024;
    dev.desc[2].len   = 1;
    dev.desc[2].flags = VRING_DESC_F_WRITE;
    dev.desc[2].next  = 0;

    if (vmmio_submit_wait(&dev, 3) < 0)
        return -1;
    if (*status != VBLK_S_OK)
        return -1;
    if (type == VBLK_T_IN)
        for (int i = 0; i < 512; i++)
            buf512[i] = data[i];
    return 0;
}

int vblk_rv_read(uint64_t lba, uint8_t *buf512)
{
    return vblk_op(VBLK_T_IN, lba, buf512);
}

int vblk_rv_write(uint64_t lba, const uint8_t *buf512)
{
    return vblk_op(VBLK_T_OUT, lba, (uint8_t *)buf512);
}

int vblk_rv_selftest(void)
{
    static uint8_t sec[512], pattern[512], readback[512];

    /* 1. Sector 0 carries bytes the smoke test PUT there (the rv64
     * boot has no MBR -- the test disk is written with a known
     * pattern before the run; asserting our own bytes is the same
     * "read what we know" contract ata32 got from Stage 1's MBR). */
    if (vblk_rv_read(0, sec) != 0)
        return -1;
    if (!(sec[0] == 'A' && sec[1] == 'u' && sec[2] == 'r' &&
          sec[3] == 'a' && sec[510] == 0x55 && sec[511] == 0xAA)) {
        sbi_puts("[blk]  FAIL: sector 0 lacks the known test pattern\n");
        return -1;
    }

    /* 2. Write/readback/restore on the last sector. */
    uint64_t victim = capacity - 1;
    if (vblk_rv_read(victim, sec) != 0)
        return -1;
    for (int i = 0; i < 512; i++)
        pattern[i] = (uint8_t)(i ^ 0xA5);
    if (vblk_rv_write(victim, pattern) != 0)
        return -1;
    if (vblk_rv_read(victim, readback) != 0)
        return -1;
    for (int i = 0; i < 512; i++) {
        if (readback[i] != pattern[i]) {
            sbi_puts("[blk]  FAIL: readback mismatch\n");
            return -1;
        }
    }
    if (vblk_rv_write(victim, sec) != 0)    /* restore */
        return -1;

    sbi_puts("[blk]  PASS: known-bytes read + write/readback/restore on LBA ");
    put_udec_(victim);
    sbi_puts("\n");
    return 0;
}
