# Build and Run Guide

This guide covers the supported build outputs and emulator/hypervisor setups.

## Required tools

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install clang lld nasm qemu-system-x86 xorriso
```

Tool purposes:

| Tool | Used for |
|---|---|
| `clang` | Kernel and userspace C compilation. |
| `ld.lld` | Static ELF linking with custom linker scripts. |
| `nasm` | x86_64 assembly files. |
| `xorriso` | Bootable ISO creation. |
| `qemu-system-x86_64` | Local integration testing. |

## Build ISO

```bash
make iso
```

Main output:

```text
build/auralite.iso
```

The ISO is a Limine hybrid image with BIOS and UEFI boot files. BIOS is the
best-tested path.

## Build user programs only

```bash
make user
```

Outputs are under:

```text
build/user/*.elf
```

The initrd is built from these ELFs and mounted at `/` during boot.

## Build kernel only

```bash
make kernel
```

Output:

```text
build/kernel.elf
```

## Run in QEMU

```bash
make run
```

This uses `tools/run_qemu.sh`. The script creates a small raw AHCI test disk at
`build/disk.img` if needed and attaches it through a QEMU AHCI controller. The
kernel AHCI self-test reads sector 0 and verifies write/readback on scratch
sector 1. If AHCI is present, AuraLite also mounts `/disk` and a FAT32 volume
at `/fat`; kernel logs are appended to `/fat/AURALOG.TXT`.

If you want a simpler command without the AHCI test disk:

```bash
qemu-system-x86_64 \
  -cdrom build/auralite.iso \
  -m 512M \
  -smp 4 \
  -vga std \
  -display none \
  -serial stdio \
  -no-reboot \
  -cpu qemu64 \
  -netdev user,id=net0 \
  -device e1000,netdev=net0
```

## Run QEMU with USB Mass Storage

```bash
make run-usb-msc
```

This creates `build/usb-stick.img` if needed and attaches it as a UHCI
`usb-storage` device. During boot the USB core should enumerate the device, the
MSC layer should run SCSI `INQUIRY` / `READ CAPACITY`, and the MSC self-test
should read sector 0.

## Debug with GDB

```bash
make debug
```

Then attach from another terminal:

```bash
gdb build/kernel.elf -ex 'target remote :1234'
```

## Unit tests

```bash
make test-unit
```

The host-side unit tests cover:

- PMM bitmap allocation helpers.
- Generic heap allocator.
- String/memory functions.
- Bitmap primitives.
- Network utility functions.
- Formatting helpers.
- libc helpers.
- 3D math helpers.
- USB protocol structures.
- Window-manager hit-testing/layout helpers.

## USB image

```bash
make usb
```

Output:

```text
build/usb.img
```

The Limine ISO is already hybrid, so this target currently copies the ISO to a
raw image file. It can be booted as a disk image or written to a USB stick:

```bash
sudo dd if=build/usb.img of=/dev/sdX bs=4M status=progress
```

Replace `/dev/sdX` carefully.

## VirtualBox

```bash
make vbox
```

If `VBoxManage` is installed, this creates or updates a VM named `AuraLite-OS`.
If not, it writes manual setup notes to:

```text
vm/virtualbox/README-VirtualBox.txt
```

Use the bundled ISO:

```text
vm/virtualbox/auralite.iso
```

Required network adapter for current networking support:

```text
Intel PRO/1000 MT Desktop (82540EM)
```

## VMware

```bash
make vmware
```

Open the generated VMX:

```text
vm/vmware/AuraLite-OS.vmwarevm/AuraLite-OS.vmx
```

The VM directory contains its own ISO copy:

```text
vm/vmware/AuraLite-OS.vmwarevm/auralite.iso
```

Use legacy VMware `e1000` networking. Do not switch to `vmxnet3` or `e1000e`
unless matching AuraLite drivers are added.

## Common build problems

### `clang: No such file or directory`

Install the compiler:

```bash
sudo apt install clang
```

### `ld.lld: No such file or directory`

Install LLD:

```bash
sudo apt install lld
```

### `nasm: No such file or directory`

```bash
sudo apt install nasm
```

### `xorriso: command not found`

```bash
sudo apt install xorriso
```

### `limine/limine: Permission denied`

Make the vendored Limine tool executable:

```bash
chmod +x limine/limine
```

### `psf_font.inc file not found`

The current source tree expects:

```text
drivers/framebuffer/psf_font.inc
```

If it is missing, regenerate or restore it before building. It contains the
embedded PSF-style console font data used by `psf_font.h`.

### VirtualBox boots but networking fails

Check the adapter type. Use:

```text
Intel PRO/1000 MT Desktop (82540EM)
```

Do not use PCnet or virtio-net.

### VMware boots but networking fails

Check that the VMX contains:

```text
ethernet0.virtualDev = "e1000"
```

Do not use `vmxnet3` or `e1000e`.
