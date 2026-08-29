#!/usr/bin/env bash
# test_aulink.sh -- SELFHOST_PLAN.md SH3 host gate: aulink vs ld.lld parity.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
FAILED=0
check(){ if [ "$1" = "$2" ]; then echo "PASS: $3"; else echo "FAIL: $3 (got '$1', want '$2')"; FAILED=1; fi; }

AULINK="$ROOT/build/aulink"
mkdir -p "$ROOT/build"
cc -std=c99 -O2 -o "$AULINK" "$ROOT/tools/aulink/aulink.c" || { echo "FAIL: aulink does not compile with host cc"; exit 1; }
echo "PASS: aulink compiles with host cc"

OBJS="build/user/crt0.o build/user/syscall.o build/user/libc.o build/user/malloc.o build/user/env.o build/user/string_extra.o build/user/stdlib_extra.o build/user/sigreturn.o build/user/setjmp.o build/user/sysinfo.o"
for o in $OBJS; do
    if [ ! -f "$o" ]; then
        echo "SKIP: userland objects not built (missing $o; run 'make user' first)"
        exit 0
    fi
done

LD_OUT="$ROOT/build/aulink-ld-sysinfo.elf"
AU_OUT="$ROOT/build/aulink-sysinfo.elf"

ld.lld -m elf_x86_64 -nostdlib -static -T lib/libc/user.ld -z max-page-size=4096 --gc-sections $OBJS -o "$LD_OUT" || { echo "FAIL: ld.lld could not link"; exit 1; }
"$AULINK" -T lib/libc/user.ld -o "$AU_OUT" $OBJS || { echo "FAIL: aulink could not link"; exit 1; }

# SH5d: a full kernel cannot fit into AuraLite's intentionally small shell
# argv.  Give aulink a directory whose ordinal prefixes preserve the explicit
# object order above; it must enumerate only *.o files, sort them, and produce
# the same executable entry rather than relying on readdir() order.
OBJDIR=$(mktemp -d "$ROOT/build/aulink-dir.XXXXXX")
EMPTYDIR=$(mktemp -d "$ROOT/build/aulink-empty.XXXXXX")
trap 'rm -rf "$OBJDIR" "$EMPTYDIR"' EXIT
n=0
for o in $OBJS; do
    n=$((n + 1))
    cp "$o" "$OBJDIR/$(printf '%02d' "$n")-$(basename "$o")"
done
# Only immediate *.o children are inputs; neither a nested object nor a
# non-object sidecar may influence the deterministic link set.
mkdir "$OBJDIR/nested"
cp build/user/crt0.o "$OBJDIR/nested/00-hidden.o"
printf 'not an object\n' > "$OBJDIR/README"
DIR_OUT="$ROOT/build/aulink-sysinfo-dir.elf"
DIR_LOG="$ROOT/build/aulink-sysinfo-dir.log"
"$AULINK" -T lib/libc/user.ld -o "$DIR_OUT" "$OBJDIR" >"$DIR_LOG" 2>&1 \
    || { echo "FAIL: aulink directory-input link failed"; exit 1; }
if grep -Fq "aulink: added $n object(s) from directory $OBJDIR" "$DIR_LOG"; then
    echo "PASS: directory input expands $n sorted immediate object files"
else
    echo "FAIL: directory input did not report its sorted object set"; FAILED=1
fi
if cmp -s "$AU_OUT" "$DIR_OUT"; then
    echo "PASS: lexical directory expansion reproduces explicit object-order ELF byte-for-byte"
else
    echo "FAIL: directory expansion did not preserve the explicit lexical object order"; FAILED=1
fi
EMPTY_LOG="$ROOT/build/aulink-empty.log"
if "$AULINK" -T lib/libc/user.ld -o "$ROOT/build/aulink-empty.elf" "$EMPTYDIR" >"$EMPTY_LOG" 2>&1; then
    echo "FAIL: aulink accepted an empty object directory"; FAILED=1
elif grep -Fq "contains no *.o inputs" "$EMPTY_LOG"; then
    echo "PASS: empty object directory is rejected with a diagnostic"
else
    echo "FAIL: empty object directory lacked its diagnostic"; FAILED=1
fi

e_ld=$(readelf -h "$LD_OUT" | awk '/Entry point/{print $4}')
e_au=$(readelf -h "$AU_OUT" | awk '/Entry point/{print $4}')
e_dir=$(readelf -h "$DIR_OUT" | awk '/Entry point/{print $4}')
check "$e_ld" "$e_au" "entry point matches ($e_ld)"
check "$e_au" "$e_dir" "directory-input entry point matches explicit input order"

ph_ld=$(readelf -lW "$LD_OUT" | awk '/LOAD/{print $7}')
ph_au=$(readelf -lW "$AU_OUT" | awk '/LOAD/{print $7}')
check "$ph_ld" "$ph_au" "PT_LOAD flags in the same order"

sec_ld=$(mktemp); sec_au=$(mktemp)
readelf -SW "$LD_OUT" | awk '/^  \[/ {print $3, $4}' | grep -vE '\.symtab|\.strtab|\.shstrtab' | sort > "$sec_ld"
readelf -SW "$AU_OUT" | awk '/^  \[/ {print $3, $4}' | grep -vE '\.symtab|\.strtab|\.shstrtab' | sort > "$sec_au"
missing=0
while IFS= read -r line; do if ! grep -qxF "$line" "$sec_au"; then echo "FAIL: section missing in aulink: $line"; missing=1; fi; done < "$sec_ld"
[ "$missing" -eq 0 ] && echo "PASS: all $(wc -l < "$sec_ld") lld sections exist identically in aulink output"
rm -f "$sec_ld" "$sec_au"

v_ld=$(readelf -sW "$LD_OUT" | awk '$4=="FUNC" && $7!="UND" && $8=="_start" {print $2; exit}')
v_au=$(readelf -sW "$AU_OUT" | awk '$4=="FUNC" && $7!="UND" && $8=="_start" {print $2; exit}')
check "$v_ld" "$v_au" "_start address matches"
for sym in main __libc_start_main; do
    p_ld=$(readelf -sW "$LD_OUT" | awk -v s="$sym" '$4=="FUNC" && $7!="UND" && $8==s {print $2; exit}')
    p_au=$(readelf -sW "$AU_OUT" | awk -v s="$sym" '$4=="FUNC" && $7!="UND" && $8==s {print $2; exit}')
    if [ -n "$p_ld" ] && [ -n "$p_au" ]; then echo "PASS: symbol $sym present in both outputs"; else echo "FAIL: symbol $sym missing (lld='$p_ld' aulink='$p_au')"; FAILED=1; fi
done

echo
if [ "$FAILED" -eq 0 ]; then echo "=== ALL AULINK PARITY TESTS PASSED ==="; exit 0; fi
echo "=== AULINK PARITY TESTS FAILED ==="; exit 1
