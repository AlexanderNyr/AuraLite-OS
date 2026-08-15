#!/usr/bin/env python3
"""Fail when tests/integration/cases/ and run_all.sh's ALL_CASES disagree.

Why this exists
---------------
run_all.sh hardcodes the list of integration cases it runs.  At the time this
guard was written there were 124 case files on disk and 97 names in the list:
27 cases -- a fifth of the suite, including several written specifically as
phase gates -- were never executed by CI.  A gate nobody runs protects
nothing, and the suite reported "118/118 green" while that fifth sat unread.

Adding a case file is easy to remember; editing ALL_CASES is easy to forget.
This makes forgetting a build failure instead of a silent hole.

Usage:
    tools/check_test_registry.py [--check]

Exit status is non-zero if the two disagree.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES_DIR = os.path.join(ROOT, "tests", "integration", "cases")
RUN_ALL = os.path.join(ROOT, "tests", "integration", "run_all.sh")


def registered():
    """Names inside the ALL_CASES=( ... ) array in run_all.sh."""
    try:
        with open(RUN_ALL, "r", encoding="utf-8") as handle:
            text = handle.read()
    except OSError as exc:
        print("check_test_registry: cannot read run_all.sh: %s" % exc,
              file=sys.stderr)
        return None

    match = re.search(r"ALL_CASES=\((.*?)^\)", text, re.S | re.M)
    if not match:
        print("check_test_registry: ALL_CASES=( ... ) not found in run_all.sh",
              file=sys.stderr)
        return None

    names = set()
    for line in match.group(1).splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.update(line.split())
    return names


def on_disk():
    try:
        entries = os.listdir(CASES_DIR)
    except OSError as exc:
        print("check_test_registry: cannot read cases/: %s" % exc,
              file=sys.stderr)
        return None
    return {name[:-3] for name in entries if name.endswith(".sh")}


def main():
    have = on_disk()
    listed = registered()
    if have is None or listed is None:
        return 1

    orphans = sorted(have - listed)     # file exists, never run
    phantoms = sorted(listed - have)    # listed, no file

    if not orphans and not phantoms:
        print("check_test_registry: %d integration cases, all registered"
              % len(have))
        return 0

    print("check_test_registry: cases/ and ALL_CASES disagree\n",
          file=sys.stderr)
    if orphans:
        print("  %d case(s) on disk but NOT in ALL_CASES -- these never run:"
              % len(orphans), file=sys.stderr)
        for name in orphans:
            print("      %s" % name, file=sys.stderr)
    if phantoms:
        print("  %d name(s) in ALL_CASES with no case file:" % len(phantoms),
              file=sys.stderr)
        for name in phantoms:
            print("      %s" % name, file=sys.stderr)
    print("\nAdd them to ALL_CASES in tests/integration/run_all.sh, or remove "
          "the stale name.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
