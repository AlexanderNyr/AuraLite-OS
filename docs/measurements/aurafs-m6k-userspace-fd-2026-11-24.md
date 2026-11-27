# M6k — end-to-end userspace AuraFS FD/OFD lifetime gate

Date: 2026-11-24 (Europe/Moscow). GitHub HEAD rechecked unchanged at
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Local incremental patch on M6j;
no commit/push, benchmark or speed claim. M6 remains IN PROGRESS.

## Scope

M6j's driver reference pins were not end-to-end FD coverage. M6k adds
`/tests/aurafsfd`, built with the actual AuraLite libc and run in ring 3 on
fresh persistent AuraFS `/data`. Every operation goes through real syscalls
and VFS OFD/FD handling; the program does not touch vnode fields or manufacture
refcounts. Runtime kernel/driver code is unchanged from the delivered M6j.

`tests/integration/cases/test_aurafs_fd.sh` is registered in both ALL_CASES
and the fs group. It boots QEMU 10.0.13 TCG, qemu64, 4 vCPUs, 512 MiB RAM,
fast boot selftests, three newly recreated 64 MiB AHCI images with `/data`
on the persistent volume. Command transport waits for shell prompts instead
of fixed sleeps. No speed threshold is imposed.

## What ran

- Create/write OLD, dup the descriptor and observe the shared initial offset.
- Unlink with both descriptors open, recreate the same pathname and confirm
  a different inode, then write NEW. The old alias still reads OLD, reports
  the original inode and nlink=0.
- Close the first duplicate. Write GONE through the remaining unlinked alias;
  the replacement file still reads NEW.
- Fork with inherited descriptors. The child reads G from the old alias,
  explicitly closes it, and exits; normal process teardown closes its other
  inherited descriptors. Parent waits for successful child status, observes
  offset 1 on the same OFD, reads ONE and still reads NEW from the replacement.
- Perform 96 real create/write/close/unlink cycles on distinct pressure files
  (more names than the 64-slot vnode cache), then re-read both held files.
- Close the final old alias. Rename the replacement, create a hard link, open
  the alias and verify inode equality/nlink=2. Append through the alias and
  read NEW! through the original new-file OFD.
- Close that OFD, unlink both remaining names, and read/stat/fsync the last
  open hardlink descriptor with nlink=0, then close it. Removed name reopen
  must fail with ENOENT.
- Write, fsync, read back and close a separate completion marker. Program
  emits the final PASS receipt only after every prerequisite succeeds.

The gate requires one final PASS receipt, zero shell-observed program exit
status, fork/pressure/unlinked-link receipts, live shell and no test/kernel
failure. It then starts a NEW QEMU on the same images with format permission
OFF and verifies the fsynced completion marker and responsive shell.

Positive gate: **12/12 assertions PASS**. The marker is constant but all three
images are deleted/recreated immediately before boot 1, so a stale disk cannot
supply it. Boot 2 only reads the marker; it does not echo the marker value.
The fork-creation check prints in both processes; it is not counted as two
independent cases. PASS once plus exit status and phase receipts are the gate.

## Negative control

Temporarily reverse only M6j's `kernel/fs/btrfs.c` patch (retain the M6k
userspace binary/gate and all earlier M6a–i changes), rebuild ISO and run the
same gate on fresh images. It fails with:

```text
[aurafs-fd] FAIL: old alias payload survives errno=2
```

The failure is a payload predicate, NOT an errno assertion; errno=2 is the
last libc errno and may be stale after successful operations. This is the
expected pre-fix vnode identity bug. Harness exits nonzero and no successful
completion marker is accepted. Restore the exact fixed driver bytes and
rebuild the fixed ISO afterward. Negative driver bytes are NOT delivered.

## Validation and build environment

Positive guest gate 12/12 and negative-control rejection, existing actual-driver
M6a–j host tests and write-bounce tests pass. Full unit/M5/sanitizer and QEMU
btrfs/data regression outcomes are listed in CHANGELOG when complete.
Registry has 248 cases, all registered. This slice makes no kernel format,
allocator, checkpoint, snapshot or syscall behavior changes.

The restored workspace contained an inconsistent source snapshot without
.git. A fresh clone in `/home/user/AuraLite-work` at the unchanged HEAD plus
checked sequential application of delivered M6a–M6j reconstructs the baseline;
old patch artifacts are unmodified. Build tools were reinstalled. Current
Rust stable 1.100 hit the documented soft-float/SSE ceiling; use Rust 1.98.0
with x86_64-unknown-none, riscv64gc-unknown-none-elf and aarch64-unknown-none
installed. No lint bypass/source workaround was used. Missing llvm-rc was
resolved by installing LLVM tools. Guest/host TCC, multilib and cross-ISA
execution remain optional skips rather than silently claimed coverage.

## Limits

This is serialized userspace FD lifecycle coverage with fork/wait ordering,
NOT a race test for concurrent lookup/close/unlink, a proof of atomic append,
a vnode-cache refcount leak/exhaustion proof or active-FD remount support.
A successful post-power-off completion marker does not prove durability of
open-unlinked inodes, GC, torn-sector recovery, physical-device FLUSH or every
checkpoint candidate. No orphan inode/extent reclaim or checkpoint retirement
is enabled. Reclaim stays none, durability WEAK. M6 remains open for durable
checkpoint publication/retirement and complete concurrency/root lifetimes.

## Reproduce

```bash
export PATH="$HOME/.cargo/bin:/usr/sbin:$PATH"
rustup default 1.98.0
rustup target add x86_64-unknown-none riscv64gc-unknown-none-elf aarch64-unknown-none
make iso -j6
bash tests/integration/cases/test_aurafs_fd.sh
make test-unit -j4
bash tests/btrfs/test_m5_host.sh
bash tests/btrfs/test_btrfs.sh
bash tests/integration/cases/test_data_volume.sh
python3 tools/check_test_registry.py
python3 tools/check_width_sweep.py --check
git diff --check
```

The test deliberately owns `/data/m6k-*` paths and should run only on the
harness-created disposable volume, not a user's production data volume.

## Evidence hashes

SHA-256 of positive boot-1 and boot-2 serial logs, negative boot-1 serial log,
restored fixed kernel/ISO and userspace executable:

```text
38b1e7d46e0144d1bd4cd85ccb97234fc9c65cd8ddd3bcc2e68a16b52f3f124e  positive boot 1
55b47d98a0bec500a8e401e4e1590e7012ea16d9ec5dc3bc33d9fa09ca6b63f2  positive boot 2
b392c7947835524c539259c04f6fc0151a3a8870c8ca0ec5cb39496751737c3d  negative boot 1
6de806a59f19a8ddaea261d0109d3e1c2ee147f47b423d0f3cb79809161638e4  fixed kernel.elf
d47a287b7367ed5b1e07bce51d95cb59b354bf880795b908304b3dfa05919aa1  fixed auralite-dual.iso
bccad510e9ac43f8ff8de8deb10100fc48d742c0320ba38950899b79b9d01546  aurafsfd.elf
```
