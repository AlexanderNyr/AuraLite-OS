# M6r — fail-closed selected-root guard for dual-v2 mount

Date: 2026-11-24, Europe/Moscow. GitHub HEAD rechecked unchanged:
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Incremental local patch on M6q,
no commit/push. First runtime change since M6n; no on-disk format change.
User's explicit empty-volume v2 creation / no legacy migration policy remains.

## Narrow policy change

Previously a highest-generation SHA-valid v2 superblock could be accepted
without reading its root block; an unreadable/corrupt selected root surfaced
only on later tree access. After normal checkpoint selection, but BEFORE
`bm.mounted` and mount state are published, M6r reads and validates exactly
ONE selected root block directly through blkdev_read.

If this guard fails, the entire mount fails. It does NOT try the older
candidate's tree, rewrite either checkpoint, migrate, repair or format. This
avoids silently introducing a new namespace rollback policy for superblocks
that are themselves valid. Existing M6n fallback for invalid checkpoint copies
is unchanged. The receipt says:

```text
[aurafs] selected checkpoint root guard failed; mount refused, no older-root rollback or repair
```

A damaged selected root with intact older checkpoint/root therefore refuses
mount. A damaged UNSELECTED older root does not veto a good selected root;
its conservative history remains retained and a later audit can still fail.
The guard is v2-only: legacy creation/mount/sync behaviour is unchanged.

## Checks and bounds

- One temporary 4096-byte heap buffer, freed on every success/failure path.
  Allocation failure is a mount error, not permission to bypass verification.
- One direct 4096-byte backend read at the selected root address. This bypasses
  filesystem/verified-block caches; errors fail closed. The superblock has
  already passed the existing geometry/root/cursor validation.
- Root address inside the checkpoint's aligned allocated prefix; block magic,
  full SHA-256 trailer, physical self-address and metadata type checked.
- A leaf root: bounded slot count plus existing read-only audit leaf-layout
  checks (sorted keys, table/payload bounds, nonoverlap and record shapes).
- An internal root: nonzero bounded pointer count, strictly sorted keys,
  aligned child pointers wholly below the committed cursor, no direct self
  child. Child blocks are NOT read by this guard.
- Failure clears tentative checkpoint history/counters and leaves mounted
  false. No successful mount/format receipt is emitted for the refused volume.

Maximum I/O is independent of volume/tree size. Local layout work is bounded
by the existing maximum block slot count (leaf overlap check is quadratic in
that fixed bound). No new static 4 KiB kernel scratch buffer or 4 KiB stack
array is added. Shared driver scratch and serialized-caller requirements are
unchanged; this is not a concurrent mount protocol.

The root's generation need not equal the checkpoint generation: unchanged
roots may be republished, and root headers have only 32-bit generation. This
slice makes no ancestry/authentication claim and does not enforce owner values
that differ between initial format and subsequent tree updates.

## Explicit non-goals

This is NOT recursive tree validation, fsck, a complete mount safety boundary,
all-descendant cycle detection, data/extent verification, or a new fallback
policy for corrupt trees. A good internal root pointing to a damaged child
can STILL mount and then fail lookup/read. The host test deliberately pins
that limitation. Leaf structural validation does not traverse extent data.
A re-sealed semantically malicious tree is not authenticated. There is no
claim of durable-media read bypass beyond the backend interface: a physical
device may itself return volatile-cache data.

No migration/repair, reclaim, root retirement, orphan GC, committed-block reuse,
format version change, performance improvement or EXACT durability claim.
Durability remains WEAK; M6 IN PROGRESS.

## Host evidence

Actual-driver tests cover:

- Valid selected leaf and internal root fixtures, with byte-exact NEW reads.
- Warm filesystem/verification caches followed by direct media SHA corruption:
  guard refuses despite a valid older checkpoint (no automatic rollback).
- Selected root backend read failure and temporary-buffer OOM refuse without
  writes, mounted state or surviving tentative history.
- Re-sealed wrong type, wrong physical self, huge leaf count and table-overlap
  leaf payload; internal empty/huge pointer count, direct self-child, unaligned
  child, child at allocation end and duplicate/unsorted keys all refuse.
- Corrupt older unselected root leaves selected NEW mount/read working.
- Good selected internal root with bad descendant still mounts, then lookup
  fails: the guard's deliberately nonrecursive limit is executable evidence.

The media fixtures are explicit host edits, not a production write path.
Negative control in a copied driver disables only the guard invocation. It
fails the new guard_refuses assertion on warmed-cache selected-root damage.
No bypass or sabotage hook is delivered.

Both actual-driver suite and the combined M6p/M6q 930-case power model pass
under ASan/UBSan (leak detection off for retained historical cache allocations).
M5 host regression passes. Full `make test-unit -j4` PASS, including the
registered root-guard and combined 930-case power targets (both receipts
verified in the full-suite log). Optional TCC/libtcc1, -m32 host ABI and
cross-ISA execution skips remain explicit, not executed coverage. Toolchain
restored: Rust 1.98.0 plus the three bare-metal targets; no lint bypass.

## Real QEMU gate

The existing registered `test_aurafs_dual.sh` is extended rather than adding
a duplicate registry case. Its first two boots still test v2 creation, torn
super fallback and actual userspace FD workload. Boot 2 then writes/syncs two
distinct guard-note states, yielding distinct valid checkpoint roots.

After QEMU exits, Python validates BOTH stored superblock SHA/self/generation
receipts, both distinct root SHA trailers, and then zeros only the newest
root's first sector on disposable media. It keeps the older root and BOTH
superblocks intact. Record a SHA-256 of the entire faulted data image.

Boot 3 has both creation knobs OFF: require the root-guard refusal receipt,
no /data success or v2 mounted receipt (no older-root fallback), a live shell,
and no format/kernel failure. After QEMU exits, require the entire faulted
data image's SHA-256 to be unchanged. This is a between-boots deterministic
media corruption fixture, not a live hardware power cut.

## Completed regression receipts

- ISO build PASS (clang/lld, Rust 1.98.0; default opt-in remains OFF).
- Extended three-boot dual-v2 QEMU gate **18/18 PASS**, including whole-image
  SHA equality after refused root-corruption boot. QEMU 10.0.13, TCG/qemu64,
  4 vCPUs, 512 MiB, three disposable 64 MiB raw AHCI disks.
- Legacy btrfs QEMU **21/21 PASS**.
- Legacy actual userspace FD QEMU gate **12/12 PASS**.
- Persistent /data QEMU gate **9/9 PASS**.
- Full unit suite, M5 host, both ASan/UBSan suites and guard-disabled negative
  control as described above PASS (negative control fails as required).
- Integration registry 249 cases all registered; width sweep PASS:
  casts 448/449, x64 includes 69/69, cross-arch 0, asm files 27/27.
- Diff whitespace, HEAD+a..r sequential check/apply and delivered-file byte
  equality PASS. This is a local patch, not an upstream commit or push.

## Reproduction

```sh
export PATH="$HOME/.cargo/bin:/usr/sbin:$PATH"
make build/test_aurafs_m6 build/test_aurafs_power build/test_fsformat
build/test_aurafs_m6 && build/test_aurafs_power && build/test_fsformat
bash tests/btrfs/test_m5_host.sh
make test-unit -j4
make iso -j6
bash tests/integration/cases/test_aurafs_dual.sh
bash tests/btrfs/test_btrfs.sh
bash tests/integration/cases/test_aurafs_fd.sh
bash tests/integration/cases/test_data_volume.sh
python3 tools/check_test_registry.py
python3 tools/check_width_sweep.py --check
git diff --check
```

ASan/UBSan: compile each actual-driver test source with buffer_cache.c,
blkdev.c and sha256.c, `-O1 -g -fsanitize=address,undefined
-fno-omit-frame-pointer -ffunction-sections -fdata-sections -I .
-Wl,--gc-sections`; run with `ASAN_OPTIONS=detect_leaks=0`.

Use disposable raw volumes only. Production recovery keeps generic format
permission OFF. Refused mount preserves bytes for explicit offline recovery;
this slice supplies no repair utility or automatic selection of an older tree.
