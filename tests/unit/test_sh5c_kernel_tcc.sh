#!/usr/bin/env bash
# test_sh5c_kernel_tcc.sh -- SELFHOST_PLAN.md SH5c host gate: the kernel,
# compiled by tcc.
#
# Runs tools/selfhost/build_kernel_tcc.sh (tcc compiles the 127 kernel C
# files, mini-asm assembles the 9 asm files, aulink links kernel-tcc.elf
# against kernel.ld) and then asserts the three parts of the SH5c story:
#
#   1. THE LINK: the tcc-built kernel links at the higher half with the
#      same ELF shape the clang kernel has (entry == _start, 3 PT_LOADs
#      R E / R / RW, ordered higher-half __bss_start/__bss_end).
#   2. THE FLAG AUDITS (measured, not assumed):
#        - no 32-bit absolute relocations in ANY tcc object (SH5a's spike
#          measurement extended to the full kernel: -mcmodel=kernel stays
#          unnecessary),
#        - zero negative-rsp-relative memory operands (tcc's frame-based
#          codegen never touches the 128-byte red zone; -mno-red-zone
#          stays unnecessary),
#        - zero __stack_chk_fail call sites (tcc has no stack protector;
#          the clang build instruments 310 -- recorded as the delta),
#        - kprintf's 16 xmm uses are READS only (tcc's varargs prologue
#          spills %xmm0-7 to the stack; nothing writes xmm state that a
#          preempted kernel float computation could lose).
#   3. THE LAYOUT PARITIES:
#        - tcc- and cc-generated asm_offsets.inc are identical (the TCB
#          contract context.asm consumes is compiler-independent),
#        - every packed aggregate in the x86_64 kernel tree has the SAME
#          sizeof under clang and under tcc (119 structs at the time of
#          landing; tcc ignores __attribute__((packed)) member placement,
#          so the SH5c wraps carry the layout -- see
#          tools/selfhost/packify_packed.py).
#
# Skips cleanly without the host tcc, the guest libtcc1.a, clang or
# readelf.  The BOOT half (tcc kernel to the shell in QEMU) lives in
# tests/integration/cases/test_selfhost_kernel_tcc.sh.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
FAILED=0
n_assert=0
ok(){ n_assert=$((n_assert+1)); echo "PASS: $1"; }
bad(){ n_assert=$((n_assert+1)); echo "FAIL: $1"; FAILED=1; }

HOST_TCC="$ROOT/build/selfhost/host-tcc-src/tcc"
LIBTCC1="$ROOT/build/selfhost/libtcc1.a"
if [ ! -x "$HOST_TCC" ] || [ ! -f "$LIBTCC1" ]; then
    echo "[selfhost] SH5c SKIP: host tcc / libtcc1 absent -- run 'make selfhost-deps selfhost-tcc selfhost-host-tcc'"
    exit 0
fi
command -v clang >/dev/null 2>&1 || { echo "[selfhost] SH5c SKIP: clang absent"; exit 0; }
command -v readelf >/dev/null 2>&1 || { echo "[selfhost] SH5c SKIP: readelf absent"; exit 0; }
command -v objdump >/dev/null 2>&1 || { echo "[selfhost] SH5c SKIP: objdump absent"; exit 0; }

OUT="$ROOT/build/selfhost/kernel-tcc"
ELF="$ROOT/build/selfhost/kernel-tcc.elf"

# ---- 1) build -----------------------------------------------------------
if ! bash tools/selfhost/build_kernel_tcc.sh > "$OUT-test.log" 2>&1; then
    echo "FAIL: tools/selfhost/build_kernel_tcc.sh exited nonzero"
    sed 's/^/    /' "$OUT-test.log" | tail -15
    exit 1
fi
grep -q '^\[sh5c\] tcc compiled 127 kernel C files' "$OUT-test.log" \
    && ok "tcc compiled all 127 kernel C files" \
    || bad "tcc object count changed (see $OUT-test.log)"
grep -q '^\[sh5c\] mini-asm assembled 9 kernel asm files' "$OUT-test.log" \
    && ok "mini-asm assembled all 9 kernel asm files" \
    || bad "mini-asm object count changed"
[ -f "$ELF" ] && ok "aulink linked kernel-tcc.elf" || bad "kernel-tcc.elf missing"

# ---- 2) ELF shape -------------------------------------------------------
entry=$(readelf -h "$ELF" | awk '/Entry point address/{print $4}' | sed 's/^0x//')
start=$(readelf -sW "$ELF" 2>/dev/null | awk '$8=="_start" && $7!="UND" {print $2; exit}')
[ -n "$start" ] && [ "$entry" = "$start" ] && ok "entry == _start ($entry)" \
    || bad "entry ($entry) != _start ($start)"
loads=$(readelf -lW "$ELF" | grep -c LOAD)
[ "$loads" -eq 3 ] && ok "3 PT_LOADs" || bad "$loads PT_LOADs (want 3)"
flg=$(readelf -lW "$ELF" | grep LOAD | sed -E 's/.* [RWE ]+ 0x[0-9a-f]+$/&/' | python3 -c '
import sys, re
out=[]
for l in sys.stdin:
    m=re.search(r" ([RWE ]+?) +0x[0-9a-f]+$", l)
    out.append(m.group(1).strip().replace(" ","") if m else "?")
print(" ".join(out))')
[ "$flg" = "RE R RW" ] && ok "PT_LOAD flags R E / R / RW" || bad "PT_LOAD flags '$flg'"
bss_lo=$(readelf -sW "$ELF" 2>/dev/null | awk '$8=="__bss_start"{print $2; exit}')
bss_hi=$(readelf -sW "$ELF" 2>/dev/null | awk '$8=="__bss_end"{print $2; exit}')
bss_ok=$(python3 -c "print('yes' if '$bss_lo'.startswith('ffffffff') and '$bss_hi'.startswith('ffffffff') and int('$bss_hi',16) > int('$bss_lo',16) else 'no')" 2>/dev/null)
[ "$bss_ok" = "yes" ] && ok "__bss_start ($bss_lo) < __bss_end ($bss_hi), higher half" \
    || bad "bss ordering/addresses wrong ($bss_lo..$bss_hi)"

# ---- 3) flag audits over every tcc object -------------------------------
rel32=0; redzone=0; canary=0
for o in "$OUT"/obj/*.o; do
    rel32=$((rel32 + $(readelf -rW "$o" 2>/dev/null | grep -cE "R_X86_64_(32|32S|PC32S)\b")))
    redzone=$((redzone + $(objdump -d "$o" 2>/dev/null | grep -cE ',?-0x[0-9a-f]+\(%rsp\)')))
    canary=$((canary + $(readelf -rW "$o" 2>/dev/null | grep -c "__stack_chk_fail")))
done
[ "$rel32" -eq 0 ] && ok "no 32-bit absolute relocations in any tcc object (-mcmodel=kernel stays unnecessary)" \
    || bad "$rel32 32-bit absolute relocations -- higher-half linkability at risk"
[ "$redzone" -eq 0 ] && ok "zero negative-rsp memory operands (no red-zone use; -mno-red-zone stays unnecessary)" \
    || bad "$redzone red-zone references in tcc objects"
[ "$canary" -eq 0 ] && ok "zero stack-canary call sites in the tcc kernel (clang: 310 -- the recorded delta)" \
    || bad "$canary canary call sites in tcc objects (tcc has no stack protector)"

# kprintf's xmm uses must be reads (source operands), never writes.
kprintf_o="$OUT/obj/kernel_lib_kprintf.c.o"
xmm_total=$(objdump -d "$kprintf_o" 2>/dev/null | grep -c "%xmm")
xmm_dst=$(objdump -d "$kprintf_o" 2>/dev/null | grep "%xmm" | grep -c ", *%xmm")
[ -f "$kprintf_o" ] && [ "$xmm_dst" -eq 0 ] && [ "$xmm_total" -gt 0 ] \
    && ok "kprintf's $xmm_total xmm ops are reads only (varargs spill; no xmm state is clobbered)" \
    || bad "kprintf writes xmm state ($xmm_dst of $xmm_total) -- IRQ-context clobber risk"

# ---- 4) layout parities --------------------------------------------------
if cmp -s "$OUT/asm_offsets.inc" "$OUT/asm_offsets-cc.inc"; then
    ok "asm offsets identical from tcc- and cc-built generators (TCB contract)"
else
    bad "tcc/cc asm offsets differ"
fi

PROBE="$OUT/packed-probe"; rm -rf "$PROBE"; mkdir -p "$PROBE"
FILES=$(grep -rl "attribute__((packed))" kernel drivers w32/src --include='*.c' --include='*.h' \
    | grep -v "arch/i386\|arch/riscv64\|arch/aarch64\|kernel/drivers" | sort)
n_probe=0; n_structs=0; n_mismatch=0
for f in $FILES; do
    id=$(echo "$f" | tr '/' '_')
    python3 tools/selfhost/gen_packed_probe.py "$f" > "$PROBE/$id.c" 2>/dev/null
    [ -s "$PROBE/$id.c" ] || continue
    clang --target=x86_64-elf -std=c11 -ffreestanding -fno-pie -fno-pic -mcmodel=kernel \
        -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -DARCH_X86_64 -I . -I build -I w32/include \
        -c "$PROBE/$id.c" -o "$PROBE/$id.clang.o" 2>/dev/null || continue
    "$HOST_TCC" -c -ffreestanding -fno-pic -I "$ROOT/build/selfhost/host-tcc-src/include" \
        -I . -I build -I w32/include -DARCH_X86_64 -o "$PROBE/$id.tcc.o" "$PROBE/$id.c" 2>/dev/null || continue
    n_probe=$((n_probe+1))
    cnt=$(python3 - "$PROBE/$id.clang.o" "$PROBE/$id.tcc.o" <<'PY'
import subprocess, sys
def syms(path):
    out = subprocess.run(['readelf','-sW',path],capture_output=True,text=True).stdout
    return {p[7]: int(p[2]) for p in (l.split() for l in out.splitlines())
            if len(p)>=8 and p[3]=='OBJECT' and p[7].startswith('p_size_')}
a, b = syms(sys.argv[1]), syms(sys.argv[2])
if a == b:
    print(len(a))
else:
    for k in sorted(set(a) | set(b)):
        if a.get(k) != b.get(k):
            print("mismatch: %s clang=%s tcc=%s" % (k, a.get(k), b.get(k)), file=sys.stderr)
    sys.exit(1)
PY
    ) || cnt=""
    if [ -n "$cnt" ]; then
        n_structs=$((n_structs + cnt))
    else
        n_mismatch=$((n_mismatch+1))
    fi
done
[ "$n_mismatch" -eq 0 ] && [ "$n_structs" -ge 100 ] \
    && ok "packed-layout parity: $n_structs packed structs across $n_probe files, sizeof matches clang<->tcc" \
    || bad "packed-layout parity broken ($n_mismatch mismatching files; $n_structs structs compared)"

if [ "$FAILED" -ne 0 ]; then
    echo "[selfhost] sh5c FAIL: the tcc-built kernel does not pass the SH5c gate"
    exit 1
fi
echo "[selfhost] sh5c PASS: tcc compiles the kernel; aulink links it at the higher half ($n_assert assertions)"
exit 0
