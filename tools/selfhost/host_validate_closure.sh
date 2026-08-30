#!/usr/bin/env bash
# tools/selfhost/host_validate_closure.sh
# SH8 host-side validation: mirror tools/selfhost/sh8_closure.sh but run the
# tcc0-compile + aulink chain on the HOST, so the closure can be debugged
# before it is booted under QEMU.  This is a *development* aid checked into the
# tree so that future SH8 work has a fast inner loop.  It is not part of the
# guest closure; the acceptance gate is still tests/integration/cases/
# test_selfhost_closure.sh.
#
# WHAT IT DOES
#   1. compiles the narrowed, tcc-compilable closure libc with the seed tcc0
#   2. compiles the tcc sources to objects with the seed tcc0
#   3. links tcc1 with the host-built aulink across libcobj + tccobj + libtcc1.a
#   4. reports any residual undefined symbols
#
# Set HOST_VAL_CC to override the compiler (default: build/selfhost/tcc.elf, the
# seed tcc0 that gets stripped to /bin/tcc in the guest).
set -u

cd "$(dirname "$0")/../.." || exit 1
ROOT=$PWD

TCC0=${HOST_VAL_CC:-"$ROOT/build/selfhost/host-tcc-src/tcc"}
CC_SRCDIR="$ROOT/build/selfhost/tcc-src"
# libc headers use #include_next <stddef.h>, so the compiler's own include dir
# (which tcc adds automatically in the guest via CONFIG_TCCDIR=/apps/tcc/include)
# must be on the path here on the host.
INC_LIBC="-I$ROOT/lib/libc/include -I$CC_SRCDIR/include"
INC_TCC="-I$CC_SRCDIR -I$CC_SRCDIR/include -I$ROOT/lib/libc/include"
TOOL="${HOST_VAL_AULINK:-/tmp/hostaulink}"
ASM_TOOL="${HOST_VAL_ASM:-/tmp/hostmini}"

OUT=/tmp/sh8val
rm -rf "$OUT"; mkdir -p "$OUT/lib" "$OUT/tcc"

DEF="-D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic -O2"

echo "== compiler: $($TCC0 -v 2>&1 | head -1) =="
echo "== host aulink: $TOOL =="

fail=0

# ---- 1. narrow closure libc (tcc-compilable subset) -----------------------
# Each entry is  <src-rel-to-lib/libc/src>:<object-name>.  All members must be
# compilable by tcc; the objects whose sources are not here are ones tcc's own
# codegen/runtime never references (or that do not compile under tcc):
#   dropped: compat.o(_Complex), posix_extra.o/posix_spawn.o/utsname.o/regex.o
#            resource.o/pwd.o/q10_stubs.o/apkg.o(__ATOMIC_*/big) and pthread/*.
LIBC_SPECS="
libc.c:libc
malloc.c:malloc
env.c:env
string_extra.c:string
stdlib_extra.c:stdlib
stdio_extra.c:stdio
dirent.c:dirent
math_extra.c:math
time_extra.c:time
getopt.c:getopt
progpath.c:prog
"
for spec in $LIBC_SPECS; do
  src="${spec%%:*}"; name="${spec##*:}"
  "$TCC0" -c $DEF $INC_LIBC -o "$OUT/lib/$name.o" \
      "$ROOT/lib/libc/src/$src" 2>>"$OUT/err.log" \
    || { echo "  [fail] libc $src"; fail=1; }
done
# tcc_builtins.c lives in tools/selfhost (staged to /src/libc/tcc_builtins.c).
"$TCC0" -c $DEF $INC_LIBC -o "$OUT/lib/bi.o" \
    "$ROOT/tools/selfhost/tcc_builtins.c" 2>>"$OUT/err.log" \
  || { echo "  [fail] libc tcc_builtins"; fail=1; }
# crt0 (C twin), syscall/sigreturn/setjmp assembled by mini-asm.
"$TCC0" -c $DEF $INC_LIBC -o "$OUT/lib/crt0.o" \
    "$ROOT/tools/selfhost/tcc_crt0.s" 2>>"$OUT/err.log" \
  || { echo "  [fail] crt0"; fail=1; }
# SH8 runtime shims (dlopen/dlsym/sem_* stubs), see tcc_closure_runtime.c.
"$TCC0" -c $DEF $INC_LIBC -o "$OUT/lib/rt.o" \
    "$ROOT/tools/selfhost/tcc_closure_runtime.c" 2>>"$OUT/err.log" \
  || { echo "  [fail] closure runtime"; fail=1; }
# syscall.asm lives in src/ (not crt/); sigreturn/setjmp in crt/.
"$ASM_TOOL" -f elf64 -I"$ROOT" -o "$OUT/lib/syscall.o" \
    "$ROOT/lib/libc/src/syscall.asm" 2>>"$OUT/err.log" \
  || { echo "  [fail] asm syscall"; fail=1; }
for s in sigreturn setjmp; do
  "$ASM_TOOL" -f elf64 -I"$ROOT" -o "$OUT/lib/$s.o" \
      "$ROOT/lib/libc/crt/$s.asm" 2>>"$OUT/err.log" \
    || { echo "  [fail] asm $s"; fail=1; }
done

# ---- 2. tcc sources -> objects with tcc0 ---------------------------------
TCC_SRCS="tcc tccpp tccgen tccdbg tccelf tccasm x86_64-gen x86_64-link i386-asm libtcc tcc_glue"
for f in $TCC_SRCS; do
  "$TCC0" -c -DONE_SOURCE=0 -DCONFIG_TCC_STATIC -ffreestanding \
      -fno-stack-protector -fno-pie -fno-pic $INC_TCC -o "$OUT/tcc/$f.o" \
      "$CC_SRCDIR/$f.c" 2>>"$OUT/err.log" \
    || { echo "  [fail] tcc $f"; fail=1; }
done

echo "== libc objects: $(ls "$OUT/lib" | wc -l) =="
echo "== tcc objects:  $(ls "$OUT/tcc" | wc -l) =="

# ---- 3. link tcc1 ----------------------------------------------------------
mkdir -p "$OUT/link"
"$TOOL" -T "$ROOT/lib/libc/user.ld" -o "$OUT/link/tcc1.elf" \
    "$OUT/lib" "$OUT/tcc" "$ROOT/build/selfhost/libtcc1.a" 2>>"$OUT/err.log" \
  || { echo "  [fail] aulink tcc1"; fail=1; }

echo "== tcc1.elf size: $(stat -c%s "$OUT/link/tcc1.elf" 2>/dev/null || echo n/a) =="

if [ -f "$OUT/link/tcc1.elf" ]; then
  echo "== residual undefined symbols (if any) =="
  nm -u "$OUT/link/tcc1.elf" 2>/dev/null | grep -v '^$' || true
fi

echo "== error log tail (if any) =="
[ -s "$OUT/err.log" ] && tail -n 30 "$OUT/err.log" || echo "  (clean)"

exit $fail
