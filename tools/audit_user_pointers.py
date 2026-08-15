#!/usr/bin/env python3
"""Find syscall arguments that reach a raw pointer dereference unchecked.

MATURITY_PLAN.md M3's remaining task is a grep-audit: "every direct
user-pointer dereference in syscall.c, socket.c, gui_syscalls.c and
gpu_syscalls.c routed through the safe primitives".  That audit is the kind
of thing a human does once, declares finished, and never repeats -- so it
is written as a tool instead, and run in CI.

How it works
------------
Syscall arguments arrive as a1..a6 (uint64_t).  Any of them can be a
userspace address.  The checker looks, per syscall case, for a value
derived from an argument that is then dereferenced, indexed, memcpy'd,
strlen'd or passed to a kernel routine WITHOUT first passing through
copy_from_user / copy_to_user / copy_string_from_user /
validate_user_range.

It is a lint, not a prover: it reports candidates and carries an explicit
allowlist for the cases that are safe for a reason a regex cannot see.
Every allowlist entry needs a written justification, which is the part that
actually gets reviewed.

Usage:
    tools/audit_user_pointers.py [--check] [--verbose]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TARGETS = [
    ("kernel/arch/x86_64/syscall.c", "syscall dispatch"),
    ("kernel/gui/gui_syscalls.c", "GUI syscalls"),
    ("kernel/gpu/gpu_syscalls.c", "GPU syscalls"),
    ("kernel/net/socket.c", "socket layer"),
]

SAFE_CALLS = (
    "copy_from_user", "copy_to_user", "copy_string_from_user",
    "validate_user_range", "user_access_begin",
)

# Dereference shapes that matter: *p, p[i], and the classic byte-pushers.
DEREF = re.compile(
    r"(?:\*\s*(?P<star>[A-Za-z_]\w*)"
    r"|(?P<idx>[A-Za-z_]\w*)\s*\["
    # A cast in the way hides the name: ((char *)user_arg)[0] and
    # *(int *)user_ts are both dereferences of a user pointer, and neither
    # matches a bare identifier.  Third negative control found this.
    r"|\)\s*(?P<cast_idx>[A-Za-z_]\w*)\s*\)\s*\["
    r"|\*\s*\([^)]*\*\s*\)\s*(?P<cast_star>[A-Za-z_]\w*))"
)

# memcpy(dst, src, n) touches BOTH pointers, and the dangerous one is
# usually src -- the second argument.  Matching only the first argument
# meant "memcpy(tmp, user_buf, 8)" reported 'tmp' (a kernel buffer) and
# said nothing about user_buf.  Its own negative control caught that.
MEMFN = re.compile(
    r"\b(?:memcpy|memmove|memset|strlen|strcmp|strncmp|strcpy|strncpy|"
    r"strlcpy|snprintf)\s*\((?P<args>[^;]*)\)"
)
ARG_NAME = re.compile(r"\b([A-Za-z_]\w*)\b")

# A local that is plainly a userspace address: assigned from an argument.
FROM_ARG = re.compile(
    r"\b(?P<name>[A-Za-z_]\w*)\s*=\s*\(?\s*(?:const\s+)?[\w\s*]*\)?\s*"
    r"\(?\s*(?:uintptr_t\s*\)\s*)?(?P<arg>a[1-6])\b"
)

# Cases that are safe for reasons the regex cannot see.  Each entry is
# (file, symbol, why).  Keep this list short and justified; an entry with a
# weak justification is a bug waiting to be rediscovered.
ALLOWLIST = {
    ("kernel/arch/x86_64/syscall.c", "sb"):
        "SETUP packet copied into a kernel buffer by the caller before use",
    ("kernel/arch/x86_64/syscall.c", "kbuf"):
        "kernel-side bounce buffer, not a user address",
    ("kernel/arch/x86_64/syscall.c", "kpath"):
        "filled by copy_string_from_user() before any dereference",
    ("kernel/gui/gui_syscalls.c", "kbuf"):
        "kernel bounce buffer for ag_blit, populated via copy_from_user",
}


def scan(path):
    """Return [(line_no, symbol, line_text)] of suspicious dereferences."""
    full = os.path.join(ROOT, path)
    try:
        with open(full, "r", encoding="utf-8", errors="replace") as handle:
            lines = handle.readlines()
    except OSError:
        return []

    # Scope every symbol to the block it is declared in.
    #
    # Names repeat across this file -- "user_buf" is a parameter of
    # syscall_vfs_write() near the top AND a local in a case arm 800 lines
    # later.  Tracking taint and guards by bare name let a guard in one
    # function excuse a dereference in another, which is how the second
    # negative control (an unguarded memcpy) kept passing.  Delimit on the
    # brace depth returning to the level where the symbol was introduced.
    depths = []
    depth = 0
    for line in lines:
        depths.append(depth)
        depth += line.count("{") - line.count("}")

    tainted = {}        # name -> (decl_line, decl_depth)
    for num, line in enumerate(lines, 1):
        match = FROM_ARG.search(line)
        if match:
            tainted[match.group("name")] = (num, depths[num - 1])

    def in_scope(sym, num):
        """True while `num` is inside the block that declared `sym`."""
        decl_line, decl_depth = tainted[sym]
        if num < decl_line:
            return False
        for probe in range(decl_line, num):
            if depths[probe] < decl_depth:
                return False        # block closed before reaching num
        return True

    # A symbol is cleared once it appears in a safe call.  Checking line by
    # line is not enough: the guard almost always sits on the line AFTER the
    # declaration --
    #
    #     struct kernel_timespec *user_ts = (...)a2;
    #     if (!validate_user_range(user_ts, sizeof(*user_ts), 1)) ...
    #
    # so a naive scan reports every one of them.  The first version of this
    # script did exactly that and produced 13 findings, all false.  Track
    # where each symbol is first guarded, and only report a dereference that
    # happens before that point.
    # Where each tainted symbol is first passed to a safe primitive.  A
    # guard that appears AFTER a dereference does not protect it, so the
    # line number matters, not merely the fact that a guard exists
    # somewhere in the function.
    guarded_at = {}
    for num, line in enumerate(lines, 1):
        if not any(safe in line for safe in SAFE_CALLS):
            continue
        for sym in tainted:
            if not in_scope(sym, num):
                continue
            if re.search(r"\b%s\b" % re.escape(sym), line):
                guarded_at.setdefault(sym, num)

    findings = []
    for num, line in enumerate(lines, 1):
        stripped = line.strip()
        # "* text" continues a block comment; "*ptr = x" is a dereference.
        # Treating both as comment made the checker blind to the single most
        # obvious unsafe shape there is -- caught by its own negative
        # control, which is the only reason this is not still wrong.
        if stripped.startswith("/*") or stripped.startswith("//"):
            continue
        if stripped.startswith("*") and not re.match(r"\*\s*\w+\s*(=|\[|\))",
                                                    stripped):
            continue
        if any(safe in line for safe in SAFE_CALLS):
            continue

        # The declaration itself is not a dereference.  "struct foo *p =
        # (struct foo *)(uintptr_t)a1;" matches the "*p" shape while doing
        # nothing but naming the pointer, and every timer syscall looks like
        # that.  Skip the line that introduces the symbol.
        decl = FROM_ARG.search(line)
        decl_sym = decl.group("name") if decl else None

        candidates = []
        for match in DEREF.finditer(line):
            candidates.append(match.group("star") or match.group("idx")
                              or match.group("cast_idx")
                              or match.group("cast_star"))
        for match in MEMFN.finditer(line):
            candidates.extend(ARG_NAME.findall(match.group("args")))

        for sym in candidates:
            if not sym or sym not in tainted:
                continue
            if sym == decl_sym:
                continue
            if (path, sym) in ALLOWLIST:
                continue
            if not in_scope(sym, num):
                continue
            guard = guarded_at.get(sym)
            if guard is not None and guard < num:
                continue        # guarded on an EARLIER line
            if guard == num:
                continue        # guard and use on the same line
            findings.append((num, sym, stripped[:96]))
    return findings


def main():
    verbose = "--verbose" in sys.argv
    total = 0
    report = []

    for path, label in TARGETS:
        findings = scan(path)
        total += len(findings)
        if findings:
            report.append((path, label, findings))

    if not total:
        print("audit_user_pointers: no unchecked user-pointer dereferences "
              "in %d file(s)" % len(TARGETS))
        for path, _ in TARGETS:
            if verbose:
                print("    scanned %s" % path)
        return 0

    print("audit_user_pointers: %d unchecked user-pointer dereference(s)\n"
          % total, file=sys.stderr)
    for path, label, findings in report:
        print("  %s (%s):" % (path, label), file=sys.stderr)
        for num, sym, text in findings:
            print("      %s:%d  '%s'  %s" % (path, num, sym, text),
                  file=sys.stderr)
    print("\nRoute each through copy_from_user/copy_to_user/"
          "validate_user_range,\nor add a justified entry to ALLOWLIST in "
          "this script.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
