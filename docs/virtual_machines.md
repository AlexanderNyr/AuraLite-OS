# AuraLite OS in VirtualBox and VMware

AuraLite boots from the same hybrid ISO — built by its own BIOS+UEFI loader — in
QEMU, VirtualBox, VMware and
on USB. The key is to select virtual hardware that the kernel already supports.

## Build

```bash
make iso
```

Output:

```text
build/auralite.iso
```

## VirtualBox

Automated setup, if `VBoxManage` is installed:

```bash
make vbox
VBoxManage startvm AuraLite-OS
```

If `VBoxManage` is not installed, `make vbox` writes manual setup notes to:

```text
vm/virtualbox/README-VirtualBox.txt
```

The script also copies the ISO to:

```text
vm/virtualbox/auralite.iso
```

Recommended manual settings:

| Setting | Value |
|---|---|
| Type | Other / Other 64-bit |
| Firmware | BIOS |
| RAM | 512 MiB or more |
| CPUs | 4 |
| Chipset | PIIX3 |
| Graphics | VBoxSVGA |
| Boot medium | `build/auralite.iso` as optical drive |
| Network | NAT |
| Adapter type | **Intel PRO/1000 MT Desktop (82540EM)** |
| Serial | COM1 redirected to `serial.log` |
| Storage | SATA/AHCI disk if you want `/disk`, `/fat` or `/ext2` |

The 82540EM adapter has PCI ID `8086:100e`, which AuraLite's `e1000` driver
supports.

## VMware Workstation / Fusion / Player

Generate a `.vmx`:

```bash
make vmware
```

Open the generated VMX in VMware:

```text
vm/vmware/AuraLite-OS.vmwarevm/AuraLite-OS.vmx
```

The script bundles a copy of the ISO next to the VMX:

```text
vm/vmware/AuraLite-OS.vmwarevm/auralite.iso
```

Important VMX settings:

```text
guestOS = "other-64"
firmware = "bios"
ethernet0.virtualDev = "e1000"
ide1:0.deviceType = "cdrom-image"
```

VMware's legacy `e1000` adapter is typically an Intel 82545EM-compatible NIC
with PCI ID `8086:100f`, which AuraLite's `e1000` driver now accepts.

Do **not** switch the VMware NIC to `vmxnet3` or `e1000e` unless AuraLite gains
a driver for those devices.

## Supported virtual NICs

AuraLite's current virtual networking driver is `drivers/e1000/`, and it
supports these common 8254x emulator IDs:

| Hypervisor | Recommended adapter | PCI ID |
|---|---|---|
| QEMU | e1000 / 82540EM | `8086:100e` |
| VirtualBox | Intel PRO/1000 MT Desktop | `8086:100e` |
| VirtualBox | Intel PRO/1000 MT Server | `8086:100f` |
| VirtualBox | Intel PRO/1000 T Server | `8086:1004` |
| VMware | e1000 / 82545EM | `8086:100f` |

Unsupported for now:

- VirtualBox PCnet adapters
- virtio-net
- VMware vmxnet3
- Intel e1000e unless tested and explicitly added

## Display, input, storage and USB

AuraLite uses the framebuffer supplied by its own bootloader, so it does not require a
native VirtualBox/VMware SVGA driver for basic graphics or the built-in GUI.
PS/2 keyboard and mouse are used for normal input; USB controllers may also be
exposed for the USB stack's probing and enumeration tests.

Writable storage is best exercised with AHCI/SATA disks. The first AHCI disk is
used for `/disk` and `/fat`; a second AHCI disk enables `/ext2`.

USB Mass Storage is currently ready through the UHCI backend. In QEMU this is
exercised by:

```bash
make run-usb-msc
```

In VirtualBox/VMware, prefer exposing USB 1.1/UHCI-compatible storage if you
want to test AuraLite's MSC path.  xHCI-attached storage also works (the U5
bulk rings are real); devices behind OHCI/EHCI may be detected but are not
yet usable by the MSC class driver (ledger RES-38).

## Serial output

Both generated VM configs redirect COM1 to a file:

- VirtualBox: `vm/virtualbox/serial.log`
- VMware: `vm/vmware/AuraLite-OS.vmwarevm/serial.log`

This captures the same UART output used by QEMU's `-serial stdio` mode.

## WHPX (QEMU on Windows with Hyper-V acceleration)

`qemu-system-x86_64 -accel whpx` boots the same ISO with the host
CPU's real feature set — which matters for exactly one receipt: a
WHPX boot reports `pcid=1 invpcid=0` in the `[cpu] features:` line,
making it the only known lane that executes the PCID path (RESIDUE
R11; TCG refuses `+pcid` outright).  On such a boot the kernel prints
`[vmm] PCID enabled (hash-slot alloc, CR3-reload+generation fallback,
no invpcid)` and `/proc/perf`'s `cr3_noflush_switches` starts moving.
That is the D-PCID-5 acceptance — see
[`metal_receipts.md`](metal_receipts.md), slots 5/6, for the exact
paste-back protocol.
