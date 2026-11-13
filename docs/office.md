# The office suite — what it is, what it is not, and what it costs

Status: **OF0 in progress** (see `docs/plans/OFFICE_PLAN.md` §10). Nothing here
is a promise about a finished product; it is the contract the rest of the plan
is written against, stated once so that no later phase has to re-argue it.

## 1. What this will be

Three applications over one document core:

| Application | Binary | Native format |
|---|---|---|
| Writer (word processor) | `/apps/awriter` | `<aodoc>` (XML, in a ZIP container from OF7) |
| Sheet (spreadsheet) | `/apps/asheet` | `<aosheet>` |
| Slide (presentations) | `/apps/aslide` | `<aoslide>` |
| Start centre | `/apps/aoffice` | — |

`libaodoc` (document model, undo log, dirty-rect canvas) and `libaoglyph`
(font rasteriser) are shared by all four. OOXML (`.docx`, `.xlsx`, `.pptx`) is
an **import/export bridge** with a documented subset: a file that uses
something outside the subset is *refused with a receipt* — `[aodoc] import:
skipped …` — never silently converted into something that looks fine and is
not.

## 2. What this will not be

Decided in OF0 and not reopened by later phases (plan §8):

* **No macros, no scripting, no formula language beyond spreadsheet
  expressions.** No Basic interpreter, no automation hooks.
* **No database application.** No Base equivalent, no SQL engine.
* **"Print" means "Export PDF".** No print spooler, no printer drivers, no
  paper-size negotiation. PDF 1.4 with a Type3 bitmap font (OF9) so the suite
  can produce a page without shipping a TrueType rasteriser.
* **No TrueType/OpenType or hinting.** Glyphs come from the in-tree PSF2 face
  plus BDF bitmap faces (OF1). A document that asks for a font the machine does
  not have falls back to the monospace face **and says so**; it does not
  substitute silently and it does not refuse to open.
* **No hyphenation, no spell-check, no grammar tools.** No dictionaries ship
  with this system.
* **No legacy binary formats.** `.doc`, `.xls`, `.ppt` are refused by name.

These are not gaps discovered later. They are the scope, and the plan's gates
assert the refusal paths so that "not supported" is a receipt rather than a
crash or, worse, a silent mangling.

## 3. The shell the suite lives in (OF0)

The suite cannot open a file dialog if the desktop cannot reach its own
windows, so OF0 fixed the shell first.

### 3.1 Taskbar: no window without a reachable button

The live pass of 2026-11-08 opened ten windows and got **Start plus six
buttons**; the rest were drawn nowhere and could not be clicked, because
`draw_taskbar()` and `taskbar_hit()` each carried their own copy of the button
arithmetic and each ran out at a different place.

Now there is one implementation of that arithmetic —
`kernel/gui/taskbar_layout.h` — and both the renderer and the hit test call it:

* buttons are 130 px while they fit, then shrink to fit, never below 44 px;
* when even 44 px does not fit, the last slot becomes a `»` button and the
  windows without a button are listed in a dropdown above it;
* the dropdown pages, so the number of open windows can never exceed the
  number of reachable ones;
* `tests/unit/test_taskbar_layout.c` asserts, for every window count from 1 to
  256 and every screen width the GUI runs at, that no button overlaps another,
  none runs under the clock, every drawn button is hit exactly where it is
  drawn, and every window is reachable — button or list. `make test-unit` runs
  it.

Live evidence: 18 windows at 1024×768 → 16 shrunk buttons plus `»`; clicking
`»` lists the two window without a button, and clicking `»` again closes the
list (`vnc/shots/of0b_*.png`).

### 3.2 Start menu: grouped, and as tall as its contents

`gui_apps[]` was ten entries in a fixed 220 px menu — sized for exactly those
ten, so a suite that adds four more would have drawn its last rows under the
taskbar while the hit test kept mapping them to entries.

* entries carry a group (`Applications`, `Office`, `System`); the `Office`
  group is empty until OF9 adds the start centre;
* the dropdown is rendered from a derived row list (group headers + entries),
  and **the hit test walks the same list** — a label cannot drift from the row
  it launches;
* the height derives from the row count.

`tools/check_start_menu.py` enforces all of this over the source: every menu
path must name a binary the image builds (a stale `/apps/...` path is a row
that silently does nothing), every entry must carry a known group, and neither
the renderer nor the hit test may re-introduce its own geometry. Its
`--selftest` injects five defects, including the exact shape of the 2026-11-08
bug, and requires a complaint for each.

### 3.3 `/documents`

The suite saves to one path: `/documents`, a tmpfs volume mounted at boot
(`kernel/fs/documents.c`, `kernel/fs/tmpfs.c`). It has its own volume rather
than a directory inside `/tmp`, so scratch traffic cannot delete a document.

**It does not survive a reboot, and this build says so.** The plan asked for
`/disk/documents`; diskfs — the tiny persistent AUFS at `/disk` — is flat and
has no `mkdir`, and the writable filesystems that do have directories
(`/fat`, `/ext4`, …) mount only when their device is present, which it is not
on a normal boot here (`[diskfs] no AHCI disk available`). When such a
filesystem *is* present, `documents_init()` creates `<mount>/documents` and
prints that receipt; the mount point still stays tmpfs because this VFS
follows a symlink when resolving a path but not when creating one, so
`/documents → /disk/documents` produced `EROFS` on the first save (that was
the first implementation; the receipt is in the boot log history, and the
design note is at the top of `kernel/fs/documents.c`). Making the durable
directory the live one is tracked in `TODO.md`.

Boot receipt:

```
[tmpfs] writable in-memory filesystem ready (4 volumes, 256 files max each)
[vfs] mounted '/documents'
[documents] /documents (volatile: tmpfs, lost at power-off)
[documents] PASS: write/read/unlink through /documents
```

and, from the shell, `echo hi > /documents/note.txt` / `cat
/documents/note.txt` → `hi`.

### 3.4 The start centre, and the floor it stands on

`/apps/aoffice` is reachable from Start ▸ Office ▸ Office and from the Office
desktop icon. Today it is OF0's definition of done rather than a program:
it draws the page furniture (a ruler with centimetre ticks, a margin box, a
text area, a caret and a status strip), types into itself, and reports what
each repaint costs. The document model arrives in OF2, the layout engine in
OF3, and the start centre that lists Writer/Sheet/Slide replaces this window in
OF9 — the plumbing (`libaodoc`, the canvas, the budget harness, the menu entry,
the CI case) is what this phase contributes.

Its keys are Enter (split), Backspace (delete/join), arrows/Home/End, Esc to
close. Everything it shows is ASCII: the console font is one CP437 face until
OF1 ships a real rasteriser, and this window was the first place where typing
an em dash turned into mojibake — recorded here because it is exactly the
problem OF1 exists to solve.

## 4. The frame budget (measured, not assumed)

The suite may not repaint the world on every keystroke. That is decision D5,
and it is now a gate rather than an intention: `aoffice --bench` measures the
three repaint classes in the guest, and `test_office_shell.sh` asserts them.

### 4.1 What a full repaint costs here

Full-surface blit, the reference point for "how slow is this machine":

| Measurement | Value | Where it came from |
|---|---|---|
| Full 800×600 blit, software VGA in QEMU | **12.2 ms/frame** (200 frames) | `[gbrowser] blit 800x600: 12171 us/frame` |
| The same benchmark in the 2026-11-08 session | 48.0 ms/frame | `[gbrowser] blit 800x600: 48030 us/frame` |
| 5 000-box layout | 20.2 ms (10.1 ms in the other session) | `[gbrowser] layout smoke: PASS (…)` |

Two boots of the same code differ by 4× on the same benchmark, so an absolute
millisecond threshold would be a coin flip. The gate is therefore **relative**.

### 4.2 What a keystroke costs, and what it asks the compositor to do

From `run aoffice --bench` in the guest (window 640×440 = 281 600 px):

```
[aoffice] budget: full=3030.3 us line=50.5 us caret=10.1 us (n=10/200/1000)
[aoffice] budget: dirty pixels full=190400 line=8640 caret=56 (window 640x440 = 281600 px)
AOFFICE BUDGET: PASS (time: caret 0% of full, line 1% of full, limits 5%/20%;
                   pixels: caret 0.01% of the window, line 3.06%, limits 0.1%/5%)
```

Two different statements, deliberately separated:

* **Time** — what the application spends asking for a repaint. A full repaint
  is ~3 ms of drawing through `ag_*` syscalls; a keystroke is ~50 µs and a
  caret move ~10 µs. Ratios: 1 % and 0.3 %.
* **Pixels** — what the compositor is asked to composite. This is the number
  that has to hold on a 1920×1080 screen too, and it is exact: a caret move
  dirties **56 px** (two 2×14 caret cells) against a 281 600 px window, i.e.
  **0.01 %**; a keystroke dirties the line band plus the status strip,
  **8 640 px**, i.e. **3.06 %**.

The bench has to be careful about the guest clock, which ticks at 1 ms: a
caret repaint is shorter than one tick and measured as `0.0 us` until the loop
runs 1 000 iterations. That is why the pixel gate exists — it cannot be fooled
by the clock.

### 4.3 The same claim, live

A keystroke typed into the real window, with two screenshots taken either side
of it (`vnc/shots/of0c_A.png`, `of0c_B.png`, window-aligned diff):

```
changed pixels: 1747 = 0.62 % of the window
bands with changes: (88,102) and (374,383)
```

Two bands: the 16-px text line the caret is on, and the status strip. Not the
window, and not the page. That is OF0's definition of done, filmed.

### 4.4 What is still not measured

Per-keystroke cost *inside a real document* — re-flowing a paragraph, laying out
a table row, re-wrapping a line — needs the layout engine that arrives in OF2
and OF3. The canvas helper and the bench harness already measure the right
thing, so those phases extend the bench rather than build a new one; §4.2/4.3
gets a row per phase that touches the paint path.

## 5. The text foundation (OF1)

The suite could not set a document before this phase: the tree had exactly one
font and two Latin layouts. What OF1 lands, and what it refuses to pretend:

### 5.1 Two faces, generated, checked in

`userspace/office/fonts/ao_sans_11.bdf` (173 glyphs, proportional, advance 4–13
px, ascent 11 / descent 3) and `ao_mono_16.bdf` (173 glyphs, 8×16). Both carry
Cyrillic U+0410–U+044F plus Ё/ё, « » — – … № — `cyrillic 64/64, typographic
4/4` is a receipt the guest prints, not a claim. They are generated by
`tools/gen_office_fonts.py`, which also rewrites them: `--check` fails the gate
the moment a face on disk differs from what the generator produces.

They are **bitmap** faces on purpose. A TrueType rasteriser with hinting is a
different project (§2 says so), and a BDF is text a human can fix — the phase's
worst bug was found by reading one.

### 5.2 `libaoglyph`: load, decode, draw, synthesise

`ao_font_load_bdf()` / `ao_font_load_psf2()` parse into a **caller-owned
arena** (the app lends two 192 KiB buffers; the library never allocates) with a
hard `AO_GLYPH_MAX 512` cap — the 513th glyph is refused with
`AO_FONT_CAPACITY`, not truncated. `ao_glyph_draw()` blits one glyph through a
clip rectangle; `ao_text_draw()` walks a UTF-8 string and reports its advance
width.

Bold and italic are **synthesis**, and the doc says so: bold is a 1 px right
smear (the same 18 glyphs go 293 → 455 pixels), italic is a `dy/4` shear. A
pixel count cannot grade a shear — a slanted row has the same area — so the
gate grades geometry (`top row starts at x=14 vs 13`).

### 5.3 Unicode on the way in, and where it stops

The keyboard contract, end to end:

| layer | carries | note |
|---|---|---|
| `keymap_lookup()` | the console byte (`char`) | ASCII and control only; a Cyrillic letter answers 0, because the VGA font cannot draw it |
| `keymap_unicode()` | `uint32_t` codepoint | the single authority; `us`/`de` derive through CP437, `ru` reads `ru_ulo/ru_uhi` |
| `kb_event_t.cp` | codepoint per key event | filled from the active layout |
| `gui_event_t.data` | codepoint to the window | `route_key_event()`, `kernel/gui/gui.c` |
| app | `data` if it is a character, else `key` | an app that ignores `data` sees the events it always saw |

The Russian table is the real ЙЦУКЕН: `йцукенгшщзхъ` on `0x10..0x1B`,
`фывапролджэ` on `0x1E..0x28`, `ячсмитьбю` on `0x2C..0x35`, `ё`/`Ё` on `0x29`,
and CapsLock is an XOR fold with Shift — including the one letter outside the
straight `-0x20` mapping.

**The defect this phase found:** the USB HID path (the only keyboard this
machine has) queued the HID *usage id* into the scan-code field, so
`evt_enqueue()` could not see the layout and every window got `cp = 0`. The
Russian layout typed «ghbdtn» where «привет» was asked for. `usb_usage_sc1()`
translates the usage to a set-1 scan code first, and the phase gate is a
screenshot of «привет» typed by hand.

### 5.4 Why a blank glyph is worse than a missing one

The BDF emitter padded each row left-aligned to the glyph box instead of
MSB-aligned within `ceil(w/8)` bytes. For an 8-wide glyph the two are
identical, so every letter rendered perfectly — and **every narrower glyph came
out blank**: the comma, the period and the em dash. A document that reads
«Привет мир  2026» is a document with a spelling problem, not a font problem,
which is why it survived a screenshot review.

Two things now prevent a repeat: the generator shifts its bits by
`stride*8 - w`, and `aoffice --font-test` prints a receipt per glyph
(`glyph U+002C: w=4 h=3 adv=4 yoff=-1 paints=4`) and fails on any non-space
glyph with `paints=0`.

### 5.5 Editing text, and the two limits that remain

A key event arrives as a codepoint; OF1's editor encoded it (`ao_utf8_encode`)
into a fixed row. That row was a **byte** cap -- 512 bytes, so a Russian letter
cost two and a line held half as many Cyrillic characters as Latin ones -- and it
is gone: OF2 (§6) replaced the rows with a document pool, and the display cache
is rebuilt from it after every edit. The 64-line display window is still a
window, not a limit on the document.

## 6. The document (OF2)

The Writer is no longer a line array with a caret.  The text lives in
`libaodoc/aomodel.c` as **byte offsets into one pool**, paragraphs are the
surviving `'\n'` bytes, and a paragraph owns a contiguous slice of one run
array whose entries carry style bits, size and font.  `ao_model_valid()` checks
every structural rule -- paragraphs tile the text, runs tile each paragraph,
adjacent equal runs are merged, no run is empty except a lone one in an empty
paragraph -- and it is called after **every** operation of the 10 000-operation
fuzz.  Positions never land inside a codepoint: an insert snaps forward, a
delete snaps outward, a style range snaps to codepoint edges.

Undo is snapshots, not inverse operations (`libaodoc/aoundo.c`): the inverse of
"merge two paragraphs and re-target a run array" is a structural transplant,
while a serialised copy is correct by construction and reuses the save format.
The budget (256 KiB per stack) evicts oldest-first and typing coalesces into one
entry per run, so a page of prose does not cost 2 000 undo steps.

The file is `<?xml version="1.0" encoding="UTF-8"?><aodoc version="1">` with one
`<p>` per paragraph and one `<r>` per run (`libaodoc/aoio.c`).  Three rules in
it are worth knowing before changing it:

1. **The paragraph's terminating newline belongs to no run.**  It is stripped on
   save, re-added on load, and a run holding only the newline is not written at
   all -- otherwise two documents that mean the same thing would serialise
   differently and "save → load → save is byte identical" would be false.
2. **Attributes are stamped on load, not inherited.**  Inserted text inherits
   the attributes of the run it lands after, so a plain `<r>` following a bold
   one must clear the bits explicitly or the whole paragraph comes back bold.
3. **Alignment is applied after the document is read.**  The model creates a
   paragraph only when something is written into it, so an empty paragraph has
   no index while the reader is at its `<p>` tag; the reader collects one
   alignment byte per paragraph and applies them at the end.

A file that is not this format -- wrong or missing `version`, unclosed tag, bad
entity, nesting deeper than 16, an attribute longer than 256 bytes -- is
**refused, and the document is left empty**.  A half-loaded page would be worse
than a message.

The OF2 closure adds the remaining editing verbs on the same model:
**Ctrl+Left/Right** word navigation and **Ctrl+Home/End** (word classes live
in `ao_model_word_left/right`, so a Cyrillic word is one word), **click-to-place
and drag-to-select** (plus double-click word select; the pixel→byte mapping
walks codepoints through `ao_text_width`, never `column * 8`), **Ctrl+C/X/V**
through the kernel text clipboard — a selection over its 512-byte cap is
*refused with a receipt*, not silently truncated — **find with a match count**,
**Ctrl+Shift+H replace-all** (`ao_model_replace_all`, one undo step, a
replacement containing the needle still terminates), and a **shared open/save
dialog** (Ctrl+O / Ctrl+Shift+S: path box, `/documents` list, and an overwrite
confirmation before an existing file is truncated).  Ctrl+S remains the quick
save to the current path.  The headless gate exercises word navigation, the
mouse mapping, replace-all and the dialog state; the clipboard verbs and the
dialog's pixels are window-path code.

What the Writer still does not do (and the plan keeps it 🟨 for): only the
live 20 cps VNC acceptance pass remains open.

### OF3: the page (layout)

`lib/libaodoc/aolayout.c` lays the model out on a page (A4 points, margins,
header = title, footer "Page N of M"): greedy word wrap measured through the
consumer's face, paragraph styles as *block* properties (Heading 1–3, Quote,
Bullet, Number — Ctrl+1..7, Ctrl+8 toggles a page break) with run formatting
layered over them, numbered-list markers with a documented continuation
rule, tables as TABLE paragraphs (one TSV row each; the grid algorithm owns
rects, merges by colspan, header shading, borders) and image paragraphs
sized from their PNG IHDR, scaled to fit and height-capped.  F6/F9/F10
insert a 4×4 table / a row / delete a row; F8 switches between the draft
rows and the page view, which is also the print preview — the PDF writer of
OF9 consumes the same layout tables.  `aoffice --layout-test` is the guest
gate (build → layout → save → reopen → identical layout), and
`tests/unit/test_ao_layout.c` grades wrap, pagination, a 20-column grid and
image fitting on the host.  Honest remainder: an image paragraph is a
correctly *sized and placed box* — the PNG pixel decode (`wv_image.c`) is
not yet wired into the Writer, and the page view's scroll is VNC-open.

## 7. Where the constraints come from

Every number above was measured on this tree, not assumed:

* *before OF1* the tree had one 8×16, 256-glyph monospace font with no
  Cyrillic (`psf2_default_font.inc`) and two layouts, `us` and `de`, as 8-bit
  `char` tables — hence the rasteriser and the `uint32_t` tables in §5 rather
  than "use the system font". The key field was already `uint32_t` end to end,
  which is why the `ru` layer needed no ABI change;
* no ZIP anywhere in the tree, but an inflate implementation exists
  (`userspace/apps/gbrowser/wv_inflate.c`) — hence OF7's reader-then-writer
  order;
* `SPAWN_MAX_IMAGE` is 16 MiB — the suite's binaries are sized against that.

## 8. Running the gates

```bash
make test-unit                                        # incl. test_taskbar_layout,
                                                      # test_ao_glyph, test_ao_utf8,
                                                      # test_keymap (all three layouts),
                                                      # test_ao_doc, test_ao_undo
python3 tools/check_start_menu.py --check             # shell invariants
python3 tools/check_start_menu.py --selftest          # …and that it can fail
python3 tools/check_plan_tables.py --check            # plan/TODO/ledger agree
/home/user/vnc/drive.sh                              # live pass, screenshots
```

`run aoffice --font-test` in the guest is the text gate: it loads both faces,
measures «Привет, мир — 2026», draws it in three styles, prints a pixel receipt
per glyph and ends with `AOFFICE FONT: PASS (2 face(s), 0 failure(s))`.
`tests/integration/cases/test_office_font.sh` asserts exactly that output.

`run aoffice --writer-test` is the OF2 gate: it types through the keystroke
path, selects all, bolds, undoes, redoes, saves `/documents/writer-gate.aod`,
reopens it, and ends with `WRITER GATE: PASS` -- and prints the byte counts it
moved on the way.  `tests/integration/cases/test_office_writer.sh` asserts those
receipts and then `cat`s the file, so the format is graded by a second program.

Live evidence for OF0: `/home/user/vnc/shots/of0_*.png`, `of0b_*.png`.
Live evidence for OF1: `/home/user/vnc/shots/of1_*.png` — the specimen strip in
four styles and two faces, and «привет» typed through `kbd ru` from a QWERTY
keyboard.
