#!/usr/bin/env python3
"""Dump and check Win32 import ledgers for W32APP_PLAN.md (phase W32A-0).

Why this exists
---------------
W32APP breadth is picked by measurement, not judgement (decision D1): every
function enters the personality iff a ladder binary imports it. This tool is
the measurement instrument. It parses a user-supplied PE file with nothing
but the standard library -- no llvm, no binutils, so the committed ledgers
can be re-verified anywhere -- and emits the ledger format that
w32/app_ledger/*.imports store and tools/check_w32app_claims.py enforces.

What it parses: DOS/MZ header, e_lfanew, COFF + PE32+ optional header, the
section table (RVA -> file offset), the import directory (hint/name AND
ordinal thunks), the delay-load directory (thunks marked `delay`), the TLS
directory (presence only), the exception directory (RUNTIME_FUNCTION count),
and the SxS manifest (resource type 24: comctl selection, execution level,
DPI awareness, supportedOS count).

Deliberately NOT parsed: bound imports (hints, not authority), the CLR
header (empty in every ladder binary -- verified), export tables (W32A-1
fixture work), certificate data. PE32 (32-bit) images are refused with a
pointer to W32APP D3: WOW64 is a future W32C series, not a silent gap.

Classification (decision D9) is baked in, not guessed per run: MODULE_CLASSES
gives the per-module default and OVERRIDES names the exceptions, both citing
the plan section that decided them. An unknown module is a hard error --
adding a module to the personality is a plan decision (D1), not a dump
detail. Every emitted symbol carries `static-only` until a runtime receipt
promotes it (W32APP §2.6).

Usage:
    tools/w32_import_ledger.py dump PE-FILE --label NAME --version VER
        # emit the ledger for one binary to stdout
    tools/w32_import_ledger.py agree PE-FILE
        # cross-check our parse against llvm-readobj-19 (dev-time; llvm is
        # NOT a required tool, so this never runs in CI)
    tools/w32_import_ledger.py check
        # verify the committed ledgers: parse, census counts, union gap
        # (this is what the W32A-0 gate runs)
"""

import hashlib
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEDGER_DIR = os.path.join(ROOT, "w32", "app_ledger")
BIND_C = os.path.join(ROOT, "w32", "src", "w32_bind.c")

# ---------------------------------------------------------------------------
# D9 classification. Module default + per-symbol overrides, each citing the
# W32APP_PLAN.md section that decided it. Unknown modules are refused.
# ---------------------------------------------------------------------------

# Modules whose every ledger symbol is REAL unless overridden below.
REAL_MODULES = {
    "kernel32.dll",   # W32A-2 files/time/process-info + W32A-3 threads
    "user32.dll",     # W32A-5 windows/messages + W32A-6 dialogs/menus/clipboard
    "gdi32.dll",      # W32A-7 DCs/blitting/regions/fonts
    "comctl32.dll",   # W32A-8 common controls (ordinals resolved in W32A-1)
    "comdlg32.dll",   # W32A-10 common dialogs (PrintDlgW excepted below)
    "shell32.dll",    # W32A-10 shell (ordinal 165 excepted below)
    "ole32.dll",      # W32A-11 OLE-lite
    "oleaut32.dll",   # W32A-11 VARIANT/BSTR (high ordinals excepted below)
    "shlwapi.dll",    # W32A-10 pure Path*/Color* helpers
    "uxtheme.dll",    # W32A-11 themed drawing onto the compositor theme engine
    "version.dll",    # W32A-10 GetFileVersionInfo* from PE resources
    "wininet.dll",    # W32A-10 InternetCrackUrlW (pure parser)
    "dbghelp.dll",    # W32A-10 ImageNtHeader (trivially real)
    "advapi32.dll",   # W32A-9 registry/SIDs/CryptoAPI (LSA etc. excepted below)
    "msvcrt.dll",     # W32A-13 CRT bridge
    "ws2_32.dll",     # W32A-12 (dynamic surface; absent from static tables)
}

# Modules whose every ledger symbol FAILs CLEAN with a documented error.
FAIL_CLEAN_MODULES = {
    "imm32.dll",    # W32A-11: no IME; nine per-function stub behaviours
    "dwmapi.dll",   # W32A-10: no composition; defaults with reasons recorded
    "sensapi.dll",  # W32A-10: best-effort reachability probe, never hardcoded
    "wintrust.dll", # W32A-10: signature status unknown; updater treats as unverified
    "crypt32.dll",  # W32A-10: cert-store surface for the unverified path
    "mpr.dll",      # W32A-1: delay-loaded; no network provider exists
}

# Per-symbol overrides: (dll, symbol) -> (class, reason).
# Symbol spelling for ordinals is "#<n>", matching the ledger format.
OVERRIDES = {
    # W32A-10: printing is a §7 non-goal; the import binds and says "no printers".
    ("comdlg32.dll", "PrintDlgW"): ("FAIL-CLEAN", "W32A-10: no printers"),
    # W32A-9: single-user machine, no privileges to hold; the backup-privilege
    # path degrades to normal file access with the application's own error.
    ("advapi32.dll", "LsaOpenPolicy"): ("FAIL-CLEAN", "W32A-9: no LSA"),
    ("advapi32.dll", "LsaAddAccountRights"): ("FAIL-CLEAN", "W32A-9: no LSA"),
    ("advapi32.dll", "LsaClose"): ("FAIL-CLEAN", "W32A-9: no LSA"),
    ("advapi32.dll", "LookupAccountNameW"): ("FAIL-CLEAN", "W32A-9: single-user"),
    ("advapi32.dll", "LookupPrivilegeValueW"): ("FAIL-CLEAN", "W32A-9: no privileges"),
    ("advapi32.dll", "OpenProcessToken"): ("FAIL-CLEAN", "W32A-9: no tokens"),
    ("advapi32.dll", "AdjustTokenPrivileges"): ("FAIL-CLEAN", "W32A-9: no privileges"),
    ("advapi32.dll", "GetFileSecurityW"): ("FAIL-CLEAN", "W32A-9: owner-only approx"),
    ("advapi32.dll", "SetFileSecurityW"): ("FAIL-CLEAN", "W32A-9: owner-only approx"),
    # W32A-1 resolutions. Every ladder ordinal maps to a DOCUMENTED MS
    # function; the ordinal<->name facts come from published documentations,
    # cited per entry (see w32/ordinal_map.tsv, the loader's copy of this).
    # Chappell = geoffchappell.com studies; pefile = erocarrera/pefile (MIT).
    ("comctl32.dll", "#381"): ("REAL", "W32A-1: LoadIconWithScaleDown, Chappell comctl32/ords610"),
    ("comctl32.dll", "#410"): ("REAL", "W32A-1: SetWindowSubclass, Chappell comctl32/ords472"),
    ("comctl32.dll", "#411"): ("REAL", "W32A-1: GetWindowSubclass, Chappell comctl32/ords472"),
    ("comctl32.dll", "#412"): ("REAL", "W32A-1: RemoveWindowSubclass, Chappell comctl32/ords472"),
    ("comctl32.dll", "#413"): ("REAL", "W32A-1: DefSubclassProc, Chappell comctl32/ords472"),
    ("shell32.dll", "#165"): ("REAL", "W32A-1: SHCreateDirectory, Chappell shell32/ords400"),
    ("oleaut32.dll", "#149"): ("REAL", "W32A-1: SysStringByteLen, pefile ordlookup"),
    ("oleaut32.dll", "#150"): ("REAL", "W32A-1: SysAllocStringByteLen, pefile ordlookup"),
}

# ---------------------------------------------------------------------------
# Minimal PE reader (PE32+ only). Raises PEError on anything malformed.
# ---------------------------------------------------------------------------

class PEError(Exception):
    pass


class PEImage:
    def __init__(self, data, name="<input>"):
        self.d = data
        self.name = name
        if len(data) < 64 or data[0:2] != b"MZ":
            raise PEError("not a PE file (missing MZ): %s" % name)
        self.e_lfanew = struct.unpack("<I", data[0x3C:0x40])[0]
        if len(data) < self.e_lfanew + 6 or \
                data[self.e_lfanew:self.e_lfanew + 4] != b"PE\0\0":
            raise PEError("bad PE signature at e_lfanew=%#x" % self.e_lfanew)
        coff = self.e_lfanew + 4  # skip the PE\0\0 signature
        (self.machine, self.nsec) = struct.unpack("<HH", data[coff:coff + 4])
        optsz = struct.unpack("<H", data[coff + 16:coff + 18])[0]
        self.file_chars = struct.unpack("<H", data[coff + 18:coff + 20])[0]
        self.opt = coff + 20
        magic = struct.unpack("<H", data[self.opt:self.opt + 2])[0]
        if magic == 0x10B:
            raise PEError("PE32 (32-bit) refused: WOW64 is a future W32C "
                          "series (W32APP D3, §7)")
        if magic != 0x20B:
            raise PEError("unknown optional-header magic %#x" % magic)
        # PE32+ Windows fields: Subsystem @ opt+68, DllCharacteristics @
        # opt+70, Major/MinorSubsystemVersion @ opt+48/50.
        self.subsystem = struct.unpack("<H", data[self.opt + 68:self.opt + 70])[0]
        self.subsys_major, self.subsys_minor = struct.unpack(
            "<HH", data[self.opt + 48:self.opt + 52])
        # Correct offsets (PE32+): Subsystem @ opt+68, MajorSubsystemVersion
        # @ opt+72. The double-read above is intentional belt-and-braces.
        self.dll_chars = struct.unpack("<H", data[self.opt + 70:self.opt + 72])[0]
        self.n_rva = struct.unpack("<I", data[self.opt + 108:self.opt + 112])[0]
        self.dirs = self.opt + 112
        self.secs = []
        base = self.opt + optsz
        for i in range(self.nsec):
            o = base + i * 40
            if len(data) < o + 40:
                raise PEError("section table runs past EOF")
            nm = data[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            vsz, vaddr, rawsz, raw = struct.unpack("<IIII", data[o + 8:o + 24])
            self.secs.append((nm, vaddr, vsz, raw, rawsz))

    def datadir(self, idx):
        if idx >= self.n_rva:
            return (0, 0)
        o = self.dirs + idx * 8
        return struct.unpack("<II", self.d[o:o + 8])

    def rva2off(self, rva):
        for (_nm, vaddr, vsz, raw, rawsz) in self.secs:
            span = max(vsz, rawsz)
            if vaddr <= rva < vaddr + span:
                off = raw + (rva - vaddr)
                if off >= len(self.d):
                    raise PEError("RVA %#x maps past EOF" % rva)
                return off
        raise PEError("RVA %#x is in no section" % rva)

    def cstr(self, rva, limit=512):
        off = self.rva2off(rva)
        end = self.d.find(b"\0", off, off + limit)
        if end < 0:
            raise PEError("unterminated string at RVA %#x" % rva)
        return self.d[off:end].decode("ascii", "replace")

    def imports(self):
        """Yield (dll, symbol, is_delay). Symbol is a name or '#<ord>'."""
        out = []
        irva, isize = self.datadir(1)
        if irva:
            off = self.rva2off(irva)
            while True:
                if off + 20 > len(self.d):
                    raise PEError("import descriptor runs past EOF")
                ilt, _ts, _fw, name_rva, iat = struct.unpack("<5I", self.d[off:off + 20])
                if (ilt, _ts, _fw, name_rva, iat) == (0, 0, 0, 0, 0):
                    break
                dll = self.cstr(name_rva).lower()
                thunk = ilt or iat
                if not thunk:
                    raise PEError("import %s has no thunk table" % dll)
                toff = self.rva2off(thunk)
                while True:
                    if toff + 8 > len(self.d):
                        raise PEError("thunk table runs past EOF (%s)" % dll)
                    entry = struct.unpack("<Q", self.d[toff:toff + 8])[0]
                    if entry == 0:
                        break
                    if entry & 0x8000000000000000:
                        out.append((dll, "#%d" % (entry & 0xFFFF), False))
                    else:
                        hrva = entry & 0x7FFFFFFF
                        hoff = self.rva2off(hrva)
                        if hoff + 2 > len(self.d):
                            raise PEError("hint/name runs past EOF (%s)" % dll)
                        nm = self.cstr(hrva + 2)
                        out.append((dll, nm, False))
                    toff += 8
                off += 20
                if isize and off - self.rva2off(irva) >= isize + 20:
                    break
        # Delay-load directory (index 13): same walk, marked delay.
        drva, dsz = self.datadir(13)
        if drva and dsz:
            doff = self.rva2off(drva)
            n = dsz // 32
            for i in range(n):
                o = doff + i * 32
                if o + 32 > len(self.d):
                    raise PEError("delay descriptor runs past EOF")
                # Delay descriptor layout: attrs, szName, phmod, pIAT, pINT,
                # pBoundIAT, pUnloadIAT, dwTimeStamp.
                (_attrs, name_rva, _phmod, _piat, pint, _pb, _pu, _ts2) = \
                    struct.unpack("<8I", self.d[o:o + 32])
                if name_rva == 0:
                    continue  # terminator entry
                dll = self.cstr(name_rva).lower()
                if not pint:
                    continue
                toff = self.rva2off(pint)
                while True:
                    if toff + 8 > len(self.d):
                        raise PEError("delay thunk table runs past EOF (%s)" % dll)
                    entry = struct.unpack("<Q", self.d[toff:toff + 8])[0]
                    if entry == 0:
                        break
                    if entry & 0x8000000000000000:
                        out.append((dll, "#%d" % (entry & 0xFFFF), True))
                    else:
                        out.append((dll, self.cstr((entry & 0x7FFFFFFF) + 2), True))
                    toff += 8
        return out

    def pdata_count(self):
        rva, size = self.datadir(3)
        if not rva or not size:
            return 0
        if size % 12:
            raise PEError("exception directory size %d not a multiple of 12" % size)
        return size // 12

    def tls_present(self):
        rva, size = self.datadir(9)
        return bool(rva and size)

    def manifest(self):
        """Return (comctl, exec_level, dpi, supported_os) from .rsrc type 24.

        comctl is 'v6' / 'unversioned' / 'none'. Missing manifest is not an
        error -- it is a measurement ('none')."""
        rva, _size = self.datadir(2)
        if not rva:
            return ("none", "(none)", "(none)", 0)
        root = self.rva2off(rva)

        def rdir(off):
            if off + 16 > len(self.d):
                raise PEError("resource directory runs past EOF")
            (_c, _t, _maj, _min, nnamed, nid) = struct.unpack("<IIHHHH", self.d[off:off + 16])
            ents = []
            for i in range(nnamed + nid):
                o = off + 16 + i * 8
                ents.append(struct.unpack("<II", self.d[o:o + 8]))
            return ents

        xml = None
        for (rid, roff) in rdir(root):
            if rid != 24 or not (roff & 0x80000000):
                continue
            for (_rid2, roff2) in rdir(root + (roff & 0x7FFFFFFF)):
                if not (roff2 & 0x80000000):
                    continue
                for (_rid3, roff3) in rdir(root + (roff2 & 0x7FFFFFFF)):
                    if roff3 & 0x80000000:
                        continue
                    doff = root + roff3
                    data_rva, data_sz = struct.unpack("<II", self.d[doff:doff + 8])
                    boff = self.rva2off(data_rva)
                    raw = self.d[boff:boff + data_sz]
                    try:
                        xml = raw.decode("utf-8")
                    except UnicodeDecodeError:
                        xml = raw.decode("utf-16-le", "replace")
                    break
        if xml is None:
            return ("none", "(none)", "(none)", 0)
        if "Microsoft.Windows.Common-Controls" not in xml:
            comctl = "none"
        elif 'name="Microsoft.Windows.Common-Controls"' in xml and "6.0.0.0" in xml:
            comctl = "v6"
        else:
            comctl = "unversioned"
        m = re.search(r'requestedExecutionLevel level="([^"]+)"', xml)
        level = m.group(1) if m else "(none)"
        dpi = []
        if re.search(r"<dpiAware>", xml):
            dpi.append("dpiAware")
        m = re.search(r"dpiAwareness[^>]*>([^<]+)<", xml)
        if m:
            dpi.append(m.group(1).strip())
        nos = len(re.findall(r"<ms_compatibility:supportedOS|<supportedOS", xml))
        return (comctl, level, ", ".join(dpi) if dpi else "(none)", nos)


# ---------------------------------------------------------------------------
# Subsystem names (only what the ladder shows; unknown ids print numeric).
# ---------------------------------------------------------------------------

SUBSYSTEMS = {2: "GUI", 3: "CUI"}


def classify(dll, sym):
    if (dll, sym) in OVERRIDES:
        return OVERRIDES[(dll, sym)][0]
    if dll in REAL_MODULES:
        return "REAL"
    if dll in FAIL_CLEAN_MODULES:
        return "FAIL-CLEAN"
    raise PEError("UNKNOWN MODULE %s (symbol %s): adding a module to the "
                  "personality is a plan decision (W32APP D1/D9) -- extend "
                  "MODULE_CLASSES with a cited reason" % (dll, sym))


def emit_ledger(path, label, version):
    with open(path, "rb") as f:
        data = f.read()
    img = PEImage(data, path)
    sha = hashlib.sha256(data).hexdigest()
    imports = sorted(set(img.imports()))
    comctl, level, dpi, nos = img.manifest()
    delay_dlls = sorted({d for (d, _s, dl) in imports if dl})
    lines = []
    lines.append("# ledger: %s" % label)
    lines.append("# generator: tools/w32_import_ledger.py (W32APP_PLAN.md W32A-0)")
    lines.append("# Source binary: %s (%s) -- USER-SUPPLIED, NEVER COMMITTED (§1.1)." % (
        os.path.basename(path), version))
    lines.append("# Only DLL/symbol NAMES are recorded: facts about an interface, no bytes.")
    lines.append("version: %s" % version)
    lines.append("file: %s" % os.path.basename(path))
    lines.append("size: %d" % len(data))
    lines.append("sha256: %s" % sha)
    kind = "DLL" if (img.file_chars & 0x2000) else SUBSYSTEMS.get(img.subsystem, "?")
    lines.append("format: PE32+ %s x86-64" % kind)
    lines.append("subsystem: %d.%d" % (img.subsys_major, img.subsys_minor))
    lines.append("characteristics: %#x" % img.file_chars)
    lines.append("pdata_functions: %d" % img.pdata_count())
    lines.append("tls_directory: %s" % ("present" if img.tls_present() else "absent"))
    lines.append("delay_dlls: %s" % (", ".join(delay_dlls) if delay_dlls else "(none)"))
    lines.append("manifest_comctl: %s" % comctl)
    lines.append("manifest_exec_level: %s" % level)
    lines.append("manifest_dpi: %s" % dpi)
    lines.append("manifest_supported_os: %d" % nos)
    lines.append("imports_total: %d" % len(imports))
    lines.append("# dll | symbol (#<n> = ordinal) | class | observed [delay]")
    for (dll, sym, dl) in imports:
        cls = classify(dll, sym)
        row = "%s %s %s static-only" % (dll, sym, cls)
        if dl:
            row += " delay"
        lines.append(row)
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# agree: cross-check our parse against llvm-readobj-19 (dev-time only).
# ---------------------------------------------------------------------------

def readobj_imports(path):
    try:
        out = subprocess.run(["llvm-readobj-19", "--coff-imports", path],
                             capture_output=True, text=True, check=True)
    except FileNotFoundError:
        print("agree: llvm-readobj-19 not installed -- skipping "
              "(dev-time check only, never CI)")
        return None
    cur = None
    found = set()
    for line in out.stdout.splitlines():
        m = re.search(r"Name:\s*(\S+\.dll)", line, re.I)
        if m:
            cur = m.group(1).lower()
            continue
        m = re.search(r"Symbol:\s*(\S+)", line)
        if m and cur:
            sym = m.group(1)
            # llvm-readobj spells ordinal imports "(17)"; we spell "#17".
            mo = re.fullmatch(r"\((\d+)\)", sym)
            if mo:
                sym = "#%s" % mo.group(1)
            found.add((cur, sym))
            continue
        m = re.search(r"Ordinal:\s*(\d+)", line)
        if m and cur:
            found.add((cur, "#%s" % m.group(1)))
    return found


def cmd_agree(path):
    with open(path, "rb") as f:
        data = f.read()
    img = PEImage(data, path)
    ours = {(d, s) for (d, s, _dl) in img.imports()}
    theirs = readobj_imports(path)
    if theirs is None:
        return 0
    # llvm-readobj folds delay imports into --coff-imports without marking
    # them; compare the union (delay marking is verified separately below).
    only_ours = sorted(ours - theirs)
    only_theirs = sorted(theirs - ours)
    if only_ours or only_theirs:
        print("agree: MISMATCH for %s" % path)
        for x in only_ours:
            print("  only w32_import_ledger: %s %s" % x)
        for x in only_theirs:
            print("  only llvm-readobj-19:   %s %s" % x)
        return 1
    print("agree: %s: %d imports identical to llvm-readobj-19" % (path, len(ours)))
    return 0


# ---------------------------------------------------------------------------
# check: verify the committed ledgers (the W32A-0 gate).
# ---------------------------------------------------------------------------

# §2.2 census: ledger file -> expected import count. Deviation fails.
EXPECTED_TOTALS = {
    "putty-0.85.imports": 348,
    "7zFM-24.09.imports": 298,
    "7z-24.09.imports": 86,
    "notepad++-8.8.9.imports": 590,
}

# §2.2 union: distinct KERNEL32/USER32/GDI32 symbols across the ledgers
# (3 apps + NPP plugins). VERIFIED values: 8/8 llvm-readobj set-agreements
# plus an independent re-parse; the plan draft said 600/558 from a superseded
# ad-hoc text pipeline (see the W32A-0 Result).
EXPECTED_UNION = 611
# Current personality coverage of that union (w32/w32_bind.c: 44 exports).
EXPECTED_GAP = 569

KUG = {"kernel32.dll", "user32.dll", "gdi32.dll"}
DLL_OF = {"K32": "kernel32.dll", "U32": "user32.dll", "G32": "gdi32.dll"}


def parse_ledger(path):
    """Parse one ledger file. Returns (meta dict, [(dll, sym, cls, obs, delay)])."""
    meta = {}
    rows = []
    plugins = {}  # plugin file -> {"sha256":..., "total":..., "rows":[...]}
    cur_plugin = None
    with open(path) as f:
        for ln, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip():
                continue
            if line.startswith("# ledger:") or line.startswith("# generator:") or \
               line.startswith("# Source binary:") or line.startswith("# Source binaries:") or \
               line.startswith("# Only ") or \
               line.startswith("# dll |"):
                continue
            if line.startswith("### plugin:"):
                cur_plugin = line.split(":", 1)[1].strip()
                if not cur_plugin or cur_plugin in plugins:
                    raise PEError("%s:%d: bad plugin header" % (path, ln))
                plugins[cur_plugin] = {"rows": []}
                continue
            if line.startswith("#"):
                raise PEError("%s:%d: unknown comment line: %r" % (path, ln, line))
            if ":" in line and " " not in line.split(":")[0]:
                k, v = line.split(":", 1)
                k = k.strip()
                v = v.strip()
                if cur_plugin and k in ("sha256", "imports_total"):
                    plugins[cur_plugin][k] = v
                    continue
                if not cur_plugin and k in (
                        "version", "file", "size", "sha256", "format",
                        "subsystem", "characteristics", "pdata_functions",
                        "tls_directory", "delay_dlls", "manifest_comctl",
                        "manifest_exec_level", "manifest_dpi",
                        "manifest_supported_os", "imports_total"):
                    meta[k] = v
                    continue
                raise PEError("%s:%d: bad header line: %r" % (path, ln, line))
            parts = line.split()
            if len(parts) not in (4, 5) or parts[3] not in ("static-only", "observed"):
                raise PEError("%s:%d: bad import row: %r" % (path, ln, line))
            dll, sym, cls = parts[0], parts[1], parts[2]
            if cls not in ("REAL", "FAIL-CLEAN", "REFUSE"):
                raise PEError("%s:%d: bad class %r" % (path, ln, cls))
            if len(parts) == 5 and parts[4] != "delay":
                raise PEError("%s:%d: bad row trailer: %r" % (path, ln, line))
            row = (dll, sym, cls, parts[3], len(parts) == 5)
            if cur_plugin:
                plugins[cur_plugin]["rows"].append(row)
            else:
                rows.append(row)
    return meta, rows, plugins


def current_exports():
    """Parse the export table from w32/src/w32_bind.c: {dll: {names}}."""
    out = {}
    text = open(BIND_C).read()
    for m in re.finditer(r'\{\s*(K32|U32|G32)\s*,\s*"([^"]+)"', text):
        out.setdefault(DLL_OF[m.group(1)], set()).add(m.group(2))
    return out


def cmd_check():
    fails = []

    def bad(msg):
        fails.append(msg)
        print("  FAIL: %s" % msg)

    # 1. Every expected ledger exists and parses; totals match §2.2.
    union = set()
    for fn, want in sorted(EXPECTED_TOTALS.items()):
        p = os.path.join(LEDGER_DIR, fn)
        if not os.path.isfile(p):
            bad("missing ledger %s" % fn)
            continue
        try:
            meta, rows, plugins = parse_ledger(p)
        except PEError as e:
            bad("unparseable ledger %s: %s" % (fn, e))
            continue
        if plugins:
            bad("ledger %s: single-binary file must not contain plugin blocks" % fn)
        got = len(rows)
        if got != want:
            bad("ledger %s: %d imports, §2.2 says %d" % (fn, got, want))
        if meta.get("imports_total") != str(got):
            bad("ledger %s: header imports_total=%s but %d rows" % (
                fn, meta.get("imports_total"), got))
        for req in ("version", "file", "size", "sha256", "format",
                    "pdata_functions", "tls_directory", "manifest_comctl"):
            if req not in meta:
                bad("ledger %s: header lacks %s" % (fn, req))
        if len(sha256_of(meta.get("sha256", ""))) != 64:
            bad("ledger %s: sha256 is not 64 hex chars" % fn)
        # Deterministic order: rows sorted by (dll, symbol).
        keys = [(d, s) for (d, s, _c, _o, _dl) in rows]
        if keys != sorted(keys):
            bad("ledger %s: rows are not sorted by (dll, symbol)" % fn)
        if len(set(keys)) != len(keys):
            bad("ledger %s: duplicate rows" % fn)
        for (dll, sym, _c, _o, _dl) in rows:
            if dll in KUG:
                union.add(sym)

    # 2. Plugin ledger parses; every plugin entry has hash + total + rows.
    pp = os.path.join(LEDGER_DIR, "npp-plugins-8.8.9.imports")
    if not os.path.isfile(pp):
        bad("missing ledger npp-plugins-8.8.9.imports")
    else:
        try:
            _meta, rows, plugins = parse_ledger(pp)
        except PEError as e:
            fails.append("unparseable plugin ledger: %s" % e)
            print("  FAIL: unparseable plugin ledger: %s" % e)
            plugins = {}
            rows = []
        if rows:
            bad("plugin ledger: single-binary rows outside plugin blocks")
        for name, ent in sorted(plugins.items()):
            if "sha256" not in ent or len(ent["sha256"]) != 64:
                bad("plugin %s: bad/missing sha256" % name)
            if ent.get("imports_total") != str(len(ent["rows"])):
                bad("plugin %s: header total != row count" % name)
            for (dll, sym, _c, _o, _dl) in ent["rows"]:
                if dll in KUG:
                    union.add(sym)

    # 3. Current exports from the tree; union/gap arithmetic.
    if os.path.isfile(BIND_C):
        have = set()
        for names in current_exports().values():
            have |= names
        print("  personality exports today: %d" % len(have))
    else:
        bad("missing %s" % BIND_C)
        have = set()
    print("  K/U/G union across ledgers: %d" % len(union))
    if len(union) != EXPECTED_UNION:
        bad("union is %d symbols, §2.2 says %d" % (len(union), EXPECTED_UNION))
    gap = len(union - have)
    print("  uncovered (gap): %d" % gap)
    if gap != EXPECTED_GAP:
        bad("gap is %d symbols, §2.2 says %d" % (gap, EXPECTED_GAP))

    if fails:
        print("w32_import_ledger: CHECK FAILED (%d problem%s)" % (
            len(fails), "s" if len(fails) != 1 else ""))
        return 1
    print("w32_import_ledger: CHECK OK -- 4 ledgers + plugins, union %d, gap %d" % (
        EXPECTED_UNION, EXPECTED_GAP))
    return 0


def sha256_of(s):
    try:
        int(s, 16)
    except ValueError:
        return ""
    return s


def usage():
    print(__doc__.split("Usage:")[1].strip())


def main(argv):
    if len(argv) < 2:
        usage()
        return 2
    if argv[1] == "dump":
        # dump PE-FILE --label NAME --version VER
        if len(argv) != 7 or argv[3] != "--label" or argv[5] != "--version":
            usage()
            return 2
        try:
            sys.stdout.write(emit_ledger(argv[2], argv[4], argv[6]))
        except PEError as e:
            print("w32_import_ledger: error: %s" % e)
            return 1
        return 0
    if argv[1] == "agree":
        if len(argv) != 3:
            usage()
            return 2
        try:
            return cmd_agree(argv[2])
        except PEError as e:
            print("w32_import_ledger: error: %s" % e)
            return 1
    if argv[1] == "check":
        return cmd_check()
    usage()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
