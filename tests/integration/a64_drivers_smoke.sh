#!/usr/bin/env bash
# tests/integration/a64_drivers_smoke.sh -- ARM64_PLAN phase A7.
#
# The driver boot: storage and network through the PROMOTED
# virtio-mmio transport (kernel/drivers/virtio_mmio.c -- the same
# object the rv64 kernel links), console interrupt-fed both ways
# (PL011 RX through the GIC, TX through the O3 ring core), one QEMU
# run with the full device set -- rv_parity_smoke's V7 stanza at the
# fourth tenant, with the SAME assertion strings, because the drivers
# keep the rv64 log lines byte for byte (the miniproto parity
# discipline extended to storage).
#
# The legacy-vs-modern lesson carries over verbatim:
# -global virtio-mmio.force-legacy=true pins version=1, and the [blk]
# banner asserts the version it negotiated, so a QEMU that flips the
# default fails loudly here instead of mysteriously later.
#
# Keeps the a64_shell_smoke disciplines: log-size fuse (a runaway
# prompt flood fails FAST -- the A5c lesson that once ate a
# workspace), PSCI ending, no-FAIL sweep.
#
# Skips cleanly without qemu-system-aarch64 / llvm-objcopy / the tar.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
IMG="$BUILD/kernela64.img"
INITRD="$BUILD/initrd.tar"
DISK="$BUILD/a64_drivers_disk.img"
LOG="$BUILD/a64_drivers.log"

if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "[a64-drivers] SKIP: qemu-system-aarch64 not installed" >&2
    exit 0
fi
if ! command -v llvm-objcopy >/dev/null 2>&1; then
    echo "[a64-drivers] SKIP: llvm-objcopy not installed" >&2
    exit 0
fi

[ -s "$IMG" ]    || make -C "$ROOT" kernela64-img >/dev/null
[ -s "$INITRD" ] || { echo "[a64-drivers] SKIP: build/initrd.tar absent (run 'make iso' first)" >&2; exit 0; }

# The known test pattern: the [blk] gate's sector-0 contract is "read
# what we know is there" (ata32 got these bytes from Stage 1's MBR;
# this board has no MBR, so the smoke writes them).
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
        printf '  [a64-drivers] OK   %s\n' "$desc"
    else
        printf '  [a64-drivers] FAIL %s\n' "$desc"
        fail=1
    fi
}
assert_no_grep() {
    local pat="$1" desc="$2"
    if grep -qa "$pat" "$LOG"; then
        printf '  [a64-drivers] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [a64-drivers] OK   %s\n' "$desc"
    fi
}

# ---- the one boot: devices attached, a short interactive session ----
# Input is PROMPT-DRIVEN, not sleep-scheduled (the second CI lesson
# here: under shared-runner TCG the prompt can take ~55 s, and bytes
# queued into the chardev during a long boot tickled a QEMU 8.2
# lost-RX-edge stall -- deterministic, byte-identical logs at 60 s
# and 180 s timeouts).  The writer watches the live log for the
# prompt and only then types; the guest side additionally recovers
# lost edges by FR polling (see pl011_try_getc).  The timeout stays
# an upper fence -- QEMU exits by PSCI on a healthy run.
rm -f "$LOG"
{
    for _ in $(seq 1 150); do
        grep -qa "auralite# " "$LOG" 2>/dev/null && break
        sleep 1
    done
    printf 'uname\n';  sleep 2
    printf 'exit\n';   sleep 3
} | timeout 240 qemu-system-aarch64 -machine virt -cpu cortex-a72 \
        -m 256M -display none -serial stdio -no-reboot \
        -kernel "$IMG" -initrd "$INITRD" \
        -global virtio-mmio.force-legacy=true \
        -drive file="$DISK",format=raw,if=none,id=hd \
        -device virtio-blk-device,drive=hd \
        -netdev user,id=n0 -device virtio-net-device,netdev=n0 \
        > "$LOG" 2>/dev/null || true

tr -d '\r' < "$LOG" > "$LOG.clean" && mv "$LOG.clean" "$LOG"

# ---- the log-size fuse (the A5c discipline, kept forever) ----
LOGSIZE=$(wc -c < "$LOG")
if [ "$LOGSIZE" -gt 200000 ]; then
    printf '  [a64-drivers] FAIL log-size fuse: %s bytes (flood?)\n' "$LOGSIZE"
    echo "[a64-drivers] FAILED (fuse)"
    exit 1
fi
printf '  [a64-drivers] OK   log-size fuse intact (%s bytes)\n' "$LOGSIZE"

# ---- the console, interrupt-fed both ways ----
assert_grep "pl011 rx armed: IRQ through the GIC into the cons ring" \
                                                   "RX armed through the GIC (INTID from the DTB)"
assert_grep "rx bytes via GIC irq: [1-9]"          "every session keystroke arrived via IRQ, counted"

# ---- storage over the promoted transport ----
assert_grep "\[blk\]  virtio-blk over mmio (legacy version 1)" \
                                                   "blk attached, legacy version pinned (force-legacy lesson)"
assert_grep "\[blk\]  PASS: known-bytes read + write/readback/restore" \
                                                   "blk gate: sector-0 contract + write/readback/restore"

# ---- network over the promoted transport + the SHARED miniproto ----
assert_grep "\[net\]  virtio-net over mmio, MAC "  "net attached, MAC from config space"
assert_grep "\[net\]  DHCP lease: 10.0.2.15"       "SLIRP lease (the same address every tenant gets)"
assert_grep "\[net\]  ARP: gateway resolved"       "gateway ARP"
assert_grep "\[net\]  PASS: lease + ARP + echo reply (payload verified)" \
                                                   "net gate: the third miniproto consumer, payload verified"

# ---- the attach-time attribute gate stayed quiet ----
assert_no_grep "not Device-mapped"                 "no window was refused (HHDM device attrs hold)"
assert_no_grep "queue alloc not contiguous"        "vring adjacency held (bitmap PMM walks upward)"

# ---- the session itself still works over the IRQ-fed console ----
assert_grep "AuraLite OS aarch64 (TTBR1 higher half" "uname round-trips over IRQ RX"
assert_grep "A7 complete; console+shell+blk+net online" \
                                                   "the healthy A7 ending, by PSCI"

# ---- nothing failed anywhere ----
assert_no_grep "FAIL"                              "no gate failed anywhere in the boot"
assert_no_grep "UNHANDLED"                         "no unhandled trap anywhere in the boot"

if [ "$fail" -eq 0 ]; then
    echo "[a64-drivers] all A7 assertions passed"
else
    echo "[a64-drivers] FAILED"
fi
exit "$fail"
