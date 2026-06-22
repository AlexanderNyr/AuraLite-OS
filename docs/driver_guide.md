# AuraLite OS Driver Guide

This guide describes the drivers and low-level protocol layers currently present
in the tree. Some entries are complete runtime drivers; others are protocol
frameworks or controller bring-up code intended for later expansion.

See also [`status.md`](status.md) for a feature matrix.

## Driver inventory

| Area | Location | Status | Purpose |
|---|---|---:|---|
| UART 16550 | `drivers/uart/` | ✅ | COM1 serial console and stdin source. |
| Framebuffer console | `drivers/framebuffer/fb.c` | ✅ | Text output on Limine framebuffer. |
| 2D graphics | `drivers/framebuffer/graphics.c` | ✅ | Double-buffered pixel/rect/line/text drawing. |
| Boot splash | `drivers/framebuffer/bootsplash.c` | 🧪 | Animated graphical boot screen helpers. |
| Window manager | `drivers/framebuffer/wm.c` | ✅/🧪 | Demo compositor, windows, widgets, mouse interaction. |
| 3D renderer | `drivers/framebuffer/render3d.c` | 🧪 | Software mesh/wireframe/flat-shaded demo. |
| PS/2 keyboard | `drivers/keyboard/` | ✅ | IRQ 1, scan-code set 1, ASCII ring buffer. |
| PS/2 mouse | `drivers/mouse/` | ✅ | IRQ 12, 3-byte packet parser, cursor state. |
| PIT | `drivers/timer/` | ✅ | 100 Hz system tick and sleeps. |
| PCI | `drivers/pci/` | ✅ | Config-space reads/writes and simple scanning. |
| e1000 NIC | `drivers/e1000/` | ✅ | Intel 8254x legacy TX/RX rings. |
| AHCI | `drivers/ahci/` | ✅/🧪 | SATA controller/port setup and DMA sector read/write self-test. |
| UHCI | `drivers/usb/uhci.c` | ✅/🧪 | USB 1.1 controller + CONTROL/BULK TD/QH transfers. |
| OHCI | `drivers/usb/ohci.c` | 🚧 | Controller/port bring-up. |
| EHCI | `drivers/usb/ehci.c` | 🚧 | Controller/port bring-up. |
| xHCI | `drivers/usb/xhci.c` | 🚧 | Controller/ring scaffolding and port detection. |
| USB core | `drivers/usb/usb_core.c` | 🧪 | UHCI enumeration + descriptor framework; other backends WIP. |
| USB MSC | `drivers/usb/msc.c` | 🧪 | Bulk-Only/SCSI read/write path through UHCI. |
| Bluetooth HCI | `drivers/bluetooth/` | 🚧 | HCI command/event protocol over USB. |
| Wi-Fi 802.11 | `drivers/wifi/` | 🚧 | MAC management layer; no chipset driver by default. |

## UART

Location: `drivers/uart/`

- Port base: `0x3F8` (COM1).
- Baud rate: 115200.
- Used by `kprintf` and as shell stdin via `SYS_READ(fd=0)`.
- Driver model is polling-based.

Important functions:

```c
void uart_init(void);
void uart_putchar(char c);
int  uart_has_data(void);
char uart_getchar(void);
```

## Framebuffer and graphics

Limine provides a linear 32-bpp framebuffer. AuraLite has two layers on top:

1. `fb.c` — console text output, used by `kputchar`.
2. `graphics.c` — double-buffered drawing API for GUI demos.

The graphics layer allocates a back buffer with `kmalloc` and flips to the
front buffer using `memcpy`.

Core functions:

```c
void gfx_putpixel(uint32_t x, uint32_t y, color_t color);
void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, color_t c);
void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, color_t c);
void gfx_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, color_t c);
void gfx_draw_string(uint32_t x, uint32_t y, const char *s, color_t c);
void gfx_flip(void);
```

Compatibility helpers such as circles, gradients and centred text are provided
for the boot splash and GUI code.

## Keyboard

Location: `drivers/keyboard/`

- PS/2 keyboard, scan-code set 1.
- IRQ 1 through the PIC layer.
- Key releases are ignored.
- ASCII characters are stored in a ring buffer.

Limitations:

- no modifier state for Shift/Ctrl/Alt in the current ASCII map;
- no keyboard layout switching;
- no USB HID keyboard input path yet.

## Mouse

Location: `drivers/mouse/`

- PS/2 auxiliary device through the 8042 controller.
- IRQ 12.
- Parses 3-byte relative movement packets.
- Maintains absolute cursor position clamped to framebuffer bounds.

Used by the window-manager demo for focus, dragging and close buttons.

## PIT timer

Location: `drivers/timer/pit.c`

- Uses PIT channel 0 in mode 3.
- Default frequency: 100 Hz.
- IRQ 0 increments a global tick counter and drives scheduler preemption.

Future work:

- LAPIC timer per CPU;
- HPET or TSC-deadline timer for higher precision.

## PCI

Location: `drivers/pci/`

Config access uses I/O ports:

```text
0xCF8  PCI_CONFIG_ADDR
0xCFC  PCI_CONFIG_DATA
```

Current scanning is intentionally simple and scans bus 0 only. This is enough
for the default QEMU/VirtualBox/VMware test VMs, but not for complex PCIe
hierarchies.

## e1000 networking

Location: `drivers/e1000/`

The driver supports common virtual Intel 8254x adapters:

| Device | PCI ID | Common source |
|---|---|---|
| 82540EM | `8086:100e` | QEMU, VirtualBox PRO/1000 MT Desktop |
| 82545EM | `8086:100f` | VMware e1000, VirtualBox PRO/1000 MT Server |
| 82543GC | `8086:1004` | VirtualBox PRO/1000 T Server |

Implementation notes:

- legacy TX/RX descriptor rings;
- descriptor rings and packet buffers are PMM-allocated for DMA;
- MMIO BAR0 is explicitly mapped because device MMIO is not covered by HHDM;
- I/O is polling-based;
- RX polls RDH rather than relying only on descriptor status visibility.

Unsupported virtual NICs:

- virtio-net;
- VMware vmxnet3;
- Intel e1000e unless a dedicated compatible path is added;
- VirtualBox PCnet adapters.

## AHCI

Location: `drivers/ahci/`

Current implemented pieces:

- PCI class-code detection for SATA AHCI;
- ABAR MMIO mapping;
- AHCI enable bit;
- implemented-port scan;
- SATA signature detection;
- command list, FIS receive area and command table allocation.

Current status:

- DMA `READ DMA EXT` and `WRITE DMA EXT` work in the QEMU AHCI test setup;
- the boot self-test reads sector 0, writes a scratch signature to sector 1 and
  reads it back;
- `kernel/fs/diskfs.c` mounts a tiny persistent AHCI-backed filesystem at
  `/disk` when a SATA disk is present;
- broader real-hardware and non-QEMU hypervisor coverage is still experimental.

The earlier PxCI issue was fixed by programming the command header PRDTL field
correctly and by clearing sticky port error/interrupt state before issuing the
command.

## USB

Locations:

- `drivers/usb/uhci.c`
- `drivers/usb/ohci.c`
- `drivers/usb/ehci.c`
- `drivers/usb/xhci.c`
- `drivers/usb/usb_core.c`
- `drivers/usb/msc.c`

### UHCI

UHCI is the first USB path with a working class-driver data path:

- PCI detection;
- I/O base setup;
- controller reset/start;
- frame list allocation;
- root-port reset/enumeration;
- TD/QH structures;
- multi-packet control transfers;
- bulk transfers with persistent DATA toggle tracking;
- USB Mass Storage Bulk-Only Transport support.

### OHCI/EHCI/xHCI

These drivers currently provide controller detection, register mapping, basic
initialisation and port reporting. Their transfer engines are not fully wired to
`usb_core.c` yet.

### USB core

The core layer contains:

- USB setup packet structures;
- standard request builders;
- descriptor structures and parser helpers;
- global USB device table;
- class detection for HID/MSC/hub-like devices.

For UHCI ports, `usb_enumerate_all()` now performs the normal standard-device
sequence: `SET_ADDRESS`, full device descriptor, configuration descriptor parse,
and `SET_CONFIGURATION`. For OHCI/EHCI/xHCI, attached devices are still recorded
for diagnostics, but class drivers cannot use them until those controller
transfer backends are implemented.

### Mass Storage

`msc.c` implements:

- CBW and CSW structures;
- SCSI INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY, READ(10),
  WRITE(10) builders;
- Bulk-Only Transport execution over UHCI bulk endpoints;
- persistent bulk IN/OUT DATA toggle tracking;
- capacity detection and a sector-0 read self-test;
- high-level `msc_read` / `msc_write` APIs.

Current limitation: the working MSC backend is UHCI. Mass-storage devices behind
OHCI/EHCI/xHCI are detected/reported but not usable until those host-controller
transfer backends are completed.

## Bluetooth HCI

Location: `drivers/bluetooth/`

Implemented protocol pieces:

- HCI command packet builder;
- HCI event parser basics;
- Reset, Read BD_ADDR, Read Local Version, Inquiry commands.

Current limitation:

- depends on working USB device enumeration and bulk/control transfers.

## Wi-Fi 802.11

Location: `drivers/wifi/`

Implemented protocol pieces:

- 802.11 frame-control structures;
- management frame headers;
- Probe Request construction;
- Beacon/Probe Response IE parser;
- open-system authentication and association request construction;
- Ethernet-to-802.11 data-frame conversion;
- abstract `wifi_driver_t` callback interface.

Current limitation:

- no actual Intel/Realtek/Atheros chipset driver is registered by default.

## Adding a new driver

Recommended pattern:

1. Put code under `drivers/<name>/`.
2. Keep hardware register definitions local to the driver `.c` unless shared.
3. Add a small `<name>_init()` and `<name>_self_test()` if possible.
4. Use PMM frames for DMA buffers and HHDM pointers for CPU access.
5. Explicitly map device MMIO with `paging_map`; do not assume HHDM covers MMIO.
6. Avoid blocking forever on hardware bits; always use timeouts and print useful
   diagnostics.
7. Update this guide and `docs/status.md`.
