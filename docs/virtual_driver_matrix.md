# Virtual Hardware Driver Matrix

AuraLite includes a virtual hardware compatibility probe (`drivers/vm/`) that
recognises common QEMU, VirtualBox and VMware PCI devices at boot. The probe is
not a replacement for real data-path drivers; it reports whether a device is
active, partial, framebuffer-only, or known but not implemented yet.

Boot log prefix:

```text
[vmdrv]
```

## Active / usable data paths

| Device family | Common IDs | Platforms | AuraLite status |
|---|---|---|---|
| Intel e1000 82540EM/82545EM/82543GC | `8086:100e`, `8086:100f`, `8086:1004` | QEMU, VirtualBox, VMware | Active network driver. |
| AHCI SATA | class `01/06`, e.g. `8086:2922` | QEMU, VirtualBox, VMware | Active DMA sector read/write. |
| UHCI USB 1.1 | class `0c/03/00`, e.g. `8086:7020` | QEMU, VirtualBox, VMware | Active control/bulk backend; USB MSC works through UHCI. |
| Framebuffer provided by Limine | firmware-provided | all | Active boot framebuffer; no native SVGA acceleration. |
| PS/2 keyboard/mouse | legacy controller | all | Active input path. |

## Partial drivers / bring-up only

| Device family | Common IDs | Status |
|---|---|---|
| OHCI USB | class `0c/03/10` | Controller/port detection; transfer backend WIP. |
| EHCI USB 2.0 | class `0c/03/20` | Controller/port detection; transfer backend WIP. |
| xHCI USB 3.x | class `0c/03/30`, `1b36:000d`, `1033:0194` | Controller/ring scaffolding; transfer backend WIP. |

## Known/probed but no functional data path yet

| Category | Devices recognised |
|---|---|
| Alternative NICs | Intel e1000e `8086:10d3`, AMD PCnet `1022:2000`, RTL8139 `10ec:8139`, VMware VMXNET3 `15ad:07b0`, virtio-net `1af4:1000/1041`. |
| Alternative storage | PIIX IDE `8086:7010/7111`, virtio-blk `1af4:1001/1042`, virtio-scsi `1af4:1004/1048`, VMware PVSCSI `15ad:07c0`, LSI SCSI/SAS, BusLogic. |
| GPUs | VMware SVGA II, VirtualBox VMSVGA/VBoxVGA, QXL, virtio-gpu. These currently rely on Limine's framebuffer only. |
| Audio | AC'97, Intel HDA, Ensoniq ES1371. |
| Guest tools / paravirt | VirtualBox Guest Device, VMware VMCI, virtio-balloon, virtio-rng, virtio-console. |

## Recommended VM settings

For best current functionality, configure VMs with:

- NIC: Intel PRO/1000 MT Desktop or legacy VMware `e1000`.
- Storage: SATA/AHCI disk.
- USB storage tests: UHCI-compatible USB storage.
- Display: any mode that gives Limine a framebuffer; VBoxSVGA/VMSVGA/VMware SVGA
  are fine for framebuffer boot, but no accelerated native driver exists yet.

## What the probe helps with

If networking or storage does not work, check the `[vmdrv]` boot output. It will
show whether the hypervisor exposed a supported active device or a known device
that still needs a driver. For example, VMXNET3 will be detected but reported as
`known / no data path`, meaning the VM should be switched to legacy `e1000` for
now.
