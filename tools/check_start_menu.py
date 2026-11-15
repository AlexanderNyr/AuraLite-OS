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

  1. every gui_apps[] entry names a binary that the build produces;
  2. every entry has a known group, every group has a title, no empty or
     duplicated entries;
  3. the renderer and the hit test both go through the shared geometry
     (gui_tb_layout / gui_tb_button_rect / gui_menu_rows) and neither carries
     its own button width or its own gui_apps[] walk;
  4. the start menu height is derived, not a constant.

The anchors live somewhere under kernel/gui/: PR #8 (4c5848e, "improve SDF
rendering and extract shell logic") lifted the start menu, the taskbar and the
desktop icons out of gui.c into gui_shell.c and renamed draw_taskbar() to
shell_draw_taskbar().  This checker read gui.c only, so the move turned it
into a no-op that still exited 0 -- exactly the failure it was written to
prevent.  It now reads every C file in kernel/gui/, so the next split cannot
blind it again.

--selftest injects each defect into a temporary copy and requires a complaint,
because a checker that cannot fail is indistinguishable from a clean tree.

Usage: tools/check_start_menu.py [--check] [--selftest]
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUI_DIR = os.path.join(ROOT, "kernel", "gui")
GUI_HDR = os.path.join(GUI_DIR, "taskbar_layout.h")
MAKEFILE = os.path.join(ROOT, "Makefile")
INITRD_APPS = os.path.join(ROOT, "build", "user", "initrd_root", "apps")

ENTRY_RE = re.compile(
    r'\{\s*"(?P<name>[^"]+)"\s*,\s*"(?P<path>[^"]+)"\s*,\s*'
    r"(?P<icon>\d+)\s*,\s*(?P<group>GUI_APP_GRP_[A-Z]+)\s*\}")

KERNEL_GROUPS = ("GUI_APP_GRP_APPS", "GUI_APP_GRP_OFFICE", "GUI_APP_GRP_SYSTEM")


def read(path: str) -> str:
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def gui_source() -> str:
    """Every C file under kernel/gui/, concatenated in sorted order.

    gui_apps[] and the taskbar/start-menu functions moved from gui.c to
    gui_shell.c in PR #8; reading the whole directory keeps this checker
    honest through the next extraction as well.
    """
    names = sorted(n for n in os.listdir(GUI_DIR) if n.endswith(".c"))
    if not names:
        raise SystemExit("check_start_menu: no C sources under kernel/gui/")
    return "\n".join(read(os.path.join(GUI_DIR, n)) for n in names)


def app_table(src: str) -> list[dict]:
    """The initialiser body of gui_apps[]."""
    start = src.find("static const gui_app_entry_t gui_apps[] = {")
    if start < 0:
        raise SystemExit(
            "check_start_menu: gui_apps[] not found under kernel/gui/")
    end = src.find("\n};", start)
    if end < 0:
        raise SystemExit("check_start_menu: gui_apps[] has no closing brace")
    body = src[start:end]
    return [m.groupdict() for m in ENTRY_RE.finditer(body)]


def function_body(src: str, name: str) -> str:
    """The body of the *definition* of `name`, or "" when it is not defined.

    The first `name(` in the tree is usually a CALL, not a definition: the
    compositor in gui.c calls shell_draw_taskbar() long before gui_shell.c
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
    brace = match.end() - 1
    depth = 0
    for pos in range(brace, len(src)):
        if src[pos] == "{":
            depth += 1
        elif src[pos] == "}":
            depth -= 1
            if depth == 0:
                return src[brace:pos + 1]
    return ""


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


def function_body_any(src: str, names) -> tuple[str, str]:
    """The first body found for any of `names` (a rename must not blind us)."""
    for name in names:
        body = function_body(src, name)
        if body:
            return name, body
    return "", ""


def check(src: str, built: set[str]) -> list[str]:
    errors: list[str] = []
    entries = app_table(src)

    if not entries:
        errors.append("gui_apps[] parsed to zero entries")

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
    titles = re.search(r"gui_group_titles\[GUI_APP_GRP_COUNT\]\s*=\s*\{(.*?)\};",
                       src, re.S)
    if not titles:
        errors.append("gui_group_titles[] not found")
    else:
        n_titles = len(re.findall(r'"[^"]*"', titles.group(1)))
        if n_titles != len(KERNEL_GROUPS):
            errors.append(
                f"gui_group_titles[] has {n_titles} titles for "
                f"{len(KERNEL_GROUPS)} groups")
    seen_names, seen_paths = set(), set()
    for e in entries:
        if e["group"] not in KERNEL_GROUPS:
            errors.append(f"{e['name']}: unknown group {e['group']}")
        if e["name"] in seen_names:
            errors.append(f"duplicate menu name {e['name']!r}")
        seen_names.add(e["name"])
        if e["path"] in seen_paths:
            errors.append(f"duplicate menu path {e['path']!r}")
        seen_paths.add(e["path"])
    groups_used = {e["group"] for e in entries}
    if "GUI_APP_GRP_APPS" not in groups_used:
        errors.append("the Applications group is empty")

    # 3. the renderer and the hit test share the geometry
    # PR #8 renamed draw_taskbar() to shell_draw_taskbar() when it moved to
    # gui_shell.c; accept either spelling so a rename is not a silent skip.
    draw_name, draw = function_body_any(
        src, ("shell_draw_taskbar", "draw_taskbar"))
    hit = function_body(src, "taskbar_hit")
    for label, body in ((draw_name or "draw_taskbar/shell_draw_taskbar", draw),
                        ("taskbar_hit", hit)):
        if not body:
            errors.append(f"{label}() not found")
            continue
        if "gui_tb_layout(" not in body:
            errors.append(f"{label}() does not call gui_tb_layout()")
        if "gui_tb_button_rect(" not in body:
            errors.append(f"{label}() does not call gui_tb_button_rect()")
        # A real width is a literal of 20 px or more; the only zero assigned
        # to btn_w is the out-parameter initialised by gui_tb_button_rect().
        if re.search(r"btn_w\s*=\s*(?:[2-9]\d|\d{3,})\b", body):
            errors.append(f"{label}() still hardcodes a button width")

    sm_hit = function_body(src, "start_menu_hit")
    if not sm_hit:
        errors.append("start_menu_hit() not found")
    elif "gui_menu_rows(" not in sm_hit:
        errors.append(
            "start_menu_hit() does not use gui_menu_rows(): the hit test and "
            "the drawn rows would drift apart")
    elif "gui_apps[" in sm_hit:
        errors.append("start_menu_hit() indexes gui_apps[] directly again")

    sm_h = re.search(r"static uint32_t start_menu_height\(void\)\s*\{([^}]*)\}",
                     src)
    if not sm_h:
        errors.append("start_menu_height() not found — menu height must be derived")
    elif "gui_menu_rows(" not in sm_h.group(1):
        errors.append("start_menu_height() does not count rows")

    # 4. the shared header must actually contain the layout decisions
    hdr = read(GUI_HDR)
    for token in ("gui_tb_layout(", "gui_tb_button_rect(", "gui_tb_overflow_geometry("):
        if token not in hdr:
            errors.append(f"{os.path.basename(GUI_HDR)} is missing {token}")
    return errors


def run_check() -> int:
    src = gui_source()
    errors = check(src, built_apps())
    if errors:
        print("check_start_menu: FAIL")
        for e in errors:
            print("  -", e)
        return 1
    entries = app_table(src)
    groups = sorted({e["group"] for e in entries})
    print(f"check_start_menu: OK ({len(entries)} entries, "
          f"{len(groups)} groups, shared taskbar geometry)")
    return 0


# ---------------------------------------------------------------- selftest

MUTATIONS = [
    ("stale app path",
     '"/apps/gcalc"', '"/apps/gcalc-does-not-exist"',
     "no binary in the image"),
    ("unknown group",
     "GUI_APP_GRP_SYSTEM }", "GUI_APP_GRP_BOGUS }",
     "unknown group"),
    ("duplicate entry",
     '{ "About",           "/apps/gabout",    6,  GUI_APP_GRP_SYSTEM },',
     '{ "About",           "/apps/gabout",    6,  GUI_APP_GRP_SYSTEM },\n'
     '    { "About",           "/apps/gabout",    6,  GUI_APP_GRP_SYSTEM },',
     "duplicate menu name"),
    ("hit test re-walks the array",
     "    return rows[row].app;",
     "    if (gui_apps[row].name) return row;\n    return rows[row].app;",
     "indexes gui_apps[] directly"),
    ("renderer hardcodes the button width",
     "uint32_t btn_h = tb_h - 8;",
     "uint32_t btn_h = tb_h - 8, btn_w = 130;",
     "still hardcodes a button width"),
    # The regression this checker actually suffered: PR #8 renamed
    # draw_taskbar() to shell_draw_taskbar() and moved it out of gui.c, and a
    # checker that could not find the function reported no error at all.
    ("renderer renamed out from under the checker",
     "void shell_draw_taskbar(void) {",
     "void shell_draw_taskbar_v2(void) {",
     "not found"),
]


def run_selftest() -> int:
    src = gui_source()
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
        errs = check(src.replace(old, new, 1), built)
        joined = " | ".join(errs)
        if expect not in joined:
            print(f"check_start_menu --selftest: FAIL ({name}: expected "
                  f"{expect!r}, got {joined or 'no complaint'})")
            failures += 1
    # Negative control for the negative control: the unmodified tree is clean.
    if check(src, built):
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
