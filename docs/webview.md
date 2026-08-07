# AuraLite OS Web View (`/apps/webview`)

**Status:** Phases W0–W2 of [`WEBVIEW_PLAN.md`](../WEBVIEW_PLAN.md) —
scaffold, HTML tokeniser and DOM complete. W3 (block layout) is next.

This document states what the web view is, what it deliberately is not, and
what the presentation path costs on this build. It follows the project
convention that limitations are discovered by reading, not by pointing at a
blank window — the window itself carries the same statement.

---

## 1. What it is

`/apps/webview` is a windowed program that renders a *standing page*: a pixel
buffer written by the program itself and presented with `ag_blit()` — the
same presentation path the renderer will use for the life of this program.

Phase W0 delivers the plumbing and its measured cost:

- a window and an event loop (close, `q`/Esc to quit, wheel and arrow keys to
  scroll);
- a heap-allocated 800×600 × 32-bit page buffer (the user stack is 64 KiB and
  must never hold it);
- the benchmark of the presentation path, run at startup;
- an optional `/tmp/webview.frames` limit (same convention as `/glcube`) so
  automated runs cannot hang;
- the honest limitation statement, drawn in the window itself.

## 2. What it cannot do (as of W0)

These are plan decisions, not TODOs; each has a pointer to where it lives.

| Limitation | Why | Reference |
|---|---|---|
| **No HTTPS** | TLS 1.3 is its own plan of comparable size (crypto, X.509, trust store). A browser that appears to do HTTPS but validates nothing would be a liability. | `WEBVIEW_PLAN.md` D6, `INTERNET_PLAN.md` |
| **No JavaScript** | A JS engine is larger than the whole plan and would not fit `SPAWN_MAX_IMAGE` (1 MiB). Permanent within this plan. | `WEBVIEW_PLAN.md` D5 |
| **No images** | PNG/JPEG/GIF decoders are each a phase in their own right; they belong in a follow-up once boxes exist to hold them. | `WEBVIEW_PLAN.md` §7 |
| **No proportional fonts** | The only rasteriser is PSF2 8×16 monospace. `<b>` is rendered as a synthesised double-strike until a real font path exists. | `WEBVIEW_PLAN.md` D7 |
| **No CSS beyond a named subset** | `display` (`block`/`inline`/`none`), `color`, `background-color`, `width`, `height`, `margin`, `padding`, `border`, `font-weight`, `text-align`. Adding to the list is a decision, not a slope. | `WEBVIEW_PLAN.md` D4 |
| **No standards compliance claim** | This renders a deliberately chosen subset. | `WEBVIEW_PLAN.md` §7 |
| **Response buffer limits** | `gbrowser`'s 16 KiB static buffer is being replaced in W6 by a growing one with an explicit cap. | `WEBVIEW_PLAN.md` W6 |

## 3. The measured presentation path

The plan (§1) measured the obvious alternative before choosing it: writing
pixels directly beats routing page compositing through the software OpenGL
stack by ~30×, so the renderer is 2D and GL appears in exactly one phase —
W7, `<canvas>` — where the page asks for 3D and there is no 2D alternative.

The number this build commits to is the full-page blit. Measured in-tree at
startup (`/apps/webview` prints it on every run):

| Environment | Full-page 800×600 blit | Notes |
|---|---|---|
| QEMU TCG (software emulation, this CI-style sandbox), 1 CPU | **~7.6 ms/frame** (7 575 µs, 200 frames, 2026-08-07) | Whole machine is emulated instruction-by-instruction; syscall + 1.92 MiB copy dominate. Expect ~10–50× faster under hardware acceleration |
| Reference machine from `WEBVIEW_PLAN.md` §1 | 0.125 ms | Native-execution baseline the plan was written against |

Scrolling (W4) reuses the same path: `memmove` of the retained buffer plus a
repaint of the exposed band, measured at 0.068 ms in the plan's reference
measurement.

The frame budget the layout phases must fit is therefore **one full-page
blit per frame plus whatever W3/W4 add**, and the W3 gate asserts a
5 000-box layout completes within it.

## 4. Hard constraints inherited from the kernel

- **The user stack is 64 KiB.** The parser (W1) and the layout walk (W3) are
  iterative by design, with explicit depth caps — not patched after a crash.
  `kernel/proc/guard.c` classifies an overflow as `[GUARD] user stack
  overflow`; W2's gate runs a 10 000-deep document in QEMU where the real
  limit applies.
- **`SPAWN_MAX_IMAGE` is 1 MiB.** `webview.elf` is ~130 KiB as of W0; the
  budget for a tokeniser, a DOM, layout, CSS and painting is the remaining
  ~870 KiB, and the failure mode (a diagnosed spawn refusal) must not become
  the only sign the budget was blown.

## 5. Roadmap

| Phase | Deliverable | Status |
|---|---|---|
| W0 | Scaffold: window, event loop, pixel buffer, blit benchmark, this doc | ✅ complete (2026-08-07) |
| W1 | HTML tokeniser (state machine, no recursion) | ✅ complete (2026-08-07) |
| W2 | DOM (flat node array, depth cap, implied structure) | ✅ complete (2026-08-07) |
| W3 | Block layout → display list (iterative, 64 KiB-safe) | 📋 planned |
| W4 | Painting: rects, borders, glyphs, clipped scroll | 📋 planned |
| W5 | Inline CSS subset (D4) | 📋 planned |
| W6 | Navigation: links, history, growing fetch, HTTP/1.1 | 📋 planned |
| W7 | `<canvas>` with an OpenGL context via FBO | 📋 planned |
| W8 | Retire or keep `gbrowser` | 📋 planned |

## 6. The tokeniser (W1)

`userspace/apps/webview/wv_html.{h,c}` turns bytes into a token stream:

- 18-state machine (WHATWG-shaped, simplified per `wv_html.h`): text, tag
  open, tag name, attribute name/value (double, single, unquoted), comment,
  bogus comment, DOCTYPE, CDATA, self-closing start.
- Character references: `&amp; &lt; &gt; &quot; &apos;` plus `&#NN;` and
  `&#xNN;` (must end in `;`; codes > 0xFF become `'?'` — the font's ceiling).
- **No recursion anywhere**; all storage is a caller-provided arena
  (tokens + attributes + string pool) with fixed caps and a `truncated`
  flag — exceeding a cap never hangs or crashes, the scan drains to EOF.
- NUL bytes are replaced with U+FFFD, rendered as `'?'`.
- Names are lower-cased; attribute values keep their bytes (minus char refs).
- Gate: `tests/unit/test_wv_html.c` — 122 host checks including 3000 fuzz
  iterations and a 64 KiB random blob; every token is walked and every
  offset verified against the arena bounds. The tokeniser also runs in-guest
  at `/apps/webview` startup (`tokeniser smoke`), asserted by
  `test_webview.sh`.

## 7. The DOM (W2)

`userspace/apps/webview/wv_dom.{h,c}` turns tokens into a tree:

- Nodes are a **flat array** linked by indices (parent / first-child /
  last-child / next-sibling) with an implicit document root at index 0 —
  one allocation, and a tree walk that cannot run away.
- An explicit open-element stack with a **depth cap of 512** (the plan's
  D3): past the cap, deeper elements are appended without nesting and
  `truncated` is set. The 10 000-deep document from the plan's gate is
  built **in QEMU on the real 64 KiB user stack** every boot
  (`dom deep test: PASS`), so a regression is caught where the bug is
  visible, not on the 8 MB host stack.
- Implicit closes: `<p>`, `<li>`, `<td>`/`<th>` (never the row), `<tr>`,
  and 14 void elements (`area base br col embed hr img input link meta
  param source track wbr`) never nest.
- Mismatched close tags are reconciled pop-until-match; unmatched end tags
  are ignored. `<b><i>x</b></i>` keeps the text and the tree.
- Gate: `tests/unit/test_wv_dom.c` — 65 host checks, including 2000 fuzz
  iterations with every parent/child/sibling link verified after every
  build. In-guest: `dom smoke` + `dom deep test`, asserted by
  `test_webview.sh`.

If only three phases are ever built, W1–W3 are the ones: a tokeniser, a DOM
and a layout engine are the parts that do not exist anywhere else in the
tree. Paint is a for-loop over rectangles; the hard work is upstream.
