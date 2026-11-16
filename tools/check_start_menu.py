#!/usr/bin/env python3
"""check_start_menu.py — OF0 gate for the shell around the office suite.

docs/plans/OFFICE_PLAN.md OF0 asks for two things that a boot cannot check
cheaply:

  * menu entries grouped (Applications / Office / System) with a height
    derived from the entry count, and
  * a taskbar whose buttons cannot vanish.

Both are the kind of defect that produces no error: a stale /apps path is a
menu row that silently does nothing (the file's own comment says so), and the
2026-11-08 bug was two copies of one piece of geometry drifting apart.  So this
checker asserts over the *source*, where those bugs are visible:

  1. every catalogue entry names a binary that the build produces;
  2. every entry has a known group, every group has a title, no empty or
     duplicated entries;
  3. the renderer and the hit test both go through the shared geometry
     (gui_tb_layout / gui_tb_button_rect in C, taskbar_layout::layout /
     taskbar_layout::button_rect in Rust) and neither carries its own button
     width or its own walk of the catalogue;
  4. the start menu height is derived, not a constant.

This checker has now been blinded twice by refactors that moved the code
without changing a single rule:

  * PR #8 lifted the start menu and the taskbar out of gui.c into gui_shell.c
    and renamed draw_taskbar() to shell_draw_taskbar().  The checker read
    gui.c only, so the move turned it into a no-op that still exited 0.
  * PR #13 ("migrate desktop shell to Rust") deleted gui_shell.c entirely and
    moved the catalogue, the taskbar and the start menu into
    kernel/gui/desktop/*.rs -- the app list is now `const APPS: [AppEntry; N]`
    in apps.rs, and shell_draw_taskbar() is a `pub extern "C" fn` in
    taskbar.rs.

Each time the failure was the same: the checker knew WHERE to look instead of
WHAT to look for.  It now reads every .c and .rs source under kernel/gui/,
recursively, and accepts either spelling of every anchor -- C or Rust -- so the
next migration cannot blind it a third time.  The properties it asserts have
not changed at all, which is the point: the rules were never about C.

--selftest injects each defect into a temporary copy and requires a complaint,
because a checker that cannot fail is indistinguishable from a clean tree.

Usage: tools/check_start_menu.py [--check] [--selftest]
"""

from __future__ import annotations

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUI_DIR = os.path.join(ROOT, "kernel", "gui")
GUI_HDR = os.path.join(GUI_DIR, "taskbar_layout.h")
GUI_RS_LAYOUT = os.path.join(GUI_DIR, "desktop", "taskbar_layout.rs")
MAKEFILE = os.path.join(ROOT, "Makefile")
INITRD_APPS = os.path.join(ROOT, "build", "user", "initrd_root", "apps")

# ---- the C spelling (pre-PR #13) -------------------------------------------

C_ENTRY_RE = re.compile(
    r'\{\s*"(?P<name>[^"]+)"\s*,\s*"(?P<path>[^"]+)"\s*,\s*'
    r"(?P<icon>\d+)\s*,\s*(?P<group>GUI_APP_GRP_[A-Z]+)\s*\}")

C_GROUPS = ("GUI_APP_GRP_APPS", "GUI_APP_GRP_OFFICE", "GUI_APP_GRP_SYSTEM")

# ---- the Rust spelling (PR #13 onwards) ------------------------------------

# AppEntry { name: "Terminal", path: b"/apps/gterm\0", icon_id: 1,
#            group: AppGroup::Applications },
RS_ENTRY_RE = re.compile(
    r"AppEntry\s*\{\s*name:\s*\"(?P<name>[^\"]+)\"\s*,\s*"
    r"path:\s*b?\"(?P<path>[^\"]*)\"\s*,\s*"
    r"icon_id:\s*(?P<icon>-?\d+)\s*,\s*"
    r"group:\s*AppGroup::(?P<group>[A-Za-z]+)\s*\}")

# const GROUPS: [(AppGroup, &str); 3] = [ (AppGroup::Applications, "Applications"), ... ]
RS_GROUPS_RE = re.compile(r"const\s+GROUPS\b[^=]*=\s*\[(?P<body>.*?)\n\];", re.S)

RS_GROUPS = ("Applications", "Office", "System")


def read(path: str) -> str:
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def gui_sources() -> dict[str, str]:
    """Every C and Rust source under kernel/gui/, recursively, by path.

    Reading the whole tree -- C and Rust both -- is what keeps this checker
    honest through the next extraction.  Knowing the file name is what blinded
    it twice.
    """
    found: dict[str, str] = {}
    for dirpath, _dirnames, filenames in os.walk(GUI_DIR):
        for name in sorted(filenames):
            if not (name.endswith(".c") or name.endswith(".rs")):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, ROOT)
            found[rel] = read(full)
    if not found:
        raise SystemExit("check_start_menu: no sources under kernel/gui/")
    return found


def gui_source() -> str:
    return "\n".join(gui_sources().values())


def app_table(sources: dict[str, str]) -> list[dict]:
    """The catalogue, in whichever language it is currently written."""
    src = "\n".join(sources.values())

    entries = []
    for m in RS_ENTRY_RE.finditer(src):
        d = m.groupdict()
        d["path"] = d["path"].replace("\\0", "")
        d["lang"] = "rust"
        entries.append(d)
    if entries:
        return entries

    start = src.find("static const gui_app_entry_t gui_apps[] = {")
    if start >= 0:
        end = src.find("\n};", start)
        if end >= 0:
            for m in C_ENTRY_RE.finditer(src[start:end]):
                d = m.groupdict()
                d["lang"] = "c"
                entries.append(d)
            if entries:
                return entries

    raise SystemExit("check_start_menu: the app catalogue was not found under "
                     "kernel/gui/ (looked for Rust `const APPS` and C "
                     "`gui_apps[]`)")


def group_titles(sources: dict[str, str]) -> list[str] | None:
    """The group titles, or None when neither spelling is present."""
    src = "\n".join(sources.values())

    m = RS_GROUPS_RE.search(src)
    if m:
        return re.findall(r'"([^"]*)"', m.group("body"))

    m = re.search(r"gui_group_titles\[GUI_APP_GRP_COUNT\]\s*=\s*\{(.*?)\};",
                  src, re.S)
    if m:
        return re.findall(r'"([^"]*)"', m.group(1))
    return None


def known_groups(lang: str) -> tuple[str, ...]:
    return RS_GROUPS if lang == "rust" else C_GROUPS


def function_body(src: str, name: str) -> str:
    """The body of the *definition* of `name`, or "" when it is not defined.

    The first `name(` in the tree is usually a CALL, not a definition: the
    compositor in gui.c calls shell_draw_taskbar() long before taskbar.rs
    defines it, and the old `src.find(name + "(")` therefore returned an
    unrelated brace block -- which is how a rename could turn a real
    violation into a clean run.  Anchor on a definition instead: the name
    preceded by a return type at the start of a line, then a parameter list
    with no semicolon or brace in it, then the opening brace.
    """
    pattern = re.compile(
        r"^[A-Za-z_][A-Za-z0-9_ \t*]*\b" + re.escape(name) +
        r"\s*\([^;{}]*\)\s*\{", re.M)
    match = pattern.search(src)
    if not match:
        return ""
    return _brace_body(src, match.end() - 1)


def rust_function_body(src: str, name: str) -> str:
    """The body of `fn name(...) { ... }`, allowing `pub extern "C"`."""
    pattern = re.compile(
        r"^[ \t]*(?:pub(?:\([a-z]+\))?\s+)?(?:extern\s+\"C\"\s+)?"
        r"(?:unsafe\s+)?fn\s+" + re.escape(name) + r"\s*(?:<[^>]*>)?\s*"
        r"\([^{;]*?\)\s*(?:->\s*[^{;]*)?\{", re.M)
    match = pattern.search(src)
    if not match:
        return ""
    return _brace_body(src, match.end() - 1)


def _brace_body(src: str, brace: int) -> str:
    depth = 0
    for pos in range(brace, len(src)):
        if src[pos] == "{":
            depth += 1
        elif src[pos] == "}":
            depth -= 1
            if depth == 0:
                return src[brace:pos + 1]
    return ""


def body_any(src: str, names) -> tuple[str, str]:
    """The first body found for any of `names` (a rename must not blind us)."""
    for name in names:
        body = rust_function_body(src, name)
        if body:
            return name, body
        body = function_body(src, name)
        if body:
            return name, body
    return "", ""


def built_apps() -> set[str]:
    """Basenames the image actually ships in /apps."""
    if os.path.isdir(INITRD_APPS):
        return {n for n in os.listdir(INITRD_APPS)
                if os.path.isfile(os.path.join(INITRD_APPS, n))}
    # Fall back to the Makefile's staging list so the check works on a clean
    # checkout (initrd not built yet) instead of skipping entirely.
    text = read(MAKEFILE)
    m = re.search(r"^INITRD_APPS\s*:?=\s*(.+?)\n(?=\S)", text, re.M | re.S)
    if not m:
        return set()
    return set(re.findall(r"[A-Za-z0-9_.-]+", m.group(1)))


def check(sources: dict[str, str], built: set[str],
          warnings: list[str]) -> list[str]:
    errors: list[str] = []
    src = "\n".join(sources.values())
    entries = app_table(sources)

    if not entries:
        errors.append("the app catalogue parsed to zero entries")

    lang = entries[0]["lang"] if entries else "rust"
    groups_known = known_groups(lang)

    # 1. every path is a binary the build produces
    if built:
        for e in entries:
            path = e["path"]
            if not path.startswith("/apps/"):
                errors.append(f"{e['name']}: path {path} is not under /apps/")
                continue
            base = path[len("/apps/"):]
            if base not in built:
                errors.append(
                    f"{e['name']}: path {path} has no binary in the image "
                    f"(menu row that silently does nothing)")

    # 2. groups
    titles = group_titles(sources)
    if titles is None:
        errors.append("the group titles were not found (looked for Rust "
                      "`const GROUPS` and C `gui_group_titles[]`)")
    else:
        if len(titles) != len(groups_known):
            errors.append(
                f"{len(titles)} group titles for {len(groups_known)} groups")
        elif len(set(titles)) != len(titles):
            errors.append("duplicate group title")

    seen_names, seen_paths = set(), set()
    for e in entries:
        if e["group"] not in groups_known:
            errors.append(f"{e['name']}: unknown group {e['group']!r}")
        if e["name"] in seen_names:
            errors.append(f"duplicate menu name {e['name']!r}")
        seen_names.add(e["name"])
        if e["path"] in seen_paths:
            errors.append(f"duplicate menu path {e['path']!r}")
        seen_paths.add(e["path"])
    groups_used = {e["group"] for e in entries}
    if groups_known[0] not in groups_used:
        errors.append(f"the {groups_known[0]} group is empty")

    # 3. the renderer and the hit test share the geometry.
    # C: gui_tb_layout() / gui_tb_button_rect(); Rust: layout::layout() /
    # layout::button_rect().  Accept either so a port is not a silent skip.
    layout_calls = ("gui_tb_layout(", "layout::layout(")
    rect_calls = ("gui_tb_button_rect(", "layout::button_rect(")
    # No generic fallback ("draw", "render"): accepting any name that happens
    # to exist would let a rename quietly swap in a different function's body
    # and report ITS violations under the renderer's name.  PR #8 did exactly
    # that rename, so both spellings are listed; nothing else is.
    draw_name, draw = body_any(src, ("shell_draw_taskbar", "draw_taskbar"))
    hit = rust_function_body(src, "taskbar_hit") or function_body(src, "taskbar_hit")
    for label, body in ((draw_name or "draw_taskbar", draw),
                        ("taskbar_hit", hit)):
        if not body:
            errors.append(f"{label}() not found")
            continue
        if not any(c in body for c in layout_calls):
            errors.append(f"{label}() does not call the shared layout "
                          f"(gui_tb_layout / layout::layout)")
        if not any(c in body for c in rect_calls):
            errors.append(f"{label}() does not call the shared button rect "
                          f"(gui_tb_button_rect / layout::button_rect)")
        # A real width is a literal of 20 px or more; the only zero assigned
        # to btn_w is the out-parameter the layout function fills in.  Both
        # languages spell the local the same way.
        if re.search(r"btn_w\s*=\s*(?:[2-9]\d|\d{3,})\b", body) or \
           re.search(r"button_width\s*:?=\s*(?:[2-9]\d|\d{3,})\b", body):
            errors.append(f"{label}() still hardcodes a button width")

    # 4. the start menu: height derived from the row count, and the hit test
    #    goes through the shared row mapping rather than walking the array.
    sm_hit = rust_function_body(src, "hit_test") or function_body(src, "start_menu_hit")
    if not sm_hit:
        errors.append("the start menu hit test was not found")
    else:
        if not any(t in sm_hit for t in ("menu_rect(", "menu_row(", "gui_menu_rows(")):
            errors.append("the start menu hit test does not use the shared "
                          "row mapping: the hit test and the drawn rows would "
                          "drift apart")
        if re.search(r"APPS\[|gui_apps\[", sm_hit):
            errors.append("the start menu hit test indexes the catalogue "
                          "directly again")

    sm_h = rust_function_body(src, "menu_height")
    if not sm_h:
        m = re.search(r"static uint32_t start_menu_height\(void\)\s*\{([^}]*)\}",
                      src)
        sm_h = m.group(1) if m else ""
    if not sm_h:
        errors.append("menu_height() not found — menu height must be derived")
    elif not any(t in sm_h for t in ("menu_row_count(", "gui_menu_rows(")):
        errors.append("menu_height() does not count rows")

    # 5. the shared layout module must actually contain the layout decisions.
    # It exists as a C header and as a Rust module; require at least one, and
    # check whichever is present.
    # Which module is authoritative is decided by what the tree actually
    # contains, not by an either/or that lets the live one disappear.  Accepting
    # "the C header OR the Rust module" is what let this checker stay green
    # after PR #13 deleted gui_shell.c: it found the stale C header, passed,
    # and said nothing while the geometry the user clicks on went untested.
    has_rust = any(name.endswith(".rs") for name in sources)
    if has_rust:
        if not os.path.isfile(GUI_RS_LAYOUT):
            errors.append("the shell is Rust (sources under kernel/gui/) but "
                          "desktop/taskbar_layout.rs is gone -- the shared "
                          "geometry has no home")
        else:
            rs = read(GUI_RS_LAYOUT)
            for token in ("pub fn layout(", "pub fn button_rect(",
                          "pub fn overflow_geometry("):
                if token not in rs:
                    errors.append(
                        f"{os.path.relpath(GUI_RS_LAYOUT, ROOT)} is missing "
                        f"{token}")
    elif os.path.isfile(GUI_HDR):
        hdr = read(GUI_HDR)
        for token in ("gui_tb_layout(", "gui_tb_button_rect(",
                      "gui_tb_overflow_geometry("):
            if token not in hdr:
                errors.append(f"{os.path.basename(GUI_HDR)} is missing {token}")
    else:
        errors.append("no shared taskbar layout module found (neither "
                      "taskbar_layout.h nor desktop/taskbar_layout.rs)")

    # Two copies of the same arithmetic is the drift that caused the
    # 2026-11-08 bug, so a second copy that no longer ships is still debt --
    # and a still-green test suite over it is worse than none, because it
    # looks like evidence.  Advertised, not failed: removing the C copy is a
    # separate decision, but it must not be silent.
    if has_rust and os.path.isfile(GUI_HDR):
        includers = [name for name, text in sources.items()
                     if name.endswith(".c") and "taskbar_layout.h" in text]
        if not includers:
            warnings.append(
                f"{os.path.relpath(GUI_HDR, ROOT)} still holds a second copy "
                f"of the taskbar geometry but no kernel source includes it; "
                f"tests/unit/test_taskbar_layout.c grades dead code while the "
                f"shipping Rust geometry is graded by "
                f"tests/unit/test_taskbar_layout_rs.rs")
    return errors


def run_check() -> int:
    sources = gui_sources()
    warnings: list[str] = []
    errors = check(sources, built_apps(), warnings)
    for w in warnings:
        print(f"check_start_menu: warning: {w}")
    if errors:
        print("check_start_menu: FAIL")
        for e in errors:
            print("  -", e)
        return 1
    entries = app_table(sources)
    groups = sorted({e["group"] for e in entries})
    print(f"check_start_menu: OK ({len(entries)} entries, "
          f"{len(groups)} groups, shared taskbar geometry)")
    return 0


# ---------------------------------------------------------------- selftest

MUTATIONS = [
    ("stale app path",
     'path: b"/apps/gcalc\\0"', 'path: b"/apps/gcalc-does-not-exist\\0"',
     "no binary in the image"),
    ("unknown group",
     "group: AppGroup::System }", "group: AppGroup::Bogus }",
     "unknown group"),
    ("duplicate entry",
     'AppEntry { name: "About",          path: b"/apps/gabout\\0",   icon_id: 6,  group: AppGroup::System },',
     'AppEntry { name: "About",          path: b"/apps/gabout\\0",   icon_id: 6,  group: AppGroup::System },\n'
     '    AppEntry { name: "About",          path: b"/apps/gabout\\0",   icon_id: 6,  group: AppGroup::System },',
     "duplicate menu name"),
    ("hit test re-walks the catalogue",
     "        Some(MenuRow::App(index)) => index as i32,",
     "        Some(MenuRow::App(_index)) => APPS[0].icon_id,\n"
     "        Some(MenuRow::App(index)) => index as i32,",
     "indexes the catalogue"),
    ("renderer hardcodes the button width",
     "    let button_height = taskbar_height.saturating_sub(8);",
     "    let button_height = taskbar_height.saturating_sub(8);\n"
     "    let button_width = 130;",
     "still hardcodes a button width"),
    # The regression this checker actually suffered, twice: PR #8 renamed
    # draw_taskbar() and PR #13 deleted gui_shell.c outright.  Both times a
    # checker that could not find the function reported no error at all.
    ("renderer renamed out from under the checker",
     'pub extern "C" fn shell_draw_taskbar() {',
     'pub extern "C" fn shell_draw_taskbar_v2() {',
     "not found"),
    ("menu height stops being derived",
     "        .saturating_add((apps::menu_row_count() as u32).saturating_mul(MENU_ITEM_HEIGHT))",
     "        .saturating_add(200u32)",
     "does not count rows"),
]


def run_selftest() -> int:
    sources = gui_sources()
    src = "\n".join(sources.values())
    built = built_apps()
    if not built:
        print("check_start_menu --selftest: SKIP (no app list available)")
        return 0
    failures = 0
    for name, old, new, expect in MUTATIONS:
        if old not in src:
            print(f"check_start_menu --selftest: FAIL ({name}: anchor missing)")
            failures += 1
            continue
        errs = check({k: v.replace(old, new, 1) for k, v in sources.items()},
                     built, [])
        joined = " | ".join(errs)
        if expect not in joined:
            print(f"check_start_menu --selftest: FAIL ({name}: expected "
                  f"{expect!r}, got {joined or 'no complaint'})")
            failures += 1
    # Negative control for the negative control: the unmodified tree is clean.
    if check(sources, built, []):
        print("check_start_menu --selftest: FAIL (clean tree reported errors)")
        failures += 1
    if failures:
        return 1
    print(f"check_start_menu --selftest: OK ({len(MUTATIONS)} defects detected, "
          f"clean tree clean)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return run_selftest()
    if args.check:
        return run_check()
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
