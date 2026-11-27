# M6m — explicit backend FLUSH and ordered checkpoint barriers

Date: 2026-11-24 (Europe/Moscow). GitHub HEAD rechecked unchanged at
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Incremental local patch on M6l;
no commit/push. M6 remains IN PROGRESS; no benchmark or EXACT durability claim.

## Backend seam

Append an optional synchronous `flush(ctx)` callback to `struct blkdev_ops`.
`blkdev_flush(dev)` is a backend/device write-cache barrier, NOT filesystem
buffer-cache writeback. Invalid device returns -EINVAL, absent callback or
explicit unsupported capability returns -ENOTSUP. Generic -1 and positive
callback failure become -EIO; typed negative errors are preserved. No read/
write or generic-cache-sync substitute silently turns unsupported into success.
Flush requests do not fabricate sector read/write statistics.

The struct change is source-compatible with designated initializers and
zero-initialized trailing fields, but requires producers AND consumers to be
rebuilt. Makefile explicitly adds the seam-header dependency to all four
kernel object lanes, plus the new AHCI policy-header dependency. This tree's
normal C rules do not generate compiler depfiles; an incremental old producer
object must not expose a shorter ops table to a new consumer. Other backends
(NVMe, virtio-mmio, i386 PIO) are NOT implemented as supporting FLUSH here;
their absent callback retains an explicitly weak fallback.

## AHCI implementation

ATA IDENTIFY word 83 is accepted only when validity bits 15:14 equal 01.
Advertised bit 13 selects FLUSH CACHE EXT (0xEA), otherwise bit 12 selects
FLUSH CACHE (0xE7), otherwise unsupported. EXT is preferred. The pure selection
helper used by the actual driver is compiled in the host seam tests.

`ahci_flush()` issues a non-data command through the existing command engine
under the same per-port IRQ-save lock as read/write. Sector count, LBA,
physical buffer and transfer length are zero; PRDTL is zero and no N-1 byte
count is computed for an empty transfer. Data commands retain their existing
one-PRDT checks. Existing busy wait, completion polling, timeout and task-file
error handling remain in force. Invalid ports fail; unsupported IDENTIFY
returns -ENOTSUP without issuing a pretend command. No extra DMA allocation.

Guest AHCI receipt proves normal advertised commands complete on QEMU; host
policy tests are not MMIO/interrupt/timeout simulation. Hardware timeout/error
recovery remains the existing driver's mechanism, not newly proven here.

## AuraFS checkpoint sequence

Under the existing serialized driver contract:

1. Flush appended data/metadata from filesystem cache via the checked range.
2. Request backend FLUSH. Supported failure aborts BEFORE new super submission.
   -ENOTSUP is explicitly tracked as the existing weak path.
3. Remember the root/end candidate BEFORE queuing its sealed superblock,
   then write back that superblock cache range.
4. Request backend FLUSH again. Any failure or capability inconsistency
   relative to the first barrier aborts before readback/generation advance.
5. Direct backend readback must match all submitted superblock bytes (M6l).
6. Advance generation and emit success only after all applicable stages pass.

For supported devices the receipt is:

```text
[aurafs] checkpoint: backend readback verified; barriers=device-FLUSH durability=WEAK, history retained
```

The unsupported fallback says `barriers=unsupported`, never device-FLUSH.
The QEMU btrfs gate specifically requires the supported AHCI receipt. Existing
mount receipt stays `reclaim=none durability=WEAK`. Candidate history is NEVER
retired by this slice, including after successful barriers and readback.

Adds two backend barrier requests per sync reaching both stages, in addition
to M6l's 4 KiB readback. Can add substantial latency; this is not a filesystem
speed optimization. No benchmark or comparison against historical A/B runs.

## Actual-code host tests

- Seam: invalid/absent callback, context routing, success, generic/positive/
typed/unsupported errors and unchanged sector statistics.
- Actual AHCI selector/classifier: invalid all-zero/all-one/invalid-validity
words, no advertised support, FLUSH-only, EXT-only/both, EXT preference and
non-data opcode classification. This does not execute AHCI MMIO on the host.
- Actual AuraFS sync: mock advertised barriers observe no super write at the
first barrier, one super write at the second, and backend readback only after
both. First-barrier failure prevents super submission; second-barrier failure
and supported-to-unsupported capability change prevent readback and generation
advance. Successful retry preserves/deduplicates checkpoint candidates.
- Explicit unsupported callbacks allow the existing weak sync path. Seam
absence independently returns unsupported rather than success.
- Existing dropped/partial/different-valid-SHA write and backend read-error
checks still pass, together with snapshot/ENOSPC/history/vnode tests.
- Negative control compiles the same tests with both barrier invocations
removed from a temporary driver copy; required two-barrier assertion fails.
Delivered driver source is not replaced by the control.

RAM callbacks verify call ordering/error propagation, NOT physical power-loss
behavior. ASan/UBSan uses detect_leaks=0 for existing retained cache/driver
allocations. Final full unit/M5/ISO/QEMU outcomes are recorded in CHANGELOG.
Optional missing TCC/multilib/cross-ISA execution is not inferred coverage.

## Still not a reclaim/retirement protocol

ATA command success depends on the device/backend honoring its advertised
contract. QEMU completion plus clean power-off/remount tests is not a real
SSD power-cut test. Single in-place superblock can still tear; there is no
dual-super selector, journal or recovery protocol. Readback verifies only the
super image, not every silently corrupted appended block. Unsupported backends
remain weak. Caller serialization does not prove concurrent syscall lifetimes.

Do not retire earlier candidate roots or reuse their blocks based only on
this new receipt. The next prerequisite is robust superblock publication /
recovery and an explicit rule for when roots cease being recovery candidates,
with fault-injected torn-write/remount evidence. On-disk format, allocator,
append-only ENOSPC, snapshot persistence and orphan allocation policy do not
change in M6m. Reclaim stays none and M6 stays open.

## Reproduce

```bash
export PATH="$HOME/.cargo/bin:/usr/sbin:$PATH"
make build/test_blkdev build/test_aurafs_m6
build/test_blkdev && build/test_aurafs_m6
cc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -I . tests/unit/test_aurafs_m6.c \
  kernel/fs/buffer_cache.c kernel/fs/blkdev.c kernel/lib/sha256.c \
  -Wl,--gc-sections -o build/test_aurafs_m6_sanitize
ASAN_OPTIONS=detect_leaks=0 build/test_aurafs_m6_sanitize
make iso -j6
bash tests/btrfs/test_btrfs.sh
bash tests/integration/cases/test_aurafs_fd.sh
bash tests/integration/cases/test_data_volume.sh
make test-unit -j4
bash tests/btrfs/test_m5_host.sh
python3 tools/check_test_registry.py
python3 tools/check_width_sweep.py --check
git diff --check
```

Workspace was reconstructed at `/home/user/AuraLite-current` from unchanged
upstream plus checked M6a–l patches after the prior snapshot lost .git/source
files. Tools restored, Rust 1.98 with three bare-metal targets; no lint bypass
or changes to old delivered patches.
