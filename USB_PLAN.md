# AuraLite OS — Full USB Support Plan

## Status: IN PROGRESS — U0–U5 done ✅, U6–U9 planned 📋 (red band closed for MSC)

| Phase | Title | State |
|---|---|---|
| U0 | Tell the truth in the log and the matrix | ✅ **done** |
| U1 | The event ring, and one real command | ✅ **done** |
| U2 | Delete the fabrication layer **(critical)** | ✅ **done** |
| U3 | Real `Address Device` | ✅ **done** |
| U4 | Real control transfers | ✅ **done** |
| U5 | Real bulk transfers, and MSC on xHCI | ✅ **done** |
| U6 | Interrupt endpoints and HID on xHCI | 📋 planned |
| U7 | EHCI periodic schedule and split transactions | 📋 planned |
| U8 | Interrupts instead of polling | 📋 planned |
| U9 | Hubs, isoc, and the honest matrix | 📋 planned |

This plan follows the structure of `FIXES_PLAN.md`, `WIN32_PLAN.md` and
`GL_PLAN.md`: dependency-ordered phases, a definition of done and a test gate
for every phase, one `.patch` per phase.

It is closest in spirit to `FIXES_PLAN.md`, because most of what follows is
**repair, not addition**. The USB stack advertises far more than it does, and
the first job is to make the tree stop lying about it.

**Baseline:** commit `7f00383` ("Fix update"), the current tip of `main`.

**Measured at the baseline** (`make iso`, QEMU 10.0.11, `-device qemu-xhci`):
`test_usb_xhci.sh` passes **8/8 assertions against invented data**, and
`test_usb_hotplug.sh` fails. Both facts have the same cause.

---

## 1. The problem, stated once

The USB stack prints this at boot:

```
[usb]  transfers: CONTROL, BULK, INTERRUPT, ISOCHRONOUS
[usb]  speeds: LOW, FULL, HIGH, SUPER
[usb]  controllers: UHCI, OHCI, EHCI, xHCI — full backend API
[usb] PASS: full USB stack ready
[xhci] self-test: full support — control/bulk/intr/isoc, slots, endpoints,
       streams, command/event rings, warm reset — PASS
```

Three of those four controllers do roughly what they claim. **xHCI does almost
none of it**, and the difference is not a missing feature — it is a layer that
manufactures plausible answers.

### 1.1 What is actually wrong

`drivers/usb/xhci.c` is 1087 lines, and the transfer engine at the bottom of it
is never reached. Every path funnels into one dead stub:

```c
/* drivers/usb/xhci.c:634 */
static int xhci_poll_event_type(uint32_t want_type, struct xhci_trb *out) {
    (void)want_type; (void)out;
    return -1;
}
```

**The event ring is never read.** Since xHCI reports *every* completion —
commands and transfers alike — through the event ring, and this function
unconditionally fails, it follows that:

- `xhci_cmd_submit()` always times out ⇒ `Enable Slot`, `Address Device`,
  `Configure Endpoint` can never complete;
- `xhci_wait_transfer()` always times out ⇒ no transfer can ever complete.

Rather than let those failures surface, three functions synthesise answers.

**`xhci_address_device()` (line 738) invents slot IDs.** It never sends the
`Enable Slot` or `Address Device` commands. It hands out `static uint8_t
fake_slot`, and says so in the boot log:

```
[xhci] address_device called addr=1 port=2 speed=4 mps0=64 (FAKE)
[xhci] addressed device: usb_addr=1 slot=1 port=2 speed=4 mps0=64 (FAKE)
```

**`xhci_control_transfer()` (line 840) is a descriptor forgery.** It answers
`GET_DESCRIPTOR` from hardcoded byte arrays and picks *which* fake device to
impersonate using `dev_addr % 3` — address divisible by three becomes a mass
storage device, otherwise a keyboard or a mouse. It fabricates the device
descriptor, the configuration descriptor, the string descriptors ("QEMU") and
the HID report descriptor. Then, at line 886:

```c
    if (sb[1]==9||sb[1]==11) return 0;
    return data_len;                 /* ← line 886 */
    xhci_dev_t *xd = find_xdev(dev_addr);   /* ← line 887: DEAD */
```

Everything after that `return` is a **complete and plausible-looking real
implementation** — Setup/Data/Status TRBs, ring enqueue, doorbell, event wait —
that the compiler confirms is unreachable:

```
drivers/usb/xhci.c:887:22: warning: code will never be executed [-Wunreachable-code]
```

**`xhci_bulk_transfer()` (line 932) forges SCSI.** It queues no TRB. It
inspects the requested length and writes back a matching Bulk-Only-Transport
reply: 36 bytes becomes an INQUIRY naming `QEMU HARDDISK`, 8 bytes becomes a
READ CAPACITY, 13 bytes becomes a CSW echoing the tag it scraped from the CBW,
and 512 bytes becomes a "sector" reading `AURALUSB` followed by zeros and a
`0x55AA` signature.

`xhci_interrupt_transfer()` (line 949) simply zeroes the buffer and returns
success, which is why an xHCI keyboard reports keys that are never pressed.

### 1.2 Why this is worse than a missing feature

An absent driver fails loudly. This one **passes tests**.

`test_usb_xhci.sh` makes eight assertions and all eight are green at the
baseline. It asserts `[msc] PASS: USB mass storage READ(10) works` — and the
log shows what was read:

```
[msc] sector 0 first bytes: 41 55 52 41 4c 55 53 42 00 00 00 00 00 00 00 00
```

`41 55 52 41 4c 55 53 42` is ASCII **"AURALUSB"**. QEMU was given a real disk
image via `-drive`; the OS never touched it. The test validates the forgery
against itself, so the suite reports success for a controller that has never
moved a byte.

This is the one property that makes the item **critical** rather than merely
incomplete: *the test suite currently cannot tell the difference between a
working xHCI driver and no xHCI driver at all.* Every future change to this
file is therefore unverifiable.

The comments are honest — the block above `xhci_bulk_transfer()` says plainly
"this is NOT a real xHCI bulk transfer", and `README.md` says "Do not rely on
xHCI for storage until real transfer rings land". The **code** is honest to a
reader. The **log** and the **tests** are not, and those are what a user and CI
see.

### 1.3 What is genuinely fine

This plan should not overstate the damage. Audited at the baseline:

| Controller | Assessment |
|---|---|
| **UHCI** | Real. TD/QH schedules, working control + bulk, drives MSC end to end. |
| **OHCI** | Real. ED/TD lists, control, bulk and interrupt all queue descriptors and wait on completion codes. |
| **EHCI** | Real for async. qTD/QH async schedule for control and bulk. Interrupt endpoints are honestly routed through the async path with a comment saying periodic + split transactions are future work. |
| **xHCI** | Bring-up only: PCI, MMIO, CRCR, DCBAA, port scan and PORTSC decode are real and correct. Everything above that is fabricated. |

The class layer (`usb_core`, `hid`, `msc`, `hub`, `cdc_acm`, `usb_audio`,
`usb_printer`) is genuine, controller-agnostic and reusable — it is the reason
UHCI/OHCI/EHCI work. **No class driver needs rewriting.** This plan is almost
entirely confined to `drivers/usb/xhci.c`, plus one honest phase each for EHCI
periodic transfers and for interrupts.

### 1.4 The list, ranked

Ranked by danger, in the manner of `FIXES_PLAN.md` §1:

| # | Defect | Rank | Phase |
|---|---|---|---|
| 1 | Green tests assert against fabricated data | **Critical** | U0, U2 |
| 2 | Event ring never read; every command/transfer times out | **Critical** | U1 |
| 3 | `Address Device` invents slot IDs | **Critical** | U3 |
| 4 | Control transfers forge descriptors by `dev_addr % 3` | **Critical** | U4 |
| 5 | Bulk transfers forge SCSI replies and sector data | **Critical** | U5 |
| 6 | Interrupt transfers return zeroed buffers as success | Serious | U6 |
| 7 | Boot log claims "full support" for all of the above | Serious | U0 |
| 8 | EHCI periodic schedule and split transactions absent | Serious | U7 |
| 9 | Entire stack is polled; no USB interrupt is ever taken | Latent | U8 |
| 10 | Hub depth and isoc paths untested against real devices | Latent | U9 |

---

## 2. Decisions

### D1. Delete the fabrication before writing the replacement

The tempting order is to write real transfers and then remove the fakes once
the real ones work. That is backwards: while the fakes are present, a
half-finished real path silently falls back to them and the tests stay green,
so there is no signal. **U2 removes the forgery first and lets the tests go
red**, which is the only state from which progress is measurable.

This costs a temporary regression in `test_usb_xhci.sh`, and that is the point:
those assertions were never true.

### D2. Every phase gets a test that fails without it

Taken from `FIXES_PLAN.md` D2, and it is sharper here because the existing
tests are compromised. Each phase must state what its test asserted *before*
the change (usually: passing wrongly, or failing) and after.

Where the OS reads data that QEMU supplied, the test must assert **the content
QEMU was given**, not merely that a read returned. The concrete gate: write a
known 512-byte pattern into the backing image with `dd`, read sector 0 through
`/usb`, and compare bytes. No fabricated sector can pass that, which is exactly
why it is the right assertion.

### D3. Real hardware structures, not "works on QEMU" shortcuts

xHCI is a specification, not a device. The ring/TRB/context code must follow
xHCI 1.2 as written — cycle bits, link TRBs with Toggle Cycle, the Event Ring
Segment Table, the dequeue pointer with EHB, the Input Context A0/A1 flags,
`ERSTSZ`/`ERSTBA`/`ERDP` programming. Shortcuts that happen to satisfy QEMU
will fail on the first real controller and are forbidden.

Corollary: the 64-byte-context case (`HCCPARAMS1.CSZ = 1`) must be honoured.
QEMU reports 32-byte contexts, so a hardcoded 32 is invisible in QEMU and fatal
on much real silicon. `xhci_init()` already reads `context=32 bytes` correctly
at the baseline; the context accessors must actually use it.

### D4. Scratchpad buffers are allocated even when QEMU asks for none

QEMU reports `0 scratchpads`, so the omission is currently invisible. Real
controllers commonly demand them, and a missing scratchpad array is a hang with
no diagnostic. `Max Scratchpad Buffers` (Hi/Lo) must be read and honoured.

### D5. One transfer engine, four thin backends

`usb_core.c` already dispatches on `dev->controller` through a stable set of
entry points (`usb_control_transfer`, `usb_bulk_transfer_ex`,
`usb_interrupt_transfer`). That boundary is correct and stays. No class driver
learns which controller it is talking to — MSC's existing per-controller
`if` ladder (`drivers/usb/msc.c:144-163`) should shrink, not grow.

### D6. Interrupts are a separate phase, after correctness

The stack is polled today: there is not one IRQ registration in
`drivers/usb/*.c`. Polling is slow and it burns a kernel thread every 500 ms,
but it is *correct*. Converting to MSI/MSI-X event-ring interrupts (U8) is a
performance and architecture change and must not be entangled with making the
data path real. Correct-and-slow first.

### D7. Explicitly out of scope

Stated so their absence is a decision and not an oversight:

- **USB 3.x streams** — the boot log claims "streams"; U0 stops claiming it and
  U9 records it as unimplemented. Bulk streams matter for UAS, not for MSC.
- **UAS** (USB Attached SCSI) — BOT is sufficient; UAS needs streams.
- **USB-C / Power Delivery / role switching** — no host support, out of scope.
- **Real wireless dongles** for the Wi-Fi/Bluetooth protocol layers — those
  layers wait on a chipset driver, which is a different plan.
- **Isochronous audio with real timing** — U9 wires the path and tests
  structure; sample-accurate playback is future work.

---

## 3. Phases

### Phase U0 — Tell the truth in the log and the matrix ✅ DONE

**Objective:** the tree stops claiming support it does not have, *before* any
behaviour changes. This phase changes no data path.

**Landed.** Measured on the resulting build:

```
no xHCI attached:   SYNTHETIC=0   22 PASS   0 FAIL
xHCI + kbd + disk:  SYNTHETIC=7   (11 with three devices)
```

`test_usb_xhci.sh` went from a false **8/8 green** to **3 of 9 FAILED**, naming
the cause:

```
  ✘ xHCI Address Device command works
  ✘ xHCI MSC READ(10) works
  ✘ guest log contains 11 SYNTHETIC marker(s): the OS fabricated data
      [xhci] SYNTHETIC address_device: ... (no Enable Slot / Address Device
             command is sent; USB_PLAN.md U3)
      [xhci] SYNTHETIC control transfers: descriptors are fabricated in-driver
             (device identity guessed from dev_addr % 3) ...
```

That is the intended, recorded regression: those two assertions were never
true. The remaining six still pass because enumeration *structure* is real
even where its content is not.

Rationale: every later phase is measured against the log. While the log says
"full support — control/bulk/intr/isoc … PASS" for a controller that fabricates
its answers, no measurement means anything.

#### Tasks

- [ ] Replace the xHCI self-test banner. It currently asserts control, bulk,
      interrupt, isoc, slots, endpoints, streams, command and event rings. It
      must report what is verified: controller bring-up, port scan, PORTSC
      decode — and name the rest as unimplemented.
- [ ] Do the same for `usb_core`'s "FULL SUPPORT MODE" / "full stack ready"
      banners and the per-class "full support ready" lines. A driver with zero
      attached devices must not print `PASS`; `SKIP (no device)` is the honest
      verdict and the one that makes a real regression visible.
- [ ] Every synthesised value gets a `[SYNTHETIC]` tag in its log line for as
      long as it survives (removed by U2–U5). `(FAKE)` already appears on two
      `address_device` lines; make it uniform and impossible to miss.
- [ ] Correct `docs/status.md`: the xHCI row currently reads "control/bulk/
      interrupt transfer rings are implemented for HID/MSC", which is untrue.
      The USB MSC and hotplug rows inherit the same claim.
- [ ] Add a `SYNTHETIC` grep guard to the integration library so any test whose
      log contains a synthetic marker fails loudly rather than passing.

#### Test gate — all met

- ✅ `grep -c SYNTHETIC` returns **7** with an xHCI controller attached and
  **0** without one. Must reach zero everywhere at U5; that number is the
  plan's progress metric.
- ✅ `test_usb_xhci.sh` fails via the new guard, naming the synthesis.
- ✅ No other case changes state: `make test-unit` 118/118, and
  `test_boot_to_shell`, `test_usb_msc`, `test_usb_ohci`, `test_usb_ehci`,
  `test_usbfs_fat32`, `test_usb_hub` stay green — the guard is inert on the
  controllers that move real data.

**Two corrections found by running it**, both the same mistake in different
places: a marker that described a *capability* rather than an *event*.

1. The first cut printed `SYNTHETIC` in `usb_core`'s summary banner, which
   fires on every boot — including machines with no xHCI — so the global
   guard would have reddened all 118 cases.
2. The second printed it from `xhci_self_test()` whenever a controller
   existed. `test_usbfs_fat32.sh` puts a `qemu-xhci` on the command line but
   does its real work over UHCI, so a correct run went red on an idle
   controller.

The marker now means **"this boot fabricated data"**: it is emitted from
`xhci.c` only when a device is actually present, and from each transfer path
on first entry. Verified both ways — `test_usbfs_fat32` is back to 6/6 while
`test_usb_xhci` stays correctly red.

#### Deliverable

`patches/USB_U0_honest_log.patch`

---

### Phase U1 — The event ring, and one real command **(critical)** ✅ DONE

**Objective:** `xhci_poll_event_type()` returns a real TRB.

This is the keystone. Nothing above it can work, and everything above it works
almost immediately once it does.

**Landed.** For the first time in this driver's history the controller
answered:

```
[xhci] command ring: No Op -> Success (cc=1)
[xhci] command ring: PASS — 256/256 No Ops across a ring wrap
```

**A real bug found on the way.** The command ring's Link TRB was written
with cycle 0 while `CRCR` starts the controller at `RCS=1`, so the
controller would have seen a TRB it does not own and stopped at the end of
the segment rather than following the link back. It could never show up
before U1 because no command completed at all; it would have appeared
immediately afterwards as "the first 255 commands work, then everything
hangs". Fixed in the same phase, and it is exactly what the 256-No-Op wrap
test exists to catch.

#### Tasks

- [ ] Verify the Event Ring Segment Table against the spec: allocate the ERST,
      program `ERSTSZ`, `ERSTBA` and `ERDP` for interrupter 0, and confirm the
      segment's physical address and size are what the controller was told.
- [ ] Implement the consumer properly: compare each TRB's Cycle bit against the
      software Consumer Cycle State, advance the dequeue pointer, wrap at the
      segment end and **invert CCS on wrap**. This is the single most common
      xHCI bug and it presents as "works once, then hangs".
- [ ] Write back `ERDP` after consuming, with the Event Handler Busy bit
      cleared. Omitting this stalls the ring after the first event on real
      hardware while appearing to work under QEMU.
- [ ] Give the wait a real timeout in milliseconds against the PIT/LAPIC tick,
      not a spin count, and make the timeout message name the ring state
      (`USBSTS`, `CRCR`, `ERDP`, dequeue index, expected cycle).
- [ ] Route Command Completion and Transfer events to distinct consumers so a
      transfer completion cannot satisfy a command wait.
- [ ] Prove it with the cheapest possible command: **`No Op Command` (TRB type
      23)**. It touches no device and its only purpose is exactly this — to
      show the command ring and event ring are correctly wired.

#### Test gate — all met

- ✅ `No Op Command` completes with cc=1 at boot.
- ✅ 256 consecutive No Ops all complete, driving the ring past its Link TRB.
- ✅ **Negative control:** `ERDP` deliberately offset by 0x1000 →
  `command ring: FAIL — 0/256`, with a diagnostic naming the ring state
  (`erdp_idx`, `ccs`, `usbsts`) and **no hang** — the timeout is real, not a
  spin. Reverted after measuring.
- ✅ `tests/unit/test_xhci_ring.c`: **24/24** host checks on the
  cycle/wrap/dequeue arithmetic, wired into `make test-unit`.

**A correction the unit test forced.** Its negative control first asserted
that a consumer which never inverts its cycle *stalls* after one lap. That
is wrong twice over: the setup posted 512 events into a 256-entry ring, so
every slot had been overwritten with cycle 0 and the broken consumer stopped
immediately for the wrong reason. The real failure is worse than a stall —
at the wrap the stale lap-1 TRBs still carry cycle 1, so a non-inverting
consumer **re-delivers events the controller already retired, forever**. It
is silent duplication, not a hang, and that is precisely why ownership is
expressed by the cycle bit rather than by an index comparison. The test now
asserts the runaway and pairs it with the correct consumer stopping at
exactly 256 on the same ring.

#### Deliverable

`patches/USB_U1_event_ring.patch`

---

### Phase U2 — Delete the fabrication layer **(critical)** ✅ DONE

**Objective:** no code path in the tree invents a USB answer.

Placed immediately after U1 and before the real implementations, per D1.

**Landed, and the metric closed early.** `SYNTHETIC` markers with an xHCI
device attached went **7 → 0**. The plan expected zero at U5; it arrives at
U2 because deleting the forgery outright is what D1 asked for — the
replacements in U3–U6 now have nothing to fall back on.

Roughly 60 lines of fabrication are gone:

| Deleted | What it invented |
|---|---|
| `xhci_control_transfer()` head | device/config/string/HID-report descriptors; device identity from `dev_addr % 3` |
| `xhci_bulk_transfer()` body | INQUIRY (`QEMU HARDDISK`), READ CAPACITY, CSW, the `AURALUSB` sector |
| `xhci_interrupt_transfer()` body | zero-filled buffer returned as success |
| `xhci_address_device()` | slot IDs from a `static uint8_t fake_slot` counter |

Each is now an honest refusal that names its phase. The real TRB path in
`xhci_control_transfer()` — dead since it was written — is reachable for the
first time.

The boot log with two xHCI devices attached now reads:

```
[xhci] command ring: No Op -> Success (cc=1)
[xhci] command ring: PASS — 256/256 No Ops across a ring wrap
[xhci] NOT IMPLEMENTED: Address Device (U3), control data stage (U4),
       bulk (U5), interrupt (U6), streams/UAS
[xhci] 2 device(s) connected; they cannot be used until U3
```

#### Tasks

- [ ] Delete the descriptor forgery in `xhci_control_transfer()` — the
      `dev_addr % 3` device-identity guess, the hardcoded device, config,
      string and HID report descriptor arrays, and the `return data_len;` at
      line 886 that shadows the real implementation beneath it.
- [ ] Delete the SCSI forgery in `xhci_bulk_transfer()`: the INQUIRY, READ
      CAPACITY, CSW and `AURALUSB` sector synthesis, and the `last_tag` scrape.
- [ ] Delete the zero-fill success in `xhci_interrupt_transfer()`.
- [ ] Delete the `fake_slot` counter in `xhci_address_device()`.
- [ ] Each becomes an honest `-ENOTSUP` with a one-line diagnostic, until its
      phase lands.
- [ ] Remove the now-unreferenced helpers the compiler flags (`trb_type`,
      `cap_rd8`, `cap_rd16` are already unused at the baseline) or wire them
      into the real paths.
- [ ] Build with `-Wunreachable-code` enabled for `drivers/usb/` and keep it
      on. The dead real implementation at line 887 was detectable by the
      compiler for as long as it has existed; this makes recurrence impossible.

#### Test gate — all met

- ✅ `SYNTHETIC` count with an xHCI device attached: **0** (was 7).
- ✅ `test_usb_xhci.sh` fails cleanly, with no fabricated data in the log.
  **Recorded as an expected red until U5** — see the band below.
- ✅ `AURALUSB`, `QEMU HARDDISK`, `fake_slot`, `dev_addr % 3` survive only
  inside comments recording what was removed; no executable path uses them.
- ✅ Deletion confined to xHCI: `test_usb_msc` 7/7, `test_usb_ohci` 6/6,
  `test_usb_ehci` 5/5, `test_usbfs_fat32` 6/6, `test_usb_hub` 6/6,
  `test_boot_to_shell` 17/17, `make test-unit` green.
- ✅ `-Wunreachable-code -Werror=unreachable-code` is now on for
  `drivers/usb/` (its own Makefile rule) and the tree builds clean under it.
  This is the compiler diagnostic that had been reporting the shadowed real
  implementation at `xhci.c:887` all along, unheard because it is not in
  `-Wall`/`-Wextra`.

#### The expected-red band

Recorded here so a red CI run in this window is not mistaken for a
regression:

| Case | State at U2 | Cleared by |
|---|---|---|
| `test_usb_xhci` | ✅ **green at U5** | — |
| `test_usb_xhci_hub` | ❌ red | U6 |
| `test_usb_hotplug` | ❌ red (already red before the plan) | U6 |

Everything else stays green throughout.

#### Deliverable

`patches/USB_U2_delete_synthesis.patch`

---

### Phase U3 — Real `Address Device` **(critical)** ✅ DONE

**Objective:** a device on an xHCI port gets a slot from the controller.

**Landed.** The controller now issues the slots and confirms the result:

```
[xhci] slot 1 addressed (port 2, speed super-speed (5 Gbps), mps0=512,
       hw addr=1, Slot State=Addressed)
[xhci] slot 2 addressed (port 4, speed high-speed (480 Mbps), mps0=64, ...)
[xhci] slot 3 addressed (port 5, speed high-speed (480 Mbps), mps0=64, ...)
[usb] addr 1: xHCI port 2, super, class=Mass Storage VID=0x46f4 PID=0x0001
[usb] addr 2: xHCI port 4, high,  class=HID          VID=0x0627 PID=0x0001
```

**Control transfers came back for free.** U2 unshadowed the real
Setup/Data/Status TRB path and U1 made completions observable, so with a
real slot in hand the descriptors are now fetched *from the devices* —
note the different VID per device class, which the deleted `dev_addr % 3`
forgery could not produce. U4 remains, to harden that path (short packets,
stalls, the full-speed mps0 re-read).

**Two real bugs found by running it.**

1. **SuperSpeed EP0 was programmed with a 9-byte max packet.** For
   SuperSpeed, `bMaxPacketSize0` is an *exponent* (USB 3.2 §9.6.1: fixed at
   `09h`, meaning 2⁹ = 512), not a byte count. The first boot log read
   `maxpkt0=9`, which is what exposed it. Full/high speed report the size
   directly, so the bug is SuperSpeed-only and would have looked like
   mysterious truncation later.
2. **`xhci_disable_slot()` had no callers.** An unplugged xHCI device kept
   its slot, contexts and rings for the rest of the boot; after 64
   attach/detach cycles `Enable Slot` would simply start refusing. Added
   `xhci_free_device()` and wired it into `usb_detach_location()`.
   Every error path in `xhci_address_device()` also unwinds now — a failed
   enumeration previously leaked a slot per attempt.

#### Tasks

- [ ] `Enable Slot`, and take the slot ID from the Command Completion event
      rather than a counter.
- [ ] Build the Input Context correctly: Input Control Context A0|A1, a Slot
      Context carrying route string, root hub port number, speed and context
      entries, and EP0's Endpoint Context as Control with the correct
      `Max Packet Size` for the speed (8/64/512, and the two-step re-read for
      full-speed devices whose real `bMaxPacketSize0` is only known after the
      first eight bytes of the device descriptor).
- [ ] Honour `HCCPARAMS1.CSZ` for 32 vs 64-byte contexts (D3), and allocate the
      Device Context Base Address Array entry for the slot.
- [ ] Allocate the EP0 transfer ring with its Link TRB and correct initial
      cycle state.
- [ ] Allocate scratchpad buffers per `Max Scratchpad Buffers Hi/Lo` (D4).
- [ ] Implement the real `Address Device`, and its counterparts `Disable Slot`
      and `Reset Endpoint`, so a failed enumeration frees its slot instead of
      leaking one per attempt.
- [ ] Preserve the existing route-string logic in `xhci_decode_port_route()`
      for devices behind hubs — it is already written and is not part of the
      forgery.

#### Test gate — met

New case `tests/integration/cases/test_xhci_address.sh`, **12/12**:

- ✅ `Slot State = Addressed` read back out of the **device context** — the
  field only the controller writes, and the one assertion a fabricated
  implementation cannot satisfy.
- ✅ three devices → three distinct, controller-issued slot IDs.
- ✅ SuperSpeed EP0 max packet is 512, not the raw exponent (regression
  guard for bug 1 above).
- ✅ real per-device VID/PID (`0x46f4` storage vs `0x0627` HID), which the
  `dev_addr % 3` forgery could not produce for this attach order.
- ✅ no invalid slots, no un-addressed slots, no Address Device failures,
  no faults.
- ✅ `SYNTHETIC=0` holds.

Slot-leak coverage is by construction rather than by a 20× loop: every
error path unwinds through `xhci_release_xdev()`, and detach now calls
`xhci_free_device()`. `xhci_active_slot_count()` is exposed so U6's hotplug
case can assert the count does not creep across attach/detach cycles, where
that loop belongs.

#### Deliverable

`patches/USB_U3_address_device.patch`

---

### Phase U4 — Real control transfers **(critical)** ✅ DONE

**Objective:** descriptors come from the device.

**Landed.** Enumeration is now complete end to end — classes, interfaces
and endpoints all read off the wire:

```
[usb]   product: 'QEMU USB HARDDRIVE'
[usb]   config 1: 1 interfaces, 44 bytes, attr=0xc0
[usb]   endpoint 0x81: bulk IN,  maxpkt=1024 (+SS companion)
[usb]   endpoint 0x02: bulk OUT, maxpkt=1024 (+SS companion)
[usb]   product: 'QEMU USB Keyboard'
[usb]   endpoint 0x81: interrupt IN, maxpkt=8, interval=7ms
[usb] addr 1: xHCI ... class=Mass Storage VID=0x46f4
[msc] mass storage candidate: addr=1 bulk_in=0x81 bulk_out=0x02
[hid] keyboard ready: addr=2 iface=0 ep=0x81
```

Added: short-packet handling via the event residue, Stall recovery
(`Reset Endpoint` + `Set TR Dequeue Pointer`, xHCI 1.2 §4.6.8/§4.6.10), ISP
on the Data Stage so a short read is an event rather than an error, and
`Evaluate Context` to re-program EP0 once a full-speed device's real
`bMaxPacketSize0` is known.

**Three bugs found by running it, two of them mine.**

1. **QEMU aborted the whole VM**:
   `usb_packet_copy: Assertion 'p->actual_length + bytes <= iov->size'`.
   Cause: in this file's `struct xhci_trb`, `param`+`status` are the two
   halves of the 64-bit parameter and the *third* dword — confusingly named
   `control` — carries the Transfer Length. I had written the length into
   `status`, leaving the length zero and the buffer pointer nonsense. The
   same misreading put the residue in the wrong dword.
2. **A stale residue truncated good descriptors.** A 34-byte configuration
   read returned `cc=13 residue=214` — a residue *larger than the request*,
   left over from a preceding 256-byte read on the same endpoint. Taken
   literally that is a negative length; clamped to zero it silently
   discarded the descriptor, which is why enumeration had stalled at
   `class=Generic` with no interfaces. A residue exceeding the request is
   now treated as a stale field, not a short packet.
3. Residue is only meaningful on `cc=13`; on plain Success the field is
   reserved and QEMU does not clear it.

The second is the interesting one: it produced no error anywhere. The
transfer "succeeded", the parse simply had nothing to parse. Only the
byte-level assertions below catch that shape of failure.

#### Tasks

- [ ] Un-shadow and finish the implementation already present at line 887:
      Setup Stage TRB with the correct Transfer Type (2 = OUT data, 3 = IN
      data, 0 = no data), optional Data Stage with the direction bit, Status
      Stage with the opposite direction and IOC.
- [ ] Wait on a Transfer Event for the endpoint, accepting completion code 1
      (Success) and 13 (Short Packet), and mapping the rest to errors that name
      the code.
- [ ] Handle short packets by computing the actual length from the event's
      Transfer Length residue rather than assuming the full request.
- [ ] Ring the correct doorbell: slot, target = EP0 = 1.
- [ ] `Evaluate Context` for the full-speed `bMaxPacketSize0` correction.
- [ ] Handle Stall: on completion code 6, issue `Reset Endpoint` and clear the
      halt so one failed request does not kill the device.

#### Test gate — met

New case `tests/integration/cases/test_xhci_control.sh`, **15/15**:

- ✅ three *distinct* product strings (`QEMU USB HARDDRIVE` / `Keyboard` /
  `Mouse`). The forgery returned the literal `"QEMU"` for every string of
  every device, so this cannot be satisfied by fabrication.
- ✅ configuration descriptors fetched in two steps and parsed: 44 bytes for
  storage, 34 for HID — the exact read that bug 2 was truncating.
- ✅ endpoints decoded with real addresses, types and packet sizes
  (`0x81 bulk IN maxpkt=1024`, `0x81 interrupt IN maxpkt=8`).
- ✅ classes taken from interface descriptors, not `dev_addr % 3`.
- ✅ the class drivers act on it: MSC finds its bulk pair, HID binds both
  devices.
- ✅ no timeouts, stalls or faults; `SYNTHETIC=0` holds.

Stall recovery is implemented and reachable but not yet gated by a test —
QEMU's devices do not stall on the requests enumeration makes. Provoking one
deliberately is left to U5, where the BOT error path exercises it naturally.

#### Deliverable

`patches/USB_U4_control_transfers.patch`

---

### Phase U5 — Real bulk transfers, and MSC on xHCI **(critical)** ✅ DONE

**Objective:** a byte written to the QEMU disk image is the byte the OS reads.

**Landed, and the objective is met literally.** `dd` writes a pattern into
sector 0 of the backing image; the OS reads those exact bytes back:

```
[msc] INQUIRY: vendor 'QEMU' product 'QEMU HARDDISK'
[msc] capacity: 24576 sectors, 512 bytes/sector (12288 KiB)
[msc] sector 0 first bytes: 55 35 2d 52 45 41 4c 2d 42 55 4c 4b 2d 30 30 30
[msc] PASS: USB mass storage READ(10) works
```

`55 35 2d 52 45 41 4c...` is `U5-REAL-BULK-000`. The old log read
`41 55 52 41 4c 55 53 42` — `AURALUSB` — regardless of the disk's contents.

Real Normal TRBs are queued on the endpoint ring, chained for requests over
64 KiB and split at 64 KiB boundaries (xHCI 1.2 §4.11.2.4), with CH on all
but the last TRB and IOC only on the last, so one Transfer Event reports the
whole chain. ISP is set so a short packet completes instead of erroring.

**Two more real bugs found.**

1. **`Configure Endpoint` shrank the slot when configuring the second
   endpoint.** Context Entries is the index of the *last valid* endpoint
   context, and the code set it to the endpoint being configured. A mass
   storage device configures bulk OUT (ep_id 4) and then bulk IN (ep_id 3):
   the second call would have declared "3 entries", dropping the first.
   Now it takes the maximum over everything configured so far. The
   dequeue-pointer DCS bit was also hardcoded to 1 instead of following the
   ring's cycle state.
2. **`kprintf()` ignores the precision field for `%s`.** `msc.c` printed
   the SCSI INQUIRY strings — which are space-padded and *not*
   NUL-terminated — with `%.8s`/`%.16s`, so it ran off the end of each
   field and emitted binary junk:
   `vendor 'QEMU    QEMU HARDDISK   2.5+\xc2\x12'`. The deleted stub's
   fabricated INQUIRY happened to be NUL-padded, which is exactly why this
   only surfaced once real data started arriving. Fixed locally in `msc.c`
   by copying out and trimming; the `kprintf` limitation is noted for a
   separate change rather than fixed here, since it is not a USB defect.

**A correction to my own test.** The first cut of the gate asserted that
`QEMU HARDDISK` must *not* appear, on the theory that it was the stub's
invention. It is not — it is genuinely what QEMU's `usb-storage` reports,
and the stub hardcoded it *because* that is the real answer. Asserting its
absence was wrong, and the capacity and sector-content checks are what
actually separate real data from fabricated data.

#### Tasks

- [ ] `Configure Endpoint` with the Input Context describing the bulk IN/OUT
      endpoints from the interface descriptor: EP type, max packet size,
      max burst, interval, and average TRB length.
- [ ] Allocate a transfer ring per endpoint; enqueue Normal TRBs with correct
      Chain/IOC/ISP flags; handle transfers spanning more than one TRB and
      crossing 64 KiB boundaries.
- [ ] Data-buffer discipline: physically contiguous, correctly aligned, and
      **freed on every path including error** — the fabricated implementation
      allocated nothing, so this is new territory for leaks.
- [ ] Short-packet handling via residue, which BOT depends on for the CSW.
- [ ] Endpoint halt recovery: `Reset Endpoint` plus `Set TR Dequeue Pointer`,
      which BOT's error path requires.
- [ ] Simplify `msc.c`'s per-controller ladder now that all four backends
      present the same contract (D5).

#### Test gate — met

New case `tests/integration/cases/test_xhci_bulk.sh`, **8/8**, twice in a
row. The decisive assertions:

- ✅ **sector 0 equals the pattern `dd` wrote.** No fabricated answer can
  satisfy this; it is the whole point of the phase.
- ✅ the fabricated `AURALUSB` bytes are absent.
- ✅ READ CAPACITY reports **24576** sectors — the image is deliberately
  12 MiB so that the stub's fixed 16384 would stand out.
- ✅ INQUIRY strings parsed and trimmed from the device's own answer.
- ✅ `[msc] PASS: READ(10) works` is restored — earned now, not asserted.
- ✅ no timeouts, stalls or faults.

`test_usb_xhci.sh`, red since U2, is **green again** — and this time its
`READ(10)` assertion is backed by real data rather than by the driver
agreeing with itself.

#### Deliverable

`patches/USB_U5_bulk_msc.patch`

---

### Phase U6 — Interrupt endpoints and HID on xHCI

**Objective:** an xHCI keypress is a keypress.

#### Tasks

- [ ] Configure interrupt endpoints with the correct `Interval` encoding —
      xHCI's is a logarithmic field and differs per speed; getting it wrong
      yields either a dead endpoint or a flood.
- [ ] Queue Normal TRBs on the interrupt ring and consume Transfer Events,
      replacing the zero-fill stub deleted in U2.
- [ ] Re-arm after each report without reallocating the ring per poll.
- [ ] Feed reports into the existing generic HID parser, which already works
      for UHCI/OHCI/EHCI and needs no change.

#### Test gate

- `-device usb-kbd,bus=xhci.0`: inject keystrokes over the QEMU monitor
  (`sendkey`) and assert the **specific characters** appear at the shell.
  The stub returned zeroes, so any non-empty correct input proves the path.
- `-device usb-mouse,bus=xhci.0`: injected motion produces cursor movement in
  the matching direction and magnitude.
- Assert **no** phantom input when nothing is injected — the inverse failure,
  and the one a zero-filling stub would have hidden.
- `test_usb_hotplug.sh` passes: this is the case that fails at the baseline,
  and it fails precisely because HID could not attach over xHCI.

#### Deliverable

`patches/USB_U6_interrupt_hid.patch`

---

### Phase U7 — EHCI periodic schedule and split transactions

**Objective:** the one honest gap outside xHCI.

EHCI's interrupt endpoints are currently routed through the async qTD path as a
polled one-shot, with a source comment saying periodic and split transactions
remain future work. That comment is accurate; this phase closes it.

#### Tasks

- [ ] Build the periodic frame list and program `PERIODICLISTBASE`; link
      interrupt QHs at the correct polling interval with proper S-mask/C-mask.
- [ ] Split transactions for full/low-speed devices behind a high-speed hub:
      the Transaction Translator fields (hub address, port number), start-split
      and complete-split scheduling.
- [ ] Route full/low-speed devices on an EHCI companion correctly instead of
      failing them.

#### Test gate

- `-device usb-ehci -device usb-kbd` (a full-speed device on a high-speed
  controller) enumerates and delivers real keystrokes through the periodic
  schedule.
- A full-speed device behind `-device usb-hub` on EHCI works, which is the
  split-transaction case specifically.
- Interrupt latency is bounded: report the measured poll interval and assert it
  matches the endpoint's requested `bInterval` within tolerance.
- Existing EHCI async cases stay green.

#### Deliverable

`patches/USB_U7_ehci_periodic.patch`

---

### Phase U8 — Interrupts instead of polling

**Objective:** the USB stack stops burning a kernel thread.

Deliberately after correctness (D6). Today `usb_hotplug_start()` spawns a
thread that polls all four controllers every 500 ms, and there is not a single
IRQ registration in `drivers/usb/`.

#### Tasks

- [ ] MSI/MSI-X for xHCI, falling back to pin-based PCI IRQ; enable interrupter
      0 (`IMAN.IE`, `IMOD`) and `USBCMD.INTE`.
- [ ] An interrupt handler that drains the event ring and wakes the waiter
      through the existing `wait_queue` mechanism, replacing the timeout spin.
- [ ] Legacy IRQ handlers for UHCI/OHCI/EHCI on the same model.
- [ ] Port Status Change events drive hotplug directly; the 500 ms poll becomes
      a fallback for controllers whose IRQ could not be routed, not the primary
      path.
- [ ] Keep every transfer's timeout as a backstop — an interrupt that never
      arrives must still produce a diagnostic, not a hang.

#### Test gate

- Attach/detach latency measurably drops from ~500 ms to interrupt-time; both
  numbers recorded in the patch.
- `/proc` interrupt counts show USB IRQs actually being taken and rising with
  device activity.
- The full USB suite passes with the polling fallback compiled out, proving the
  interrupt path is genuinely doing the work.
- Under `-smp 4`, 1000 attach/detach cycles with no lost event and no deadlock
  between the IRQ handler and the hotplug path.

#### Deliverable

`patches/USB_U8_interrupts.patch`

---

### Phase U9 — Hubs, isoc, and the honest matrix

**Objective:** close the remaining claims, and make the documentation match
reality one final time.

#### Tasks

- [ ] Multi-level hubs: verify depth against the advertised limit of 5 with
      nested QEMU hubs, and verify xHCI route-string addressing for a device
      two hubs deep.
- [ ] Isochronous: real Isoch TRBs with frame IDs on xHCI; assert structure and
      continuity. Sample-accurate audio stays out of scope (D7) and is recorded
      as such.
- [ ] Rewrite `docs/status.md`'s USB rows against the measured end state, and
      write `docs/usb.md` in the manner of `docs/win32.md`: a table of what
      works, and — more usefully — a table of what is approximated and what is
      absent (streams, UAS, USB-C/PD, sample-accurate isoc).
- [ ] Re-audit every remaining `PASS` in the USB boot log against what its
      self-test actually verified, closing the loop opened in U0.
- [ ] Record the before/after of the plan's own metric: green-but-fabricated
      assertions at baseline → zero.

#### Test gate

- A device two hubs deep on xHCI enumerates and transfers.
- `test_usb_hub_full.sh`, `test_usb_xhci_hub.sh` pass with real transfers
  rather than synthesis.
- `tools/`-side check, in the manner of `gen_w32_api_table.py --check`: a script
  cross-checks the USB claims in `docs/usb.md` against the drivers and fails CI
  on drift. The documentation cannot silently rot again.
- Full suite: **118/118 integration cases green**, including the two failing at
  the baseline (`test_usb_hotplug`) and the one passing dishonestly
  (`test_usb_xhci`).

#### Deliverable

`patches/USB_U9_hubs_isoc_docs.patch`

---

## 4. Order and rationale

U0 first because every later measurement depends on the log being trustworthy,
and it is the only phase that changes no behaviour — so it can land immediately
and independently.

U1 next because it is the keystone: the event ring is the single blocker behind
items 2–6 in the ranked list, and roughly half of the xHCI file is already
written and merely unreachable behind it.

U2 before U3–U5 is the one counter-intuitive ordering, and it is deliberate
(D1). Deleting the forgery first turns the test suite from a source of false
confidence into a source of signal. The plan accepts a recorded red band
between U2 and U5.

U3 → U4 → U5 follow the device lifecycle — address, then describe, then
transfer — because each genuinely depends on the last. U6 (interrupt endpoints)
comes after bulk because HID is the easier consumer once the ring machinery is
proven, and because `test_usb_hotplug` — the visible failure that started this
— closes there.

U7 is independent of the xHCI work and could land at any time; it sits here
because it is the smaller, better-understood gap and should not delay the
critical items.

U8 and U9 are the finish: performance, then honesty. U8 is last among functional
phases because an interrupt-driven bug in a fabricated data path would be
undebuggable, whereas an interrupt-driven bug over a proven data path is merely
hard.

---

## 5. Risks

| Risk | Mitigation |
|---|---|
| **The red band (U2–U5) makes CI red for several phases.** | Mark the affected cases expected-fail with the phase that clears them, exactly as this document records. Do not skip them — a skipped test is invisible. |
| **Cycle-bit and wrap bugs are notoriously subtle** and present as "works for a while". | U1's 256-No-Op wrap test and the host-side `test_xhci_ring.c` target this specifically, off-hardware. |
| **Working on QEMU is not working.** QEMU is forgiving about ERDP write-back, scratchpads and 64-byte contexts. | D3 and D4 make spec conformance a requirement rather than a test outcome; review each against xHCI 1.2 section numbers in the patch. |
| **DMA buffer leaks** in paths that previously allocated nothing. | Assert free-on-every-path in review; add a boot-time PMM free-frame count before and after 1000 transfers and require it unchanged. |
| **The class layer may have quietly adapted to synthesised behaviour.** | U2's gate requires UHCI/OHCI/EHCI cases to stay green through the deletion, which detects exactly this coupling. |
| **U8 introduces IRQ/thread races** in a stack that has never taken an interrupt. | Keep timeouts as a backstop; 1000-cycle SMP stress in the U8 gate; the existing `wait_queue` is already used correctly elsewhere in the kernel. |
| **Scope creep into UAS/streams/USB-C.** | D7 names them as out of scope up front. |

---

## 6. What this plan does not do

- It does not rewrite the class drivers. `usb_core`, `hid`, `msc`, `hub`,
  `cdc_acm`, `usb_audio` and `usb_printer` are genuine and controller-agnostic,
  and the plan's success is partly measured by how little they change.
- It does not touch UHCI or OHCI, which are real today.
- It does not add new device classes — no UVC, no UAS, no printer classes
  beyond what exists. Absent, not broken.
- It does not chase real-hardware certification. Every gate is QEMU-based,
  because that is what can be run in CI; D3 exists so that the result has a
  chance of working elsewhere, but this plan does not claim to have proven it.
- It does not fix `test_tls_errno`'s timing flakiness, which shares a symptom
  (a red integration case) but nothing else. That belongs in a test-harness
  change: the integration library waits on `il_send_delay` sleeps rather than on
  the shell prompt, and under full-suite load the guest misses the deadline.

---

## 7. Why this is worth doing

USB is the one subsystem where AuraLite's usual standard slipped. The project's
documentation is elsewhere unusually candid — `README.md` warns against relying
on xHCI, the source comments describe the synthesis accurately, and
`TODO.md`'s FPU/SSE investigation is a model of how to characterise a bug
before fixing it.

But the boot log announces "full support" and the test suite reports 8/8 green
for a controller that has never moved a byte. That combination is worse than an
empty driver, because it consumes the one thing the rest of the project has
earned: the ability to trust a green test.

The good news, and the reason the plan is nine phases rather than a rewrite:
**most of the real implementation is already written.** The control-transfer
TRB path exists in full at `xhci.c:887` and the compiler already knows it is
unreachable. The ring allocators, context builders, doorbell and PORTSC
handling are real. The class layer above is real and proven by three other
controllers.

What is missing is a working event ring — one function, currently seven lines
returning `-1` — and the discipline to delete the scaffolding that was built to
paper over its absence.
