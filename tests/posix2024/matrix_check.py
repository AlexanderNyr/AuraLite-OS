#!/usr/bin/env python3
"""
tests/posix2024/matrix_check.py -- POSIX.1-2024 compliance-matrix drift check.

POSIX2024_PLAN.md phase Q12.  Reads the compliance matrix
(docs/posix2024_compliance.md), extracts every row symbol, and proves that
each one actually resolves to a defined symbol in the built libc archive
(build/lib/libaurac.a).  If the docs claim something the archive does not
ship, the gate fails: the matrix can never rot silently.

Row-cell grammar (see the matrix for the actual cells):
  - cells split on '/' and ','          -> "socket/connect/bind" = 3 symbols
  - "X_*" wildcards                     -> prefix-expanded, >=1 hit required
  - short tail tokens after a wildcard  -> completed against the wildcard
    prefix ("pthread_cleanup_push/pop"  -> "pthread_cleanup_pop")
  - prose cells (contain spaces)        -> skipped ("errno macro")
  - CAPS_UNDERSCORE cells               -> skipped (macros/constants, e.g.
    TCP_NODELAY; not link symbols)
  - tokens listed in the known-macros file are *verified* as real
    `#define`s in the public headers, not waved through.

Partial rows (status "partial") must equal the allowlist file exactly, in
both directions -- partials can neither grow silently nor vanish
undocumented.  "missing" rows are rejected outright.

Usage:
  matrix_check.py <matrix.md> <libaurac.a> <known_partials.txt> \
                  [known_macros.txt [include-dir]]

Exit status: 0 = matrix matches the archive; 1 = drift (details on stdout).
"""

import os
import re
import subprocess
import sys


def defined_symbols(archive):
    """Return the set of globally-defined symbols in `archive` via nm."""
    out = subprocess.run(["nm", "-A", archive], capture_output=True, text=True)
    syms = set()
    # nm -A format: "archive(obj.o):addr TYPE name" (defined) or
    #               "archive(obj.o): U name" (undefined, no address)
    for line in out.stdout.splitlines():
        m = re.match(r"^.*:([0-9a-fA-F]+) ([A-Za-z]) (\S+)$", line)
        if m and m.group(2) != "U":
            syms.add(m.group(3))
    return syms


def split_cell(cell):
    """Split a row's first cell into candidate symbol tokens."""
    tokens = []
    for part in cell.split("/"):
        for token in part.split(","):
            token = token.strip()
            if not token:
                continue
            tokens.append(token)
    return tokens


def complete_tokens(tokens):
    """Complete short tail tokens against the leading token's prefix.

    The matrix writes "pthread_cleanup_push/pop" for the push+pop pair; the
    bare "pop" must be completed to "pthread_cleanup_pop" using the leading
    token's stem (up to and including its last '_').  Tokens that already
    carry an underscore or a wildcard, or that belong to a cell whose first
    token has no stem (e.g. "write/read"), pass through untouched.
    """
    # The stem comes from the FIRST token only: the matrix writes
    # "pthread_cleanup_push/pop" (push is the family head, pop is its tail).
    # "strtok/strtok_r" is NOT a completion pair -- strtok has no stem.
    stem = tokens[0][: tokens[0].rfind("_") + 1] if "_" in tokens[0] else None
    if stem is None:
        return tokens
    out = []
    for i, t in enumerate(tokens):
        if i == 0 or "_" in t or "*" in t:
            out.append(t)
        else:
            out.append(stem + t)  # e.g. "pop" -> "pthread_cleanup_pop"
    return out


def is_prose(token):
    return any(c.isspace() for c in token)


def is_const_macro(token):
    # All-caps constants like TCP_NODELAY are #defines, not link symbols.
    return bool(re.fullmatch(r"[A-Z][A-Z0-9_]*", token))


def load_known_macros(path):
    if not path:
        return set()
    out = set()
    with open(path) as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if line:
                out.add(line.split()[0])
    return out


def macro_defined_anywhere(macro, include_dir):
    if not include_dir:
        return False
    pat = re.compile(r"#\s*define\s+" + re.escape(macro) + r"(?:\(|\s|$)")
    for root, _dirs, files in os.walk(include_dir):
        for name in files:
            if not name.endswith(".h"):
                continue
            p = os.path.join(root, name)
            try:
                with open(p, encoding="utf-8", errors="replace") as fh:
                    if pat.search(fh.read()):
                        return True
            except OSError:
                continue
    return False


def parse_rows(matrix_path):
    rows = []  # (status, token, note)
    with open(matrix_path, encoding="utf-8") as fh:
        for line in fh:
            if not line.startswith("|"):
                continue
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if len(cells) < 2:
                continue
            status_cell = cells[1]
            status = None
            if "\u2705" in status_cell:      # full
                status = "full"
            elif "\U0001F536" in status_cell:  # partial (orange diamond 🔶)
                status = "partial"
            elif "\u274c" in status_cell:      # missing
                status = "missing"
            if status is None:
                continue
            rows.append((status, cells[0], cells[2] if len(cells) > 2 else ""))
    return rows


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    matrix_path, archive, partials_path = sys.argv[1:4]
    macros_path = sys.argv[4] if len(sys.argv) > 4 else None
    include_dir = sys.argv[5] if len(sys.argv) > 5 else None

    syms = defined_symbols(archive)
    known_macros = load_known_macros(macros_path)
    with open(partials_path) as fh:
        allow_partials = set(
            line.split("#", 1)[0].strip()
            for line in fh
            if line.split("#", 1)[0].strip()
        )

    rows = parse_rows(matrix_path)
    n_full = n_partial = n_missing = 0
    checked, macro_ok, missing_syms, extra_partials = 0, 0, [], []
    seen_partials = set()

    for status, cell, note in rows:
        if status == "missing":
            n_missing += 1
            continue
        if status == "partial":
            n_partial += 1
            seen_partials.update(complete_tokens(split_cell(cell)))
            continue
        n_full += 1
        for token in complete_tokens(split_cell(cell)):
            if is_prose(token) or is_const_macro(token):
                continue
            checked += 1
            if token in syms:
                continue
            if token.endswith("*"):
                hits = [s for s in syms if s.startswith(token[:-1])]
                if hits:
                    continue
                missing_syms.append(f"{token} (wildcard, no archive hits)")
                continue
            if token in known_macros and macro_defined_anywhere(token, include_dir):
                macro_ok += 1
                continue
            missing_syms.append(token)

    # Partial rows must match the allowlist exactly, both directions.
    extra_partials = sorted(seen_partials - allow_partials)
    vanished_partials = sorted(allow_partials - seen_partials)

    fail = False
    if missing_syms:
        fail = True
        print("FAIL: matrix rows claim symbols the archive does not define:")
        for s in sorted(set(missing_syms)):
            print(f"  - {s}")
    if extra_partials:
        fail = True
        print("FAIL: partial rows in the matrix not in the allowlist "
              "(undocumented new partials):")
        for s in extra_partials:
            print(f"  - {s}")
    if vanished_partials:
        fail = True
        print("FAIL: allowlist partials absent from the matrix "
              "(partials must be declared, not silently dropped):")
        for s in vanished_partials:
            print(f"  - {s}")
    if n_missing:
        fail = True
        print(f"FAIL: {n_missing} row(s) marked MISSING in the matrix")
        for status, cell, note in rows:
            if status == "missing":
                print(f"  - {cell}: {note[:80]}")

    if not fail:
        print(f"matrix: OK -- {checked} symbols verified in {os.path.basename(archive)}"
              f" ({macro_ok} as header macros), {n_partial} partial rows == allowlist,"
              f" 0 missing")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
