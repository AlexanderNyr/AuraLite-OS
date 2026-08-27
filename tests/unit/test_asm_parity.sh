#!/usr/bin/env bash
# test_asm_parity.sh -- SELFHOST_PLAN.md SH4 host gate: mini-asm vs nasm.
#
# D7 ("the host is the judge"): the assembler is only as good as its output,
# so this gate assembles the tree's flat-binary (`-f bin`) sources with BOTH
# nasm and tools/mini-asm/mini-asm.c and requires BYTE-IDENTICAL results.
# `-f bin` has no relocations and no symbol table, so the bytes are the whole
# contract -- there is nothing structural left to hide a difference in.
#
# SH4a covers the simplest real flat file (mbr_dual.asm).  SH4b extends
# COVERED to the other three `-f bin` files; the receipt counts against the
# flat-file total so the gate reports progress (1/4 -> 4/4) instead of a
# silent pass.
#
# Skips cleanly (exit 0) when nasm is unavailable, like the other host gates.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

# The four `-f bin` (flat) objects in the tree.  SH4a landed the two MBR
# variants; SH4b added the SMP trampoline (64-bit mode, REX, control regs,
# far jmp).  SH4c brings stage2_start.asm, which needs %include/%if and the
# full encoder (SIB, segment overrides, bits 32).  Keep in sync with the
# plan's SH4 survey.
FLAT_TOTAL=4
COVERED=(
    boot/bios/stage1/mbr.asm
    boot/bios/stage1/mbr_dual.asm
    boot/smp/ap_trampoline.asm
)

if ! command -v nasm >/dev/null 2>&1; then
    echo "[selfhost] asm SKIP: nasm not installed -- cannot judge parity here"
    exit 0
fi

BUILD="$ROOT/build"
mkdir -p "$BUILD"
MINI="$BUILD/mini-asm"

# Build the assembler under test with the host cc (the same cc the guest
# toolchain bootstraps from; SH4d rebuilds it in-guest with tcc).
if ! cc -std=c99 -O2 -Wall -Wextra -Werror -o "$MINI" tools/mini-asm/mini-asm.c 2>"$BUILD/mini-asm.build.log"; then
    echo "[selfhost] asm FAIL: mini-asm does not compile cleanly with host cc"
    sed 's/^/    /' "$BUILD/mini-asm.build.log"
    exit 1
fi
echo "PASS: mini-asm compiles cleanly with host cc (-Werror)"

FAILED=0
IDENTICAL=0
for src in "${COVERED[@]}"; do
    if [ ! -f "$src" ]; then
        echo "FAIL: covered source missing: $src"
        FAILED=1
        continue
    fi
    ref="$BUILD/.parity_ref.bin"
    got="$BUILD/.parity_got.bin"
    if ! nasm -f bin "$src" -o "$ref" 2>/dev/null; then
        echo "FAIL: nasm could not assemble $src"
        FAILED=1
        continue
    fi
    if ! "$MINI" -f bin "$src" -o "$got" 2>"$BUILD/.parity_err"; then
        echo "FAIL: mini-asm could not assemble $src"
        sed 's/^/    /' "$BUILD/.parity_err"
        FAILED=1
        continue
    fi
    if cmp -s "$ref" "$got"; then
        echo "PASS: $(basename "$src") -- $(wc -c < "$ref") bytes, byte-identical to nasm"
        IDENTICAL=$((IDENTICAL + 1))
    else
        echo "FAIL: $src differs from nasm"
        # Show the first divergence to make the encoder bug findable.
        cmp -l "$ref" "$got" 2>/dev/null | head -8 | sed 's/^/    byte /'
        echo "    ref=$(wc -c < "$ref")B got=$(wc -c < "$got")B"
        FAILED=1
    fi
    rm -f "$ref" "$got"
done

echo
if [ "$FAILED" -eq 0 ]; then
    echo "[selfhost] asm PASS (bin): $IDENTICAL/$FLAT_TOTAL flat objects byte-identical"
    exit 0
fi
echo "[selfhost] asm FAIL (bin): $IDENTICAL/$FLAT_TOTAL flat objects byte-identical"
exit 1
