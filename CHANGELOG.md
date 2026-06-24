# Changelog

All notable changes to AuraLite OS. Dates are ISO 8601 (Europe/Moscow local).

## [QEMU integration test harness] 2026-06-24

### Added
- Added `tests/integration/` — a black-box QEMU test harness that boots the
  real ISO and asserts on the serial console.
- `tests/integration/lib/lib.sh`: shared helpers (qemu launcher with stdin
  pumping, raw-disk image bootstrap, colored asserts, log capture).
- 11 self-contained test cases in `tests/integration/cases/`:
  - `test_boot_to_shell`        — phases 0..11 reach Ring 3 init shell.
  - `test_shell_commands`       — help/ls/cat/echo/pwd/free/ps/run.
  - `test_syscalls`             — read/write/open/listdir/getpid surface.
  - `test_user_processes`       — spawn + isolated address space.
  - `test_ahci_rw`              — AHCI DMA + `/disk` + `/fat` write/read.
  - `test_fat32_persistence`    — write file → reboot → still there.
  - `test_usb_msc`              — UHCI + USB MSC READ(10) sector 0.
  - `test_networking`           — e1000 + ICMP + DNS + TCP (DHCP-branched).
  - `test_http_get`             — user-mode `/http` against a local httpd.
  - `test_graphics`             — framebuffer + WM + 3D demo render.
  - `test_smp`                  — Limine MP brings up application processors.
- `tests/integration/run_all.sh`: orchestrator with summary, timings,
  `--fast` mode, name-pattern filter, and `NO_COLOR=1` support.
- Makefile targets `make test-integration`, `make test-integration-fast`,
  and umbrella `make test` (host unit + QEMU integration).
- `.github/workflows/integration.yml`: CI job that installs the toolchain,
  builds the ISO, runs host unit tests + fast integration subset, and
  uploads `build/integration-logs/` as an artifact on failure.
- `tests/integration/README.md` and `tests/integration/RESULTS.md`
  document the harness and a reference run.

### Verified
- Full run on Debian 13 / QEMU 10.0.8 / clang 19 (2 vCPU, 512 MiB):
  **11/11 cases PASSed, 73/73 assertions, ~5 min wall-time.**
- FAT32 persistence: a marker written in boot #1 is read back in boot #2
  from the same disk image.
- USB Mass Storage: kernel completes UHCI control transfers, INQUIRY,
  READ CAPACITY, and READ(10) of sector 0 in a single boot.
- AHCI DMA: kernel self-test + userspace `/disk` and `/fat` round-trip a
  user-provided string through the VFS.


## [FAT32 persistent logging] 2026-06-22

### Added
- Added `kernel/fs/fat32.{c,h}`: a compact AHCI-backed FAT32 implementation.
  - Formats/mounts a small FAT32 volume at LBA 64 when an AHCI disk is present.
  - Mounts the volume at `/fat`.
  - Supports flat 8.3 files with create/read/write through VFS.
  - Appends kernel logs to `/fat/AURALOG.TXT`.
- Added kernel log buffering/sink support in `kernel/lib/klog.{c,h}`.
  - Early boot logs are buffered in memory.
  - When FAT32 is mounted, the backlog is flushed to `AURALOG.TXT`.
  - Later logs are flushed from the idle loop.

### Verified
- QEMU AHCI disk contains a FAT32 signature and root entries for
  `AURALOG.TXT` and `TEST.TXT`.
- `AURALOG.TXT` contains early boot log lines starting with UART/framebuffer/GDT
  initialization.

## [Virtual hardware driver catalog] 2026-06-22

### Added
- Added `drivers/vm/virtual_drivers.{c,h}`: a compatibility/probe layer for
  common QEMU, VirtualBox and VMware PCI devices.
- The boot log now reports the detected hypervisor vendor string and known
  virtual devices with driver status (`active`, `partial`, `boot framebuffer`,
  or `known / no data path`).
- Added recognition entries for many common VM devices: e1000/e1000e, PCnet,
  RTL8139, VMXNET3, virtio-net/block/scsi/gpu/balloon/rng/console, AHCI, PIIX
  IDE, VMware PVSCSI/VMCI/SVGA, LSI SCSI/SAS, BusLogic, VirtualBox Guest Device,
  VBox/VMSVGA, QEMU VGA/QXL, AC'97, HDA, ES1371 and common USB controllers.
- Added `docs/virtual_driver_matrix.md`.

## [VirtualBox stdin noise fix] 2026-06-22

### Fixed
- Fixed infinite `auralite# ... : command not found` loops caused by bogus bytes
  from unattached/floating COM1 serial ports in VirtualBox.
- `SYS_READ(fd=0)` now accepts PS/2 keyboard input as well as serial input, and
  filters invalid UART bytes (`0x00`, `0xFF`, non-ASCII/control noise).
- The init shell sanitises command lines defensively before tokenising them.

## [VirtualBox network boot-timeout tuning] 2026-06-22

### Changed
- Shortened DHCP/ARP/ICMP/UDP/TCP polling budgets so a disconnected or
  unsupported VM network does not stall boot for a long time.
- The e1000 driver now forces `CTRL.SLU`/full-duplex on emulated adapters and
  exposes link-state detection.
- If the link is down, networking skips DHCP entirely and boot continues.
- If DHCP fails, AuraLite keeps fallback static addressing but skips online
  ping/DNS/TCP self-tests to avoid repeated ARP delays during boot.
- DHCP DISCOVER/REQUEST and ARP requests now fail fast when TX fails instead of
  waiting for receive timeouts.

## [AHCI read/write + tmpfs writable files] 2026-06-22

### Added
- Fixed and enabled AHCI DMA sector I/O:
  - command header PRDTL is now written to the high 16 bits of DW0;
  - port interrupt/error state is cleared before command issue;
  - command issue waits for BSY/DRQ to clear;
  - AHCI self-test now reads sector 0, writes scratch sector 1, and reads it
    back to verify DMA read/write.
- `tools/run_qemu.sh` now creates a small raw AHCI test disk automatically and
  forces CD boot order.
- Added `tmpfs`, a writable in-memory filesystem mounted at `/tmp`.
- Added `diskfs`, a tiny persistent AHCI-backed filesystem mounted at `/disk`
  when a SATA disk is available (8 flat files, 4 KiB each).
- VFS can now create files on filesystems that provide a `create` operation.
- VFS file descriptors start at 3, preserving stdin/stdout/stderr semantics.
- `SYS_WRITE` now writes to VFS descriptors `fd >= 3` in addition to console
  stdout/stderr.
- Shell command `write <file> <text>` demonstrates writable files, e.g.
  `write /tmp/note hello` then `cat /tmp/note`.
- The userspace editor now supports `:w <filename>` for saving to writable files.

### Verified
- QEMU AHCI self-test passes: sector 0 read + sector 1 write/readback.
- tmpfs self-test passes at boot.
- diskfs self-test passes: create/write/read `/disk/persist.txt`.

## [USB Mass Storage over UHCI] 2026-06-22

### Added
- Completed the first working USB Mass Storage path through UHCI:
  - multi-packet UHCI control transfers using the actual EP0 max-packet size;
  - UHCI bulk transfers with persistent DATA toggle tracking;
  - real UHCI device enumeration via `SET_ADDRESS`, device/config descriptors,
    endpoint parsing, and `SET_CONFIGURATION`;
  - MSC Bulk-Only Transport: CBW → optional data → CSW;
  - SCSI `TEST UNIT READY`, `REQUEST SENSE`, `INQUIRY`, `READ CAPACITY`,
    `READ(10)`, and `WRITE(10)` plumbing;
  - capacity detection and sector-0 read self-test.
- Added `tools/run_qemu_usb_msc.sh` and `make run-usb-msc` for a QEMU boot with
  a UHCI `usb-storage` disk image.

### Verified
- QEMU boot with attached UHCI USB storage enumerates the device as Mass Storage,
  reads capacity, and reads sector 0 successfully:
  - VID/PID: `0x46f4:0x0001`
  - endpoints: bulk IN `0x81`, bulk OUT `0x02`
  - capacity: 32768 sectors × 512 bytes
  - sector 0 starts with the test signature `AURALUSB`

## [Documentation refresh + VM guide] 2026-06-22

### Changed
- Rewrote the top-level README to reflect the current post-phase repository
  state, including stable vs experimental subsystems, VM support, known
  limitations, and documentation map.
- Added `docs/README.md` as the documentation index.
- Added `docs/build_and_run.md` with build, QEMU, VirtualBox, VMware and
  troubleshooting instructions.
- Added `docs/status.md` with a feature-completeness matrix.
- Updated `docs/syscall_abi.md` to include process and networking syscalls.
- Updated `docs/driver_guide.md` to cover AHCI, USB, mouse, Bluetooth, Wi-Fi,
  VirtualBox/VMware e1000 variants and WIP status.
- Updated architecture and memory-map docs with post-phase subsystem notes and
  current caveats.

## [Full GUI] 2026-06-21

### Upgraded - Window Manager
- Rewrote wm.{c,h} with full GUI framework: desktop gradient, taskbar with clock,
  window shadows, close [X] buttons, widget framework (buttons, labels, progress
  bars, text areas, rectangles), mouse interaction (focus/drag/close/press).
- 3 demo windows: Terminal+buttons, System Monitor+progress bars, About+close.

## [Web Browser] 2026-06-21

### Added
- `userspace/browser/browser.c`: text-based web browser.
  - URL parser: `host[:port]/path` (strips `http://` prefix)
  - HTTP/1.0 GET request builder
  - HTML tag stripper: renders visible text content from HTML
  - Title extraction: `<title>` → `=== Title ===`
  - Heading formatting: `<h1>-<h3>` get blank line separation
  - Link extraction: `<a href="...">` → `[url]` prefix
  - HTML entity decoder: `&amp;`, `&lt;`, `&gt;`, `&nbsp;`, `&quot;`, `&#39;`
  - Script/style content suppression
  - Whitespace collapsing (no excessive blank lines)
  - HTTP status line display
  - Response body extraction (skips HTTP headers)
- Verified: connected to example.com, sent HTTP GET, received 8191 bytes
  of real HTML via DNS → TCP → HTTP over QEMU user-mode networking.

### Verified end-to-end
```
browser> example.com
  Resolved: example.com (172.66.147.243)
  [tcp] ESTABLISHED (seq=4097, ack=1408002)
  Received 8191 bytes
```

## [Full Ethernet/Internet Support] 2026-06-21

### Added — Network Syscalls (userspace internet access)
- `SYS_NET_CONNECT` (83): TCP connect to IP:port from userspace
- `SYS_NET_SEND` (84): Send data over established TCP connection
- `SYS_NET_RECV` (85): Receive data from TCP connection (polling)
- `SYS_NET_CLOSE` (86): Close TCP connection (FIN/ACK teardown)
- `SYS_NET_PING` (87): ICMP echo from userspace
- Libc wrappers: `net_connect()`, `net_send()`, `net_recv()`, `net_close()`,
  `net_ping()`

### Added — Gateway Routing (ARP)
- Subnet mask tracking: the global `subnet_mask` is set from DHCP
- IP routing: when the target IP is NOT on our local subnet (based on the
  subnet mask), ARP resolves the gateway's MAC and routes through it
- This enables pinging and connecting to external hosts (not just the local
  subnet/gateway)

### Added — Real HTTP Client
- Rewrote `userspace/http/http.c` as a real HTTP/1.0 client:
  - DNS resolution → TCP connect → HTTP GET → response display
  - User types a hostname (e.g. `example.com`)
  - Resolves via DNS, connects via TCP (port 80)
  - Sends `GET / HTTP/1.0\r\nHost: ...\r\n\r\n`
  - Receives and prints the HTTP response
  - Verified: connected to example.com, sent HTTP request

### Added — Shell `ping` Command
- `ping <hostname>` in the interactive shell
  - Resolves hostname via DNS
  - Sends ICMP echo via `net_ping()` syscall
  - Reports reply or timeout

### Full Network Stack Summary
| Layer | Protocol | Status |
|-------|----------|--------|
| Physical | e1000 NIC (PCI, MMIO, DMA) | ✅ |
| Auto-config | DHCP (DISCOVER→OFFER→REQUEST→ACK) | ✅ |
| L2 | Ethernet framing, ARP (with gateway routing) | ✅ |
| L3 | IPv4 (routing, fragmentation, checksum) | ✅ |
| L4 | ICMP (ping), UDP (DNS), TCP (connect/send/recv/close) | ✅ |
| App | DNS resolver, DHCP client, HTTP client | ✅ |
| Userspace | Network syscalls (connect/send/recv/close/ping) | ✅ |
| Shell | `ping`, `nslookup`, `run /http` | ✅ |

## [Wi-Fi (IEEE 802.11)] 2026-06-21

### Added
- `drivers/wifi/wifi.{c,h}`: IEEE 802.11 Wi-Fi MAC layer management.
  - **802.11 frame structures**: Frame Control (type/subtype bit fields),
    Management header (24 bytes), Beacon/Probe Response body, Authentication
    body, Association Request/Response body
  - **Active scanning**: builds Probe Request frames with SSID wildcard,
    Supported Rates, and DS Parameter IEs; sends on channels 1-11
  - **Information Element parser**: extracts SSID, channel, RSN (WPA2)
    from Beacon/Probe Response frames
  - **Connection state machine**: DISCONNECTED → SCANNING → AUTHENTICATING →
    ASSOCIATING → CONNECTED → ERROR
  - **Authentication**: Open System auth frame construction
  - **Association**: Association Request with capability, listen interval,
    SSID IE, and Supported Rates IE
  - **Data frame conversion**: Ethernet → 802.11 Data frame with LLC/SNAP
    header, addr1=BSSID, addr2=our MAC, addr3=destination
  - **Driver interface**: `wifi_driver_t` with `tx_raw`, `set_channel`,
    `get_mac` callbacks — any wireless NIC chipset driver (Intel iwlwifi,
    Realtek rtl8188, Atheros ath9k) can register
  - `wifi_init()`, `wifi_scan()`, `wifi_connect()`, `wifi_send_data()`,
    `wifi_get_state()`, `wifi_get_bssid()`

## [Bluetooth HCI] 2026-06-21

### Added
- `drivers/bluetooth/bt.{c,h}`: Bluetooth HCI driver.
  - USB device detection (class 0xE0 or vendor 0x0A12)
  - HCI command builder + packet structures
  - Commands: Reset, Read BD_ADDR, Read Local Version, Inquiry
  - Event parser: Command Complete, Command Status, Inquiry Result
  - USB transport via usb_control_transfer + uhci_bulk_transfer
  - `bt_init()`, `bt_inquiry()`, `bt_get_bd_addr()`

## [Mouse + Window Manager] 2026-06-21

### Added
- `drivers/mouse/mouse.{c,h}`: PS/2 mouse driver (8042 auxiliary channel,
  IRQ 12). Initialises the mouse via the 8042 command interface, parses 3-byte
  relative-movement packets, maintains absolute cursor position clamped to
  screen bounds, and tracks button states.
- `drivers/framebuffer/wm.{c,h}`: minimal window manager with:
  - Z-ordered windows with title bars, borders, and content areas.
  - Compositing: renders all visible windows bottom-to-top into the back
    buffer, then draws the mouse cursor on top and flips.
  - Mouse interaction: click a title bar to focus + drag, release to drop.
  - `wm_draw_text`, `wm_clear_window`, `wm_fill_window_rect` for content.
  - Window demo: "AuraLite Terminal", "System Info", and "Tip" windows.
- Mouse cursor rendering (arrow shape with outline).

### Changed
- kmain now calls `mouse_init()` and `wm_demo()` alongside the graphics init.
- CI gate message updated to match the new "[gfx] framebuffer GUI + window
  manager rendered" output.

## [Full USB Support: Enumeration + Transfers] 2026-06-21

### Added — USB Core Enumeration Layer
- `drivers/usb/usb_core.{c,h}`: USB device enumeration and protocol.
  - Standard USB request builders: SET_ADDRESS, GET_DESCRIPTOR, SET_CONFIGURATION
  - USB descriptor parsing: device (18B), configuration, interface, endpoint
  - Device class detection: HID (0x03), MSC (0x08), Hub (0x09)
  - USB device table management (up to 16 devices)
  - `usb_control_transfer()`: dispatches to the correct host controller
  - Full enumeration sequence: SET_ADDRESS → GET_DESCRIPTOR(DEVICE)

### Added — UHCI Transfer Layer
- `uhci_control_transfer()`: builds SETUP → DATA → STATUS TD chain
  - `make_td_token()`: encodes PID, device address, endpoint, toggle, length
  - `make_td_ctrl()`: encodes low-speed, error counter, active bit
  - `uhci_schedule_tds()`: replaces frame list entries with transfer QH,
    waits for completion, restores frame list
  - `uhci_bulk_transfer()`: single-TD bulk transfer for MSC
  - `uhci_port_is_low_speed()`: returns device speed per port
- Verified: successfully enumerated USB keyboard (VID=0x0627 PID=0x0001)
  and USB hub (VID=0x0409 PID=0x55AA) via UHCI.

### Bugs found and fixed
- **SET_ADDRESS(0) no-op**: `dev->address` was set to 0 for the initial
  transfer but never restored before SET_ADDRESS, so it sent SET_ADDRESS(0).
  Fixed: save the assigned address, use 0 only for the initial descriptor read.
- **STATUS TD link chain**: in the no-data-phase case, TD1 (STATUS) linked to
  TD2 (unused, all zeros). The controller processed TD2's zero ctrl field
  indefinitely because CERR=0 prevented retirement. Fixed: TD1.link = 0x1
  (terminate) directly, eliminating the spurious TD2.
- **Frame list replacement**: the original approach chained via the idle QH's
  head_link, which was unreliable. Fixed: replace all 1024 frame list entries
  directly with the transfer QH for guaranteed scheduling.

### Complete USB Stack Summary
AuraLite OS now has a full USB stack:

| Layer | Component | Status |
|-------|-----------|--------|
| Host Controllers | UHCI, OHCI, EHCI, xHCI | ✅ All detected and running |
| Transfer Layer | UHCI control + bulk transfers | ✅ Working (TD scheduling) |
| Enumeration | SET_ADDRESS, GET_DESCRIPTOR, SET_CONFIGURATION | ✅ Working |
| Device Table | Up to 16 devices with class detection | ✅ |
| Class Drivers | MSC (CBW/CSW/SCSI), HID (protocol ready) | Protocol ready |
| Mass Storage | Read/write API + SCSI command set | Protocol ready |

Verified: QEMU USB keyboard + hub enumerated with VID/PID and class detection.

## [xHCI (USB 3.0)] 2026-06-21

### Added
- `drivers/usb/xhci.{c,h}`: xHCI host controller driver for USB 3.0.
  - PCI detection (class 0x0C/0x03, prog_if 0x30)
  - Capability register parsing: CAPLENGTH, HCIVERSION, HCSPARAMS1/2/3,
    HCCPARAMS1, DBOFF (doorbell offset), RTSOFF (runtime register offset)
  - Full register space mapping: capability, operational, runtime, doorbell
  - Controller halt → HCRST → wait for CNR clear → start sequence
  - MaxSlotsEn configuration
  - DCBAA (Device Context Base Address Array) allocation and programming
  - Scratchpad buffer allocation (when requested by the controller)
  - Command Ring: circular TRB ring with Link TRB, CRCR programming
  - Event Ring + ERST (Event Ring Segment Table): primary interrupter setup
  - Port power-on, port reset (50ms), port speed detection
  - Supports all USB speeds: low (1.5 Mbps), full (12 Mbps), high (480 Mbps),
    and super-speed (5 Gbps)
  - Full data structures defined: TRB (16 bytes), ERST entry (16 bytes),
    QH/qTD templates, all TRB types and control bits
- Verified: detects super-speed (5 Gbps) USB storage + high-speed (480 Mbps)
  keyboard simultaneously on a single xHCI controller.

### Bug found and fixed
- **HCSPARAMS1 MaxPorts field**: the port count field in HCSPARAMS1 is at
  bits 24-31 (not 16-23 as in some documentation). QEMU's xHCI stores the port
  count at bits 24-31. Fixed the mask to use `0xFF000000`.

### Complete USB Stack
AuraLite OS now implements all four USB host controller interfaces:

| Controller | Interface | Speed | Status |
|---|---|---|---|
| **UHCI** | I/O ports (PIIX3) | USB 1.1 full-speed | ✅ |
| **OHCI** | Memory-mapped | USB 1.1 full-speed | ✅ |
| **EHCI** | Memory-mapped | USB 2.0 high-speed | ✅ |
| **xHCI** | Memory-mapped | USB 3.0 (all speeds) | ✅ |

All four can coexist and detect devices simultaneously.

## [EHCI (USB 2.0)] 2026-06-21

### Added
- `drivers/usb/ehci.{c,h}`: EHCI host controller driver for high-speed USB 2.0.
  - PCI detection (class 0x0C/0x03, prog_if 0x20)
  - Capability register parsing (CAPLENGTH, HCIVERSION, HCSPARAMS, HCCPARAMS)
  - Controller halt → reset → operational transition
  - 1024-entry periodic frame list (4 KiB, PMM-allocated)
  - Async list head QH (self-referencing circular list, HBR bit set)
  - Configured Flag (route ports to EHCI)
  - Port power-on, port reset (50ms), companion release for low-speed
  - Frame index verification (confirms schedule is advancing)
  - QH (48 bytes) and qTD (32 bytes) structures fully defined with all fields
  - 64-bit addressing support detection
  - Companion controller routing awareness (releases low/full-speed to UHCI/OHCI)
- Verified with QEMU `-device usb-ehci`: detects high-speed USB storage device,
  async + periodic schedules active, frame index advancing (280 → 352).

### USB Stack Summary
AuraLite OS now supports all three USB host controller interfaces:
  - **UHCI** (Intel PIIX3, I/O port-mapped, USB 1.1) ✅
  - **OHCI** (memory-mapped, USB 1.1) ✅
  - **EHCI** (memory-mapped, USB 2.0 high-speed) ✅

All three can coexist: UHCI handles full-speed keyboard/mouse, OHCI is
available for companion devices, EHCI handles high-speed devices and
releases low/full-speed ports to companions.

## [OHCI + USB Mass Storage] 2026-06-21

### Added — OHCI (USB 1.1)
- `drivers/usb/ohci.{c,h}`: OHCI host controller driver for memory-mapped USB.
  - PCI detection (class 0x0C/0x03, prog_if 0x10)
  - Controller reset, HCCA allocation (256-byte DMA structure)
  - Frame interval, periodic start, low-speed threshold setup
  - Root hub port enumeration (up to 15 ports)
  - Port reset, port enable, power-on sequencing
  - Operational state transition (RESET → OPERATIONAL)
  - ED (Endpoint Descriptor) and TD (Transfer Descriptor) structures defined
  - Frame counter verification
  - Verified: detects USB device on OHCI port in QEMU with `-device pci-ohci`

### Added — USB Mass Storage (MSC)
- `drivers/usb/msc.{c,h}`: USB Mass Storage Class (Bulk-Only Transport).
  - CBW (Command Block Wrapper) builder with correct 31-byte layout
  - CSW (Command Status Wrapper) parser
  - SCSI command builders: INQUIRY, READ_CAPACITY, READ(10), WRITE(10),
    TEST_UNIT_READY, REQUEST_SENSE
  - `msc_exec_scsi()` transport function (stub — needs USB bulk transfer layer)
  - Reads from both UHCI and OHCI controllers for device detection
  - Full block-device API: `msc_read()`, `msc_write()`, `msc_get_sector_count()`

### Status
- **OHCI**: Controller detection, reset, port enumeration, and operational
  transition all verified working. Frame counter advancing confirms scheduling.
- **MSC**: CBW/CSW protocol layer and SCSI command set are fully implemented
  and unit-testable. Actual USB bulk transfers require the UHCI/OHCI TD
  scheduling layer to complete the data path.

## [Boot from USB] 2026-06-21

### Added
- `make usb` target: creates a bootable USB image (`build/usb.img`) from the
  ISOhybrid Limine ISO. The resulting image can be:
  - Booted in QEMU: `qemu-system-x86_64 -drive file=usb.img,format=raw`
  - Written to a real USB stick: `sudo dd if=usb.img of=/dev/sdX bs=4M`
- `tools/mkusbimage.sh`: documents the USB image creation process.
- `boot/limine/limine-usb.conf`: boot config for USB/HDD boot (uses `boot():`
  for partition-relative paths).

### Verified
- Full boot from USB image in QEMU with `-drive file=usb.img,format=raw`:
  - Limine loads the kernel + initrd module
  - All subsystem self-tests pass (PMM, VMM, heap, timer, scheduler, VFS,
    DHCP, ping, DNS, TCP, UHCI)
  - USB keyboard + mouse detected on UHCI ports 0 and 1
  - Interactive shell available
- The ISOhybrid image boots from both CD-ROM (`-cdrom`) and hard drive
  (`-drive`) positions.

## [USB UHCI Driver] 2026-06-21

### Added
- `drivers/usb/uhci.{c,h}`: UHCI (USB 1.1) host controller driver.
  - PCI detection (class 0x0C/0x03 or vendor 0x8086:0x7020 for PIIX3)
  - Controller reset + global reset sequence
  - 1024-entry frame list (PMM-allocated, 4 KiB) with idle QH per entry
  - Port enumeration: detects attached devices, reports speed (low/full)
  - Port reset sequence (50ms reset pulse, port enable, status clear)
  - Frame counter verification (proves the controller is actively scheduling)
  - UHCI data structures: Transfer Descriptor (TD), Queue Head (QH)
- Verified: detects USB keyboard (full-speed) + USB mouse (full-speed)
  in QEMU with `-usb -device usb-kbd -device usb-mouse`.

### QEMU configuration
```
-usb -device usb-kbd -device usb-mouse
```

## [3D Software Renderer] 2026-06-21

### Added
- `drivers/framebuffer/render3d.{c,h}`: software 3D renderer with:
  - `vec3` vector math: add, sub, scale, dot, cross, length, normalize
  - 4x4 `mat4` matrices: identity, multiply, rotation (X/Y/Z), translation,
    perspective projection
  - Freestanding `sin`/`cos`/`sqrt`/`tan` (Taylor series, no `<math.h>`)
  - Perspective projection (3D world → 2D screen coordinates)
  - Wireframe mesh rendering (`r3d_draw_mesh_wire`)
  - Filled triangle rasterisation with flat shading + painter's algorithm
    depth sort + backface culling (`r3d_draw_mesh_filled`)
  - Built-in meshes: cube (8 verts, 12 tris) and pyramid (5 verts, 6 tris)
  - Demo: 30-frame animation of a rotating filled cube + wireframe cube +
    wireframe pyramid with directional lighting
- SSE enabled in boot.asm (CR0.MP, CR4.OSFXSR) for floating-point math
- render3d.c compiled with `-msse -msse2 -mfpmath=sse` (per-file override)

## [PSF2 Font Support] 2026-06-21

### Added
- `drivers/framebuffer/psf.{c,h}`: PSF (PC Screen Font) parser and renderer.
  Supports PSF1 format (8xN glyphs). Renders glyphs with proper MSB-first bit
  ordering and configurable fg/bg colours.
- `drivers/framebuffer/psf_font.h`: the lat0-16.psf PSF1 8x16 font embedded
  as a C array (256 glyphs × 16 bytes = 4 KiB). Replaces the previous 8x8
  font for much sharper, more readable text.
- `psf_draw_glyph()` and `psf_draw_string()` for rendering text at arbitrary
  pixel positions with the PSF font.

### Changed
- `drivers/framebuffer/fb.c`: now uses the PSF 8x16 font instead of the old
  8x8 font8x8_basic. The console cursor metrics (cols/rows) are derived from
  the font dimensions at init time.
- The framebuffer console now shows 80×50 characters (was 160×100 with 8x8)
  — fewer but much more readable characters at the 1280×800 resolution.

## [Applications + libc Fixes] 2026-06-21

### Added — User-space applications
- `userspace/calc/calc.c`: interactive calculator with recursive-descent parser
  supporting +, -, *, /, %, parentheses, and negative numbers. Correct operator
  precedence verified: `2+3*4=14`, `(2+3)*4=20`, `100/7=14`.
- `userspace/sysinfo/sysinfo.c`: system information display (OS version, arch,
  features, subsystem checklist, PID).
- `userspace/editor/editor.c`: line-based text editor (:p print, :d N delete,
  :q quit, type to append).
- `userspace/http/http.c`: HTTP client stub (TCP syscalls not yet exposed).
- `userspace/clock/clock.c`: clock/uptime display with 5-second countdown demo.
- `userspace/guess/guess.c`: number guessing game (1-100, xorshift RNG,
  higher/lower feedback, attempt scoring).
- `userspace/snake/snake.c`: turn-based terminal Snake game (wasd controls,
  20x10 grid, food, score, wall/self collision detection).
- `libc/include/stdlib.h`: atoi, strtol, srand, rand (xorshift32).
- All 7 new apps (plus init + hello = 9 total) packaged in the initrd.

### Fixed
- **User-space printf %ld format:** the printf didn't parse length modifiers
  (`l`/`ll`), so `%ld` printed literally. Added length modifier support that
  reads the correct 32-bit or 64-bit va_arg based on the modifier.
- **SYS_READ sched_yield crash:** SYS_READ called sched_yield() from within the
  SYSCALL handler (which runs on the user stack), corrupting the context switch.
  Fixed: SYS_READ now spin-polls the UART directly without yielding.
- **libdeps:** removed unused `buf` from sysinfo.c and unused `n` from http.c.

## [AHCI SATA Driver] 2026-06-21

### Added
- `drivers/ahci/ahci.{c,h}`: AHCI SATA driver skeleton. PCI class-code scan
  (0x01/0x06) to find the AHCI controller, ABAR (BAR5) MMIO mapping, port
  enumeration via PI register, device detection via SSTS/SIG, per-port command
  list + FIS receive + command table setup (all PMM-allocated for DMA).
- `pci_find_class()` and `pci_get_subclass()` added to the PCI driver.
- QEMU launch scripts updated with `-device ahci,id=ahci0 -device ide-hd`.

### Status
- Controller detection, port init, and command table setup all verified working.
- The PxCI command-issue write triggers a triple fault (investigation ongoing —
  likely a QEMU AHCI interrupt delivery interaction or TLB invalidation issue
  after address-space switching). The self-test is disabled until resolved.

## [DHCP] 2026-06-21

### Added
- DHCP client (`net_dhcp()`): full DORA exchange (DISCOVER → OFFER → REQUEST →
  ACK) over UDP broadcast (port 67/68). Parses the DHCP options to extract the
  assigned IP, subnet mask, gateway, and DNS server. Updates `our_ip` and
  `gateway_ip` on success. Falls back to the hardcoded QEMU defaults on failure.
- `net_init()` now calls `net_dhcp()` before the self-tests, so all subsequent
  network operations use the DHCP-assigned address.
- DHCP option parser: `dhcp_find_option()` walks the variable-length options
  field, handling padding (0x00) and termination (0xFF).

### Fixed
- **e1000 broadcast acceptance (RCTL_BAM):** the NIC was configured with
  unicast promiscuous mode but NOT broadcast accept mode (bit 15 of RCTL).
  DHCP OFFER packets (sent to the broadcast MAC) were silently dropped. Fix:
  added `RCTL_BAM` to the receive control register.

## [TCP] 2026-06-21

### Added
- `kernel/net/tcp.{c,h}`: minimal TCP client implementation with:
  - Three-way handshake (SYN → SYN-ACK → ACK) for active open
  - Data send/recv with sequence numbers and acknowledgments
  - Clean teardown (FIN → FIN-ACK → ACK)
  - Correct TCP checksum with pseudo-header (IPv4 src/dst + protocol + length)
  - Single-connection model (polling-based, consistent with the rest of the stack)
  - `tcp_connect()`, `tcp_send()`, `tcp_recv()`, `tcp_close()`
- TCP self-test: connects to QEMU's DNS server (10.0.2.3:53) via TCP, sends a
  DNS-over-TCP query, receives a response, and cleanly closes.
- Exposed `net_eth_send`, `net_arp_resolve`, `net_get_mac`, `net_get_our_ip`
  from net.c for TCP's use.

### Fixed
- **TCP checksum pseudo-header byte order**: the IP addresses were being passed
  to the checksum function in network byte order (via `htonl_`) but the function
  expected host byte order. This caused an incorrect checksum and QEMU SLIRP
  silently dropped the SYN. Fix: pass host-order IPs and extract octets
  manually inside the checksum function.

## [UDP + DNS + Per-Process Address Spaces] 2026-06-21

### Added — Per-Process Address Spaces
- `kernel/proc/process.{c,h}`: `do_fork()`, `do_execve()`, `do_wait4()`,
  `process_spawn()`. Each user process gets its own PML4 (kernel half shared).
- `paging_clone_user_space()`: deep-copy of user-space pages for fork().
- `paging_switch_to()`: CR3 switch (only when entering a user process — never
  when switching back, since the kernel half is shared).
- `fork_return.asm`: SYSRET for fork children (returns to user mode with RAX=0).
- Scheduler switches CR3 based on the TCB's `pml4_phys` field.
- New syscalls: SYS_FORK (57), SYS_EXECVE (59), SYS_WAIT4 (61), SYS_SPAWN (81).
- Shell `run <prog>` command: spawns a program in an isolated address space.
- Process self-test: spawns /hello in its own address space and verifies output.

### Added — UDP + DNS
- `net_udp_send()` / `net_udp_recv()`: send/receive UDP datagrams over IPv4.
- `net_dns_resolve()`: DNS resolver via UDP to QEMU's proxy (10.0.2.3:53).
  Encodes hostname to DNS label format, sends query, parses A-record response.
- New syscall: SYS_DNS (82) — userspace `dns_resolve()` wrapper.
- Shell `nslookup <hostname>` command (e.g. `nslookup google.com`).
- Verified: `example.com → 172.66.147.243`, `google.com → 142.250.107.102`.

### Changed
- Shell now runs in its own address space (not the kernel's).
- TCB extended with `pml4_phys`, `exit_code`, `parent`, `waited_on`.
- `thread_exit()` clears `parent->waited_on` to unblock wait4.

## [Phases 13–14 — Networking + GUI] 2026-06-21

### Added — Phase 13: Networking
- `drivers/pci/pci.{c,h}`: PCI config space access (0xCF8/0xCFC), bus scan,
  device lookup, BAR read, bus-master enable.
- `drivers/e1000/e1000.{c,h}`: Intel 82540EM NIC driver. MMIO register
  access, legacy TX/RX descriptor rings, polling-based send/recv.
- `kernel/net/net.{c,h}`: Ethernet + ARP + IPv4 + ICMP stack. ARP resolution
  with cache, RFC 1071 internet checksum, ICMP echo request/reply.
- 32-bit port I/O (`inl`/`outl`) added to `portio.h`.
- MMIO region explicitly mapped via paging (HHDM doesn't cover device MMIO).
- TX/RX descriptors and buffers allocated from the PMM (DMA needs physical
  addresses; descriptors marked volatile for DMA visibility).
- `net_ping()` and `net_self_test()`: ARP-resolve 10.0.2.2, send ICMP echo,
  poll for reply.

### Added — Phase 14: GUI
- `drivers/framebuffer/graphics.{c,h}`: 2D graphics library with double-
  buffering. Pixel plotting, filled/outlined rectangles, Bresenham line,
  bitmap-font text, back-buffer flip.
- `drivers/keyboard/keyboard.{c,h}`: PS/2 keyboard driver (IRQ 1, scan-code
  set 1, ring buffer, ASCII translation).
- Boot screen demo: title bar, coloured rectangles, diagonal line, info text.

### Fixed
- **EEPROM read hang:** QEMU's e1000 doesn't reliably set EERD_DONE. Added
  timeout + RAL/RAH fallback.
- **MMIO unmapped:** the e1000's BAR0 lives at ~4GB, beyond the HHDM's RAM
  range. Fix: explicitly map 128 KiB of MMIO via paging.
- **TX descriptor layout:** corrected the 16-byte legacy descriptor field
  layout (cso/cmd/status/css are bytes, not uint16s).
- **RX descriptor polling:** QEMU advances RDH but the descriptor status byte
  may not be visible through the HHDM due to DMA ordering. Fix: poll RDH via
  MMIO instead of reading descriptor status.

## [Phase 12 — SMP] 2026-06-21

### Added
- `kernel/arch/x86_64/smp.{c,h}`: multi-processor bringup via Limine's MP
  request. Each AP gets a goto_address function that loads the shared GDT/IDT,
  switches to its own stack, reports online atomically, and idles (hlt).
- Limine MP request added to the boot-protocol bridge.
- Exposed `gdtr` (gdt.c) and `idtp` (idt.c) as non-static so APs can reload them.
- SMP-safe `kprintf`: global print spinlock (cli/sti is per-CPU under SMP).
- `smp_self_test()`: detects single-core vs multi-core and reports CPU count.

### Fixed
- **BSP in cpus[] array:** Limine includes the BSP in the MP response. Setting
  goto_address on the BSP was a no-op, leaving one AP asleep. Fix: skip entries
  matching bsp_lapic_id.
- **Volatile visibility:** the goto_address/extra_argument writes needed volatile
  access + mfence to be visible to Limine's AP polling.

## [Phase 11 — init, Shell & Utilities] 2026-06-21

### Added
- Expanded syscalls: SYS_OPEN (2), SYS_CLOSE (3), serial-input SYS_READ (0,
  fd=0 polls UART with sched_yield), SYS_LISTDIR (80).
- UART receive: `uart_has_data()`, `uart_getchar()`.
- Expanded libc: `printf` (%s %d %u %x %c %% with width/zero-pad), `puts`,
  `putchar`, `strtok`, `strcmp`, `strncmp`, `strcpy`, `memset`, `memcpy`,
  `strlen`, `memcmp`.
- `libc/include/stdio.h`, `libc/include/string.h`.
- `userspace/init/init.c`: interactive shell with built-in commands (ls, cat,
  echo, pwd, uname, free, help, exit). Reads from serial input (stdin=fd 0).
- Two separate user ELFs: init.elf (shell, embedded in kernel) and hello.elf
  (simple test, in initrd only).

### Changed
- The embedded user binary is now the init shell (not hello). The initrd
  contains both /init and /hello.
- kmain yields forever after starting the shell (instead of halting).
- CI test now sends shell commands via serial and verifies output.
- VFS initialisation moved before user-mode init (shell needs VFS for ls/cat).

### Fixed
- **IF leakage in context_switch:** RFLAGS (including IF) wasn't saved/restored,
  so the interrupt flag leaked between threads. A timer firing mid-SYSCALL
  corrupted the stack. Fix: pushfq/popfq in context.asm.
- **SYSRET SS DPL mismatch:** GDT had user code (index 3) before user data
  (index 4). SYSRET's formula loaded SS from the kernel data segment (DPL=0)
  with RPL=3, failing the CPL check. Fix: swapped user code/data in the GDT,
  set STAR[63:48]=0x10 so SYSRET produces SS=0x1B and CS=0x23 (both DPL-3).
- Stack frame for new threads updated to include RFLAGS slot (matching the
  pushfq/popfq in context_switch).

## [Phase 10 — File System & VFS] 2026-06-21

### Added
- `kernel/fs/vfs.{c,h}`: virtual file system with a mount table (longest-prefix
  matching), vnode abstraction, a global FD table, and `vfs_open`/`read`/`write`/
  `close`.
- `kernel/fs/initrd.{c,h}`: USTAR (POSIX tar) initrd parser. Walks 512-byte
  headers, parses octal sizes, strips `./` prefixes, exposes files read-only
  via VFS ops.
- `kernel/fs/devfs.{c,h}`: `/dev/null` (EOF on read, discards writes) and
  `/dev/zero` (zero-filled reads, discards writes).
- Limine module request to receive the initrd as a boot module.
- `tools/mkinitrd.sh`: packs userspace binaries into a USTAR tarball.
- `limine.conf` + `mkisoimage.sh`: the initrd is included in the ISO as a module.
- `strcmp` added to the freestanding string library.
- VFS self-test (dev/null write, dev/zero read, /init read).

### Fixed
- **`/init` not found:** GNU tar stores paths with a `./` prefix; the USTAR
  parser now strips it.

## [Phase 9 — System Calls] 2026-06-21

### Added
- Minimal libc for user programs:
  - `libc/include/unistd.h`: syscall number constants + POSIX-style declarations.
  - `libc/src/syscall.asm`: generic 7-arg syscall wrapper (remaps C ABI → SYSCALL ABI).
  - `libc/src/libc.c`: `write`/`read`/`_exit`/`getpid` wrappers.
  - `libc/crt/crt0.asm`: `_start` → `main` → `_exit`.
  - `libc/user.ld`: user linker script (links at `0x40000000`).
- `userspace/hello/hello.c`: the Phase 9 gate-test program (`write(1, "hello\n", 6)`).
- `kernel/proc/elf.{c,h}`: ELF64 loader (validates Ehdr, maps PT_LOAD segments
  with USER perms, skips already-mapped pages for co-located segments, zero-fills .bss).
- `tools/gen_user_binary.py`: converts compiled ELF → C array for kernel embedding.
- Makefile `user` target: builds hello.elf → generates `hello_bin.h` → kernel.
- Expanded syscalls: SYS_READ, SYS_WRITE, SYS_EXIT, SYS_GETPID.

### Changed
- `user_mode_self_test` now loads the compiled `hello.elf` via the ELF loader
  instead of the Phase 8 hand-assembled program.
- SYSCALL handler now switches to a dedicated kernel stack (`set_syscall_stack`)
  before processing, preventing user-stack corruption.
- CI gate updated to check for "hello" output + new PASS message.

### Fixed
- **SYSRET wrong CS:** NASM `sysret` (32-bit operand) set `CS = STAR[63:48] | 3`
  instead of `(STAR[63:48] + 0x10) | 3`. Fixed with `o64 sysret` (`48 0F 07`).
- **SYSCALL stack corruption:** SYSCALL doesn't switch stacks, so the C handler
  ran on the user's RSP and corrupted return addresses. Fixed by manually
  switching to a kernel stack at `syscall_entry`.
- **ELF segment co-location:** two PT_LOAD segments sharing a page caused the
  second mapping to overwrite the first. Fixed by skipping already-mapped pages.

## [Phase 8 — Processes & User Mode] 2026-06-21

### Added
- Expanded GDT with user code/data segments (DPL=3) and a 64-bit TSS descriptor
  (7 entries: the TSS descriptor occupies 16 bytes / 2 slots).
- `kernel/arch/x86_64/tss.{c,h}`: TSS setup with RSP0 (the kernel stack loaded
  on Ring 3→0 transitions) and IST1 (a dedicated stack for the #DF handler).
- `kernel/arch/x86_64/syscall.{c,h}` + `syscall_entry.asm`: SYSCALL/SYSRET
  MSR configuration (STAR, LSTAR, SFMASK, EFER.SCE) + a C dispatch with
  SYS_WRITE and SYS_EXIT.
- `kernel/proc/user.{c,h}` + `user_entry.asm`: `iretq` to Ring 3, an embedded
  user program (syscall write + cli), and the Phase 8 gate test.

### Changed
- Exception handler now detects the faulting privilege level (CS & 3) and, for
  user-mode faults, recovers by killing the user thread instead of halting.
- `gdt_set_tss()` correctly writes the upper 32 bits of the higher-half TSS
  base into the 16-byte descriptor's second half.
- The user test runs as its own kernel thread so its kernel stack (TSS.RSP0)
  is isolated from kmain.

### Fixed
- TSS #GP on LTR: GDT expanded to 7 entries (16-byte TSS descriptor).
- LSTAR truncated to 32 bits: `mov rdx,rax; shr rdx,32` before WRMSR.
- `sysretq` → `sysret` (NASM mnemonic).
- User program RIP-relative offset corrected to point at the message.

## [Phase 7 — Multitasking & Scheduler] 2026-06-21

### Added
- `kernel/proc/context.asm`: `context_switch(old, new)` — saves/restores
  callee-saved registers (rbx, rbp, r12–r15) and RSP; resumes via `ret`.
- `kernel/proc/thread.{c,h}`: Thread Control Block (rsp at offset 0 for asm
  access), thread-state enum, `kthread_create` (crafts the initial stack frame
  so the first switch lands at the `thread_entry` trampoline), `thread_exit`.
- `kernel/proc/scheduler.{c,h}`: round-robin ready queue (FIFO tail-append /
  head-dequeue), `schedule` / `sched_yield` / `sched_tick` / `sched_current`,
  idle-thread fallback, `scheduler_self_test`.
- Timer IRQ handler now calls `sched_tick()` for quantum-based preemption.
- `strncpy` added to the freestanding string library.

### Changed
- `irq_dispatch` sends PIC EOI *before* the handler (enables timer to fire
  again after a context switch inside the handler).
- `kprintf` is now atomic (cli/sti wrapper) to prevent garbled interleaving
  under preemption.
- `paging_self_test` no longer deliberately faults (Phase 4 historical record).

### Fixed
- **kmain never resumed after test threads exited**: the kmain TCB had state
  THREAD_RUNNING, but `schedule()` only re-queues THREAD_READY threads. Fix:
  `sched_yield`/`sched_tick` set current→THREAD_READY before calling schedule.

## [Phase 6 — Timer & PIT] 2026-06-21

### Added
- `drivers/timer/pit.{c,h}`: 8254 Programmable Interval Timer driver.
  - Programs channel 0 in mode 3 (square wave) with a divisor derived from the
    1193182 Hz base clock; records the divisor-rounded actual frequency.
  - IRQ 0 handler (registered via the Phase 2 IRQ layer) increments a global
    `volatile` monotonic tick counter.
  - `timer_get_ticks` / `timer_get_frequency` / `timer_sleep_ms`.
  - `timer_sleep_ms` spins with `hlt` (idles the CPU between ticks).
  - Self-test: sleeps 1 second and verifies the tick count is within ±5%.

### Fixed
- `kprintf` `%f` unsupported under `-mno-sse`; timer self-test now reports
  accuracy via integer percentage instead of floating point.

## [Rename + Phase 5 — Kernel Heap] 2026-06-21

### Renamed
- Project renamed **NovOS → AuraLite OS** throughout:
  - display name, `AURALITE_NAME` / `AURALITE_VERSION`
  - all include guards `NOVOS_*` → `AURALITE_*`, macros, GDT selectors
  - project directory `novos/` → `auralite/`, ISO `novos.iso` → `auralite.iso`
  - all docs, Makefile, tooling scripts, Limine entry (`/AuraLite`)

### Added — Phase 5: Kernel Heap
- `kernel/mm/heap.{c,h}`: generic freestanding first-fit allocator with
  boundary-tag (header+footer) coalescing, a doubly-linked free list, splitting,
  and `heap_alloc`/`heap_free`/`heap_realloc`. No kernel deps (only `<stdint.h>`);
  expansion is injected as a callback so the same code is host-unit-tested.
- `kernel/mm/kheap.{c,h}`: kernel wrapper backing the allocator with PMM frames
  mapped on demand by the VMM into a 16 MiB region at `0xFFFFFFFF88000000`.
  `kmalloc`/`kfree`/`krealloc`/`kheap_dump`.
- `tests/unit/test_heap.c`: host tests (basic, alignment, coalescing, realloc,
  10 000-cycle stress, leak check).
- In-kernel self-test: 10 000 alloc/free cycles, no corruption, no leak.

### Changed
- `paging_self_test` no longer deliberately faults at boot (it would halt before
  the heap runs); the #PF demonstration remains documented from Phase 4.
- Heap frames mapped No-Execute.
- CI gate extended to assert the heap PASS line.

## [Phase 4 — Virtual Memory & Paging] 2026-06-21

### Added
- `kernel/arch/x86_64/cpu.h`: consolidated low-level primitives for control
  registers (CR0/2/3/4), MSRs (read/write), and `invlpg` (single-page TLB flush).
- `kernel/arch/x86_64/paging.{c,h}`: 4-level paging VMM.
  - Reads the current PML4 from CR3 (Limine-set); enables NX via EFER.NXE.
  - `walk_pte()`: walks PML4→PDPT→PD→PT, allocating and zeroing missing
    intermediate tables from the PMM, accessed through the HHDM.
  - `paging_map` / `paging_unmap` (with `invlpg`) / `paging_get_phys`.
  - `paging_new_address_space()`: allocates a fresh PML4 and copies the kernel
    half (entries 256–511) for future process creation.
  - In-kernel self-test: map→seed→read→write→verify→unmap→deliberate #PF.
- Consolidated `read_cr2` from `isr.c` into the shared `cpu.h`.

### Changed
- `kmain` now initialises the VMM after the PMM and runs the paging self-test.

## [Phase 3 — Physical Memory Manager] 2026-06-20

### Added
- Limine bridge: `limine_get_memmap()` exposes the full memory-map entry list.
- `kernel/lib/bitmap.h`: header-only, pure-C bitmap with single-bit ops,
  byte-granular `bm_first_free`, and a linear `bm_find_contiguous` run search.
- `kernel/lib/spinlock.{c,h}`: test-and-set (LOCK CMPXCHG) spinlock with a
  `pause`-yielding slow path and an irqsave acquire/restore variant.
- `kernel/mm/pmm.{c,h}`: bitmap physical memory manager.
  - Sizes the bitmap from the highest usable address.
  - Places it in bootloader-reclaimable memory (usable as fallback) and reaches
    it via the Limine HHDM — consuming zero usable RAM.
  - `pmm_alloc_frame` / `pmm_alloc_contiguous` / `pmm_free_frame`, serialised by
    an irqsave spinlock; double-free / bad-address detection.
  - `pmm_dump_stats` + `pmm_get_free_frames` / `pmm_get_usable_frames`.
  - In-kernel self-test: 1000 unique frames, no leak, contiguous alloc.
- `tests/unit/test_pmm.c` host unit test + `make test-unit` Makefile target.

### Changed
- Removed the Phase 2 deliberate divide-by-zero from the boot path (it halts and
  would block later phases); the IDT remains installed and active.
- CI gate now asserts PMM initialisation + self-test PASS (Phase 2 exception
  checks relaxed to structural IDT/PIC assertions).

## [Phase 2 — Interrupts & Exceptions] 2026-06-20

### Added
- 256-entry Interrupt Descriptor Table (`idt.c`/`idt.h`) with LIDT load.
- Macro-generated 256 ISR stubs (`isr_stubs.asm`): separate `ISR_NOERR` and
  `ISR_ERR` macros for the uniform stack frame, plus an `isr_table[]` address
  table that drives IDT population.
- Top-level dispatcher (`isr.c`/`isr.h`): exception classification, full
  GPR + RIP/CS/RFLAGS/RSP/SS register dump, a bounded frame-pointer stack
  trace, and CR2 reporting for page faults.
- 8259A PIC driver (`irq.c`/`irq.h`): remap IRQ 0-15 -> vectors 32-47, per-IRQ
  mask/unmask, End-Of-Interrupt, and a handler dispatch table
  (`irq_register_handler`).
- Divide-by-zero self-test in `kmain` demonstrating the gate criterion.
- `-fno-omit-frame-pointer` for meaningful stack traces.

### Fixed
- Makefile object-path collision: `isr.c` and `isr.asm` both compiled to
  `isr.o` and double-linked. Renamed the stubs to `isr_stubs.asm` and
  documented the base-name-uniqueness constraint for `.c`/`.asm` pairs.

## [Phase 1 — Hello Kernel] 2026-06-20

### Added
- Limine boot-protocol bridge (`kernel/limine_requests.{c,h}`) issuing
  framebuffer, memmap, HHDM, and base-revision requests (v12 marker protocol).
- 64-bit entry point `boot.asm`: own 64 KiB stack, defensive `.bss` zero, C call.
- Flat long-mode GDT (`gdt.c` + `gdt_flush.asm`) with a far-return CS reload.
- 16550 UART driver (COM1, 115200 baud) — the reliable early console.
- Linear framebuffer console (`fb.c`) with a public-domain 8x8 font.
- `kprintf` supporting `%s %d %u %x %X %c %p %%` plus width and zero-padding.
- Freestanding `string.c` (memset/memcpy/memmove/memcmp/strlen).
- `kernel.ld` higher-half linker script, page-separated by permission.
- `Makefile` (Clang/LLD/NASM) and ISO pipeline (`mkisoimage.sh`, `run_qemu.sh`).
- Headless debug tooling: `boot_debug.py`, `analyze_screen.py`, `read_screen.py`.

### Fixed
- Renamed `limine.cfg` → `limine.conf` (Limine v12 only searches `.conf`).
- Resolved Limine panic "PHDRs with different permissions sharing the same
  memory page" by page-aligning all segment boundaries and folding the Limine
  request structs into the writable `.data` segment.
- Corrected the data PHDR flags from `R E` to `R W`.
- `kprintf` now parses width and zero-padding (was printing `%016llx` verbatim).

## [Phase 0 — Bootstrap] 2026-06-20

### Added
- Vendored Limine 12.3.3 (binary release + matching `limine.h`).
- Toolchain bring-up: Clang 19 (`--target=x86_64-elf`), LLD, NASM, QEMU, xorriso.
- Initial bootable ISO that Limine loads into the higher half without faulting.
