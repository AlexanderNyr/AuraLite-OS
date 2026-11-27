# M6o — initial dual-v2 format interruption matrix

Date: 2026-11-24, Europe/Moscow. GitHub HEAD rechecked unchanged:
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Local incremental patch after
M6n, no commit/push. Test/evidence-only slice: no production driver change.
User's new-empty-volume opt-in/no migration decision remains in force.

## Purpose and model

M6n covered ordinary alternating sync failure and fresh QEMU recovery but
left initial-format interruption coverage incomplete. This slice drives the
actual btrfs.c + buffer_cache.c + blkdev.c against an 8 MiB raw RAM backend.
Every case starts with zero media and explicit v2 + generic format permission.
The successful baseline has three backend write requests (initial metadata,
slot A, slot B), four FLUSH requests and two publication readbacks (subsequent
mount reads are counted separately in the baseline instrumentation). The test
asserts the current request shape; intentional format changes require review
of this matrix rather than silently changing fault positions.

Faults are one-shot rejected backend requests, accepted prefixes of a
checkpoint, failed barrier calls and failed direct checkpoint readbacks.
Writes normally become immediately visible; this is NOT a volatile device
write-cache simulator. `bc_init()` discards the filesystem cache without a
clean unmount. No timing/performance claim, concurrent callers, physical
power-cut, guest reboot or controller reset is modelled by these new cases.

## Matrix: 17 cases PASS

| Injection | Cases | Valid stored slots after failed init |
|---|---:|---|
| Reject initial metadata write | 1 | none |
| Reject A write once | 1 | A (bounded range-flush retry succeeds, return remains failure) |
| Reject B write once | 1 | A and B (same retry behaviour) |
| Accept A prefix: 0, 512, 2048, 3584 bytes | 4 | none |
| Accept B prefix: 0, 512, 2048, 3584 bytes | 4 | A |
| Fail initial pre-scan FLUSH | 1 | none; zero media |
| Fail post-metadata FLUSH | 1 | none; orphan metadata remains |
| Fail post-A FLUSH | 1 | A visible; persistence not inferred |
| Fail post-B FLUSH | 1 | A and B visible; persistence not inferred |
| Fail A direct publication readback | 1 | A |
| Fail B direct publication readback | 1 | A and B |

The one-shot rejected write is not a permanent disk outage. The buffer cache
retries dirty buffers within its bounded range-flush passes but preserves the
error result. Therefore "format failed" does NOT mean no checkpoint exists.
Accepted prefix writes return success at the seam, so only exact readback
exposes the incomplete target image. Cuts are checkpoint-only: arbitrary
silently corrupted appended metadata is outside this copy-selection guarantee.

For every case the test checks:

1. Initial format returns an error and `bm.mounted` stays false.
2. Independently SHA/geometry-validate both stored slot images and check the
   expected valid-copy count from the matrix, not merely the return status.
3. Disable injection, discard cache, turn BOTH creation permissions OFF.
4. Mount succeeds iff there is at least one valid stored copy, with unchanged
   backend write count. No valid copy means refusal, not auto-format.
5. For no-copy states, explicitly re-enable both permissions and retry. Zero
   media can be provisioned; nonzero remnants must be refused without writes.
   No automatic wipe/resume tool is supplied; operators must use deliberate
   external provisioning if they choose to discard an interrupted volume.
6. Every recovered/provisioned tree supports file creation, byte-exact write,
   sync, another cache-drop mount with permissions OFF and byte-exact read.

Thus the test does more than count superblock receipts: it traverses/extends
the recovered metadata and exercises subsequent inactive-slot publication.
It does not prove recovery from arbitrary re-sealed malformed tree blocks.

## Negative control

A temporary driver copy removes only the all-zero-content refusal in the
full-device creation scan. A dedicated main invokes the new M6o matrix (not
the earlier M6n tests). It fails the nonzero retry assertion in
`format_fault_recover`: destructive reformat is incorrectly accepted. The
control is not included in delivered runtime source. Normal source passes.

## Validation performed for this slice

- Focused actual-driver suite M6a through M6o PASS; new M6o receipt: 17 cases,
  three baseline write requests, four barriers.
- fsformat API tests PASS.
- ASan + UBSan PASS, `ASAN_OPTIONS=detect_leaks=0` because prior cache/vnode
  retention is not a leak-free lifetime claim.
- M5 host regression PASS (layout, splits, lookup, CoW, delete/reinsert,
  oversize, root growth, hash collisions).
- Integration registry: 249 cases, all registered; no new guest case added.
- Width sweep PASS: 448/449 casts, 69/69 x64 includes, zero cross-arch hits,
  27/27 asm files. `git diff --check` PASS.
- No new full unit, ISO or QEMU run in this host-test-only slice. M6n's guest
  12/12 recovery, legacy 21/21 + FD 12/12 + persistence 9/9 are historical
  receipts, not reruns here. Host Makefile emits missing-rustc discovery
  diagnostics in this restored environment; the explicitly selected C test
  targets compile/run successfully. No Rust/guest coverage is inferred.

Reproduction from HEAD + patches a through o:

```sh
make build/test_aurafs_m6 build/test_fsformat
build/test_aurafs_m6
build/test_fsformat
bash tests/btrfs/test_m5_host.sh
cc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -I . tests/unit/test_aurafs_m6.c \
  kernel/fs/buffer_cache.c kernel/fs/blkdev.c kernel/lib/sha256.c \
  -Wl,--gc-sections -o build/test_aurafs_m6_sanitize
ASAN_OPTIONS=detect_leaks=0 build/test_aurafs_m6_sanitize
python3 tools/check_test_registry.py
python3 tools/check_width_sweep.py --check
git diff --check
```

## Boundaries

No legacy migration, mount-time repair, on-disk format change, root
retirement, reuse, orphan reclamation or generation protocol change. A failed
format can recover a completed copy; no rollback guarantee. Nonzero no-copy
media remains refused. Backend barrier honesty and integrity of a surviving
checkpoint and its referenced tree remain assumptions. Durability stays WEAK;
M6 remains IN PROGRESS. Broader volatile-cache/crash and lifetime/retirement
proofs remain work, not permission to enable reclaim.
