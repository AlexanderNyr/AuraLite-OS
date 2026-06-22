# AuraLite OS — Universal ISO

Use `auralite-universal.iso` for QEMU, VirtualBox, VMware, and USB/HDD-style
boot tests. The image is a Limine hybrid ISO.

## What is included

- AHCI DMA sector read/write support.
- `/tmp` writable in-memory tmpfs.
- `/disk` tiny persistent AHCI-backed read/write filesystem when an AHCI disk is attached.
- `/fat` FAT32 volume with persistent kernel log at `/fat/AURALOG.TXT`.
- UHCI USB Mass Storage read path.
- e1000 networking for QEMU/VirtualBox/VMware legacy Intel adapters.

## QEMU

```bash
qemu-system-x86_64 -cdrom auralite-universal.iso -m 512M -smp 4 \
  -vga std -serial stdio -netdev user,id=net0 -device e1000,netdev=net0 \
  -boot order=d
```

For AHCI testing, attach a raw disk through an AHCI controller. AuraLite's boot
self-test reads sector 0 and writes/reads scratch sector 1. `/disk` is mounted
on the first AHCI disk and supports small persistent files.

For USB Mass Storage testing, attach storage through UHCI. The MSC self-test
reads sector 0 from a USB storage device.

## VirtualBox

Recommended settings:

- Type: Other / Other 64-bit
- Firmware: BIOS recommended
- RAM: 512 MiB or more
- CPUs: 4
- Graphics: VBoxSVGA
- Optical drive: `auralite-universal.iso`
- Network: NAT
- Adapter type: Intel PRO/1000 MT Desktop (82540EM)
- Optional storage: SATA/AHCI disk for `/disk`

## VMware

Recommended settings:

- Guest OS: Other 64-bit
- Firmware: BIOS recommended
- RAM: 512 MiB or more
- CPUs: 4
- CD/DVD image: `auralite-universal.iso`
- Network adapter: legacy `e1000`
- Optional storage: SATA/AHCI disk for `/disk`

Do not use unsupported NICs such as VirtualBox PCnet, virtio-net, VMware vmxnet3
or e1000e unless matching drivers are added.

## Checksum

See `SHA256SUMS`.
