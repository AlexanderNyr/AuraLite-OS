#!/usr/bin/env bash
# tests/unit/test_gdb_scripts.sh -- RESIDUE2 T9 gate: the GDB helper kit
# loads against the real kernel image and its printers render real values.
#
# What is pinned, in increasing order of "actually works":
#   1. tools/gdb/pretty.py is valid python (py_compile);
#   2. gdb sources aura.gdb (which sources pretty.py) without error --
#      against build/kernel.elf with its DWARF;
#   3. the const default theme prints THROUGH the pretty-printer with a
#      #RRGGBB color (file-backed .rodata: no live target needed, so the
#      gate runs anywhere gdb runs);
#   4. the tcb type resolves from DWARF and the window-table command
#      degrades cleanly without a live inferior (it must print a hint,
#      not crash gdb).
#
# Skips loudly (not red) when gdb or build/kernel.elf is absent -- the
# CI image installs gdb for exactly this gate.

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

fail=0
note() { printf '  [gdb] %s\n' "$1"; }

if ! command -v gdb >/dev/null 2>&1; then
    echo "[gdb] SKIP: gdb not installed"
    exit 0
fi
if [ ! -s build/kernel.elf ]; then
    echo "[gdb] SKIP: build/kernel.elf absent (make kernel)"
    exit 0
fi

# 1. python validity (cfile into /tmp: py_compile must not leave a
# tools/gdb/__pycache__ behind in a clean tree)
if python3 -c "import py_compile; py_compile.compile('tools/gdb/pretty.py', cfile='/tmp/aura_pretty.pyc')" 2>/dev/null; then
    note "OK   pretty.py compiles"
else
    note "FAIL pretty.py does not compile"
    fail=1
fi

# 2+3. load + render through the printer
OUT="$(gdb --batch \
      -ex 'source tools/gdb/aura.gdb' \
      -ex 'print default_theme' \
      -ex 'ptype struct tcb' \
      build/kernel.elf 2>&1)" || fail=1
echo "$OUT" | grep -q "gui_theme {" && echo "$OUT" | grep -q "#2F60C0"
rc=$?
if [ "$rc" -eq 0 ]; then
    note "OK   theme prints through the pretty-printer (#RRGGBB seen)"
else
    note "FAIL theme printer output missing -- got:"
    echo "$OUT" | head -5 | sed 's/^/         /'
    fail=1
fi
echo "$OUT" | grep -q "struct tcb {" && echo "$OUT" | grep -q "char name"
rc=$?
if [ "$rc" -eq 0 ]; then
    note "OK   struct tcb resolves from kernel DWARF"
else
    note "FAIL struct tcb did not resolve"
    fail=1
fi

# 4. commands degrade without a live inferior
OUT2="$(gdb --batch \
      -ex 'source tools/gdb/aura.gdb' \
      -ex 'aura_windows' \
      build/kernel.elf 2>&1)"
if echo "$OUT2" | grep -qE "attach to a live target|live window|Cannot access"; then
    note "OK   aura_windows degrades cleanly without a target"
else
    note "FAIL aura_windows crashed or printed nothing"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "[gdb] all T9 helper-kit gates passed"
else
    echo "[gdb] FAILED"
fi
exit "$fail"
