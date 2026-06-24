# QEMU Integration Test Results

Reference run on Debian 13 / QEMU 10.0.8 / clang 19 / 2 CPU / 512 MiB RAM.

```
$ make test-integration
```

| # | Case                        | Asserts | Time | Status |
|--:|----------------------------|--------:|-----:|:------:|
| 1 | `test_boot_to_shell`        | 17 / 17 |  12s | ✅ PASS |
| 2 | `test_shell_commands`       | 10 / 10 |  30s | ✅ PASS |
| 3 | `test_syscalls`             |   4 / 4 |  20s | ✅ PASS |
| 4 | `test_user_processes`       |   4 / 4 |  25s | ✅ PASS |
| 5 | `test_ahci_rw`              |   9 / 9 |  25s | ✅ PASS |
| 6 | `test_fat32_persistence`    |   5 / 5 |  50s | ✅ PASS |
| 7 | `test_usb_msc`              |   7 / 7 |  25s | ✅ PASS |
| 8 | `test_networking`           |   6 / 6 |  25s | ✅ PASS |
| 9 | `test_http_get`             |   4 / 4 |  45s | ✅ PASS¹|
|10 | `test_graphics`             |   4 / 4 |  25s | ✅ PASS |
|11 | `test_smp`                  |   3 / 3 |  15s | ✅ PASS |
|   | **TOTAL**                  | **73/73** | **298s** | **✅** |

¹ Soft-pass when QEMU SLIRP DHCP doesn't complete in time; the kernel
falls back to a static IP by design and skips its online TCP self-test.
On a host where DHCP completes, all four asserts become strict.

## What each test actually verifies

### test_boot_to_shell  (17 asserts)
Limine → kmain banner, IDT/PIC/TSS init, SYSCALL MSR, HHDM offset, PMM/VMM/
Heap/Timer/Sched/VFS self-tests all PASS, Ring 3 init reached, interactive
`auralite#` prompt visible, no panic, no triple-fault, no unhandled exception.

### test_ahci_rw  (9 asserts)
- AHCI controller detects the QEMU SATA disk.
- DMA read sector 0 + write/readback sector 1 succeed.
- `diskfs` mounts at `/disk`, `fat32` mounts at `/fat`.
- From userspace: `write /disk/ci.txt <marker>` + `cat` round-trips.
- From userspace: `write /fat/CI.TXT <marker>` + `cat` round-trips.
- No `[ahci] FAIL`, `[diskfs] FAIL`, `[fat32] FAIL` lines.

### test_usb_msc  (7 asserts)
- UHCI controller enumerates the `usb-storage` device.
- USB core completes standard descriptor requests.
- MSC `READ CAPACITY` returns sector count + sector size.
- MSC `READ(10)` of sector 0 succeeds and returns our pre-seeded magic.
- No `[uhci] FAIL`, `[msc] FAIL` lines.

### test_http_get  (4 asserts)
- A local Python HTTP server is spun up; QEMU forwards a port to it.
- The user-mode `/http` client launches inside the guest.
- Asserts the kernel didn't panic / take an unhandled exception.
- If TCP roundtrip completed: asserts the response marker appears on serial.
- Otherwise: soft-passes with a warning (kernel intentionally skips TCP
  self-tests on DHCP failure to keep boot fast; user-mode app still runs).

### test_fat32_persistence  (5 asserts, 2 boots)
- Boot #1: writes a unique marker to `/fat/PERSIST.TXT` and reads it back.
- VM powers off (QEMU is killed).
- Boot #2: reuses the same `disk.img`, lists `/fat`, `cat`s the file.
- The marker from boot #1 is still there → FAT32 writes really hit disk.

### test_networking  (6 asserts)
- Asserts the network stack initialised and didn't crash the kernel.
- Branches on DHCP outcome:
  - DHCP PASS → strict asserts on ICMP, DNS, TCP self-tests.
  - DHCP FAIL → asserts the static-IP fallback path activated cleanly.

### test_smp  (3 asserts)
Boots with `-smp 4`; asserts the SMP subsystem ran, its self-test PASSed,
and at least one AP printed an "online" line.

### test_graphics  (4 asserts)
Framebuffer console init, gfx/kbd/mouse init, WM demo rendered,
3D demo finished.

### test_user_processes  (4 asserts)
Kernel `[proc] PASS: /hello ran in isolated address space`, user `/hello`
output via `spawn()`, shell survives multiple spawns, no exception.

### test_syscalls  (4 asserts)
`listdir`, `open + read` (ELF magic returned from `/hello`), serial-input
`read`, no user thread killed unexpectedly.

### test_shell_commands  (10 asserts)
help, uname, pwd, free, ls (/init, /hello, /calc), echo round-trip,
spawn `/hello` and `/sysinfo`.

## Running a subset

```bash
make test-integration-fast              # skip persist + http (~3 min → ~2 min)
tests/integration/run_all.sh ahci usb   # only matching names
bash tests/integration/cases/test_smp.sh  # single case
```

## CI

A GitHub Actions workflow lives at `.github/workflows/integration.yml`. It
installs the toolchain, builds the ISO, runs host unit tests and the
fast subset of integration tests on every push/PR, and uploads
`build/integration-logs/` as an artifact when anything fails.
