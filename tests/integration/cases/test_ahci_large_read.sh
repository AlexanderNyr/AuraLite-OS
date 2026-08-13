#!/usr/bin/env bash
# test_ahci_large_read.sh — the AHCI/FAT32 read path, on a file big enough to matter.
#
# DOOM_PLAN.md D4/D5.  This is the cheap gate for a bug that made EVERY file
# on an AHCI-backed FAT32 volume come back truncated at exactly 173824 bytes:
# ahci_read() allocated a DMA bounce buffer per transfer and never freed it,
# so after a few hundred sector reads pmm_alloc_contiguous() ran dry, the read
# returned -1, and FAT32 reported that as a clean EOF.  Silently short data,
# not an error.
#
# Nothing in the suite noticed for the life of the driver, because nothing
# read a large file.  The boot path and the self-tests touch a few dozen
# sectors; the leak needs a few hundred to bite.  That is the whole lesson of
# this case, and why it exists separately from the DOOM case: it needs no WAD,
# no download and no UEFI, so it can run on every push.
#
# It attaches a purpose-built FAT32 disk carrying one file of a known size and
# asserts the guest reads back exactly that many bytes.  /apps/filesize reads
# to EOF rather than calling stat(), because a size taken from the directory
# entry is precisely what would hide this bug.
#
# The disk layout is not arbitrary: the kernel mounts whatever valid FAT32 BPB
# sits at LBA 64.  An image formatted at offset 0 has no signature there, so
# the kernel decides the disk is blank and FORMATS it -- destroying the very
# file under test.  Hence the 64 zero sectors in front.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 mformat mcopy

il_section "AHCI + FAT32 large-file read (regression: the 173824-byte ceiling)"

LOG="$IL_LOGDIR/ahci_large_read.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

DISK="$IL_BUILD/ahci_read_test.img"
PART="$IL_BUILD/ahci_read_part.img"
PAYLOAD="$IL_BUILD/ahci_read_payload.bin"

# 16 MiB of payload.  The size is MEASURED, not guessed: with the leak
# reintroduced deliberately, reads failed after 10239488 bytes (~9.8 MiB) on a
# 512 MiB guest -- that is where pmm_alloc_contiguous() finally cannot place
# the next bounce buffer.  A 1 MiB payload was tried first and the reintroduced
# bug SURVIVED it, which is the only reason this number is not 1 MiB.
#
# The lesson generalises: a regression test for a resource leak has to exceed
# the resource, and the only way to know that threshold is to put the bug back
# and measure it.
PAYLOAD_BYTES=$((16 * 1024 * 1024))

rm -f "$DISK" "$PART" "$PAYLOAD"

# Deterministic contents, so a corrupt read is a size mismatch rather than
# an accident of whatever was in /dev/urandom.
dd if=/dev/zero bs=1M count=16 2>/dev/null | tr '\0' 'A' > "$PAYLOAD"

dd if=/dev/zero of="$PART" bs=1M count=48 2>/dev/null
mformat -i "$PART" -F -h 32 -s 32 -t 96 ::
mcopy -i "$PART" "$PAYLOAD" ::/payload.bin

# 64 zero sectors, then the filesystem -- see the header comment.
dd if=/dev/zero of="$DISK" bs=512 count=64 2>/dev/null
cat "$PART" >> "$DISK"

# Fail early and clearly if the layout is wrong, rather than letting the
# guest silently reformat the disk and report a confusing 0 bytes.
python3 - "$DISK" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
bpb = d[64 * 512:65 * 512]
assert bpb[510] == 0x55 and bpb[511] == 0xAA, "no 0x55AA at LBA 64"
assert bpb[82:87] == b'FAT32', "no FAT32 signature at LBA 64"
PY

il_send_delay 16
il_send "run /apps/filesize /fat/payload.bin"
# 16 MiB at the ~740 KB/s the driver sustains is ~22 s; allow generous margin.
il_send_delay 45

il_run_qemu "$LOG" 100 \
    -drive "id=fatdisk,file=$DISK,format=raw,if=none,snapshot=on" \
    -device ahci,id=ahci \
    -device ide-hd,drive=fatdisk,bus=ahci.0

il_assert_grep_fixed "$LOG" "[fat32] mounted FAT32 at /fat" "second AHCI disk mounted at /fat"
il_assert_no_grep_fixed "$LOG" "[fat32] formatting default FAT32 volume" "kernel did not reformat the test disk"

# The assertion this case exists for.
il_assert_grep_fixed "$LOG" "FILESIZE /fat/payload.bin $PAYLOAD_BYTES" \
    "read back all $PAYLOAD_BYTES bytes"

# With the leak present the read does not merely come up short, it ERRORS --
# so assert the absence of that too, naming it so a future reader does not
# have to recognise the failure from a byte count alone.
il_assert_no_grep_fixed "$LOG" "FILESIZE /fat/payload.bin 173824" \
    "not truncated at the original DMA-leak ceiling"
il_assert_no_grep_fixed "$LOG" "FILESIZE-READ-ERROR" "no read error"
il_assert_no_grep_fixed "$LOG" "FILESIZE-OPEN-FAIL"  "file opened"

il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no exception"
il_assert_no_grep "$LOG" "PANIC"               "no panic"

rm -f "$DISK" "$PART" "$PAYLOAD"

il_summary
