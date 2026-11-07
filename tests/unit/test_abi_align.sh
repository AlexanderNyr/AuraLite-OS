#!/usr/bin/env bash
# test_abi_align.sh — RESIDUE3 C2 gate: every assembly entry point into C
# reaches the call with RSP % 16 == 0.
#
# Three lanes, and the third one is the point:
#   1. the tree as it stands is green;
#   2. the model's own selftest (three injected defects) is caught;
#   3. a DOCTORED COPY of the real tree is caught.  Lane 2 proves the model
#      can fail on a synthetic blob; lane 3 proves it is actually wired to
#      this tree's files, which is the failure mode the plan's D-class
#      defects keep producing.
set -u
cd "$(dirname "$0")/../.." || exit 2

fail=0
say()  { echo "[abi-align] $*"; }
bad()  { echo "[abi-align] FAIL: $*"; fail=1; }

say "lane 1: the tree"
if python3 tools/check_abi_align.py; then
    say "PASS: every call into C enters at RSP%16 == 0"
else
    bad "the tree violates the C ABI's stack alignment"
fi

say "lane 2: the model's selftest"
if python3 tools/check_abi_align.py --selftest; then
    say "PASS: injected misalignments are caught"
else
    bad "the model cannot detect a misaligned call — the gate is decorative"
fi

say "lane 3: a doctored tree"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/tools" "$tmp/kernel/arch/x86_64" "$tmp/kernel/proc"
cp tools/check_abi_align.py "$tmp/tools/"
# The model resolves ROOT from its own location, so a copy of the tool plus a
# copy of ONE asm file under the same layout is enough to doctor.
sed 's/^    and rsp, -16$/    nop/' \
    kernel/arch/x86_64/isr_stubs.asm > "$tmp/kernel/arch/x86_64/isr_stubs.asm"
: > "$tmp/kernel/proc/context.asm"
if python3 "$tmp/tools/check_abi_align.py" > "$tmp/out.txt" 2>&1; then
    bad "the realign was removed and the gate still passed"
    sed 's/^/    /' "$tmp/out.txt"
elif grep -q "isr_handler" "$tmp/out.txt"; then
    say "PASS: removing isr_common_stub's realign turns the gate red"
else
    bad "the gate failed for the wrong reason:"
    sed 's/^/    /' "$tmp/out.txt"
fi

if [ "$fail" -eq 0 ]; then
    say "ALL LANES PASSED"
fi
exit "$fail"
