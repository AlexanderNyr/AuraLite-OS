# CW — Compositor child-window embedding & clipping

Phase spun out of **WR-2**: the 7-Zip File Manager launches live, but its panes
(toolbar, both file panels, `SysListView32`) are `WS_CHILD` windows and the
compositor drew every window as an independent top-level surface — children were
neither positioned inside, nor clipped to, nor stacked with their parent. That
made the plan's pixel-driven WR-2 gates (panel listing, navigate, extract via
the panel, options) unreachable. This phase gives the kernel compositor real
parent/child windows so those gates become reachable.

## Root cause (measured at `9b61f0a` + WR-2)

`kernel/gui/gui.c`'s `gui_win_t` has **no parent field**. Every window owns an
absolute position, an independent back buffer and a global `z`. `user32`
(`CreateWindowExW`) accepted `WS_CHILD` but only nudged the child's x/y by the
parent's *outer* origin and let it composite as its own top-level surface. No
clipping, no stacking-with-parent, no move/hide/destroy propagation.

## Design

**Ownership stays in the compositor.** A child stores its position *relative to
its parent's content origin* (Win32 `WS_CHILD` semantics); the compositor
resolves absolute coordinates by walking the parent chain. This gives
parent-move propagation for free (a child never stores an absolute coord, so
moving the parent moves the whole subtree with zero extra work).

1. **Data model** — add `int parent` to `gui_win_t` (`-1` = top-level).
   `win_abs_x/win_abs_y` resolve absolute screen coords up the chain;
   `content_x/content_y/content_w/content_h` build on them, so every existing
   caller (blit, invalidate, event-local coords) becomes child-correct with no
   change at the call site. Top-level windows are unaffected (`abs == raw`).

2. **Hierarchical compositing** — both render paths (`compositor_render` full and
   `compositor_render_dirty` partial) iterate only **top-level** visible windows
   in z-order, then recurse into each window's children in sibling-z order. Each
   child is blitted with the gfx clip armed to the **intersection of every
   ancestor's content rectangle** (and the dirty union on the partial path), so
   a child can never paint outside its parent. Children carry no decoration
   (`WS_CHILD` ⇒ `NO_DECOR|BORDERLESS`) and no shadow.

3. **Hit-testing** — `hit_window` finds the top-level under the point, then
   descends into the deepest visible child that contains the point, so clicks
   route to the pane/listview. Activation still raises the top-level ancestor.

4. **Lifecycle propagation** — destroy recurses to children; a child is only
   ever drawn as part of a visible, non-minimized parent subtree, so
   hide/minimize propagate for free. Children are excluded from the taskbar
   (already true: taskbar skips `NO_DECOR`).

5. **Plumbing** — `GUI_OP_SET_PARENT` (syscall) → `gui_set_parent(wid, parent)`;
   `ag_window_set_parent` in libauragui; `user32` links a `WS_CHILD` to its
   parent and stores parent-relative coords (drops the old outer-origin nudge).

## Acceptance

- Unit: rect-intersection + abs-coord resolution math (host test).
- Integration: `make iso` builds; `test_wr2_7zip_live.sh` still green; a new
  assertion (or a follow-up gate) shows the 7-Zip panel region is **no longer
  black** — child content is drawn inside the frame.
- Honesty: whatever remains human-run stays named in the receipt.

## Status (2026-09-30) — DELIVERED

Child embedding + clipping is implemented and verified. Live gate **14/14**
(new pixel assertion: 7-Zip client interior is a light embedded panel, ≈0.99
near-white, vs 0.00 black pre-CW-1); `test_wm` **35/35** (+6 clip-geometry
cases); GUI-lane smoke **9/9** (no top-level regression); host gates green; full
multi-arch `make iso` builds. Receipt: `docs/w32app_receipts.md #CW-1`. The
remaining WR-2 pixel steps (file-row text, navigate, extract-via-panel, options)
are unblocked by the compositor but still need listview `LVN_GETDISPINFO`
rendering + folder navigation — named, not faked.

## Deliverable

`patches/CW1_child_clipping.patch` on top of `9b61f0a` + `WR2_7zip_live.patch`.
