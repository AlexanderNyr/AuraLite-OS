#!/usr/bin/env bash
# test_nvme.sh — RESIDUE3 C7b: an NVMe namespace, mounted.
#
# RES-46's storage successor closes with a MOUNTED FILESYSTEM, not a probe, so
# this case does three things and refuses to count any of them as the others:
#
#   1. the controller's own identity — model/serial/firmware read back from an
#      Identify Controller command (a receipt a reader can tell apart from a
#      printf, exactly like the Bluetooth driver's BD_ADDR);
#   2. the namespace as a block device — size, LBA shape, and the driver's own
#      self-test reading LBA 0 twice (through the driver and through the
#      blkdev seam) and comparing the two;
#   3. ext2 ON that namespace — the kernel mounts /ext2 from blkdev 1, which
#      here is the NVMe namespace and nothing else, and the shell reads a file
#      Linux wrote.  The host then reads back what the guest wrote.
#
# The disk order is load-bearing: the AHCI disk registers first (blkdev 0) so
# the NVMe namespace lands on blkdev 1, which is the disk kernel.c mounts at
# /ext2.  That is not a trick for this test — it is the same "second disk"
# rule every other filesystem case uses.
#
# A second, quieter boot asserts the opposite: with no NVMe device attached
# the driver says NOTHING.  A probe that prints on every machine is noise; a
# probe that stays silent when there is nothing to drive is the receipt that
# the lines above came from hardware.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "NVMe: a namespace that mounts (RESIDUE3 C7b)"

MKFS=""; DEBUGFS=""
for p in /sbin/mkfs.ext2 /usr/sbin/mkfs.ext2 /sbin/mke2fs /usr/sbin/mke2fs; do
    [ -x "$p" ] && MKFS="$p" && break
done
for p in /sbin/debugfs /usr/sbin/debugfs; do
    [ -x "$p" ] && DEBUGFS="$p" && break
done
if [ -z "$MKFS" ] || [ -z "$DEBUGFS" ]; then
    il_skip "e2fsprogs not installed (need mkfs.ext2 + debugfs): no NVMe filesystem was mounted"
    il_summary
    exit 0
fi

IMGDIR="$IL_BUILD/nvme"
mkdir -p "$IMGDIR"
NVME_IMG="$IMGDIR/nvme-ext2.img"
AHCI_IMG="$IMGDIR/ahci-first.img"
LOG="$IL_LOGDIR/nvme.log"
LOG_ABSENT="$IL_LOGDIR/nvme_absent.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# 16 MiB namespace, 1 KiB ext2 blocks: the same shape test_ext2.sh uses on
# AHCI, so a failure here is the transport and not the filesystem.
dd if=/dev/zero of="$NVME_IMG" bs=1M count=16 status=none
"$MKFS" -q -b 1024 -I 128 -F "$NVME_IMG" 2>/dev/null
SEED="hello from linux, read back over NVMe $$"
printf '%s\n' "$SEED" > "$IMGDIR/seed.txt"
"$DEBUGFS" -w -R "write $IMGDIR/seed.txt NVME.TXT" "$NVME_IMG" >/dev/null 2>&1

# The first disk only has to EXIST so the namespace is not disk 0.
dd if=/dev/zero of="$AHCI_IMG" bs=1M count=8 status=none

AURA_TOKEN="AURANVME_$$"
il_send "ls /ext2"
il_send "cat /ext2/NVME.TXT"
il_send "write /ext2/from_aura.txt $AURA_TOKEN"
il_send "cat /ext2/from_aura.txt"

IL_QEMU_EXTRA=(
    -drive "file=$AHCI_IMG,format=raw,if=none,id=ahcidisk"
    -device "ahci,id=ahci0"
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0"
    -drive "file=$NVME_IMG,format=raw,if=none,id=nvmedisk"
    -device "nvme,serial=AURA0001,drive=nvmedisk"
)
il_run_qemu "$LOG" 90 \
    -drive file="$IL_BUILD/auralite.iso",format=raw,if=ide,snapshot=on \
    -boot order=c -m 512M -smp 2 -display none -no-reboot -cpu qemu64 \
    "${IL_QEMU_EXTRA[@]}"

# --- 1. The controller's own identity ------------------------------------
il_assert_grep "$LOG" "\\[nvme\\] controller at PCI [0-9]+:[0-9]+\\.[0-9]+" \
    "an NVMe controller was found on the PCI bus"
il_assert_grep "$LOG" "\\[nvme\\] VS=0x00010400 CAP=0x" \
    "the controller reported a version and capabilities"
il_assert_grep_fixed "$LOG" "[nvme] reset: CC.EN=0 -> CSTS.RDY=0" \
    "the controller acknowledged the reset (CC.EN=0)"
il_assert_grep_fixed "$LOG" "[nvme] reset: CC.EN=1 -> CSTS.RDY=1 (controller ready)" \
    "the controller came ready with the admin queues armed"
il_assert_grep "$LOG" '\[nvme\] identify controller: model="QEMU NVMe Ctrl" serial="AURA0001" firmware="' \
    "the model/serial/firmware are the controller's own Identify bytes"

# --- 2. The namespace as a block device ---------------------------------
il_assert_grep_fixed "$LOG" "[nvme] namespace 1: 32768 blocks x 512 bytes = 16 MiB" \
    "Identify Namespace reports the real geometry"
il_assert_grep_fixed "$LOG" "[nvme] I/O queue pair ready (qid 1, 8 entries, physically contiguous, no interrupts — completion is polled)" \
    "an I/O queue pair was created and bound"
il_assert_grep_fixed "$LOG" "[blkdev] registered 'nvme0' as disk 1 (32768 sectors)" \
    "the namespace became a blkdev (disk 1: the AHCI disk registered first)"
il_assert_grep "$LOG" "\\[nvme\\] read LBA 0: first bytes [0-9a-f]{2}" \
    "LBA 0 was read back through the driver"
il_assert_grep "$LOG" "\\[nvme\\] PASS: 32768 blocks reachable, both paths agree on LBA 0" \
    "the driver path and the blkdev seam agree byte for byte"
il_assert_no_grep "$LOG" "\\[nvme\\] FAIL" "no NVMe self-test failure"
il_assert_no_grep "$LOG" "completion timeout" "no queue timeout"

# --- 3. ext2 mounted off that namespace ---------------------------------
il_assert_grep_fixed "$LOG" "[ext2] blkdev 1 mounted at /ext2" \
    "ext2 mounted from the NVMe namespace"
il_assert_grep "$LOG" "\\[ext2\\] mounted existing volume: block_size=1024" \
    "the volume was recognised, not formatted"
il_assert_grep "$LOG" "\\[ext2\\] PASS: ext2 read/write/dir/indirect works" \
    "the filesystem's own self-test ran over NVMe"
il_assert_grep_fixed "$LOG" "NVME.TXT" \
    "the shell lists the file Linux wrote onto the namespace"
il_assert_grep_fixed "$LOG" "$SEED" \
    "the guest read Linux's content back over NVMe"
il_assert_grep_fixed "$LOG" "$AURA_TOKEN" \
    "the guest wrote and re-read its own file on the namespace"

# The host reads what the guest wrote — the round trip is closed outside the
# guest's own opinion of itself.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))   # manual assert: count it, or the
                                           # total under-reports (AUDIT_A2)
if "$DEBUGFS" -R "ls /" "$NVME_IMG" 2>/dev/null | grep -q "from_aura.txt"; then
    il_pass "host debugfs sees the file the guest created on the NVMe namespace"
else
    il_fail "host debugfs did not see from_aura.txt on the NVMe image"
fi
CONTENT="$("$DEBUGFS" -R "cat from_aura.txt" "$NVME_IMG" 2>/dev/null | tr -d '\0')"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if printf '%s' "$CONTENT" | grep -q "$AURA_TOKEN"; then
    il_pass "host read the guest's bytes back off the namespace"
else
    il_fail "host read mismatched content off the namespace ('$CONTENT')"
fi

il_assert_no_grep "$LOG" "PANIC|page fault in kernel" "no kernel fault"

# --- 4. The negative control: no controller, no claim -------------------
il_section "NVMe: with no controller attached the driver stays silent"
il_run_qemu "$LOG_ABSENT" 60 \
    -drive file="$IL_BUILD/auralite.iso",format=raw,if=ide,snapshot=on \
    -boot order=c -m 512M -smp 2 -display none -no-reboot -cpu qemu64
IL_LAST_LOG="$LOG_ABSENT"
il_assert_no_grep "$LOG_ABSENT" "\\[nvme\\]" \
    "no NVMe lines at all when the machine has no NVMe controller"
il_assert_no_grep "$LOG_ABSENT" "PANIC|page fault in kernel" \
    "and the boot is unaffected"

il_summary
