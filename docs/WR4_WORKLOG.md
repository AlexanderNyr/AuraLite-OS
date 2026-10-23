# WR-4 worklog — PuTTY live (config dialog, terminal, live net)

Pinned app: PuTTY 0.85 Win64 `putty.exe`
(`d01fdb5aae8f112526040a39b0bfb9e27d813003178645e65f8d1cfdb2a26c87`,
1706136 B), staged via `$HOME/appbins` (D-WR3: pins only, never committed).
Base: `4450da7` (WR-3 PARTIAL landed).

## Fixes landed

1. **Builtin modules answer path-bearing loads** (`w32/src/w32_module.c`,
   `find_by_name`): builtin rows now also match the request's basename, so
   `LoadLibraryA("C:\win\sys\ws2_32.dll")` finds builtin `ws2_32` instead of
   falling through to disk (MOD_NOT_FOUND).  File rows keep the exact match.
   The twin (`w32/tests/w32a14_putty.c`) now also loads the
   GetSystemDirectoryA-built ws2_32 path — the shape that missed the live
   failure (below).

2. **DefDlgProc forwards unhandled messages to DefWindowProc**
   (`w32/src/w32_dlg.c`, `DefDlgProcW`): it used to return a blanket 0.
   PuTTY registers `PuTTYConfigBox` with `lpfnWndProc = DefDlgProc` (class
   proc, subclassed per-instance later), so the frame's WM_NCCREATE went to
   DefDlgProcA, the 0 refused creation, and both the attempt and the dialog
   engine's exstyle retry came back NULL (`hw=0x0`, no INITDIALOG, no
   children — measured live, lab5–lab9).  Forwarding matches the real
   DefDlgProc contract and the engine's own frameproc tail.

3. **Window-class names compare case-insensitively** (`w32/src/user32_win.c`,
   new `w16_ieq` used by `ui_find_class_w` + the comctl re-mark): PuTTY
   creates panel controls as `BUTTON`/`STATIC`/`EDIT` while the predefined
   classes register as `Button`/`Static`/`Edit`; the exact match refused
   every panel child with 1410 (black Session panel, only the exact-case
   `SysTreeView32` painted — measured live, lab10).  Message/property/title
   matches keep exact `w16_eq`.

## Live evidence

- **lab1 (2026-10-19, pre-fix):** `putty.exe` binds 348/348, enters WinMain,
  and raises its own "PuTTY Fatal Error" box: *"Unable to load any WinSock
  library"* (`build/integration-logs/gui-lane-shots/wr4_lab1_frame.png`).
  Serial: `open failed for "C:\win\sys\ws2_32.dll" (MOD_NOT_FOUND)`, then the
  `wsock32.dll` fallback, also `C:\win\sys\`-prefixed.  PuTTY builds the
  system-dir path itself (it imports GetSystemDirectoryA, which answers
  `C:\win\sys`); the W32A-14 twin only ever loaded the bare `"WS2_32.DLL"`,
  which is why the gate was green and the app was red.

- **lab2 (2026-10-19, post-fix):** the WinSock box is gone; PuTTY probes
  `hhctrl.ocx` / `winmm.dll` / `shcore.dll` / `secur32.dll` / `sspicli.dll`
  (all MOD_NOT_FOUND — optional features: help, sound, DPI, GSSAPI/SSPI)
  and then shows NOTHING — no config dialog, no error, no exit, no crash
  (`build/integration-logs/gui-lane-shots/wr4_lab2_frame.png`).  Under
  investigation via the w32trace + `info registers` (lab3).
- Resource recon (host `windres` on the pinned exe): the config dialog
  (id 102) carries `CLASS "PuTTYConfigBox"` and ZERO template controls —
  PuTTY builds its ~100 controls programmatically.  So the dialog engine
  only has to honour the custom frame class; the control-creation path is
  `CreateWindowExA` per control.

- **lab3/lab4 (2026-10-19):** PuTTY is ALIVE (`ps` shows `/apps/w32run`)
  and both vCPUs are in HLT — it is blocked in a kernel wait, not spinning
  and not exited (no `exited (code=…)` line).  `/fat/w32trace.txt` holds
  exactly 5 `[cw] high-water windows=` lines and NO `[wlist]` dump: PuTTY
  created 5 w32 windows and never showed a top-level one.  Diagnosis now
  runs through temporary `[wr4t]` serial probes (RegisterClass /
  CreateWindowExW / ShowWindow / DialogBox+CreateDialog ×A/Indirect /
  GetMessageW-first-12), to be removed before the phase patch.
- Method notes: HMP needs ONE persistent connection for `cpu 1` +
  `info registers` (`/tmp/hmp_multi.py`); QMP `query-cpus` is gone in
  QEMU 10 (`query-cpus-fast` has no PC).  `w32trace.txt` is written only
  by the `[wlist]` dump (first top-level ShowWindow), the SEH unwind path
  and the `[cw]` high-water — it is NOT a general API trace.  Never
  `pkill -f` with a pattern present in your own command line (the wrapper
  shell matches and suicides — kill strays by PID).  Never run parallel
  `edit_file` calls against the SAME file (they race: 2 of 4 probes landed
  plus a stray fragment at EOF that broke the build) — one python patch per
  file, or serialise across turns.

- **lab5–lab7 (2026-10-19):** PuTTY is not stuck — it runs its message loop
  (`GetMessageW` returns).  The config dialog (id 102, `PuTTYConfigBox`)
  fails: `CreateWindowExW` is entered twice (attempt + the dialog engine's
  exstyle retry) and the modeless frame comes back `hw=0x0`, so no
  `WM_INITDIALOG` ever runs and no child controls are created.  Dialog 111
  (`#32770`, same style bits) succeeds.  The w32trace holds only the 5
  high-water lines — none of the loud failure paths (`no class`,
  `rejected style`, `compositor refused`) fired and the PuTTY-class
  creations never claimed a slot — while every silent pre-claim failure
  (size/table/child-parent) is inapplicable on paper.  `lasterr` sampled in
  the dialog engine is stale (120/127 from PuTTY's own earlier calls), so a
  TEMP result+lasterr wrapper around `CreateWindowExW` now samples the error
  at the exact return (lab8).

## Watch items (observed, not yet needing a fix)

- Bare `"kernel32"` via the ps lane (`LoadLibraryW`/`LoadLibraryEx`, the two
  `[w32dll] LoadLibrary request: "kernel32"` lines in lab1) answers NULL —
  `ps_resolve_dll` only returns existing files, never builtins.  Harmless so
  far (delay-loads take the api-set shortcut); if a live app ever needs it,
  fall back to `w32_GetModuleHandleA` when resolution fails.

## lab12–lab14 — fix #4: MapDialogRect (ДОКАЗАН, работает)

lab12 (xy-зонд): дети панели создаются с ОТРИЦАТЕЛЬНЫМИ h (EDIT `xy=110,84 wh=154x-60` и т.д.).
lab13 (SetWindowPos-зонд): всего 1 SWP (фрейм) ⇒ PuTTY детей НЕ двигает после создания — creation rects финальны.
Загадка: на Windows эти rects работают ⇒ наш MapDialogRect врёт.
Источник истины: `putty-src.zip` 0.85 → `windows/controls.c:doctl`:
```c
r.left += cp->xoff;
MapDialogRect(cp->hwnd, &r);
ctl = CreateWindowEx(exstyle, wclass, wtext, wstyle,
                     r.left, r.top, r.right, r.bottom, ...);
```
PuTTY хранит (x, y, WIDTH, HEIGHT) в RECT и полагается на линейность MapDialogRect
(«nonstandard use of RECT... deliberate»). Рядом `windows/dialog.c` (дерево) хранит
CORNERS и вычитает (`r.right - r.left`).
Наш MapDialogRect делал `right = left + (right-left)*8/4`, top/left НЕ масштабировал:
- corners-конвенция случайно работала (w/h верные: дерево `190x438` ✓);
- width/height-конвенция давала отрицательные (EDIT h: `84 + (12-84)*2 = -60` ✓ сошлось).
Fix (w32_dlg.c): честный контракт — все 4 члена как координаты
(`left*8/4, top*16/8, right*8/4, bottom*16/8`, базы = GetDialogBaseUnits).
Внутренних вызовов MapDialogRect нет (только экспорт) — NPP не задет.
lab14: 0 отрицательных rects, NCCREATE fails 0. Кадр: диалог полного размера,
дерево со всеми категориями, геометрия панели — точная Session-панель PuTTY
(радиокнопки, EDIT host (220,168,264x24), groupbox (206,48,382x328), кнопки низа).
Остаток: правая панель БЕЛАЯ без контента — дети не рисуют содержимое
(Button/Edit/Static/ListBox красятся через DefWindowProc = только фон).
lab15: зонд WM_PAINT (кто красится) — в работе.

## Вайп №3 (2026-10-19) + восстановление

Снапшот снёс `.git`, `build/`, `~/.cargo`; worktree откачен СТАРЕЕ апстрима
(без WR-3 `stdctl` — чужое старье). Выжили: `docs/WR4_WORKLOG.md`,
`~/appbins` (putty sha ✓), apt-пакеты (qemu/mingw/llvm), `/tmp/wr4_lab*.sh`,
`/tmp/putty085` (исходники PuTTY).
Восстановление: fetch upstream → HEAD 4450da7 ✓ → `cp -a .git` →
`git checkout HEAD -- .` (только untracked worklog+WR3-патч остались) →
rustup 1.99.1 + 3 таргета → ре-апплай фиксов №1–4 + зонды (entry+wrapper) →
`make iso` PASS.
lab16: состояние lab14 воспроизведено точь-в-точь (0 негативов, 33 CW,
1 `=> 0x0 lasterr=8`, кадр идентичен).

## lab15–lab16 — белый void объяснён полностью (ДОКАЗАНО кодом)

- `w32_win_bg_color`: `has_bg==0 → 0x00FFFFFF` (БЕЛЫЙ). Все stdctl
  (Button/Edit/Static, hbrBackground=0) заливаются белым и НЕ рисуют контент
  (DefWindowProc WM_PAINT = только Begin/EndPaint). Панель = белые прямоугольники.
- Фрейм: PuTTY `shinydialogbox.c` регистрирует `PuTTYConfigBox` с
  `lpfnWndProc=DefDlgProc` (пруф корня fix#2 в сорцах) и
  `hbrBackground=COLOR_BACKGROUND+1` (тёмный) ⇒ чёрные зазоры между белыми детьми.
- Все дети красятся ровно 1 раз (зонд paint, lab15); дерево красится через comctl.
- CTLCOLOR: дефолт возвращает BTNFACE (серый), но stdctl родителя НЕ спрашивают
  (красятся через DefWindowProc) ⇒ протокол не работает. PuTTY CTLCOLORSTATIC
  обрабатывает только для hostkey-заголовка; конфиг получит серый по дефолту.
- Открыто: (a) STATIC (220,200,354x48) `=> 0x0 lasterr=8` — композитор НЕ отказывал
  (в serial нет `[gui]`-жалоб; слоты 256/256 свободны) ⇒ или тихий copy_title fail,
  или NCCREATE-путь со stale lasterr; (b) LISTBOX Saved Sessions НЕ создаётся
  вообще (в гистограмме классов его нет) — PuTTY должен создавать безусловно
  (`controls.c:CTRL_LISTBOX`) ⇒ разбираться; (c) fix#5 = рисование stdctl
  (static text, button frame+label, radio/check, groupbox прозрачный, edit
  text+caret+ввод, listbox) + CTLCOLOR-протокол.
lab17: titles в entry-зонде (карта контрол↔rect) — в работе.

## lab17 — titles: видимая панель = LOGGING, не Session!

Entry-зонд с titles показал: диалог 111 = About-бокс PuTTY (скрыт), а панель
конфига — Logging ("Session logging:", "Log file name:", "Browse..." и т.д.).
Дерево выбрало Logging, item'а Session нет вообще, LISTBOX не создаётся.
Падающий STATIC = "(Log file name can contain &Y, &M, &D for date, &T for
time, &H for host name, and &P for port number)" — 99 chars + NUL.

## lab18 — fix #5: длинные titles (ДОКАЗАН, верифицирован)

Корень падающего STATIC: `copy_string_from_user` возвращает -1, если в первых
GUI_TITLE_MAX=64 байтах нет NUL; title хинта — 100 байт ⇒ GUI_OP_CREATE → -1
⇒ `=> 0x0 lasterr=8`. Все остальные titles < 64 (проверено длинами).
Fix (`kernel/gui/gui_syscalls.c`, `copy_gui_title`): title ТРАНКИРУЕТСЯ
(первые 63 байта + NUL), вызов падает только на fault. Это намерение ядра
(`gui_create_window` уже делает strncpy-truncate); чинит и CREATE, и TITLE
(SetWindowText). lab18: НОЛЬ `=> 0x0` ✓.

## lab18 — корень #6: TVI_ROOT усечён до 32 бит (ДОКАЗАН)

tvins-зонд: PuTTY ВООБЩЕ не вставляет "Session" (первый инсерт — Logging),
все топ-уровни (Session/Terminal/Window/Connection) отсутствуют, дети — в корне.
Цепочка: наш `W32_TVI_ROOT=0xFFFF0000`, настоящий `TVI_ROOT=-0x10000`
(0xFFFFFFFFFFFF0000) ⇒ `==` в TVM_INSERTITEM промахивается ⇒ первый root-инсерт
("Session") REFUSED (INVALID_PARAMETER) ⇒ `lastat[0]=NULL` ⇒ `hfirst` остаётся
NULL ⇒ `firstpath` = вторая панель (Session/Logging) ⇒ конфиг открывается
на Logging; следующие инсерты идут с hParent=NULL(0) ⇒ в корень (наш код
трактует 0 как root). Один наш баг объясняет ВСЕ три аномалии (нет Session,
firstpath=Logging, дети в корне).
Fix #6 (`w32/include/w32/comctl32.h`): истинные значения commctrl.h для
TVI_ROOT/FIRST/LAST/SORT. lab19: ждём 'Session' первым + панель Session.
