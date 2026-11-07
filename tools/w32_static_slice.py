#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""tools/w32_static_slice.py -- RESIDUE3_PLAN.md C8, box 2 (RES-57).

The five W32 app ledgers record 1694 `static-only` import markers: 1670 by name
and 24 by ordinal, which is 820 distinct named DLL/symbol pairs.  They are
symbols the personality's export tables bind and that no pinned application was
ever seen *calling*.  A marker like that is a claim about
reachability that nothing has tested from the guest side -- the apps import
them, but nobody has watched `LoadLibraryA` + `GetProcAddress` hand a real
pointer back for them.

This tool turns the markers into a fixture:

  * it reads the ledgers, keeps the `static-only` pairs;
  * it reads `w32/src/w32_bind.c`'s export table (the longhand `{ DLL, "Name"`
    rows, resolved through the `#define`s at the top);
  * it writes `build/w32_static_slice.h`: one name pair per line, split into
    EXPORTED (the table has it -- the guest must get a non-NULL pointer back)
    and ABSENT (the table does not -- the guest must get NULL, and the fixture
    names it, because a static-only marker over a symbol the personality does
    not export is a ledger error worth seeing).

The guest fixture (`w32/tests/w32c8_static_slice.c`) resolves all of them by
hand and calls a hand-picked slice whose signatures are known, so the
marker count drops by measurement rather than by relabelling: after this gate
runs, every pair in the file has a receipt from the machine.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEDGER_DIR = os.path.join(ROOT, "w32", "app_ledger")
BIND_C = os.path.join(ROOT, "w32", "src", "w32_bind.c")
# The export surface is two tables: the longhand D7 table and the generated
# FAIL-CLEAN stubs.  A parser that reads only the first reports 9 exported
# symbols as absent -- measured, and the reason this list has two files.
STUBS_C = os.path.join(ROOT, "w32", "src", "w32_stubs_gen.c")
OUT_H = os.path.join(ROOT, "build", "w32_static_slice.h")
OUT_TXT = os.path.join(ROOT, "build", "w32_static_slice.txt")


ORDINALS = []


def parse_ledgers():
    """Return {(dll, sym): [ledger names]} for the static-only markers."""
    marks = {}
    for name in sorted(os.listdir(LEDGER_DIR)):
        if not name.endswith(".imports"):
            continue
        for line in open(os.path.join(LEDGER_DIR, name), encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("imports_total"):
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            dll, sym = parts[0], parts[1]
            if not dll.endswith(".dll"):
                continue
            if "static-only" not in parts[2:]:
                continue
            if sym.startswith("#"):          # ordinal import: no name to resolve
                ORDINALS.append((dll.lower(), sym))
                continue
            marks.setdefault((dll.lower(), sym), []).append(name)
    return marks


DEF_RE = re.compile(r'^#define\s+(\w+)\s+"([^"]+\.dll)"\s*$', re.IGNORECASE)
# A row is `{ MACRO, "Name", ... }` or `{ "dll.name.dll", "Name", ... }` --
# both spellings appear in the table, and the second one is the reason a
# macro-only parser reports 72 "absent" symbols that are in fact exported.
ROW_RE = re.compile(r'^\s*\{\s*(?:"([^"]+\.dll)"|(\w+))\s*,\s*"([^"]+)"\s*,',
                    re.IGNORECASE)


def parse_stub_exports():
    """Only the generated stub table: these take `(void)`, so the fixture can
    call one with no arguments and it cannot walk off the stack."""
    rows = []
    for line in open(STUBS_C, encoding="utf-8"):
        m = ROW_RE.match(line)
        if m and m.group(1):
            rows.append((m.group(1).lower(), m.group(3)))
    return set(rows)


def parse_bind_exports():
    """The longhand D7 table only."""
    defines, rows = {}, []
    for line in open(BIND_C, encoding="utf-8"):
        m = DEF_RE.match(line)
        if m:
            defines[m.group(1)] = m.group(2).lower()
            continue
        m = ROW_RE.match(line)
        if not m:
            continue
        if m.group(1):
            rows.append((m.group(1).lower(), m.group(3)))
        elif m.group(2) in defines:
            rows.append((defines[m.group(2)], m.group(3)))
    return set(rows)


def parse_exports():
    defines, rows = {}, []
    for line in open(BIND_C, encoding="utf-8"):
        m = DEF_RE.match(line)
        if m:
            defines[m.group(1)] = m.group(2).lower()
            continue
        m = ROW_RE.match(line)
        if not m:
            continue
        if m.group(1):
            rows.append((m.group(1).lower(), m.group(3)))
        elif m.group(2) in defines:
            rows.append((defines[m.group(2)], m.group(3)))
    for line in open(STUBS_C, encoding="utf-8"):
        m = ROW_RE.match(line)
        if m and m.group(1):
            rows.append((m.group(1).lower(), m.group(3)))
    return set(rows)


def _unused():  # keeps the old loop shape readable in diffs
    defines, rows = {}, []
    for line in []:
        m = DEF_RE.match(line)
        if m:
            defines[m.group(1)] = m.group(2).lower()
            continue
        m = ROW_RE.match(line)
        if not m:
            continue
        if m.group(1):
            rows.append((m.group(1).lower(), m.group(3)))
        elif m.group(2) in defines:
            rows.append((defines[m.group(2)], m.group(3)))
    return set(rows)


def main():
    marks = parse_ledgers()
    sum_suppressed = [len(ORDINALS) + sum(len(v) for v in marks.values())]
    exports = parse_exports()
    stubs = parse_stub_exports()
    # A symbol present in BOTH tables is a real implementation that shadows
    # the generated stub (w32_resolve prefers the longhand table).  Calling
    # one with fabricated arguments is not a binding test: measured, the slice
    # went through user32!CreateIconIndirect -- a real function -- and faulted
    # at CR2=0xa4 because its first argument was NULL.  Calls are restricted
    # to stubs that nothing shadows, which by construction take no arguments.
    shadowed = parse_bind_exports()
    have = sorted(p for p in marks if p in exports)
    # The representative slice: static-only symbols the fixture can CALL as
    # well as resolve.  Three per DLL, taken from the generated stub table so
    # the call is argument-free and the answer is the stub's own receipt.
    per_dll, callable_ = {}, []
    for d, sym in have:
        if (d, sym) not in stubs or (d, sym) in shadowed:
            continue
        # msvcrt's surface is CRT machinery: data exports (`_acmdln`), the SEH
        # dispatcher, C++ terminate.  Calling one of those with fabricated
        # registers is not a test of binding, it is a crash -- measured: the
        # first version of this slice called msvcrt!__C_specific_handler and
        # faulted at RIP 0x40007bf8 (CR2=0x42) before printing a receipt.
        # They stay in the resolution list (that is what RES-57 asks about)
        # and leave the call slice.
        if d == "msvcrt.dll" or sym.startswith("_"):
            continue
        # The C++ runtime stubs are FAIL-CLEAN in name only: `?terminate`,
        # `_CxxThrowException` and friends *abort the run* by design, which is
        # not a receipt about binding.  Measured -- calling
        # msvcrt!??1type_info@@UEAA@XZ ended the guest with W32-CXX-TERMINATE.
        if sym.startswith("?") or sym.startswith("_Cxx") or \
           sym.startswith("__Cxx") or sym.startswith("_Xcpt") or \
           "terminate" in sym:
            continue
        if per_dll.get(d, 0) >= 8:
            continue
        per_dll[d] = per_dll.get(d, 0) + 1
        callable_.append((d, sym))
    absent = sorted(p for p in marks if p not in exports)
    total = len(marks)

    if not have:
        print("w32_static_slice: no static-only marker resolves -- is the "
              "export table still parsed?", file=sys.stderr)
        return 1

    dlls = sorted({d for d, _ in have})
    out = []
    out.append("/* Generated by tools/w32_static_slice.py -- do not edit.\n"
               " * RESIDUE3_PLAN.md C8 box 2: every `static-only` ledger pair,\n"
               " * split by whether the personality's export tables provide it. */\n")
    out.append("#define K_SO_ORDINALS %d\n" % len(ORDINALS))
    out.append("#define K_SO_MARKERS %d\n" % sum_suppressed[0])
    out.append("#define K_SO_TOTAL %d\n" % total)
    out.append("#define K_SO_PRINTED %d\n" % len(have))
    out.append("#define K_SO_MISSING %d\n" % len(absent))
    out.append("#define K_SO_DLLS %d\n" % len(dlls))
    out.append("#define K_SO_CALLS %d\n\n" % len(callable_))
    out.append("static const w32_so_pair_t k_so_printed[] = {\n")
    for d, sym in have:
        out.append('    { "%s", "%s" },\n' % (d, sym))
    out.append("};\n\nstatic const w32_so_pair_t k_so_missing[] = {\n")
    for d, sym in absent:
        out.append('    { "%s", "%s" },\n' % (d, sym))
    out.append("};\n\nstatic const w32_so_pair_t k_so_callable[] = {\n")
    for d, sym in callable_:
        out.append('    { "%s", "%s" },\n' % (d, sym))
    out.append("};\n\nstatic const char *const k_so_dlls[] = {\n")
    for d in dlls:
        out.append('    "%s",\n' % d)
    out.append("};\n")
    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("".join(out))

    with open(OUT_TXT, "w", encoding="utf-8") as f:
        for d, s in have:
            f.write("have %s %s %s\n" % (d, s, ",".join(sorted(set(marks[(d, s)])))))
        for d, s in absent:
            f.write("absent %s %s %s\n" % (d, s, ",".join(sorted(set(marks[(d, s)])))))

    per_dll = {}
    for d, _ in marks:
        per_dll[d] = per_dll.get(d, 0) + 1
    print("w32_static_slice: %d static-only markers (%d by name, %d by ordinal),"
          " %d distinct named pairs"
          % (sum_suppressed[0], sum(len(v) for v in marks.values()),
             len(ORDINALS), total))
    print("  callable slice (argument-free stubs): %d, over %d DLLs"
          % (len(callable_), len(per_dll)))
    print("  exported by the personality: %d   absent from the table: %d   "
          "input DLLs: %d" % (len(have), len(absent), len(dlls)))
    for d in sorted(per_dll):
        print("    %-16s %4d" % (d, per_dll[d]))
    if absent:
        print("  absent (these markers name symbols the table does not export):")
        for d, s in absent[:20]:
            print("    %s!%s" % (d, s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
