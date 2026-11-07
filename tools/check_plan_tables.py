#!/usr/bin/env python3
"""Cross-check the plan tables, the TODO box tags and the ledger against each other.

RESIDUE3 C0 (the rig).  Seven checkers already pin *claims* to
artefacts; none of them reads the three places where a status can drift
away from the tree without any claim changing:

  R1  orphan residue  — a W/S ledger row that no live plan phase owns;
  R2  stale box tag   — a TODO `- [ ]` box tagged with a phase already ✅;
  R3  closure claim   — a ✅ phase whose prose says a RES row closes while
                        the row is still OPEN/HANDED-OFF;
  R4  table drift     — a plan's phase table disagreeing with that same
                        plan's per-phase status (heading glyph or
                        `**Status:` line).

R1..R4 are the classes the RESIDUE3 audit found, one rule each.  The
live violations are pinned in `tools/plan_tables_baseline.txt` the way
`tools/residue_baseline.txt` pins marker counts: a NEW violation fails,
and so does a FIXED one that did not move the pin in the same commit
(a ratchet that can silently shrink is not a ratchet).

Usage:
    tools/check_plan_tables.py            # --check against the pin
    tools/check_plan_tables.py --report   # live list, no pin compare
    tools/check_plan_tables.py --openers  # re-derive quoted line counts
    tools/check_plan_tables.py --bless    # print the pin the tree needs
    tools/check_plan_tables.py --selftest # doctored-tree receipts
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PIN = os.path.join("tools", "plan_tables_baseline.txt")
LEDGER = os.path.join("docs", "residue_ledger.md")
TODO = "TODO.md"

PLAN_GLOB = os.path.join("docs", "plans", "*_PLAN.md")

# A phase id as the tables and headings write them: C0, T8, R12, Y7, L7,
# D7, G13, W32A-12, WR-4, SH7d, RT3, V0, Q12, M5, O9, H3, FS9, F7.
PHASE_ID = re.compile(r"^(?=.{2,9}$)(?=.*\d)[A-Z][A-Z0-9]*(-[A-Z0-9]+)?[a-z]?$")
PHASE_ID_ANY = re.compile(r"\b([A-Z][A-Z0-9]{0,4}(?:-[A-Z0-9]{1,3})?[a-z]?)\b")

LEDGER_ROW = re.compile(r"^\| (RES-\d\d) \| (\w) \| ([A-Z@R\d-]+) \|")
DONE_STATES = ("DONE",)

# Words that turn "this row is mentioned" into "this row is claimed closed".
CLOSURE_VERB = re.compile(
    r"\b(close[sd]?|closing|flip(s|ped)?|flipped|DONE@|landed|shipped)\b", re.I)

STATUS_GLYPHS = (("\u2705", "done"), ("\U0001F7E8", "partial"),
                 ("\u2B1C", "planned"))
STATUS_WORDS = (("COMPLETE", "done"), ("PARTIAL", "partial"),
                ("IN PROGRESS", "inprogress"), ("PLANNED", "planned"))


# --------------------------------------------------------------------------
# reading helpers
# --------------------------------------------------------------------------

def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def plan_files():
    return sorted(glob.glob(os.path.join(ROOT, PLAN_GLOB)))


def status_of(text):
    """Classify a status fragment: heading tail or a `**Status:` body."""
    if not text:
        return None
    for glyph, name in STATUS_GLYPHS:
        if glyph in text:
            return name
    upper = text.upper()
    for word, name in STATUS_WORDS:
        if word in upper:
            return name
    return None


# --------------------------------------------------------------------------
# plan parsing: table rows and phase sections
# --------------------------------------------------------------------------

STATE_CELL = re.compile(r"^\s*(?:\u2705|\U0001F7E8|\u2B1C|"
                        r"done\b|planned\b|partial\b|todo\b|shipped\b)",
                        re.I)


def table_rows(text):
    """[(phase_id, state, line_no)] from a plan's phase table.

    The state cell must START with a state glyph or a bare state word:
    a plan's *dependency* tables also begin their rows with a phase id
    ("| W32A-8 | COMCTL32: needs windows ... |"), and reading those as
    status claims was this checker's first false positive.
    """
    out = []
    for n, line in enumerate(text.split("\n"), 1):
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 2:
            continue
        first = cells[0].split("\u2014")[0].split(" ")[0].strip()
        if not PHASE_ID.match(first):
            continue
        if not STATE_CELL.match(cells[1]):
            continue
        state = status_of(cells[1])
        if state is None:
            continue
        out.append((first, state, n))
    return out


def phase_sections(text):
    """{phase_id: (state, [lines])} from `### ...` phase sections.

    Two shapes exist in this tree and both are read:

        ### Phase W32A-1 - Loader ... ✅ DONE     (status in the heading)
        ### T7 - GUI / **Status: ✅ COMPLETE**    (status on its own line)
    """
    lines = text.split("\n")
    sections = {}
    starts = []
    for n, line in enumerate(lines):
        if not line.startswith("### "):
            continue
        hit = None
        for tok in PHASE_ID_ANY.findall(line):
            if PHASE_ID.match(tok):
                hit = tok
                break
        if hit:
            starts.append((n, hit))
    for i, (n, pid) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else len(lines)
        body = lines[n:end]
        state = status_of(body[0])
        if state is None:
            for candidate in body[:12]:
                stripped = candidate.replace("*", "").strip()
                if stripped.lower().startswith("status:"):
                    state = status_of(stripped.split(":", 1)[1])
                    break
        sections[pid] = (state, body)
    return sections


# --------------------------------------------------------------------------
# the four rules
# --------------------------------------------------------------------------

def ledger_rows():
    rows = {}
    for line in read(LEDGER).split("\n"):
        m = LEDGER_ROW.match(line)
        if m:
            rows[m.group(1)] = {"class": m.group(2),
                                "status": re.split(r"[@(]", m.group(3))[0]}
    return rows


def dod_paragraph(body):
    """The lines of a phase's `**Definition of done:**` paragraph."""
    out = []
    inside = False
    for line in body:
        if line.strip().lower().startswith("**definition of done"):
            inside = True
            out.append(line)
            continue
        if inside:
            stripped = line.strip()
            if not stripped:
                break
            if stripped.startswith("**") and stripped.endswith(":**"):
                break
            out.append(line)
    return out


def todo_boxes():
    """[(line_no, slug, [tags])] for every `- [ ]` box in TODO.md."""
    lines = read(TODO).split("\n")
    out = []
    for i, line in enumerate(lines):
        if not line.startswith("- [ ]"):
            continue
        block = [line]
        j = i + 1
        while j < len(lines) and lines[j].startswith("  "):
            block.append(lines[j])
            j += 1
        text = " ".join(b.strip() for b in block)
        tags = re.findall(r"\((RESIDUE\d?)\s+([A-Z]{1,3}\d+[a-z]?)\)", text)
        slug = re.sub(r"\s+", " ", line[6:].strip())[:60].rstrip()
        out.append((i + 1, slug, tags))
    return out


def tag_plan(series):
    return "docs/plans/%s_PLAN.md" % series


def violates(sections_by_plan, rows):
    """Return the sorted list of live violation ids."""
    found = set()

    # R1 - a W/S row no plan phase mentions at all.
    mentioned = set()
    for _plan, sections in sections_by_plan.items():
        for _state, body in sections.values():
            for line in body:
                for rid in re.findall(r"RES-\d\d", line):
                    mentioned.add(rid)
    for rid, row in rows.items():
        if row["status"] == "DONE":
            continue
        if row["class"] in ("M", "N"):
            continue          # metal-pending / re-affirmed non-goals
        if rid not in mentioned:
            found.add("r1:%s" % rid)

    # R2 - a TODO box tagged with a phase that is already done.
    for _line_no, slug, tags in todo_boxes():
        for series, pid in tags:
            plan = tag_plan(series)
            sections = sections_by_plan.get(plan)
            if not sections or pid not in sections:
                continue
            if sections[pid][0] == "done":
                found.add("r2:TODO.md:%s" % slug)

    # R3 - a done phase whose *Definition of done* claims a row closes,
    # while the row is still OPEN/HANDED-OFF.  The DoD paragraph is the
    # claim; a phase's Result is the receipt, and a Result may name a row
    # without claiming it (T9's "ledger OPEN 6 -> 1 (RES-54)" is arithmetic,
    # not a closure) - so the rule reads the claim and never the receipt.
    for _plan, sections in sections_by_plan.items():
        for pid, (state, body) in sections.items():
            if state != "done":
                continue
            for line in dod_paragraph(body):
                if not CLOSURE_VERB.search(line):
                    continue
                for rid in re.findall(r"RES-\d\d", line):
                    if rid in rows and rows[rid]["status"] not in DONE_STATES:
                        found.add("r3:%s" % rid)

    # R4 - a table row disagreeing with its own plan's section status.
    for path in plan_files():
        name = os.path.basename(path)
        body = read(path)
        sections = phase_sections(body)
        for pid, state, _n in table_rows(body):
            if pid not in sections:
                continue
            sec = sections[pid][0]
            if sec is None:
                continue
            if state != sec:
                found.add("r4:%s:%s" % (name, pid))

    return sorted(found)


def collect():
    sections_by_plan = {}
    for path in plan_files():
        sections_by_plan[os.path.relpath(path, ROOT)] = \
            phase_sections(read(path))
    return violates(sections_by_plan, ledger_rows())


# --------------------------------------------------------------------------
# openers: re-derive the line counts the ledger quotes
# --------------------------------------------------------------------------

QUOTED = re.compile(r"([a-z0-9_]+\.(?:c|h|asm|S))\s+(\d+)\b")


def opener_drift():
    """Lines in non-DONE ledger rows quoting a file's line count.

    Only the `name.c N` form is re-derived.  A row that quotes a bare
    basename ("xhci 1995 lines", RES-38) is NOT caught here - that needs a
    human - so the mode reports what it can prove and says nothing about
    the rest rather than implying a clean bill of health.
    """
    out = []
    for line in read(LEDGER).split("\n"):
        m = LEDGER_ROW.match(line)
        if not m:
            continue
        rid = m.group(1)
        if re.split(r"[@(]", m.group(3))[0] == "DONE":
            continue
        for name, quoted in QUOTED.findall(line):
            hits = glob.glob(os.path.join(ROOT, "**", name), recursive=True)
            hits = [h for h in hits if "/build/" not in h]
            if not hits:
                continue
            try:
                actual = sum(1 for _ in open(hits[0], encoding="utf-8",
                                             errors="replace"))
            except OSError:
                continue
            if actual != int(quoted):
                out.append("%s %s quoted %s, actual %d (%s)"
                           % (rid, name, quoted, actual,
                              os.path.relpath(hits[0], ROOT)))
    return out


# --------------------------------------------------------------------------
# pin handling
# --------------------------------------------------------------------------

def read_pin():
    try:
        with open(os.path.join(ROOT, PIN), encoding="utf-8") as fh:
            return sorted(l.split("#")[0].strip() for l in fh
                          if l.split("#")[0].strip())
    except OSError:
        return None


# --------------------------------------------------------------------------
# selftest: a doctored tree must fail, class by class
# --------------------------------------------------------------------------

FIXTURE_PLAN = """# Fixture plan

## Status: PLANNED - 0/2 phases landed

| Phase | State |
|-------|-------|
| X0 - the clean phase | \u2705 done |
| X1 - the open phase | \u2B1C planned |

### X0 - the clean phase

**Status:** \u2705 COMPLETE

**Definition of done:** nothing here claims a ledger row.

### X1 - the open phase

**Status:** \u2B1C planned

Owns RES-02 (the row this fixture keeps open).

- [ ] a task
"""

FIXTURE_LEDGER = """| ID | Class | Status | Item (source) | Exit gate |
|----|-------|--------|---------------|-----------|
| RES-01 | W | DONE@X0 | closed at X0 | gate |
| RES-02 | W | OPEN | named by X1 | gate |
| RES-03 | M | PENDING-USER@X0 | metal | gate |
"""

FIXTURE_TODO = "- [ ] a task owned by X1. (RESIDUE3 X1)\n"


def selftest():
    """Build doctored trees in a temp dir; each planted class must report."""
    import shutil
    import tempfile

    cases = []

    def build(plan=FIXTURE_PLAN, ledger=FIXTURE_LEDGER, todo=FIXTURE_TODO,
              pin=None):
        tmp = tempfile.mkdtemp(prefix="plantables-")
        os.makedirs(os.path.join(tmp, "docs", "plans"), exist_ok=True)
        os.makedirs(os.path.join(tmp, "tools"), exist_ok=True)
        open(os.path.join(tmp, "docs", "plans", "RESIDUE3_PLAN.md"),
             "w").write(plan)
        open(os.path.join(tmp, "docs", "residue_ledger.md"), "w").write(ledger)
        open(os.path.join(tmp, "TODO.md"), "w").write(todo)
        if pin is not None:
            open(os.path.join(tmp, "tools", "plan_tables_baseline.txt"),
                 "w").write("\n".join(pin) + "\n")
        return tmp

    def run(tmp):
        global ROOT
        saved = ROOT
        ROOT = tmp
        try:
            return collect()
        finally:
            ROOT = saved

    # 1 - the clean fixture reports nothing.
    tmp = build()
    got = run(tmp)
    cases.append(("clean fixture reports no violation", got == [], got))
    shutil.rmtree(tmp)

    # 2 - planted table drift: the table says planned, the section is done.
    tmp = build(plan=FIXTURE_PLAN.replace(
        "| X0 - the clean phase | \u2705 done |",
        "| X0 - the clean phase | \u2B1C planned |"))
    got = run(tmp)
    cases.append(("planted table drift reports r4",
                  any(v.startswith("r4:") for v in got), got))
    shutil.rmtree(tmp)

    # 3 - planted orphan box: a box tagged on the done phase X0.
    tmp = build(todo="- [ ] a task owned by X0. (RESIDUE3 X0)\n")
    got = run(tmp)
    cases.append(("planted stale box tag reports r2",
                  any(v.startswith("r2:") for v in got), got))
    shutil.rmtree(tmp)

    # 4 - planted closure claim: a done phase's DoD says RES-02 closes.
    tmp = build(plan=FIXTURE_PLAN.replace(
        "nothing here claims a ledger row.",
        "RES-02 closes with this phase."))
    got = run(tmp)
    cases.append(("planted closure claim reports r3",
                  any(v.startswith("r3:") for v in got), got))
    shutil.rmtree(tmp)

    # 5 - planted orphan residue: the phase stops naming RES-02.
    tmp = build()
    got = run(tmp)
    cases.append(("a named row is not orphaned", "r1:RES-02" not in got, got))
    tmp2 = build(plan=FIXTURE_PLAN.replace(
        "Owns RES-02 (the row this fixture keeps open).", "Owns nothing."))
    got2 = run(tmp2)
    cases.append(("planted orphan residue reports r1",
                  "r1:RES-02" in got2, got2))
    shutil.rmtree(tmp)
    shutil.rmtree(tmp2)

    # 6 - the pin ratchet: an exact pin passes, a stale pin fails.
    tmp = build(todo="- [ ] a task owned by X0. (RESIDUE3 X0)\n",
                pin=["r2:TODO.md:a task owned by X0. (RESIDUE3 X0)"])
    got = run(tmp)
    saved = ROOT
    globals()["ROOT"] = tmp
    try:
        exact = (read_pin() == got)
    finally:
        globals()["ROOT"] = saved
    cases.append(("an exact pin matches the live list", exact, got))
    shutil.rmtree(tmp)

    tmp = build(todo="- [ ] a task owned by X0. (RESIDUE3 X0)\n",
                pin=["r2:TODO.md:some earlier wording"])
    saved = ROOT
    globals()["ROOT"] = tmp
    try:
        pin = read_pin()
    finally:
        globals()["ROOT"] = saved
    live = run(tmp)
    cases.append(("a stale pin is detected",
                  sorted(pin) != sorted(live), (pin, live)))
    shutil.rmtree(tmp)

    failed = 0
    for name, ok, detail in cases:
        print(("  ok   " if ok else "  FAIL ") + name +
              ("" if ok else "  -> %r" % (detail,)))
        if not ok:
            failed += 1
    print("check_plan_tables --selftest: %d/%d" % (len(cases) - failed,
                                                   len(cases)))
    return 0 if failed == 0 else 1


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main(argv):
    root = os.environ.get("PLAN_TABLES_ROOT")
    if root:
        globals()["ROOT"] = root

    if "--selftest" in argv:
        return selftest()

    live = collect()

    if "--report" in argv:
        for v in live:
            print(v)
        print("check_plan_tables: %d live violation(s)" % len(live))
        return 0

    if "--openers" in argv:
        drift = opener_drift()
        for line in drift:
            print("opener drift: " + line)
        print("check_plan_tables --openers: %d stale opener(s)" % len(drift))
        return 0

    if "--bless" in argv:
        for v in live:
            print(v)
        return 0

    pin = read_pin()
    if pin is None:
        print("check_plan_tables: FAIL - %s is missing (pin the tree with "
              "--bless and commit the file)" % PIN)
        return 1

    new = [v for v in live if v not in pin]
    fixed = [v for v in pin if v not in live]
    for v in new:
        print("check_plan_tables: NEW   " + v)
    for v in fixed:
        print("check_plan_tables: FIXED " + v +
              " (move the pin in the same commit)")

    if new or fixed:
        print("check_plan_tables: FAIL - %d new, %d fixed, %d pinned"
              % (len(new), len(fixed), len(pin)))
        return 1

    if live:
        print("check_plan_tables: OK - %d pinned violation(s) match the tree"
              % len(live))
    else:
        print("check_plan_tables: OK - no violation reaches the tree "
              "(the pin is empty; the next one is red)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
