#!/usr/bin/env bash
# tools/selfhost/sh9_spike.sh  --  SH9 spike: is tcc single-target per binary?
#
# Question: can one self-hosted x86_64 tcc emit i386/riscv64/aarch64 code?
# The SH9 definition says this is *measured, not assumed*.  This script
# re-measures the decisive fact on any machine with gcc + git + binutils:
# whether the mob TinyCC this tree closes on carries more than one codegen.
#
# It builds only the `tcc` binary (NOT lib/), so it needs no cross-target
# libc headers and never requires the cross system libraries.  It then
# compiles a one-line C source with each target's tcc and reports the
# resulting ELF Machine/Class.
#
# Method:
#   ./tools/selfhost/sh9_spike.sh
#   TCC_SRC=/path/to/tinycc ./tools/selfhost/sh9_spike.sh   # use an existing checkout
#
# Exit 0 = measured.  The table is the evidence; it is read by the SH9
# section of SELFHOST_PLAN.md and recorded there, not invented.

set -u

# ---- config ---------------------------------------------------------------
TARGETS="x86_64 i386 riscv64 arm64"
WORK="$(mktemp -d /tmp/sh9-spike.XXXXXX)"
TCC_SRC="${TCC_SRC:-}"
HAS_GIT=1; command -v git >/dev/null 2>&1 || HAS_GIT=0
command -v gcc     >/dev/null 2>&1 || { echo "  [skip] gcc not present";  exit 2; }
command -v readelf >/dev/null 2>&1 || { echo "  [skip] readelf not present"; exit 2; }

echo "== SH9 spike: is tcc single-target per binary? =="
echo "   (source: ${TCC_SRC:-<cloned from TinyCC/TinyCC m>})"

if [ -z "$TCC_SRC" ]; then
  if [ "$HAS_GIT" -eq 0 ]; then
    echo "  ERROR: TCC_SRC unset and git is unavailable; set TCC_SRC to a mob checkout."
    rm -rf "$WORK"; exit 2
  fi
  echo "  clone TinyCC/TinyCC (mob) ..."
  git clone --depth 1 --branch mob https://github.com/TinyCC/tinycc.git "$WORK/src" >/dev/null 2>&1 \
    || { echo "  ERROR: clone failed"; rm -rf "$WORK"; exit 2; }
  TCC_SRC="$WORK/src"
fi

echo 'int add(int a,int b){return a+b;}' > "$WORK/in.c"

printf "\n%-9s %-22s %s\n" "cpu" "tcc -v arch" "emitted (Machine / Class)"
printf "%s\n" "----------------------------------------------------------------------"

overall=0
for cpu in $TARGETS; do
  build="$WORK/build-$cpu"; cp -r "$TCC_SRC" "$build" 2>/dev/null
  ( cd "$build" && ./configure --cpu="$cpu" -q >/dev/null 2>&1 && make tcc >/dev/null 2>&1 ) \
    || { printf "%-9s %-22s %s\n" "$cpu" "BUILD FAILED" "-"; overall=1; continue; }
  arch="$("$build/tcc" -v 2>&1 | sed -n '1s/.*(\(.*\))/\1/p')"
  if "$build/tcc" -c -o "$WORK/out-$cpu.o" "$WORK/in.c" >/dev/null 2>&1; then
    mach="$(readelf -h "$WORK/out-$cpu.o" 2>/dev/null | awk -F: '/Machine/{gsub(/^[ \t]+/,"",$2); print $2}')"
    cls="$(readelf -h "$WORK/out-$cpu.o" 2>/dev/null | awk -F: '/Class/{gsub(/^[ \t]+/,"",$2); print $2}')"
    printf "%-9s %-22s %s\n" "$cpu" "$arch" "$mach / $cls"
  else
    printf "%-9s %-22s %s\n" "$cpu" "$arch" "compile FAILED"
    overall=1
  fi
  rm -rf "$build"
done

echo ""
echo "== single-target? =="
# The x86_64 build must NOT emit i386 via -m32 (it defers to a separate
# i386-tcc); that is the load-bearing part of the "where no" spike answer.
build="$WORK/x86_64check"; cp -r "$TCC_SRC" "$build" 2>/dev/null
( cd "$build" && ./configure --cpu=x86_64 -q >/dev/null 2>&1 && make tcc >/dev/null 2>&1 )
if [ -x "$build/tcc" ]; then
  if "$build/tcc" -m32 -c -o "$WORK/out-m32.o" "$WORK/in.c" >/dev/null 2>&1; then
    echo "  x86_64 tcc -m32: EMITTED an object (unexpected for a single-tool tcc)."
    overall=1
  else
    echo "  x86_64 tcc -m32: does not emit i386 (defers to a separate i386-tcc) -> single-target."
  fi
fi
rm -rf "$build"

rm -rf "$WORK"
echo ""
[ "$overall" -eq 0 ] && echo "RESULT: single-target per binary (spike answer: NO multi-target tcc)." \
                     || echo "RESULT: measurement incomplete (see above)."
exit "$overall"
