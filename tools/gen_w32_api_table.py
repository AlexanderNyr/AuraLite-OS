#!/usr/bin/env python3
"""gen_w32_api_table.py — WIN32_PLAN.md W32-8, extended in W32APP_PLAN.md W32A-18.

Generates the supported-function table in docs/win32.md from two committed
sources that cannot lie without failing a build:

  - the export table in `w32/src/w32_bind.c` (what the personality binds), and
  - the D9 implementation class of every ledger symbol in
    `w32/app_ledger/*.imports` (REAL / FAIL-CLEAN / REFUSE, per §D9).

The plan requires the table be generated "so it cannot drift (the sdk-check
pattern)". A hand-maintained list is wrong the moment somebody adds an export,
and the failure is silent: the documentation promises a function that does not
exist, omits one that does, or -- the W32A-18 addition -- calls a FAIL-CLEAN
function REAL and lets a caller proceed on a false premise. Every module the
export table binds is listed, and every function carries the D9 class the
ledgers record. A function no committed ledger mentions defaults to REAL: it is
bound and exported, and FAIL-CLEAN/REFUSE are recorded explicitly in the
ledgers (a class change is a reviewed ledger edit, never a silent default).

Two modes:
  --write   rewrite the table in docs/win32.md between its markers
  --check   exit 1 if the file is out of date (used by make test-unit)
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIND = os.path.join(ROOT, "w32", "src", "w32_bind.c")
DOC = os.path.join(ROOT, "docs", "win32.md")
LEDGER_GLOB = os.path.join(ROOT, "w32", "app_ledger", "*.imports")
BEGIN = "<!-- BEGIN GENERATED: w32 export table -->"
END = "<!-- END GENERATED: w32 export table -->"

# Order the D9 classes are presented in, worst-news-last so the honest
# limitations are never buried above the fold within a module.
CLASS_ORDER = ["REAL", "FAIL-CLEAN", "REFUSE"]
CLASS_LABEL = {
    "REAL": "REAL",
    "FAIL-CLEAN": "FAIL-CLEAN (binds, then reports honest failure)",
    "REFUSE": "REFUSE (load-time refusal naming the symbol)",
}


def module_label(name):
    """Canonical display label: UPPER stem + lower extension (PE convention)."""
    stem, dot, ext = name.rpartition(".")
    if not dot:
        return name
    return stem.upper() + "." + ext.lower()


def ledger_classes():
    """Union of D9 classes across the committed ledgers: {(module,name): class}."""
    cls = {}
    for f in sorted(glob.glob(LEDGER_GLOB)):
        for ln in open(f):
            m = re.match(r"^(\S+\.\w+)\s+(\S+)\s+(REAL|FAIL-CLEAN|REFUSE)\b", ln)
            if m:
                key = (m.group(1).lower(), m.group(2))
                c = m.group(3)
                if key in cls and cls[key] != c:
                    raise SystemExit(
                        "gen_w32_api_table: ledgers disagree on the D9 class "
                        "of %s!%s (%s vs %s) -- a class change is a reviewed "
                        "ledger edit, not two truths" % (
                            key[0], key[1], cls[key], c))
                cls[key] = c
    return cls


def collect():
    """Parse the export table -> { module_label: { name: D9-class } }.

    Handles both row forms in the table: macro rows `{ K32, "Name", ... }`
    (the #define map turns the macro into a DLL name) and literal rows
    `{ "shell32.dll", "Name", ... }`.
    """
    src = open(BIND).read()
    defs = dict(re.findall(r'#define\s+([A-Z0-9_]+)\s+"([^"]+)"', src))
    # Only the table body, so a mention of a name in a comment elsewhere in
    # the file cannot add a phantom row.
    body = src.split("static const w32_export_t exports[] = {", 1)[1]
    body = body.split("\n};", 1)[0]

    cls = ledger_classes()
    by_dll = {}
    pairs = []
    for macro, name in re.findall(r'\{\s*([A-Z0-9_]+)\s*,\s*"([^"]+)"', body):
        if macro in defs:
            pairs.append((defs[macro], name))
    for dll, name in re.findall(
            r'\{\s*"([^"]+\.(?:dll|drv))"\s*,\s*"([^"]+)"', body):
        pairs.append((dll, name))

    for dll, name in pairs:
        label = module_label(dll)
        klass = cls.get((dll.lower(), name), "REAL")
        by_dll.setdefault(label, {})[name] = klass
    return by_dll


def render(by_dll):
    total = sum(len(v) for v in by_dll.values())
    counts = {c: 0 for c in CLASS_ORDER}
    for funcs in by_dll.values():
        for k in funcs.values():
            counts[k] = counts.get(k, 0) + 1
    summary = ", ".join("%d %s" % (counts[c], c)
                        for c in CLASS_ORDER if counts.get(c))
    out = [BEGIN,
           "",
           "*%d functions across %d modules (%s). This table is generated "
           "from" % (total, len(by_dll), summary),
           "`w32/src/w32_bind.c` and the D9 classes in "
           "`w32/app_ledger/*.imports` by",
           "`tools/gen_w32_api_table.py`; edit the export table or the "
           "ledgers, not this list.*",
           "",
           "*D9 class per function: **REAL** — full behaviour for the "
           "ladder's flows; **FAIL-CLEAN** — binds, then reports honest "
           "failure (never a success-shaped lie); **REFUSE** — load-time "
           "refusal naming the symbol. A function no ledger records defaults "
           "to REAL.*",
           ""]
    for dll in sorted(by_dll):
        funcs = by_dll[dll]
        by_class = {}
        for name, k in funcs.items():
            by_class.setdefault(k, []).append(name)
        head_counts = ", ".join(
            "%d %s" % (len(by_class[c]), c)
            for c in CLASS_ORDER if c in by_class)
        out.append("**%s** (%d) — %s" % (dll, len(funcs), head_counts))
        out.append("")
        for c in CLASS_ORDER:
            if c not in by_class:
                continue
            names = sorted(by_class[c])
            if c != "REAL":
                out.append("*%s*" % CLASS_LABEL[c])
            # Three columns keeps the table readable without horizontal scroll.
            for i in range(0, len(names), 3):
                chunk = names[i:i + 3]
                out.append("- " + " · ".join("`%s`" % n for n in chunk))
            out.append("")
    out.append(END)
    return "\n".join(out)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "--check"
    by_dll = collect()
    if not by_dll:
        print("gen_w32_api_table: parsed no exports; refusing to write an "
              "empty table", file=sys.stderr)
        return 2

    new_block = render(by_dll)
    doc = open(DOC).read()

    if BEGIN not in doc or END not in doc:
        print("gen_w32_api_table: markers not found in %s" % DOC,
              file=sys.stderr)
        return 2

    pre = doc.split(BEGIN, 1)[0]
    post = doc.split(END, 1)[1]
    updated = pre + new_block + post
    n = sum(len(v) for v in by_dll.values())

    if mode == "--write":
        if updated != doc:
            open(DOC, "w").write(updated)
            print("  [w32] %s export table regenerated (%d functions, "
                  "%d modules)" % (DOC, n, len(by_dll)))
        else:
            print("  [w32] %s already up to date" % DOC)
        return 0

    if updated != doc:
        print("FAIL: %s is out of date with %s." % (DOC, BIND), file=sys.stderr)
        print("      Run: python3 tools/gen_w32_api_table.py --write",
              file=sys.stderr)
        return 1
    print("  PASS: %s matches the export table + ledgers (%d functions, "
          "%d modules)" % (DOC, n, len(by_dll)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
