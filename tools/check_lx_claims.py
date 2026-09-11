#!/usr/bin/env python3
"""Cross-check LX_COMPAT_PLAN.md's phase claims against the tree.

Why this exists
---------------
The same failure class AUDIT_A7 found in FIXES_PLAN.md (and pinned by
FSFULL's F6 checker and OTA's O5 checker): a plan's status lines are
prose, and prose does not fail a build.  LX_COMPAT_PLAN.md landed the
OS's second personality -- unmodified Linux applications running on the
native kernel through a per-process syscall-number map -- across six
phases (L0..L5), from a static `hello` to an unmodified stock lua
interpreter running a real script.  Each phase's "done" is only durable
if the tree artefacts and greppable RECEIPTS the phase claims actually
exist.

Ties the plan to the tree:
  - every phase L0..L5 has a `### Phase Lx` section;
  - the Status line says COMPLETE;
  - done phases (✅ DONE in the heading) are backed by the files the
    phase names -- the translation-table arms, the marshal functions,
    the signal-frame header, the loader, the CI cases -- AND by their
    greppable receipts (a case that lost its `LX5-LUA-OK` pattern, or
    a map that lost its `time` row, would prove nothing);
  - the L5 wiring is structural: all five cases registered in
    run_all.sh, the `lx` shard in the group partition, the shard in
    the CI matrix, the lua payload presence-assert and this checker in
    the workflow, and the checker in `make test-unit`.

Deliberately NOT asserted: existence of `patches/*.patch` receipts (a
patch file on disk is evidence a FILE EXISTS, not that code works; the
RINET2 precedent) and correctness of the translation (that is the job
of the five integration cases and the two host unit tests,
`test_lx_translate` and `test_lx_sig`).

Usage:
    tools/check_lx_claims.py --check
    tools/check_lx_claims.py --selftest   # prove the checker can fail
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PHASES = ["L%d" % n for n in range(0, 6)]

# Durable artefacts each done phase must have (existence checks).
ARTEFACTS = {
    "L0": ["docs/plans/LX_COMPAT_PLAN.md",
           "tools/residue_baseline.txt"],
    "L1": ["kernel/lx/lx.h",
           "kernel/lx/lx_translate.c",
           "kernel/arch/x86_64/syscall.c",
           "userspace/apps/lxrun/lxrun.c",
           "tests/unit/test_lx_translate.c",
           "tests/integration/cases/test_lx_hello.sh"],
    "L2": ["tests/integration/cases/test_lx_busybox.sh"],
    "L3": ["kernel/lx/lx_sig.h",
           "kernel/proc/signal.c",
           "tests/unit/test_lx_sig.c",
           "tests/integration/cases/test_lx_shell.sh"],
    "L4": ["kernel/proc/elf.c",
           "kernel/proc/process.c",
           "kernel/proc/clone.c",
           "tests/integration/cases/test_lx_dynamic.sh"],
    "L5": ["lx/tests/lua_script.lua",
           "tests/integration/cases/test_lx_lua.sh",
           "tests/integration/run_all.sh",
           ".github/workflows/integration.yml",
           "tools/check_lx_claims.py",
           "Makefile",
           "docs/status.md",
           "README.md",
           "TODO.md",
           "docs/residue_ledger.md",
           "tools/residue_baseline.txt"],
}

# Greppable receipts: (phase, path, required substring).  A phase marked
# done whose receipt vanished from the file is a lie about the tree.
RECEIPTS = [
    ("L0", "tools/residue_baseline.txt", "LX_COMPAT_PLAN.md"),
    ("L1", "kernel/lx/lx_translate.c", "LX_ARM_UNAME"),
    ("L1", "kernel/lx/lx_translate.c", "LX_ARM_EXIT_GROUP"),
    ("L1", "kernel/lx/lx_translate.c", "LX_ARM_SET_TID_ADDRESS"),
    ("L1", "userspace/apps/lxrun/lxrun.c", "/linux/"),
    ("L1", "tests/integration/cases/test_lx_hello.sh",
     "lxrun /linux/tests/hello"),
    ("L2", "kernel/lx/lx_translate.c", "LX_ARM_GETDENTS64"),
    ("L2", "kernel/lx/lx_translate.c", "LX_ARM_NEWFSTATAT"),
    ("L2", "kernel/arch/x86_64/syscall.c", "lx_marshal_stat"),
    ("L2", "tests/integration/cases/test_lx_busybox.sh", "LXBEE-7f3a"),
    ("L3", "kernel/lx/lx_sig.h", "rt_sigframe"),
    ("L3", "kernel/lx/lx_translate.c", "LX_ARM_SIGACTION"),
    ("L3", "kernel/lx/lx_translate.c", "LX_ARM_CLONE"),
    ("L3", "tests/integration/cases/test_lx_shell.sh", "LX3-ECHO-OK"),
    ("L3", "tests/integration/cases/test_lx_shell.sh", "LX3-TRAP-OK"),
    ("L4", "kernel/lx/lx_translate.c", "202, 530"),
    ("L4", "kernel/lx/lx_translate.c", "17,  17"),
    ("L4", "kernel/proc/elf.c", "elf_interp_path"),
    ("L4", "tests/integration/cases/test_lx_dynamic.sh", "LX4-HELLO-OK"),
    ("L4", "tests/integration/cases/test_lx_dynamic.sh", "LX4-SH-OK"),
    ("L5", "lx/tests/lua_script.lua", "LX5-LUA-OK"),
    ("L5", "kernel/lx/lx_translate.c", "201, 520"),
    ("L5", "tests/integration/cases/test_lx_lua.sh", "LX5-LUA-OK"),
    ("L5", "tests/integration/cases/test_lx_lua.sh",
     "/linux/tests/lua_script.lua"),
    ("L5", "tests/integration/run_all.sh", "test_lx_lua"),
    ("L5", "tests/integration/run_all.sh", "test_lx_hello"),
    ("L5", "tests/integration/run_all.sh", "test_lx_busybox"),
    ("L5", "tests/integration/run_all.sh", "test_lx_shell"),
    ("L5", "tests/integration/run_all.sh", "test_lx_dynamic"),
    ("L5", "tests/integration/run_all.sh", "test_lx_[a-z0-9_]+"),
    ("L5", ".github/workflows/integration.yml", "fsfull, ota, lx"),
    ("L5", ".github/workflows/integration.yml", "linux/tests/lua"),
    ("L5", ".github/workflows/integration.yml", "check_lx_claims.py"),
    ("L5", "Makefile", "lx-payload"),
    ("L5", "Makefile", "LX_LUA_SHA"),
    ("L5", "Makefile", "check_lx_claims.py"),
    ("L5", "docs/status.md", "Linux personality"),
    ("L5", "README.md", "Linux personality"),
    ("L5", "TODO.md", "LX_COMPAT_PLAN"),
    ("L5", "docs/residue_ledger.md", "LX_COMPAT L5"),
    ("L5", "tools/residue_baseline.txt", "LX_COMPAT_PLAN.md"),
]


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def check_plan(read_fn):
    """Return a list of failure strings.  read_fn is injected so
    --selftest can plant violations."""
    fails = []
    plan = read_fn("docs", "plans", "LX_COMPAT_PLAN.md")

    if not plan:
        return ["LX_COMPAT_PLAN.md missing or unreadable"]

    # All six phase sections exist.
    seen = set(re.findall(r"^### Phase (L\d)\b", plan, re.M))
    for phase in PHASES:
        if phase not in seen:
            fails.append("phase table: missing section %s" % phase)

    # The plan is a closed series: COMPLETE once L5 lands.
    status_line = ""
    for line in plan.splitlines():
        if line.startswith("## Status:"):
            status_line = line
            break
    if "COMPLETE" not in status_line:
        fails.append("status: plan Status line is not COMPLETE "
                     "(got: %r)" % status_line.strip())

    done = set()
    for m in re.finditer(r"^### Phase (L\d)\b.*?\bDONE\b", plan, re.M):
        done.add(m.group(1))
    for phase in PHASES:
        if phase not in done:
            fails.append("%s: phase not marked done (the series is "
                         "COMPLETE; a pending phase contradicts it)" % phase)

    # Artefacts and receipts for every done phase.
    for phase, arts in ARTEFACTS.items():
        if phase not in done:
            continue
        for art in arts:
            if not read_fn(*art.split("/")):
                fails.append("%s: marked done but %s missing" % (phase, art))
    for phase, path, needle in RECEIPTS:
        if phase not in done:
            continue
        if needle not in read_fn(*path.split("/")):
            fails.append("%s: receipt %r missing from %s"
                         % (phase, needle, path))

    return fails


def main():
    if "--selftest" in sys.argv:
        # Planted violation 1: a done phase's artefact is missing.
        fails = check_plan(lambda *p: "" if "/".join(p) ==
                           "tests/integration/cases/test_lx_lua.sh" else
                           read(*p))
        if not any("L5: marked done but tests/integration/cases/"
                   "test_lx_lua.sh missing" in f for f in fails):
            print("check_lx_claims: SELFTEST FAILED -- planted "
                  "missing-artefact violation not caught")
            return 1
        # Planted violation 2: a receipt vanished from a present file.
        def no_receipt(*p):
            txt = read(*p)
            if "/".join(p) == "kernel/lx/lx_translate.c":
                return txt.replace("201, 520", "999, 999")
            return txt
        fails = check_plan(no_receipt)
        if not any("L5: receipt '201, 520' missing" in f for f in fails):
            print("check_lx_claims: SELFTEST FAILED -- planted "
                  "missing-receipt violation not caught")
            return 1
        # Planted violation 3: the plan's status is not COMPLETE.
        def pending_plan(*p):
            if p == ("docs", "plans", "LX_COMPAT_PLAN.md"):
                return read(*p).replace(
                    "## Status: COMPLETE", "## Status: OPEN", 1)
            return read(*p)
        fails = check_plan(pending_plan)
        if not any("status: plan Status line is not COMPLETE" in f
                   for f in fails):
            print("check_lx_claims: SELFTEST FAILED -- planted "
                  "not-COMPLETE violation not caught")
            return 1
        print("check_lx_claims: SELFTEST OK (planted violations caught)")
        return 0

    fails = check_plan(read)
    if fails:
        for f in fails:
            print("check_lx_claims: FAIL -- %s" % f)
        print("check_lx_claims: %d claim(s) disagree with the tree"
              % len(fails))
        return 1
    print("check_lx_claims: OK -- the LX plan matches the tree "
          "(%d phases, %d artefact + %d receipt pins)"
          % (len(PHASES), sum(len(v) for v in ARTEFACTS.values()),
             len(RECEIPTS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
