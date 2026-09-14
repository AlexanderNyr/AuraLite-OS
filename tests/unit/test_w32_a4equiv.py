#!/usr/bin/env python3
# test_w32_a4equiv.py — W32A-4 unwind-output equivalence vs llvm-readobj.
#
# The plan's gate: OUR parser (w32/tools/unwinddump, which walks .pdata
# through w32_seh_lookup + w32_seh_validate_chain) must agree with an
# independent reader on every UNWIND_INFO of the repo's own binaries.
# This script re-encodes llvm-readobj's DECODED text back to raw bytes
# (the scaling rules below were verified against hexdumps, not memory)
# and byte-compares against the dump.
#
# Skips cleanly (exit 0) when unwinddump, a fixture, or llvm-readobj is
# unavailable — the test_w32_peinfo.sh convention.  Fails LOUDLY (exit 1,
# first mismatch with both sides) on any divergence, including on
# constructs this decoder does not know (chained info, v2 epilogues):
# an unreadable table must extend the decoder, never pass silently.
#
# Fixtures: build/user/w32a4_{try,unwind,raise,continue,cxthrow,filter,
# crash,term,purecall,dialog,cxx}.exe — hand-written tables plus real
# compiler output (the cxx binary's 189 libgcc functions).

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
DUMP = os.path.join(ROOT, "build", "w32_unwinddump")
USER = os.path.join(ROOT, "build", "user")

FIXTURES = ["try", "unwind", "raise", "continue", "cxthrow", "filter",
            "crash", "term", "purecall", "dialog", "cxx"]

READOBJ_CANDIDATES = ["llvm-readobj", "llvm-readobj-19", "llvm-readobj-18",
                      "llvm-readobj-17"]

REGS = {"RAX": 0, "RCX": 1, "RDX": 2, "RBX": 3, "RSP": 4, "RBP": 5,
        "RSI": 6, "RDI": 7, "R8": 8, "R9": 9, "R10": 10, "R11": 11,
        "R12": 12, "R13": 13, "R14": 14, "R15": 15}
XMM = {"XMM%d" % i: i for i in range(16)}

# UWOP_* (low nibble), mirroring w32/w32_seh.h.
OP = {"PUSH_NONVOL": 0, "ALLOC_LARGE": 1, "ALLOC_SMALL": 2, "SET_FPREG": 3,
      "SAVE_NONVOL": 4, "SAVE_NONVOL_FAR": 5, "SAVE_XMM128": 8,
      "SAVE_XMM128_FAR": 9, "PUSH_MACHFRAME": 10}


def skip(msg):
    print("[w32] SKIP: %s" % msg)
    return 0


def fail(msg):
    print("  FAIL %s" % msg)
    return 1


def parse_dump(text):
    """unwinddump lines -> list of dicts (raw ints + code bytes)."""
    funcs = []
    for line in text.splitlines():
        if not line.startswith("func "):
            continue
        parts = line.split()
        d = {"begin": int(parts[1], 16), "end": int(parts[2], 16)}
        for p in parts[3:]:
            k, v = p.split("=", 1)
            d[k] = v
        d["ver"] = int(d["ver"])
        d["flags"] = int(d["flags"], 16)
        d["prolog"] = int(d["prolog"])
        d["freg"] = int(d["freg"])
        d["fpoff"] = int(d["fpoff"])
        d["codes"] = b"" if d["codes"] == "-" else bytes.fromhex(d["codes"])
        d["handler"] = None if d["handler"] == "-" else int(d["handler"], 16)
        d["xdata"] = int(d["xdata"], 16)
        if "chained" in d:
            d["chained"] = tuple(int(x, 16) for x in d["chained"].split(","))
        funcs.append(d)
    return funcs


def encode_code(opname, operands, offset):
    """One readobj code line -> raw bytes (verifies the scaling)."""
    op = OP[opname]
    info = 0
    extra = b""
    if opname == "PUSH_NONVOL":
        info = REGS[operands["reg"]]
    elif opname == "ALLOC_SMALL":
        size = operands["size"]
        assert (size - 8) % 8 == 0, "ALLOC_SMALL size %d" % size
        info = (size - 8) // 8
    elif opname == "ALLOC_LARGE":
        size = operands["size"]
        if size <= 0xFFFF * 8:
            info = 0
            assert size % 8 == 0, "ALLOC_LARGE size %d" % size
            extra = (size // 8).to_bytes(2, "little")
        else:
            info = 1
            extra = size.to_bytes(4, "little")
    elif opname == "SET_FPREG":
        off = operands["offset"]
        assert off % 16 == 0, "SET_FPREG offset %#x" % off
        info = 0  # Info unused; the offset lives in the header nibble.
    elif opname == "SAVE_NONVOL":
        info = REGS[operands["reg"]]
        off = operands["offset"]
        assert off % 8 == 0, "SAVE_NONVOL offset %#x" % off
        extra = (off // 8).to_bytes(2, "little")
    elif opname == "SAVE_NONVOL_FAR":
        info = REGS[operands["reg"]]
        extra = operands["offset"].to_bytes(4, "little")
    elif opname == "SAVE_XMM128":
        info = XMM[operands["reg"]]
        off = operands["offset"]
        assert off % 16 == 0, "SAVE_XMM128 offset %#x" % off
        extra = (off // 16).to_bytes(2, "little")
    elif opname == "SAVE_XMM128_FAR":
        info = XMM[operands["reg"]]
        extra = operands["offset"].to_bytes(4, "little")
    elif opname == "PUSH_MACHFRAME":
        info = operands.get("info", 0)
    else:
        raise AssertionError("unknown op %s" % opname)
    return bytes([offset, (info << 4) | op]) + extra


def parse_readobj(text, base):
    """llvm-readobj --unwind -> list of dicts matching parse_dump's shape."""
    funcs = []
    cur = None
    in_codes = False
    for line in text.splitlines():
        s = line.strip()
        if s == "RuntimeFunction {":
            cur = {"codes_list": []}
        elif s == "}" and cur is not None and not in_codes and "end" in cur \
                and "ver" in cur:
            # End of UnwindInfo: materialise.
            codes = b"".join(cur["codes_list"])
            cur["codes"] = codes
            del cur["codes_list"]
            funcs.append(cur)
            cur = None
        elif cur is None:
            continue
        elif s == "UnwindCodes [":
            in_codes = True
        elif s == "]":
            in_codes = False
        elif in_codes:
            m = re.match(r"0x([0-9a-fA-F]+): (\w+)(.*)", s)
            assert m, "unparsed code line: %r" % s
            offset = int(m.group(1), 16)
            opname = m.group(2)
            rest = m.group(3)
            if opname not in OP:
                raise AssertionError("new opcode %r (extend the decoder)"
                                     % opname)
            ops = {}
            m2 = re.search(r"reg=(\w+)", rest)
            if m2:
                ops["reg"] = m2.group(1)
            m2 = re.search(r"size=(\d+)", rest)
            if m2:
                ops["size"] = int(m2.group(1))
            m2 = re.search(r"offset=(0x[0-9a-fA-F]+)", rest)
            if m2:
                ops["offset"] = int(m2.group(1), 16)
            cur["codes_list"].append(encode_code(opname, ops, offset))
        else:
            m = re.match(r"StartAddress: .*\(0x([0-9a-fA-F]+)\)", s)
            if m:
                cur["begin"] = int(m.group(1), 16) - base
                continue
            m = re.match(r"EndAddress: .*\(0x([0-9a-fA-F]+)\)", s)
            if m:
                cur["end"] = int(m.group(1), 16) - base
                continue
            m = re.match(r"UnwindInfoAddress: .*\(0x([0-9a-fA-F]+)\)", s)
            if m:
                cur["xdata"] = int(m.group(1), 16) - base
                continue
            m = re.match(r"Version: (\d+)", s)
            if m:
                cur["ver"] = int(m.group(1))
                if cur["ver"] != 1:
                    raise AssertionError("Version %d (extend the decoder)"
                                         % cur["ver"])
                continue
            m = re.match(r"Flags \[ \(0x([0-9a-fA-F]+)\)", s)
            if m:
                cur["flags"] = int(m.group(1), 16)
                if cur["flags"] & 0x04:
                    raise AssertionError("chained info (extend the decoder)")
                continue
            m = re.match(r"PrologSize: (\d+)", s)
            if m:
                cur["prolog"] = int(m.group(1))
                continue
            m = re.match(r"FrameRegister: (.*)", s)
            if m:
                r = m.group(1)
                cur["freg"] = 0 if r == "-" else int(
                    re.search(r"0x([0-9a-fA-F]+)", r).group(1), 16)
                continue
            m = re.match(r"FrameOffset: (.*)", s)
            if m:
                o = m.group(1)
                # Raw nibble (verified: 0x8 with SET_FPREG deriving 0x80).
                cur["fpoff"] = 0 if o == "-" else int(o, 16)
                continue
            m = re.match(r"Handler: .*\(0x([0-9a-fA-F]+)\)", s)
            if m:
                cur["handler"] = int(m.group(1), 16) - base
                continue
    for c in funcs:
        c.setdefault("handler", None)
    return funcs


def check_one(readobj, path):
    name = os.path.basename(path)
    try:
        ours = subprocess.run([DUMP, path], capture_output=True, text=True,
                              timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        return fail("%s: unwinddump failed: %s" % (name, e))
    if ours.returncode != 0:
        return fail("%s: unwinddump rc=%d: %s" % (name, ours.returncode,
                                                  ours.stderr.strip()))
    try:
        fh = subprocess.run([readobj, "--file-headers", path],
                            capture_output=True, text=True, timeout=60)
        ru = subprocess.run([readobj, "--unwind", path], capture_output=True,
                            text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as e:
        return fail("%s: readobj failed: %s" % (name, e))
    m = re.search(r"ImageBase:\s*(0x[0-9a-fA-F]+)", fh.stdout)
    if not m:
        return fail("%s: no ImageBase in --file-headers" % name)
    base = int(m.group(1), 16)

    a = parse_dump(ours.stdout)
    try:
        b = parse_readobj(ru.stdout, base)
    except AssertionError as e:
        return fail("%s: readobj-side: %s" % (name, e))

    if len(a) != len(b):
        return fail("%s: func count ours=%d readobj=%d" % (name, len(a),
                                                           len(b)))
    # Sortedness: our binary search requires it; assert it here too.
    for i in range(1, len(a)):
        if a[i]["begin"] <= a[i - 1]["begin"]:
            return fail("%s: .pdata not sorted at func %d" % (name, i))
    fails = 0
    for i, (x, y) in enumerate(zip(a, b)):
        for k in ("begin", "end", "xdata", "ver", "flags", "prolog",
                  "freg", "fpoff", "handler"):
            if x[k] != y[k]:
                print("  FAIL %s func %d field %s: ours=%r readobj=%r" %
                      (name, i, k, x[k], y[k]))
                fails += 1
                break
        else:
            if x["codes"] != y["codes"]:
                print("  FAIL %s func %d codes: ours=%s readobj=%s" %
                      (name, i, x["codes"].hex(), y["codes"].hex()))
                fails += 1
        if fails >= 5:
            print("  (... stopping after 5 mismatches in %s)" % name)
            break
    if fails:
        return 1
    print("  ok   %s: %d funcs agree" % (name, len(a)))
    return 0


def main():
    if not os.path.isfile(DUMP) or not os.access(DUMP, os.X_OK):
        return skip("%s not built" % DUMP)
    readobj = None
    for c in READOBJ_CANDIDATES:
        for d in os.environ.get("PATH", "").split(os.pathsep):
            p = os.path.join(d, c)
            if os.path.isfile(p) and os.access(p, os.X_OK):
                readobj = p
                break
        if readobj:
            break
    if not readobj:
        return skip("no llvm-readobj available for cross-check")

    fails = 0
    covered = 0
    for fx in FIXTURES:
        path = os.path.join(USER, "w32a4_%s.exe" % fx)
        if not os.path.isfile(path) or os.path.getsize(path) == 0:
            print("  skip %s (not built)" % os.path.basename(path))
            continue
        covered += 1
        fails += check_one(readobj, path)
    if covered == 0:
        return skip("no w32a4 fixtures built")
    if fails:
        print("[w32] test_w32_a4equiv: FAIL")
        return 1
    print("[w32] test_w32_a4equiv: PASS (%d binaries vs %s)" %
          (covered, os.path.basename(readobj)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
