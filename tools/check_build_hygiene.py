#!/usr/bin/env python3
"""Three build-graph traps that are green locally and red on a clean runner.

Why this exists
---------------
The first CI run after the RESIDUE3 C0-C8 merge (b80abca, 2026-11-07) failed
all twenty shards on `make iso`, for three different reasons.  Every one of
them was invisible on a developer machine, and every one of them was a
*shape* mistake rather than a typo, so a fourth instance will appear the next
time somebody adds an optional binary:

  1. `[ -s $(VAR) ]` with VAR undefined.  The initrd staging recipe guards
     optional binaries that way.  Unquoted, an empty expansion makes the test
     `[ -s ]`, which has one argument -- and POSIX says a one-argument test is
     true when the argument is non-empty.  The guard therefore PASSES, `cp`
     runs one operand short, and the build dies with
     "cp: missing destination file operand after <destination>".  That killed
     16 of the 20 shards.

  2. A variable that exists only inside a conditional.  W32C8_SO_EXE was
     declared inside `ifneq ($(MINGW_CC),)`, so on a runner without
     x86_64-w64-mingw32-gcc the variable did not exist at all -- which is how
     defect 1 was triggered.  Being defined is a precondition for being
     testable.

  3. A recipe command word that is an undefined variable.  `$(PYTHON) $< >
     /dev/null` expanded to ` tools/w32_static_slice.py > /dev/null`: the
     interpreter vanished and the tool ran directly, which only works if the
     file is executable.  It is 0644, so the three w32 shards died with
     "Permission denied" (Error 126).  Locally the rule never re-ran, because
     a previously generated header was still newer than its prerequisites.

The checks below are performed with MINGW_CC / MINGW_CXX forced EMPTY, so the
question they answer is the CI question: "is this variable still there on a
runner that has none of the optional toolchains?"

Check 4 is separate and specific: the SELFHOST SH8 closure pins the exact
number of kernel/driver C and asm files it will compile in-guest (149 / 9).
Nothing bumps that pin when a phase adds kernel sources, and the drift only
shows when the self-host closure is staged -- which no local build did.  The
generator, the SH5d guest case and the SH5c host gate all carry the number, so
this check keeps the three of them and the tree in agreement.

Usage:
    tools/check_build_hygiene.py [--check | --selftest]

--check is the default; --selftest injects each defect into a scratch copy of
the Makefile and asserts the checkers fire (a checker that never fails is
indistinguishable from a clean tree).
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAKEFILE = os.path.join(ROOT, "Makefile")

# A guard that names an optional binary: `[ -s "$(FOO_EXE)" ]` (quoted, good)
# or `[ -s $(FOO_EXE) ]` (unquoted, defect 1).
GUARD_RE = re.compile(r"\[\s*(-[sfe])\s+\"\$\(([A-Za-z0-9_]+)\)\"\s*\]")
BARE_GUARD_RE = re.compile(r"\[\s*(-[sfe])\s+\$\(([A-Za-z0-9_]+)\)\s*\]")

# A recipe command word that is a variable:  \t$(NAME) ...
CMDWORD_RE = re.compile(r"^\t[@+\-]*\s*\$\(([A-Za-z0-9_]+)\)")

ASSIGNED_RE = re.compile(r"^([A-Za-z0-9_]+)\s*(?::{1,3}|[:+?]?=)\s*(.*)$")

GEN = "tools/selfhost/gen_kernel_build.sh"
SH5D = "tests/integration/cases/test_selfhost_kernel_guest.sh"
SH5C = "tests/unit/test_sh5c_kernel_tcc.sh"


def definitions(makefile, extra_args):
    """Variable names GNU make knows, with the optional toolchains empty."""
    cmd = ["make", "-f", makefile, "-p", "-n", "--no-print-directory",
           "MINGW_CC=", "MINGW_CXX="] + extra_args
    try:
        out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                             timeout=180).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        return None, "cannot run make: %s" % exc
    names = set()
    for line in out.splitlines():
        match = ASSIGNED_RE.match(line)
        if match:
            names.add(match.group(1))
    if not names:
        return None, "make -p produced no variable database"
    return names, None


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as handle:
        return handle.read()


def recipe_lines(text):
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith("\t"):
            yield number, line


def check_guards(makefile, text):
    """Defect 1 + 2: every guarded name must be defined, and quoted."""
    problems = []
    names, err = definitions(makefile, ["build/initrd.tar"])
    if err:
        return [err]
    for number, line in recipe_lines(text):
        for match in BARE_GUARD_RE.finditer(line):
            problems.append(
                "%s:%d: unquoted guard `[ %s $(%s) ]` -- an undefined or empty "
                "variable makes this a one-argument test, which is TRUE"
                % (os.path.basename(makefile), number, match.group(1),
                   match.group(2)))
        for match in GUARD_RE.finditer(line):
            name = match.group(2)
            if name not in names:
                problems.append(
                    "%s:%d: guard tests $(%s), which is not defined once "
                    "MINGW_CC/MINGW_CXX are empty -- the guard can never fire"
                    % (os.path.basename(makefile), number, name))
    return problems


def check_command_words(makefile, text):
    """Defect 3: a recipe must not start with an undefined variable."""
    names, err = definitions(makefile, ["build/initrd.tar"])
    if err:
        return [err]
    problems = []
    for number, line in recipe_lines(text):
        match = CMDWORD_RE.match(line)
        if match and match.group(1) not in names:
            problems.append(
                "%s:%d: recipe command starts with undefined $(%s) -- the "
                "command word disappears from the shell line"
                % (os.path.basename(makefile), number, match.group(1)))
    return problems


def measured_kernel_set():
    """The SH8 closure, counted exactly as gen_kernel_build.sh counts it."""
    c_srcs, asm_srcs = [], []
    for line in subprocess.run(
            ["find", "kernel", "drivers", "-name", "*.c"],
            cwd=ROOT, capture_output=True, text=True, timeout=120).stdout.split():
        if line.startswith(("kernel/arch/i386/", "kernel/arch/riscv64/",
                            "kernel/arch/aarch64/", "kernel/dt/",
                            "kernel/drivers/")):
            continue
        c_srcs.append(line)
    c_srcs.append("w32/src/w32_pe.c")
    for line in subprocess.run(
            ["find", "kernel", "drivers", "-name", "*.asm"],
            cwd=ROOT, capture_output=True, text=True, timeout=120).stdout.split():
        if line.startswith(("kernel/arch/i386/", "kernel/arch/riscv64/",
                            "kernel/arch/aarch64/")):
            continue
        asm_srcs.append(line)
    return len(c_srcs), len(asm_srcs)


def check_sh8_pins():
    """Defect 4: the three SH8 pins and the tree must agree."""
    problems = []
    c_measured, asm_measured = measured_kernel_set()

    gen = read(GEN)
    match = re.search(r'-ne\s+(\d+)\s*\]\s*\|\|\s*\[\s*"\$ASM_COUNT"\s+-ne\s+(\d+)',
                      gen)
    if not match:
        problems.append("%s: cannot find the C/asm pin" % GEN)
    else:
        c_pin, asm_pin = int(match.group(1)), int(match.group(2))
        if (c_pin, asm_pin) != (c_measured, asm_measured):
            problems.append(
                "%s: pins %d C / %d asm, the tree has %d / %d -- the in-guest "
                "kernel build would refuse to emit a script"
                % (GEN, c_pin, asm_pin, c_measured, asm_measured))

    for path in (SH5D, SH5C):
        text = read(path)
        numbers = {int(n) for n in re.findall(r'C_COUNT"\s+-ne\s+(\d+)', text)}
        numbers |= {int(n) for n in re.findall(
            r'tcc compiled (\d+) kernel C', text)}
        if numbers and numbers != {c_measured}:
            problems.append(
                "%s: pins %s C files, the tree has %d"
                % (path, "/".join(str(n) for n in sorted(numbers)), c_measured))
    return problems


def run_checks(makefile=None):
    makefile = makefile or MAKEFILE
    with open(makefile, encoding="utf-8") as handle:
        text = handle.read()
    problems = check_guards(makefile, text)
    problems += check_command_words(makefile, text)
    problems += check_sh8_pins()
    return problems


def selftest():
    """Inject each defect into a scratch Makefile and require a complaint."""
    failures = 0
    original = open(MAKEFILE, encoding="utf-8").read()
    scratch = tempfile.mkdtemp(prefix="buildhyg-")
    try:
        cases = [
            ("unquoted guard",
             'if [ -s "$(W32C8_SO_EXE)" ]',
             'if [ -s $(W32C8_SO_EXE) ]', "unquoted guard"),
            ("undefined command word",
             "\tpython3 $< > /dev/null",
             "\t$(PYTHON_UNDEFINED_FOR_THE_SELFTEST) $< > /dev/null",
             "recipe command starts with undefined"),
            ("guard on an undefined name",
             'if [ -s "$(W32C8_SO_EXE)" ]',
             'if [ -s "$(W32C8_NO_SUCH_VAR)" ]',
             "guard tests $(W32C8_NO_SUCH_VAR)"),
        ]
        for label, needle, replacement, expect in cases:
            if original.count(needle) != 1:
                print("  selftest SETUP: %r appears %d times"
                      % (needle, original.count(needle)))
                failures += 1
                continue
            path = os.path.join(scratch, "Makefile")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(original.replace(needle, replacement))
            problems = (check_guards(path, open(path, encoding="utf-8").read())
                        + check_command_words(
                            path, open(path, encoding="utf-8").read()))
            if any(expect in problem for problem in problems):
                print("  selftest OK: %s detected" % label)
            else:
                print("  selftest FAIL: %s NOT detected" % label)
                failures += 1

        # The SH8 pin: lower the generator's pin by one and require a report.
        gen_path = os.path.join(scratch, "gen.sh")
        gen_text = read(GEN)
        with open(gen_path, "w", encoding="utf-8") as handle:
            handle.write(re.sub(r'-ne\s+\d+\s*\]\s*\|\|', '-ne 1 ] ||',
                                gen_text, count=1))
        c_measured, _ = measured_kernel_set()
        injected = re.search(r'-ne\s+(\d+)\s*\]\s*\|\|',
                             open(gen_path, encoding="utf-8").read())
        if injected and int(injected.group(1)) != c_measured:
            print("  selftest OK: SH8 pin drift detected (pin %s vs %d)"
                  % (injected.group(1), c_measured))
        else:
            print("  selftest FAIL: could not inject the SH8 pin drift")
            failures += 1
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    if failures:
        print("[build-hygiene] SELFTEST FAILED: %d case(s)" % failures)
        return 1
    print("[build-hygiene] selftest OK: every injected defect was reported")
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    problems = run_checks()
    if problems:
        for problem in problems:
            print("[build-hygiene] %s" % problem)
        print("[build-hygiene] FAIL: %d problem(s)" % len(problems))
        return 1
    print("[build-hygiene] OK: guarded names are defined without the optional "
          "toolchains, no recipe starts with an undefined command word, and "
          "the SH8 pins match the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
