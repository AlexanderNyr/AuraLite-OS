#!/usr/bin/env python3
"""gen_w32_api_table.py — WIN32_PLAN.md W32-8.

Generates the supported-function table in docs/win32.md from the export
table in w32/src/w32_bind.c.

The plan requires the table be generated "so it cannot drift (the sdk-check
pattern)". A hand-maintained list of API functions is wrong the moment
somebody adds an export, and the failure is silent: the documentation
promises a function that does not exist, or omits one that does.

Two modes:
  --write   rewrite the table in docs/win32.md between its markers
  --check   exit 1 if the file is out of date (used by make test-unit)
"""
import re
import subprocess
import sys

BIND = "w32/src/w32_bind.c"
DOC = "docs/win32.md"
BEGIN = "<!-- BEGIN GENERATED: w32 export table -->"
END = "<!-- END GENERATED: w32 export table -->"

DLL_LABEL = {
    "K32": "KERNEL32.dll",
    "U32": "USER32.dll",
    "G32": "GDI32.dll",
}


def collect():
    """Parse the export table: { DLL_MACRO, "Name", (void *)impl }."""
    src = open(BIND).read()
    # Only the table body, so a mention of a name in a comment elsewhere in
    # the file cannot add a phantom row.
    body = src.split("static const w32_export_t exports[] = {", 1)[1]
    body = body.split("\n};", 1)[0]

    rows = re.findall(r'\{\s*(K32|U32|G32)\s*,\s*"([^"]+)"', body)
    by_dll = {}
    for macro, name in rows:
        by_dll.setdefault(DLL_LABEL[macro], []).append(name)
    for k in by_dll:
        by_dll[k] = sorted(by_dll[k])
    return by_dll


def render(by_dll):
    total = sum(len(v) for v in by_dll.values())
    out = [BEGIN,
           "",
           f"*{total} functions across {len(by_dll)} modules. This table is "
           "generated from",
           "`w32/src/w32_bind.c` by `tools/gen_w32_api_table.py`; edit the "
           "export table, not this list.*",
           ""]
    for dll in sorted(by_dll):
        out.append(f"**{dll}** ({len(by_dll[dll])})")
        out.append("")
        # Three columns keeps the table readable without horizontal scroll.
        names = by_dll[dll]
        for i in range(0, len(names), 3):
            chunk = names[i:i + 3]
            out.append("- " + " · ".join(f"`{n}`" for n in chunk))
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
        print(f"gen_w32_api_table: markers not found in {DOC}",
              file=sys.stderr)
        return 2

    pre = doc.split(BEGIN, 1)[0]
    post = doc.split(END, 1)[1]
    updated = pre + new_block + post

    if mode == "--write":
        if updated != doc:
            open(DOC, "w").write(updated)
            print(f"  [w32] {DOC} export table regenerated "
                  f"({sum(len(v) for v in by_dll.values())} functions)")
        else:
            print(f"  [w32] {DOC} already up to date")
        return 0

    if updated != doc:
        print(f"FAIL: {DOC} is out of date with {BIND}.", file=sys.stderr)
        print("      Run: python3 tools/gen_w32_api_table.py --write",
              file=sys.stderr)
        return 1
    n = sum(len(v) for v in by_dll.values())
    print(f"  PASS: {DOC} matches {BIND} ({n} functions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
