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
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "ext4 (mkfs.ext4 interop + full mutation surface)"

DISK0="$IL_BUILD/disk-ahci-test.img"
DISK_HOST="$IL_BUILD/ext4-host.img"
DISK_BLANK="$IL_BUILD/ext4-blank.img"
LOG_HOST="$IL_LOGDIR/ext4_host.log"
LOG_BLANK="$IL_LOGDIR/ext4_blank.log"

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

il_make_disk "$DISK0" 16 "AURALHCI"

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

# ext4 is the 4th AHCI disk (blkdev 3), like /ext4 in the kernel.
il_run_qemu "$LOG_HOST" 45 \
    -drive "file=$DISK0,format=raw,if=none,id=ahcidisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0" \
    -drive "file=$DISK_HOST,format=raw,if=none,id=ext4disk" \
    -device "ide-hd,drive=ext4disk,bus=ahci0.3" \
    -fw_cfg "opt/auralite.fsformat,string=0"

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
if "$E2FSCK" -fn "$DISK_HOST" >/dev/null 2>&1; then
    il_pass "host e2fsck -fn passes on the volume AuraLite mutated"
else
    il_fail "host e2fsck -fn reported errors after AuraLite mutations"
fi

# ---- Pass 2: blank disk -> kernel formats, host fsck verifies ----
IL_LAST_LOG="$LOG_BLANK"
rm -f "$DISK_BLANK"
dd if=/dev/zero of="$DISK_BLANK" bs=1M count=64 status=none
rm -f "$DISK0"
il_make_disk "$DISK0" 16 "AURALHCI"

il_send_delay 8
il_send "write /ext4/fsckme.txt kernelwrote"
il_send_delay 1
il_send "cat /ext4/fsckme.txt"
il_send_delay 1
il_send "sync"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG_BLANK" 30 \
    -drive "file=$DISK0,format=raw,if=none,id=ahcidisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0" \
    -drive "file=$DISK_BLANK,format=raw,if=none,id=ext4disk" \
    -device "ide-hd,drive=ext4disk,bus=ahci0.3" \
    -fw_cfg "opt/auralite.fsformat,string=1"

il_assert_grep "$LOG_BLANK" "format complete"                "kernel formatted the blank ext4 volume"
il_assert_grep "$LOG_BLANK" "kernelwrote"                    "write+read on kernel-formatted volume"

if "$E2FSCK" -fn "$DISK_BLANK" >/dev/null 2>&1; then
    il_pass "host e2fsck -fn passes on kernel-formatted ext4 image"
else
    il_fail "host e2fsck -fn rejected kernel-formatted ext4 image"
fi

rm -rf "$SEEDDIR" /tmp/aura_seed.$$.txt /tmp/aura_seed2.$$.txt
il_summary
