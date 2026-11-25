#!/usr/bin/env bash
# tests/integration/cases/test_dir_scale.sh — FSMODERN M5 directory scaling gate.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# shellcheck source=../lib/lib.sh
source "$ROOT_DIR/tests/integration/lib/lib.sh"
il_init
il_have qemu-system-x86_64
IL_SELFTEST=fast

LOG="$ROOT_DIR/build/integration-logs/dir_scale.log"
mkdir -p "$(dirname "$LOG")"
DATA_IMG="$ROOT_DIR/build/integration-logs/dir_scale_data.img"
rm -f "$DATA_IMG"
truncate -s 256M "$DATA_IMG"

# The default mount recipe reserves disks 0/1 for exchange FS; /data is disk 2.
DISKS=(-device "ahci,id=ahci0" -fw_cfg "name=opt/auralite.fsformat,string=1")
for i in 0 1; do
    img="$IL_LOGDIR/dir_scale_exchange_$i.img"
    rm -f "$img"; truncate -s 16M "$img"
    DISKS+=(-drive "file=$img,if=none,id=exchange$i,format=raw"
            -device "ide-hd,drive=exchange$i,bus=ahci0.$i")
done
DISKS+=(-drive "file=$DATA_IMG,if=none,id=data0,format=raw"
        -device "ide-hd,drive=data0,bus=ahci0.2")

il_send "fsbench --scale --dir /data --files 1024 --size 512"
il_send_wait "[shell] '/bin/fsbench'" 170
il_send "fsync /data"
il_send "exit"
il_run_qemu "$LOG" 180 "${DISKS[@]}"

il_assert_grep "$LOG" "\[fsbench\] scale: dir=\S+ sizes=64/256/1024 size=512 clock=tsc" \
    "the run asked for three directories of the intended sizes"
il_assert_grep "$LOG" "\[fsbench\] scale: n=64 lookups=[0-9]+ passes=[0-9]+ -> [0-9]+\.[0-9]+ ops/s" \
    "the 64-name directory was looked up"
il_assert_grep "$LOG" "\[fsbench\] scale: n=256 lookups=[0-9]+ passes=[0-9]+ -> [0-9]+\.[0-9]+ ops/s" \
    "the 256-name directory was looked up"
il_assert_grep "$LOG" "\[fsbench\] scale: n=1024 lookups=[0-9]+ passes=[0-9]+ -> [0-9]+\.[0-9]+ ops/s" \
    "the 1024-name directory was looked up"
il_assert_grep "$LOG" "\[fsbench\] scale: ratio small/large = [0-9]+\.[0-9]+x for 16x the names" \
    "the ratio line is printed (16x the names between the two sizes)"
il_assert_grep "$LOG" "\[fsbench\] scale: PASS" \
    "the scale run completed"

RATIO=$(grep -E "\[fsbench\] scale: ratio small/large =" "$LOG" | sed -E 's/.*= ([0-9]+\.[0-9]+)x.*/\1/' | tail -1) || RATIO=999
if awk -v r="${RATIO:-999}" 'BEGIN { exit (r < 3.0) ? 0 : 1 }'; then
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    il_pass "16x the names cost ${RATIO}x the lookup (measured warm lookup ratio)"
else
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    il_fail "16x the names cost ${RATIO:-?}x the lookup (expected < 3.0x)"
fi

il_assert_grep "$LOG" "\[btrfs\] fsync: persisted tree root .* \(depth=[0-9]+ leaves=[1-9][0-9]+ items=" \
    "the tree really has many leaves (measured geometry, not the compiled constant)"

if grep -qE "^\[fsbench\].*FAIL" "$LOG"; then
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    il_fail "fsbench reported FAIL"
else
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    il_pass "nothing failed (no FAIL line from the tool)"
fi

il_summary
