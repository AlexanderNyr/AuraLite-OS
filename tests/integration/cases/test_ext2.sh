#!/usr/bin/env bash
# test_ext2.sh — boot AuraLite with a second AHCI disk and exercise ext2.
#
# Two passes:
#   1. With a Linux-formatted (mkfs.ext2) image — verify cross-OS compat.
#   2. With a blank disk — verify the in-kernel mkfs.ext2 path.
#
# Tests:
#   - Kernel self-test PASSes (regular file, dir, indirect blocks, rename).
#   - Shell can mkdir/rmdir/mv/rm/stat/write/cat against /ext2.
#   - Files written by AuraLite are readable on the host via debugfs (round-trip).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "ext2 (Linux-formatted + cross-OS round-trip)"

DISK_HOST_FMT="$IL_BUILD/ext2-host.img"
LOG_HOST="$IL_LOGDIR/ext2_host.log"
LOG_BLANK="$IL_LOGDIR/ext2_blank.log"
DISK_BLANK="$IL_BUILD/ext2-blank.img"

# Need /usr/sbin/mkfs.ext2 + debugfs (e2fsprogs).
MKFS=""
DEBUGFS=""
for p in /sbin/mkfs.ext2 /usr/sbin/mkfs.ext2; do [ -x "$p" ] && MKFS="$p"; done
for p in /sbin/debugfs   /usr/sbin/debugfs;   do [ -x "$p" ] && DEBUGFS="$p"; done
if [ -z "$MKFS" ] || [ -z "$DEBUGFS" ]; then
    echo "  ${C_YELLOW}skip: e2fsprogs not installed (need mkfs.ext2 + debugfs)${C_RESET}"
    il_summary
    exit 0
fi

IL_LAST_LOG="$LOG_HOST"
trap il_dump_on_error EXIT

# ---- Pass 1: Linux-formatted ext2 ----
rm -f "$DISK_HOST_FMT"
dd if=/dev/zero of="$DISK_HOST_FMT" bs=1M count=4 status=none
"$MKFS" -q -b 1024 -I 128 -F "$DISK_HOST_FMT" 2>/dev/null

# Seed a file we'll read from AuraLite.
echo "hello from linux mkfs $$" > /tmp/aura_seed.$$.txt
"$DEBUGFS" -w -R "write /tmp/aura_seed.$$.txt LINUX.TXT" "$DISK_HOST_FMT" >/dev/null 2>&1

# Need an AHCI port 0 disk too (kernel expects it for /fat); reuse the
# integration suite's standard one.
DISK0="$IL_BUILD/disk-ahci-test.img"
il_make_disk "$DISK0" 16 "AURALHCI"

AURA_TOKEN="AURATOK_$$"
il_send_delay 8
il_send "ls /ext2"
il_send_delay 1
il_send "cat /ext2/LINUX.TXT"           # read what Linux wrote
il_send_delay 1
il_send "write /ext2/from_aura.txt $AURA_TOKEN"
il_send_delay 1
il_send "cat /ext2/from_aura.txt"
il_send_delay 1
il_send "mkdir /ext2/aura_dir"
il_send_delay 1
il_send "ls /ext2"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG_HOST" 40 \
    -drive "file=$DISK0,format=raw,if=none,id=ahcidisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0" \
    -drive "file=$DISK_HOST_FMT,format=raw,if=none,id=ext2disk" \
    -device "ide-hd,drive=ext2disk,bus=ahci0.1"

# Kernel side
il_assert_grep "$LOG_HOST" "\[ext2\] mounted existing volume"   "ext2 recognised Linux-formatted image"
il_assert_grep "$LOG_HOST" "\[ext2\] PASS:"                     "ext2 kernel self-test"

# Shell side
il_assert_grep "$LOG_HOST" "LINUX.TXT"                          "ls sees Linux-created file"
il_assert_grep "$LOG_HOST" "hello from linux mkfs"              "AuraLite read Linux file"
il_assert_grep "$LOG_HOST" "$AURA_TOKEN"                        "AuraLite write+read round-trip"
il_assert_grep "$LOG_HOST" "mkdir: created /ext2/aura_dir"      "mkdir on ext2"
il_assert_grep "$LOG_HOST" "aura_dir/"                          "subdir visible in listing"

# Linux re-reads what AuraLite wrote.
if "$DEBUGFS" -R "ls /" "$DISK_HOST_FMT" 2>/dev/null | grep -q "from_aura.txt"; then
    il_pass "host debugfs sees AuraLite-created from_aura.txt"
else
    il_fail "host debugfs did not see AuraLite-created file"
fi
content=$("$DEBUGFS" -R "cat from_aura.txt" "$DISK_HOST_FMT" 2>/dev/null | tr -d '\0')
if echo "$content" | grep -q "$AURA_TOKEN"; then
    il_pass "host debugfs read AuraLite-written content"
else
    il_fail "host debugfs read mismatched content: $content"
fi
if "$DEBUGFS" -R "ls /" "$DISK_HOST_FMT" 2>/dev/null | grep -q "aura_dir"; then
    il_pass "host debugfs sees AuraLite-created directory"
else
    il_fail "host debugfs did not see AuraLite-created directory"
fi

# ---- Pass 2: blank disk → kernel formats ----
IL_LAST_LOG="$LOG_BLANK"
rm -f "$DISK_BLANK"
dd if=/dev/zero of="$DISK_BLANK" bs=1M count=4 status=none
rm -f "$DISK0"
il_make_disk "$DISK0" 16 "AURALHCI"

il_send_delay 8
il_send "ls /ext2"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG_BLANK" 25 \
    -drive "file=$DISK0,format=raw,if=none,id=ahcidisk" \
    -device "ahci,id=ahci0" \
    -device "ide-hd,drive=ahcidisk,bus=ahci0.0" \
    -drive "file=$DISK_BLANK,format=raw,if=none,id=ext2disk" \
    -device "ide-hd,drive=ext2disk,bus=ahci0.1"

il_assert_grep "$LOG_BLANK" "\[ext2\] no ext2 magic"            "kernel detected blank disk"
il_assert_grep "$LOG_BLANK" "formatting fresh volume"           "kernel mkfs.ext2 ran"
il_assert_grep "$LOG_BLANK" "\[ext2\] PASS:"                    "self-test on freshly formatted volume"

# Verify the resulting image is mountable by Linux too.
if "$DEBUGFS" -R "stats" "$DISK_BLANK" 2>/dev/null | grep -q "Filesystem magic number:  0xEF53"; then
    il_pass "Linux recognises kernel-formatted ext2"
else
    il_fail "Linux did not recognise kernel-formatted ext2"
fi

rm -f /tmp/aura_seed.$$.txt
il_summary
