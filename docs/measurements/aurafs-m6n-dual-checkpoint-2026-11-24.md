# M6n — explicit opt-in dual checkpoint format

Date: 2026-11-24 (Europe/Moscow). User selected **opt-in for new volumes,
no automatic legacy migration**. GitHub HEAD remains
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Incremental local patch on M6m,
no commit/push. M6 remains IN PROGRESS; reclaim is NOT enabled.

## Opt-in and compatibility

On the supported x86_64 QEMU boot path, creating v2 requires BOTH:

```text
-fw_cfg name=opt/auralite.fsformat,string=1
-fw_cfg name=opt/auralite.aurafs_dual,string=1
```

The new knob defaults OFF and only exact strings 0/1 are recognised. It is
independent of the generic format gate. It does not request migration.
A valid existing legacy volume remains legacy even with both knobs on;
the host test checks no device writes occur during that mount. Without the
new knob, fresh legacy formatting remains the default. No in-place converter
or generic hardware provisioning utility is supplied by this slice.

V2 creation first requires a completed backend barrier, aligned sufficient
capacity, and a direct read scan proving EVERY byte of the raw device is zero.
Any nonzero byte or read failure refuses creation before filesystem writes.
This can be expensive on large devices; it is a provisioning safety check,
not a mount-time scan of existing volumes. The scan does request a backend
FLUSH first, which can persist previously queued device-cache writes; "no
writes" means no new filesystem overwrite requests, not zero device effects.

Existing v2 volumes are autodetected without either creation permission.
Recovery boots should keep the generic format permission OFF. Recognisable
corrupt/ambiguous checkpoints are never reformatted, even with permission on.
As with the legacy generic format facility, destroying every recognisable
signature can make a volume appear foreign; never use format permission as a
recovery option. The explicit v2 creation lane additionally requires a fully
zero device, preventing that lane from overwriting hidden nonzero content.

**Older kernels do NOT understand v2.** In particular, do not boot an old
kernel with auto-format permission against a v2 disk: it can treat the new
magic as foreign. This is an opt-in incompatible custom AuraFS format, not
Linux Btrfs interoperability. Back up data; no production migration claim.

## Layout and selection

- Slot A: byte offset 65536. Slot B: 61440, in the previously reserved prefix.
- Each is a full 4096-byte SHA-256-sealed superblock with distinct eight-byte
  magic `_AURA2FS`, self-address, generation, root, cursor and next-inode fields.
- Data/tree layout and first append block at 69632 are unchanged. Legacy
  `_BHRfS_M` remains single-slot. No partition parsing is added; these are raw
  filesystem devices, and the zero-device check rejects partition tables.
- Validate each candidate independently: SHA, type/magic, physical-slot self,
  geometry, root/cursor bounds, inode bounds, nonzero/non-wrapping 64-bit
  generation and matching low header generation.
- Choose the higher valid generation. Equal-generation v2 candidates must
  have identical bodies (excluding slot-specific header/self and SHA), or
  mount fails as ambiguous. Inconsistent total geometry fails closed.
- Valid legacy A plus valid v2 B is rejected as mixed format, not migrated.
  A copied image in the wrong slot is invalid because its self-address differs.
- A valid surviving v2 slot permits mount when the other is corrupt/unreadable.
  With no valid recognised candidate, corruption/ambiguity is refused. An
  unreadable unknown device is not formatted as if it were blank.
- Selection is read-only: no mount-time repair, checkpoint rewrite or migration.
  Both valid v2 roots seed the existing conservative history; the diagnostic
  reads the active checkpoint and includes that history. No root retirement.

Selection validates the checkpoint itself, NOT every reachable tree/data
block before choosing it. A SHA-valid super pointing at damaged tree/data
can still produce later read errors; this is not a complete fsck or a full
metadata-corruption fallback policy. There is no authentication against
out-of-band re-sealed checkpoint injection or mixed-device transplantation.

## Publication

Initial v2 format flushes/barriers the complete initial metadata before
publishing slot A, verifies A through direct backend readback, then publishes,
barriers and reads back slot B. Both initially have generation 1. Interrupted
initial format can leave one valid copy and return failure; a subsequent
mount can recover that surviving copy. No active mount is published on format
failure. The initial-format interruption matrix is not exhaustively fault-tested.

Normal v2 sync writes ONLY the inactive slot:

1. Flush appended blocks, require a successful backend FLUSH (unsupported is
   failure for v2, not legacy's explicit weak fallback).
2. Build the new checkpoint from the active image, remember its root/end,
   write the inactive slot with the next generation and that slot's self.
3. Write back, require the second FLUSH and exact direct backend readback.
4. Only then switch the in-memory active slot and advance generation.

A failed/incomplete target write leaves the prior active slot untouched by
that sync. There is no in-place rewrite of the sole current checkpoint.
A complete new image may nevertheless exist after a returned error (e.g.
post-write barrier failure): remount may select NEW. **Failure is ambiguous,
not rollback.** Host tests explicitly demonstrate this. Sequence exhaustion
refuses sync rather than wrapping v2 generation. No automatic retirement or
block reuse is inferred from a successful publication.

## Evidence

Actual-driver host tests cover:

- Explicit creation, barrier requirement, nonzero byte at the LAST device byte
  refusing format, initial two valid copies, and alternating active slots.
- Accepted target writes cut at 0, 512, 2048 and 3584 bytes; sync fails readback,
  filesystem cache is dropped, remount serves OLD and resumes its old cursor.
- Successful NEW checkpoint/remount; newest SHA corruption falls back to OLD;
  damaged older copy still permits NEW; both damaged refuse mount without writes
  even with format permission; wrong-slot copy, equal-generation conflict and
  wrapped generation handling; no mount-time repair.
- Existing v2 with unsupported barriers refuses sync; ambiguous failed second
  barrier can remount a complete new AMB image. Both old/new recovery roots
  remain visible to the reachability audit; no reclamation.
- Existing legacy volume with v2 opt-in remains legacy with unchanged write
  count. Separate actual fsformat API tests pin OFF default/explicit toggles.
- Negative selector control that always prefers valid primary A fails the
  required latest-mirror remount assertion. Control is a temporary driver copy,
  not delivered runtime source.

New registered fs-group `test_aurafs_dual.sh`: **12/12 PASS** on QEMU 10.0.13
TCG/qemu64, 4 vCPUs, 512 MiB, three fresh 64 MiB raw AHCI disks. Boot 1 opts
in, writes OLD and syncs, writes NEW and syncs. After QEMU exits, Python checks
both stored SHA/self/generation receipts and zeros the FIRST SECTOR of the
newest slot on the disposable image. Boot 2 has both creation knobs OFF:
v2 autodetection selects the older intact slot, serves OLD not NEW without
reformat, then passes the actual userspace open/dup/fork/close/pressure/fsync
program on the recovered v2 filesystem. No test/kernel failures; shell lives.

This is a deterministic torn-MEDIA-IMAGE fixture and real fresh QEMU boot,
NOT a live hardware power cut, SSD validation or device-cache fault emulator.
Assumes the non-target checkpoint remains intact and the backend honours
successful barriers; multiple faults can still destroy both copies. Existing
legacy QEMU gates PASS: btrfs 21/21, userspace FD 12/12, persistent data 9/9.
Full unit, M5 host and ISO build PASS. Registry: 249 cases; width sweep
448/449 casts, 69/69 x64 includes, cross-arch 0, 27/27 asm PASS. ASan/UBSan leak detection is off for retained cache allocations;
optional TCC/multilib/cross-ISA execution skips remain explicit.

## Reproduce

```bash
export PATH="$HOME/.cargo/bin:/usr/sbin:$PATH"
make build/test_aurafs_m6 build/test_fsformat
build/test_aurafs_m6 && build/test_fsformat
make iso -j6
bash tests/integration/cases/test_aurafs_dual.sh
make test-unit -j4
bash tests/btrfs/test_m5_host.sh
bash tests/btrfs/test_btrfs.sh
bash tests/integration/cases/test_aurafs_fd.sh
bash tests/integration/cases/test_data_volume.sh
python3 tools/check_test_registry.py
python3 tools/check_width_sweep.py --check
git diff --check
```

Use only disposable harness volumes for these formatting/fault tests. Default
ISO does not opt in by itself. Mounted receipt names `legacy`/`dual-v2`, active
slot and generation, always `migration=none`. Reclaim and orphan allocation
policy remain unchanged. Durability declaration stays WEAK pending broader
crash/fault/concurrency evidence. M6 is not closed: root retirement/reuse
protocol, complete lifetime coverage and bounded failure recovery remain.
