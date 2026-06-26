# AuraLite OS

AuraLite OS is a from-scratch **x86_64 hobby operating system** booted by
Limine. It includes a higher-half kernel, preemptive multitasking, Ring 3 ELF
user programs, a small libc, an initrd-backed VFS, writable tmpfs/FAT32/ext2
storage, e1000 networking, AHCI, USB Mass Storage through UHCI, framebuffer
graphics, a kernel GUI/window compositor, a small user-space GUI toolkit, and
several experimental device/protocol layers.

The project is intentionally incremental and educational: most subsystems have
small self-tests, host-side unit tests, and documentation explaining the design
trade-offs.

---

## Current status

The original 14-phase roadmap is complete, and the repository now contains
additional post-phase extensions.

### Stable / exercised in normal builds

- Limine BIOS/UEFI ISO boot path.
- x86_64 long mode, higher-half kernel.
- GDT, IDT, PIC IRQ dispatch, TSS, SYSCALL/SYSRET.
- Physical memory manager, virtual memory manager, kernel heap.
- Preemptive round-robin scheduler and kernel threads.
- Ring 3 ELF loading and minimal libc.
- Initrd-backed VFS plus `/dev/null`, `/dev/zero`, writable `/tmp`, `/disk`,
  full FAT32 at `/fat`, and ext2 at `/ext2` when a second AHCI disk is present.
- AHCI SATA sector read/write on QEMU-style AHCI disks.
- e1000 networking with ARP, IPv4, ICMP, DHCP/fallback addressing, UDP DNS and
  a minimal single-connection TCP client.
- Framebuffer console, 2D graphics, PS/2 keyboard/mouse, window-manager demo,
  kernel GUI compositor v2.0 (theme engine, desktop icons, notifications, window snapping, start menu, context menus, 100 FPS guaranteed refresh rate), GUI syscalls and bundled GUI applications.
- Host-side unit tests and QEMU integration tests for the main subsystems.

### Experimental / partial

- **Advanced Storage / Filesystems**:
  - `buffer_cache`: Synchronized block cache layer.
  - `ext4`: Experimental ext4-like driver with extent tree parsing (`/ext4`).
  - `btrfs`: Experimental Copy-on-Write B-tree filesystem prototype (`/btrfs`).
  - `f2fs`: Experimental Flash-Friendly File System log-structured prototype (`/f2fs`).
  - `exfat` & `ntfs`: Skeleton/scaffolding drivers (`/exfat`, `/ntfs`).
- Per-process address spaces, `spawn`, `fork`, `execve`, `wait4` are present but
  simplified.
- USB host-controller support is uneven: UHCI has working control/bulk
  transfers and can drive USB Mass Storage; OHCI, EHCI and xHCI currently focus
  on controller/port bring-up and detection.
- AHCI detects/initialises ports and DMA read/write passes the QEMU AHCI test
  disk self-test; broader real-hardware coverage remains experimental.
- USB Mass Storage is ready through UHCI. MSC devices behind OHCI/EHCI/xHCI
  remain future work until those transfer backends are completed.
- Bluetooth HCI and Wi-Fi 802.11 layers are protocol frameworks that require
  working lower-level USB/chipset drivers.
- GUI v2.0 adds a theme engine, desktop icons, notifications, window snapping, start menu, and context menus, but the dirty-rect compositor currently forces full redraws each frame (partial redraw pending integration testing).

See [`docs/status.md`](docs/status.md) for a detailed support matrix.

---

## Quickstart

### Install dependencies

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install clang lld nasm qemu-system-x86 xorriso
# Optional but needed for the full integration suite:
sudo apt install e2fsprogs vncdotool
```

### Build the bootable ISO

```bash
make iso
```

Output:

```text
build/auralite.iso
```

A convenience copy may also be placed at:

```text
auralite.iso
```

### Run in QEMU

```bash
make run
```

Manual equivalent:

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

> Note: `tools/run_qemu.sh` creates/attaches AHCI test disks automatically so
> `/disk`, `/fat` and `/ext2` can be exercised. The simpler manual command above
> boots without those writable/persistent mounts.

### Run in QEMU on Windows 10 (run.bat)

For Windows 10 users running native QEMU without Linux, place `auralite.iso` in a folder and create `run.bat` with the following robust script. It automatically verifies QEMU's installation path, creates necessary AHCI/ext2 virtual disks (`disk.img`, `ext2.img`) if missing, and launches QEMU in a single reliable command line:

```batch
@echo off
echo ========================================================
echo       Checking files and launching AuraLite OS...
echo ========================================================

:: 1. Check QEMU installation
echo [Step 1] Checking for QEMU installation...
SET QEMU_PATH="C:\Program Files\qemu\qemu-system-x86_64.exe"
IF NOT EXIST %QEMU_PATH% GOTO NO_QEMU
echo          - QEMU found at %QEMU_PATH%

:: 2. Check ISO image
echo [Step 2] Checking for auralite.iso...
IF NOT EXIST "auralite.iso" GOTO NO_ISO
echo          - auralite.iso found.

:: 3. Check and create disk.img
echo [Step 3] Checking for disk.img...
IF EXIST "disk.img" GOTO DISK1_OK
echo          - Creating disk.img (16MB)...
fsutil file createnew disk.img 16777216 > nul
:DISK1_OK
echo          - disk.img ready.

:: 4. Check and create ext2.img
echo [Step 4] Checking for ext2.img...
IF EXIST "ext2.img" GOTO DISK2_OK
echo          - Creating ext2.img (8MB)...
fsutil file createnew ext2.img 8388608 > nul
:DISK2_OK
echo          - ext2.img ready.

echo ========================================================
echo [Step 5] All files ready! Starting QEMU...
echo ========================================================

%QEMU_PATH% -cdrom auralite.iso -m 512M -smp 4 -vga std -serial stdio -no-reboot -no-shutdown -cpu qemu64 -netdev user,id=net0 -device e1000,netdev=net0 -device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0,port=1 -device usb-mouse,bus=uhci.0,port=2 -drive file=disk.img,format=raw,if=none,id=ahcidisk -device ahci,id=ahci0 -device ide-hd,drive=ahcidisk,bus=ahci0.0 -drive file=ext2.img,format=raw,if=none,id=ext2disk -device ide-hd,drive=ext2disk,bus=ahci0.1

echo.
echo QEMU finished.
pause
exit /b

:NO_QEMU
echo.
echo [ERROR] QEMU emulator NOT FOUND at: %QEMU_PATH%
echo         Please install QEMU from https://qemu.weilnetz.de/w64/
echo         (Or edit run.bat if QEMU is installed in a different folder).
echo.
pause
exit /b

:NO_ISO
echo.
echo [ERROR] File 'auralite.iso' NOT FOUND in the current folder!
echo         Please put 'auralite.iso' into the same folder as run.bat.
echo.
pause
exit /b
```

### Run tests

```bash
make test-unit              # host-side unit tests
make test-integration-fast  # QEMU smoke/integration subset
make test-integration       # full QEMU integration suite
```

The full suite currently boots QEMU for 14 black-box cases, including AHCI,
FAT32 persistence, ext2 cross-OS round-trips, USB MSC, networking, SMP, graphics
and GUI/VNC checks.

---

## VirtualBox and VMware

AuraLite can boot in desktop hypervisors as long as the virtual hardware matches
currently implemented drivers.

### VirtualBox

```bash
make vbox
```

If `VBoxManage` is installed, this creates/updates a VM named `AuraLite-OS`.
Otherwise it writes manual setup notes to:

```text
vm/virtualbox/README-VirtualBox.txt
```

Recommended NIC: **Intel PRO/1000 MT Desktop (82540EM)**.

### VMware Workstation / Fusion / Player

```bash
make vmware
```

Open:

```text
vm/vmware/AuraLite-OS.vmwarevm/AuraLite-OS.vmx
```

Recommended NIC: **legacy `e1000`**, not `vmxnet3` or `e1000e`.

More details: [`docs/virtual_machines.md`](docs/virtual_machines.md).

---

## Make targets

| Target | Description |
|---|---|
| `make iso` | Build user programs, initrd, kernel and bootable `build/auralite.iso`. |
| `make kernel` | Build `build/kernel.elf` only. |
| `make user` | Build user-space ELF programs. |
| `make run` | Boot the ISO in QEMU with serial output and e1000 networking. |
| `make run-usb-msc` | Boot QEMU with a UHCI USB mass-storage test disk attached. |
| `make debug` | Boot QEMU paused and wait for GDB on port `1234`. |
| `make usb` | Copy the hybrid ISO to `build/usb.img` for USB/HDD-style booting. |
| `make vbox` | Build ISO and create/update VirtualBox configuration. |
| `make vmware` | Build ISO and generate a VMware `.vmx`. |
| `make vm-configs` | Generate both VirtualBox and VMware configs. |
| `make test-unit` | Build and run host-side unit tests. |
| `make test-integration-fast` | Run the faster QEMU integration subset. |
| `make test-integration` | Run the full QEMU black-box integration suite. |
| `make test` | Run unit tests and then full integration tests. |
| `make clean` | Remove `build/`. |

---

## Repository layout

```text
AuraLite-OS/
├── boot/limine/              # Limine boot configs
├── docs/                     # Architecture, ABI, drivers, VM setup, status
├── drivers/
│   ├── ahci/                 # AHCI SATA detection and DMA sector I/O
│   ├── bluetooth/            # Bluetooth HCI protocol layer
│   ├── e1000/                # Intel 8254x/e1000 NIC driver
│   ├── framebuffer/          # Console, 2D graphics, PSF font, WM, 3D demo
│   ├── keyboard/             # PS/2 keyboard
│   ├── mouse/                # PS/2 mouse
│   ├── pci/                  # PCI config-space access
│   ├── timer/                # PIT timer
│   ├── uart/                 # COM1 serial
│   ├── usb/                  # UHCI/OHCI/EHCI/xHCI + USB core + MSC layer
│   └── wifi/                 # 802.11 MAC management layer
├── kernel/
│   ├── arch/x86_64/          # CPU, GDT, IDT, IRQ, paging, syscall, SMP, TSS
│   ├── fs/                   # VFS, initrd, devfs, tmpfs, diskfs, FAT32, ext2
│   ├── gui/                  # Kernel GUI, compositor and GUI syscalls
│   ├── lib/                  # kprintf, string, bitmap, spinlock, assert
│   ├── mm/                   # PMM, heap core, kernel heap wrapper
│   ├── net/                  # Ethernet/ARP/IPv4/ICMP/UDP/DNS/TCP
│   ├── proc/                 # Threads, scheduler, ELF loader, processes
│   └── kernel.c              # kmain() orchestration
├── libauragui/               # User-space GUI toolkit wrappers/widgets
├── libc/                     # Minimal user-space libc and crt0
├── limine/                   # Vendored Limine binaries and header
├── scripts/                  # CI/integration helper
├── tests/unit/               # Host-side unit tests
├── tests/integration/        # QEMU black-box integration tests
├── tools/                    # ISO/initrd/VM/QEMU helper scripts
├── userspace/                # init shell and user programs
├── kernel.ld                 # Kernel linker script
├── Makefile                  # Build system
└── README.md
```

---

## User-space programs

The initrd currently packages:

| Path | Purpose |
|---|---|
| `/init` | Interactive shell. |
| `/hello` | Hello-world test program. |
| `/calc` | Calculator. |
| `/sysinfo` | System information. |
| `/editor` | Simple line editor. |
| `/clock` | Clock/uptime demo. |
| `/guess` | Number guessing game. |
| `/snake` | Terminal snake game. |
| `/http` | HTTP client. |
| `/browser` | Text web browser with simple HTML rendering. |
| `/selftest` | Userspace regression checks for usercopy, FD and socket syscalls. |
| `/gcalc` | Graphical calculator. |
| `/gedit` | Graphical text editor. |
| `/gfiles` | Graphical file manager. |
| `/gterm` | Graphical terminal-style demo. |
| `/gsysmon` | Graphical system monitor demo. |
| `/gabout` | Graphical about dialog. |
| `/glaunch` | GUI application launcher. |
| `/gtheme` | GUI Theme Manager. |

Common shell commands:

```text
help
ls /
cat /hello
echo hello
write /tmp/note hello
cat /tmp/note
run /calc
run /sysinfo
nslookup example.com
ping example.com
gui
exit
```

---

## Documentation map

Start here:

- [`docs/README.md`](docs/README.md) — documentation index.
- [`docs/build_and_run.md`](docs/build_and_run.md) — build/run/troubleshooting.
- [`docs/status.md`](docs/status.md) — current feature and limitation matrix.
- [`docs/architecture.md`](docs/architecture.md) — kernel architecture.
- [`docs/memory_map.md`](docs/memory_map.md) — virtual/physical memory layout.
- [`docs/syscall_abi.md`](docs/syscall_abi.md) — syscall ABI and numbers.
- [`docs/driver_guide.md`](docs/driver_guide.md) — driver inventory and notes.
- [`docs/virtual_machines.md`](docs/virtual_machines.md) — VirtualBox/VMware setup.
- [`docs/virtual_driver_matrix.md`](docs/virtual_driver_matrix.md) — QEMU/VirtualBox/VMware device compatibility matrix.
- [`PLAN.md`](PLAN.md) — historical phase plan.
- [`TODO.md`](TODO.md) — known limitations and future work.
- [`CHANGELOG.md`](CHANGELOG.md) — chronological changes.

---

## Known limitations

Short version:

- AHCI sector read/write is enabled and self-tested on QEMU AHCI disks; broader
  physical-hardware coverage is still experimental.
- Scheduler state is not SMP-safe; APs are brought online and idle rather than
  participating in general scheduling.
- File descriptors are now per-process, but descriptor inheritance/lifetime semantics are still simplified.
- User pointers passed to syscalls now go through basic range/permission validation and copy helpers, but there is not yet a fault-recovering uaccess layer.
- `fork`/`execve`/`wait4` are simplified and not POSIX-complete.
- Dead TCBs and kernel stacks are deferred-reaped, but full user address-space/page-table reaping is not implemented yet.
- Networking is polling-based. User space has process-owned socket-style handles, and the TCP transport supports per-connection state up to 8 streams.
- `/disk` is intentionally tiny: flat namespace, 8 files maximum, 4 KiB per file.
- FAT32 and ext2 are featureful enough for integration tests, but their hardware
  coverage is primarily QEMU/AHCI and they should still be treated as hobby OS
  filesystems rather than production-grade implementations.
- USB MSC currently uses the UHCI backend; OHCI/EHCI/xHCI transfer engines are
  not wired to class drivers yet.

See [`docs/status.md`](docs/status.md) and [`TODO.md`](TODO.md).

---

## License notes

This repository vendors Limine binaries and `limine.h`; see `limine/LICENSE` for
Limine licensing. Font assets and third-party snippets are documented in their
respective source files where applicable.
