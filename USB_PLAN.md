# AuraLite OS — Full USB Support Plan

## Status: PLANNED 📋 (phases U0–U9)

| Phase | Title | State |
|---|---|---|
| U0 | Tell the truth in the log and the matrix | 📋 planned |
| U1 | The event ring, and one real command | 📋 planned |
| U2 | Delete the fabrication layer **(critical)** | 📋 planned |
| U3 | Real `Address Device` | 📋 planned |
| U4 | Real control transfers | 📋 planned |
| U5 | Real bulk transfers, and MSC on xHCI | 📋 planned |
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

### Phase U0 — Tell the truth in the log and the matrix

**Objective:** the tree stops claiming support it does not have, *before* any
behaviour changes. This phase changes no data path.

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

#### Test gate

- `grep -c SYNTHETIC` over a boot log with `-device qemu-xhci,...` returns a
  non-zero count at U0 and **must reach zero at U5**. That number is the
  plan's progress metric.
- `test_usb_xhci.sh` now **fails** at U0 via the new guard, with a message
  naming the synthesis. This is the intended, recorded regression.
- No other integration case changes state.

#### Deliverable

`patches/USB_U0_honest_log.patch`

---

### Phase U1 — The event ring, and one real command **(critical)**

**Objective:** `xhci_poll_event_type()` returns a real TRB.

This is the keystone. Nothing above it can work, and everything above it works
almost immediately once it does.

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

#### Test gate

- New boot-time self-test: submit `No Op Command`, receive a Command Completion
  event with completion code 1 (Success). Log it as
  `[xhci] command ring: No Op -> Success (cc=1)`.
- Submit **256 consecutive No Ops** so the command ring wraps its 255-entry
  segment and the Link TRB with Toggle Cycle is exercised; all 256 complete.
- Negative control: deliberately program a wrong `ERDP` and confirm the
  self-test reports a timeout rather than hanging the boot. Revert.
- New host unit test `test_xhci_ring.c`: the cycle-bit/wrap/dequeue arithmetic
  is pure logic and must be tested off-hardware, in the manner of
  `test_w32_pe.c`.

#### Deliverable

`patches/USB_U1_event_ring.patch`

---

### Phase U2 — Delete the fabrication layer **(critical)**

**Objective:** no code path in the tree invents a USB answer.

Placed immediately after U1 and before the real implementations, per D1.

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

#### Test gate

- `test_usb_xhci.sh` fails cleanly with `-ENOTSUP` diagnostics and **no
  fabricated data anywhere in the log**. Recorded as an expected red.
- `grep -i 'AURALUSB\|QEMU HARDDISK\|FAKE\|dev_addr % 3' drivers/usb/` returns
  nothing.
- `test_usbfs_fat32.sh`, `test_usb_msc.sh` (UHCI) and every OHCI/EHCI case stay
  green — proof the deletion was confined to xHCI.
- The build produces zero `-Wunreachable-code` warnings under `drivers/usb/`.

#### Deliverable

`patches/USB_U2_delete_synthesis.patch`

---

### Phase U3 — Real `Address Device` **(critical)**

**Objective:** a device on an xHCI port gets a slot from the controller.

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

#### Test gate

- Boot with `-device qemu-xhci -device usb-kbd,bus=xhci.0`:
  `[xhci] slot 1 addressed (port 2, speed 3)` with the slot ID **reported by
  the controller**, and no `(FAKE)`/`SYNTHETIC` marker in the log.
- Read back the Slot Context from the Device Context after addressing and
  assert the controller wrote `Slot State = Addressed`. This is the assertion
  a fabricated implementation cannot satisfy.
- Attach three devices; assert three distinct slot IDs, all controller-issued.
- Detach and reattach 20 times; assert slots are released and the slot count
  does not creep. Guards the leak this phase makes possible.

#### Deliverable

`patches/USB_U3_address_device.patch`

---

### Phase U4 — Real control transfers **(critical)**

**Objective:** descriptors come from the device.

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

#### Test gate

- The device descriptor read from a QEMU `usb-kbd` reports VID `0x0627`, PID
  `0x0001` — **and the test asserts these came from the device** by also
  attaching `usb-mouse` and `usb-storage` and requiring three *different*
  descriptor sets. The forged path used `dev_addr % 3` and would produce
  exactly this pattern, so the test additionally attaches devices in an order
  that makes the modulo mapping wrong, and asserts the classes still match the
  QEMU command line.
- String descriptors return QEMU's real product strings, not the hardcoded
  `"QEMU"`.
- The HID report descriptor is fetched from the device and its length matches
  what QEMU exposes.
- A deliberate stall (request an invalid descriptor index) recovers, and the
  next request succeeds.

#### Deliverable

`patches/USB_U4_control_transfers.patch`

---

### Phase U5 — Real bulk transfers, and MSC on xHCI **(critical)**

**Objective:** a byte written to the QEMU disk image is the byte the OS reads.

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

#### Test gate

The gate that no forgery can pass:

- Create a disk image, write a **known 512-byte pattern** to sector 0 with
  `dd`, boot with `-device usb-storage,bus=xhci.0`, read sector 0 through
  `/usb/sector0.bin`, and assert **byte equality with the pattern**.
  The baseline returns `AURALUSB…`; only a real transfer returns the pattern.
- INQUIRY returns the vendor/product QEMU was configured with, and the test
  varies them from the defaults so hardcoded values cannot pass.
- READ CAPACITY matches the image's real size; the test uses a size that is not
  the fabricated 16384 sectors.
- Format the image FAT32 and assert `/usb/fat` mounts and lists real files —
  the synthesised sector deliberately had no BPB, so this could never have
  worked before.
- Write a sector, read it back, assert equality; then assert the change is
  visible in the host image file after shutdown.
- `test_usbfs_fat32.sh` passes with the device on **xHCI** as well as UHCI.
- `SYNTHETIC` count in the log reaches **zero** — the U0 metric closes here.

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
