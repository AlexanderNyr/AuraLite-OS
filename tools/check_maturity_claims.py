#!/usr/bin/env python3
"""Cross-check MATURITY_PLAN.md's phase statuses against the tree.

Why this exists
---------------
MATURITY_AUDIT.md measured the plan and found three statuses that were
simply wrong: M3 and M4 had landed but were listed "pending | --", M6
understated features already present in tcp.c, and M10 asked for USB work
that USB_PLAN.md U0-U9 had already delivered.  Nothing caught any of it,
because a status line is prose and prose does not fail a build.

Each check below ties a status to something in the source.  The absences
are checked in reverse too, so a gap that quietly gets implemented also
fails -- the same asymmetry tools/check_usb_claims.py uses.

Usage:
    tools/check_maturity_claims.py [--check]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLAN = os.path.join(ROOT, "MATURITY_PLAN.md")


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
    plan = read("MATURITY_PLAN.md")
    tcp = read("kernel", "net", "tcp.c")
    xhci = strip_comments(read("drivers", "usb", "xhci.c"))
    ctx = read("kernel", "proc", "context.asm")
    thread_h = read("kernel", "proc", "thread.h")
    syscall = read("kernel", "arch", "x86_64", "syscall.c")
    kernel_c = read("kernel", "kernel.c")
    usercopy = read("kernel", "proc", "usercopy.h")
    mmapshare = read("userspace", "tests", "mmapshare", "mmapshare.c")

    checks = [
        # --- M1: claimed complete ---
        ("M1 claims complete: FPU state is saved on context switch",
         "fxsave" in ctx and "fxrstor" in ctx and "fpu_valid" in thread_h),

        # --- M2: claimed core complete ---
        ("M2 claims complete: the IOAPIC is initialised at boot",
         "ioapic_init" in kernel_c),

        # --- M3: claimed complete ---
        ("M3 claims complete: safe uaccess primitives exist",
         "validate_user_range" in usercopy and
         syscall.count("copy_from_user") + syscall.count("copy_to_user") > 20),
        ("M3 claims complete: its gate exists and uses the real lib.sh API",
         "il_run_qemu" in read("tests", "integration", "cases",
                               "test_uaccess.sh")),

        # --- M4: claimed complete for anonymous MAP_SHARED ---
        ("M4 claims complete: anonymous MAP_SHARED is wired to shmem",
         "MAP_SHARED" in syscall and "shmem" in syscall),
        ("M4 claims complete: a program proves it across fork",
         "MAP_SHARED" in mmapshare and "fork" in mmapshare),
        # A6: file-backed MAP_SHARED is claimed complete too.
        ("M4 claims complete: file-backed MAP_SHARED is no longer ENOSYS",
         "MAP_SHARED) && !anonymous) return (uint64_t)-ENOSYS"
         not in syscall),
        ("M4 claims complete: the page cache dirty bit is actually set",
         "page_cache_mark_dirty" in read("kernel", "mm", "page_cache.c") and
         "page_cache_mark_dirty" in read("kernel", "mm", "vma.c")),
        ("M4 claims complete: msync() is wired up",
         "SYS_MSYNC" in syscall and "msync" in read("lib", "libc", "src",
                                                    "libc.c")),

        # --- M5: claimed complete ---
        ("M5 claims complete: session/process-group syscalls are wired",
         "do_setsid" in syscall and "do_setpgid" in syscall),

        # --- M6: the plan must credit what exists ... ---
        ("M6 correctly credits existing congestion control",
         "cwnd" in tcp and "ssthresh" in tcp and "rto_ms" in tcp),
        # M6 partial: fast retransmit, Nagle, delayed ACK and TIME_WAIT
        # policy now exist and are wired into the send path.
        ("M6 partial: the fast-retransmit policy header exists",
         "tcpm6_on_ack" in read("kernel", "net", "tcp_m6.h") and
         "TCPM6_DUPACK_THRESH" in read("kernel", "net", "tcp_m6.h")),
        ("M6 partial: fast retransmit is wired into tcp.c, not dead code",
         "tcpm6_on_ack" in tcp and "TCPM6_ACK_FAST_RETX" in tcp),
        ("M6 partial: Nagle and delayed ACK policy exist",
         "tcpm6_nagle_may_send" in read("kernel", "net", "tcp_m6.h") and
         "tcpm6_delack_on_segment" in read("kernel", "net", "tcp_m6.h")),
        # ... and the phase is still open, because SACK is genuinely absent:
        ("M6 is still pending: no SACK yet",
         "sack" not in tcp.lower()),

        # --- M10: superseded, and the plan must say so ---
        ("M10 is marked superseded by USB_PLAN.md",
         "SUPERSEDED" in plan and "USB_PLAN.md" in plan),
        ("M10 is superseded for a reason: xHCI transfers are real",
         "xhci_bulk_transfer" in xhci and "xhci_address_device" in xhci),
    ]

    # The plan's status table must not contradict itself.
    for phase, must_say in (("M3", "complete"), ("M4", "complete")):
        row = re.search(r"^\|\s*%s\s*[—-].*$" % phase, plan, re.M)
        checks.append(
            ("%s's status row says '%s'" % (phase, must_say),
             row is not None and must_say in row.group(0).lower()))

    return checks


def main():
    if not read("MATURITY_PLAN.md"):
        print("check_maturity_claims: MATURITY_PLAN.md is missing",
              file=sys.stderr)
        return 1

    failures = [label for label, holds in claims() if not holds]
    if failures:
        print("check_maturity_claims: the plan and the tree disagree\n",
              file=sys.stderr)
        for item in failures:
            print("  FAIL: %s" % item, file=sys.stderr)
        print("\nUpdate MATURITY_PLAN.md, or the code, so they agree.",
              file=sys.stderr)
        return 1

    print("check_maturity_claims: %d phase claims verified against the tree"
          % len(claims()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
