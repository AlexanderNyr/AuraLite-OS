# USB

Status of the USB stack after `USB_PLAN.md` phases U0–U9.

This document follows the convention of `docs/win32.md`: the second table —
what is **approximated or absent** — matters more than the first. Every row
is checked against the source by `tools/check_usb_claims.py`, which CI runs;
the documentation cannot silently rot again.

## Controllers

| Controller | Status | Notes |
|---|---|---|
| UHCI | real | control, bulk, interrupt, isoc |
| OHCI | real | TD + ED, `ohci_run_transfer` |
| EHCI | real | async qTD; interrupt endpoints on the periodic schedule (U7) |
| xHCI | real | slots, control, bulk, interrupt, nested hubs (U3–U6, U9) |

## What works

| Capability | Evidence |
|---|---|
| Event ring, command ring | `test_xhci_ring` (unit, 24/24); No-Op 256/256 across a wrap |
| Enable Slot / Address Device | `Slot State=Addressed` read back from the device context |
| Control transfers | descriptors, strings, short packets via residue, stall recovery |
| Bulk transfers, MSC | `test_xhci_bulk`: `dd` pattern in sector 0 read back byte-for-byte |
| Interrupt endpoints, HID | `test_usb_hid_input`: HID usage codes, with a no-USB control run |
| Runtime hotplug | `test_usb_hotplug` 5/5 — attach, enumerate, bind, detach |
| Nested hubs | `test_usb_hub_depth`: device two hubs deep, route string `0x00011` |
| Slot lifecycle | `xhci_free_device()` on detach; every error path unwinds |

## What is approximated, and what is absent

This is the honest half of the document.

| Gap | State | Why it matters |
|---|---|---|
| **Event ring is not drained by the IRQ** | approximated | The xHCI interrupt is taken and acknowledged (U8), but the ring is still consumed by the polling thread. Hotplug latency is therefore ~400 ms — the poll — not interrupt time. |
| **Event ring has no lock** | absent | The consumer state (`event_ring_idx`, cycle bit, parked list) is unsynchronised. This is the blocker for draining in the handler; doing so first would trade latency for rare event loss. |
| **IRQ sharing** | absent (kernel) | `irq_register_handler()` keeps one handler per line with no chaining. xHCI initialises after e1000 and replaces its handler on the shared line. Both still work only because both are also polled. |
| **MSI/MSI-X** | absent | xHCI uses pin-based INTx. |
| **Isochronous** | approximated | Real Isoch TRBs with SIA (U9), but no frame-accurate scheduling, no iTD/siTD on EHCI, and no sample-accurate audio (decision D7). |
| **EHCI split transactions** | untested | TT hub address/port and S-mask/C-mask are programmed, but QEMU cannot build the case: `usb-hub` is full-speed and QEMU refuses to attach it to an EHCI bus. Implemented, unproven. |
| **Streams / UAS** | absent | Mass storage uses Bulk-Only Transport. |
| **USB-C / Power Delivery** | absent | No PD negotiation, no alternate modes. |
| **Suspend / resume** | partial | Port suspend and resume exist; no device-level power management. |
| **`i8042=off` boot** | broken (kernel) | The kernel hangs initialising the PS/2 controller when the i8042 is absent, so a USB-only input configuration cannot boot. |

## Device location encoding

A device's location is its root port plus the chain of hub ports taken to
reach it (`usb_core.h`):

```
bits  0-7   root port (0-based, as usb_core stores it)
bits  8-11  first hub port
bits 12-15  second hub port
     ...    up to five tiers — USB's own depth limit
```

The upper 20 bits are the xHCI route string verbatim. Before U9 the whole
location was one byte, which could not represent a second tier: a hub two
levels down computed its own location for its children, and every device
behind it was dropped as a duplicate.

## The plan's own metric

| | baseline | after U9 |
|---|---|---|
| Fabricated-but-green assertions | 8 (`test_usb_xhci`) | **0** |
| `SYNTHETIC` markers in a boot log | 7 | **0** |
| Red USB cases | 2 (`test_usb_hotplug`, `test_tls_errno` flake) | 0 |

`test_usb_xhci` passes with the same eight assertions it had at the
baseline — the difference is that they are now backed by data from the
device rather than by the driver agreeing with itself.
