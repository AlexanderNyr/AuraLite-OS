# AuraLite OS — QEMU Integration Tests

Black-box tests that boot the real ISO in QEMU and assert on the serial
console output. Each case is a self-contained Bash script; the runner
orchestrates them and prints a colored summary.

```
tests/integration/
├── README.md                 ← this file
├── run_all.sh                ← top-level orchestrator (+ ALL_CASES registry)
├── lib/lib.sh                ← shared helpers (qemu launcher, asserts, colors)
└── cases/                    ← 162 case scripts, one per behaviour
```

The 162 cases are partitioned into **10 thematic CI shards** so they run in
parallel instead of one ~2 h job. The partition lives in `run_all.sh`
(`GROUP_NAMES` / `group_re()`) and is **self-checked on every invocation**:
each registered case must match exactly one shard regex — a case that matches
none (or two) refuses to run rather than silently dropping out of CI.

| Shard | Representative cases |
|---|---|
| `core` | `test_boot_to_shell`, `test_syscalls`, `test_execve_args`, `test_fork_cow`, `test_elf_permissions`, `test_smp`, `test_mmap_shared`, `test_panic_diag` |
| `posix` | `test_posix_p10`, `test_posix2024_conf`, `test_signals`, `test_termios`, `test_jobcontrol`, `test_apm_packages`, `test_sdk_examples` |
| `fs` | `test_ahci_rw`, `test_fat32_full`, `test_ext2`, `test_fs_stress`, `test_tmpfs`, `test_procfs`, `test_devfs` |
| `fsfull` | `test_ext4`, `test_f2fs`, `test_btrfs`, `test_exfat`, `test_ntfs` (FSFULL F2–F7, multi-boot harnesses) |
| `usb` | `test_usb_msc`, `test_usb_hotplug`, `test_xhci_bulk`, `test_usbfs_fat32` |
| `net` | `test_networking`, `test_dns_cache`, `test_ip_frag`, `test_tcp_x5`, `test_tcp6`, `test_https6`, `test_x25519mlkem`, `test_tls`, `test_gbrowser_net` |
| `gui` | `test_gui`, `test_opengl`, `test_3d_render`, `test_virgl_gpu`, `test_gbrowser`, `test_doom`, `test_w32_*` |
| `selfhost-script` | `test_selfhost_script`, `test_selfhost_pipe`, `test_selfhost_shmake`, `test_selfhost_build` (SH6) |
| `selfhost-closure` | `test_selfhost_tcc`, `test_selfhost_kernel_guest`, `test_selfhost_closure` (SH8 — the only shard needing the guest `/bin/tcc`) |
| `selfhost-img` | `test_selfhost_mkinitrd`, `test_selfhost_mkiso`, `test_selfhost_iso` (SH7 image twins) |

## Running

```bash
make test-integration              # all cases
make test-integration-fast         # skip slow cases (see SLOW_CASES_RE below)
tests/integration/run_all.sh ahci  # only cases matching 'ahci'
tests/integration/run_all.sh --group net         # one CI shard
tests/integration/run_all.sh --check-groups      # verify the shard partition
NO_COLOR=1 tests/integration/run_all.sh          # plain text
```

The fast subset skips everything matching `SLOW_CASES_RE` in `run_all.sh`:
FAT32 persistence, `http_get`, `ext2`, `fs_stress`, `doom`,
`ahci_large_read`, the selfhost closure pair (`kernel_guest`, `closure`)
and the whole `fsfull` shard (`ext4`, `f2fs`, `btrfs`, `exfat`, `ntfs`).

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
| `mtools`              | FAT helpers: `make deps-check`, `make iso-bios`, and the DOOM case's `fatpart` |
| `python3`             | Disk-image bootstrap + HTTP test server      |
| `e2fsprogs`           | Optional/full: ext2 `mkfs.ext2` + `debugfs`  |
| `vncdotool`           | Optional/full: GUI VNC screenshot assertions |

The ISO itself needs **no external tool**: the default `make iso` (dual
BIOS+UEFI) is built by the in-tree C tool `tools/selfhost/mkiso.c`, which
also writes the FAT ESP image directly (no `mformat`/`mcopy`, no `xorriso` —
`xorriso` is not invoked by any current Makefile target). The legacy
`mkisoimage_bios.sh` / `mkisoimage_dual.sh` scripts still use mtools, and the
guest-toolchain arc (`selfhost-closure` shard) builds TinyCC in the guest via
`make selfhost-deps`.

Install on Debian/Ubuntu:

```bash
sudo apt install clang lld nasm mtools qemu-system-x86 python3
sudo apt install e2fsprogs vncdotool   # optional, for full ext2/GUI coverage
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
- The DHCP client works against QEMU SLIRP (FIXES R9: lease acquired,
  10.0.2.15/24 with gw 10.0.2.2). The networking cases nevertheless assert
  on the always-available SLIRP endpoints — ICMP echo to `10.0.2.2` and
  DNS/TCP toward `10.0.2.3` (SLIRP DNS proxy) — so they never depend on a
  real lease or outside connectivity.
- `test_fat32_persistence` reuses a single disk image across two boots —
  the first boot **writes** it, the second **reads** it. It removes the
  image at the start to ensure a clean run.
- `test_ext2` needs `mkfs.ext2` and `debugfs`; if they are missing, it soft-skips
  after reporting the missing dependency.
- `test_http_get` spins up a temporary Python HTTP server on a free
  loopback port; if your CI box already has port 80 forwarded, override
  `HTTP_PORT=…`. The test soft-passes when DHCP falls back and the HTTP body is
  not observable, as long as the path runs without kernel/user exceptions.
- `test_gui` uses QEMU VNC plus `vncdotool` screenshots when available. Without
  `vncdotool`, it keeps serial-level GUI assertions and soft-skips visual ones.
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

Registration is **enforced**: `tools/check_test_registry.py --check` fails if
any file under `cases/` is missing from `ALL_CASES` (or vice versa) — that is
the AUDIT_A0 disease (cases on disk that CI never ran) kept dead. The new
case must also match exactly one shard regex in `group_re()`; run
`run_all.sh --check-groups` to confirm.

The case scripts deliberately have no shared mutable state — each one
boots a fresh QEMU instance, so they can run in any order or in parallel
(if you ever wire that up).
