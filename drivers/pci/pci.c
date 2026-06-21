/* pci.c — PCI configuration space access via I/O ports 0xCF8/0xCFC. */

#include <stdint.h>
#include "drivers/pci/pci.h"
#include "kernel/arch/x86_64/portio.h"

uint32_t pci_config_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8)
                  | ((uint32_t)off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off,
                      uint32_t val) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8)
                  | ((uint32_t)off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

uint16_t pci_get_vendor(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint16_t)(pci_config_read(bus, dev, func, 0) & 0xFFFF);
}

uint16_t pci_get_device(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint16_t)(pci_config_read(bus, dev, func, 0) >> 16);
}

uint8_t pci_get_class(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint8_t)(pci_config_read(bus, dev, func, 0x08) >> 24);
}

uint32_t pci_get_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_idx) {
    return pci_config_read(bus, dev, func, 0x10 + bar_idx * 4);
}

void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t cmd = pci_config_read(bus, dev, func, 0x04);
    /* Bit 2 = bus master, bit 1 = memory space, bit 0 = I/O space. */
    pci_config_write(bus, dev, func, 0x04, cmd | 0x06);
}

int pci_find_device(uint16_t vendor, uint16_t device,
                    uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_func) {
    for (uint8_t bus = 0; bus < 1; bus++) {      /* scan bus 0 only */
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                if (pci_get_vendor(bus, dev, func) == vendor &&
                    pci_get_device(bus, dev, func) == device) {
                    if (out_bus)  *out_bus  = bus;
                    if (out_dev)  *out_dev  = dev;
                    if (out_func) *out_func = func;
                    return 0;
                }
            }
        }
    }
    return -1;
}

uint8_t pci_get_subclass(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint8_t)(pci_config_read(bus, dev, func, 0x08) >> 16) & 0xFF;
}

uint8_t pci_get_prog_if(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint8_t)(pci_config_read(bus, dev, func, 0x08) >> 8) & 0xFF;
}

int pci_find_class(uint8_t class_code, uint8_t subclass,
                   uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_func) {
    for (uint8_t bus = 0; bus < 1; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                if (pci_get_vendor(bus, dev, func) == 0xFFFF) continue;
                if (pci_get_class(bus, dev, func) == class_code &&
                    pci_get_subclass(bus, dev, func) == subclass) {
                    if (out_bus)  *out_bus  = bus;
                    if (out_dev)  *out_dev  = dev;
                    if (out_func) *out_func = func;
                    return 0;
                }
            }
        }
    }
    return -1;
}
