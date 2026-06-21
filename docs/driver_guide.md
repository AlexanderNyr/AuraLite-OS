# AuraLite OS Driver Guide

## Driver inventory

| Driver | Location | Phase | Purpose |
|--------|----------|-------|---------|
| UART (16550) | `drivers/uart/` | 1 | COM1 serial console (TX + RX) |
| Framebuffer | `drivers/framebuffer/fb.c` | 1 | Linear FB text console (8×8 font) |
| Graphics | `drivers/framebuffer/graphics.c` | 14 | Double-buffered 2D rendering |
| Keyboard | `drivers/keyboard/` | 14 | PS/2 scan-code set 1 (IRQ 1) |
| PIT (8254) | `drivers/timer/` | 6 | 100 Hz periodic timer (IRQ 0) |
| PCI bus | `drivers/pci/` | 13 | Config-space access, device scan |
| e1000 NIC | `drivers/e1000/` | 13 | Intel 82540EM TX/RX |

## UART (16550)

Port-mapped I/O at `0x3F8`. Initialised at 115200 baud (divisor 1).

- `uart_putchar(char)` — busy-waits for THRE (transmit holding register empty),
  then writes to THR.
- `uart_has_data()` / `uart_getchar()` — poll LSR bit 0 (data ready) and read RBR.
- Used by `kprintf` for serial output and by the shell's `SYS_READ(fd=0)` for input.

## Framebuffer

Limine provides a 32-bpp RGB linear framebuffer. The console driver renders an
8×8 public-domain bitmap font into it. `kputchar()` fans out to both UART and
framebuffer.

## Graphics library (Phase 14)

Built on top of the raw framebuffer, adds double-buffering:

- A back buffer is allocated via `kmalloc` (same size as the framebuffer).
- All drawing operations (`gfx_putpixel`, `gfx_fill_rect`, `gfx_draw_line`,
  `gfx_draw_string`) write to the back buffer.
- `gfx_flip()` copies the back buffer to the visible framebuffer with `memcpy`,
  avoiding tearing during multi-element redraws.

Colour packing: `make_color(rgb)` converts a `0x00RRGGBB` value to the
framebuffer's actual mask layout (red/green/blue shifts from Limine).

## PS/2 keyboard (Phase 14)

Uses scan-code set 1 (the default). IRQ 1 (vector 33) fires when a byte is
available at port `0x60`.

- Extended codes (`0xE0` prefix) are tracked.
- Key-release (high bit set) is ignored.
- Press events are translated to ASCII via a US QWERTY scancode table and
  stored in a ring buffer (`keyboard_getchar()`).

## PIT timer (Phase 6)

Channel 0 in mode 3 (square wave), divisor = `1193182 / 100 ≈ 11932`, giving
~100 Hz. IRQ 0 handler bumps a global tick counter and calls `sched_tick()`
for preemptive scheduling.

## PCI (Phase 13)

Type-0 configuration access via I/O ports `0xCF8` (address) / `0xCFC` (data):

```
addr = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC)
```

`pci_find_device(vendor, device, ...)` scans bus 0 for a matching device.
`pci_enable_bus_master()` sets the command register so the device can DMA.

## e1000 NIC (Phase 13)

The Intel 82540EM is QEMU's default NIC (`-device e1000`).

### MMIO

The NIC's register file is at BAR0 (physical `0xFEBC0000`). Since this is beyond
the HHDM's RAM range, it is explicitly mapped via `paging_map()`:

```c
for (off = 0; off < 0x20000; off += 0x1000)
    paging_map(hhdm + mmio_phys + off, mmio_phys + off, RW);
mmio = (volatile uint32_t *)(hhdm + mmio_phys);
```

### Descriptor rings

Legacy (non-split) TX/RX descriptor rings, each with 8 entries of 16 bytes.
Both the rings and the packet buffers are allocated from the PMM (physical
frames) so the NIC can DMA to them. The kernel accesses them through the HHDM.

Descriptors are declared `volatile` because the NIC writes to them via DMA.

### RX polling

The recv function polls the RDH MMIO register (not the descriptor status byte,
which is unreliable through the HHDM due to DMA ordering):

```c
uint32_t rdh = mmio_read(E1000_RDH);
if (rdh == last_rdh) return 0;   // no new packet
```

### TX

Copies the packet into a pre-allocated buffer, sets `cmd = 0x0B` (EOP + IFCS +
RS), writes TDT, and polls the descriptor status for the DD (descriptor done) bit.

### MAC address

Read from the EEPROM (3 × 16-bit words). Falls back to RAL/RAH registers, then
the QEMU default MAC if both fail.

## Network stack (`kernel/net/net.c`)

```
Application: net_ping(target_ip)
   │
   ├── ARP: eth_send(broadcast, ARP_REQUEST)
   │        poll e1000_recv() for ARP_REPLY → cache gateway MAC
   │
   ├── Build ICMP echo request (type 8, ident 0x1234, seq 1)
   ├── Build IPv4 header (version 4, IHL 5, TTL 64, protocol ICMP)
   │        checksum per RFC 1071
   ├── Build Ethernet frame (dst=gateway MAC, ethertype 0x0800)
   ├── e1000_send(frame)
   │
   └── Poll e1000_recv() for ICMP_ECHO_REPLY (type 0)
```

Our IP: `10.0.2.15` (QEMU user-mode default). Gateway: `10.0.2.2`.
