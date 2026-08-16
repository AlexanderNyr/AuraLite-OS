#!/usr/bin/env bash
# tests/integration/i386_boot32_smoke.sh -- I386_PLAN phase I1 smoke test.
#
# The full dual-kernel matrix, on the real `make iso` image:
#
#   1. qemu-system-i386  -> Stage 2 takes the 32-bit branch, loads
#      KERNEL32.ELF through elf32.inc, enters protected mode through
#      pmode32.inc, and the stub proves the ESI hand-off: banner +
#      boot_info magic OK + a non-zero mmap count (the count is the
#      canary for the i386/AMD64 struct-alignment contract -- it read
#      zero until -malign-double landed, see the Makefile comment).
#   2. qemu-system-x86_64, same bytes -> the 64-bit kernel boots exactly
#      as before; no 32-bit artefacts appear in its log.
#   3. Negative control: a copy of the image with KERNEL32.ELF deleted
#      boots on i386 into I0's refusal -- not a hang, not the stub.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite.iso"
LOG32="$BUILD/i386_boot32.log"
LOG64="$BUILD/i386_boot32_x64.log"
LOGNK="$BUILD/i386_boot32_nok32.log"
ISONK="$BUILD/i386_boot32_nok32.img"

[ -s "$ISO" ] || make -C "$ROOT" iso >/dev/null

if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    echo "[i386-boot32] SKIP: qemu-system-i386 not installed" >&2
    exit 0
fi

fail=0
run_qemu() {
    local bin="$1" img="$2" log="$3"
    rm -f "$log"
    timeout 25 "$bin" \
        -drive format=raw,file="$img",if=ide,snapshot=on \
        -m 512M \
        -display none -serial file:"$log" -no-reboot \
        >/dev/null 2>&1 || true
}

assert_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-boot32] OK   %s\n' "$desc"
    else
        printf '  [i386-boot32] FAIL %s\n' "$desc"
        fail=1
    fi
}

assert_no_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -q "$pat" "$log"; then
        printf '  [i386-boot32] FAIL %s\n' "$desc"
        fail=1
    else
        printf '  [i386-boot32] OK   %s\n' "$desc"
    fi
}

# ---- Case 1: the 32-bit CPU boots the 32-bit kernel. ----
run_qemu qemu-system-i386 "$ISO" "$LOG32"
assert_grep "$LOG32" "\[BL10\] taking the 32-bit path: KERNEL32.ELF"       "i386: Stage 2 branches to KERNEL32.ELF"
assert_grep "$LOG32" "\[BL10\] ELF32 PT_LOAD segments copied to phys"       "i386: ELF32 loader ran"
assert_grep "$LOG32" "\[BL10\] entering protected mode"                     "i386: protected-mode hand-off"
assert_grep "$LOG32" "\[kernel32\] AuraLite i386 stub alive"                "i386: stub banner"
assert_grep "$LOG32" "\[kernel32\] boot_info handoff (ESI) magic OK"        "i386: boot_info magic via ESI"
assert_no_grep "$LOG32" "mmap entries: 0x00000000"                          "i386: mmap count non-zero (ABI-alignment canary)"
assert_no_grep "$LOG32" "entering long mode"                                "i386: long mode never attempted"

# ---- Case 2: the same bytes still boot the 64-bit kernel. ----
run_qemu qemu-system-x86_64 "$ISO" "$LOG64"
assert_grep "$LOG64" "\[BL10\] CPU supports long mode"                      "x86_64: check passes"
assert_grep "$LOG64" "Hello from AuraLite OS kernel!"                       "x86_64: kernel banner unchanged"
assert_no_grep "$LOG64" "\[kernel32\]"                                      "x86_64: no 32-bit artefacts in the log"

# ---- Case 3: negative control -- image without KERNEL32.ELF refuses. ----
# The FAT32 partition starts at LBA 256 in the dual image; hand mtools
# the partition by byte offset so mdel can remove the file from a copy.
cp "$ISO" "$ISONK"
if mdel -i "$ISONK@@$((256 * 512))" ::/KERNEL32.ELF 2>/dev/null; then
    run_qemu qemu-system-i386 "$ISONK" "$LOGNK"
    assert_grep "$LOGNK" "\[BL10\] no KERNEL32.ELF on the boot partition"   "no-k32: honest refusal printed"
    assert_no_grep "$LOGNK" "\[kernel32\] AuraLite i386 stub alive"         "no-k32: stub does not run"
    assert_no_grep "$LOGNK" "entering long mode"                            "no-k32: long mode never attempted"
else
    echo "  [i386-boot32] SKIP negative control (mdel could not open the partition)"
fi
rm -f "$ISONK"

if [ "$fail" -ne 0 ]; then
    echo "[i386-boot32] FAILED"
    exit 1
fi
echo "[i386-boot32] all assertions passed"
