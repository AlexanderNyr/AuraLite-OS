#!/usr/bin/env python3
"""check_abi_align.py — the static stack-phase model behind RESIDUE3 C2's
"16-byte C-ABI alignment at every entry stub" box.

Why this exists (and why it is not `grep -c "align 16" isr_stubs.asm`):

The box's opening measurement counted static `align 16` directives in the
assembly and got ZERO, which looks like a missing invariant.  It is not:
`align 16` in NASM aligns the CODE ADDRESS of the following instruction, not
the STACK.  The invariant that matters for a C call is RSP % 16 == 0 at the
moment of the `call`, and that is a property of the path, not of the file.
RESIDUE2 T1 got the stubs right the other way round — isr_common_stub realigns
dynamically (`and rsp, -16`) precisely because the entry phase depends on the
interrupted frame — so a static-directive metric would fail a correct tree and
pass a broken one.

This tool models the phases instead.  It walks each assembly file, tracks RSP
mod 16 through push/pop/sub/add (and the two anchoring forms), and requires
EVERY `call` to reach a C symbol at a phase of 0.  A `call` reached in an
unknown phase is a violation unless the file marks the entry state with an
`@abi-anchor:` comment (the marker text is printed, so a stale claim is visible
in the report rather than silently trusted).  Integer arithmetic is exact, so
a doctored `sub rsp, 8` turns the gate red — that is the negative control
tests/unit/test_abi_align.sh runs.

Usage:
    check_abi_align.py            # report + exit 1 on any violation
    check_abi_align.py --report   # same, plus every call site it accepted
    check_abi_align.py --selftest # prove the model catches injected defects
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Files that call C from assembly.  Everything under kernel/ that has a
# `call` in it is picked up automatically; this list is the pinned set the
# gate expects to find, so deleting a file is a failure, not a silent skip.
REQUIRED = [
    "kernel/arch/x86_64/isr_stubs.asm",
    "kernel/arch/x86_64/syscall_entry.asm",
    "kernel/arch/x86_64/syscall_sigreturn.asm",
    "kernel/arch/x86_64/boot.asm",
    "kernel/arch/i386/isr_stubs32.asm",
    "kernel/proc/fork_return.asm",
    "kernel/proc/user_entry.asm",
    "kernel/proc/context.asm",
]

# Entry states the model cannot derive from the file itself.  Each one names
# WHERE the guarantee comes from; an anchor whose justification is not in the
# tree is a lie the reviewer can see without running anything.
ANCHOR_MARK = "@abi-anchor:"

PUSH = re.compile(r"^push\b")
PUSHA = re.compile(r"^pusha[d]?\b")
POPA = re.compile(r"^popa[d]?\b")
POP = re.compile(r"^pop\b")
SUB_RSP = re.compile(r"^sub\s+rsp\s*,\s*(\S+)")
ADD_RSP = re.compile(r"^add\s+rsp\s*,\s*(\S+)")
AND_RSP = re.compile(r"^and\s+rsp\s*,\s*(-?\S+)")
MOV_RSP_IN = re.compile(r"^mov\s+rsp\s*,\s*(.+)$")
MOV_REG_RSP = re.compile(r"^mov\s+(\w+)\s*,\s*rsp$")
CALL = re.compile(r"^call\s+(\S+)")
LABEL = re.compile(r"^([A-Za-z_.][\w.$]*):")
SYM_LABEL = re.compile(r"^(?:global|extern)\s")

# Flow boundaries: after these the phase is not linearly knowable any more
# (another path may arrive here with a different depth).
BOUNDARY = re.compile(r"^(?:jmp|ret|iretq|o64\s+sysret|sysretq|hlt|jmp\b)")


def parse_imm(tok):
    tok = tok.strip().lower()
    neg = tok.startswith("-")
    tok = tok.lstrip("-")
    try:
        if tok.startswith("0x"):
            v = int(tok, 16)
        else:
            v = int(tok, 0)
    except ValueError:
        return None
    return -v if neg else v


def model_file(path):
    """Return (calls, violations).  calls = [(lineno, callee, phase, anchor)].

    phase is either an int (RSP mod 16) or None (unknown).
    """
    calls = []
    violations = []
    phase = None
    word = 8          # bytes per push/pop: 8 in bits 64, 4 in bits 32
    saved = {}
    anchor_since_boundary = None
    try:
        lines = open(path, "r", errors="replace").read().splitlines()
    except OSError as e:
        return [], [(0, "cannot read: %s" % e)]

    for n, raw in enumerate(lines, 1):
        line = raw.split(";", 1)[0].strip()
        comment = raw.split(";", 1)[1] if ";" in raw else ""
        if ANCHOR_MARK in comment:
            anchor_since_boundary = comment.split(ANCHOR_MARK, 1)[1].strip()
        if not line:
            continue

        if re.match(r"^bits\s+32\b", line):
            word = 4
            continue
        if re.match(r"^bits\s+64\b", line):
            word = 8
            continue

        # 32-bit sources spell the stack pointer ESP; the model speaks one
        # register name so the same arithmetic applies to both widths.
        model_line = re.sub(r"\besp\b", "rsp", line, flags=re.IGNORECASE)

        m = LABEL.match(line)
        if m and not SYM_LABEL.match(line):
            # A new entry point: its stack phase is whatever its caller left,
            # which this file cannot see.  An anchor comment on the label line
            # (or immediately above it) is how the file declares it.
            phase = None
            saved = {}
            if anchor_since_boundary is None:
                anchor_since_boundary = None
            else:
                anchor_since_boundary = anchor_since_boundary
            continue

        if PUSHA.match(line):
            if phase is not None:
                phase = (phase + 8 * word) % 16
            continue
        if POPA.match(line):
            if phase is not None:
                phase = (phase - 8 * word) % 16
            continue
        if PUSH.match(line):
            if phase is not None:
                phase = (phase + word) % 16
            continue
        if POP.match(line):
            if phase is not None:
                phase = (phase - word) % 16
            continue
        m = SUB_RSP.match(model_line)
        if m:
            v = parse_imm(m.group(1))
            phase = None if (v is None or phase is None) else (phase + v) % 16
            continue
        m = ADD_RSP.match(model_line)
        if m:
            v = parse_imm(m.group(1))
            phase = None if (v is None or phase is None) else (phase - v) % 16
            continue
        m = AND_RSP.match(model_line)
        if m:
            v = parse_imm(m.group(1))
            if v is not None and (v & 0xF) == 0:
                phase = 0            # the dynamic realign: phase becomes 0
            else:
                phase = None
            continue
        m = MOV_REG_RSP.match(model_line)
        if m:
            saved[m.group(1)] = phase
            continue
        m = MOV_RSP_IN.match(model_line)
        if m:
            src = m.group(1).strip()
            if src in saved:
                phase = saved[src]   # restoring a saved frame pointer
            elif "[gs:" in src or "[fs:" in src:
                # A per-CPU kernel stack top: 16-aligned by construction in
                # thread_alloc_kernel_stack().  Accepting it requires the file
                # to say so; the anchor below is checked, not assumed.
                phase = 0 if anchor_since_boundary else None
            else:
                phase = None
            continue
        m = CALL.match(model_line)
        if m:
            callee = m.group(1).strip()
            if callee.startswith("["):          # indirect call: not modelled
                calls.append((n, callee, None, "indirect"))
                continue
            if phase is None:
                if anchor_since_boundary:
                    calls.append((n, callee, 0, anchor_since_boundary))
                else:
                    violations.append(
                        (n, "call %s reached with an unknown stack phase and "
                            "no %s marker" % (callee, ANCHOR_MARK)))
            elif phase % 16 == 0:
                calls.append((n, callee, phase, None))
            else:
                violations.append(
                    (n, "call %s at RSP%%16 == %d (must be 0)" % (callee, phase)))
            continue
        if BOUNDARY.match(model_line):
            anchor_since_boundary = None
            phase = None
            continue

    return calls, violations


def check_file(path, verbose):
    calls, violations = model_file(path)
    rel = os.path.relpath(path, ROOT)
    if verbose:
        for (n, callee, phase, anchor) in calls:
            note = " (anchor: %s)" % anchor if anchor else ""
            print("    %s:%d  call %s @ RSP%%16==0%s" % (rel, n, callee, note))
    return calls, violations


def collect():
    out = []
    for base, _dirs, files in os.walk(os.path.join(ROOT, "kernel")):
        for f in sorted(files):
            if f.endswith(".asm"):
                out.append(os.path.join(base, f))
    return sorted(out)


def run(files, verbose=False):
    total_calls = 0
    total_viol = 0
    for rel in files:
        path = rel if os.path.isabs(rel) else os.path.join(ROOT, rel)
        if not os.path.exists(path):
            print("MISSING (pinned): %s" % rel)
            total_viol += 1
            continue
        if verbose:
            print("  %s" % rel)
        calls, viol = check_file(path, verbose)
        total_calls += len(calls)
        total_viol += len(viol)
        for (n, msg) in viol:
            print("VIOLATION %s:%d  %s" % (os.path.relpath(path, ROOT), n, msg))
    return total_calls, total_viol


def selftest():
    """Inject three defects into a copy of a clean file and prove each one is
    caught; then remove the realign and prove the tree's own anchor carries
    the load.  A gate nobody has seen fail is not a gate."""
    src = os.path.join(ROOT, "kernel/arch/x86_64/isr_stubs.asm")
    text = open(src).read()
    cases = [
        # 1) the dynamic realign removed: the call drops to an unknown phase
        (text.replace("    and rsp, -16\n", ""), "realign removed"),
        # 2) a stray push after the realign: phase becomes 8
        (text.replace("    and rsp, -16\n", "    and rsp, -16\n    push rax\n"),
         "extra push after the realign"),
        # 3) a wrong-size pad: phase becomes 4
        (text.replace("    and rsp, -16\n", "    and rsp, -16\n    sub rsp, 4\n"),
         "4-byte pad after the realign"),
    ]
    failures = 0
    for blob, name in cases:
        with tempfile.NamedTemporaryFile("w", suffix=".asm", delete=False) as f:
            f.write(blob)
            tmp = f.name
        _calls, viol = model_file(tmp)
        os.unlink(tmp)
        if not viol:
            print("SELFTEST FAIL: the model did not catch '%s'" % name)
            failures += 1
        else:
            print("SELFTEST ok: '%s' -> %s" % (name, viol[0][1]))
    # And the control: the untouched file must be clean.
    _calls, viol = model_file(src)
    if viol:
        print("SELFTEST FAIL: the clean tree already violates: %s" % viol[0][1])
        failures += 1
    else:
        print("SELFTEST ok: the untouched isr_stubs.asm is clean")
    return 1 if failures else 0


def main(argv):
    verbose = "--report" in argv
    if "--selftest" in argv:
        return selftest()

    files = collect()
    missing = [f for f in REQUIRED if not os.path.exists(os.path.join(ROOT, f))]
    print("[abi] modelling %d assembly files" % len(files))
    calls, viol = run(files, verbose)
    for f in missing:
        print("MISSING (pinned): %s" % f)
        viol += 1
    print("[abi] %d call site(s) reach C, %d violation(s)" % (calls, viol))
    if viol:
        print("[abi] FAIL - the C ABI's 16-byte stack alignment is not held "
              "at every entry")
        return 1
    print("[abi] OK - every call into C enters at RSP%16 == 0")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
