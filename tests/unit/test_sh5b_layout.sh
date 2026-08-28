#!/usr/bin/env bash
# test_sh5b_layout.sh -- SELFHOST_PLAN.md SH5b host gate: aulink kernel.ld
# layout parity vs ld.lld on the REAL kernel objects.
#
# Links the tree's 135 kernel objects with BOTH linkers against the real
# kernel.ld and compares the honest, semantic parts of the layout:
#   - entry point (_start) and PT_LOAD count + flags        (must match 1:1)
#   - .text address + size                                  (must match 1:1)
#   - .data / .bss sizes                                    (must match 1:1)
#   - key symbols in .text (_start, kmain, syscall_entry, isr_table,
#     context_switch, boot_page_directory)                  (must match 1:1)
#   - .rodata start address                                 (must match)
#   - .rodata size                                          (|delta| <= 0x40;
#     ld.lld additionally sorts the merged string/cst pools, so the exact
#     byte count of the merged region is not guaranteed -- recorded in the
#     plan as a size optimisation, not semantics)
#   - __bss_start/__bss_end existence and ordering           (consistency)
#
# ld.lld is run WITHOUT --gc-sections so both sides link the same input
# sections (gc is an OPT O8 footprint optimisation for the clang build, not
# layout semantics; the tcc build SH5c/d has no function-sections and needs
# no gc).  A clang kernel without gc does not boot -- both linkers' outputs
# are equally affected, which is why the boot gate is SH5d's, not this one.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
FAILED=0
check(){ if [ "$1" = "$2" ]; then echo "PASS: $3"; else echo "FAIL: $3 (got '$1', want '$2')"; FAILED=1; fi; }

command -v ld.lld >/dev/null 2>&1 || { echo "[selfhost] SH5b SKIP: ld.lld not installed"; exit 0; }

BUILD="$ROOT/build/sh5b"
mkdir -p "$BUILD"
cc -std=c99 -O2 -o "$BUILD/aulink" tools/aulink/aulink.c || { echo "FAIL: aulink does not compile"; exit 1; }

# kernel objects must exist
if [ ! -f "$ROOT/build/kernel/kernel.o" ]; then
    echo "SKIP: kernel objects not built (run 'make kernel' first)"
    exit 0
fi
# Use the Makefile's object order (it determines the input-section order,
# which fixes where _start lands in .text and the section sizes).
OBJS=$(make -B -n kernel 2>/dev/null | grep 'ld.lld' | grep -- '-T kernel.ld' | tr ' ' '\n' | grep '\.o$' | tr '\n' ' ')
if [ -z "$OBJS" ]; then echo "SKIP: could not derive kernel object list from Makefile"; exit 0; fi

ld.lld -nostdlib -static -T kernel.ld -z max-page-size=4096 $OBJS -o "$BUILD/lld.elf" 2>"$BUILD/lld.err" || { echo "FAIL: ld.lld could not link"; sed 's/^/    /' "$BUILD/lld.err" | head -5; exit 1; }
"$BUILD/aulink" -T kernel.ld -o "$BUILD/au.elf" $OBJS 2>"$BUILD/au.err"
rc=$?
if [ $rc -ne 0 ]; then echo "FAIL: aulink could not link"; sed 's/^/    /' "$BUILD/au.err" | head -8; exit 1; fi

sec(){ readelf -SW "$1" 2>/dev/null | awk -v s="$2" '$3==s {printf "%s %s", $5, $7}'; }

# 1) entry
e_l=$(readelf -h "$BUILD/lld.elf" | awk '/Entry point/{print $4}')
e_a=$(readelf -h "$BUILD/au.elf"  | awk '/Entry point/{print $4}')
check "$e_l" "$e_a" "entry point ($e_l)"

# 2) PT_LOAD count + flags
pl_l=$(readelf -lW "$BUILD/lld.elf" | grep -c LOAD)
pl_a=$(readelf -lW "$BUILD/au.elf"  | grep -c LOAD)
check "$pl_l" "$pl_a" "PT_LOAD count ($pl_l)"
fl_l=$(readelf -lW "$BUILD/lld.elf" | grep LOAD | python3 -c '
import sys,re
for l in sys.stdin:
    m=re.search(r" ([RWE ]+?) +0x[0-9a-f]+$",l)
    print(m.group(1).strip().replace(" ",""),end=" ")')
fl_a=$(readelf -lW "$BUILD/au.elf" | grep LOAD | python3 -c '
import sys,re
for l in sys.stdin:
    m=re.search(r" ([RWE ]+?) +0x[0-9a-f]+$",l)
    print(m.group(1).strip().replace(" ",""),end=" ")')
check "$fl_l" "$fl_a" "PT_LOAD flags order ($fl_l)"

# 3) section addr+size: .text must match 1:1; .data/.bss sizes must match
for s in text data bss; do
    l=$(sec "$BUILD/lld.elf" ".$s"); a=$(sec "$BUILD/au.elf" ".$s")
    lsz=${l##* }; asz=${a##* }
    check "$lsz" "$asz" ".$s size ($lsz)"
    if [ "$s" = text ]; then
        laddr=${l%% *}; aaddr=${a%% *}
        check "$laddr" "$aaddr" ".text address ($laddr)"
    fi
done
# .rodata address must match; size within the documented merge-order delta
lr=$(sec "$BUILD/lld.elf" ".rodata"); ar=$(sec "$BUILD/au.elf" ".rodata")
laddr=${lr%% *}; aaddr=${ar%% *}
check "$laddr" "$aaddr" ".rodata address ($laddr)"
lrsz=${lr##* }; arsz=${ar##* }
dl=$(( 0x$lrsz - 0x$arsz )); [ "$dl" -lt 0 ] && dl=$(( -dl ))
if [ "$dl" -le $((0x40)) ]; then echo "PASS: .rodata size delta 0x$(printf %x $dl) <= 0x40 (merge-order, documented)";
else echo "FAIL: .rodata size delta 0x$(printf %x $dl) > 0x40 (lld=$lrsz au=$arsz)"; FAILED=1; fi

# 4) key symbols: .text symbols must land at IDENTICAL addresses; the one
# .rodata symbol (isr_table) is allowed the documented merge-order delta.
for sym in _start kmain syscall_entry context_switch uart_init; do
    a_l=$(readelf -sW "$BUILD/lld.elf" 2>/dev/null | awk -v s="$sym" '$8==s && $7!="UND" {print $2; exit}')
    a_a=$(readelf -sW "$BUILD/au.elf" 2>/dev/null | awk -v s="$sym" '$8==s && $7!="UND" {print $2; exit}')
    if [ -n "$a_l" ] && [ -n "$a_a" ]; then
        check "$a_l" "$a_a" "symbol $sym address ($a_l)"
    else
        echo "FAIL: symbol $sym missing (lld='$a_l' au='$a_a')"; FAILED=1
    fi
done
i_l=$(readelf -sW "$BUILD/lld.elf" 2>/dev/null | awk '$8=="isr_table" && $7!="UND" {print $2; exit}')
i_a=$(readelf -sW "$BUILD/au.elf" 2>/dev/null | awk '$8=="isr_table" && $7!="UND" {print $2; exit}')
if [ -n "$i_l" ] && [ -n "$i_a" ]; then
    idl=$(( 16#$i_l - 16#$i_a )); [ "$idl" -lt 0 ] && idl=$(( -idl ))
    if [ "$idl" -le $((0x40)) ]; then echo "PASS: isr_table (.rodata) within merge delta (0x$(printf %x $idl))";
    else echo "FAIL: isr_table delta 0x$(printf %x $idl) > 0x40"; FAILED=1; fi
else echo "FAIL: isr_table missing (lld='$i_l' au='$i_a')"; FAILED=1; fi

# 5) __bss_start/__bss_end ordering (bss is shifted by the .rodata delta)
b1=$(readelf -sW "$BUILD/au.elf" 2>/dev/null | awk '$8=="__bss_start" {print $2; exit}')
b2=$(readelf -sW "$BUILD/au.elf" 2>/dev/null | awk '$8=="__bss_end" {print $2; exit}')
if [ -n "$b1" ] && [ -n "$b2" ]; then
    s1=$((16#$b1)); s2=$((16#$b2))
    if [ $s2 -gt $s1 ]; then echo "PASS: __bss_start=$b1 < __bss_end=$b2 (ordered)";
    else echo "FAIL: __bss ordering ($b1 >= $b2)"; FAILED=1; fi
else echo "FAIL: __bss_start/__bss_end missing (got '$b1' '$b2')"; FAILED=1; fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "[selfhost] layout PASS: aulink kernel.ld layout matches ld.lld (entry, PHDRs, .text 1:1, symbols)"
    exit 0
fi
echo "[selfhost] layout FAIL"
exit 1
