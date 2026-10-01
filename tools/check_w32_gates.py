#!/usr/bin/env python3
"""Keep tests/integration/w32_required_gates.txt honest.

The Win32 personality's CI gate list was inlined in the workflow until the
single `w32` shard became the critical path (86 min against 46 for the
next-slowest) and was split three ways.  A split inlined list can silently
shrink: drop a case from one shard's arm and every shard still passes its
own, shorter list, so the gate stops running and CI stays green.  The list
therefore lives in one file and this checker pins it:

  1. exactly REQUIRED_TOTAL entries (the count the single shard asserted);
  2. every entry names a case that really exists in
     tests/integration/cases/ AND is registered in run_all.sh's ALL_CASES
     (an unregistered case never runs -- the AUDIT_A0 disease);
  3. every entry is filed under the shard whose group_re() actually
     matches it, so the workflow asks the right job for the right gate;
  4. every shard named in the file is a real group in GROUP_NAMES.

Same shape as the other tools/check_*_claims.py gates: a hand-maintained
claim drifts, a checked one cannot.  --selftest proves the checker still
detects a violation.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GATES = os.path.join(ROOT, "tests", "integration", "w32_required_gates.txt")
RUNNER = os.path.join(ROOT, "tests", "integration", "run_all.sh")
CASEDIR = os.path.join(ROOT, "tests", "integration", "cases")

# The single pre-split `w32` shard asserted 16 non-vacuous gates.  The
# three-way split must keep covering the same 16; raising this number is a
# deliberate act that belongs in the same commit as the new gate.
REQUIRED_TOTAL = 16


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def parse_gates(text):
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 2:
            out.append((n, None, None))
            continue
        out.append((n, parts[0], parts[1]))
    return out


def runner_groups(src):
    """group name -> regex, straight out of run_all.sh's group_re()."""
    groups = {}
    for m in re.finditer(r"^\s{8}([a-z0-9-]+)\)\s+echo '([^']+)'", src, re.M):
        groups[m.group(1)] = m.group(2)
    return groups


def runner_cases(src):
    m = re.search(r"ALL_CASES=\((.*?)^\)", src, re.S | re.M)
    if not m:
        return set()
    return set(re.findall(r"\btest_[a-z0-9_]+\b", m.group(1)))


def check(gates_text):
    problems = []
    src = read(RUNNER)
    groups = runner_groups(src)
    registered = runner_cases(src)
    names = re.search(r'GROUP_NAMES="([^"]+)"', src)
    group_names = set(names.group(1).split()) if names else set()

    entries = parse_gates(gates_text)

    for lineno, shard, case in entries:
        if shard is None:
            problems.append(f"line {lineno}: not '<shard> <case>'")
            continue
        if shard not in group_names:
            problems.append(
                f"line {lineno}: shard '{shard}' is not in GROUP_NAMES")
            continue
        if case not in registered:
            problems.append(
                f"line {lineno}: {case} is not in run_all.sh's ALL_CASES "
                "(it would never run)")
            continue
        if not os.path.exists(os.path.join(CASEDIR, case + ".sh")):
            problems.append(
                f"line {lineno}: tests/integration/cases/{case}.sh is missing")
            continue
        rx = groups.get(shard)
        if rx is None or not re.match(rx, case):
            owner = [g for g, r in groups.items() if re.match(r, case)]
            problems.append(
                f"line {lineno}: {case} is filed under '{shard}' but "
                f"run_all.sh puts it in {owner or 'NO group'}")

    total = sum(1 for _, s, _ in entries if s is not None)
    if total != REQUIRED_TOTAL:
        problems.append(
            f"the file lists {total} required gates, not {REQUIRED_TOTAL} "
            "-- the three w32 shards must together still assert every gate "
            "the single shard did (raise REQUIRED_TOTAL in the same commit "
            "if a gate was genuinely added)")
    return problems, total


def selftest():
    """Drop a gate and prove the checker notices."""
    text = read(GATES)
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.strip().startswith("w32-"):
            doctored = "\n".join(lines[:i] + lines[i + 1:])
            break
    else:
        print("check_w32_gates: SELFTEST FAIL -- no gate line found",
              file=sys.stderr)
        return 1
    problems, _ = check(doctored)
    if not problems:
        print("check_w32_gates: SELFTEST FAIL -- a removed gate was "
              "not detected", file=sys.stderr)
        return 1

    misfiled = text.replace("w32-gui test_w32a7_gdi",
                            "w32-core test_w32a7_gdi", 1)
    problems2, _ = check(misfiled)
    if not any("filed under" in p for p in problems2):
        print("check_w32_gates: SELFTEST FAIL -- a gate filed under the "
              "wrong shard was not detected", file=sys.stderr)
        return 1

    print("check_w32_gates: selftest PASS (dropped gate and misfiled gate "
          "both detected)")
    return 0


def main():
    if "--selftest" in sys.argv:
        return selftest()
    problems, total = check(read(GATES))
    if problems:
        for p in problems:
            print(f"  FAIL: {p}")
        print(f"check_w32_gates: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    print(f"check_w32_gates: OK -- {total} required Win32 gates, each "
          "registered and filed under the shard that runs it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
