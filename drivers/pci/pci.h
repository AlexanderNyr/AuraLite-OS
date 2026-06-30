#ifndef AURALITE_DRIVERS_PCI_PCI_H
#define AURALITE_DRIVERS_PCI_PCI_H

#include <stdint.h>

/*
 * PCI configuration space access (Type 0) via I/O ports 0xCF8/0xCFC.
 *
 * Address port (0xCF8) format:
 *   bit 31    : enable
 *   bits 30-24: reserved
 *   bits 23-16: bus number
 *   bits 15-11: device number
 *   bits 10-8 : function number
 *   bits 7-2  : register offset (must be 4-byte aligned)
 */

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* Read a 32-bit configuration word. */
uint32_t pci_config_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);

/* Write a 32-bit configuration word. */
void pci_config_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off,
                      uint32_t val);

/* Read a 16-bit vendor ID. */
uint16_t pci_get_vendor(uint8_t bus, uint8_t dev, uint8_t func);

/* Read a 16-bit device ID. */
uint16_t pci_get_device(uint8_t bus, uint8_t dev, uint8_t func);

/* Read a class code (byte 3 of the class/reg at offset 0x08). */
uint8_t pci_get_class(uint8_t bus, uint8_t dev, uint8_t func);

/* Read a BAR (base address register) value at offset 0x10+4*n. */
uint32_t pci_get_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_idx);

/* Read the legacy PCI interrupt line register (offset 0x3C, low byte). */
uint8_t pci_get_interrupt_line(uint8_t bus, uint8_t dev, uint8_t func);

/* Enable bus mastering (so the device can DMA). */
void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func);

/*
 * Scan PCI bus 0 for a device matching the given vendor/device IDs.
 * Sets out_bus / out_dev / out_func on success. Returns 0 on found.
 */
int pci_find_device(uint16_t vendor, uint16_t device,
                    uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_func);

/* Read the subclass byte (offset 0x09 of config space). */
uint8_t pci_get_subclass(uint8_t bus, uint8_t dev, uint8_t func);

/* Read the programming interface byte (offset 0x0A). */
uint8_t pci_get_prog_if(uint8_t bus, uint8_t dev, uint8_t func);

/*
 * Scan PCI bus 0 for a device matching the given class + subclass.
 * Returns 0 on found, -1 if not.
 */
int pci_find_class(uint8_t class_code, uint8_t subclass,
                   uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_func);

#endif /* AURALITE_DRIVERS_PCI_PCI_H */
