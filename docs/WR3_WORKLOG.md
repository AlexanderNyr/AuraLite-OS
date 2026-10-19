# WR-3 worklog — Notepad++ live (2026-10-11 … 2026-10-19)

The pinned `notepad++.exe` 8.8.9 (`a470014b…`, 8365216 B) runs on real pixels:
frame, byte-exact typing, VFS open/save. Gate:
`tests/integration/cases/test_wr3_npp_live.sh`. Patch: `patches/WR3_npp_live.patch`.

## Fixes landed (11)

1. **fat32 `find_free_run` 0x00 barrier** (`kernel/fs/fat32.c`): a create whose
   tail free run sat past the first 0x00 wrote *behind* the terminator, so
   lookup never saw the entry (LFN creates and shell `>` lost files). The run
   must now start at/before the first 0x00; the grow path starts there.
   Proved by selftest case 5 (`GROWTEST`) + `[fat32] PASS`; `stylers.xml` and
   long names survive. Side effect: `/fat/w32trace.txt` started working,
   which unlocked every diagnosis below.
2. **WM_SIZE/WM_MOVE gating** (`w32/src/user32_win.c`): `ShowWindow` no longer
   emits `WM_SIZE` unconditionally and `SetWindowPos` only on real geometry
   change (`ever_shown`). The main `new 1 – Notepad++` frame appears (NPP was
   stuck in a show/hide layout loop every ~3 s).
3. **GDI window-origin translation** (`w32/src/w32_gdi.c`): every primitive
   translates logical→device (`device = logical − worg`); `RectVisible` moved
   to device units per MSDN (regions are device units). Live `worg` is 0, so
   this is latent hygiene, not a behaviour change.
4. **stdio/heap locks** (`lib/libc/src/{libc,malloc,stdio_extra}.c`,
   `w32/src/w32_seh.c`): recursive global stdio lock (owner TCB + depth),
   real `flockfile`, heap lock, malloc trace via raw `write`, w32trace via
   raw fd/`O_APPEND`. Killed the input-path `0xc0000005` crashes (file/heap
   races; `QM64`-style interleavings in the log). Verified: 106 keys, 6
   queues, two series, zero crashes.
5. **GDI object lock** (`w32/src/w32_gdi.c`): pool bump + object table +
   `DeleteObject` under a lock. Closed the `CreateBitmap`-`memcpy` crash from
   a lost bump update.
6. **`TCM_GETROWCOUNT = 1`** (`w32/src/comctl32.c`, header): we returned 0 via
   `DefWindowProc`, so TabBar computed `tabsHight = -4` and parked the editor
   over the tab strip. Editor lands at exactly `(2,48,1016,597)`.
7. **`GUI_OP_SET_HOLE`** (kernel `gui.c`/`gui_syscalls.*`, `libauragui`,
   tab `WM_SIZE`): the tab window is full-size by design (NPP parents the
   editor to the frame, not the tab), so its display area is cut transparent
   for pixels *and* clicks. Scintilla text shows through.
8. **`CreateBitmap` WORD stride** (`w32/src/w32_gdi.c`): source rows are
   WORD-aligned per MSDN (`((w*bpp+15)/16*2)`); our DWORD stride over-read
   Scintilla's 32-byte caret pack. One clean run + one fresh-heap run.
9. **Cooked keys → VK** (`w32/src/user32_win.c` `ui_kb_to_vk`): the compositor
   delivers cooked ASCII (`'e'` vs `'E'`, `'\n'`) + `KB_KEY_*` ≥ 0x100 (Shift
   is 0x120). Posting that verbatim as the VK dropped every lowercase letter
   (only digits/spaces, whose ASCII equals their VK, landed) and never set
   `keystate[VK_SHIFT]`. Full map now: letters, shifted symbols to their base
   position, OEM keys, arrows/nav/F1–F24/numpad. Unit cover in `test_w32_a5`.
10. **Delivery-time key state** (`ui_q_pop`): the pump can post a whole chord
    (Shift-down, key-down, key-up, Shift-up) before the first read, so
    post-time `keystate[]` feeds left `TranslateMessage` a cleared Shift —
    every fast shifted char arrived unshifted. Win32 feeds the thread state
    at retrieval; so do we. Chord-burst regression test in `test_w32_a5`.
11. **GETITEMA bounds** (`w32/src/comctl32.c` `a_to_a_buf`): tab/tree/header
    `*M_GETITEMA` copied `strlen+1` ignoring `cchTextMax`; listview wrote wide
    units into ANSI buffers. All four funnel through the bounded helper now.
12. **Standard-control style check** (`w32/src/user32_win.c` `stdctl`): the
    style low word is class-defined for Button/Edit/Static/ListBox/ScrollBar/
    ComboBox, so only the high word is validated for them. NPP's autocomplete
    ListBox popup (`LBS_NOINTEGRALHEIGHT`) was refused outright before this;
    creation must succeed for the popup to exist at all. Necessary, but — as
    fix 13 proved — not sufficient.
13. **GetMonitorInfo honours cbSize** (the actual killer,
    `w32/src/user32_win.c` + `w32/include/w32/user32.h`): our `W32_MONITORINFO`
    lacked the leading `cbSize` (all fields shifted by 4) and both
    GetMonitorInfo forms always filled the EX tail — 104 bytes into NPP's
    40-byte stack `MONITORINFO`, parked directly under its /GS cookie when
    AutoComplete positions the popup (MonitorFromPoint + GetMonitorInfoW).
    The `0xc0000409` at `npp+0x3d9289` was never a deliberate suicide: it is
    UCRT `__report_gsfailure`, reached through `__security_check_cookie`.
    Proved by disassembly (IAT `GetMonitorInfoW`, cbSize=0x28, struct ending
    exactly at the cookie slot) and a live gdbstub run ([RSP] at the abort
    entry = the AutoComplete epilogue). The popup now opens, typing survives,
    the edit saves byte-exact. The shared abort address with Find is
    explained: one `__report_gsfailure`, many smashers.

Plus: `tools/check_width_sweep.py` baseline 401→402 (one documented
`(uint64_t)-1` in `gui_syscalls.c`, pin-moves-code-stays precedent).

## Live evidence (runs 1–5, 1280x800, OVMF)

- Window `new 1 – Notepad++` + tab strip; no crash dialogs.
- `abc XYZ 123! -_ end` lands **byte-exact**, incl. shift chords.
- Open `C:/fat/WR3FIX.TXT` (argv): title + 28-char line paint.
- Edit + Ctrl+S: guest file is exactly
  `WR3-FIXTURE-LINE1-0123456789\n WR3GATE` — 37 B, `a829ed05…`
  (fixture alone: 29 B, `c5e474fd…`); `ls` size + `cat` lines agree.
- `test-unit` green; host `test_fat32` 39/39; guest `test_fat32_mkdir` 5/5
  + `test_fat32_full` 12/12; `test_w32` green except the pre-existing
  `test_w32a8_comctl32` TREEVIEW failure (identical on clean `1a6d7f0` —
  not a regression); provenance PASS; `test_wr3_npp_live.sh` 17/17 + 5 SKIP.

## Named gaps (D-WR4) with exact diagnoses

- **File > Open dialog**: NPP 8.8.9 never calls `GetOpenFileName` (consistent
  with the W32A-16 §2.3 note); Ctrl+O issues
  `CoCreateInstance(CLSID_FileOpenDialog dc1c5a9c-…)` → our zero-row COM table
  answers `REGDB_E_CLASSNOTREG` → no dialog. Needs an `IFileDialog` surface.
- **Find (Ctrl+F)**: the dialog builds (`[cw]` high-water 99), then NPP dies
  at the same `npp+0x3d9289` — now known to be UCRT `__report_gsfailure`
  (fix 13), so this is a second stack-cookie smash on the dialog path, not
  a NULL-handle abort; the `no class: msctls_trackbar32` line is incidental
  (a missing control NPP tolerates). The follow-up user `#PF` (inside w32run
  `syscall`) is teardown fallout, not the disease. Next step is the same
  recipe that caught fix 13: gdbstub break at the abort entry, [RSP] names
  the failing function. Separate session, not this phase.
- **Plugins / tray**: no DLL staged (D-WR3); tray never driven (NPP lights it
  only minimised-to-tray).
- **Open-path paint smear** (transient): the first line briefly shows a 4-char
  `WR3-` ghost at the margin's old x — a pre-margin paint surviving until the
  next line repaint erases it. Document bytes are correct (`cat` agrees);
  next step is a paint-rect trace, time-boxed out of this phase.

## Stand recipe (kept)

Boot plain → wait `auralite#` → `w32run /fat/notepad++.exe &` → 28 s →
`click --qmp 460 302` → 12 s → `click --qmp 400 300` → `type`/`key` → shot.
Guest reads: `serial-run --cmd "$(printf '\x15\x15cat …')" --secs N` (no
`--wait`; the PS/2 lane also feeds the shell, so a first `cat` can garble —
retry reads clean). `w32trc_*` writes `/fat/w32trace.txt`, never serial.
