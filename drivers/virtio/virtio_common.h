#ifndef AURALITE_VIRTIO_COMMON_H
#define AURALITE_VIRTIO_COMMON_H

#include <stdint.h>
#include <stddef.h>

/* Virtqueue descriptor flags */
#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[256]; /* Max size for simplicity */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[256];
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

#endif /* AURALITE_VIRTIO_COMMON_H */
