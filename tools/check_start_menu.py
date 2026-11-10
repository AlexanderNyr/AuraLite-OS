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
GUI_C = os.path.join(ROOT, "kernel", "gui", "gui.c")
GUI_HDR = os.path.join(ROOT, "kernel", "gui", "taskbar_layout.h")
MAKEFILE = os.path.join(ROOT, "Makefile")
INITRD_APPS = os.path.join(ROOT, "build", "user", "initrd_root", "apps")

ENTRY_RE = re.compile(
    r'\{\s*"(?P<name>[^"]+)"\s*,\s*"(?P<path>[^"]+)"\s*,\s*'
    r"(?P<icon>\d+)\s*,\s*(?P<group>GUI_APP_GRP_[A-Z]+)\s*\}")

KERNEL_GROUPS = ("GUI_APP_GRP_APPS", "GUI_APP_GRP_OFFICE", "GUI_APP_GRP_SYSTEM")


def read(path: str) -> str:
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def app_table(src: str) -> list[dict]:
    """The initialiser body of gui_apps[]."""
    start = src.find("static const gui_app_entry_t gui_apps[] = {")
    if start < 0:
        raise SystemExit("check_start_menu: gui_apps[] not found in gui.c")
    end = src.find("\n};", start)
    if end < 0:
        raise SystemExit("check_start_menu: gui_apps[] has no closing brace")
    body = src[start:end]
    return [m.groupdict() for m in ENTRY_RE.finditer(body)]


def function_body(src: str, name: str) -> str:
    """Crude but adequate body extractor: from `name(` to the matching brace."""
    idx = src.find(name + "(")
    if idx < 0:
        return ""
    brace = src.find("{", idx)
    if brace < 0:
        return ""
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
    draw = function_body(src, "static void draw_taskbar")
    hit = function_body(src, "static int taskbar_hit")
    for label, body in (("draw_taskbar", draw), ("taskbar_hit", hit)):
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

    sm_hit = function_body(src, "static int start_menu_hit")
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
    src = read(GUI_C)
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
]


def run_selftest() -> int:
    src = read(GUI_C)
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
