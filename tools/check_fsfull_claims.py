#!/usr/bin/env python3
"""Cross-check FSFULL_PLAN.md's phase claims against the tree.

Why this exists
---------------
The same failure class AUDIT_A7 found in FIXES_PLAN.md and AUDIT_A3 in
MATURITY_PLAN.md: a plan's status lines are prose, and prose does not fail
a build.  FSFULL_PLAN.md replaced the ext4/btrfs/f2fs/exFAT/NTFS skeletons
with real drivers over several phases (F1..F5b), and each phase's "done"
is only durable if the tree artefacts that phase claims actually exist.

Ties the plan to the tree:
  - every phase F1..F7 has a `### Phase Fx` section;
  - done phases (marked ✅ in the heading) are backed by the files the
    phase names in its `**Result:**` gate block and its `**Deliverable**`
    line, so a status flip cannot outlive the code/harness/patch it
    claims;
  - the five on-disk headers are honest: they must not carry the retracted
    phrases the plan says were pulled back (btrfs "subvolumes and
    snapshots", ext4 "full journaling"/"HTree-ready writing"/"JBD2",
    f2fs "multi-head logging"), and must state an honest boundary
    (out-of-scope / refused / read-only).

Deliberately NOT asserted: correctness of the drivers.  That is the job of
the harnesses (`tests/<fs>/test_*.sh`, `make test-unit`).  This checker
only stops a phase ✅ from being a lie about the tree.

Usage:
    tools/check_fsfull_claims.py [--check]
    tools/check_fsfull_claims.py --selftest   # prove the checker can fail
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PHASES = ["F%d" % n for n in range(1, 8)]

# Artefacts each phase's `**Result:**`/`**Deliverable**` claims, keyed by
# phase.  Only phases marked ✅ in the plan heading are checked (F7 stays
# planned).  We deliberately do NOT pin `patches/FS_F1_mount_safety.patch`:
# a patch on disk is weak evidence (see check_selfhost_claims.py's rationale
# for the same choice), and F1's durable receipts are its gate artefacts —
# the format-knob unit test and the integration case that prove a foreign
# volume is refused (not auto-formatted) with the knob off and formatted
# with it on.
ARTEFACTS = {
    "F1":  ["tests/unit/test_fsformat.c",
            "tests/integration/cases/test_fsformat_knob.sh"],
    "F2":  ["patches/FS_F2_bcache_seam.patch",
            "tests/unit/test_blkdev.c"],
    "F3":  ["patches/FS_F3_ext4.patch",
            "tests/ext4/test_ext4.sh",
            "tests/unit/test_exfat_ntfs.c"],
    "F4":  ["patches/FS_F4_f2fs.patch",
            "tests/f2fs/test_f2fs.sh"],
    "F4b": ["patches/FS_F4b_btrfs.patch",
            "tests/btrfs/test_btrfs.sh"],
    "F5":  ["patches/FS_F5_exfat.patch",
            "tests/exfat/test_exfat.sh",
            "tests/exfat/seed_exfat.py"],
    "F5b": ["patches/FS_F5b_ntfs.patch",
            "tests/ntfs/test_ntfs.sh",
            "tests/unit/test_exfat_ntfs.c"],
    "F6":  ["tools/check_fsfull_claims.py",
            "kernel/fs/ext4.h",
            "kernel/fs/f2fs.h",
            "kernel/fs/btrfs.h",
            "kernel/fs/exfat.h",
            "kernel/fs/ntfs.h",
            "docs/status.md",
            "docs/filesystem.md",
            "TODO.md"],
    "F7":  ["tests/integration/run_all.sh",
            "tests/integration/cases/test_ext4.sh",
            "tests/integration/cases/test_f2fs.sh",
            "tests/integration/cases/test_btrfs.sh",
            "tests/integration/cases/test_exfat.sh",
            "tests/integration/cases/test_ntfs.sh",
            ".github/workflows/integration.yml",
            "kernel/kernel.c",
            "docs/residue_ledger.md",
            "tools/residue_baseline.txt"],
}

# Retracted claims that must NOT appear in the on-disk headers.  The F5b
# header (ntfs.h) is read-only by design and is honest; exfat.h is honest.
# These are the specific overclaims F6 retracted.
RETRACTED = [
    # btrfs: no subvolumes / snapshots (single FS tree only)
    "subvolumes and snapshots",
    "Subvolumes and snapshots",
    # ext4: no JBD2 journal / full journaling / delayed allocation
    "full journaling",
    "JBD2-compatible journal",
    "JBD2 journal",
    "delayed allocation",
    # ext4: HTree is parsed read-only, never written
    "HTree-ready writing",
    # f2fs: no cleaning / multi-head logging
    "multi-head logging",
    "hot/cold node/data",
]

# Every driver header must carry an honest boundary marker so a header that
# says nothing (or claims everything) fails.
BOUNDARY = ["out of scope", "not implemented", "NOT implemented",
            "read-only", "READ-ONLY", "refused", "gaps are deliberate"]


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def _done_phases(plan):
    """Return the set of phase IDs whose heading carries "DONE" (a bare
    "✅ planned" is NOT done — F7 stays planned)."""
    done = set()
    for m in re.finditer(r"^### Phase (F\d+\w*)\b.*?\bDONE\b", plan, re.M):
        done.add(m.group(1))
    return done


def check_plan(plan, tree_has_file):
    """Return a list of failure strings.  tree_has_file is injected so
    --selftest can plant violations."""
    fails = []

    if not plan:
        return ["FSFULL_PLAN.md missing or unreadable"]

    # All seven phase sections exist.
    seen = set(re.findall(r"^### Phase (F\d+\w*)\b", plan, re.M))
    for phase in PHASES:
        if phase not in seen:
            fails.append("phase table: missing section %s" % phase)

    done = _done_phases(plan)
    # F4b and F5b are sub-phases folded into the F4/F5 section headings (there
    # is no `### Phase F4b/F5b` section), so a standalone heading never marks
    # them done.  Tie their "done" to the real deliverable patch: the moment
    # the patch exists the phase's receipts are enforced.
    for sub in ("F4b", "F5b"):
        if sub in done or tree_has_file("patches/FS_F%s.patch" % sub):
            done.add(sub)
    # F6 and F7 must be done once the plan's series has landed.
    if "F6" not in done:
        fails.append("F6: phase not marked done (expected ✅ after this phase)")
    if "F7" not in done:
        fails.append("F7: phase not marked done (expected ✅ after this phase)")

    # F7's own receipts are pinned as artefacts above; the two structural
    # ones are machine-checked here: the fsfull shard really exists in
    # run_all.sh, and the ledger really carries the DONE@F7 row.
    if "F7" in done:
        run_all = read("tests", "integration", "run_all.sh")
        if "fsfull" not in run_all or "GROUP_NAMES" not in run_all:
            fails.append("F7: run_all.sh has no 'fsfull' shard")
        ledger = read("docs", "residue_ledger.md")
        if "RES-46" not in ledger or "DONE@F7" not in ledger:
            fails.append("F7: ledger has no RES-46 DONE@F7 row")

    # Each done phase must have its receipts present.
    for phase, arts in ARTEFACTS.items():
        if phase not in done:
            continue
        for art in arts:
            if not tree_has_file(art):
                fails.append("%s: marked done but %s missing" % (phase, art))

    # The on-disk headers must be honest.  A retracted phrase is a violation
    # only when it CLAIMS the feature; an honest negation ("X is not
    # implemented", "no X") is exactly what F6 wants and is not flagged.
    NEG = ("not implemented", "NOT implemented", "not supported",
           "out of scope", "out of scope", "no ", "none", "never",
           "deliberately no", "refused")
    for hdr in ["ext4.h", "f2fs.h", "btrfs.h", "exfat.h", "ntfs.h"]:
        txt = read("kernel", "fs", hdr)
        if not txt:
            fails.append("headers: kernel/fs/%s missing" % hdr)
            continue
        for phrase in RETRACTED:
            for m in re.finditer(re.escape(phrase), txt):
                head = txt[max(0, m.start() - 60):m.start()].lower()
                tail = txt[m.end():m.end() + 60].lower()
                # skip if the claim is negated/refuted either side of it
                if any(neg in head for neg in NEG) or \
                   any(neg in tail for neg in NEG):
                    continue
                fails.append("headers: %s still claims '%s'" % (hdr, phrase))
        if not any(b in txt for b in BOUNDARY):
            fails.append("headers: %s states no honest boundary "
                         "(out of scope / refused / read-only)" % hdr)

    return fails


def main():
    plan = read("FSFULL_PLAN.md")

    if "--selftest" in sys.argv:
        # Planted violation 1: a done phase's patch receipt is missing.
        fails = check_plan(plan, lambda p: p != "patches/FS_F5b_ntfs.patch")
        if not any("F5b: marked done but patches/FS_F5b_ntfs.patch missing"
                   in f for f in fails):
            print("check_fsfull_claims: SELFTEST FAILED -- planted "
                  "missing-receipt violation not caught")
            return 1
        # Planted violation 2: a header still claims a retracted feature.
        fake = plan
        fails = check_plan(fake, lambda *p: True)
        btrfs_txt = read("kernel", "fs", "btrfs.h")
        planted = btrfs_txt + "/* subvolumes and snapshots */"
        orig_read = read
        read_backup = globals()["read"]
        # Simulate a btrfs.h that still claims subvolumes.
        import builtins  # noqa: E402
        def fake_read(*parts):
            if parts == ("kernel", "fs", "btrfs.h"):
                return planted
            return orig_read(*parts)
        globals()["read"] = fake_read
        fails = check_plan(fake, lambda *p: True)
        globals()["read"] = read_backup
        if not any("headers: btrfs.h still claims 'subvolumes and snapshots'"
                   in f for f in fails):
            print("check_fsfull_claims: SELFTEST FAILED -- planted "
                  "retracted-claim violation not caught")
            return 1
        print("check_fsfull_claims: SELFTEST OK (planted violations caught)")
        return 0

    def tree_has_file(*parts):
        return bool(read(*parts))

    fails = check_plan(plan, tree_has_file)
    if fails:
        print("check_fsfull_claims: FAIL -- %d finding(s):" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("check_fsfull_claims: OK -- FSFULL_PLAN.md phases and receipts "
          "agree with the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
