#!/usr/bin/env bash
# tools/repro_smp_chk.sh — FIX_R2 reproduction harness.
#
# "Reproduce it deliberately: run the affected cases under -smp 2 in a loop
#  and record the failure rate."
#
# Boots build/auralite.iso under `-smp 2` repeatedly, drives the affected
# workload over serial (named pipes, prompt-synchronized — works under both
# KVM and slow TCG), and classifies every iteration as:
#
#   CLEAN             — workload completed, no protector trip, no exception
#   STACK-CHK         — "[security] STACK CORRUPTION DETECTED" seen
#   DOUBLE-FAULT      — FIX_R1 #DF diagnostic seen (kernel stack overflow
#                       that used to surface as a canary trip pre-R1)
#   GUARD-KERNEL      — "[GUARD] kernel stack overflow" seen
#   KERNEL-EXC        — any other "[diag] === KERNEL EXCEPTION" line
#   FAIL/OTHER        — selftest reported failures / hung / no marker
#
# Usage:
#   tools/repro_smp_chk.sh [iterations] [case]
#     case = selftest (default; the case that historically tripped)
#          | gltest   (373-check GL/rasteriser suite — the sensitive case:
#                     fails intermittently ONLY under -smp 2)
#          | shell    (boot to an idle shell — baseline)
#
# SMP=<n> (default 2) overrides the QEMU CPU count — `SMP=1` is the control
# group for the SMP-only gltest flakiness documented in TODO.md (FIX_R2).
#
# Results are appended to build/r2_repro/results.tsv; logs per iteration go
# to build/r2_repro/iter-<case>-<N>.log.  A N:=0 measurement honestly means
# "zero trips in N boots", printed as such — that IS the measured rate.

set -u

ITERS="${1:-10}"
CASE="${2:-selftest}"
ISO="${ISO:-build/auralite.iso}"
QEMU="${QEMU:-qemu-system-x86_64}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-240}"
TEST_TIMEOUT="${TEST_TIMEOUT:-240}"
GRACE="${GRACE:-3}"

OUTDIR=build/r2_repro
mkdir -p "$OUTDIR"
TSV="$OUTDIR/results.tsv"

if [ ! -f "$ISO" ]; then
    echo "[repro] missing $ISO — build it first (make iso)" >&2
    exit 2
fi

[ -f "$TSV" ] || printf 'case\titer\tresult\tcpu\tclass\textra\n' > "$TSV"

clean=0; chk=0; df=0; guardk=0; kexc=0; fail=0

classify() {  # $1=log
    local log="$1"
    if grep -qE "STACK CORRUPTION DETECTED" "$log"; then
        local cpu cls
        cpu=$(grep -oE "STACK CORRUPTION DETECTED on cpu#[0-9]+" "$log" | head -1 | grep -oE "[0-9]+")
        cls=$(grep -m1 -oE "class=[A-Z-]+" "$log" | head -1 | cut -d= -f2)
        echo "STACK-CHK	${cpu:-?}	${cls:-?}	$(grep -m1 -oE 'fault-detected-at-rip=0x[0-9a-f]+' "$log" | cut -d= -f2)"
    elif grep -qE "KERNEL EXCEPTION cpu#[0-9]+: Double Fault" "$log"; then
        echo "DOUBLE-FAULT	$(grep -m1 -oE 'cpu#[0-9]+' "$log" | tr -d 'cpu#')	-	-"
    elif grep -qE "\[GUARD\] kernel stack overflow" "$log"; then
        echo "GUARD-KERNEL	-	-	-"
    elif grep -qE "\[diag\] === KERNEL EXCEPTION" "$log"; then
        echo "KERNEL-EXC	-	-	$(grep -m1 -oE 'KERNEL EXCEPTION cpu#[0-9]+: [A-Za-z ]+' "$log")"
    elif grep -qE "SELFTEST [0-9]+ FAILURES" "$log"; then
        echo "FAIL	-	-	selftest failures"
    elif grep -qE "\[gl\] SUMMARY [0-9]+ checks, [1-9][0-9]* failed" "$log"; then
        # A completed gltest with failed checks: this is the SMP-only
        # user-visible corruption the R2 diagnosis root-caused to the
        # missing FPU/SSE context switch (see TODO.md).  Record WHICH
        # checks failed -- reruns fail different, random checks.
        echo "FAIL	-	-	gl-failed: $(grep -oE '\[gl\] FAIL [A-Za-z0-9_]+' "$log" | sed 's/\[gl\] FAIL //' | paste -sd, -)"
    elif grep -qE "SELFTEST ALL PASS|\[gl\] ALL TESTS PASSED" "$log"; then
        echo "CLEAN	-	-	-"
    else
        echo "FAIL	-	-	no completion marker (hang?)"
    fi
}

i=1
while [ "$i" -le "$ITERS" ]; do
    log="$OUTDIR/iter-${CASE}-${i}.log"
    base="$OUTDIR/ser-${CASE}-${i}"
    rm -f "$base.in" "$base.out" "$log"
    mkfifo "$base.in" "$base.out"

    # shellcheck disable=SC2064
    timeout --foreground "$((BOOT_TIMEOUT + TEST_TIMEOUT + 30))" "$QEMU" \
        -drive "file=$ISO,format=raw,if=ide,snapshot=on" \
        -boot order=c -m 512M -smp "${SMP:-2}" -vga std -display none \
        -serial "pipe:$base" -no-reboot -cpu qemu64 \
        -netdev user,id=net0 -device e1000,netdev=net0 \
        >/dev/null 2>&1 &
    qpid=$!

    : > "$log"; cat "$base.out" >> "$log" &
    catpid=$!
    exec 3<>"$base.in"    # r/w open never blocks; keeps qemu's reader fed

    # 1. wait for the shell prompt
    t=0; booted=""
    while [ "$t" -lt "$BOOT_TIMEOUT" ]; do
        grep -q "auralite#" "$log" && { booted=1; break; }
        grep -qE "STACK CORRUPTION|KERNEL EXCEPTION" "$log" && break
        sleep 1; t=$((t+1))
    done

    # 2. drive the workload (if the boot didn't already die)
    if [ -n "$booted" ]; then
        [ "$CASE" = "selftest" ] && printf 'run selftest\n' >&3
        [ "$CASE" = "gltest" ] && printf 'run gltest\n' >&3
        t=0
        while [ "$t" -lt "$TEST_TIMEOUT" ]; do
            # \[gl\] SUMMARY also terminates gltest: with failures the suite
            # exits non-pass, and waiting the full timeout per iteration
            # would just waste wall-clock.
            grep -qE "SELFTEST ALL PASS|SELFTEST [0-9]+ FAILURES|\[gl\] ALL TESTS PASSED|\[gl\] SUMMARY|STACK CORRUPTION|KERNEL EXCEPTION" "$log" && break
            [ "$CASE" = "shell" ] && break
            sleep 1; t=$((t+1))
        done
        sleep "$GRACE"
    fi

    kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
    exec 3>&- 3<&-
    kill "$catpid" 2>/dev/null; wait "$catpid" 2>/dev/null
    rm -f "$base.in" "$base.out"

    # 3. classify
    res=$(classify "$log")
    printf '%s\t%s\t%s\n' "$CASE" "$i" "$res" >> "$TSV"
    verdict=$(printf '%s' "$res" | cut -f1)
    echo "[repro] iter $i/$ITERS ($CASE): $verdict ${res#*	}"
    case "$verdict" in
        CLEAN)        clean=$((clean+1));;
        STACK-CHK)    chk=$((chk+1));;
        DOUBLE-FAULT) df=$((df+1));;
        GUARD-KERNEL) guardk=$((guardk+1));;
        KERNEL-EXC)   kexc=$((kexc+1));;
        *)            fail=$((fail+1));;
    esac
    i=$((i+1))
done

total=$((clean+chk+df+guardk+kexc+fail))
bad=$((chk+df+guardk+kexc))
echo "=============================================================="
echo "[repro] $CASE: $total iteration(s) under -smp ${SMP:-2}"
echo "[repro]   CLEAN=$clean STACK-CHK=$chk DOUBLE-FAULT=$df GUARD-KERNEL=$guardk KERNEL-EXC=$kexc FAIL/OTHER=$fail"
echo "[repro]   measured trip rate: $bad / $total"
echo "[repro]   per-iteration logs: $OUTDIR/iter-${CASE}-*.log; summary: $TSV"
[ "$bad" -gt 0 ] && exit 1 || exit 0
