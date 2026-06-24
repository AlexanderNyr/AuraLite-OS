# AuraLite OS — QEMU Integration Tests

Black-box tests that boot the real ISO in QEMU and assert on the serial
console output. Each case is a self-contained Bash script; the runner
orchestrates them and prints a colored summary.

```
tests/integration/
├── README.md                 ← this file
├── run_all.sh                ← top-level orchestrator
├── lib/lib.sh                ← shared helpers (qemu launcher, asserts, colors)
└── cases/
    ├── test_boot_to_shell.sh        Phase 0 → 11 reach an interactive prompt
    ├── test_shell_commands.sh       help/ls/cat/echo/pwd/free/run …
    ├── test_syscalls.sh             read/write/open/listdir/getpid
    ├── test_user_processes.sh       spawn(), isolated address spaces
    ├── test_ahci_rw.sh              AHCI DMA + /disk + /fat write/read
    ├── test_fat32_persistence.sh    write file → reboot → still there
    ├── test_usb_msc.sh              UHCI + USB MSC READ(10) sector 0
    ├── test_networking.sh           e1000 + ARP + ICMP + DNS + TCP
    ├── test_http_get.sh             real HTTP/1.0 GET to a python httpd
    ├── test_graphics.sh             framebuffer + WM + 3D demo render
    └── test_smp.sh                  Limine MP brings up ≥ 1 AP
```

## Running

```bash
make test-integration              # all cases
make test-integration-fast         # skip the slow ones (FAT32 persist, HTTP)
tests/integration/run_all.sh ahci  # only cases matching 'ahci'
NO_COLOR=1 tests/integration/run_all.sh   # plain text
```

Per-case run:

```bash
bash tests/integration/cases/test_ahci_rw.sh
```

## Requirements

| Tool                  | Used for                                     |
|-----------------------|----------------------------------------------|
| `qemu-system-x86_64`  | Booting the ISO                              |
| `clang`, `ld.lld`     | Building the kernel (Makefile)               |
| `nasm`                | Building assembly stubs                      |
| `xorriso`             | Creating the ISO                             |
| `python3`             | Disk-image bootstrap + HTTP test server      |

Install on Debian/Ubuntu:

```bash
sudo apt install clang lld nasm xorriso qemu-system-x86 python3
```

## How a case is structured

Every case follows the same pattern:

1. `source lib/lib.sh; il_init` — sets `IL_ROOT`, `IL_ISO`, `IL_LOGDIR`.
2. `il_send …` queues shell commands to be typed once QEMU starts.
3. `il_run_qemu <log> <timeout> [extra qemu args…]` launches the OS,
   pipes the queued input into the serial console, and captures all output.
4. `il_assert_grep / il_assert_no_grep / il_assert_count` evaluate the log.
5. `il_summary` prints a per-case ✓/✗ tally and returns 0/1.

The runner sums these into the overall pass/fail count.

## Log files

Each run writes its own log under:

```
build/integration-logs/<case>.log
```

When an assertion fails, the runner prints the failed assertion plus the
relevant line of the log. The full log is preserved for post-mortem.

## Caveats / known limitations

- AuraLite uses **polling-based serial input** in the shell, so we send
  characters with a ~200 ms gap between each line (`_il_feed_queue`).
  Faster typing causes dropped characters.
- QEMU SLIRP DHCP isn't reliable for AuraLite's current DHCP client; the
  networking case asserts ICMP echo to `10.0.2.2` (always available) and
  TCP handshake to `10.0.2.3:53` (SLIRP DNS proxy).
- `test_fat32_persistence` reuses a single disk image across two boots —
  the first boot **writes** it, the second **reads** it. It removes the
  image at the start to ensure a clean run.
- `test_http_get` spins up a temporary Python HTTP server on a free
  loopback port; if your CI box already has port 80 forwarded, override
  `HTTP_PORT=…`.
- The kernel's USB self-test calls `READ(10)` on sector 0 of the attached
  `usb-storage`. We pre-seed sector 0 with the magic `AURALUSB\x55\xAA`
  so the test is hermetic (no reliance on whatever was on the image).

## Adding a new case

```bash
cp tests/integration/cases/test_boot_to_shell.sh \
   tests/integration/cases/test_my_thing.sh
$EDITOR tests/integration/cases/test_my_thing.sh
# add 'test_my_thing' to ALL_CASES in run_all.sh
```

The case scripts deliberately have no shared mutable state — each one
boots a fresh QEMU instance, so they can run in any order or in parallel
(if you ever wire that up).
