#!/usr/bin/env python3
"""Cross-check OTA_PLAN.md's phase claims against the tree.

Why this exists
---------------
The same failure class AUDIT_A7 found in FIXES_PLAN.md (and FSFULL's F6
checker pinned): a plan's status lines are prose, and prose does not
fail a build.  OTA_PLAN.md landed the OS's first self-maintenance
feature across O0..O5 -- partition-aware boot volume, reboot + version
identity, stage2 A/B fallback, the `ota` tool, CI wiring -- and each
phase's "done" is only durable if the tree artefacts and RECEIPTS the
phase claims actually exist.

Ties the plan to the tree:
  - every phase O1..O5 has a `### Phase Ox` section;
  - the Status line says COMPLETE;
  - done phases (✅ DONE in the heading) are backed by the files the
    phase names, AND by their greppable receipts (a harness that lost
    its `[ota] sha256 MISMATCH` line would prove nothing);
  - the O5 wiring is structural: all four cases registered in
    run_all.sh, the `ota` shard in the group partition, the shard in
    the CI matrix, this checker in the workflow and `make test-unit`.

Deliberately NOT asserted: existence of `patches/*.patch` receipts (a
patch file on disk is evidence a FILE EXISTS, not that code works; the
RINET2 precedent) and correctness of the flow (that is the job of the
four harnesses and `make test-unit`).

Usage:
    tools/check_ota_claims.py --check
    tools/check_ota_claims.py --selftest   # prove the checker can fail
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PHASES = ["O%d" % n for n in range(1, 6)]

# Durable artefacts each done phase must have (existence checks).
ARTEFACTS = {
    "O1": ["kernel/fs/fat32.c",
           "kernel/fs/diskfs.c",
           "drivers/ahci/ahci.c",
           "tests/integration/cases/test_ota_bootvol.sh"],
    "O2": ["kernel/arch/x86_64/syscall.c",
           "lib/libc/include/unistd.h",
           "lib/libc/src/libc.c",
           "userspace/system/init/init.c",
           "kernel/version.h",
           "Makefile",
           "tests/integration/cases/test_ota_reboot.sh"],
    "O3": ["boot/bios/stage2/stage2_start.asm",
           "tests/integration/cases/test_ota_fallback.sh"],
    "O4": ["userspace/apps/ota/ota.c",
           "userspace/apps/ota/ota_manifest.c",
           "userspace/apps/ota/ota_manifest.h",
           "tests/unit/test_ota_manifest.c",
           "tests/integration/cases/test_ota_apply.sh"],
    "O5": ["tests/integration/run_all.sh",
           ".github/workflows/integration.yml",
           "tools/check_ota_claims.py",
           "docs/status.md",
           "README.md",
           "TODO.md",
           "docs/residue_ledger.md",
           "CHANGELOG.md"],
}

# Greppable receipts: (phase, path, required substring).  A phase marked
# done whose receipt vanished from the file is a lie about the tree.
RECEIPTS = [
    ("O1", "kernel/fs/fat32.c",
     "found FAT32 partition at LBA %u (via %s)"),
    ("O1", "kernel/fs/fat32.c",
     "not formatting a partitioned disk"),
    ("O1", "kernel/fs/diskfs.c",
     "not an AUFS scratch disk"),
    ("O1", "drivers/ahci/ahci.c",
     "non-destructive write verify"),
    ("O2", "kernel/arch/x86_64/syscall.c", "SYS_REBOOT"),
    ("O2", "lib/libc/include/unistd.h", "SYS_REBOOT"),
    ("O2", "userspace/system/init/init.c", "cmd_reboot"),
    ("O2", "Makefile", "AURALITE_VERSION"),
    ("O3", "boot/bios/stage2/stage2_start.asm",
     "falling back to KERNEL.OLD"),
    ("O3", "boot/bios/stage2/stage2_start.asm", "KERNEL  OLD"),
    ("O4", "userspace/apps/ota/ota.c", "sha256 MISMATCH"),
    ("O4", "userspace/apps/ota/ota.c", "A/B swap done"),
    ("O4", "Makefile", "test_ota_manifest"),
    ("O4", "Makefile", "ota"),
    ("O5", "tests/integration/run_all.sh", "test_ota_bootvol"),
    ("O5", "tests/integration/run_all.sh", "test_ota_reboot"),
    ("O5", "tests/integration/run_all.sh", "test_ota_fallback"),
    ("O5", "tests/integration/run_all.sh", "test_ota_apply"),
    ("O5", "tests/integration/run_all.sh", "ota)   echo '^test_ota_"),
    ("O5", ".github/workflows/integration.yml", "check_ota_claims.py"),
    # LX_COMPAT L1 widened the matrix line to "fsfull, ota, lx]" -- the
    # shard-registration receipt this pin proves is the ota group's
    # presence, which the widened line still carries.
    ("O5", ".github/workflows/integration.yml", "fsfull, ota"),
    ("O5", "docs/status.md", "ota"),
    ("O5", "README.md", "/apps/ota"),
    ("O5", "TODO.md", "OTA"),
    ("O5", "docs/residue_ledger.md", "OTA_PLAN O5"),
    ("O5", "CHANGELOG.md", "OTA O5"),
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
    plan = read_fn("docs", "plans", "OTA_PLAN.md")

    if not plan:
        return ["OTA_PLAN.md missing or unreadable"]

    # All five phase sections exist.
    seen = set(re.findall(r"^### Phase (O\d)\b", plan, re.M))
    for phase in PHASES:
        if phase not in seen:
            fails.append("phase table: missing section %s" % phase)

    # The plan is a closed series: COMPLETE once O5 lands.
    status_line = ""
    for line in plan.splitlines():
        if line.startswith("## Status:"):
            status_line = line
            break
    if "COMPLETE" not in status_line:
        fails.append("status: plan Status line is not COMPLETE "
                     "(got: %r)" % status_line.strip())

    done = set()
    for m in re.finditer(r"^### Phase (O\d)\b.*?\bDONE\b", plan, re.M):
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
                           "userspace/apps/ota/ota.c" else read(*p))
        if not any("O4: marked done but userspace/apps/ota/ota.c missing"
                   in f for f in fails):
            print("check_ota_claims: SELFTEST FAILED -- planted "
                  "missing-artefact violation not caught")
            return 1
        # Planted violation 2: a receipt vanished from a present file.
        orig = read

        def no_receipt(*p):
            txt = orig(*p)
            if "/".join(p) == "userspace/apps/ota/ota.c":
                return txt.replace("sha256 MISMATCH", "REDACTED")
            return txt
        fails = check_plan(no_receipt)
        if not any("O4: receipt 'sha256 MISMATCH' missing" in f
                   for f in fails):
            print("check_ota_claims: SELFTEST FAILED -- planted "
                  "missing-receipt violation not caught")
            return 1
        # Planted violation 3: the plan's status is not COMPLETE.
        plan = orig("docs", "plans", "OTA_PLAN.md")

        def pending_plan(*p):
            if p == ("docs", "plans", "OTA_PLAN.md"):
                return plan.replace(
                    "## Status: COMPLETE", "## Status: IN PROGRESS", 1)
            return orig(*p)
        fails = check_plan(pending_plan)
        if not any("status: plan Status line is not COMPLETE" in f
                   for f in fails):
            print("check_ota_claims: SELFTEST FAILED -- planted "
                  "not-COMPLETE violation not caught")
            return 1
        print("check_ota_claims: SELFTEST OK (planted violations caught)")
        return 0

    fails = check_plan(read)
    if fails:
        for f in fails:
            print("check_ota_claims: FAIL -- %s" % f)
        print("check_ota_claims: %d claim(s) disagree with the tree"
              % len(fails))
        return 1
    print("check_ota_claims: OK -- the OTA plan matches the tree "
          "(%d phases, %d artefact + %d receipt pins)"
          % (len(PHASES), sum(len(v) for v in ARTEFACTS.values()),
             len(RECEIPTS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
