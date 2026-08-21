#!/usr/bin/env bash
# tests/integration/a64_parity_smoke.sh -- ARM64_PLAN phase A8.
#
# The parity boot: EVERY gate from EVERY phase green in ONE boot --
# rv_parity_smoke's shape at the fourth arch.  One QEMU run with the
# full device set, then one assert per phase gate:
#
#   A0 boot / A1 boot_info+DTB / A2 isr+timer+gic / A3 pmm+vmm+heap
#   (both alignment polarities measured) / A4 sched+fpu+EL0 / A5
#   init+shell+spawn / A7 blk+net+uart-irq -- PLUS the fourth
#   tenant's side of the refusal matrix: this kernel exec-refuses all
#   THREE foreign tenants in the same session (x86_64 by machine 62,
#   i386 by ELF class, riscv64 by machine 243) -- and the standing
#   x86_64 no-regression pair, because the four-tenant initrd must
#   not break the first tenant's boot.
#
# Keeps the a64 disciplines: log-size fuse first (the A5c
# prompt-flood lesson), PSCI ending, no-FAIL sweep.
#
# Skips cleanly when qemu-system-aarch64 is absent.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
IMG="$BUILD/kernela64.img"
TAR="$BUILD/initrd.tar"
DISK="$BUILD/a64_parity_disk.img"
LOG="$BUILD/a64_parity.log"
LOG64="$BUILD/a64_parity_x64.log"

if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "[a64-parity] SKIP: qemu-system-aarch64 not installed" >&2
    exit 0
fi
if ! command -v llvm-objcopy >/dev/null 2>&1; then
    echo "[a64-parity] SKIP: llvm-objcopy not installed" >&2
    exit 0
fi

[ -s "$IMG" ] || make -C "$ROOT" kernela64-img >/dev/null
[ -s "$TAR" ] || { echo "[a64-parity] SKIP: build/initrd.tar absent (run 'make iso' first)" >&2; exit 0; }

python3 - "$DISK" << 'PYEOF'
import sys
disk = bytearray(4 * 1024 * 1024)
disk[0:4] = b"Aura"
disk[4:32] = b"Lite a64 test disk pattern.."
disk[510] = 0x55; disk[511] = 0xAA
open(sys.argv[1], "wb").write(disk)
PYEOF

fail=0
assert_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        printf '  [a64-parity] OK   %s\n' "$desc"
    else
        printf '  [a64-parity] FAIL %s\n' "$desc"
        fail=1
    fi
}
assert_no_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        printf '  [a64-parity] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [a64-parity] OK   %s\n' "$desc"
    fi
}

# ---- the one boot: everything attached, the refusal matrix in-session ----
rm -f "$LOG"
{
    sleep 6
    printf 'uname\n';           sleep 1
    printf 'run bin/init\n';    sleep 1
    printf 'run bin32/init32\n'; sleep 1
    printf 'run binrv/init\n';  sleep 1
    printf 'exit\n';            sleep 2
} | timeout 60 qemu-system-aarch64 -machine virt -cpu cortex-a72 \
        -m 256M -display none -serial stdio -no-reboot \
        -kernel "$IMG" -initrd "$TAR" \
        -global virtio-mmio.force-legacy=true \
        -drive file="$DISK",format=raw,if=none,id=hd \
        -device virtio-blk-device,drive=hd \
        -netdev user,id=n0 -device virtio-net-device,netdev=n0 \
        > "$LOG" 2>/dev/null || true

tr -d '\r' < "$LOG" > "$LOG.clean" && mv "$LOG.clean" "$LOG"

# ---- the fuse FIRST (the A5c discipline, kept forever) ----
LOGSIZE=$(wc -c < "$LOG")
if [ "$LOGSIZE" -gt 200000 ]; then
    printf '  [a64-parity] FAIL log-size fuse blown (%s bytes)\n' "$LOGSIZE"
    echo "[a64-parity] FAILED (fuse)"
    exit 1
fi
printf '  [a64-parity] OK   log-size fuse intact (%s bytes)\n' "$LOGSIZE"

# ---- one assert per phase gate ----
assert_grep "Hello from AuraLite OS kernel (aarch64)!"     "A0: boot banner"
assert_grep "handoff magic OK, path=PSCI"                  "A1: boot_info from the DTB (shared walker)"
assert_grep "\[isr\]  PASS: undefined instruction named"   "A2: exception round-trip"
assert_grep "PASS: claim/complete round-trip"              "A2: timer through the GIC"
assert_grep "\[pmm\]  PASS"                                "A3: frame allocator"
assert_grep "\[vmm\]  PASS: positive map cycle"            "A3: positive path + 3 fault probes"
assert_grep "SUCCEEDS on Normal WB (both polarities"       "A3: alignment measured both ways"
assert_grep "\[heap\] PASS"                                "A3: alloc/free cycles"
assert_grep "\[sched\] PASS"                               "A4: forced preemption"
assert_grep "\[fpu\]  PASS"                                "A4: q8/q9 across clobbering switches"
assert_grep "A64-U-OK!"                                    "A4: EL0 write round-trip"
assert_grep "\[user\] PASS"                                "A4: EL0 self-test + contained fault"
assert_grep "\[init\] PASS: inita64 ran and exited 7"      "A5: compiled init exits 7"
assert_grep "auralite# "                                   "A5: the shared shell prompt"
assert_grep "AuraLite OS aarch64 (TTBR1 higher half"       "A5: uname names the tenant"
assert_grep "\[blk\]  PASS"                                "A7: storage gate"
assert_grep "\[net\]  PASS"                                "A7: lease + ARP + verified echo"
assert_grep "rx bytes via GIC irq: [1-9]"                  "A7: interrupt-driven console receipt"

# ---- the refusal matrix, this kernel's row: all THREE foreigners ----
assert_grep "refused: machine 62 (x86_64"                  "A8: /bin refused BY NAME (x86_64, machine 62)"
assert_grep "refused: not ELFCLASS64"                      "A8: /bin32 refused BY CLASS (the i386 tenant)"
assert_grep "refused: machine 243 (riscv64"                "A8: /binrv refused BY NAME (riscv64, machine 243)"

# ---- the healthy ending ----
assert_grep "A7 complete; console+shell+blk+net online"    "kernel reached the end with every gate up"
assert_no_grep "FAIL"                                      "no gate failed anywhere in the boot"
assert_no_grep "UNHANDLED"                                 "no unhandled trap anywhere in the boot"

# ---- the standing x86_64 no-regression pair ----
ISO="$BUILD/auralite.iso"
if command -v qemu-system-x86_64 >/dev/null 2>&1 && [ -s "$ISO" ]; then
    rm -f "$LOG64"
    timeout 35 qemu-system-x86_64 \
            -drive format=raw,file="$ISO",if=ide,snapshot=on \
            -m 512M -display none -serial file:"$LOG64" -no-reboot \
            >/dev/null 2>&1 || true
    if grep -qa "Hello from AuraLite OS kernel!" "$LOG64"; then
        printf '  [a64-parity] OK   x86_64: banner unchanged with the four-tenant tar\n'
    else
        printf '  [a64-parity] FAIL x86_64: did not boot with the four-tenant tar\n'
        fail=1
    fi
else
    printf '  [a64-parity] SKIP x86_64 pair (no qemu-system-x86_64 or no ISO)\n'
fi

if [ "$fail" -eq 0 ]; then
    echo "[a64-parity] all A8 assertions passed"
else
    echo "[a64-parity] FAILED"
fi
exit "$fail"
