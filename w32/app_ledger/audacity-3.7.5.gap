# gap ledger: Audacity 3.7.5 (64-bit) — W32APP_PLAN.md phase W32A-17 (App horizon)
# generator: tools/w32_import_ledger.py facts + a transitive walk of the bundle
#            (cross-checked with the loader's own parser, build/w32_peinfo).
# Source binary: Audacity.exe (Audacity 3.7.5, win 64-bit portable zip)
#   -- USER-SUPPLIED, NEVER COMMITTED (§1.1).  Only NAMES/COUNTS/HASHES here.
# This is the DIFF artefact the phase promised: what Audacity needs vs. what the
# personality provides.  Outcome: B (does not run) -- see the decision below.
#
# Provenance of the pinned subject (obtained from the publisher, GitHub release
# audacity/audacity Audacity-3.7.5, never redistributed here):
zip:          audacity-win-3.7.5-64bit.zip
zip_size:     26834384
zip_sha256:   0bc382a89cd44ee2df80f9b725fa22a697163572a8aa04899ee8544dc2932926
file:         Audacity.exe
size:         13302832
sha256:       e82c5ef50ae6da08acf6c272ddddceeed677a86f636e120ee98ca58a81bc8a8f
format:       PE32+ GUI x86-64, subsystem 2.0, image_base 0x140000000, 9 sections
bundle_dlls:  117   # lib-*.dll (Audacity), wxmsw313u_*_vc_x64_custom.dll (wxWidgets
                    # 3.1.3), plus FFmpeg/FLAC/vcruntime140/msvcp140/concrt140/...

# ---------------------------------------------------------------------------
# WHY THE SINGLE-BINARY LEDGER MODEL BREAKS HERE (the horizon surprise)
# ---------------------------------------------------------------------------
# PuTTY / 7-Zip / Notepad++ import the Win32 SYSTEM DLLs directly from the .exe,
# so `w32_import_ledger.py dump <exe>` measured their whole surface.  Audacity's
# .exe does NOT: it is a thin loader shell.  Measured with the loader's own PE
# parser (build/w32_peinfo):
#
#   Audacity.exe imports 6349 symbols across 34 modules --
#   EVERY module is a BUNDLED DLL (lib-*.dll + wxmsw313u_*_vc_x64_custom.dll),
#   every symbol a C++-mangled wx/Audacity name.  ZERO Windows system imports,
#   ZERO delay imports at the .exe level.
#
# So the honest gap is TRANSITIVE: the real Win32 surface lives inside the 117
# bundled DLLs.  Aggregated across the whole bundle (117 DLLs + the exe),
# de-duplicated per system module:
#
#   33 distinct Windows system modules, 1241 symbol imports.
#
# ---------------------------------------------------------------------------
# MET by the personality (14 modules, 822 symbols) — already REAL/FAIL-CLEAN
# ---------------------------------------------------------------------------
#   dll                symbols  class
    kernel32.dll          291   REAL
    user32.dll            216   REAL
    gdi32.dll             110   REAL
    ws2_32.dll             37   REAL
    advapi32.dll           31   REAL
    ole32.dll              25   REAL
    comctl32.dll           22   REAL
    shell32.dll            19   REAL
    uxtheme.dll            19   REAL
    oleaut32.dll           18   REAL
    comdlg32.dll            9   REAL
    shlwapi.dll             6   REAL
    version.dll             3   REAL
    crypt32.dll            16   FAIL-CLEAN

# ---------------------------------------------------------------------------
# GAP — Win32 system modules with NO personality (7 modules, 117 symbols)
# ---------------------------------------------------------------------------
#   dll             symbols  note
    winmm.dll            74   AUDIO + multimedia timers (waveOut/waveIn, timeSetEvent,
                              mmio, MIDI) — the anticipated first gap; Audacity is an
                              audio editor, this is its reason to exist
    wsock32.dll          31   legacy WinSock 1.1 (personality has ws2_32, not wsock32)
    rpcrt4.dll            4   RPC runtime (UuidCreate & friends)
    winspool.drv          3   print spooler (a §7 non-goal, like PrintDlgW)
    msimg32.dll           2   AlphaBlend / TransparentBlt / GradientFill
    oleacc.dll            2   Active Accessibility (MSAA)
    bcrypt.dll            1   CNG (BCryptGenRandom) — personality has crypt32 only

# ---------------------------------------------------------------------------
# GAP — the Universal CRT api-set (12 modules, 302 symbols)
# ---------------------------------------------------------------------------
# Audacity is built with MSVC 2015+ (vcruntime140/msvcp140/concrt140, bundled),
# whose C runtime is the UCRT reached through the api-ms-win-crt-* api-set.  The
# personality bridges only the LEGACY msvcrt.dll (W32A-13).  None of these exist:
    api-ms-win-crt-stdio-l1-1-0.dll         67
    api-ms-win-crt-math-l1-1-0.dll          55
    api-ms-win-crt-string-l1-1-0.dll        50
    api-ms-win-crt-runtime-l1-1-0.dll       33
    api-ms-win-crt-convert-l1-1-0.dll       23
    api-ms-win-crt-filesystem-l1-1-0.dll    21
    api-ms-win-crt-time-l1-1-0.dll          21
    api-ms-win-crt-locale-l1-1-0.dll        11
    api-ms-win-crt-heap-l1-1-0.dll           8
    api-ms-win-crt-environment-l1-1-0.dll    7
    api-ms-win-crt-utility-l1-1-0.dll        5
    api-ms-win-crt-conio-l1-1-0.dll          1

# TOTAL GAP: 19 modules, 419 symbols (7 Win32 + 12 UCRT api-set).

# ---------------------------------------------------------------------------
# LAUNCH ATTEMPT (real, in-guest) — the first fatal gap, measured live
# ---------------------------------------------------------------------------
# Audacity.exe staged into the initrd, booted under QEMU, `run /tests/audacity.exe`:
#
#   [shell] /tests/audacity.exe imports Win32 DLLs; running via /apps/w32run
#   [elf]  loaded 3 segment(s), entry 0x40074a00
#   [proc] entering Ring 3 at 0x40074a00
#   w32run: manifest: comctl v5, exec=asInvoker
#   w32run: too many relocations
#   [thread] '/apps/w32run' (tid 8) exited (code=1)
#
# FIRST FATAL GAP: the loader's fixed relocation buffer.  Audacity.exe carries
# 90932 base relocations (a 236 KB .reloc section); w32run maps them through a
# fixed `static pe_reloc_t relocs[16384]` (userspace/apps/w32run/w32run.c:167),
# so it dies at "too many relocations" BEFORE import resolution even begins.
# It never reaches the bundled-DLL graph or the 419-symbol system gap above --
# those are the SECOND and THIRD walls, established statically here.

# ---------------------------------------------------------------------------
# DECISION: OUTCOME B — Audacity does not run.  (W32APP_PLAN.md W32A-17)
# ---------------------------------------------------------------------------
# Three independent walls, each fatal on its own:
#   1. Scale: 90932 relocations vs a 16384-entry loader buffer (measured live).
#   2. Shape: a 117-DLL bundled application graph (wxWidgets 3.1.3 + ~60 lib-*
#      + FFmpeg/FLAC/...) — an order of magnitude past Notepad++'s one-exe gate.
#   3. Runtime: the Universal CRT api-set (302 symbols) the personality does not
#      bridge, plus winmm audio (74) — whole subsystems, not a handful of stubs.
#
# NEXT-PLAN SEED (this plan's §7 gains the measured line): Audacity is not the
# next rung of THIS ladder; it defines the boundary of it.  A future plan
# ("W32U" — the modern-runtime horizon) would need, in dependency order:
#   (a) a dynamic-scale loader (heap-allocated relocation/import tables + a
#       multi-DLL bundled-application module graph),
#   (b) a Universal CRT bridge (api-ms-win-crt-* -> the msvcrt engine already
#       built in W32A-13, re-exported under the api-set names),
#   (c) a winmm/WASAPI audio subsystem (the personality has no audio device at
#       all — a kernel-level dependency, not just a Win32 shim).
# Cheapest first, biggest payoff: (a) then (b); (c) is a separate audio plan.
#
# This gap ledger IS the phase's deliverable (Outcome B).  No personality .c
# code changed: an honest gap list beats a padded implementation of the wrong
# order of magnitude.
