#!/usr/bin/env bash
# test_ext4.sh — F3 external-formatter harness: prove interop with the real
# ext4 on-disk format produced by host mkfs.ext4, plus the full mutation
# surface from the shell, plus an internal-lane fsck on a kernel-formatted
# volume.
#
# This mirrors the pattern every later FS phase (F4/F5) copies:
#   1. mkfs.ext4 formats the image; AuraLite mounts it read-write (no
#      journal / -O ^has_journal, the tested lane) and drives the full
#      mutation script from the shell.
#   2. AuraLite formats a fresh disk itself; the host e2fsck -fn must pass
#      on the resulting image.
#
# Requires host e2fsprogs (mkfs.ext4, e2fsck).  Skips cleanly when absent.

set -u
cd "$(dirname "$0")/../.."
. tests/integration/lib/lib.sh
il_init
# F7 (FSFULL_PLAN.md): Pass 2 boots in FAST selftest mode so the kernel's
# other on-disk self-tests stay quiet during the blank-volume lane.  Pass 1
# below overrides this to FULL: the case asserts the ext4 self-test receipt
# ("[ext4] PASS:"), which only runs in the FULL lane — the original harness
# asserted it from a FAST boot, a receipt that lane never prints.
IL_SELFTEST=fast
il_have qemu-system-x86_64

il_section "ext4 (mkfs.ext4 interop + full mutation surface)"

# 6 AHCI disks so the fixed ext4 slot (blkdev 3, /ext4) is reachable.  The
# other slots are fresh AURA disks (formatted by diskfs/fat32/ext2 on their
# own slots); ext4 owns D3.  Same layout as the f2fs/btrfs/exfat/ntfs
# harnesses.
D0="$IL_BUILD/ext4-d0.img"; D1="$IL_BUILD/ext4-d1.img"
D2="$IL_BUILD/ext4-d2.img"; D3="$IL_BUILD/ext4-d3.img"
D4="$IL_BUILD/ext4-d4.img"; D5="$IL_BUILD/ext4-d5.img"
DISK_HOST="$IL_BUILD/ext4-host.img"
DISK_BLANK="$IL_BUILD/ext4-blank.img"
LOG_HOST="$IL_LOGDIR/ext4_host.log"
LOG_BLANK="$IL_LOGDIR/ext4_blank.log"
QEMU_DISKS=(
    -drive "file=$D0,format=raw,if=none,id=d0" -device "ide-hd,drive=d0,bus=ahci0.0"
    -drive "file=$D1,format=raw,if=none,id=d1" -device "ide-hd,drive=d1,bus=ahci0.1"
    -drive "file=$D2,format=raw,if=none,id=d2" -device "ide-hd,drive=d2,bus=ahci0.2"
    -drive "file=$D3,format=raw,if=none,id=d3" -device "ide-hd,drive=d3,bus=ahci0.3"
    -drive "file=$D4,format=raw,if=none,id=d4" -device "ide-hd,drive=d4,bus=ahci0.4"
    -drive "file=$D5,format=raw,if=none,id=d5" -device "ide-hd,drive=d5,bus=ahci0.5"
)
AHCI_DEV=( -device "ahci,id=ahci0" )

MKFS=""
E2FSCK=""
for p in /sbin/mkfs.ext4 /usr/sbin/mkfs.ext4; do [ -x "$p" ] && MKFS="$p"; done
for p in /sbin/e2fsck /usr/sbin/e2fsck /sbin/fsck.ext4; do [ -x "$p" ] && E2FSCK="$p"; done
if [ -z "$MKFS" ] || [ -z "$E2FSCK" ]; then
    echo "  ${C_YELLOW}skip: e2fsprogs not installed (need mkfs.ext4 + e2fsck)${C_RESET}"
    il_summary
    exit 0
fi

AURA_TOKEN="AURATOK_$$"

# ---- Pass 1: Linux-formatted ext4 (no journal) ----
IL_LAST_LOG="$LOG_HOST"
trap il_dump_on_error EXIT
rm -f "$DISK_HOST"
dd if=/dev/zero of="$DISK_HOST" bs=1M count=64 status=none
# ^has_journal is the F3 tested lane (JBD2 replay is out of scope).
"$MKFS" -q -F -O ^has_journal -b 4096 "$DISK_HOST" 2>/dev/null

# Seed a file Linux wrote that AuraLite must read.
echo "hello-from-mkfs-$$" > /tmp/aura_seed.$$.txt
printf 'from-linux\n' > /tmp/aura_seed2.$$.txt
# Use debugfs-less seeding via mke2fs -d root dir: simpler, no debugfs dep.
SEEDDIR="$IL_BUILD/ext4_seed_$$"
rm -rf "$SEEDDIR"; mkdir -p "$SEEDDIR/sub"
printf 'hello-from-mkfs-%s\n' "$$" > "$SEEDDIR/LINUX.TXT"
printf 'nested\n' > "$SEEDDIR/sub/inner.txt"
"$MKFS" -q -F -O ^has_journal -b 4096 -d "$SEEDDIR" "$DISK_HOST" 2>/dev/null

# 6 disks so the fixed /ext4 slot (blkdev 3) exists; ext4 owns D3.
for d in "$D0" "$D1" "$D2" "$D4" "$D5"; do
    rm -f "$d"; il_make_disk "$d" 4 "AURALHCI"
done
cp -f "$DISK_HOST" "$D3"

il_send_delay 8
il_send "ls /ext4"
il_send_delay 1
il_send "cat /ext4/LINUX.TXT"           # read what Linux wrote
il_send_delay 1
il_send "cat /ext4/sub/inner.txt"       # nested
il_send_delay 1
il_send "write /ext4/from_aura.txt $AURA_TOKEN"
il_send_delay 1
il_send "cat /ext4/from_aura.txt"
il_send_delay 1
il_send "mkdir /ext4/aura_dir"
il_send_delay 1
il_send "mv /ext4/from_aura.txt /ext4/renamed.txt"
il_send_delay 1
il_send "stat /ext4/renamed.txt"
il_send_delay 1
il_send "rm /ext4/renamed.txt"
il_send_delay 1
il_send "rmdir /ext4/aura_dir"
il_send_delay 1
il_send "sync"
il_send_delay 1
il_send "exit"

# ext4 is the 4th AHCI disk (blkdev 3, /ext4 in the kernel); the volume is at
# D3.  FULL selftest lane: the "[ext4] PASS:" receipt below is printed by the
# kernel's ext4 self-test, which runs only in FULL (it exercises the driver
# against the just-mounted mkfs volume before the shell does).  Budget 150 s
# covers the full boot selftest suite plus the mutation surface on a loaded
# CI runner (45 s met only local timings).
IL_SELFTEST=full
il_run_qemu "$LOG_HOST" 150 "${AHCI_DEV[@]}" "${QEMU_DISKS[@]}" \
    -fw_cfg "opt/auralite.fsformat,string=0"
IL_SELFTEST=fast

il_assert_grep "$LOG_HOST" "\[ext4\] mounted existing volume"  "ext4 recognised mkfs.ext4 image (no journal)"
il_assert_grep "$LOG_HOST" "\[ext4\] PASS:"                    "ext4 kernel self-test"
il_assert_grep "$LOG_HOST" "hello-from-mkfs"                  "AuraLite read the file Linux wrote"
il_assert_grep "$LOG_HOST" "nested"                            "nested subdir file read"
il_assert_grep "$LOG_HOST" "$AURA_TOKEN"                       "AuraLite write+read round-trip"
il_assert_grep "$LOG_HOST" "mkdir: created /ext4/aura_dir"     "mkdir on ext4"
il_assert_grep "$LOG_HOST" "rename:.*from_aura.txt.*renamed"   "rename on ext4"
il_assert_grep "$LOG_HOST" "rm: removed /ext4/renamed.txt"      "rm on ext4"
il_assert_grep "$LOG_HOST" "rmdir: removed /ext4/aura_dir"      "rmdir on ext4"

# Host re-verifies the volume is still structurally sound after the kernel
# wrote, renamed and deleted on it.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if "$E2FSCK" -fn "$DISK_HOST" >/dev/null 2>&1; then
    il_pass "host e2fsck -fn passes on the volume AuraLite mutated"
else
    il_fail "host e2fsck -fn reported errors after AuraLite mutations"
fi

# ---- Pass 2: blank disk -> kernel formats, host fsck verifies ----
IL_LAST_LOG="$LOG_BLANK"
rm -f "$DISK_BLANK"
dd if=/dev/zero of="$DISK_BLANK" bs=1M count=64 status=none
# Fresh 6-disk layout again, blank ext4 volume at D3 for the kernel to format.
for d in "$D0" "$D1" "$D2" "$D4" "$D5"; do
    rm -f "$d"; il_make_disk "$d" 4 "AURALHCI"
done
cp -f "$DISK_BLANK" "$D3"

il_send_delay 8
il_send "write /ext4/fsckme.txt kernelwrote"
il_send_delay 1
il_send "cat /ext4/fsckme.txt"
il_send_delay 1
il_send "sync"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG_BLANK" 45 "${AHCI_DEV[@]}" "${QEMU_DISKS[@]}" \
    -fw_cfg "opt/auralite.fsformat,string=1"

il_assert_grep "$LOG_BLANK" "format complete"                "kernel formatted the blank ext4 volume"
il_assert_grep "$LOG_BLANK" "kernelwrote"                    "write+read on kernel-formatted volume"

# RESIDUE2 CI fix: fsck the disk the GUEST formatted.  The harness used to
# re-check $DISK_BLANK — the pristine zero image cp'd onto $D3 before boot —
# so e2fsck always saw blank bytes ("Bad magic number") even when the kernel
# had minted a perfectly valid volume on $D3.  The QEMU disk args carry no
# snapshot flag, so $D3 holds the guest-formatted filesystem.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if "$E2FSCK" -fn "$D3" >/dev/null 2>&1; then
    il_pass "host e2fsck -fn passes on kernel-formatted ext4 image"
else
    il_fail "host e2fsck -fn rejected kernel-formatted ext4 image"
fi

rm -rf "$SEEDDIR" /tmp/aura_seed.$$.txt /tmp/aura_seed2.$$.txt
il_summary
