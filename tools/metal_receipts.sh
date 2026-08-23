#!/usr/bin/env bash
# tools/metal_receipts.sh — the R11 user-executable metal package.
#
# Default: make sure release/auralite.iso exists and print the
# paste-back protocol (docs/metal_receipts.md is the canonical copy).
#
# --null-test: boot the ISO under QEMU TCG and assert that every
# boot-time receipt line the package asks the user to capture still
# PRINTS — the format rehearsal.  What TCG cannot judge (PCID on,
# counters moving) is asserted in its pcid=0 fallback form: the
# counters exist and read zero.  Runs in CI via
# tests/integration/cases/test_metal_null.sh, which carries the same
# greps through the integration lib's boot harness.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ISO="release/auralite.iso"

ensure_iso() {
    if [ ! -s "$ISO" ]; then
        echo "[metal] building $ISO ..."
        make release >/dev/null || { echo "[metal] FAIL: make release"; exit 1; }
    fi
    echo "[metal] image: $ISO ($(stat -c %s "$ISO") bytes)"
}

if [ "${1:-}" = "--null-test" ]; then
    ensure_iso
    command -v qemu-system-x86_64 >/dev/null 2>&1 || {
        echo "[metal] SKIP: qemu-system-x86_64 not installed"; exit 0; }
    LOG="build/metal_null.log"
    rm -f "$LOG"
    # cat /proc/perf through the shell so the counter NAMES are format-
    # checked too; QEMU output goes to the log only (project rule).
    {
        sleep 25
        printf 'cat /proc/perf\n'; sleep 3
        printf 'exit\n';           sleep 2
    } | timeout --foreground 60 qemu-system-x86_64 \
            -drive "file=$ISO,format=raw,if=ide,snapshot=on" \
            -m 512M -smp 2 -display none -serial stdio -no-reboot \
            -fw_cfg name=opt/auralite.selftest,string=fast \
            > "$LOG" 2>/dev/null || true

    fail=0
    ok()   { echo "  [metal-null] OK   $1"; }
    bad()  { echo "  [metal-null] FAIL $1"; fail=1; }
    pin()  { grep -aq "$1" "$LOG" && ok "$2" || bad "$2"; }

    pin '\[cpu\]   features: pat=[01] pcid=[01] invpcid=[01] erms=[01]' \
        "slot 1: feature ground-truth line"
    pin '\[vmm\] IA32_PAT: PA4=WC'                    "slot 1: PAT readback line"
    pin 'memcpy small-copy crossover'                 "slot 2: ERMSB crossover line"
    pin '\[ioapic\] base 0x.* (MADT agree)'           "slot 7: MADT discovery line (QEMU must agree)"
    pin '^cr3_noflush_switches 0$'                    "slot 6 fallback: counter exists, 0 on pcid=0"
    pin '^pcid_generation_wraps 0$'                   "slot 6 fallback: counter exists, 0 on pcid=0"
    if grep -aq 'features: pat=[01] pcid=1' "$LOG"; then
        # A pcid=1 lane (KVM some day): the enable line must be there.
        pin '\[vmm\] PCID enabled'                    "slot 5: PCID enable line on a pcid=1 lane"
    else
        grep -aq '\[vmm\] PCID enabled' "$LOG" \
            && bad "slot 5: enable line printed on a pcid=0 boot (gate broken)" \
            || ok  "slot 5 fallback: no enable line on pcid=0 (gate holds)"
    fi

    if [ "$fail" -ne 0 ]; then
        echo "[metal] NULL TEST FAILED (see $LOG)"
        exit 1
    fi
    echo "[metal] NULL test PASS: every boot-time receipt line prints under TCG"
    exit 0
fi

ensure_iso
echo
echo "[metal] The paste-back protocol (canonical copy: docs/metal_receipts.md):"
echo
sed -n '/## The receipt slots/,/## The WHPX-targeted/p' docs/metal_receipts.md
echo
echo "[metal] PCID (slots 5+6): run on the WHPX machine whose log said pcid=1 —"
echo "        boot, use it a minute, 'cat /proc/perf', paste the counters."
echo "[metal] Rehearse locally first: tools/metal_receipts.sh --null-test"
