# AuraLite OS — Win32 Applications Live-Run Plan (from *binds* to *runs*)

## Status: ACTIVE — the successor to `W32APP_PLAN.md`; closes the "human-run non-goals" that plan named, plus SHELL32 §7. WR-0 landed 2026-09-29 (the live-GUI lane is green); WR-1 landed 2026-09-29 (the SHELL32 §7 namespace is REAL, host gate green); WR-2…WR-5 planned.

| Phase | State |
|---|---|
| WR-0 The live-GUI lane and its oracles (OVMF + screendump + input harness) | ✅ done (2026-09-29) |
| WR-1 `SHELL32` namespace (§7 closed) — `IShellFolder`, desktop/drives/CFSFolder, PIDL | ⬜ planned |
| WR-2 App run I — 7-Zip File Manager: panel, navigate, extract, options, drag-drop | ⬜ planned |
| WR-3 App run II — Notepad++: Scintilla view, tabs, open/save, Find, plugins, tray | ⬜ planned |
| WR-4 App run III — PuTTY: config dialog, terminal render, live SSH/Telnet/Raw | ⬜ planned |
| WR-5 The honest live matrix — screenshot receipts, docs, the run/no-run table | ⬜ planned |

This document answers one question:

> *`W32APP_PLAN.md` proved that PuTTY, 7-Zip and Notepad++ **bind** — every
> import resolves to an honest body. What is the smallest honest path from
> "binds" to **runs**: each unmodified binary opening its real main window
> and doing its real work, measured on the real framebuffer, with the GUI
> gates that plan deferred as "human-run non-goals" made automatable?*

It is a sequel to [`W32APP_PLAN.md`](W32APP_PLAN.md) and inherits its method
wholesale: dependency-ordered phases, a definition of done and a test gate for
every phase, one `.patch` per phase, and the two-gate evidence model (fixture
gate + receipt gate). It adds a **third gate** — the *live-frame gate* — and
the harness that makes it automatable (WR-0). It inherits `WIN32_PLAN.md` §1
and decision D4 (the legal boundary) unchanged: the binaries stay test
subjects, never tree contents; only names, sizes and sha256 hashes are
committed.

**Baseline:** on top of `W32APP_PLAN.md` W32A-0 … W32A-18 (the full breadth
surface + the integration matrix). The persistent tree is the source of truth;
this plan changes runtime bodies, not the import census.

**What is new about the method — the measurement that commissioned this plan.**
On 2026-09-29 the pinned `7zFM.exe` (24.09, `dc4fdcd9…`, size 990 720) was
run for real, unmodified, on a live AuraLite boot:

- It **binds and executes**: `w32run: /fat/7zFM.exe mapped at
  0x400000000000, 292 import(s) bound`, entered Ring 3, ran its CRT startup.
- It **draws its own native window** — titled `7-Zip`, with a taskbar entry —
  over the kernel compositor, alongside two other live windows and the dock.
- It **then dies on `Error #80004005` (E_FAIL)** at startup, *before any file
  panel opens*, because `SHGetDesktopFolder` returns **E_NOTIMPL**
  (`w32/src/shell32.c:234`) — the shell namespace is deliberately unbuilt
  ("plan section 7"). 7-Zip needs the desktop `IShellFolder` to initialise
  its panels; it gets `E_NOTIMPL` and aborts.

Two facts fell out of that run and shape the whole plan:

1. **The GUI is only real under UEFI.** The BIOS Stage 2 loader sets no VBE
   mode, so `boot_get_framebuffer()` is `NULL`, the compositor renders into
   backing buffers that never reach the screen, and every BIOS-booted GUI is
   black (fb.c's own comment; the honest-skip precedent is
   `test_gui_dirty_uefi.sh`). Booting OVMF gives a GOP linear framebuffer
   (1280×800) and the desktop appears. **Every live-frame gate in this plan
   boots OVMF.** This is what `W32APP_PLAN.md` meant by "no framebuffer/pixel
   oracle in this environment" — it is no longer true, and WR-0 makes it a
   lane.

2. **`binds ≠ runs`.** The app-ledger marks `SHGetDesktopFolder` `REAL
   static-only`; the fixture gate was green; W32A-15 was "done". All true —
   at the *binding and fixture* level. The **runtime body** was a stub. This
   plan is exactly the set of runtime bodies the fixture gates could not see.

**Evidence model (three gates now).**

- **Fixture gate** — mingw-w64-built PEs, committed, in CI (inherited).
- **Receipt gate** — the user runs the pinned binary, pastes the log, the
  receipt names the sha256 (inherited; `docs/w32app_receipts.md`).
- **Live-frame gate** *(new, WR-0)* — an OVMF boot drives the pinned binary
  through the serial shell, captures the framebuffer via the monitor
  `screendump`, and asserts an *oracle* over the pixels: a brightness floor
  (not-black), a window-chrome/title match, and a content delta after an
  injected action. Fixtures prove the mechanism; receipts prove the
  application; the live frame proves it **on real pixels**. None is accepted
  as proof of another.

---

## 1. The legal boundary (inherited, unchanged)

`WIN32_PLAN.md` §1 and `W32APP_PLAN.md` §1 stand verbatim. No Microsoft code
or SDK headers; no Wine/ReactOS as source *or reference*; declarations from
mingw-w64 headers and published documentation only; subsystem stays `w32`. The
binaries are obtained by the user from their publishers, are **never
committed**, and reach the guest through the `/fat` delivery disk (§3.3), not
the tree. Only version, size, sha256 and import ledgers are committed —
`tools/check_provenance.sh`'s `MZ`+`PE\0\0` rule is the backstop.

The shell namespace built in WR-1 is written from the documented `IShellFolder`
/ `IEnumIDList` interface contracts (MSDN) and mingw-w64's `shobjidl`/`shlobj`
headers — interface *shapes*, which are facts about an ABI, the same class of
fact `docs/win32.md` already records. No shell32 implementation is consulted.

---

## 2. Where things stand — the measured run/no-run frontier

### 2.1 The live run of the ladder (2026-09-29)

| App | Binds? | Enters Ring 3? | Draws a window? | Reaches its main UI? | Blocker |
|---|---|---|---|---|---|
| 7-Zip FM 24.09 | ✅ 292/292 | ✅ | ✅ (`7-Zip` error box) | ❌ | `SHGetDesktopFolder` E_NOTIMPL → E_FAIL before panels |
| Notepad++ 8.8.9 | ✅ 590/590 | — (not yet live-run) | — | — | to be measured under WR-0/WR-3 |
| PuTTY 0.85 | ✅ 348/348 | — (not yet live-run) | — | — | GUI config + live net, deferred human-run in W32A-14 |

The 7-Zip row is a *measured* live boot; the other two rows are the
fixture/binding state from `W32APP_PLAN.md`, not yet exercised on the OVMF
framebuffer. WR-0 exists to turn every `—` above into a ✅/❌ with a pixel.

### 2.2 The SHELL32 §7 gap, by measurement

7-Zip's committed ledger (`w32/app_ledger/7zFM-24.09.imports`) lists the
shell-namespace surface it actually imports — all currently marked
`REAL static-only`, i.e. **bound**, but several backed by an `E_NOTIMPL` /
`ERROR_INVALID_PARAMETER` stub body:

| Import | Current body | WR-1 target |
|---|---|---|
| `SHGetDesktopFolder` | **E_NOTIMPL** (`shell32.c:234`) — *the fatal one* | real desktop `IShellFolder` |
| `SHGetFileInfoW` (PIDL flag) | `ERROR_INVALID_PARAMETER` (`shell32.c:517`) | PIDL → name/icon/attrs |
| `SHGetSpecialFolderLocation` | stub | real PIDL for CSIDL roots |
| `SHGetSpecialFolderPathW` | stub | real path for CSIDL roots |
| `SHGetPathFromIDListW` | partial (`shell32.c:208`) | round-trips WR-1 PIDLs |
| `SHBrowseForFolderW` | stub | folder-picker over the namespace |
| `SHChangeNotify` | stub (no-op ok) | documented no-op, named |
| `ExtractIconExW` | present | icons for the panel |

The interfaces those entry points must actually hand back (from the 7-Zip
panel's use, MSDN-documented): `IShellFolder` (`BindToObject`, `EnumObjects`
→ `IEnumIDList`, `GetDisplayNameOf`, `GetAttributesOf`, `ParseDisplayName`,
`CompareIDs`), the PIDL helpers 7-Zip reaches through its own copies
(`ILClone`/`ILCombine`/`ILFree`/`ILIsEqual` — 7-Zip carries these, so only the
byte layout must agree with `SHGetPathFromIDListW`), and a `My Computer` root
enumerating one drive, `C:`, that binds to a **CFSFolder** enumerating the
AuraLite VFS via `FindFirstFileW`/`FindNextFileW` (already REAL in
`kernel32_fs.c`). This is the whole of WR-1.

### 2.3 What "runs" costs beyond "binds", per app

- **7-Zip**: the namespace (WR-1) is the hard dependency. After it, the panel
  is `SysListView32` (REAL, W32A-8) fed by `IEnumIDList`; extract is `7z.dll`
  (`CreateObject`, REAL DLL-chain, W32A-15) writing through the VFS; options
  is a `PropertySheetW` over the registry (REAL, W32A-9). No new kernel work.
- **Notepad++**: Scintilla is a self-contained custom control that paints via
  `GDI32` (REAL, W32A-7) into a child `HWND` — the risk is *paint fidelity*
  (fonts, carets, scrollbars), not missing imports. Tabs/toolbar are
  COMCTL32 (REAL). Find is a modeless `DialogBox` (REAL, W32A-6). Plugins are
  `LoadLibraryW` of the pinned NPP DLLs (REAL DLL-chain). Tray is
  `Shell_NotifyIconW` (present). WR-3 measures which of these paint correctly.
- **PuTTY**: the config dialog is a `DialogBox` tree (REAL) whose persistence
  half already passes headlessly (W32A-14); WR-4 adds the *pixel* half. The
  terminal is a GDI text grid. Live SSH/Telnet/Raw run over `WS2_32` (REAL,
  W32A-12) on QEMU user-net SLIRP against a server WR-0 provisions.

---

## 3. The method — the live-GUI lane (this is WR-0, stated once)

### 3.1 The boot line that has pixels

```
qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=<writable copy of>OVMF_VARS_4M.fd \
  -drive file=<auralite.iso>,format=raw,if=ide,snapshot=on \
  -drive id=dapp,file=<app.img>,format=raw,if=none,snapshot=on \
  -device ahci,id=ahci -device ide-hd,drive=dapp,bus=ahci.0 \
  -m 512M -smp 2 -cpu qemu64 -no-reboot -no-shutdown -boot order=c \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -fw_cfg name=opt/auralite.selftest,string=full \
  -vga std -display none \
  -serial unix:/tmp/ser.sock,server,nowait \
  -monitor unix:/tmp/mon.sock,server,nowait
```

The three lessons this line encodes, each a dead end already paid for:
- **OVMF, not SeaBIOS** — BIOS has no linear framebuffer (§0). Under TCG the
  UEFI path reaches `auralite#` in ~40–60 s; the gate budgets 90 s.
- **The ISO is a hybrid MBR disk, not optical** — attach `if=ide,snapshot=on`
  with `-boot order=c`. Attaching it `media=cdrom` drops OVMF to the UEFI
  Shell with no `FS` mapping (measured). OVMF then enumerates the FAT32
  partition and runs `/EFI/BOOT/BOOTX64.EFI` (`mkisoimage_dual.sh` §UEFI).
- **`-vga std` + `-display none`** still lets the monitor `screendump` the
  framebuffer; a headless box needs no VNC for the capture.

### 3.2 The oracle (`screendump` → PPM → assert)

The monitor `screendump /abs/path.ppm` writes the live GOP framebuffer even
with `-display none`; PIL converts and measures. The three assertions:

- **brightness floor** — mean luma > 10 (a black frame fails; this is the gate
  W32APP §GUI could only *skip* on the BIOS lane).
- **window oracle** — the target window's title-bar band matches expected
  chrome (colour histogram of the title strip + a glyph-free template match on
  the close-box), so "a `7-Zip` window exists" is a pixel fact, not a log line.
- **content delta** — after an injected action, `ImageChops.difference` has a
  non-empty bbox in the panel region (the listing actually changed).

### 3.3 Delivery and input

- **App delivery**: a FAT image (`mformat -F`, 64 zero sectors of MBR slack,
  `mcopy` the pinned `.exe`/`.dll`) attached as the first AHCI disk → the
  guest sees `/fat`. Never the initrd, never the tree (§1). This is proven.
- **Command input**: the serial unix socket; `run w32run /fat/<app>.exe
  [args]` — note the shell's w32run auto-route drops extra argv
  (`init.c:851`), so a *folder/file argument* must go through `w32run`
  explicitly (measured: `run w32run /fat/7zFM.exe C:\fat`, where `C:\` maps to
  the VFS root per `kernel32_fs.c`).
- **GUI input**: the monitor `sendkey` (e.g. `ret`, `tab`, `a`…) and
  `mouse_move`/`mouse_button` inject keyboard and PS/2 mouse events into the
  live desktop — the missing half that made W32APP's UI flows "human-run".
- **Serial hygiene**: filter the buffer-cache `[bc] writeback…` spam.

#### WR-0 Tasks

- [x] `tests/integration/lib/gui_lane.sh` — the OVMF boot helper: pflash
      copy, disk attach (`GL_APP_DISK`), serial+monitor sockets, a `usb-tablet`
      for absolute clicks, the boot budget, and `gl_boot`/`gl_send`/`gl_cmd`/
      `gl_run_w32`/`gl_shot`/`gl_key`/`gl_type`/`gl_click`/`gl_stop`.
- [x] `tools/fb_oracle.py` — `brightness`, `region-brightness`,
      `band-brighter`, `find-color` (title bar / close box / selection), and
      `delta <region>` over a `screendump` PPM/PNG; exit codes drive the gate.
- [x] `tools/gui_input.py` — the sole socket talker: serial
      send/wait/run (filters the `[bc]` spam) + monitor `sendkey`/`type`/
      `screendump`/`mouse`, with a settle poll on the framebuffer.
- [x] `tests/integration/cases/test_gui_lane_smoke.sh` — boots OVMF, asserts
      `GOP framebuffer located` + `auralite#`, captures the desktop, asserts
      brightness > 10 and the taskbar/title-bar chrome oracle, and proves the
      injected-input + re-capture path. The floor `test_gui_dirty_uefi.sh`
      could only skip on BIOS — now a positive gate.
- [x] The reference frame committed as a **digest** (oracle values, not the
      image — same rule as binaries: no foreign bytes; a measurement of *our*
      framebuffer is our own output): the WR-0 table in `docs/gui_receipts.md`.
- [x] Registered in `tests/integration/run_all.sh` (`ALL_CASES`, the `gui`
      shard, and `SLOW_CASES_RE`) — `check_test_registry.py` + `--check-groups`
      green.

#### WR-0 Test gate

- `test_gui_lane_smoke.sh` green (**measured 2026-09-29, 9/9**): OVMF boots,
  GOP present, desktop non-black, taskbar + title-bar chrome oracles match,
  serial input live, monitor re-capture live. The harness is reusable by
  WR-2…WR-4 unchanged. Receipt: `docs/gui_receipts.md` §WR-0.

**Deliverable:** `tests/integration/lib/gui_lane.sh`, `tools/fb_oracle.py`,
`tools/gui_input.py`, `tests/integration/cases/test_gui_lane_smoke.sh`,
`patches/WR0_gui_lane.patch`, and a `docs/gui_receipts.md` protocol stub in
the style of `docs/w32app_receipts.md`.

---

## 4. Phases

### Phase WR-1 — `SHELL32` namespace (§7 closed): `IShellFolder`, drives, CFSFolder, PIDL ✅

_Landed 2026-09-29. Host gate green (`tests/unit/test_shell_ns.c`, 45 checks,
ASan/UBSan, Find handles balanced 9/9); provenance + test-registry gates green;
`shell32.c`/`shell32_ns.c` compile clean. The mingw fixture twin and the full
`w32run.elf` link are toolchain-gated (no cross-compiler / `llvm-rc`+`rustc` in
this environment) and run when those are present — the fixture loud-SKIPs
otherwise, exactly like the W32A-2…A-16 guest fixtures._

**Objective:** replace the `E_NOTIMPL`/`ERROR_INVALID_PARAMETER` shell-namespace
stubs with a real, minimal, honest namespace — the exact surface §2.2 measured
7-Zip to need — so `SHGetDesktopFolder` hands back a working desktop
`IShellFolder` rooted at `My Computer → C: → CFSFolder(VFS)`.

This is the one hard-dependency phase; WR-2 cannot start without it. It is
kept minimal by measurement (D-WR1: implement the 7-Zip ledger surface, no
more) and by reuse — the file enumeration is `FindFirstFileW` (REAL), the
list control is `SysListView32` (REAL); only the COM object graph is new.

#### Tasks

- [x] `IShellFolder` vtable in a new `w32/src/shell32_ns.c`:
      `QueryInterface`/`AddRef`/`Release`, `EnumObjects` → `IEnumIDList`,
      `BindToObject`, `GetDisplayNameOf`, `GetAttributesOf`,
      `ParseDisplayName`, `CompareIDs` — the documented subset the panel calls.
      All 13 vtable slots present in the documented order; the GUI-object verbs
      (`CreateViewObject`/`GetUIObjectOf`/`BindToStorage`/`SetNameOf`) fail
      clean `E_NOTIMPL`, named (D-WR4).
- [x] The desktop root: `SHGetDesktopFolder` returns the desktop folder whose
      `EnumObjects` yields one child, `My Computer`, whose `EnumObjects`
      yields one drive, `C:`.
- [x] `CFSFolder`: binding `C:` (or any filesystem PIDL) yields a folder whose
      `EnumObjects` walks the VFS via `FindFirstFileW`/`FindNextFileW`, and
      whose `GetDisplayNameOf` (`SHGDN_FORPARSING`) round-trips through
      `ns_pidl_to_path` (the same bytes `SHGetPathFromIDListW` decodes).
- [x] The PIDL byte layout: byte-compatible with the existing single-item pair
      (`{ u16 cb, u16 kind, u16 units, path, u16 0, u16 0 }`) so the in-tree
      `IL*`/`SHGetPathFromIDListW` see the same bytes. Unit-tested both
      directions.
- [x] `SHGetFileInfoW` PIDL branch: resolve PIDL → path, then reuse the
      by-path body for name/type/icon/attrs (the `ERROR_INVALID_PARAMETER`
      early-out is gone; the virtual "Computer" root answers name/type/attrs
      without a filesystem path).
- [~] `SHGetSpecialFolderLocation` / `SHGetSpecialFolderPathW`: already REAL
      from W32A-10 (the CSIDL PIDL/path pair); no change needed for the
      measured 7-Zip surface, so left as-is (D-WR1: measured surface only).
- [~] `SHBrowseForFolderW` / `SHChangeNotify`: already REAL from W32A-10;
      untouched — not on the WR-1 critical path.
- [x] Host unit test `tests/unit/test_shell_ns.c` — enumerate desktop → My
      Computer → C: → a scripted VFS tree, assert names/attrs/PIDL round-trip,
      enum Next/Skip/Reset, fail-clean verbs, and Find-handle balance, under
      ASan/UBSan (45 checks, 0 failures).
- [x] Fixture twin `tests/integration/cases/test_wr1_shell_namespace.sh` +
      guest `w32/tests/wr1_shellns.c` — a mingw-w64 PE that calls
      `SHGetDesktopFolder`, walks the graph through the COM vtable and asserts
      each step, printing `WR1-SHELLNS-OK`/exit 78; registered in `run_all.sh`,
      loud-SKIPs when the cross-compiler is absent.

#### Test gate

- Host unit test green under ASan/UBSan (45 checks); provenance and
  test-registry gates green (210 cases); `shell32.c`/`shell32_ns.c` compile
  `-Wall -Wextra -Werror` clean. Census gap does not grow (`SHGetDesktopFolder`
  was already a ledger symbol — it changed from stub to REAL, no new import).
  Fixture twin green in-guest and the full `w32run.elf` link are toolchain-gated
  (mingw / `llvm-rc`+`rustc`), run when those are installed. No live-frame gate
  yet — WR-1 is headless COM.

**Deliverable:** `w32/src/shell32_ns.c` (+ `shell32.c`/`shell32.h` edits),
`w32/include/w32/shell32_priv.h`, `tests/unit/test_shell_ns.c`,
`w32/tests/wr1_shellns.c`,
`tests/integration/cases/test_wr1_shell_namespace.sh`, Makefile wiring,
`patches/WR1_shell_namespace.patch`.

---

### Phase WR-2 — App run I: 7-Zip File Manager, live on the framebuffer

**Objective:** the pinned `7zFM.exe`+`7z.dll` (24.09, `dc4fdcd9…`/`88206394…`)
open their **real main window** — the two-pane file manager — list `/fat`,
navigate, extract a known archive byte-exactly, and persist an option. The
E_FAIL of §0 is gone; the frame shows the listing.

#### Tasks

- [ ] Live launch: `gui_run 7zFM.exe C:\fat` reaches the main window (no
      `Error #80004005`); serial shows the panel init completing.
- [ ] **Live-frame gate**: `window_present` matches the 7-Zip main window
      chrome (not the message box); brightness floor; the panel region shows
      `SysListView32` rows for `/fat` (`7zFM.exe`, `7Z.DLL`, `README.TXT`, …).
- [ ] Navigate: inject a double-click / `ret` on `SUBDIR`, assert a content
      delta (the listing changed) and a back-nav returns.
- [ ] Extract: point 7-Zip at a **byte-known** committed-fixture `.7z`/`.zip`
      (built in CI from in-tree sources, per §1), extract-all through the VFS,
      `sha256` the output against the known plaintext — the first *headless*
      extract proof (was human-run in W32A-15).
- [ ] Options: toggle a setting via the `PropertySheetW`, close/reopen,
      assert the registry round-trip *and* the UI reflects it (pixel delta).
- [ ] Drag-drop add: inject a mouse drag; assert `SHFileOperationW` fired and
      the archive grew (or document as human-run if the drag source needs a
      second live window — named either way).
- [ ] Update the `#7zip` receipt in `docs/w32app_receipts.md`: the human-run
      non-goals W32A-15 named that are **now automated** move to the
      live-frame gate, with the frame digest; anything still human-run stays
      named.

#### Test gate

- `test_wr2_7zip_live.sh` green under the WR-0 lane: main window present,
  `/fat` listed, navigate delta, byte-exact extract of the CI fixture archive,
  option round-trip. Receipt updated; frame digest committed.

**Deliverable:** `tests/integration/cases/test_wr2_7zip_live.sh`, the CI
fixture archive builder, receipt edits, `patches/WR2_7zip_live.patch` (any
personality fixes the live run forces — e.g. panel-init ordering).

---

### Phase WR-3 — App run II: Notepad++, live on the framebuffer

**Objective:** the pinned `notepad++.exe` (8.8.9, `a470014b…`) opens its
**real editor window**, shows a Scintilla view, opens and saves a file through
the VFS, runs Find, loads a pinned plugin, and shows a tray icon — every W32A-16
"human-run" UI flow, now on real pixels.

#### Tasks

- [ ] Live launch: `gui_run notepad++.exe` reaches the editor window; serial
      shows Scintilla registered and the frame laid out (`BeginDeferWindowPos`
      batch, REAL since W32A-16).
- [ ] **Live-frame gate**: `window_present` on the Notepad++ frame; the
      Scintilla client region is non-empty; the tab bar (COMCTL32) is present.
- [ ] Type + render: inject text via `gui_input type`, assert a content delta
      in the Scintilla region (the caret advanced, glyphs painted) — the
      GDI text-paint fidelity check.
- [ ] Open/Save: open a committed fixture file from `/fat`, edit, save,
      `sha256` the saved bytes against expected — headless open/save proof.
- [ ] Find: open the Find dialog (`DialogBox`, REAL), search a known string,
      assert the match-count status / selection delta.
- [ ] Plugins: `LoadLibraryW` a pinned NPP plugin DLL (delivered on `/fat`),
      assert it appears in the Plugins menu (menu-item oracle) or document
      the exact failure if a plugin needs an unbuilt surface.
- [ ] Tray: `Shell_NotifyIconW` places an icon; assert the taskbar/tray
      region delta (or name it human-run if the tray is off-frame).
- [ ] Receipt `#notepad++` in `docs/w32app_receipts.md` updated to the
      live-frame gate; measured-not-run items named honestly.

#### Test gate

- `test_wr3_npp_live.sh` green: editor window present, Scintilla paints typed
  text, fixture file open/save byte-exact, Find match, plugin loaded (or named
  gap). Receipt + frame digest committed.

**Deliverable:** `tests/integration/cases/test_wr3_npp_live.sh`, fixture
file + pinned-plugin delivery, receipt edits, `patches/WR3_npp_live.patch`.

---

### Phase WR-4 — App run III: PuTTY, live on the framebuffer + live network

**Objective:** the pinned `putty.exe` (0.85, `d01fdb5a…`) opens its **real
config dialog**, saves/loads a session through the UI (not just the registry),
opens a terminal, and completes a **live** Telnet/Raw and SSH session against
a server the WR-0 lane provisions over QEMU user-net SLIRP — every W32A-14
"human-run" flow, automated.

#### Tasks

- [ ] Live launch: `gui_run putty.exe` reaches the config dialog; the
      dialog tree renders (`window_present` + control-region oracle).
- [ ] Session UI round-trip: type a host/port in the dialog, `Save`, close,
      reopen, `Load`, assert the fields repopulate (pixel + registry, the
      W32A-14 headless half now joined to its UI half).
- [ ] Terminal render: connect Raw/Telnet to an **in-lane echo/telnet server**
      (spawned by `gui_lane.sh` on the SLIRP host side, reachable at
      `10.0.2.2`), type a line, assert the echoed text paints in the terminal
      grid (GDI text delta). First headless PuTTY net+render proof.
- [ ] SSH: connect to an in-lane SSH server, complete auth (password or the
      pinned test key), run a command, assert its output paints. If the crypto
      surface (`CryptoAPI`/`bcrypt`) has a gap PuTTY's SSH needs, name the
      exact symbol and its status — do not fake a pass.
- [ ] Font/colour choosers + clipboard: exercise the COMDLG32 six and `gclip`
      round-trip via injected input, or name the residual human-run item.
- [ ] Serial backend stays fail-clean (COMM set FALSE + `ERROR_INVALID_FUNCTION`,
      already proven) — re-assert from the live process.
- [ ] Receipt `#putty` in `docs/w32app_receipts.md` updated to the live-frame
      + live-net gate; the auth-fallback / crypto items named honestly.

#### Test gate

- `test_wr4_putty_live.sh` green: config dialog present, session UI round-trip,
  Telnet/Raw echo painted, SSH command output painted (or a named crypto gap).
  Receipt + frame digest committed.

**Deliverable:** `tests/integration/cases/test_wr4_putty_live.sh`, the in-lane
server provisioning, receipt edits, `patches/WR4_putty_live.patch`.

---

### Phase WR-5 — The honest live matrix

**Objective:** one table that tells the truth about *running*, and the docs
that carry it — the live counterpart to `W32APP_PLAN.md` W32A-18's binding
matrix.

#### Tasks

- [ ] `docs/win32_run.md` (generated, like `docs/win32.md`): per app, the
      run-state matrix — *binds / enters / draws / main UI / core task /
      live net* — each cell backed by a gate name or a named human-run reason.
- [ ] `docs/gui_receipts.md` finalised: the screendump protocol, the frame
      digests, the sha256 pins, one section per app.
- [ ] `README.md` + `docs/status.md` updated: the "runs, measured on pixels"
      claim replaces the "binds" claim, with the gate names as evidence.
- [ ] `tools/check_w32app_claims.py` (or a sibling) extended: a WR phase
      section, a run-matrix consistency check (no cell claims a state its
      gate does not prove), a REFUSE-guard for faked live passes.
- [ ] The plan's own status table (top) flipped to ✅ with dates and receipts.

#### Test gate

- The claim checker is green; every ✅ in `docs/win32_run.md` resolves to a
  green gate or a named human-run reason; no orphan claims; `make test`
  selective run recorded (the OVMF lanes are slow under TCG — the gate names
  which ran).

**Deliverable:** `docs/win32_run.md`, `docs/gui_receipts.md`, checker edits,
doc edits, `patches/WR5_live_matrix.patch`.

---

## 5. Decisions

- **D-WR0 — OVMF is the only GUI truth.** Every live-frame gate boots OVMF;
  BIOS GUI is black by construction and is never a GUI oracle. The
  BIOS-lane skip in `test_gui.sh`/`test_gui_dirty_uefi.sh` stays as the
  documented precedent.
- **D-WR1 — the namespace is measured, not general.** WR-1 implements exactly
  the `IShellFolder`/PIDL surface the 7-Zip ledger names (§2.2), rooted at
  `My Computer → C: → CFSFolder(VFS)`. No shell extensions, no network
  namespace, no virtual junctions — those fail clean and are named.
- **D-WR2 — `binds` is not `runs`, and the receipt says which.** A green
  fixture/binding gate is never restated as a run claim. The run/no-run
  matrix (WR-5) is the only place a "runs" claim lives, and every cell cites
  a live gate.
- **D-WR3 — no foreign bytes, still.** Binaries reach the guest on `/fat`,
  never the tree; only names/sizes/hashes and *our own* framebuffer digests
  are committed. `check_provenance.sh` stays the backstop.
- **D-WR4 — name the gap, never fake the pass.** Where a live run hits an
  unbuilt surface (a crypto symbol, a shell extension, an off-frame tray),
  the exact symbol/reason is named in the receipt and the matrix; the gate
  fails or is marked human-run, never silently greened.

---

## 6. Appendix — the reproduction recipe (hard-won, kept verbatim)

- **Toolchain**: `source "$HOME/.cargo/env"` then `make iso -j2` (multiarch:
  needs the three rustup targets `x86_64-unknown-none`,
  `riscv64gc-unknown-none-elf`, `aarch64-unknown-none`; Rust via rustup only,
  never distro `rustc`).
- **OVMF**: `/usr/share/OVMF/OVMF_CODE_4M.fd` + a writable copy of
  `OVMF_VARS_4M.fd`.
- **App disk**: `mformat -i img -F -h 32 -s 32 -t 128 ::` + 64 zero sectors of
  MBR slack (`dd count=64` then `cat`) + `mcopy` the pinned files → guest
  `/fat`.
- **Boot line**: §3.1. **Capture**: monitor `screendump /abs.ppm` → PIL PNG.
  **Drive**: serial `run w32run /fat/<app>.exe [C:\path]`; GUI input via
  monitor `sendkey`/`mouse_move`/`mouse_button`. **Filter** the `[bc]` serial
  spam. **Budget** ≥ 90 s to the shell under TCG (no KVM in this environment).
- **Path mapping**: Win32 `C:\` = VFS `/` (`kernel32_fs.c`); one drive only.
- **Gotcha**: the shell's w32run auto-route drops extra argv (`init.c:851`);
  pass file/folder arguments through `w32run` explicitly.
