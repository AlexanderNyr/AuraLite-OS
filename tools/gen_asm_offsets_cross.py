#!/usr/bin/env python3
"""Extract NASM offsets from kernel-target assembly; fail closed on drift."""
import pathlib
import re
import sys

names = (
    "TCB_RSP TCB_TLS_BASE TCB_USER_GS TCB_PML4 TCB_SIZE TCB_SWITCH_PARKED "
    "TCB_FPU TCB_FPU_VALID CL_SYS_RIP CL_SYS_RFLAGS CL_SYS_RSP CL_SYS_KRSP "
    "CL_SYS_RBX CL_SYS_RBP CL_SYS_R12 CL_SYS_R13 CL_SYS_R14 CL_SYS_R15"
).split()
if len(sys.argv) != 3:
    sys.exit("usage: gen_asm_offsets_cross.py input.s output.inc")
text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
entries = re.findall(r"^\s*#\s*AURALITE_OFFSET\s+([A-Z0-9_]+)\s+([0-9]+)\s*$", text, re.MULTILINE)
if [name for name, _ in entries] != names:
    sys.exit("gen_asm_offsets_cross: missing, duplicate or unexpected offset markers")
lines = ["; AUTO-GENERATED from kernel-target ABI — do not edit.\n"]
lines += [f"%define {name} {value}\n" for name, value in entries]
pathlib.Path(sys.argv[2]).write_text("".join(lines), encoding="utf-8")
