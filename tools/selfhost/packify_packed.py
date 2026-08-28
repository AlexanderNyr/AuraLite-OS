#!/usr/bin/env python3
"""One-shot transformer for SELFHOST SH5c: wrap every aggregate whose
declaration carries a trailing __attribute__((packed)) in a __TINYC__-guarded
#pragma pack(push,1) / #pragma pack(pop) pair.

Why: the pinned tcc mob (2ba12e8) parses __attribute__((packed)) but never
applies it to member placement (tccgen.c only consults a.packed for the
struct-level final alignment; member packing is driven by pragma_pack).
#pragm pack(push,1) is the one spelling that packs members on both tcc and
clang/gcc.  The wrap is inert on clang/gcc (guarded by __TINYC__), so the
shipped kernel layout is byte-identical to before.

Handled shape (the only one the x86_64 kernel tree uses):
    [typedef] struct|union [tag] { ... } __attribute__((packed)) [decls] ;
The attribute may carry further attributes before/after it on the same
declaration; the wrap covers the WHOLE declaration from its first keyword
to the terminating ';'.

Usage: tools/selfhost/packify_packed.py <file.c> [<file.c> ...]   (in-place)
"""

import re
import sys

ATTR = "__attribute__((packed))"
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\s*$")
AGG = ("struct", "union")


def skip_blank_bw(text, i):
    while i >= 0 and text[i] in " \t\r\n":
        i -= 1
    return i


def find_open_brace(text, attr_pos):
    """The attribute follows the aggregate's closing '}': find it, then brace-
    match backward to the '{' that opens the attributed aggregate."""
    i = skip_blank_bw(text, attr_pos - 1)
    if i < 0 or text[i] != "}":
        return None  # not a trailing-placement attribute
    depth = 1
    i -= 1
    while i >= 0 and depth > 0:
        if text[i] == "}":
            depth += 1
        elif text[i] == "{":
            depth -= 1
        i -= 1
    if depth != 0:
        return None
    return i + 1  # position of '{'


def find_decl_start(text, open_brace):
    """Walk backward from the aggregate's '{' over 'tag struct/union' and an
    optional leading 'typedef'; returns the index where the declaration
    starts."""
    i = skip_blank_bw(text, open_brace - 1)
    m = IDENT.search(text[: i + 1]) if i >= 0 else None
    if not m:
        return None
    first = m.group(0).strip()
    pos = i + 1 - len(first)
    if first in AGG:
        # no tag: "struct {" — check for a leading typedef
        k = skip_blank_bw(text, pos - 1)
        m3 = IDENT.search(text[: k + 1]) if k >= 0 else None
        if m3 and m3.group(0).strip() == "typedef":
            pos = k + 1 - len(m3.group(0))
        return pos
    # first is the tag; before it must be struct/union
    k = skip_blank_bw(text, pos - 1)
    m2 = IDENT.search(text[: k + 1]) if k >= 0 else None
    if not m2 or m2.group(0).strip() not in AGG:
        return None
    pos = k + 1 - len(m2.group(0))
    k = skip_blank_bw(text, pos - 1)
    m3 = IDENT.search(text[: k + 1]) if k >= 0 else None
    if m3 and m3.group(0).strip() == "typedef":
        pos = k + 1 - len(m3.group(0))
    return pos


def find_stmt_end(text, attr_end):
    """Walk forward from just after the attribute to the ';' that ends the
    declaration, tracking bracket depth (initializers may nest)."""
    depth = 0
    i, n = attr_end, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            continue
        if c == "'":
            i += 1
            while i < n and text[i] != "'":
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == ";" and depth == 0:
            return i
        i += 1
    return None


def attr_positions(text):
    """Indexes of every ATTR occurrence outside comments/strings."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
        elif c == "'":
            i += 1
            while i < n and text[i] != "'":
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
        else:
            if text.startswith(ATTR, i):
                out.append(i)
                i += len(ATTR)
                continue
            i += 1
    return out


def transform(path):
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    attrs = attr_positions(text)
    if not attrs:
        return 0
    edits = []  # (position, inserted_text)
    for a in attrs:
        ob = find_open_brace(text, a)
        if ob is None:
            raise SystemExit(
                "%s: packed at byte %d is not in trailing position after '}'"
                % (path, a)
            )
        start = find_decl_start(text, ob)
        if start is None:
            raise SystemExit(
                "%s: cannot find declaration start for packed at byte %d" % (path, a)
            )
        end = find_stmt_end(text, a + len(ATTR))
        if end is None:
            raise SystemExit(
                "%s: no terminating ';' for packed at byte %d" % (path, a)
            )
        line_start = text.rfind("\n", 0, start) + 1
        indent = re.match(r"[ \t]*", text[line_start:start]).group(0)
        pre = (
            "#if defined(__TINYC__)\n"
            + indent
            + "#pragma pack(push, 1)\n"
            + indent
            + "#endif\n"
            + indent
        )
        post = (
            "\n"
            + indent
            + "#if defined(__TINYC__)\n"
            + indent
            + "#pragma pack(pop)\n"
            + indent
            + "#endif"
        )
        edits.append((start, pre))
        edits.append((end + 1, post))
    for pos, ins in sorted(edits, key=lambda e: -e[0]):
        text = text[:pos] + ins + text[pos:]
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    return len(attrs)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    total = 0
    for path in sys.argv[1:]:
        n = transform(path)
        total += n
        print("%-46s %d site(s)" % (path, n))
    print("total: %d" % total)


if __name__ == "__main__":
    main()
