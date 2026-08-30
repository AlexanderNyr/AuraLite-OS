#!/usr/bin/env bash
# test_selfhost_mkiso.sh — SELFHOST_PLAN.md SH7d: in-guest MBR+GPT+FAT32 writer.
#
# SH7d is the largest "image tooling in C" twin: a /bin/mkiso that lays down
# the whole hybrid disk (MBR partition table, GPT header+array+backup, and a
# FAT32 ESP with the kernel/EFI/initrd files), replacing the host
# mformat/mcopy pair and the inline python3 BPB patch.  The host unit test
# (test_mkiso.c) parses the produced image and the same image boots on both
# SeaBIOS and OVMF; this case proves the stripped ELF runs on AuraLite and the
# in-guest shell can drive it end to end.
#
# The guest probe (sh7d_probe.sh) covers: the built-in geometry/CRC/BPB/FAT
# self check, assembling a real hybrid image from the staged boot blobs
# (/tests/mbr_dual.bin + /tests/stage2.bin) onto /fat, and a negative control
# (a sub-40 MiB ESP is refused).  Needs no guest toolchain: /bin/mkiso is a
# normal user ELF, so like SH7a/SH7b/SH7c this never skips on a plain `make iso`.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host image tooling (SH7d): in-guest mkiso MBR+GPT+FAT32 writer"

LOG="$IL_LOGDIR/selfhost_mkiso.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "sh /tests/sh7d_probe.sh"
# assembling a 48 MiB image in the TCG guest takes a while; give it room.
il_send_delay 20
il_send "echo SH7D_STILL_ALIVE"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 90

# ---- the §8 receipt ----
il_assert_grep "$LOG" "\[selfhost\] mkiso PASS: .* written in-guest" \
    "the probe ran to completion and printed the SH7d receipt"

# ---- S1: the built-in geometry/CRC/BPB/FAT self check passed in-guest ----
il_assert_grep "$LOG" "SH7d mkiso selftest checks passed" \
    "the tool's --selftest (CRC32 vector, FAT32 geometry, BPB, FSInfo) passed"

# ---- S2: a real hybrid image was assembled in-guest from the staged blobs ----
il_assert_grep "$LOG" "sh7d: s2-image-assembled" \
    "mkiso laid down MBR+GPT+FAT32 onto /fat in-guest"
il_assert_no_grep "$LOG" "UNREACHABLE-ASSEMBLY" \
    "the in-guest image assembly did not fail"

# ---- S3: the negative control fired (a sub-floor ESP is refused) ----
il_assert_grep "$LOG" "sh7d: s3-small-esp-rejected" \
    "a sub-40 MiB ESP (<65525 clusters) is rejected rather than shipped"
il_assert_no_grep "$LOG" "UNREACHABLE" \
    "no probe branch that should be unreachable was taken"

# ---- the shell survives all of it ----
il_assert_grep "$LOG" "^SH7D_STILL_ALIVE$" \
    "the shell still takes commands afterwards"

il_summary
