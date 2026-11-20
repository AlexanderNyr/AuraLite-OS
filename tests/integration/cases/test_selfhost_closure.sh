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
il_have qemu-system-x86_64 mformat mcopy || exit 2
. lib/selfhost_disk.sh

il_section "self-host bootstrap closure (SH8): toolchain closes, loop runs twice"

# Each loop links against the same prebuilt Rust input, outside the initrd.
# Leave room for that input, the guest-produced ELF and the ~48 MiB ISO.
DISK="$IL_BUILD/selfhost-sh8-fat.img"
selfhost_make_disk "$DISK" "$ROOT" || exit 2

AHCI=(
    -drive "file=$DISK,format=raw,if=none,id=sh8disk"
    -device "ahci,id=ahci0"
    -device "ide-hd,drive=sh8disk,bus=ahci0.0"
)

LOG="$IL_LOGDIR/selfhost_closure.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# Do not feed commands while the foreground closure owns the shell.
il_send_prompt "sh /tests/sh8_closure.sh"
il_send_prompt "echo SH8_STILL_ALIVE"
il_send_prompt "exit"
il_run_qemu_prompt "$LOG" 1400 "${AHCI[@]}"

# ---- the §8 receipt, only after both loops are clean ----------------------
il_assert_grep "$LOG" "\[selfhost\] FULL LOOP PASS \(2/2 clean loops\)" \
    "the closure ran both loops cleanly (the §8 receipt)"
il_assert_grep "$LOG" "\[selfhost\] sh8: worktree staged" \
    "the driver set up its worktree"
il_assert_grep "$LOG" "\[selfhost\] sh8: tool chain rebuilt \(aulink \+ mini-asm from tcc0\)" \
    "the SH3/SH4 linkers were rebuilt from the seed tcc"
il_assert_grep "$LOG" "\[selfhost\] sh8: tcc1 built in-guest \(compiled by tcc0" \
    "tcc1 was compiled by the seed tcc0"
il_assert_grep "$LOG" "\[selfhost\] sh8: tcc2 built in-guest \(compiled by tcc1" \
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
il_assert_grep "$LOG" "\[selfhost\] mkiso PASS: .*written in-guest" \
    "both loops spliced a hybrid ISO on /fat"
il_assert_grep "$LOG" "sha256" \
    "the chain and loop artifacts were hashed (SH7a twin)"
il_assert_count "$LOG" "\[selfhost\] mkiso PASS:.*written in-guest" 2 \
    "the ISO was written in BOTH loops"

# ---- no failures in the in-guest C/asm build or final link ---------------
il_assert_no_grep "$LOG" "tcc: error|aulink: [1-9][0-9]* error|undefined reference|cannot open object directory" \
    "no in-guest build error"
il_assert_no_grep "$LOG" "UNREACHABLE" \
    "no closure branch that should be unreachable was taken"

# ---- the guest survives all of it -----------------------------------------
il_assert_grep "$LOG" "^SH8_STILL_ALIVE$" \
    "the shell still takes commands after the closure"

il_summary
