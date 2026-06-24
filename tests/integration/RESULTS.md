# QEMU Integration Test Results

Reference run on Debian 13 / QEMU 10.0.8 / clang 19 / 2 CPU / 512 MiB RAM.
Full ext2 and GUI visual assertions require `e2fsprogs` and `vncdotool`.

```bash
make test-unit && make test-integration
```

## Latest full run

| # | Case                        | Asserts | Time | Status |
|--:|-----------------------------|--------:|-----:|:------:|
| 1 | `test_boot_to_shell`        | 17 / 17 |  12s | ✅ PASS |
| 2 | `test_shell_commands`       | 10 / 10 |  30s | ✅ PASS |
| 3 | `test_syscalls`             |   4 / 4 |  20s | ✅ PASS |
| 4 | `test_selftest`             | 13 / 13 |  35s | ✅ PASS |
| 5 | `test_user_processes`       |   4 / 4 |  25s | ✅ PASS |
| 6 | `test_ahci_rw`              |   9 / 9 |  25s | ✅ PASS |
| 7 | `test_fat32_persistence`    |   5 / 5 |  50s | ✅ PASS |
| 8 | `test_fat32_full`           | 12 / 12 |  35s | ✅ PASS |
| 9 | `test_ext2`                 | 14 / 14 |  66s | ✅ PASS |
|10 | `test_fs_stress`            | 13 / 13 |  80s | ✅ PASS |
|11 | `test_usb_msc`              |   7 / 7 |  25s | ✅ PASS |
|12 | `test_networking`           |   6 / 6 |  25s | ✅ PASS |
|13 | `test_http_get`             |   4 / 4 |  45s | ✅ PASS¹ |
|14 | `test_graphics`             |   4 / 4 |  25s | ✅ PASS |
|15 | `test_smp`                  |   3 / 3 |  15s | ✅ PASS |
|16 | `test_gui`                  |   9 / 9 |  17s | ✅ PASS² |
|   | **TOTAL**                   | **134/134** | **530s** | **✅** |

Unit tests in the same run also passed: PMM, heap, string, bitmap, net helpers,
kprintf, libc, 3D math, USB protocol structures and WM helpers.

¹ Soft-pass when QEMU SLIRP DHCP does not complete in time; the kernel falls
back to a static IP by design and skips online TCP self-tests. On a host where
DHCP completes, the HTTP body/marker path is asserted strictly.

² With `vncdotool` installed, the GUI case captures VNC screenshots and checks
that the framebuffer is non-black. Without it, visual assertions are soft-skipped
and serial-level GUI checks still run.

## What each test verifies

### `test_boot_to_shell` — 17 asserts

Limine → `kmain` banner, IDT/PIC/TSS init, SYSCALL MSRs, HHDM offset,
PMM/VMM/heap/timer/scheduler/VFS self-tests, Ring 3 init shell, visible
`auralite#` prompt, no panic/triple-fault/unhandled exception.

### `test_shell_commands` — 10 asserts

Shell surface: `help`, `uname`, `pwd`, `free`, `ls` output for key initrd apps,
`echo` round-trip, and spawning `/hello` plus `/sysinfo`.

### `test_syscalls` — 4 asserts

`listdir`, `open + read` returning ELF magic from `/hello`, serial-input
`read`, and no unexpected user-thread kill.

### `test_selftest` — 13 asserts

Runs `/selftest`, a bundled userspace regression program.  It verifies that
invalid user pointers are rejected by `write`, `open` and `stat`, that valid
`stat/open/read/write` paths still work, and that the new socket-style API can
create and close process-owned socket handles without kernel faults.

### `test_user_processes` — 4 asserts

Kernel process self-test, `/hello` via `spawn()`, shell remains alive after
multiple spawns, and no exception in the process path.

### `test_ahci_rw` — 9 asserts

AHCI detects a QEMU SATA disk, DMA read/write self-test passes, `/disk` and
`/fat` mount, userspace write/read round-trips through both filesystems, and no
AHCI/diskfs/FAT32 failure lines appear.

### `test_fat32_persistence` — 5 asserts, 2 boots

Boot #1 writes a unique marker to `/fat/PERSIST.TXT`; boot #2 reuses the same
image and reads the marker back, proving persistence across VM power-off.

### `test_fat32_full` — 12 asserts

FAT32 subdirectories, VFAT long-name visibility, `mkdir`, nested `write/cat`,
`stat`, `mv`, `rm`, `rmdir`, and absence of exceptions during those operations.

### `test_ext2` — 14 asserts

Two-pass ext2 coverage:

1. Mount a Linux-`mkfs.ext2` image, read a Linux-created file, write AuraLite
   files/dirs, and verify them on the host through `debugfs`.
2. Boot with a blank disk, let AuraLite run its in-kernel `mkfs.ext2`, run the
   kernel self-test, and verify Linux recognises the resulting filesystem.

### `test_fs_stress` — 13 asserts, 2 boots

A heavier FAT32/ext2 churn regression: nested directories, write/read, rename,
unlink, stat, reboot persistence and optional host `debugfs` inspection for ext2.

### `test_usb_msc` — 7 asserts

UHCI enumeration, USB core descriptor flow, MSC `READ CAPACITY`, ready state,
`READ(10)` sector 0, and no UHCI/MSC failure lines.

### `test_networking` — 6 asserts

Network stack initialises cleanly. The test branches on DHCP:

- DHCP success: asserts ICMP/DNS/TCP self-tests.
- DHCP fallback: asserts static-IP fallback activated cleanly.

### `test_http_get` — 4 asserts

Starts a local Python HTTP server, launches user-mode `/http`, checks that the
kernel/user path does not panic or fault, and strictly checks the body marker
when DHCP/TCP completes. DHCP fallback is a documented soft pass.

### `test_graphics` — 4 asserts

Framebuffer console init, graphics/keyboard/mouse init, framebuffer WM demo
rendered, and 3D demo completed.

### `test_smp` — 3 asserts

Boots with SMP enabled and checks that the SMP subsystem ran, self-test passed,
and AP online messages appeared.

### `test_gui` — 9 asserts

Kernel GUI self-test, framebuffer/input initialisation, no panic/exception, and
when VNC tooling is available, screenshot capture plus non-black framebuffer
checks before/after launching a GUI app.

## Running a subset

```bash
make test-integration-fast                 # skip slow cases
FILTER=fs_stress tests/integration/run_all.sh
bash tests/integration/cases/test_selftest.sh
```

## CI

A GitHub Actions workflow lives at `.github/workflows/integration.yml`. It
installs the toolchain, builds the ISO, runs host unit tests and the fast subset
of integration tests on every push/PR, and uploads `build/integration-logs/` as
an artifact when anything fails.
