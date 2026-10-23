# GUI live-frame receipts

Receipt protocol for `docs/plans/W32RUN_PLAN.md` (the W32RUN ladder, WR-0…WR-5).
A receipt is greppable evidence a claimed thing exists and was measured. For
this plan the new class of evidence is a **live frame**: an OVMF boot captures
the GOP framebuffer with the QEMU monitor `screendump`, and `tools/fb_oracle.py`
asserts over its pixels. Prose is not a receipt; a green gate and a captured
frame are.

This mirrors `docs/w32app_receipts.md` (the binding/fixture receipts) and
`docs/metal_receipts.md` (the paste-back protocol). The three gates of the
plan (WR-0 §Evidence model):

- **fixture gate** — mingw-w64 PEs, committed, in CI (inherited from W32APP);
- **receipt gate** — the user runs the pinned binary, pastes the log, the
  receipt names the sha256 (inherited);
- **live-frame gate** — the OVMF lane drives the binary and asserts pixels.

No foreign bytes enter the tree: the app binaries are USER-SUPPLIED and reach
the guest on the `/fat` delivery disk (`gl_make_fat_disk`), never committed.
What *may* be committed as a receipt is a digest of **our own** framebuffer —
a histogram or a hash — because that is our output, not the vendor's.

## Why the GUI needs UEFI (the measurement behind the whole plan)

The BIOS Stage 2 loader sets no VBE mode, so `boot_get_framebuffer()` is
`NULL`, the compositor renders into backing buffers that never reach the
screen, and every BIOS-booted GUI is black (fb.c's own comment; the
honest-skip precedent is `test_gui_dirty_uefi.sh` / `test_gui.sh`). Booting
OVMF gives a GOP linear framebuffer (1280×800 in this environment) and the
desktop appears. **Every live-frame gate boots OVMF** (`W32RUN_PLAN` D-WR0).

## The lane (WR-0)

| artefact | what it is |
|---|---|
| `tests/integration/lib/gui_lane.sh` | boots AuraLite under OVMF with serial + monitor UNIX sockets; `gl_boot` / `gl_send` / `gl_cmd` / `gl_run_w32` / `gl_shot` / `gl_key` / `gl_type` / `gl_click` / `gl_stop` |
| `tools/gui_input.py` | the only talker to the sockets: serial send/wait/run (filters the `[bc]` buffer-cache spam) and monitor `sendkey` / `type` / `screendump` / `mouse` |
| `tools/fb_oracle.py` | pixel assertions: `brightness`, `region-brightness`, `band-brighter`, `find-color`, `delta` |
| `tests/integration/cases/test_gui_lane_smoke.sh` | the WR-0 gate |

### WR-0 receipt — live-lane smoke

Gate: `tests/integration/cases/test_gui_lane_smoke.sh` (group `gui`, on
`SLOW_CASES_RE` — OVMF boots in ~1 min under TCG). Measured 2026-09-29,
**9/9 assertions**:

```
== GUI live-lane smoke (W32RUN_PLAN WR-0) ==
  [gui-lane] OVMF booting (TCG; up to 100s to the shell)...
serial-wait: matched /auralite#/
  ✔ UEFI GOP framebuffer present
  ✔ reached the shell
  ✔ GUI subsystem self-test passed
  ✔ captured the desktop framebuffer
  ✔ desktop is not black (brightness floor cleared)
  ✔ taskbar band present (structurally brighter than the desktop)
  ✔ a window title bar is present on the desktop
  ✔ serial input path is live
  ✔ monitor re-captured after injected input (input harness live)
── 9/9 assertions passed ──
```

The captured frame's oracle values on the 1280×800 GOP framebuffer (the
committable digest — our own pixels, not a vendor's):

| oracle | value | gate |
|---|---|---|
| `brightness` (whole frame) | mean ≈ 44 | > 10 (not black) |
| `band-brighter` taskbar `0,772..1280,800` vs desktop `0,380..1280,420` | Δ ≈ 35 | ≥ 8 |
| `find-color` title bar blue `(43,87,151)` in `0,60..1280,130` | frac ≈ 0.05 | ≥ 0.02 |

### Reproducing WR-0

```
source "$HOME/.cargo/env"      # rustup only; never distro rustc
make iso -j2                   # build/auralite.iso (+ OVMF from the ovmf package)
bash tests/integration/cases/test_gui_lane_smoke.sh
```

The lane locates OVMF at `/usr/share/OVMF/OVMF_CODE_4M.fd` +
`OVMF_VARS_4M.fd` (override with `OVMF_CODE=` / `OVMF_VARS=`); with no OVMF it
loud-SKIPs and exits 0, the `test_gui_dirty_uefi.sh` convention.

## WR-1 — SHELL32 namespace (no live frames, by design)

WR-1 is headless COM (`IShellFolder`/PIDL, `docs/plans/W32RUN_PLAN.md`
§WR-1): no window, no pixels, no frame receipt. Its evidence is the host
unit test (`tests/unit/test_shell_ns.c`, 45 checks, ASan/UBSan) and the
fixture twin (`tests/integration/cases/test_wr1_shell_namespace.sh`).

## WR-2 — 7-Zip File Manager, live on the framebuffer

Pinned subject (full pins in `docs/w32app_receipts.md` §1; binaries reach
the guest on `/fat`, never the tree):

| binary | sha256 | size |
|---|---|---:|
| 7zFM.exe (7-Zip 24.09) | `dc4fdcd96efe7b41e123c4cba19059162b08449627d908570b534e7d6ec7bf58` | 990720 |
| 7z.dll (7-Zip 24.09) | `882063948d675ee41b5ae68db3e84879350ec81cf88d15b9babf2fa08e332863` | 1907712 |

Live gate: `tests/integration/cases/test_wr2_7zip_live.sh`, **24/24**
(measured 2026-10-10, OVMF/TCG): launch with 292 imports bound and no
`E_FAIL`, the main window with CW-1 clipped panes, `/fat` rows drawn by
the panel, double-click navigate into `SUBDIR`, Backspace navigate-back
to exactly the `C:\fat` root rows, and byte-exact extract (217 B, md5
`3b44a2fc…`) in a second window. Receipt:
`docs/w32app_receipts.md` §WR-2.

Frame digest: the 2026-09-30 reference frame recorded only a short
prefix — `d5a96b225a8e0541…` (`wr2_7zip_live.png`) — and the full digest
was never committed (named gap: no later gate run re-recorded it). No
oracle values were pasted back for the panel asserts (named gap).

Named gaps (SKIP lines, not passes): the clean extract (blank copy
dialog, then "Progress Error" although the bytes are correct), Tools >
Options (the menu *bar* is drawn; the *popup* is not painted, so no
mouse path reaches it), drag-drop (human-run).

## WR-3 — Notepad++, live on the framebuffer

Pinned subject:

| binary | sha256 | size |
|---|---|---:|
| notepad++.exe (8.8.9) | `a470014bb7f6d8587d3a4b9ddc3bab0a356a945e8f63b8067834805d3082472d` | 8365216 |

Live gate: `tests/integration/cases/test_wr3_npp_live.sh`, **17/17 + 5
SKIP** (measured 2026-10-19, OVMF 1280x800): binds 590/590, resolves
both delay-load api-set probes, opens the real `new 1` frame, paints
typed text byte-exact (`abc XYZ 123! -_ end` on pixels), and round-trips
a save through the VFS (29 B fixture `c5e474fd…` + ` WR3GATE` edit reads
back as exactly 37 B, `a829ed05…`), with no user #PF, no fatal signal
and no panic. Receipt: the `docs/w32app_receipts.md` addendum +
`docs/WR3_WORKLOG.md` (13 fixes, the stand recipe, exact diagnoses).

Frame digest: none was recorded (named gap — the WR-3 Test-gate line
that said "frame digest committed" is corrected by WR-5; the recorded
pixel facts are the gate's oracles and the diagnosis screenshots, not a
committed digest).

Named gaps (SKIP lines, not passes): Find (the dialog builds, then NPP
dies in UCRT `__report_gsfailure` — a second stack-cookie smash, separate
session), File > Open (`CoCreateInstance(CLSID_FileOpenDialog)` →
`REGDB_E_CLASSNOTREG`: the COM class table is committed zero rows), no
plugin DLL staged, tray never driven, and a transient open-path paint
smear (a 4-char ghost until the next line repaint).

## WR-4 — PuTTY, live on the framebuffer + live network

Pinned subject:

| binary | sha256 | size |
|---|---|---:|
| putty.exe (0.85) | `d01fdb5aae8f112526040a39b0bfb9e27d813003178645e65f8d1cfdb2a26c87` | 1706136 |

Live gate: `tests/integration/cases/test_wr4_putty_live.sh`, **14/14**
(measured 2026-10-22, two OVMF boots — dialog + raw, then ssh — each leg
with a single live TCP connection): the real config dialog paints, a
typed host lands in the edit, `Save` persists byte-exact into
`/disk/w32hive`, `Cancel` + relaunch repopulates the edit, `-raw`
against the in-lane echo server paints the `ECHO:` reply in the terminal
grid, the terminal closes via X + Exit Confirmation OK, and `-ssh`
against the in-lane paramiko server completes kex+auth+channel with the
command output painted. No panic. Lane servers (committed, host side):
`tests/integration/wr4_echo_server.py` (`:2323`) and
`tests/integration/wr4_ssh_server.py` (`:2222`, `wr4`/`wr4pass`), key
`tests/integration/wr4_ssh_host_rsa.key`. Receipt: the
`docs/w32app_receipts.md` WR-4 addendum.

Frame digest (our own pixels — the ten gate shots, `wr4_frames.sha256`
from the green run):

```
a35c844b46b0704cd85827d859812f1fb8e71c123bb838e23c10475a5c91bb4d  wr4_pre.png
e40926859908a42c3670b1b388b0aecb9be788be8e75225abd58d8d5677d47a7  wr4_dlg0.png
55e244f9e379a0b7199dd0da5029940015eba0f4fcfcbe0cf9d53cc6dca847ad  wr4_typed.png
54059370108d8114d03a4465a1d59bcbcd3788a5ac00e67674cf201a4b233ae4  wr4_closed.png
49ca65c48584da8f817fdf2a16ee650b5e83e487330549b4963f207493b2b2b7  wr4_dlg1.png
1843c4769288797d085223cb7e8a9a5594dff8ed0b053c0095579354357e2749  wr4_term0.png
4c41ce12efafbbeec517195aa331b95d98de9e528021fe3449254b5380acfd73  wr4_term1.png
49ee7f6724578768729d187778be421a8c14595e0da0a4dbdf126e4704c2de8a  wr4_rawclosed.png
0e0093d271b7f313a6633cf47220b4867180199e28c42f254f19114293298bc3  wr4_ssh0.png
a2b3543233455f32cbc03439129c7f388204ab165960c0762ef0e8e217b18237  wr4_ssh1.png
```

Named gaps (not faked, not greened): Telnet untested (Raw only), the
dialog `Load` button untested (Default-Settings auto-load is what the
gate proves), font/colour choosers + clipboard + live-process serial
re-assert human-run, the concurrent-TCP envelope is one live connection
(two corrupt each other — shared NIC queue vs the ws2 readiness pump),
closing the raw window orphans its connection process-side and the
process never quits (lifecycle follow-up), `SECUR32`/`GSSAPI64`
auth-fallback unexercised (password auth succeeded outright).

## WR-5 — the run matrix

`docs/win32_run.md`, generated by `tools/gen_w32_run_matrix.py` from the
gates and receipts above: per app, binds / enters / draws / main UI /
core task / live net. Every green cell resolves to a gate file + a
recorded result; `tools/check_w32app_claims.py` (WR section) enforces
the resolution in `make test-unit`.
