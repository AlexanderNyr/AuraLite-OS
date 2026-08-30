#!/usr/bin/env bash
# test_selfhost_closure.sh -- SELFHOST_PLAN.md SH8: bootstrap closure, slow-shard gate.
#
# The terminal gate for Stage 2 (D1): the toolchain closes on itself -- the
# seed tcc0 (/bin/tcc) builds tcc1, tcc1 builds tcc2 (each hashed with the SH7a
# twin), the SH3/SH4 linkers and the host-visible generators rebuild in-guest,
# and then the full assembly loop (kernel from source -> initrd -> hybrid ISO)
# runs TWICE from a clean /fat with no host tool in the loop.  The host only
# boots QEMU, gives it a persistent AHCI /fat, and drives the serial; it never
# compiles, links, assembles or packs anything.
#
# The guest driver (tools/selfhost/sh8_closure.sh) is staged under /tests and
# this case simply runs it and greps the §8 receipt:
#     [selfhost] FULL LOOP PASS (2/2 clean loops)
# plus the per-stage receipts (tcc1/tcc2 built, loop 1 / loop 2 PASS) and a
# guard that no host-tool line was ever launched.
#
# Slow-shard by design: the closure compiles the full x86_64 kernel twice plus
# a two-stage tcc.  The plan routes it to the CI slow shard (SH9 wiring); here
# it is registered in the selfhost shard and skipped loudly when the guest tcc
# toolchain is absent.
set -u
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$(dirname "$0")/.."
. lib/lib.sh

# The closure needs the guest tcc (the seed tcc0).  A missing bootstrap is a
# loud skip, never a hidden fetch from an integration test.
if [ ! -f "$ROOT/build/selfhost/tcc.elf" ]; then
    echo "${C_YELLOW}[selfhost] guest tcc not built -- skipping SH8 closure (run 'make selfhost-deps selfhost-tcc' and rebuild the ISO)${C_RESET}"
    il_skip "guest tcc absent from build/selfhost (selfhost-deps not run)"
    exit 0
fi

# Always refresh the bootstrap image: the SH8 source closure (kernel/drivers/
# libc/tcc/src) is an initrd prerequisite, so this prevents a stale ISO from
# claiming an in-guest compile while shipping yesterday's sources.
PREP_LOG="$ROOT/build/selfhost/sh8-bootstrap-image.log"
mkdir -p "$(dirname "$PREP_LOG")"
if ! (cd "$ROOT" && make iso) >"$PREP_LOG" 2>&1; then
    echo "${C_RED}[selfhost] SH8 bootstrap ISO build failed${C_RESET}"
    tail -30 "$PREP_LOG" | sed 's/^/    /'
    exit 2
fi

il_init
il_have qemu-system-x86_64 || exit 2

il_section "self-host bootstrap closure (SH8): toolchain closes, loop runs twice"

# /fat must hold the two tcc stages, the linkers, the generated kernel, the
# packed initrd and the ~48 MiB hybrid image, so pre-format a real 64 MiB FAT32
# volume on the host (the guest's stock 4 MiB superfloppy cannot).  Same BPB /
# FSInfo / FAT shape the guest's parse_or_format accepts (see test_selfhost_iso.sh).
DISK="$IL_BUILD/selfhost-sh8-fat.img"
rm -f "$DISK"
mkdir -p "$(dirname "$DISK")"
dd if=/dev/zero of="$DISK" bs=512 count=131000 status=none
python3 - "$DISK" <<'PY'
import sys, struct
path = sys.argv[1]
bs = 512
BASE = 64; T = 131000; RES = 32; NF = 2
fat = ((T - 1) // 128) + 1
clusters = T - (RES + NF * fat)
def w16(v): return struct.pack('<H', v)
def w32(v): return struct.pack('<I', v)
disk = bytearray((BASE + T) * bs)
mb = bytearray(bs)
mb[510] = 0x55; mb[511] = 0xAA
mb[446 + 0] = 0x80; mb[446 + 4] = 0x0C
struct.pack_into('<I', mb, 446 + 8, BASE)
struct.pack_into('<I', mb, 446 + 12, T)
disk[0:bs] = mb
def put(lba, data): disk[lba*bs:(lba+1)*bs] = data
bpb = bytearray(bs)
bpb[0] = 0xEB; bpb[1] = 0x58; bpb[2] = 0x90
bpb[3:11] = b'AURALITE'
bpb[11:13] = w16(512); bpb[13] = 1
bpb[14:16] = w16(RES); bpb[16] = NF
bpb[21] = 0xF8; bpb[24:26] = w16(63); bpb[26:28] = w16(255)
bpb[28:32] = w32(BASE); bpb[32:36] = w32(T)
bpb[36:40] = w32(fat); bpb[44:48] = w32(2)
bpb[48:50] = w16(1); bpb[50:52] = w16(6)
bpb[64] = 0x80; bpb[66] = 0x29; bpb[67:71] = w32(0xA2026022)
bpb[71:82] = b'AURALITE   '; bpb[82:90] = b'FAT32   '
bpb[510] = 0x55; bpb[511] = 0xAA
put(BASE, bpb); put(BASE + 6, bpb)
fs = bytearray(bs)
fs[0:4] = w32(0x41615252); fs[484:488] = w32(0x61417272)
fs[488:492] = w32(clusters - 1); fs[492:496] = w32(3); fs[508:512] = w32(0xAA550000)
put(BASE + 1, fs); put(BASE + 7, fs)
for j in range(NF):
    fla = bytearray(bs)
    fla[0:4] = w32(0x0FFFFFF8); fla[4:8] = w32(0x0FFFFFFF); fla[8:12] = w32(0x0FFFFFFF)
    put(BASE + RES + j * fat, fla)
open(path, "wb").write(disk)
PY

AHCI=(
    -drive "file=$DISK,format=raw,if=none,id=sh8disk"
    -device "ahci,id=ahci0"
    -device "ide-hd,drive=sh8disk,bus=ahci0.0"
)

LOG="$IL_LOGDIR/selfhost_closure.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# Long-running: the closure compiles the whole x86_64 kernel twice plus the
# two-stage tcc, so give it a generous budget and keep the guest alive after.
il_send_delay 8
il_send "sh /tests/sh8_closure.sh"
il_send_delay 30
il_send "echo SH8_STILL_ALIVE"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 1400 "${AHCI[@]}"

# ---- the §8 receipt, only after both loops are clean ----------------------
il_assert_grep "$LOG" "\[selfhost\] FULL LOOP PASS (2/2 clean loops)" \
    "the closure ran both loops cleanly (the §8 receipt)"
il_assert_grep "$LOG" "\[selfhost\] sh8: worktree staged" \
    "the driver set up its worktree"
il_assert_grep "$LOG" "\[selfhost\] sh8: tool chain rebuilt (aulink + mini-asm from tcc0)" \
    "the SH3/SH4 linkers were rebuilt from the seed tcc"
il_assert_grep "$LOG" "\[selfhost\] sh8: tcc1 built in-guest (compiled by tcc0" \
    "tcc1 was compiled by the seed tcc0"
il_assert_grep "$LOG" "\[selfhost\] sh8: tcc2 built in-guest (compiled by tcc1" \
    "tcc2 was compiled by tcc1 (a tcc->tcc->tcc chain)"
il_assert_grep "$LOG" "\[selfhost\] sh8: generators rebuilt" \
    "the host-visible generators rebuilt in-guest"
il_assert_grep "$LOG" "\[selfhost\] sh8: loop 1 PASS" \
    "the first assembly loop produced its ISO"
il_assert_grep "$LOG" "\[selfhost\] sh8: loop 2 PASS" \
    "the second assembly loop produced its ISO"

# ---- each stage compiled a real artifact ----------------------------------
il_assert_grep "$LOG" "\[selfhost\] sh8: kernel assembled in-guest" \
    "the generated kernel build compiled all kernel sources and linked them"
il_assert_grep "$LOG" "\[selfhost\] mkiso PASS: auralite.iso written in-guest" \
    "both loops spliced a hybrid ISO on /fat"
il_assert_grep "$LOG" "sha256" \
    "the chain and loop artifacts were hashed (SH7a twin)"
il_assert_count "$LOG" "\[selfhost\] mkiso PASS:.*written in-guest" 2 \
    "the ISO was written in BOTH loops"

# ---- nothing used a host compiler/linker/assembler ------------------------
il_assert_no_grep "$LOG" "tcc: error|aulink: [1-9][0-9]* error|undefined reference|cannot open object directory" \
    "no in-guest build error"
il_assert_no_grep "$LOG" "UNREACHABLE" \
    "no closure branch that should be unreachable was taken"

# ---- the guest survives all of it -----------------------------------------
il_assert_grep "$LOG" "^SH8_STILL_ALIVE$" \
    "the shell still takes commands after the closure"

il_summary
