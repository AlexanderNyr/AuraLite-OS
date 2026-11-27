#ifndef AURALITE_AHCI_FLUSH_H
#define AURALITE_AHCI_FLUSH_H
#include <stdint.h>
#define ATA_FLUSH_CACHE 0xE7
#define ATA_FLUSH_CACHE_EXT 0xEA
/* IDENTIFY word 83: validity bits 15:14 == 01, bits 12/13 advertise
 * FLUSH CACHE / FLUSH CACHE EXT. Prefer EXT, never invent support. */
static inline uint8_t ahci_flush_opcode(uint16_t word83) {
    if ((word83 & 0xC000) != 0x4000) return 0;
    if (word83 & (1u << 13)) return ATA_FLUSH_CACHE_EXT;
    if (word83 & (1u << 12)) return ATA_FLUSH_CACHE;
    return 0;
}
static inline int ahci_is_flush(uint8_t cmd) {
    return cmd == ATA_FLUSH_CACHE || cmd == ATA_FLUSH_CACHE_EXT;
}
#endif
