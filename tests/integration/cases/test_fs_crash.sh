#!/usr/bin/env bash
# test_fs_crash.sh — FSMODERN_PLAN.md M3: what survives a power cut.
#
# The acceptance criterion has two halves, and they are different KINDS of
# claim, so each is measured separately:
#
#   (a) a file that a program wrote is either COMPLETE or ABSENT after a
#       power cut -- never half-written, never a directory entry pointing at
#       somebody else's clusters;
#   (b) the filesystem MOUNTS on the next boot without a repair pass.
#
# FAT32 must pass both (its state IS the cache; M3 declared it EXACT).
# btrfs may fail (b) -- it declares FS_DURABILITY_WEAK and the mount receipt
# says so -- but it must never produce a half-file.
#
# WHY THE GUEST DOES THE WRITE.  The first version drove the pen from the
# host: type `write /fat/crash.txt <payload>`, wait for the receipt, kill.
# That measures the host's 200 ms serial poll, not the filesystem: by the
# time `wrote` reaches the log the shell has finished, and a kill
# "immediately after" the marker is really a kill up to a second later --
# the size of the 1 Hz writeback window M3 measured.  So the guest runs
# tools/fsbench/fsbench.c's --crash-loop mode, which does
#     write -> fsync() -> print -> (unsynced rewrite) -> (unsynced append)
# in ONE process.  The host's kill lands within a few instructions of the
# fsync receipt instead of a few hundred milliseconds.
#
# THE FOUR BOOTS:
#
#   BOOT 1  --crash-loop on a fresh FAT32 volume, SIGKILL a random 0..1200 ms
#           after the `synced` receipt.  The range deliberately straddles
#           M3's measured loss window (~1 s without a sync), so some runs
#           kill inside the window and some after it.  Either answer is
#           legal for (a); a TORN file is not.
#   BOOT 2  same disk, no format gate.  Criterion (b): mount, and prove the
#           volume needs no repair.  Also criterion (a), measured on the
#           MEDIUM rather than in cache: every byte present must match the
#           payload's arithmetic, and the size must be a whole payload
#           multiple (8000, 16000 or 24000).
#   BOOT 3  --crash-loop again, killed the moment `synced` appears (0 ms).
#           THIS is the contract: fsync() returned success, so the synced
#           bytes must be on the medium.  Absence here is a FAILURE -- the
#           difference between M3 and M2.
#   BOOT 4  read the medium again: the synced prefix is there and whole.
#
# -fw_cfg fsformat=1 is passed on EVERY boot on purpose.  Without the gate a
# blank volume is refused, and a refused mount looks exactly like a pristine
# one -- how the first version of this test "proved" survival against a disk
# it had just reformatted.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "M3: power-cut crash test (/fat is EXACT, never half a file)"

LOG="$IL_LOGDIR/fs_crash.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

DISK="$IL_LOGDIR/fs_crash.img"
rm -f "$DISK"
truncate -s 64M "$DISK" 2>/dev/null || dd if=/dev/zero of="$DISK" bs=1M count=64 status=none

# fsbench fills the payload with ((i*7+1) & 0xff); 8000 bytes = 16 sectors,
# big enough for a cut-short write to be visible, small enough that the whole
# loop fits in one burst of cache work.  Legal sizes after a crash are
# SIZE (only the synced write landed), 2*SIZE (rewrite landed) or 3*SIZE.
SIZE=8000
S1=$SIZE; S2=$((SIZE * 2)); S3=$((SIZE * 3))

# --- helpers ---------------------------------------------------------
#
# NOTE ON set -e: il_run_qemu ends with `set -e` on purpose (a failed boot
# must fail the case).  A crash case kills QEMU itself, so its own `wait`
# returns 137 -- which under set -e would kill THIS script before a single
# assertion ran.  Every boot wrapper below turns it back off around the
# parts that are supposed to see a killed process.
set +e

# il_crash_boot <log> <marker-regex> <delay_s> <command>
#
# Boot with a FIFO-fed pen (a plain `< file` redirect makes QEMU read EOF
# once and never look again), poll the live log every 20 ms for <marker>,
# then sleep <delay> IN THE KILLER and SIGKILL.  The delay must live here:
# in the feeder it delays the NEXT COMMAND, not the kill, and every case
# then measures the same instant at the marker.
il_crash_boot() {
    local log="$1" marker="$2" delay="$3" cmd="$4"
    local fifo="$IL_LOGDIR/crash.fifo"
    rm -f "$fifo"; mkfifo "$fifo"

    ( sleep 34; printf '\r'; printf '%s\r' "$cmd"; sleep 180 ) >"$fifo" 2>/dev/null &
    local feeder=$!

    qemu-system-x86_64 \
        -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on" \
        -drive "file=$DISK,format=raw,if=none,id=d0" \
        -device ahci,id=ahci0 -device ide-hd,drive=d0,bus=ahci0.0 \
        -m 512M -smp 2 -display none -serial stdio -no-reboot \
        -fw_cfg name=opt/auralite.fsformat,string=1 \
        <"$fifo" >"$log" 2>&1 &
    local qp=$!

    local end=$(( $(date +%s) + 150 )) seen=0
    while [ "$(date +%s)" -lt "$end" ]; do
        if grep -qE -- "$marker" "$log" 2>/dev/null; then seen=1; break; fi
        sleep 0.02
    done

    if [ "$seen" -eq 1 ]; then
        sleep "$delay"
        kill -9 "$qp" 2>/dev/null
        echo "[crash] marker seen, SIGKILL after ${delay}s"
    else
        kill -9 "$qp" 2>/dev/null
        echo "[crash] marker NEVER seen within 150s -- killing anyway"
    fi
    wait "$qp" 2>/dev/null
    kill "$feeder" 2>/dev/null
    wait "$feeder" 2>/dev/null
    rm -f "$fifo"
    [ "$seen" -eq 1 ]
}

# il_boot_read <log> <guest command...> — a normal boot on the SAME disk.
il_boot_read() {
    local log="$1"; shift
    local cmd
    for cmd in "$@"; do il_send "$cmd"; done
    il_send "exit"
    il_run_qemu "$log" 140 \
        -drive "id=d0,file=$DISK,format=raw,if=none" -device ahci,id=ahci0 \
        -device ide-hd,drive=d0,bus=ahci0.0 \
        -fw_cfg name=opt/auralite.fsformat,string=1
    set +e
}

# crash_size <log>   → the Size: line's number, empty when the file is absent
crash_size() {
    grep -oE "Size: +[0-9]+" "$1" 2>/dev/null | head -1 | grep -oE "[0-9]+"
}

# ===== BOOT 1: write + fsync, die at a random point ==================
DELAY_MS=$(( RANDOM % 1200 ))
DELAY=$(awk "BEGIN{ printf \"%.3f\", $DELAY_MS/1000 }")
echo "[crash] boot 1: fsbench --crash-loop on /fat, SIGKILL ${DELAY}s after 'synced'"

B1="$IL_LOGDIR/fs_crash.b1.log"
il_crash_boot "$B1" "\\[crash\\] synced /fat" "$DELAY" "fsbench --dir /fat --crash-loop --size $SIZE"

il_assert_grep "$B1" "\\[crash\\] synced /fat/crash[.]txt \\($SIZE bytes\\)" \
    "boot 1: write+fsync returned success before the machine died"
il_assert_grep "$B1" "\\[fat32\\]" "boot 1: FAT32 was the filesystem under test"

# ===== BOOT 2: mount without repair; the medium holds a whole file ===
B2="$IL_LOGDIR/fs_crash.b2.log"
il_boot_read "$B2" "stat /fat/crash.txt" "crashbytes /fat/crash.txt $SIZE"

il_assert_grep "$B2" "\\[vfs\\] mounted '/fat'" \
    "(b) /fat mounts after the power cut"
il_assert_no_grep "$B2" "\\[fat32\\] repair" \
    "(b) /fat needed NO repair pass (it declared EXACT and means it)"
il_assert_no_grep "$B2" "\\[fat32\\] corrupt\\|\\[fat32\\] bad" \
    "(b) /fat came back readable, not corrupt"

B2_SIZE=$(crash_size "$B2")
# il_pass/il_fail do not touch IL_ASSERT_COUNT (lib.sh says so in
# il_assert_count's own comment); a manual verdict has to bump it or the
# case reports "12/10 assertions passed".
if [ -z "$B2_SIZE" ]; then
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    il_pass "(a) the file is absent -- an allowed outcome for an unsynced write"
else
    echo "[crash] boot 2: surviving size ${B2_SIZE} (legal: $S1 $S2 $S3)"
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    case "$B2_SIZE" in
        "$S1"|"$S2"|"$S3")
            il_pass "(a) the surviving size ${B2_SIZE} is a whole payload multiple -- nothing was torn" ;;
        *)
            il_fail "(a) the surviving size ${B2_SIZE} is no whole write's size: the file was TORN" ;;
    esac

    # Size alone can be right while the bytes are wrong (an allocation that
    # reused somebody else's cluster keeps the length).  crashbytes walks the
    # file and checks every byte against the payload's arithmetic.
    il_assert_grep "$B2" "crashbytes /fat/crash[.]txt: [0-9]+ bytes, whole=1" \
        "(a) every byte that survived matches the payload it was written from"
fi

# ===== BOOT 3: the contract's own promise ============================
#
# fsync() returned success => the bytes are on the medium.  Kill at 0 ms.
# FAT32 is EXACT, so this MUST pass; a WEAK filesystem would be within its
# declared rights to fail here, which is why the receipt names it.
B3="$IL_LOGDIR/fs_crash.b3.log"
il_crash_boot "$B3" "\\[crash\\] synced /fat" 0 \
    "fsbench --dir /fat --crash-loop --size $SIZE"
il_assert_grep "$B3" "\\[crash\\] synced /fat/crash[.]txt" \
    "boot 3: fsync() returned success before an immediate SIGKILL"

B4="$IL_LOGDIR/fs_crash.b4.log"
il_boot_read "$B4" "stat /fat/crash.txt" "crashbytes /fat/crash.txt $SIZE"

il_assert_grep "$B4" "Type: +regular file" \
    "the file that fsync() reported durable EXISTED on the next boot"
B4_SIZE=$(crash_size "$B4")
if [ -n "$B4_SIZE" ]; then
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    case "$B4_SIZE" in
        "$S1"|"$S2"|"$S3")
            il_pass "and it is at least the synced $S1 bytes (saw ${B4_SIZE})" ;;
        *)
            il_fail "the synced file came back with an impossible size ${B4_SIZE}" ;;
    esac
fi
if [ -z "$B4_SIZE" ]; then
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    il_fail "the file fsync() reported durable is ABSENT on the next boot"
    # The decisive one: the bytes that were synced are the bytes that come
    # back.  This is what "fsync returned 0" has to mean.
    il_assert_grep "$B4" "crashbytes /fat/crash[.]txt: [0-9]+ bytes, whole=1" \
        "the synced DATA survived WHOLE and correct, not truncated at the kill"
fi

# ===== the declaration is visible where it matters ===================
il_assert_grep "$B4" "\\[vfs\\] mounted '/fat'" \
    "the EXACT filesystem mounts silently -- so a WEAK warning means something"

# ===== a WEAK filesystem DECLARES, and still never tears a file =======
#
# /data is btrfs (M1 put the persistent volume there).  btrfs keeps its tree
# root in RAM, so it is allowed to LOSE a recent write -- and its mount
# receipt must say so before a user relies on it.  What it is never allowed
# to do, WEAK or not, is hand back a half-written file: criterion (a) is
# about corruption, not about recency.
#
# /data is the THIRD AHCI slot, so this needs two more disks on blk0/blk1
# (a second FAT32 volume and an ext2 one) for datavol to reach blk2.  The
# disks are fresh on purpose: a freshly formatted btrfs still has to make
# its declaration.
if [ "${IL_CRASH_BTRFS:-1}" = "1" ]; then
    # THREE disks: FAT32 takes blk0, ext2 blk1, and /data is blk2 -- the
    # same ladder `make run` gives a user (M1's durability.sh boots it too),
    # so the receipt being asserted is the one a user actually sees.
    D2="$IL_LOGDIR/fs_crash.d1.img"
    D3="$IL_LOGDIR/fs_crash.d2.img"
    D4="$IL_LOGDIR/fs_crash.d3.img"
    rm -f "$D2" "$D3" "$D4"
    truncate -s 64M "$D2"; truncate -s 64M "$D3"; truncate -s 64M "$D4"

    B6="$IL_LOGDIR/fs_crash.b6.log"
    il_send "exit"
    il_run_qemu "$B6" 140 \
        -drive "id=d0,file=$D2,format=raw,if=none" -device ahci,id=ahci0 \
        -device ide-hd,drive=d0,bus=ahci0.0 \
        -drive "file=$D3,format=raw,if=none,id=d1" \
        -device ide-hd,drive=d1,bus=ahci0.1 \
        -drive "file=$D4,format=raw,if=none,id=d2" \
        -device ide-hd,drive=d2,bus=ahci0.2 \
        -fw_cfg name=opt/auralite.fsformat,string=1
    set +e

    il_assert_grep "$B6" "\\[data\\] PASS: btrfs.*mounted at /data" \
        "/data really is the btrfs persistent volume on this three-disk boot"

    il_assert_grep "$B6" "\\[vfs\\] mounted '/data' -- DURABILITY WEAK" \
        "the WEAK filesystem says so IN ITS MOUNT RECEIPT, before anyone relies on it"
    il_assert_grep "$B6" "call sync before relying on it" \
        "and the receipt tells the reader what to do about it"
    il_assert_no_grep "$B6" "\\[vfs\\] mounted '/fat' -- DURABILITY" \
        "while the EXACT filesystem on the same boot stays silent"
else
    echo "[crash] btrfs lane skipped (IL_CRASH_BTRFS=0)"
fi

set +e
il_summary
