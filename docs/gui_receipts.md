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

## WR-1 … WR-5

Reserved. Each app phase records here: the pinned sha256 (already in
`docs/w32app_receipts.md`), the live gate name, and the frame digest for the
main-window / listing / terminal / editor capture. WR-5 collects them into the
run/no-run matrix (`docs/win32_run.md`).
