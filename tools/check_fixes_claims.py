#!/usr/bin/env python3
"""Cross-check FIXES_PLAN.md's status against the tree.

Why this exists
---------------
The same failure AUDIT_A3 found in MATURITY_PLAN.md, one document over:
FIXES_PLAN.md's header says "Status: PLANNED (phases R0-R8)" while all 33
of its own task checkboxes are ticked, every repair is in the source, and
all seven of its test gates exist and are registered in run_all.sh.

Nothing caught it, because a status line is prose and prose does not fail
a build.  This ties each phase to something in the source, and -- like
check_maturity_claims.py -- checks the header itself, so the document
cannot drift back.

Usage:
    tools/check_fixes_claims.py [--check]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def claims():
    plan = read("docs", "plans", "FIXES_PLAN.md")
    runall = read("tests", "integration", "run_all.sh")

    tss = strip_comments(read("kernel", "arch", "x86_64", "tss.c"))
    idt = strip_comments(read("kernel", "arch", "x86_64", "idt.c"))
    sprot = strip_comments(read("kernel", "lib", "stack_protector.c"))
    libc = strip_comments(read("lib", "libc", "src", "libc.c"))
    signal = strip_comments(read("kernel", "proc", "signal.c"))
    tcp = strip_comments(read("kernel", "net", "tcp.c"))
    initrd = strip_comments(read("kernel", "fs", "initrd.c"))

    checks = [
        # --- R1: the IST is armed for #DF ---
        ("R1: the TSS provides an IST1 stack",
         "ist1_low" in tss and "ist1_high" in tss),
        ("R1: #DF (vector 8) is routed to IST1",
         "idt_set_ist(8, 1)" in tss and "idt_set_ist" in idt),

        # --- R2: the SMP stack-protector trip is diagnosable ---
        ("R2: the stack guard is seeded, not left at its constant",
         "stack_protector_init" in sprot and "read_tsc" in sprot),
        ("R2: the seeding routine is itself unprotected",
         "no_stack_protector" in sprot),

        # --- R3: thread-local errno ---
        ("R3: errno is per-thread, not a global",
         "__errno_location" in libc),

        # --- R4: the unchecked allocations ---
        ("R4: initrd_init() checks its allocation",
         "kmalloc" in initrd and
         re.search(r"if\s*\(\s*!\s*\w+\s*\)", initrd) is not None),

        # --- R5: .init_array runs ---
        ("R5: .init_array constructors are run",
         "init_array" in libc),

        # --- R6: a stopped state exists ---
        ("R6: SIGSTOP/SIGCONT reach a real stopped state",
         "SIGSTOP" in signal and "SIGCONT" in signal),

        # --- R7: socket syscalls report real errno ---
        ("R7: TCP reports specific errno, not a blanket failure",
         all(e in tcp for e in ("EMFILE", "ETIMEDOUT", "ECONNRESET"))),

        # --- R8: selectable keymaps ---
        ("R8: a second keyboard layout exists",
         os.path.exists(os.path.join(ROOT, "drivers", "keyboard", "keymap.c"))),
    ]

    # Every gate the plan promises must exist AND be registered -- an
    # unregistered case does not run (that was AUDIT_A0's finding).
    for case in ("test_ist_double_fault", "test_stack_guard",
                 "test_panic_diag", "test_errno", "test_stopped",
                 "test_tls_errno", "test_keymaps"):
        path = os.path.join(ROOT, "tests", "integration", "cases",
                            case + ".sh")
        checks.append(
            ("gate %s exists and is registered in run_all.sh" % case,
             os.path.exists(path) and
             re.search(r"^\s*%s\s*$" % re.escape(case), runall, re.M)
             is not None))

    # The header must not contradict the body.  All 33 task boxes are
    # ticked; a plan in that state is not "PLANNED".
    unticked = len(re.findall(r"^- \[ \]", plan, re.M))
    ticked = len(re.findall(r"^- \[x\]", plan, re.M))
    # Only the STATUS LINE counts, not the whole document: the corrected
    # header explains what it used to say, and a substring search over the
    # file matched that explanation and kept failing.  A guard that cannot
    # be satisfied by fixing the thing it complains about is a broken guard.
    status = re.search(r"^##\s*Status:.*$", plan, re.M)
    status_line = status.group(0) if status else ""
    checks.append(
        ("the status line does not say PLANNED while every task is ticked",
         not (ticked > 0 and unticked == 0 and "PLANNED" in status_line)))
    checks.append(
        ("the plan still has its task list (%d ticked, %d open)"
         % (ticked, unticked), ticked + unticked >= 30))

    return checks


def main():
    if not read("docs", "plans", "FIXES_PLAN.md"):
        print("check_fixes_claims: FIXES_PLAN.md is missing", file=sys.stderr)
        return 1

    failures = [label for label, holds in claims() if not holds]
    if failures:
        print("check_fixes_claims: the plan and the tree disagree\n",
              file=sys.stderr)
        for item in failures:
            print("  FAIL: %s" % item, file=sys.stderr)
        print("\nUpdate FIXES_PLAN.md, or the code, so they agree.",
              file=sys.stderr)
        return 1

    print("check_fixes_claims: %d repair claims verified against the tree"
          % len(claims()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
