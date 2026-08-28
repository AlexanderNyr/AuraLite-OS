#!/usr/bin/env python3
"""gen_packed_probe.py -- SH5c packed-layout parity probe generator.

For every aggregate whose declaration carries a trailing
__attribute__((packed)) in the given source file, emit one probe symbol
whose SIZE is the aggregate's sizeof:

    const unsigned char p_size_<ident>[sizeof(<type-expr>)];

The test compiles the probe twice -- once with the kernel's clang flags,
once with the host tcc and its #pragma pack(push,1) interpretation of the
SH5c wraps -- and compares every symbol's size.  A tcc that silently
ignores the attribute (tccgen.c only consults a.packed for struct-level
alignment, never for member placement) shows up as a size mismatch on the
very struct that would then corrupt AHCI descriptors / USB descriptors /
network headers at runtime.

File-scope aggregates are probed by reference (sizeof(struct tag) /
sizeof(typedef name)).  Function-local aggregates (ipv6.c re-declares
struct eth_hdr inside functions) cannot be referenced from file scope, so
their declaration text is copied into the probe as a fresh file-scope
definition -- byte-identical source, so the same layout rules apply.

Usage: gen_packed_probe.py <file.c|file.h> [...]
       (emits one compilable probe TU per input file, on stdout)
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from packify_packed import (  # noqa: E402
    ATTR,
    attr_positions,
    find_decl_start,
    find_open_brace,
    find_stmt_end,
)

IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def brace_depth_at(text, pos):
    """Brace depth at pos, ignoring comments and string/char literals."""
    depth = 0
    i, n = 0, pos
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
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            i += 1
    return depth


def probe_for(path):
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    attrs = attr_positions(text)
    if not attrs:
        return None
    lines = ["#include <stddef.h>", '#include "%s"' % path]
    seen = set()
    anon_n = 0
    for a in attrs:
        ob = find_open_brace(text, a)
        if ob is None:
            continue
        start = find_decl_start(text, ob)
        if start is None:
            continue
        end = find_stmt_end(text, a + len(ATTR))
        if end is None:
            continue
        head = text[start:ob]
        toks = IDENT_RE.findall(head)
        tag = None
        for kw in ("struct", "union"):
            if kw in toks:
                i = toks.index(kw)
                if i + 1 < len(toks):
                    tag = toks[i + 1]
                break
        depth = brace_depth_at(text, start)
        if depth == 0:
            # file scope: reference the original type
            if tag:
                if tag in seen:
                    continue
                seen.add(tag)
                kw = "struct" if "struct" in toks else "union"
                lines.append(
                    "const unsigned char p_size_%s[sizeof(%s %s)];" % (tag, kw, tag)
                )
            else:
                tail = text[a + len(ATTR) : end]
                m = IDENT_RE.search(tail)
                if not m or m.group(0) in seen:
                    continue
                seen.add(m.group(0))
                lines.append(
                    "const unsigned char p_size_%s[sizeof(%s)];" % (m.group(0), m.group(0))
                )
        else:
            # function-local: copy the declaration text as a fresh file-scope
            # definition (anonymous ones get a probe variable to sizeof)
            decl = text[start : a + len(ATTR)]
            anon_n += 1
            if tag:
                kw = "struct" if "struct" in toks else "union"
                key = "local_" + tag
                if key in seen:
                    continue
                seen.add(key)
                lines.append("%s;" % decl)
                lines.append(
                    "const unsigned char p_size_%s[sizeof(%s %s)];" % (tag, kw, tag)
                )
            else:
                lines.append("%s p_probe_local_%d;" % (decl, anon_n))
                lines.append(
                    "const unsigned char p_size_local_%d[sizeof(p_probe_local_%d)];"
                    % (anon_n, anon_n)
                )
    if len(lines) <= 2:
        return None
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for path in sys.argv[1:]:
        text = probe_for(path)
        if text:
            print(text, end="")


if __name__ == "__main__":
    main()
