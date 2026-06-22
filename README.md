# AuraLite OS

AuraLite OS is a from-scratch **x86_64 hobby operating system** booted by
Limine. It includes a higher-half kernel, preemptive multitasking, Ring 3 user
programs, a small libc, an initrd-backed VFS, networking, framebuffer graphics,
a window-manager demo, and several experimental device/protocol layers.

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
- Initrd + VFS + `/dev/null` and `/dev/zero`.
- e1000 networking with ARP, IPv4, ICMP, DHCP, UDP DNS and minimal TCP client.
- Framebuffer console, 2D graphics, PS/2 keyboard/mouse, window-manager demo.
- Host-side unit tests for core algorithms and protocol helpers.

### Experimental / partial

- Per-process address spaces, `spawn`, `fork`, `execve`, `wait4` are present but
  simplified.
- USB host-controller support is uneven: UHCI has working control/bulk
  transfers and can drive USB Mass Storage; OHCI, EHCI and xHCI currently focus
  on controller/port bring-up and detection.
- USB Mass Storage is ready through the UHCI backend. MSC behind OHCI/EHCI/xHCI
  remains future work.
- AHCI detects and initialises ports, but sector I/O is disabled because PxCI
  command issue currently faults under the known test setup.
- Bluetooth HCI and Wi-Fi 802.11 layers are protocol frameworks that require
  working lower-level USB/chipset drivers.

See [`docs/status.md`](docs/status.md) for a detailed support matrix.

---

## Quickstart

### Install dependencies

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install clang lld nasm qemu-system-x86 xorriso
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

> Note: `tools/run_qemu.sh` also attaches an AHCI test disk. If you use it
> directly and `build/disk.img` does not exist, create one first or use the
> simpler command above.

### Run unit tests

```bash
make test-unit
```

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
| `make clean` | Remove `build/`. |

---

## Repository layout

```text
AuraLite-OS/
├── boot/limine/              # Limine boot configs
├── docs/                     # Architecture, ABI, drivers, VM setup, status
├── drivers/
│   ├── ahci/                 # AHCI SATA controller/port bring-up, I/O WIP
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
│   ├── fs/                   # VFS, initrd, devfs
│   ├── lib/                  # kprintf, string, bitmap, spinlock, assert
│   ├── mm/                   # PMM, heap core, kernel heap wrapper
│   ├── net/                  # Ethernet/ARP/IPv4/ICMP/UDP/DNS/TCP
│   ├── proc/                 # Threads, scheduler, ELF loader, processes
│   └── kernel.c              # kmain() orchestration
├── libc/                     # Minimal user-space libc and crt0
├── limine/                   # Vendored Limine binaries and header
├── scripts/                  # CI/integration helper
├── tests/unit/               # Host-side unit tests
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

Common shell commands:

```text
help
ls /
cat /hello
echo hello
run /calc
run /sysinfo
nslookup example.com
ping example.com
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
- [`PLAN.md`](PLAN.md) — historical phase plan.
- [`TODO.md`](TODO.md) — known limitations and future work.
- [`CHANGELOG.md`](CHANGELOG.md) — chronological changes.

---

## Known limitations

Short version:

- AHCI sector read/write is not enabled.
- USB Mass Storage transport is incomplete.
- Scheduler is not SMP-safe; APs are brought online but not used for general
  scheduling.
- File descriptors are global, not per-process.
- User pointers passed to syscalls are not yet validated.
- `fork`/`execve`/`wait4` are simplified and not POSIX-complete.
- Networking is polling-based and TCP supports one client connection at a time.
- There is no persistent writable filesystem yet.

See [`docs/status.md`](docs/status.md) and [`TODO.md`](TODO.md).

---

## License notes

This repository vendors Limine binaries and `limine.h`; see `limine/LICENSE` for
Limine licensing. Font assets and third-party snippets are documented in their
respective source files where applicable.
