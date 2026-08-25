# AuraLite OS GUI Browser (`/apps/gbrowser`)

**Status:** Phases W0–W7 of [`WEBVIEW_PLAN.md`](../WEBVIEW_PLAN.md) —
all phases W0–W8 complete. `gbrowser` was retired in W8.

This document states what the web view is, what it deliberately is not, and
what the presentation path costs on this build. It follows the project
convention that limitations are discovered by reading, not by pointing at a
blank window — the window itself carries the same statement.

---

## 1. What it is

`/apps/gbrowser` is a windowed program that renders a *standing page*: a pixel
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
| **HTTPS is real, not complete** | TLS 1.3 + chain validation against `/etc/ssl/roots.pem`.  Missing: P-384/SHA-384 leaf signatures (`example.com` / `ietf.org` still `hrc=-26`), images, JS. | `WEBVIEW_PLAN.md` D6, `docs/live_web.md` |
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
startup (`/apps/gbrowser` prints it on every run):

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
- **`SPAWN_MAX_IMAGE` is 1 MiB.** `gbrowser.elf` is ~130 KiB as of W0; the
  budget for a tokeniser, a DOM, layout, CSS and painting is the remaining
  ~870 KiB, and the failure mode (a diagnosed spawn refusal) must not become
  the only sign the budget was blown.

## 5. Roadmap

| Phase | Deliverable | Status |
|---|---|---|
| W0 | Scaffold: window, event loop, pixel buffer, blit benchmark, this doc | ✅ complete (2026-08-07) |
| W1 | HTML tokeniser (state machine, no recursion) | ✅ complete (2026-08-07) |
| W2 | DOM (flat node array, depth cap, implied structure) | ✅ complete (2026-08-07) |
| W3 | Block layout → display list (iterative, 64 KiB-safe) | ✅ complete (2026-08-07) |
| W4 | Painting: rects, borders, glyphs, clipped scroll | ✅ complete (2026-08-07) |
| W5 | Inline CSS subset (D4) | ✅ complete (2026-08-07) |
| W6 | Navigation: links, history, growing fetch, HTTP/1.1 | ✅ complete (2026-08-07) |
| W7 | `<canvas>` with an OpenGL context via FBO | ✅ complete (2026-08-07) |
| W8 | Retire or keep `gbrowser` | ✅ complete (2026-08-07) — retired |

## 6. The tokeniser (W1)

`userspace/apps/gbrowser/wv_html.{h,c}` turns bytes into a token stream:

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
  at `/apps/gbrowser` startup (`tokeniser smoke`), asserted by
  `test_webview.sh`.

## 7. The DOM (W2)

`userspace/apps/gbrowser/wv_dom.{h,c}` turns tokens into a tree:

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

## 8. Block layout (W3)

`userspace/apps/gbrowser/wv_layout.{h,c}` turns the DOM into a **display
list** (boxes + text runs) — nothing is rasterised yet; W4 paints it.

- Iterative by construction: a (node, phase) walk stack and explicit
  block/inline context stacks, all caller-provided arrays with caps. The
  5 000-box gate document is laid out in-guest on the real 64 KiB stack
  every boot (`layout smoke: PASS`).
- Box model: width/margin/padding honoured; children lay out inside the
  content box. Until W5's CSS lands, values come from a small UA
  stylesheet (body 8 px margin, p margins, h1–h6 bold, ul/ol 32 px indent,
  blockquote margins, hr rule).
- Inline flow: HTML whitespace collapsing (runs → one space, leading
  whitespace dropped), word wrap at the content edge, `<br>`, `<pre>`
  preserving whitespace, inline style stack for `<b>`/`<strong>` (bold),
  `<a>` (blue + underline), `<u>` (underline) — nesting-safe.
- Placeholders: `<img>` 16×16 inline box; `<hr>` rule; `<canvas width
  height>` block (W7 backs it with an FBO).
- Hidden elements (head, title, style, script, meta, link, base, noscript)
  produce no boxes.
- Gate: `tests/unit/test_wv_layout.c` — 79 host checks, 0 failures: wrap,
  nested indent (summed margins/padding), an exact expected display list,
  whitespace, `<pre>`/`<br>`, styles, placeholders, **5 000 boxes in
  1 064 µs (budget 7 500 µs)**, 1 000 fuzz iterations. In-guest:
  `layout smoke` asserted by `test_webview.sh`.

## 9. Painting (W4)

`userspace/apps/gbrowser/wv_paint.{h,c}` turns the display list into pixels
— the first phase with something on the screen.

- The project's PSF2 VGA 8×16 font is embedded as data (the same blob the
  kernel console uses); glyphs are rasterised MSB-first with clip-aware
  row/column skipping.
- Synthesised bold (plan D7): each glyph drawn twice, one pixel apart.
  Underline: a bar at the glyph baseline.
- Culling: a box or word whose page y falls entirely outside the viewport
  is skipped before a single pixel is touched.
- **Scrolling** (the 0.068 ms path): `wv_paint_scroll()` memmoves the
  retained buffer; `wv_paint_band()` repaints only the exposed band, with
  boxes clipped to the band so a box straddling the band edge cannot erase
  content painted above it.
- **The hash gate**: `wv_paint_hash()` (FNV-1a) pins a fixed document to
  reference `0xA29E776C` in the host test, and `/apps/gbrowser` prints the
  same value at boot (`paint smoke: PASS`) — the guest and host agree, so
  a rendering change is a deliberate act with an updated expectation.
- Gate: `tests/unit/test_wv_paint.c` — 42 host checks, 0 failures,
  including scroll equivalence on a 2 000-line page with an opaque
  `<body>` box, and a 10 000-line page scrolling in **144 µs** (budget
  7 500 µs). In-guest: `paint smoke` + `paint scroll smoke`, asserted by
  `test_webview.sh`.

## 10. Inline CSS (W5)

`userspace/apps/gbrowser/wv_css.{h,c}` implements the D4 subset from
`style=` attributes and `<style>` blocks:

- Properties: `display` (block/inline/none), `color`, `background-color`,
  `width`, `height`, `margin`, `padding` (1-2-4 value forms), `border`
  (width; the style keyword and colour are parsed and ignored per CSS),
  `font-weight`, `text-align` (left/center/right — lines are tracked and
  shifted on wrap and at block close).
- Selectors: tag, `#id`, `.class`, `type.class`, comma lists. No
  combinators; no specificity — later rules win, inline wins.
- Colours: `#rgb`, `#rrggbb`, the 16 named colours.
- Error handling per CSS: unknown properties ignored, unknown selectors
  skipped, a malformed declaration never discards the rest of its block.
- The demo page carries a 5-rule stylesheet (h1 colour + centring, p
  margins, green links, `.note` background + border, `#footer` right-
  aligned grey) and boots with `css smoke: PASS (styled=0x4d394d5c,
  plain=0x2f39d54f)` — the stylesheet demonstrably changes the output.
- Gate: `tests/unit/test_wv_css.c` — 71 host checks, 0 failures, every D4
  property with a test that changes the output; 500 fuzz iterations.

## 11. Navigation and networking (W6)

The web view is now a usable (if humble) browser:

- `wv_url.{h,c}` — URL parsing + relative resolution (32 host checks).
- Fetch path (REALINTERNET_PLAN **X6**): the browser goes through
  **libahttp's keep-alive client** (`ahttp_client`), shared across
  navigations.  http:// and https:// are both first-class: TLS 1.3 with
  chain validation against `/etc/ssl/roots.pem`, one cached connection
  per origin (reuse logged as `[ahttp] keep-alive:` lines), http→https
  redirects followed automatically, POST/PUT support behind a bounded
  interface.  `wv_http.{h,c}` keeps its request/response helpers for the
  host unit tests; the guest fetch path itself is `wv_fetch_url()` in
  `gbrowser.c`.
- Scheme-less input in the address bar / steps hook defaults to
  `https://` (secure by default); `wv_url`'s parser default is unchanged
  so the host tests stay stable.
- Links live in the display list (`link_off` on text items); clicking one
  (or the Back/Go buttons and the address bar in the chrome strip)
  navigates. History: 8 entries, back re-fetches without pushing.
- Test hooks (written before `run webview`, because the init shell blocks
  on a running child): `/tmp/webview.url` = initial page,
  `/tmp/webview.steps` = `link 0|back|https|nav <url>` actions.
- Gate: `tests/integration/cases/test_webview_net.sh` — a real host server
  serves `/`, `/page2.html`, `/chunked` and `/big` (100 KB); the guest
  fetches home, follows a link, goes back, attempts https for real
  (never the pre-X6 refusal page), decodes chunked and receives 100 KB
  in full.
- Kernel note: the fetch path now rides the BSD-socket API via libahttp,
  so the old legacy-TCP FIN truncation workaround is gone.

## 12. `<canvas>` with OpenGL (W7)

The only place in the web view where GL is used (plan D1 — "building one
that can host OpenGL content is the right reason"):

- `<canvas width height data-scene="cube">` becomes a normal layout box;
  `wv_canvas.c` (the only GL-importing module) renders the built-in cube
  scene into an FBO (G12) and reads it back with glReadPixels.
- **One-shot render at page load** into a cached buffer: GL is NOT on the
  paint critical path. A page without a canvas never touches libgl — the
  W4 demo-page reference hash is unchanged. Per frame the canvas costs
  only a clipped blit, scrolled with the page.
- Measured: 64×48 cube in ~58 µs on the host (sub-tick in QEMU TCG) —
  the number the plan's §1 table was missing.
- No JavaScript (D5): a page can only *ask* for a scene via
  `data-scene`, it cannot drive the canvas imperatively.
- Gate: `tests/unit/test_wv_canvas.c` — 21 host checks (byte-identical
  re-renders, face colours visible, blit clipping incl. off-screen and
  scroll, 2 000 fuzz blits); in-guest `canvas smoke: PASS` and a
  `/canvas.html` page in the networking test that renders text AND a 3D
  cube on one page.
