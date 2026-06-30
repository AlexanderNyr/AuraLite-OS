/*
 * test_virtio_net.c — host unit tests for virtio-net wire-layout invariants.
 *
 * The kernel virtio_net.c cannot be linked here (it pulls in PCI/paging/PMM and
 * other kernel transport), so this test re-declares the same packed structures
 * and validates the layout contract that the driver relies on:
 *
 *   - struct virtio_net_hdr is exactly 12 bytes under VIRTIO_F_VERSION_1 (the
 *     num_buffers field is always present in the 1.0 spec, independent of
 *     MRG_RXBUF).  This is the bug that, when the header was 10 bytes, shifted
 *     every transmitted frame by 2 bytes on the wire.
 *   - Field offsets within the header.
 *   - The split-virtqueue structures (desc/avail/used) match the spec layout.
 *
 * If virtio_net.c changes any of these, these expectations must change with it.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define VNET_Q_SIZE  16
#define VNET_HDR_LEN 12

struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
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

static int passed = 0, failed = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } else { passed++; } } while (0)

int main(void) {
    printf("=== virtio-net wire-layout tests ===\n\n");

    /* Header is 12 bytes (VIRTIO_F_VERSION_1) and matches the driver's len. */
    CHECK(sizeof(struct virtio_net_hdr) == 12);
    CHECK(sizeof(struct virtio_net_hdr) == VNET_HDR_LEN);

    /* Field offsets. */
    CHECK(offsetof(struct virtio_net_hdr, flags)       == 0);
    CHECK(offsetof(struct virtio_net_hdr, gso_type)    == 1);
    CHECK(offsetof(struct virtio_net_hdr, hdr_len)     == 2);
    CHECK(offsetof(struct virtio_net_hdr, gso_size)    == 4);
    CHECK(offsetof(struct virtio_net_hdr, csum_start)  == 6);
    CHECK(offsetof(struct virtio_net_hdr, csum_offset) == 8);
    CHECK(offsetof(struct virtio_net_hdr, num_buffers) == 10);

    /* Split-virtqueue descriptor: 16 bytes, fields in spec order. */
    CHECK(sizeof(struct vring_desc) == 16);
    CHECK(offsetof(struct vring_desc, addr)  == 0);
    CHECK(offsetof(struct vring_desc, len)   == 8);
    CHECK(offsetof(struct vring_desc, flags) == 12);
    CHECK(offsetof(struct vring_desc, next)  == 14);

    /* Available ring: flags, idx, then the ring array. */
    CHECK(offsetof(struct vring_avail, flags) == 0);
    CHECK(offsetof(struct vring_avail, idx)   == 2);
    CHECK(offsetof(struct vring_avail, ring)  == 4);
    CHECK(sizeof(struct vring_avail) == 4 + 2 * VNET_Q_SIZE);

    /* Used ring element is 8 bytes; used ring header is 4 bytes. */
    CHECK(sizeof(struct vring_used_elem) == 8);
    CHECK(offsetof(struct vring_used, flags) == 0);
    CHECK(offsetof(struct vring_used, idx)   == 2);
    CHECK(offsetof(struct vring_used, ring)  == 4);
    CHECK(sizeof(struct vring_used) == 4 + 8 * VNET_Q_SIZE);

    /* A frame buffer must hold the header plus a full Ethernet frame. */
    CHECK(VNET_HDR_LEN + 1518 <= 2048);

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
