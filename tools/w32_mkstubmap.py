#!/usr/bin/env python3
"""Draft w32/stub_map.tsv from the W32A-0 ledgers + mingw-w64 declarations.

W32APP_PLAN.md W32A-1 ("the loader binds all five pinned files' static
tables against possibly-stubbed modules -- binding is not behaviour").
Every ledger (dll, symbol) NOT already implemented in w32/src/w32_bind.c
needs exactly one stub-map row, which tools/w32_gen_stubs.py turns into a
bound address.  Ordinal rows (#17) resolve through w32/ordinal_map.tsv to
their canonical name first: the map carries names, the ordinal table carries
the numbers, and the loader joins them.

Return-type facts come from mingw-w64 headers (D4-permitted declarations).
Only ONE fact per symbol crosses into the committed TSV -- the width class
its stub must return (32-bit zero, 64-bit NULL, E_NOTIMPL, ...).  No
declaration, no sample code, no macro is copied: the generated stubs take
(void) and fail loudly, which is all W32A-1 promises.

Two-level pipeline, and why:
  maintainer:  w32_mkstubmap.py (this; needs mingw headers, NOT hermetic)
               -> w32/stub_map.tsv (committed, curation tables below)
  CI-hermetic: tools/w32_gen_stubs.py (python + TSV only)
               -> w32/src/w32_stubs_gen.c + w32/include/w32/w32_gen.h
The claim checker re-runs the SECOND level byte-for-byte, never this one.

Curation tables (HANDLE conventions, mpr codes, owning phases) live HERE,
not in the TSV, so re-running never loses a human decision.  Re-run after
a ledger changes and diff the TSV -- the diff is the review.

Usage:
    python3 tools/w32_mkstubmap.py [--check]
      (writes w32/stub_map.tsv; --check only reports what would change)
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEDGER_DIR = os.path.join(ROOT, "w32", "app_ledger")
BIND_C = os.path.join(ROOT, "w32", "src", "w32_bind.c")
ORDMAP = os.path.join(ROOT, "w32", "ordinal_map.tsv")
OUT = os.path.join(ROOT, "w32", "stub_map.tsv")

MINGW_INC_CANDIDATES = [
    "/usr/x86_64-w64-mingw32/include",
    "/usr/share/mingw-w64/include",
]

DLL_OF = {"K32": "kernel32.dll", "U32": "user32.dll", "G32": "gdi32.dll"}

# ---------------------------------------------------------------------------
# Curation table 1: owning phase per module (advisory -- the log text a TODO
# stub prints; behaviour phases replace the stub and the row's kind).
# ---------------------------------------------------------------------------
PHASE_OF_DLL = {
    "kernel32.dll": "W32A-2",
    "user32.dll": "W32A-5",
    "gdi32.dll": "W32A-7",
    "comctl32.dll": "W32A-8",
    "advapi32.dll": "W32A-9",
    "shell32.dll": "W32A-10",
    "comdlg32.dll": "W32A-10",
    "shlwapi.dll": "W32A-10",
    "version.dll": "W32A-10",
    "wininet.dll": "W32A-10",
    "dbghelp.dll": "W32A-10",
    "sensapi.dll": "W32A-10",
    "wintrust.dll": "W32A-10",
    "crypt32.dll": "W32A-10",
    "dwmapi.dll": "W32A-10",
    "ole32.dll": "W32A-11",
    "oleaut32.dll": "W32A-11",
    "uxtheme.dll": "W32A-11",
    "imm32.dll": "W32A-11",
    "ws2_32.dll": "W32A-12",
    "msvcrt.dll": "W32A-13",
    "mpr.dll": "W32A-1",
}

# Substring rules refining the dll default: (dll, substring, phase).
# Threading lives in W32A-3, dialogs/menus/clipboard in W32A-6.
PHASE_REFINEMENTS = [
    ("kernel32.dll", "Thread", "W32A-3"),
    ("kernel32.dll", "Tls", "W32A-3"),
    ("kernel32.dll", "Fiber", "W32A-3"),
    ("kernel32.dll", "Wait", "W32A-3"),
    ("kernel32.dll", "Sleep", "W32A-3"),
    ("user32.dll", "Dialog", "W32A-6"),
    ("user32.dll", "Dlg", "W32A-6"),
    ("user32.dll", "Menu", "W32A-6"),
    ("user32.dll", "Menu", "W32A-6"),
    ("user32.dll", "Clipboard", "W32A-6"),
    ("user32.dll", "MessageBox", "W32A-6"),
]

# ---------------------------------------------------------------------------
# Curation table 2: REAL in W32A-1 (hand implementations, not stubs).
# The eight BSTR/VARIANT memory functions are pure libc -- malloc, length
# headers, memcpy -- with no OS surface, so they land early as REAL the way
# dbghelp's ImageNtHeader was "trivially real" in the plan.  W32A-11 adopts
# them (vector suite) instead of writing them.  The generator never emits
# these; the checker asserts they resolve from w32/src/w32_oleaut32.c.
# ---------------------------------------------------------------------------
REAL_IN_W32A1 = {
    ("oleaut32.dll", "SysAllocString"),
    ("oleaut32.dll", "SysAllocStringLen"),
    ("oleaut32.dll", "SysAllocStringByteLen"),
    ("oleaut32.dll", "SysFreeString"),
    ("oleaut32.dll", "SysStringLen"),
    ("oleaut32.dll", "SysStringByteLen"),
    ("oleaut32.dll", "VariantClear"),
    ("oleaut32.dll", "VariantCopy"),
}

# ---------------------------------------------------------------------------
# Curation table 3: FINAL fail-clean behaviours owned by W32A-1.
# (dll, symbol) -> (fail expression, note).  Everything else FAIL-CLEAN in
# the ledger stays a TODO owned by its behaviour phase (W32A-9/10/11 refine
# the per-function accept/ignore tables); only mpr is final here, reached
# via the W32A-1 delay path per the plan.
# ---------------------------------------------------------------------------
FAILCLEAN_FINAL = {
    # No network provider exists: open/add/query fail with NO_NETWORK,
    # enumeration is empty (NO_MORE_ITEMS), closing a never-opened enum
    # is an invalid handle.  All six return the code directly (DWORD).
    ("mpr.dll", "WNetOpenEnumW"): ("1222", "no network provider"),
    ("mpr.dll", "WNetAddConnection2W"): ("1222", "no network provider"),
    ("mpr.dll", "WNetGetResourceInformationW"): ("1222", "no network provider"),
    ("mpr.dll", "WNetGetResourceParentW"): ("1222", "no network provider"),
    ("mpr.dll", "WNetEnumResourceW"): ("259", "empty enumeration"),
    ("mpr.dll", "WNetCloseEnum"): ("6", "never-opened handle"),
}

# ---------------------------------------------------------------------------
# Curation table 4: HANDLE-returning symbols and their failure value.
# A wrong choice here is a D9 success-shaped lie (CreateFile never returns
# NULL; FindFirstFile never returns NULL), so EVERY HANDLE symbol must be
# listed with its documented convention, and the generator fails loudly on
# any HANDLE symbol missing from the table.  Conventions from published
# MSDN return-value documentation, one fact per row.
# ---------------------------------------------------------------------------
HANDLE_FAIL = {
    # --- INVALID_HANDLE_VALUE (-1) family ---
    ("kernel32.dll", "CreateFileA"): "-1",
    ("kernel32.dll", "CreateFileW"): "-1",
    ("kernel32.dll", "CreateNamedPipeA"): "-1",
    ("kernel32.dll", "CreateToolhelp32Snapshot"): "-1",
    ("kernel32.dll", "FindFirstChangeNotificationW"): "-1",
    ("kernel32.dll", "FindFirstFileA"): "-1",
    ("kernel32.dll", "FindFirstFileW"): "-1",
    ("kernel32.dll", "FindFirstFileExA"): "-1",
    ("kernel32.dll", "FindFirstFileExW"): "-1",
    ("kernel32.dll", "FindFirstStreamW"): "-1",
    # Pseudo-handles, never fail: the stub returns the constant itself.
    ("kernel32.dll", "GetCurrentProcess"): "-1",
    ("kernel32.dll", "GetCurrentThread"): "-2",
    # --- NULL family ---
    ("kernel32.dll", "CreateEventA"): "NULL",
    ("kernel32.dll", "CreateEventW"): "NULL",
    ("kernel32.dll", "CreateFileMappingA"): "NULL",
    ("kernel32.dll", "CreateFileMappingW"): "NULL",
    ("kernel32.dll", "CreateMutexA"): "NULL",
    ("kernel32.dll", "CreateMutexW"): "NULL",
    ("kernel32.dll", "CreateSemaphoreW"): "NULL",
    ("kernel32.dll", "CreateThread"): "NULL",
    ("kernel32.dll", "OpenProcess"): "NULL",
    ("user32.dll", "GetClipboardData"): "NULL",
    ("user32.dll", "GetPropW"): "NULL",
    ("user32.dll", "LoadImageA"): "NULL",
    ("user32.dll", "LoadImageW"): "NULL",
    ("user32.dll", "RemovePropW"): "NULL",
    ("user32.dll", "SetClipboardData"): "NULL",
}

# ---------------------------------------------------------------------------
# Curation table 5: return-type overrides where mingw-w64 is silent or
# self-contradictory.  Each value is a width spelling (PTR64/i32/void) or a
# plain type name; the source of the fact is the comment (MSDN = published
# Microsoft documentation, readable for facts per LICENSING.md).
# ---------------------------------------------------------------------------
RETTYPE_OVERRIDES = {
    # MSDN: BOOLEAN; mingw only carries "#define RtlGenRandom".
    ("advapi32.dll", "SystemFunction036"): "BOOLEAN",
    # MSDN: PIMAGE_NT_HEADERS; absent from mingw's dbghelp.h.
    ("dbghelp.dll", "ImageNtHeader"): "PTR64",
    # kernel32!GetVersion is DWORD; a shlwapi HRESULT namesake collides.
    ("kernel32.dll", "GetVersion"): "DWORD",
    # kernel32!RtlVirtualUnwind returns PEXCEPTION_ROUTINE (a pointer).
    ("kernel32.dll", "RtlVirtualUnwind"): "PTR64",
    ("user32.dll", "GetWindow"): "HWND",
    ("user32.dll", "SetCursor"): "HCURSOR",
    ("user32.dll", "GetParent"): "HWND",
}

# msvcrt internals mingw-w64 never declares (no header carries them): the
# width facts from published CRT documentation.  All are W32A-13-owned;
# W32A-1 only binds them.
CRT_INTERNAL = {
    ("msvcrt.dll", "_CxxThrowException"): "void",
    ("msvcrt.dll", "__CxxFrameHandler"): "i32",
    ("msvcrt.dll", "__dllonexit"): "PTR64",
    ("msvcrt.dll", "__getmainargs"): "i32",
    ("msvcrt.dll", "__set_app_type"): "void",
    ("msvcrt.dll", "__setusermatherr"): "void",
    ("msvcrt.dll", "_initterm"): "void",
}

# msvcrt DATA exports mingw only exposes via __p__ accessors (never as
# externs): (rawtype, size).  Facts from published CRT documentation.
DATA_OVERRIDES = {
    ("msvcrt.dll", "_acmdln"): ("char*", 8),
    ("msvcrt.dll", "_fmode"): ("int", 4),
    ("msvcrt.dll", "_commode"): ("int", 4),
}

# MSVC-mangled C++ names (no header declares them; the ABI facts are that
# both return void and take opaque args the stub ignores).
MANGLED_VOID = {
    ("msvcrt.dll", "??1type_info@@UEAA@XZ"),
    ("msvcrt.dll", "?terminate@@YAXXZ"),
}

# Exit-family: a (void) stub that merely returns would resume AFTER exit.
# These get a one-arg stub that really terminates via ExitProcess.
EXIT_FAMILY = {
    # Single-threaded until W32A-3, so ExitThread(code) is ExitProcess(code).
    ("kernel32.dll", "ExitThread"),
    ("msvcrt.dll", "exit"),
    ("msvcrt.dll", "_exit"),
    ("msvcrt.dll", "_c_exit"),
    ("msvcrt.dll", "_cexit"),
}

# Symbols whose stub returns E_NOTIMPL rather than zero, beyond what the
# HRESULT return type already implies (kept for the day a non-HRESULT
# success-zero appears; empty today, asserted so).
E_NOTIMPL_EXTRA = set()


def static_exports():
    out = {}
    text = open(BIND_C).read()
    for m in re.finditer(r'\{\s*(K32|U32|G32)\s*,\s*"([^"]+)"', text):
        out.setdefault(DLL_OF[m.group(1)], set()).add(m.group(2))
    # Hand-added REAL rows outside K/U/G spell the dll literally.
    for m in re.finditer(r'\{\s*"([A-Za-z0-9_]+\.dll)"\s*,\s*"([^"]+)"', text):
        out.setdefault(m.group(1).lower(), set()).add(m.group(2))
    return out


def load_ordmap():
    m = {}
    for line in open(ORDMAP):
        if not line.strip() or line.startswith("#") or line.startswith("dll\t"):
            continue
        dll, ordinal, name, _src = line.rstrip("\n").split("\t")
        m[(dll.lower(), int(ordinal))] = name
    return m


def ledger_rows():
    rows = {}
    for path in sorted(glob.glob(os.path.join(LEDGER_DIR, "*.imports"))):
        for line in open(path):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 4 or parts[2] not in ("REAL", "FAIL-CLEAN", "REFUSE"):
                continue
            rows.setdefault((parts[0].lower(), parts[1]), parts[2])
    return rows


def find_mingw_inc():
    for c in MINGW_INC_CANDIDATES:
        if os.path.isfile(os.path.join(c, "winbase.h")):
            return c
    return None


def scan_mingw(inc):
    """Build name -> rettype and name -> (rawtype, size) from mingw headers.

    Four declaration shapes, one fact each (the return type / data size):
      RET <any *API* macro> Name ( ...      (WINAPI, APIENTRY, THEMEAPI,
                                            DWMAPI, WINOLEAPI, WINGDIAPI...)
      SHSTDAPI_(RET) / LWSTDAPI_(RET) /
      WINOLEAPI_(RET) Name ( ...            (shell/shlwapi/ole spellings)
      RET __cdecl Name ( ...                (msvcrt C functions)
      extern [declspec] TYPE name[([N])];   (CRT data exports)
    """
    decls = {}
    datas = {}

    def note(name, rettype):
        if name in decls and decls[name] != rettype:
            decls[name] = "CONFLICT:%s/%s" % (decls[name], rettype)
        else:
            decls.setdefault(name, rettype)

    files = sorted(glob.glob(os.path.join(inc, "*.h")))
    for path in files:
        try:
            text = open(path, errors="replace").read()
        except OSError:
            continue
        flat = re.sub(r"\s+", " ", text)
        # Fused single-macro prefixes (each a #define for a fuller
        # spelling); expanded so shape 1 matches.  The (?!_\() guard keeps
        # the MACRO_(RET) shape-2 forms intact.
        flat = flat.replace("BOOLAPI", "BOOL WINAPI")
        flat = re.sub(r"\b(STDAPI|SHSTDAPI|LWSTDAPI|THEMEAPI|DWMAPI|"
                      r"WINOLEAPI|SHFOLDERAPI)\b(?!_\s*\()",
                      r"HRESULT WINAPI", flat)
        # Shape 1: RET CONV Name(.  CONV is any all-caps word containing
        # API (WINAPI, APIENTRY, WINAPIV, NTAPI, THEMEAPI...).  The
        # declspec salad before RET (WINBASEAPI etc.) cannot misparse: at
        # "WINBASEAPI BOOL WINAPI Find", taking WINBASEAPI as CONV would
        # need "BOOL" to be followed by "(" -- it is followed by WINAPI.
        for m in re.finditer(
                r"([A-Za-z_][A-Za-z0-9_]*(?:\s*\*+)?)\s+"
                r"[A-Z0-9_]*API[A-Z0-9_]*\s+"
                r"(?:__attribute__\(\([^)]*\)\)\s+)?"
                r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", flat):
            note(m.group(2), m.group(1).strip())
        # Shape 2: MACRO_(RET) Name(.
        for m in re.finditer(
                r"(?:SHSTDAPI|LWSTDAPI|WINOLEAPI|THEMEAPI|DWMAPI|"
                r"SHFOLDERAPI|SHSTDAPIV|LWSTDAPIV|"
                r"WINOLEAPIV)_\s*\(\s*([A-Za-z_][A-Za-z0-9_]*(?:\s*\*+)?)"
                r"\s*\)\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(", flat):
            note(m.group(2), m.group(1).strip())
        # Shape 3: RET __cdecl Name(.
        for m in re.finditer(
                r"([A-Za-z_][A-Za-z0-9_]*(?:\s*\*+)?)\s*__cdecl\s+"
                r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", flat):
            note(m.group(2), m.group(1).strip())
        # Shape 4: extern data.
        for m in re.finditer(
                r"extern\s+(?:__declspec\([^)]*\)\s+)?"
                r"([A-Za-z_][A-Za-z0-9_]*\s*\**)\s*"
                r"(_[A-Za-z][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;", flat):
            rawtype, name, arr = m.group(1), m.group(2), m.group(3)
            rawtype = rawtype.strip()
            size = 8 if "*" in rawtype else 4
            if arr:
                try:
                    size *= int(arr.strip("[]"))
                except ValueError:
                    size = -1  # unsized: the human must size it
            datas.setdefault(name, (rawtype, size))
    return decls, datas


def width_of(rettype):
    """Map a return-type spelling to a stub width class."""
    t = rettype.strip()
    if t in ("VOID", "void"):
        return "void"
    if t in ("HRESULT", "SCODE"):
        return "hr"
    if t in ("float", "double"):
        return "f64"
    if t.endswith("*") or "PTR" in t or t in (
            "HANDLE", "HWND", "HDC", "HBITMAP", "HBRUSH", "HFONT", "HICON",
            "HCURSOR", "HMENU", "HPEN", "HPALETTE", "HRGN", "HINSTANCE",
            "HMODULE", "HIMAGELIST", "HIMC", "HHOOK", "HACCEL", "HCERTSTORE",
            "HCRYPTPROV", "HCRYPTKEY", "HCRYPTHASH", "LPVOID", "LRESULT",
            "LPARAM", "WPARAM", "LONG_PTR", "ULONG_PTR", "DWORD_PTR",
            "SIZE_T", "ULONGLONG", "LONGLONG", "UINT_PTR", "HWINSTA",
            "HDESK", "PVOID", "HANDLE_PTR"):
        return "p64"
    # Everything else is a 32-bit-or-less integer/bool/char/enum: BOOL,
    # WINBOOL, BOOLEAN, BYTE, WORD, DWORD, INT, UINT, LONG, ULONG, SHORT,
    # NTSTATUS, char, WCHAR...
    return "i32"


def main(argv):
    check_only = "--check" in argv
    inc = find_mingw_inc()
    if inc is None:
        print("w32_mkstubmap: no mingw-w64 headers found (tried %s)" %
              ", ".join(MINGW_INC_CANDIDATES), file=sys.stderr)
        print("w32_mkstubmap: Debian/Ubuntu: sudo apt install mingw-w64-x86-64-dev",
              file=sys.stderr)
        return 1
    mingw_ver = "unknown"
    try:
        spec = open(os.path.join(inc, "_mingw.h")).read()
        m = re.search(r"__MINGW64_VERSION_(MAJOR|MINOR)\s+(\d+)", spec)
        # (best-effort; the dpkg version is recorded in the header note)
    except OSError:
        pass

    static = static_exports()
    ordmap = load_ordmap()
    rows = ledger_rows()

    decls, datas = scan_mingw(inc)

    out_lines = []
    problems = []
    n_static = 0
    for (dll, sym), cls in sorted(rows.items()):
        if sym.startswith("#"):
            n = int(sym[1:])
            if (dll, n) not in ordmap:
                problems.append("ordinal %s!%s has no ordinal_map row" % (dll, sym))
                continue
            sym = ordmap[(dll, n)]
        # REAL rows are emitted even when their static row already
        # exists: the TSV documents "hand implementation", and the checker
        # pins static coverage from it.  Regen must not lose them.
        if (dll, sym) not in REAL_IN_W32A1 and sym in static.get(dll, set()):
            n_static += 1
            continue
        if cls == "REFUSE":
            problems.append("REFUSE row %s!%s has no stub by design; "
                            "harness must expect refusal" % (dll, sym))
            continue
        key = (dll, sym)
        phase = PHASE_OF_DLL.get(dll, "W32A-?")
        for r_dll, sub, r_phase in PHASE_REFINEMENTS:
            if dll == r_dll and sub in sym:
                phase = r_phase
        if key in REAL_IN_W32A1:
            out_lines.append((dll, sym, cls, "REAL", "-", "-", phase,
                              "w32_oleaut32.c"))
            continue
        if key in FAILCLEAN_FINAL:
            fail, note = FAILCLEAN_FINAL[key]
            out_lines.append((dll, sym, cls, "FAILCLEAN", "DWORD", fail,
                              phase, note))
            continue
        if key in DATA_OVERRIDES:
            _rawtype, size = DATA_OVERRIDES[key]
            out_lines.append((dll, sym, cls, "DATA", _rawtype, str(size),
                              phase, "zeroed BSS (override)"))
            continue
        if sym in datas and sym not in decls:
            _rawtype, size = datas[sym]
            if size is None or size <= 0:
                problems.append("DATA %s!%s needs a human size (%s)" %
                                (dll, sym, _rawtype))
                continue
            out_lines.append((dll, sym, cls, "DATA", _rawtype, str(size),
                              phase, "zeroed BSS"))
            continue
        if key in MANGLED_VOID:
            out_lines.append((dll, sym, cls, "TODO", "void", "-",
                              phase, "MSVC-mangled; void, opaque args"))
            continue
        if key in EXIT_FAMILY:
            out_lines.append((dll, sym, cls, "EXIT", "void", "-",
                              phase, "one-arg stub, really terminates"))
            continue
        if key in CRT_INTERNAL:
            width = {"PTR64": "p64", "i32": "i32",
                     "void": "void"}[CRT_INTERNAL[key]]
            fail = "-" if width == "void" else "0"
            out_lines.append((dll, sym, cls, "TODO", width, fail,
                              phase, "CRT internal, undeclared in mingw"))
            continue
        if key in RETTYPE_OVERRIDES:
            rettype = RETTYPE_OVERRIDES[key]
            if rettype in ("PTR64", "i32", "void"):
                width = {"PTR64": "p64", "i32": "i32",
                         "void": "void"}[rettype]
            else:
                width = width_of(rettype)
        else:
            rettype = decls.get(sym)
            if rettype is None or rettype.startswith("CONFLICT"):
                problems.append("no mingw declaration for %s!%s%s" %
                                (dll, sym,
                                 " (%s)" % rettype if rettype else
                                 " (not in headers)"))
                continue
            width = width_of(rettype)
        if width == "void":
            fail = "-"
        elif width == "hr" or key in E_NOTIMPL_EXTRA:
            fail = "E_NOTIMPL"
        elif rettype == "HANDLE":
            if key in HANDLE_FAIL:
                fail = HANDLE_FAIL[key]
            else:
                problems.append("HANDLE %s!%s missing from HANDLE_FAIL "
                                "(add with -1/-2/NULL)" % (dll, sym))
                continue
        elif width == "f64":
            fail = "0"
        else:
            fail = "0"
        kind = "TODO"
        note = "phase-owned"
        if cls == "FAIL-CLEAN":
            note = "FAIL-CLEAN final lands in %s" % phase
        out_lines.append((dll, sym, cls, kind,
                          rettype if width != "p64" or rettype == "HANDLE"
                          else "PTR64", fail, phase, note))

    header = [
        "# stub_map.tsv -- every ladder import the W32A-1 loader binds.",
        "#",
        "# GENERATED by tools/w32_mkstubmap.py -- do not edit.  Curation",
        "# (phases, HANDLE conventions, mpr codes) lives in that script;",
        "# re-run it after a ledger changes and review the diff.",
        "#",
        "# One row per ledger (dll, symbol) not already implemented in",
        "# w32/src/w32_bind.c's static table.  Ordinal ledger rows resolve",
        "# to their canonical name via w32/ordinal_map.tsv first.",
        "#",
        "# Columns: dll | symbol | ledger-class | kind | rettype | fail |",
        "#          phase | note",
        "# kind: REAL (hand impl, generator skips) | TODO (phase-owned",
        "#   placeholder: logs once, sets ERROR_NOT_SUPPORTED, fails) |",
        "#   FAILCLEAN (final W32A-1 fail-clean) | DATA (zeroed variable).",
        "# fail: 0 | NULL | -1 | -2 | E_NOTIMPL | <win32 code> | -.",
    ]
    body = ["\t".join(r) for r in sorted(out_lines)]
    text = "\n".join(header + ["dll\tsymbol\tclass\tkind\trettype\tfail\t"
                              "phase\tnote"] + body) + "\n"

    if check_only:
        if os.path.isfile(OUT) and open(OUT).read() == text:
            print("w32_mkstubmap: %s in sync (%d rows, %d static)" %
                  (OUT, len(body), n_static))
            return 0
        print("w32_mkstubmap: %s would change (%d rows)" % (OUT, len(body)))
        return 1

    for p in problems:
        print("w32_mkstubmap: STOP: %s" % p, file=sys.stderr)
    if problems:
        print("w32_mkstubmap: %d row(s) need curation; TSV not written" %
              len(problems), file=sys.stderr)
        return 1
    open(OUT, "w").write(text)
    print("w32_mkstubmap: wrote %s (%d rows, %d already static)" %
          (OUT, len(body), n_static))
    return 0



if __name__ == "__main__":
    sys.exit(main(sys.argv))
