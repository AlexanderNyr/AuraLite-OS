# AuraLite OS — Office Suite Plan

## Status: PLANNED — OF0–OF9 not started

This document answers:

> *Can we ship a full office suite — Writer, Sheet, Slide — on this OS, and what
> has to exist before the first paragraph can be typed?*

The answer is yes, and the interesting part is the word *before*. An office
suite is not a big app; it is a **text engine, a formula engine, a file-format
stack and a print path** that happen to be wearing three windows. Two of those
four do not exist yet in this tree, and the two that do (a 256-glyph monospace
font and a byte-oriented keyboard map) cannot carry a document a human would
write.

It follows the structure of the existing plans (`WEBVIEW_PLAN.md`,
`GL_PLAN.md`, `FSLAYOUT_PLAN.md`, `SDK_PLAN.md`): dependency-ordered phases, a
definition of done and a test gate for every phase, one `.patch` per phase, and
a measurement section that is allowed to change the plan.

**Baseline:** commit `1d02762`, plus the live-VNC display fixes in
`patches/VNC_LIVE_FIXES_2026-11-08.patch`. Those two fixes are prerequisites,
not neighbours: before them the compositor never woke for an app's own
invalidation (so a typing caret would repaint only when an unrelated IRQ
arrived) and a drag into the snap zone could wedge the whole GUI. Both are
fixed and verified live; see `VNC_LIVE_TEST_2026-11-08.md`.

---

## 1. The measurements that shape this plan

Everything below was measured against this tree, not assumed. Where a number
came from a live VNC session on 2026-11-08, the receipt is named.

### 1.1 Text: one font, 256 glyphs, monospace, no Cyrillic

| Fact | Value | Source |
|---|---|---|
| Shipped font | **PSF2, VGA 8×16, 256 glyphs, monospace** | `drivers/framebuffer/psf2_default_font.inc` |
| Fonts in the tree | **one** (no TTF, no OTF, no BDF, no bold/italic face) | `find . -name '*.ttf' -o -name '*.psf*'` |
| Loader capability | PSF1 with 512 glyphs and PSF2 with an arbitrary glyph count are already parsed | `drivers/framebuffer/psf.c:61,73,82` |
| Text drawing | kernel-side only (`gfx_draw_string`, `ag_draw_text`), **one global font for every window** | `drivers/framebuffer/graphics.c`, `lib/libauragui` |
| Proportional metrics | none — every cell is 8 px | same |
| Cyrillic | **absent**: the keyboard ships `us` and `de`, the font is a 256-glyph VGA set | `drivers/keyboard/keymap.c:1`, §1.2 |

A word processor that cannot set «Привет, мир» is not a word processor for this
project's users. Fonts and input are therefore **phase OF1**, before any
document code — this is the single most important ordering decision in the
plan.

### 1.2 Input: two layouts, byte tables, 32-bit key field

| Fact | Value | Source |
|---|---|---|
| Layouts | `us` (QWERTY) and `de` (QWERTZ); `keymap` struct has `lo`, `hi`, `altgr`, `dead`, `name` | `drivers/keyboard/keymap.c` |
| Table element type | `const char [128]` per layer, **single-byte CP437 by design** — no codepoint above 0xFF can be expressed | `drivers/keyboard/keymap.h:19-31` |
| Event field | `kb_event_t.key` is **`uint32_t`** — a Unicode codepoint travels the whole path without an ABI change | `drivers/keyboard/keyboard.h:148` |
| GUI event field | `gui_event_t.key` / `ag_event_t.key` are `uint32_t` | `kernel/gui/gui.h:191`, `lib/libauragui/include/auragui.h` |
| HID poll interval | **7 ms** for the keyboard (live boot receipt) | `[hid] keyboard ready: addr=2 … interval=7ms` |
| Kernel rings | 128 events each for keyboard and mouse | `keyboard.c:39`, `mouse.c:44` |
| Live typing check | 120 ms/char injected → 20/20 characters arrived; burst injection loses characters | `VNC_LIVE_TEST_2026-11-08.md` D6 |

So the *transport* is Unicode-clean; the *table* is the 8-bit bottleneck. That
is a two-file change plus one host test — cheap, and it unblocks Russian
documents.

### 1.3 Display: full repaint is affordable once, not per keystroke

| Measurement | Value | Source |
|---|---|---|
| Full 800×600 blit, in guest, TCG, pixel console on | **46–48 ms/frame** (200 frames, 9.6 s) | live receipt `[gbrowser] blit 800x600: 48030 us/frame` |
| Same benchmark recorded in the webview plan | **7.6 ms/frame** | `docs/webview.md` §3 |
| Layout of 5 000 boxes | 10.1 ms | `[gbrowser] layout smoke: PASS (items=10001, 5000 boxes in 10101 us, page_h=80000)` |
| Repaint mechanism | dirty-rect only; `gui_invalidate_rect()`, `compositor_render_dirty()` | `kernel/gui/gui.c` |
| Compositor wake on app invalidation | **broken until 2026-11-08**, fixed by `gui_poke()` | `patches/VNC_LIVE_FIXES_2026-11-08.patch` |

A typed character must repaint a caret and a line — tens of thousands of
pixels, not 480 000. The plan therefore forbids full-window repaints in the
typing path and gates on a measured budget (§4, OF0).

### 1.4 Formats and packaging: one third of a zip, no zip

| Piece | State |
|---|---|
| DEFLATE inflate (RFC 1951) + zlib wrapper (RFC 1950), no heap, caller buffer | **exists** — `userspace/apps/gbrowser/wv_inflate.c`, 269 lines |
| DEFLATE compress (deflate), i.e. writing compressed data | **absent** |
| ZIP container: local headers, central directory, EOCD | **absent** (`grep 0x04034b50` → nothing) |
| HTML5 tokeniser (state machine, entities, no recursion, 122 unit checks) | **exists** — `wv_html.c`, 951 lines, `tests/unit/test_wv_html.c` |
| XML parser / writer | **absent** as a library; the HTML tokeniser is the obvious base |
| PNG decode | **exists** — `wv_image.c`, 872 lines |
| PDF writer | **absent** |
| Print spooler / printer driver | **absent**, and out of scope (§8) |

OOXML without ZIP is unimplementable; ZIP **without a compressor** is legal —
method 0 (stored) is a first-class ZIP method and every OOXML consumer accepts
it. That single fact is why this plan can promise `.docx` in phase OF8 without
writing a deflate.

### 1.5 Program budget and the shell around the apps

| Fact | Value | Source |
|---|---|---|
| Max process image | **16 MiB** (`SPAWN_MAX_IMAGE`) | `run sysinfo` in guest |
| User stack | 4 MiB | same |
| Existing GUI app sizes | 27 KB (`gabout`, `gaudio`, `gtaskmgr`, `gcalc`) … 42 KB (`gedit`), 321 KB (`gbrowser`) | `build/user/*.elf` |
| Clipboard | **text only**: `ag_set_clipboard` / `ag_get_clipboard` | `lib/libauragui/include/auragui.h:175` |
| Editor baseline | `gedit`, 363 lines: line array, `MAXCOL` cap, Load/Save via a path textbox, **no undo, no wrap, no selection** | `userspace/apps/gui-edit/gedit.c` |
| Start menu | kernel table `gui_apps[]`, 10 entries, menu **fixed at 220 px**, rows 18 px at `y = 544 + 18i` (confirmed by clicking rows in VNC) | `kernel/gui/gui.c:1888,1900` |
| Taskbar | window buttons are **dropped when the strip fills**: a 10-window workload drew Start + 6 buttons, the rest unreachable | `vnc/shots/p1_s.png` (live) |
| Writable document locations | `/tmp`, `/disk`, `/fat` — **no `/home`, no documents directory, no recent-files store** | shell `help`, `docs/plans/FSLAYOUT_PLAN.md` |

---

## 2. Where things actually stand

| Capability a suite needs | Today |
|---|---|
| Proportional text, bold/italic, Cyrillic | **no** (one 8×16 bitmap font) |
| Russian keyboard layout | **no** (`us`, `de`) |
| Byte-accurate text model with undo | **no** (`gedit` is a line array with a column cap) |
| Multi-document / recent files | **no** |
| Formula parser / dependency graph | **no** |
| Grid view with virtualisation | **no** (auragui has `LISTBOX`, `TABLE` does not exist) |
| Charts | **no** primitives beyond rect/line/circle; `LIBGL` exists if ever needed |
| Image embedding | decode **yes** (`wv_image.c`), embedding in a document **no** |
| Clipboard (text) | **yes** |
| ZIP container | **no** |
| XML read/write | **no** (HTML tokeniser exists) |
| PDF export | **no** |
| Undo/redo anywhere in the tree | **no** |
| File dialogs (open/save) | pattern exists (path textbox in `gedit`), no shared widget |
| Integration-test harness for GUI apps | **yes** (227 cases, `il_*` helpers), 227-case shard partition |
| Live VNC acceptance harness | **yes** (`vnc/drive.sh`, `vnc/alive.sh`, `vnc/btn.py`) |

---

## 3. Decisions

### D1. Three applications over one document core

`lib/libaodoc` owns the text model, the layout engine, the undo log, the file
I/O and the glyph rasteriser. Writer, Sheet and Slide are three shells over it
— each a few thousand lines, sharing every hard part. A single monolithic
"office app" would share memory with nothing and duplicate the text engine
three times.

### D2. The suite renders its own glyphs

Text inside a document window is drawn by a **userspace rasteriser**
(`lib/libaoglyph`) reading font files from the initrd; the kernel keeps its VGA
8×16 font for window chrome, the console and the boot splash. Reasons: the
kernel font is a single global monospace face, and proportional metrics,
synthesised bold/italic, kerning-free advance widths and Unicode→glyph mapping
are document concerns. It also means no kernel change is needed to ship a
better font, and a bad font can never wedge the compositor.

The trade is explicit: **the suite cannot use `ag_draw_text()`** in document
content — it composites into its own buffer and presents with one `ag_blit()`
per dirty rect. That is the same model `gbrowser` already uses.

### D3. UTF-8 end to end, UTF-32 in the keymap

Documents are UTF-8. The keymap tables become `uint32_t` codepoint tables and
gain `ru` (ЙЦУКЕН); `us` and `de` keep their behaviour, and `tests/unit/test_keymap.c`
grows Cyrillic row checks. The event path already carries 32 bits, so no ABI
changes.

### D4. One native format, and it is XML

`<aodoc>`, `<aosheet>`, `<aodeck>` — plain XML files, one schema-version
attribute. Phase OF7 wraps **the same XML** in a store-only ZIP with the OPC part
layout OOXML uses, so the container code written once serves both `.aod` and
`.docx`. A native format that is not the export format doubles the work of
every later phase.

### D5. Dirty rects only, and the budget is a gate

No full-window repaint in the typing path, ever. OF0 measures and records:
caret repaint, line repaint, page scroll, full repaint. A phase that cannot
meet its budget does not merge.

### D6. Undo is a command log, not snapshots

Insert/delete runs, format runs, cell edits, slide edits — each a record with a
byte budget (`8 MiB` default) and coalescing of adjacent typing into one record
per 750 ms of idle. Snapshotting a 2 MiB document per keystroke is not
affordable at 512 MiB of RAM.

### D7. OOXML is a bridge, not a working format

`.docx/.xlsx/.pptx` are imported into the native model and exported from it.
The subset is documented in `docs/office.md` and round-trip tested; fidelity
beyond it is explicitly not promised. If a document uses a feature we cannot
represent, the import says so in the window — a silent downgrade is worse than
a warning.

### D8. No macros, no scripting, no database app

No VBA, no Basic, no JavaScript in documents, no embedded scripting engine, no
`Base`-style SQL front end. This mirrors `WEBVIEW_PLAN.md` D5: a scripting
surface multiplies the security story (sandbox, capability model, DoS limits)
by an order of magnitude, and there is no such model in this kernel for
untrusted documents yet. Formulas in Sheet are the only executable content, and
they are a closed, side-effect-free expression language.

### D9. "Print" means "Export PDF"

There is no print subsystem, no spooler and no printer driver, and this plan
does not add one. A PDF 1.4 writer with an embedded **Type3 bitmap** font
(our own glyphs, no CFF/TrueType subsetting, no CMap problem, Cyrillic works
by construction) covers the real requirement: a page that can leave the
machine. A later plan can add a spooler on top of the same page model.

### D10. One patch and one gate per phase

Every phase lands as a single `.patch` in `patches/`, with:

* host unit tests (`tests/unit/test_ao_*.c`, `checks=N fails=0`),
* an integration case (`tests/integration/cases/test_office_*.sh`) wired into
  `ALL_CASES` **and** into the shard partition (`--check-groups` must stay
  green — every case matches exactly one group),
* a VNC live acceptance pass (screenshots + serial receipts, the
  `vnc/drive.sh` harness), because a green assert set has never been evidence
  that a window works (see `VNC_LIVE_TEST_2026-11-08.md`).

---

## 4. Phases

Dependency-ordered. Each phase is independently shippable and independently
useful.

### Phase OF0 — Foundations, budget and the shell around the apps

**Objective:** the skeleton every later phase sits on, and the numbers that
police it.

#### Tasks

- [x] `userspace/office/` tree: `libaodoc/` (document core), `libaoglyph/`
      (font rasteriser), `writer/`, `sheet/`, `slide/`, `office/` (start centre).
      **Partly**: `libaodoc/` and `office/` (the start centre at OF0's floor)
      exist and build; `libaoglyph/`, `writer/`, `sheet/`, `slide/` arrive with
      OF1–OF6.
- [x] `libaodoc`: document handle, arena allocator per document, dirty-rect
      canvas helper (`ao_canvas_*`) that batches `ag_window_invalidate_rect()`
      calls and presents once per frame. **Done**: `ao_arena_*`, `ao_doc_*`,
      `ao_canvas_*`; the geometry is pure so the shipping source is graded on
      the host by `tests/unit/test_aodoc_canvas.c` (59 checks: every marked
      pixel is inside some planned rect, touching rects merge, the budget
      degrades to a correct union instead of dropping pixels, clipping is exact
      at all four edges).
- [x] A `docs/office.md` that states up front: no macros, no SQL app, PDF-only
      print, OOXML subset, monospace-fallback if no font is found. (Written in
      OF0a; the frame-budget table in it is filled from measurements taken in
      the same boot, and is re-measured by every phase that touches paint.)
- [x] **Measure the frame budget** with `fbconsole=off` and record it in
      `docs/office.md`: full 1024×768 repaint, one-line repaint, caret
      repaint, scroll of one page. **Done** for the three classes the suite
      owns, by `aoffice --bench` in the guest: full 3 030 µs, line 50.5 µs
      (1 %), caret 10.1 µs; and in pixels — the number that has to hold on a
      bigger screen — caret 56 px of 281 600 (0.01 %), line 8 640 px (3.06 %).
      A live keystroke changed 0.62 % of the window in two bands
      (`docs/office.md` §4.3). Scroll-of-a-page waits for OF2, which is the
      first phase with a page to scroll.
- [x] Taskbar overflow: buttons must not vanish when a suite is open. Either
      shrink-to-fit with a minimum width, or a `»` overflow list. (Live
      evidence: 10 windows → Start + 6 buttons drawn, the rest unreachable,
      `vnc/shots/p1_s.png`.) **Done both ways**: shrink-to-fit 130→44 px, then
      a paging `»` list. One implementation of the arithmetic in
      `kernel/gui/taskbar_layout.h`, called by the renderer and the hit test.
      Gate: `tests/unit/test_taskbar_layout.c` (57 388 928 checks, every
      count 1..256 × every width). Live: 18 windows → 16 buttons + `»`,
      `vnc/shots/of0b_*.png`.
- [x] Start menu growth: `gui_apps[]` is 10 entries in a fixed 220 px menu.
      Group the entries (Applications / Office / System) and make the menu
      height derive from the entry count — the hit-test must keep following the
      draw, because today it is 18 px rows at `y = 544 + 18i`. **Done**: rows
      are derived (headers + entries) and both draw and hit test walk
      `gui_menu_rows()`. Gate: `tools/check_start_menu.py` (+ `--selftest`,
      5 injected defects). Live: grouped menu, and a click on a grouped row
      launches its own binary (`[gui] launched: Terminal (/apps/gterm)`).
- [x] A `/documents` location: a writable directory that survives a reboot
      (`/disk/documents` when a disk is present, `/tmp/documents` otherwise),
      created at boot if missing. **Partly**: mounted at boot as its own tmpfs
      volume, receipt and probe in `kernel/fs/documents.c`; durability is NOT
      achieved and is stated as such — diskfs has no `mkdir`, and a symlink to
      a disk directory cannot be created *through* (the VFS follows symlinks on
      resolve, not on create), so that work is tracked in `TODO.md`.

#### Test gate

- `tests/integration/cases/test_office_shell.sh`: the suite's start centre
  window opens from the Start menu, lists three applications, and closes
  cleanly (`[thread] '…' exited (code=0)`, no kernel fault).
- `test_taskbar_overflow.sh`: 8 windows open → all 8 windows reachable by
  click (a taskbar click still focuses the right app window).
- `test_start_menu_groups.sh`: every menu entry launches the binary its label
  names (the pattern used to find the wrong row in the live pass —
  `[gui] launched:` receipts are the assert).
- Frame budget recorded in `docs/office.md` and asserted by
  `test_office_budget.sh` (a window that prints its own repaint timings).

#### Deliverable

`patches/OFFICE_OF0_foundations.patch`

#### Definition of done

A window that draws a ruler, a margin box and a caret, types text into itself
with **no full-window repaint**, and reports its repaint costs. Nothing is
saved yet.

---

### Phase OF1 — The text foundation: fonts, Unicode, Russian input

**Objective:** the machine can render and accept a document a human would
write.

**Status: ✅ COMPLETE (2026-11-09)** — deliverable
`patches/OFFICE_OF1_text.patch`.

#### Tasks

- [x] `lib/libaoglyph`: PSF2 loader (the kernel loader is the reference,
      `drivers/framebuffer/psf.c`), plus a **BDF** loader (human-editable, so
      new faces do not need a bitmap editor). **Done**:
      `userspace/office/libaoglyph/aoglyph.{h,c}` — `ao_font_load_bdf()`,
      `ao_font_load_psf2()`, a caller-owned arena (no allocation), keyword
      matching that requires a delimiter (a prefix match reads
      `CHARSET_REGISTRY` as `CHARSET`), `AO_GLYPH_MAX 512` with a loud refusal
      above it.
- [x] Ship **two** faces in the initrd under `/lib/fonts`: a proportional
      bitmap face (UI + body text, ~10–13 px em) and the existing 8×16
      monospace. Both **with Cyrillic** (U+0410–U+044F, Ё/ё, plus the
      typographic set: « » — – … №). **Done**: `ao_sans_11.bdf` (173 glyphs,
      cell 16×14, advance 4–13 px) and `ao_mono_16.bdf` (173 glyphs, 8×16),
      generated and checked by `tools/gen_office_fonts.py --check`; both
      report `cyrillic 64/64, typographic 4/4` in the guest.
- [x] Synthesised **bold** (horizontal smear with a 1-px column doubling) and
      **italic** (row-wise shear), documented as synthesis, not faces.
      **Done**: bold = 1 px right smear (`293 → 455` pixels for the same 18
      glyphs), italic = `dy/4` shear, graded by **geometry** (`top row starts
      at x=14 vs 13`) because a sheared row keeps its pixel count.
- [x] Unicode→glyph mapping: BMP coverage table, `?` fallback, no crash on
      unmapped codepoints, and a hard cap per font. **Done**:
      `ao_font_glyph()` answers a notdef glyph for an uncovered codepoint,
      `ao_font_covers()`/`ao_font_has()` expose the table, and the cap is
      gated (512 loads, 513 is refused with `AO_FONT_CAPACITY`).
- [x] Keymap: tables become `uint32_t`; add `ru` (ЙЦУКЕН layout, both
      registers); `kbd ru` works from the shell; `test_keymap.c` grows row
      checks for all three layouts. **Done**: `keymap_ru.lo/.hi` stay `char`
      (the console byte, ASCII and control only) while `ru_ulo/ru_uhi` are the
      `uint32_t` truth tables; `keymap_unicode()` is the single authority and
      derives `us`/`de` through CP437; `kbd ru` prints
      `[kbd] layout set to 'ru'`.
- [x] UTF-8 decode/encode helpers in `libaodoc` (`ao_utf8_*`), with the classic
      overlong/truncated/surrogate rejections unit-tested. **Done**:
      `ao_utf8_encode/decode/prev/valid/count` in `libaodoc/aodoc.c`, 53 host
      checks (`E_LEAD`, `E_RANGE`, `E_TRUNC`, `E_SURROGATE`, `E_OVERLONG`).
- [x] **Carry the codepoint to the window.** Not a line in the original task
      list, and it is what made Cyrillic actually arrive: `kb_event_t.cp` is
      filled by the driver from the active layout, `gui_event_t.data` carries
      it through the compositor (`kernel/gui/gui.c`, `route_key_event`), and
      `aoffice` inserts `data` when it holds a character and `key` otherwise.
      The USB HID path had to be taught the difference between a usage id and
      a set-1 scan code first — see the Result.
- Line breaking (greedy wrap, breakpoint table) **moved to OF3**, which is the
  phase that owns layout: OF1 edits fixed rows, so there is nothing to wrap
  yet. Recorded here so the task is not silently dropped.

#### Test gate

- `tests/unit/test_ao_glyph.c` (62 checks): BDF/PSF2 parsing from byte arrays,
  a hermetic 4-wide glyph that pins the row padding direction, the 512 cap and
  the 513 refusal, truncated/garbage/tiny-arena cases, exact style pixel
  counts, italic geometry.
- `tests/unit/test_ao_utf8.c` (53 checks): round-trip including `U+10FFFF`,
  overlong/surrogate/range/lead/truncated rejection classes, `valid`/`count`,
  a NUL inside the buffer, `ao_utf8_prev`.
- `tests/unit/test_keymap.c` (133 checks, was 105): `ru` produces `йцукен` and
  `ЙЦУКЕН`, ФЫВА/ЯЧС positions, ё/Ё, CapsLock XOR Shift, punctuation, and
  leaves control bytes untouched; `keymap_unicode()` for `us`/`de`.
- `tests/integration/cases/test_office_font.sh` (16 assertions, `gui` shard):
  both faces staged and loaded, 64/64 Cyrillic, the sample measures in both,
  three styles, **no glyph of the sample paints zero pixels**, `kbd ru`.
- In-guest receipt: `aoffice --font-test` →
  `AOFFICE FONT: PASS (2 face(s), 0 failure(s))` with one
  `glyph U+%04X … paints=N` line per glyph.
- **Live VNC acceptance: done.** (1) The specimen strip draws «Привет, мир —
  2026» in regular, bold, italic and monospace. (2) `kbd ru` in the shell, then
  the keys `g h b d t n` on a QWERTY keyboard, put «привет» into the document
  — a screenshot of the framebuffer, not a claim.

#### Result

Two real defects surfaced during this phase, both invisible to a screenshot
that "looks right":

1. **BDF rows were left-aligned to the glyph box instead of MSB-aligned within
   `ceil(w/8)` bytes.** The bytes are identical for `w == 8`, so every letter
   looked perfect while **every glyph narrower than 8 px was blank** — the
   comma, the period and the em dash were missing from both shipped faces. The
   generator now shifts (`bits << (stride*8 - w)`), the fonts were regenerated
   (`ao_sans_11.bdf` 20 355 B, `ao_mono_16.bdf` 19 778 B), and the phase gate
   now refuses a non-space glyph with `paints=0`, because a blank letter reads
   as a spelling mistake rather than as a bug.
2. **The USB HID keyboard path queued usage ids where the layout expected
   set-1 scan codes**, so `evt_enqueue()` computed `cp = 0` for every key on
   the machine's only working keyboard: the Russian layout typed «ghbdtn»
   instead of «привет» in every window. `usb_usage_sc1()` is now the standard
   usage → scan-code table, and the codepoint is computed from it.

One thing carried in from OF0: that patch named
`tests/unit/test_aodoc_canvas.c` in the Makefile and never shipped the file, so
`make test-unit` was red on a missing target from OF0 until now. It exists in
this patch, 59 checks, and the whole unit sweep is green.

Two smaller things the phase forced out: hand-written `\xDD` escape literals in
the sample text silently lost characters (`французских` arrived as `фра`) — the
sample is literal UTF-8 now — and `aoffice`'s line buffer was a 96-**byte** cap,
which a 94-byte Russian sentence fills, so a two-byte letter could not be typed
after it; it is 512 bytes.

#### Deliverable

`patches/OFFICE_OF1_text.patch`

#### Definition of done

A window that shows mixed-script, mixed-style text with proportional metrics,
and accepts Cyrillic typing from a real keyboard event stream. **Met.**

---

### Phase OF2 — The word processor core (Writer)

**Objective:** open, edit, save a document — correctly, with undo.

**Status: 🟨 PARTIAL** — the model, the undo history, the native XML format and
the Writer application on top of them are in the tree and gated:
`tests/unit/test_ao_doc.c` (107 checks, a 10 000-operation fuzz run through both
the binary and the XML form), `tests/unit/test_ao_undo.c` (74 checks) and
`tests/integration/cases/test_office_writer.sh` (model → file → `cat` → second
instance). What is NOT done is listed as unchecked below rather than quietly
counted as finished: mouse selection, the clipboard verbs, the shared open/save
dialog, word navigation, and the live VNC 20 cps pass.

#### Tasks

- [x] Document model: paragraphs of runs (`text`, `bold`, `italic`,
      `underline`, `size`, `font`), paragraph attributes (align, indent,
      spacing, style name), one flat structure with index links (the
      `wv_dom` pattern: no pointers into a heap, no runaway recursion).
      `libaodoc/aomodel.{h,c}`.
- [x] A **view** that lays out only the visible page band plus one screen of
      margin, and repaints by dirty rect. `aoffice.c` draws the visible lines
      run by run from the model; wrapping and justification are OF3's.
- [ ] Caret and selection: shift-extends, Home/End, PageUp/PageDown and
      click-to-focus are in; click-to-place, drag-to-select, word navigation and
      Ctrl+Home/End are not. The caret is a **byte offset** into the model and
      moves through `next_cp`/`prev_cp`, so none of the remaining ones can land
      inside a codepoint.
- [ ] Editing: insert, delete, backspace/Delete, split/join paragraph and
      replace-selection are in; paste, cut and copy through the existing text
      clipboard are not.
- [x] **Undo/redo** (D6) for every edit, with coalescing and a byte budget.
      `libaodoc/aoundo.{h,c}`.
- [ ] Find and replace: Ctrl+F searches for a literal, ASCII-case-insensitive
      string and SELECTS the hit, F5 finds the next one (wrapping once), Ctrl+H
      replaces the selection. Replace-all and a match count are not in.
- [x] Save/Load native XML (`<aodoc version="1">`, `libaodoc/aoio.{h,c}`):
      escaping (`&amp; &lt; &gt; &quot;`), numeric entities by decimal and hex,
      a **hard depth cap** (16) and attribute cap (256), a missing or unknown
      `version` refused, unknown attributes ignored for forward compatibility,
      and a failed load leaves the document EMPTY rather than half-loaded.
      Ctrl+S writes `/documents/Untitled.aod`, Ctrl+O reads it back. The ZIP
      container and OOXML are OF7/OF8, where the plan puts them.
- [ ] File dialogs: one shared open/save widget (path box + directory listbox +
      overwrite confirmation), replacing the bare textbox pattern `gedit` uses.
      Today the Writer saves to one fixed path, which is honest but not a
      dialog.

#### Test gate

- `tests/unit/test_ao_doc.c` — **107 checks, PASS**: model invariants under a
  **randomised edit fuzz** (10 000 operations, every invariant re-checked after
  each, including a paragraph-tiling check the fuzz itself forced into the
  test), save→load→save byte-identical through the binary form AND through XML,
  and the XML reader against files that are wrong on purpose: truncated,
  unclosed, unknown version, missing version, run outside a paragraph, bad
  numeric entity, surrogate entity, 40-level nesting.
- `tests/unit/test_ao_undo.c` — **74 checks, PASS**: coalescing boundaries (a
  barrier, a kind change and an edit entry each split a run), budget eviction
  oldest-first, redo invalidation after a new edit, a snapshot too large for the
  whole history refused without corrupting what is already stored, and a
  200-operation session undone to the top and redone to the bottom, compared
  byte for byte at both ends.
- `tests/integration/cases/test_office_writer.sh`: `aoffice --writer-test` types
  through the keystroke path, selects all, bolds, undoes, redoes, saves to
  `/documents`, reopens through the VFS; the shell's `cat` prints the XML and
  the case asserts the declaration, the version, the bold run, the Cyrillic and
  the em dash; the whole gate runs a second time so a second instance has to
  read what the first one wrote.
- **Live VNC acceptance** (open): type 200 characters at 20 cps, save, reopen,
  assert zero loss (the input-ceiling gate from §1.2), screenshots per step.

#### Result so far (model, history, format, application)

The model is one flat structure with index links, and two decisions carry it:

- **Text first, structure rebuilt.** An edit applies to the text, then clips the
  run array and rebuilds paragraphs from the surviving `'\n'` bytes. Patching
  paragraph and run records in place is where this code usually breaks; a clip
  plus a rebuild is O(n) over at most 64 KiB and is *checkable*.
- **Positions are byte offsets, and they never sit inside a codepoint.** An
  insert at such a position snaps forward, a delete snaps outward, and a style
  range snaps to codepoint edges before it splits a run -- that last one was
  found by the XML writer emitting half a Cyrillic letter, which the reader then
  refused. None of those were hypothetical.

Undo stores **serialised copies of the document**, not inverse operations: the
inverse of "merge two paragraphs and re-target a run array" is a structural
transplant, while a copy is correct by construction and reuses the save format.
The byte budget evicts oldest-first and typing coalesces into one entry per run.
The two stacks get **separate buffers** -- a shared buffer has no room for the
redo entry exactly when it is needed, because the entry being consumed sits
where the new one must go.

The **format** is deliberately boring: one `<aodoc>`, one `<p>` per paragraph,
one `<r>` per run, text as literal UTF-8, five escapes. Three rules cost real
debugging and are worth recording:

- **A paragraph's terminating newline belongs to no run.** It is stripped on
  save and re-added on load, so a run that held only the newline is not written
  at all; otherwise two documents that mean the same thing would serialise
  differently, which is what "save → load → save is byte identical" forbids.
- **Attributes must be stamped, not inherited.** Text inserted after a bold run
  inherits bold, so a plain `<r>` following a bold one has to clear the bits
  explicitly or the paragraph comes back entirely bold.
- **Alignment cannot be applied while reading.** The model creates a paragraph
  only when something is written into it, so an EMPTY paragraph has no index
  yet; the reader collects one alignment byte per `<p>` and applies them once
  the document is in. A `n--` on a zero-length run (four gigabytes past the
  text) came from the same fuzz run.

The **application** keeps OF0's rendering, OF1's text layer and the dirty-rect
budget, and swaps the line array for the model: `lines[][]` survives as a
display cache rebuilt after every edit, so the two cannot disagree. Ctrl+A/B/I/U
/Z/Y/S/O/F/H, Shift+arrows, Home/End, PageUp/PageDown and F5 are wired; the
status strip shows the block style, the selection length and the undo depth.

#### Deliverable

`patches/OFFICE_OF2_writer.patch` — folded into the cumulative patch against
the upstream base (`patches/OFFICE_CUMULATIVE.patch`), because one patch that
applies to `5c1e13e` is what was asked for rather than one per phase.

#### Definition of done

A document can be written, formatted with bold/italic/underline, undone,
searched, saved and reopened — with no full-window repaint in the typing path.
**Every verb in that sentence works; the file dialog, the clipboard, mouse
selection and the live VNC pass are the four open items above, and the phase
stays 🟨 until they close.**

---

### Phase OF3 — Writer: layout, tables, lists, images

**Objective:** a document that looks like a document.

#### Tasks

- [ ] Page model: page size, margins, headers/footers, page numbering, page
      break, **page view** vs draft view.
- [ ] Paragraph styles (Normal, Heading 1–3, Quote, List) with an
      applied-style picker; direct formatting layered over the style.
- [ ] Lists: bulleted and numbered, multi-level, with continuation rules.
- [ ] Tables: insert, delete row/column, column widths, cell shading, merge
      horizontally, borders; layout is a second algorithm over the same run
      model.
- [ ] Images: inline PNG (reuse `wv_image.c` decode; move it to
      `lib/libaodoc` so both Writer and Slide link one copy), scaled to fit,
      with a placement cap.
- [ ] Print preview, driven by the same page layout the PDF writer will use —
      one page model, two consumers.

#### Test gate

- `tests/unit/test_ao_layout.c`: wrap points against a fixed-width font
  fixture, a 200-paragraph document, a table with 20 columns, an image
  larger than the page, a paragraph taller than the page (must split, not hang).
- `tests/integration/cases/test_office_writer_layout.sh`: build a document with
  a heading, a bulleted list, a numbered list, a 4×4 table and an image;
  scroll through it; save; reload; assert the layout reproduces (page count and
  line count printed by the app).
- **Live VNC acceptance:** scroll a 20-page document with the wheel and with
  the scrollbar; per-frame repaint must stay inside the OF0 budget.

#### Deliverable

`patches/OFFICE_OF3_writer_layout.patch`

---

### Phase OF4 — The spreadsheet engine (Sheet, headless)

**Objective:** a calculation engine that is correct before it is pretty.

#### Tasks

- [ ] Sparse sheet: a hash map of non-empty cells, bounded to `1 048 576 ×
      16 384` *addressing* with a practical cell cap (documented), plus a
      workbook of sheets.
- [ ] Value model: number (double), text, boolean, error (`#DIV/0!`,
      `#VALUE!`, `#REF!`, `#NAME?`, `#NUM!`, `#CYCLE!`), blank.
- [ ] Formula parser: recursive descent over a tokeniser; operators
      `+ - * / ^ % &`, comparison, unary minus, parentheses, ranges `A1:B9`,
      sheet-qualified references, absolute/relative markers parsed and preserved.
- [ ] Dependency graph + recalculation: topological order, **cycle detection**,
      dirty propagation on edit, and a recalculation budget that refuses a
      pathological graph instead of freezing.
- [ ] Functions, 45 of them: `SUM AVERAGE MIN MAX COUNT COUNTA COUNTIF SUMIF
      IF AND OR NOT ABS ROUND ROUNDUP ROUNDDOWN INT MOD POWER SQRT EXP LN LOG
      SIN COS TAN PI CONCATENATE LEFT RIGHT MID LEN UPPER LOWER TRIM
      VALUE TEXT TODAY NOW VLOOKUP HLOOKUP INDEX MATCH RANK MEDIAN STDEV
      PRODUCT AVERAGEIF SUMPRODUCT` — each with unit tests, including the
      error-propagation rules.
- [ ] Number formatting: general, fixed decimals, thousands separator, percent,
      date, currency (a format string parser shared with the cell editor).
- [ ] CSV/TSV import and export (quoted fields, embedded newlines, delimiter
      sniffing) — the format that makes the engine usable before the view exists.

#### Test gate

- `tests/unit/test_ao_formula.c`: expression → value for every operator and
  function; precedence; string coercion; error propagation; a 1 000-cell
  `SUM`; a self-referential cell (`=A1+1` in `A1`) → `#CYCLE!`, not a hang.
- `tests/unit/test_ao_sheet.c`: edit → recalc correctness (a chain of 1 000
  dependents), deletion compacting the map, sheet copy/rename, CSV round-trip
  against hostile input (quotes, CRLF, 1 MiB field).
- `tests/integration/cases/test_office_sheet_core.sh`: a headless mode
  (`/apps/aosheet --eval file.aosheet`) that prints computed cell values as
  receipts — the engine gated without a single pixel.
- Assert: no allocation failure leaks (the arena is freed; the allocator's own
  counters are printed).

#### Deliverable

`patches/OFFICE_OF4_sheet_engine.patch`

#### Definition of done

A `.aosheet` file with formulas computes to the same values through the shell
as through the engine's unit tests, and a cyclic workbook is a message, not a
hang.

---

### Phase OF5 — Sheet: the grid and the charts

**Objective:** a spreadsheet a user can actually fill in.

#### Tasks

- [ ] Virtualised grid: only visible rows/columns are drawn; a 100 000-row
      workbook scrolls without allocating 100 000 widgets.
- [ ] Cell editor driven by the same format parser; formula entry with a
      reference-highlight mode (dragging colours the referenced cells).
- [ ] Keyboard-first navigation (arrows, Tab/Enter, Ctrl+arrows, F2, Esc) —
      the office idiom that makes or breaks a spreadsheet.
- [ ] Selection: rectangular ranges, fill handle, copy/paste (formula
      translation on paste), drag-and-drop move, undo for every one of them.
- [ ] Column/row resize, freeze panes, sort/filter over a range.
- [ ] Charts: bar, line, pie from a range, drawn with the existing 2D
      primitives into the sheet's canvas; chart objects live in the document
      (so they save, load and undo).

#### Test gate

- `tests/unit/test_ao_grid.c`: virtualisation arithmetic (visible range for
  every scroll offset), selection normalisation, paste translation, sort
  stability, filter predicates.
- `tests/integration/cases/test_office_sheet_gui.sh`: launch, type a 10×5
  table, add `=SUM(B2:B11)`, read the result **off the framebuffer** via the
  app's own status receipt, save, reload, assert the sum survives.
- `test_office_sheet_perf.sh`: scroll a 10 000-row sheet; assert the per-frame
  repaint stays inside the OF0 budget and that no allocation happens per frame
  (arena counters).

#### Deliverable

`patches/OFFICE_OF5_sheet_gui.patch`

---

### Phase OF6 — Presentations (Slide)

**Objective:** a fourth document type sharing the same core.

#### Tasks

- [ ] Deck → slide → object model: text boxes, shapes (rect, rounded rect,
      ellipse, line, polyline), images, z-order, per-object transform.
- [ ] Layout guides and snapping, master slide (background, default text
      style, page number), duplicate slide, reorder in a slide sorter.
- [ ] Text boxes reuse the Writer run model and the OF3 layout engine — the
      second consumer that proves the engine is a library.
- [ ] **Slide show:** a borderless maximised window (`GUI_WIN_NO_DECOR`), one
      slide per screen, advance on click/Space/arrows, Esc exits; no
      transitions, no animations (§8).
- [ ] Speaker notes in the document; presenter view is out of scope.

#### Test gate

- `tests/unit/test_ao_slide.c`: model invariants, z-order operations, snapping
  arithmetic, undo across slide reorder.
- `tests/integration/cases/test_office_slide.sh`: create 3 slides, put a title
  and a shape on each, save, reload, run the show (assert the app prints
  `SLIDE-SHOW: slide 1/3` … `3/3` and exits on Esc), assert clean exit.
- **Live VNC acceptance:** drive the show with real clicks and Space, capture
  each slide; assert the fullscreen window covers the taskbar and returns.

#### Deliverable

`patches/OFFICE_OF6_slide.patch`

---

### Phase OF7 — The container: ZIP and XML

**Objective:** the packaging layer that makes native saves and OOXML share
code.

#### Tasks

- [ ] ZIP **reader**: EOCD scan from the tail (with a bounded search), central
      directory parse, local header parse, method 0 (stored) and method 8
      (deflate **inflate only**, reusing `wv_inflate.c` moved into
      `lib/libaodoc`), CRC-32 verification, ZIP64 refusal with a clear message.
- [ ] ZIP **writer**: method 0 only (D4), deterministic entry order and
      timestamps (so a save is byte-reproducible), streaming from a document
      buffer — no full copy in RAM beyond the document itself.
- [ ] XML **writer** and **reader**: an escaping writer and a pull parser with
      depth/attribute/size caps, built from the `wv_html.c` state machine (the
      HTML tokeniser is a superset of what a document needs; the XML mode is a
      subset with no entity fuzz and no implied-close rules).
- [ ] Native package `.aod` = ZIP{ `mimetype` (stored first), `content.xml`,
      `styles.xml` } with the OPC-shaped part names OOXML uses.
- [ ] Migration: OF2–OF6 saves were plain XML; the loader sniffs the ZIP magic
      and accepts both.

#### Test gate

- `tests/unit/test_ao_zip.c`: read a fixture zip (stored + deflated entries),
  a truncated central directory, a lying uncompressed size, a CRC mismatch, a
  ZIP64 EOCD; write→read round-trip byte-identical; a 50-entry package.
- `tests/unit/test_ao_xml.c`: escaping (`& < > " '`), a 1 MiB text node, a
  10 000-deep nesting (must be refused), CDATA, comments, a BOM.
- `tests/integration/cases/test_office_container.sh`: save a Writer document
  as `.aod`, `ls` the file, extract `content.xml` through the shell's `cat`
  (the reader prints an entry listing), reload, assert identical text.
- Interop receipt: the produced `.aod`/`.docx` is opened **on the host** by
  `python3 -c "import zipfile; zipfile.ZipFile('x.docx').testzip()"` and the
  parts are listed — a real ZIP, not a lookalike.

#### Deliverable

`patches/OFFICE_OF7_container.patch`

---

### Phase OF8 — The OOXML bridge: .docx / .xlsx / .pptx

**Objective:** read and write the formats the rest of the world uses.

#### Tasks

- [ ] `.docx` **import**: `word/document.xml` — paragraphs, runs with
      `b/i/u`, alignment, list numbering (basic), tables (rows/cells/spans),
      inline images from `word/media/` (PNG only), section page size/margins.
- [ ] `.docx` **export**: the same subset, valid `[Content_Types].xml`,
      `_rels/.rels`, `word/_rels/document.xml.rels`, deterministic output.
- [ ] `.xlsx` **import/export**: sheets, cells (number/string/boolean/formula),
      `sharedStrings.xml`, number formats (subset), column widths, merged cells.
      Formula export is text-pass-through (`<f>`), values written as
      `<v>` so a viewer without recalculation still shows numbers.
- [ ] `.pptx` **import/export**: slides, text frames, placeholder positions,
      simple shapes, images; notes slide text.
- [ ] **Refusal, not corruption:** an unsupported construct is reported in the
      window and in the log (`[aodoc] import: skipped <w:sdt>`), and the import
      continues. Nothing is silently dropped without a receipt.
- [ ] A documented subset table in `docs/office.md`, kept in sync by a unit
      test that fails when the code and the table disagree.

#### Test gate

- `tests/unit/test_ao_ooxml.c`: parse fixtures (hand-written minimal
  documents, one per feature), assert model contents; export then re-import →
  model equality; hostile inputs (XXE-shaped entity, a 100 MB `document.xml`
  claimed size, a zip entry that is itself a zip).
- `tests/integration/cases/test_office_docx.sh`: import a fixture `.docx` from
  the initrd, print the paragraph/table/run counts from the app, export it,
  and assert the exported zip's part list and CRCs on the host.
- Host-side fidelity receipt in CI: `python3 tools/office/check_docx.py` (new
  small tool) validates the produced package's XML well-formedness and part
  layout — no external office suite needed in CI.

#### Deliverable

`patches/OFFICE_OF8_ooxml.patch`

---

### Phase OF9 — Export, the suite shell, and the acceptance soak

**Objective:** the suite as a product: start centre, PDF, recent files, and
the long-run evidence.

#### Tasks

- [ ] **PDF 1.4 writer**: page tree, text with an embedded Type3 bitmap font
      (our glyphs, per-size bitmaps), rectangles/lines for tables and shapes,
      PNG images as `/Image` XObjects (using the existing decode + a simple
      Flate-free stream or the stored `wv_inflate` inverse), text extraction
      not required. Cyrillic works by construction.
- [ ] Export targets: PDF (Writer/Sheet/Slide), HTML (Writer/Sheet), CSV
      (Sheet), PNG (current page/slide), plain text (Writer).
- [ ] **Start centre** (`/apps/aoffice`): new/open/recent per document type,
      recent-files list persisted in `/documents/.recent`, three big buttons,
      an honest "what this cannot do" panel.
- [ ] Start-menu entries and desktop icons for the four apps (`gui_apps[]` is
      the one place paths are hardcoded — `kernel/gui/gui.c:1888`).
- [ ] Suite-wide keyboard map documented (`Ctrl+S/O/N/P, Ctrl+Z/Y/X/C/V/F,
      Ctrl+B/I/U`), one table in `docs/office.md`, tested once.
- [ ] `docs/office.md` completes: formats, subsets, budgets, limits, and the
      "no macros / no SQL / PDF-only print" statements.
- [ ] **Acceptance soak** (the point of the whole plan): open one document of
      each type, edit each, export each to PDF, reopen each, run the slide
      show, for 30 minutes, through VNC with real input; zero leaks, zero
      kernel faults, repaint budget held, memory flat.

#### Test gate

- `tests/unit/test_ao_pdf.c`: byte-level structure (header, xref table,
  objects, content stream length), a page with Cyrillic text, a page with an
  image; the produced PDF is parsed back by a small host checker
  (`tools/office/check_pdf.py`) that validates the xref offsets.
- `tests/integration/cases/test_office_export.sh`: export each document type,
  assert file sizes and the app's receipts, `sync`, and confirm the files are
  readable after a reboot (`/disk` persistence).
- `tests/integration/cases/test_office_soak.sh` (`SLOW_CASES_RE`): the 30-minute
  loop with periodic receipts; asserts memory flat within 2 % and no
  `Page Fault|kernel panic`.
- **Live VNC acceptance:** the full scripted pass, screenshots at every step,
  plus the two failure modes that hid for months in this tree — a drag to a
  screen edge (the landed snap fix) and an app that animates its own content
  (the compositor-wake fix). Both must still hold with a suite running.

#### Deliverable

`patches/OFFICE_OF9_export_shell.patch`

#### Definition of done

A user can be handed this ISO, write a document with tables and an image,
compute a spreadsheet with formulas and a chart, build a three-slide deck,
export all three to PDF, reopen them after a reboot, and the machine is still
healthy after half an hour.

---

## 5. Order and rationale

The order is not "easiest first", it is "what would force a rewrite later":

1. **OF0 before everything.** The frame budget and the shell around the apps
   (taskbar overflow, menu geometry) are cheap now and expensive after four
   apps exist. Both problems are already visible today with eight windows open.
2. **OF1 before OF2.** Text without Cyrillic would have to be replaced wholesale
   once it appears — every buffer, every metric, every save format. Fonts and
   Unicode are the one thing that cannot be retrofitted.
3. **OF2 before OF3.** Rich layout on top of a broken edit model is a rewrite;
   the edit model is 300 lines, tables are 800.
4. **OF4 (headless engine) before OF5 (grid).** The formula engine is fully
   testable without pixels. Writing it behind a GUI means every bug arrives as
   "the window is wrong".
5. **OF6 after OF3.** Slide is the proof that the text engine is a library: it
   is the second consumer of the same layout code, and if it needs changes,
   they belong in `libaodoc`, not in two apps.
6. **OF7 before OF8.** ZIP is a hard prerequisite for OOXML. Writing OOXML
   against a native XML format and adding packaging later would mean writing
   the OOXML reader twice.
7. **OF9 last, but its PDF task is small.** The PDF writer depends on OF3's page
   model and OF7's stream handling; the start centre depends on everything.

---

## 6. Effort estimate

| Phase | Lines of code (estimate) | Main risk |
|---|---:|---|
| OF0 | ~1 400 | frame budget under TCG; taskbar/menu geometry in the kernel |
| OF1 | ~2 200 | producing a Cyrillic proportional bitmap face that is readable at 10 px |
| OF2 | ~3 800 | the edit model + undo + XML reader correctness |
| OF3 | ~4 200 | table layout; page model that two consumers accept |
| OF4 | ~3 400 | formula semantics and cycle detection time limits |
| OF5 | ~3 000 | virtualised grid correctness under scroll |
| OF6 | ~2 600 | fullscreen show without a chrome-less window regressing |
| OF7 | ~1 900 | ZIP edge cases (EOCD search, CRCs, hostile sizes) |
| OF8 | ~4 800 | OOXML is a swamp; the subset discipline is the whole difficulty |
| OF9 | ~2 700 | PDF xref correctness; the soak finding a leak late |
| **Total** | **~30 000** | |

For comparison: `drivers/` is 15 565 lines, `lib/` (including libgl at 17 500)
is the largest subtree in the project. An office suite here lands at roughly
**twice the driver tree** — which is what "full office suite" costs, and why
this plan is ten phases and not one.

---

## 7. Risks

| Risk | Why it is real here | Mitigation |
|---|---|---|
| Font quality stalls the whole plan | A bitmap face readable at 10 px, with Cyrillic, is art as much as code | Two fallbacks: ship a BDF from a public domain 8×13/10×20 face with a Cyrillic block, and a monospace fallback that always renders; OF1's gate accepts both |
| Performance under TCG | 46–48 ms for a full 800×600 blit was measured live; a naive writer repaints the window per keystroke | Dirty-rect-only rule (D5), OF0 budget gate, glyph cache, per-window back buffer |
| OOXML scope creep | Every user has a document that will not round-trip | Documented subset + "refusal, not corruption" receipts + a test that keeps the doc and the code in sync |
| Formula semantic drift | Spreadsheet users expect exact Excel behaviour for edge cases | Unit tests per function with documented deviations; log the deviation list in `docs/office.md` |
| Undo memory | 512 MiB guest, 16 MiB image limit | Command log with budget (D6), measured in OF2 |
| Input ceiling | Burst injection lost characters in the live pass; 7 ms HID poll, 128-event ring | OF2's 20 cps zero-loss gate; if it fails, the fix is a bigger per-window event queue, and it is a *kernel* fix that must be scoped in OF2 |
| Kernel coupling | Start menu table, menu geometry, taskbar overflow all live in `kernel/gui/gui.c` | OF0 does them first, with their own integration cases, so later phases never touch the kernel |
| Process leak after window close | `ps` in the live pass still listed a `/bin/hello` whose window had been closed (unconfirmed, one observation) | OF9's soak asserts a flat process count; if confirmed it becomes its own small fix in OF0 |
| PDF fidelity expectations | "Export to PDF" invites comparison with LibreOffice output | The doc states Type3 bitmap text, no font subsetting, no reflow; the gate is a valid, openable PDF, not a pixel match |

---

## 8. What this plan does not do

* **No macros, no scripting, no embedded Basic/JS.** (D8)
* **No database application (`Base`), no SQL engine.** A real SQL layer is its
  own plan (and its own security story).
* **No print subsystem**: no spooler, no printer driver, no CUPS-shaped layer.
  "Print" is PDF export (D9).
* **No collaborative editing, no change tracking, no comments** (they need a
  network protocol and an identity model).
* **No hyphenation, no spell check, no thesaurus** (needs dictionaries and a
  licence decision).
* **No animations or transitions** in slides beyond advance-on-click.
* **No TrueType/OpenType rendering, no hinting, no subpixel antialiasing** —
  bitmap faces and synthesis only.
* **No OOXML fidelity guarantee** beyond the documented subset, and no
  `.doc/.xls/.ppt` (legacy binary) support at all.
* **No kernel font API**: the suite owns its glyphs (D2); the kernel keeps its
  VGA font.
* **No multi-window-per-document**, no tabbed documents in OF0–OF6 (a start
  centre opens documents in their own windows).

---

## 9. How the gates are run

```bash
# host unit tests (adds the ao_* binaries to the existing target)
make test-unit

# one integration shard, and the partition check that keeps CI honest
tests/integration/run_all.sh --group office
tests/integration/run_all.sh --check-groups

# live acceptance through VNC (the harness that found the 2026-11-08 bugs)
/home/user/vnc/drive.sh            # snap/click/dbl/rclick/drag/typ/key
/home/user/vnc/alive.sh NAME       # ALIVE or FROZEN from the taskbar clock
/home/user/vnc/btn.py SHOT.png     # locate window close/maximise buttons by colour
```

Every phase's results section is filled in only from these three sources —
`make test-unit`, the integration case, and a VNC pass with screenshots and
serial receipts — the same standard the rest of the plans hold to.

---

## 10. Progress log

| Phase | Status | Patch |
|---|---|---|
| OF0 | 🟩 done (shell, `/documents`, `libaodoc`, start centre at the floor, budget gate) | `patches/OFFICE_OF0_shell.patch` |
| OF1 | 🟩 done (two BDF faces, Cyrillic, three styles, `ru` layout, codepoint to the window) | `patches/OFFICE_OF1_text.patch` |
| OF2 | 🟨 partial (model, undo, XML format and the Writer app landed; dialogs, clipboard, mouse selection, VNC pass to go) | — |
| OF3 | ⬜ planned | — |
| OF4 | ⬜ planned | — |
| OF5 | ⬜ planned | — |
| OF6 | ⬜ planned | — |
| OF7 | ⬜ planned | — |
| OF8 | ⬜ planned | — |
| OF9 | ⬜ planned | — |
