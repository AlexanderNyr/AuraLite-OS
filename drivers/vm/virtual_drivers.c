/* virtual_drivers.c — virtual hardware compatibility/probe layer.
 *
 * QEMU, VirtualBox and VMware can expose many different virtual devices for
 * the same role (network, storage, GPU, guest tools). AuraLite only has full
 * data paths for a subset, but knowing what the VM actually exposed makes boot
 * diagnostics dramatically better. This module detects common virtual devices
 * and prints whether AuraLite has an active driver, a partial driver, or only a
 * probe entry.
 */

#include <stdint.h>
#include "drivers/vm/virtual_drivers.h"
#include "drivers/pci/pci.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"

struct known_pci_device {
    uint16_t vendor;
    uint16_t device;
    const char *name;
    const char *driver;
    const char *status;
};

static const struct known_pci_device known[] = {
    /* Network adapters. */
    {0x8086, 0x100e, "Intel 82540EM e1000 / PRO/1000 MT Desktop", "e1000", "active"},
    {0x8086, 0x100f, "Intel 82545EM e1000 / PRO/1000 MT Server", "e1000", "active"},
    {0x8086, 0x1004, "Intel 82543GC e1000 / PRO/1000 T Server", "e1000", "active"},
    {0x8086, 0x10d3, "Intel 82574L e1000e", "e1000e", "known / no data path"},
    {0x1022, 0x2000, "AMD PCnet-PCI II / Am79C970A", "pcnet32", "known / no data path"},
    {0x10ec, 0x8139, "Realtek RTL8139", "rtl8139", "known / no data path"},
    {0x15ad, 0x07b0, "VMware VMXNET3", "vmxnet3", "known / no data path"},
    {0x1af4, 0x1000, "Virtio transitional network", "virtio-net", "known / no data path"},
    {0x1af4, 0x1041, "Virtio modern network", "virtio-net", "known / no data path"},

    /* Storage controllers. */
    {0x8086, 0x2922, "Intel ICH9 AHCI SATA", "ahci", "active"},
    {0x8086, 0x2829, "Intel ICH8/ICH9 AHCI SATA", "ahci", "active"},
    {0x8086, 0x7010, "Intel PIIX3 IDE", "ata-piix", "known / no data path"},
    {0x8086, 0x7111, "Intel PIIX4 IDE", "ata-piix", "known / no data path"},
    {0x1af4, 0x1001, "Virtio transitional block", "virtio-blk", "known / no data path"},
    {0x1af4, 0x1042, "Virtio modern block", "virtio-blk", "known / no data path"},
    {0x1af4, 0x1004, "Virtio transitional SCSI", "virtio-scsi", "known / no data path"},
    {0x1af4, 0x1048, "Virtio modern SCSI", "virtio-scsi", "known / no data path"},
    {0x15ad, 0x07c0, "VMware PVSCSI", "pvscsi", "known / no data path"},
    {0x1000, 0x0054, "LSI Logic SAS 1068", "mpt-sas", "known / no data path"},
    {0x1000, 0x0030, "LSI Logic 53C1030", "mpt-scsi", "known / no data path"},
    {0x104b, 0x1040, "BusLogic MultiMaster", "buslogic", "known / no data path"},

    /* Display adapters / GPUs. */
    {0x1234, 0x1111, "QEMU/Bochs std VGA", "limine-framebuffer", "boot framebuffer"},
    {0x80ee, 0xbeef, "VirtualBox VMSVGA/VBoxVGA", "vboxvideo", "boot framebuffer"},
    {0x15ad, 0x0405, "VMware SVGA II", "vmware-svga", "boot framebuffer"},
    {0x15ad, 0x0710, "VMware SVGA II", "vmware-svga", "boot framebuffer"},
    {0x1b36, 0x0100, "QXL paravirtual GPU", "qxl", "known / no data path"},
    {0x1af4, 0x1050, "Virtio GPU", "virtio-gpu", "known / no data path"},

    /* USB host controllers commonly seen in VMs. */
    {0x8086, 0x7020, "Intel PIIX3 UHCI", "uhci", "active"},
    {0x8086, 0x7112, "Intel PIIX4 UHCI", "uhci", "active"},
    {0x8086, 0x293a, "Intel ICH9 UHCI", "uhci", "active"},
    {0x8086, 0x293c, "Intel ICH9 USB2 EHCI", "ehci", "partial"},
    {0x1b36, 0x000d, "QEMU xHCI", "xhci", "partial"},
    {0x1033, 0x0194, "NEC/Renesas xHCI", "xhci", "partial"},
    {0x106b, 0x003f, "Apple/VMware OHCI", "ohci", "partial"},

    /* Audio. */
    {0x8086, 0x2415, "Intel/ICH AC'97 Audio", "ac97", "known / no data path"},
    {0x8086, 0x2668, "Intel HD Audio", "hda", "known / no data path"},
    {0x1274, 0x1371, "Ensoniq ES1371", "es1371", "known / no data path"},

    /* Guest/balloon/paravirtual devices. */
    {0x80ee, 0xcafe, "VirtualBox Guest Device", "vboxguest", "known / no data path"},
    {0x15ad, 0x0740, "VMware VMCI", "vmci", "known / no data path"},
    {0x1af4, 0x1002, "Virtio transitional balloon", "virtio-balloon", "known / no data path"},
    {0x1af4, 0x1045, "Virtio modern balloon", "virtio-balloon", "known / no data path"},
    {0x1af4, 0x1005, "Virtio RNG", "virtio-rng", "known / no data path"},
    {0x1af4, 0x1044, "Virtio RNG modern", "virtio-rng", "known / no data path"},
    {0x1af4, 0x1003, "Virtio console", "virtio-console", "known / no data path"},
};

static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(subleaf));
}

static void detect_hypervisor(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 31))) {
        kprintf("[vmdrv] hypervisor CPUID bit not set\n");
        return;
    }

    char vendor[13];
    cpuid(0x40000000, 0, &a, &b, &c, &d);
    memcpy(vendor + 0, &b, 4);
    memcpy(vendor + 4, &c, 4);
    memcpy(vendor + 8, &d, 4);
    vendor[12] = 0;
    kprintf("[vmdrv] hypervisor: %s\n", vendor);
}

static const struct known_pci_device *lookup(uint16_t vendor, uint16_t device) {
    for (uint32_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (known[i].vendor == vendor && known[i].device == device) return &known[i];
    }
    return 0;
}

static const char *class_name(uint8_t cls, uint8_t sub, uint8_t prog_if) {
    if (cls == 0x01 && sub == 0x06) return "SATA/AHCI storage";
    if (cls == 0x01 && sub == 0x08) return "NVMe storage";
    if (cls == 0x01) return "mass-storage controller";
    if (cls == 0x02) return "network controller";
    if (cls == 0x03) return "display controller";
    if (cls == 0x04 && sub == 0x01) return "audio controller";
    if (cls == 0x0c && sub == 0x03 && prog_if == 0x00) return "UHCI USB controller";
    if (cls == 0x0c && sub == 0x03 && prog_if == 0x10) return "OHCI USB controller";
    if (cls == 0x0c && sub == 0x03 && prog_if == 0x20) return "EHCI USB controller";
    if (cls == 0x0c && sub == 0x03 && prog_if == 0x30) return "xHCI USB controller";
    if (cls == 0x06) return "bridge/controller";
    return "other PCI device";
}

void virtual_drivers_init(void) {
    detect_hypervisor();
    kprintf("[vmdrv] scanning common QEMU/VirtualBox/VMware PCI devices...\n");

    int seen = 0, active = 0, partial = 0, missing = 0;
    for (uint8_t bus = 0; bus < 1; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = pci_get_vendor(bus, dev, func);
                if (vendor == 0xFFFF) continue;
                uint16_t device = pci_get_device(bus, dev, func);
                uint8_t cls = pci_get_class(bus, dev, func);
                uint8_t sub = pci_get_subclass(bus, dev, func);
                uint8_t prog = pci_get_prog_if(bus, dev, func);
                const struct known_pci_device *k = lookup(vendor, device);
                seen++;
                if (k) {
                    kprintf("[vmdrv] PCI %u:%u.%u %04x:%04x %-28s driver=%s status=%s\n",
                            bus, dev, func, vendor, device, k->name, k->driver, k->status);
                    if (strcmp(k->status, "active") == 0 || strcmp(k->status, "boot framebuffer") == 0) active++;
                    else if (strcmp(k->status, "partial") == 0) partial++;
                    else missing++;
                } else {
                    kprintf("[vmdrv] PCI %u:%u.%u %04x:%04x class=%02x/%02x/%02x %s\n",
                            bus, dev, func, vendor, device, cls, sub, prog,
                            class_name(cls, sub, prog));
                }
            }
        }
    }
    kprintf("[vmdrv] summary: %d PCI devices, %d active/bootfb, %d partial, %d known missing\n",
            seen, active, partial, missing);
}
