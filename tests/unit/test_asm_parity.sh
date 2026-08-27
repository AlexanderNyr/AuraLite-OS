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
    boot/bios/stage2/stage2_start.asm
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

# stage2_start.asm %includes build/boot_offsets.inc (a generated header); build
# it if absent so the parity check is self-contained outside `make test-unit`.
if [ ! -f "$BUILD/boot_offsets.inc" ] && [ -f tools/gen_boot_offsets.c ]; then
    if cc -std=c11 -I . tools/gen_boot_offsets.c -o "$BUILD/gen_boot_offsets" 2>/dev/null; then
        "$BUILD/gen_boot_offsets" --asm > "$BUILD/boot_offsets.inc" 2>/dev/null
    fi
fi
INC="-I . -I build/"

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
    if ! nasm -f bin $INC "$src" -o "$ref" 2>/dev/null; then
        echo "FAIL: nasm could not assemble $src"
        FAILED=1
        continue
    fi
    if ! "$MINI" -f bin $INC "$src" -o "$got" 2>"$BUILD/.parity_err"; then
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
else
    echo "[selfhost] asm FAIL (bin): $IDENTICAL/$FLAT_TOTAL flat objects byte-identical"
fi

# ---------------------------------------------------------------------------
# SH4d: the `-f elf64` readelf-parity mode.
#
# nasm does not guarantee ELF byte layout, so the SH4 bar for the ELF objects
# is structural: section names/types/flags, symbol names/bindings/section
# indices, relocation types/addends -- all compared via `readelf`.  We also
# compare the emitted section DATA bytes, which the plan's DoD does not
# require but which catches encoder regressions the headers would miss (and
# which currently passes 13/13).
#
# Covered: the 13 `-f elf64` files (kernel/arch/x86_64/* x5, kernel/proc/* x4,
# lib/libc/crt/* x3, lib/libc/src/syscall.asm).  The tree's only other asm
# formats are `-f bin` (SH4a-c, above), `-f elf32` and `-f win64` (SH4e and
# the w32 test fixtures; both scoped to later phases / out of closure).
ELF_TOTAL=13
ELF_COVERED=(
    kernel/arch/x86_64/boot.asm
    kernel/arch/x86_64/gdt_flush.asm
    kernel/arch/x86_64/isr_stubs.asm
    kernel/arch/x86_64/syscall_entry.asm
    kernel/arch/x86_64/syscall_sigreturn.asm
    kernel/proc/context.asm
    kernel/proc/fork_return.asm
    kernel/proc/user_entry.asm
    kernel/proc/usercopy_fault.asm
    lib/libc/crt/crt0.asm
    lib/libc/crt/setjmp.asm
    lib/libc/crt/sigreturn.asm
    lib/libc/src/syscall.asm
)

if ! command -v readelf >/dev/null 2>&1 || ! command -v objcopy >/dev/null 2>&1; then
    echo "[selfhost] asm SKIP: readelf/objcopy not installed -- cannot judge elf64 parity"
    exit 0
fi

# context.asm and syscall_entry.asm %include build/asm_offsets.inc (generated
# by tools/gen_asm_offsets.c); build it if absent so the gate is
# self-contained outside `make test-unit`.
if [ ! -f "$BUILD/asm_offsets.inc" ] && [ -f tools/gen_asm_offsets.c ]; then
    if cc -std=c11 -I . tools/gen_asm_offsets.c -o "$BUILD/gen_asm_offsets" 2>/dev/null; then
        "$BUILD/gen_asm_offsets" > "$BUILD/asm_offsets.inc" 2>/dev/null
    fi
fi

FAILED=0
IDENTICAL=0
for src in "${ELF_COVERED[@]}"; do
    if [ ! -f "$src" ]; then
        echo "FAIL: covered elf64 source missing: $src"
        FAILED=1
        continue
    fi
    ref="$BUILD/.parity_ref_elf.o"
    got="$BUILD/.parity_got_elf.o"
    # same invocation (relative path) for both so the FILE symbol name matches
    if ! nasm -f elf64 $INC "$src" -o "$ref" 2>/dev/null; then
        echo "FAIL: nasm could not assemble $src"
        FAILED=1
        continue
    fi
    if ! "$MINI" -f elf64 $INC "$src" -o "$got" 2>"$BUILD/.parity_err"; then
        echo "FAIL: mini-asm could not assemble $src (elf64)"
        sed 's/^/    /' "$BUILD/.parity_err"
        FAILED=1
        continue
    fi
    # 1) section headers: Name Type Size Flg Lk Inf Al (layout Off/Addr differ
    #    legitimately -- nasm does not guarantee ELF byte layout)
    if ! diff <(readelf -SW "$ref" 2>/dev/null | tail -n +4 | awk '{printf "%s %s %s %s %s %s %s\n",$3,$4,$7,$9,$10,$11,$12}') \
              <(readelf -SW "$got" 2>/dev/null | tail -n +4 | awk '{printf "%s %s %s %s %s %s %s\n",$3,$4,$7,$9,$10,$11,$12}') > "$BUILD/.secdiff" 2>&1; then
        echo "FAIL: $src section headers differ from nasm"
        sed 's/^/    /' "$BUILD/.secdiff" | head -6
        FAILED=1
        continue
    fi
    # 2) symbol table: Value Type Bind Ndx Name
    if ! diff <(readelf -sW "$ref" 2>/dev/null | tail -n +4 | awk '{printf "%s %s %s %s %s\n",$2,$4,$5,$7,$8}') \
              <(readelf -sW "$got" 2>/dev/null | tail -n +4 | awk '{printf "%s %s %s %s %s\n",$2,$4,$5,$7,$8}') > "$BUILD/.symdiff" 2>&1; then
        echo "FAIL: $src symbol table differs from nasm"
        sed 's/^/    /' "$BUILD/.symdiff" | head -6
        FAILED=1
        continue
    fi
    # 3) relocations: Offset Type Symbol + Addend
    if ! diff <(readelf -rW "$ref" 2>/dev/null | grep R_X86_64 | awk '{printf "%s %s %s %s %s\n",$1,$3,$4,$5,$6}') \
              <(readelf -rW "$got" 2>/dev/null | grep R_X86_64 | awk '{printf "%s %s %s %s %s\n",$1,$3,$4,$5,$6}') > "$BUILD/.reldiff" 2>&1; then
        echo "FAIL: $src relocations differ from nasm"
        sed 's/^/    /' "$BUILD/.reldiff" | head -6
        FAILED=1
        continue
    fi
    # 4) section data bytes (stricter than the DoD, currently green)
    for s in $(readelf -SW "$ref" 2>/dev/null | awk '$2 ~ /^\.(text|rodata|data|bss)$/ {print $2}'); do
        if ! cmp -s <(objcopy -O binary --only-section="$s" "$ref" /dev/stdout 2>/dev/null) \
                     <(objcopy -O binary --only-section="$s" "$got" /dev/stdout 2>/dev/null); then
            echo "FAIL: $src section $s data differs from nasm"
            FAILED=1
            continue 2
        fi
    done
    echo "PASS: $(basename "$src") -- readelf parity with nasm (elf64)"
    IDENTICAL=$((IDENTICAL + 1))
    rm -f "$ref" "$got"
done

echo
if [ "$FAILED" -eq 0 ]; then
    echo "[selfhost] asm PASS (elf64): $IDENTICAL/$ELF_TOTAL objects readelf-parity"
else
    echo "[selfhost] asm FAIL (elf64): $IDENTICAL/$ELF_TOTAL objects readelf-parity"
fi
exit $([ "$FAILED" -eq 0 ] && echo 0 || echo 1)
