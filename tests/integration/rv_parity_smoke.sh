#!/usr/bin/env bash
# tests/integration/rv_parity_smoke.sh -- RISCV_PLAN phase V8.
#
# The parity boot: EVERY gate from EVERY phase green in ONE boot --
# i386_parity_smoke.sh's shape at the third arch.  One QEMU run with
# the full device set, then one assert per phase gate:
#
#   V0 boot / V1 boot_info / V2 isr+timer+plic / V3 pmm+vmm+heap (W^X
#   fault probes included) / V4 sched+user / V5 init+shell / V7
#   blk+net+uart-irq -- and the standing x86 no-regression pair, which
#   after V8's three-tenant initrd matters doubly: the fatter tar must
#   not break either x86 boot.
#
# Skips cleanly when qemu-system-riscv64 is absent.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ELF="$BUILD/kernelrv.elf"
TAR="$BUILD/initrd.tar"
DISK="$BUILD/rv_parity_disk.img"
LOG="$BUILD/rv_parity.log"
LOG64="$BUILD/rv_parity_x64.log"

if ! command -v qemu-system-riscv64 >/dev/null 2>&1; then
    echo "[rv-parity] SKIP: qemu-system-riscv64 not installed" >&2
    exit 0
fi

[ -s "$ELF" ] || make -C "$ROOT" kernelrv >/dev/null
[ -s "$TAR" ] || make -C "$ROOT" "$TAR" >/dev/null

python3 - "$DISK" << 'PYEOF'
import sys
disk = bytearray(4 * 1024 * 1024)
disk[0:4] = b"Aura"
disk[4:32] = b"Lite rv64 test disk pattern."
disk[510] = 0x55; disk[511] = 0xAA
open(sys.argv[1], "wb").write(disk)
PYEOF

fail=0
assert_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        printf '  [rv-parity] OK   %s\n' "$desc"
    else
        printf '  [rv-parity] FAIL %s\n' "$desc"
        fail=1
    fi
}
assert_no_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        printf '  [rv-parity] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [rv-parity] OK   %s\n' "$desc"
    fi
}

# ---- the one boot, everything attached, a short shell session ----
rm -f "$LOG"
{
    sleep 5
    printf 'uname\n';  sleep 1
    printf 'exit\n';   sleep 2
} | timeout 60 qemu-system-riscv64 \
        -machine virt -m 256M \
        -display none -serial stdio -no-reboot \
        -kernel "$ELF" -initrd "$TAR" \
        -global virtio-mmio.force-legacy=true \
        -drive file="$DISK",format=raw,if=none,id=hd \
        -device virtio-blk-device,drive=hd \
        -netdev user,id=n0 -device virtio-net-device,netdev=n0 \
        > "$LOG" 2>/dev/null || true

tr -d '\r' < "$LOG" > "$LOG.clean" && mv "$LOG.clean" "$LOG"

# ---- one assert per phase gate ----
assert_grep "Hello from AuraLite OS kernel (riscv64)!"     "V0: boot banner"
assert_grep "handoff magic OK, path=SBI"                   "V1: boot_info from the DTB"
assert_grep "\[isr\]  PASS"                                "V2: illegal-instruction round-trip"
assert_grep "\[timer\] PASS"                               "V2: 100 Hz ticks"
assert_grep "\[plic\] PASS"                                "V2: claim/complete with a real irq"
assert_grep "\[pmm\]  PASS"                                "V3: frame allocator"
assert_grep "execute-from-data faulted"                    "V3: W^X execute half (the arch's own gate)"
assert_grep "\[vmm\]  PASS"                                "V3: positive path + 3 fault probes"
assert_grep "\[heap\] PASS"                                "V3: alloc/free cycles"
assert_grep "\[sched\] PASS"                               "V4: forced preemption"
assert_grep "RING-U-OK"                                    "V4: U-mode ecall write"
assert_grep "\[user\] PASS"                                "V4: round trip + contained fault"
assert_grep "\[init\] PASS"                                "V5: compiled init exits 7"
assert_grep "auralite# "                                   "V5: the shared shell prompt"
assert_grep "\[blk\]  PASS"                                "V7: storage gate"
assert_grep "\[net\]  PASS"                                "V7: lease + ARP + verified echo"
assert_grep "rx bytes via PLIC irq: [1-9]"                 "V7: interrupt-driven console receipt"
assert_grep "V7 complete; console+shell+blk+net online"    "kernel reached idle with every gate up"
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
        printf '  [rv-parity] OK   x86_64: banner unchanged with the three-tenant tar\n'
    else
        printf '  [rv-parity] FAIL x86_64: did not boot with the three-tenant tar\n'
        fail=1
    fi
else
    printf '  [rv-parity] SKIP x86_64 pair (no qemu-system-x86_64 or no ISO)\n'
fi

if [ "$fail" -ne 0 ]; then
    echo "[rv-parity] FAILED"
    exit 1
fi
echo "[rv-parity] all assertions passed"
