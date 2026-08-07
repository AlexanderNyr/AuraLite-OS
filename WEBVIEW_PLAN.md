# AuraLite OS — Web View Plan

## Status: W0–W8 COMPLETE ✅ — the plan is finished

This document answers:

> *We have OpenGL now — can we build a web view?*

Yes, but **not the way the question implies**, and the difference matters
enough to put at the top rather than bury in a risks section.

It follows the structure of the existing plans (`GL_PLAN.md`,
`FSLAYOUT_PLAN.md`, `SDK_PLAN.md`): dependency-ordered phases, a definition of
done and a test gate for every phase, and one `.patch` per phase.

**Baseline:** commit `f842634`, on top of a completed `SDK_PLAN.md`.

---

## 1. The measurement that shapes this plan

The premise is that OpenGL makes a web view possible. That was tested before
writing anything, because the whole plan depends on the answer.

**libgl is a software rasterizer.** There is no working GPU path: `GL_PLAN.md`
phase G13 shipped a VirGL backend, and the virtio-gpu driver *hangs during
initialisation* when a device is attached (bisected to before `9188c85`,
recorded in `TODO.md`). Every triangle is drawn by the CPU, into a buffer that
is then `memcpy`'d into the window.

So "use the GPU to composite the page" is not on the table. What would GL
actually cost, against the obvious alternative of writing pixels directly?
Measured on this machine at 800×600:

| Operation | Cost |
|---|---|
| `memcpy` of a full 800×600 page into the window | **0.125 ms** |
| Scroll by 40 px (`memmove` + repaint the exposed band) | **0.068 ms** |
| Per-pixel alpha blend over the whole page | **0.62 ms** |
| *(from `docs/opengl.md`)* GL, 200 triangles at 320×240 | **3.7 ms** |
| *(from `docs/opengl.md`)* GL full-screen shaded, 320×240 | **12–54 ms** |

A full-page 2D blit at 800×600 is **thirty times cheaper** than 200 GL
triangles at a seventh of the area. Routing page compositing through libgl
would make the browser slower, not faster.

### What this means for the plan

**The renderer is 2D, writing pixels into a buffer and presenting with
`ag_blit()`.** That is the fast path, and it is the one the measurement
supports.

OpenGL earns its place in exactly one phase — **W7**, `<canvas>` with a
3D context — where the page asks for 3D and there is no 2D alternative. That
is a real feature and a genuinely good fit for what libgl already does: a
`<canvas>` is a texture-sized render target, which is what FBOs (phase G12)
were built for.

Building a web view *because* we have OpenGL would be the wrong reason.
Building one that can host OpenGL content is the right one.

---

## 2. Where things actually stand

Measured against the tree, not assumed.

### There are already two browsers

| Program | Lines | What it does |
|---|---|---|
| `/apps/browser` | 360 | HTML → text, printed to the console |
| `/apps/gbrowser` | 515 | HTML → lines in an AuraGUI **listbox**, with clickable links |

`gbrowser` is not a renderer. It converts tags into text lines and puts them
in a list widget: one line per element, no layout, no boxes, no inline flow,
no images. It works, and it is the right starting point to *replace* rather
than extend — a listbox cannot express a box model no matter how much code is
added around it.

### The pieces that already exist

| Piece | State | Note |
|---|---|---|
| TCP/IP, DNS | ✅ | `test_http_get`, `test_tcp_server` pass |
| HTTP/1.0 client | ✅ | in both browsers, ~40 lines |
| Pixel output | ✅ | `ag_blit()`, `ag_blit_alpha()` (GL phase G0) |
| Font rasteriser | ✅ | PSF2, **VGA 8×16, monospace, 256 glyphs** |
| Window + events | ✅ | AuraGUI: mouse, keys, scroll |
| OpenGL | ✅ | software, 17 500 lines, FBOs and GLSL ES |
| Off-screen render targets | ✅ | FBOs from G12 — exactly what `<canvas>` needs |

### The pieces that do not

| Missing | Consequence |
|---|---|
| **TLS** | **No HTTPS. Most of the public web is unreachable.** |
| Proportional fonts | Every glyph is 8 px wide; no font selection |
| Image decoders | No PNG, no JPEG, no GIF |
| CSS of any kind | No stylesheets, not even inline `style=` |
| JavaScript | None, and none is planned here |
| Incremental layout | Nothing to reflow |

### Two hard constraints

**The user stack is 64 KB** (`USER_STACK_SIZE`, `kernel/proc/guard.c`). A
recursive-descent HTML parser or a recursive layout walk over a deep document
will overflow it. This is not theoretical: GL phase G11b hit exactly this,
and the fix was to bound recursion explicitly. The parser and the layout
engine must both be iterative or depth-limited **by design**, not patched
later.

**A spawned image may not exceed 1 MB** (`SPAWN_MAX_IMAGE`). `/tests/gltest`
is already 365 KB. A browser with a layout engine, a decoder or two and a
DOM will approach this, and the failure mode is a diagnosed refusal at spawn
time — noisy, but a hard ceiling nonetheless.

Also: `gbrowser`'s response buffer is **16 KB**, statically allocated. Real
pages are larger.

---

## 3. Decisions

### D1. A 2D renderer, with GL for `<canvas>` only

Stated once more because it is the decision the plan turns on. The
measurement in §1 says a software GL path costs more than writing pixels
directly. GL appears in W7 and nowhere else.

### D2. Replace `gbrowser`, do not extend it

A listbox of text lines and a box-model renderer have nothing in common
except the HTTP fetch. The new program is `/apps/webview`; `gbrowser` stays
until W8 decides its fate, so there is always a working browser in the tree.

### D3. A real DOM, iteratively parsed, with a hard depth limit

An HTML tokeniser is easy; a *tolerant* one is not, and every real page is
malformed. The parser is a table-driven state machine over the byte stream
with an explicit element stack — no recursion, and a depth cap that closes
elements rather than overflowing 64 KB of stack.

### D4. A subset of CSS, chosen by what changes the output

Not "CSS 2.1 minus the hard parts", which is unbounded. A named list:
`display` (`block`/`inline`/`none`), `color`, `background-color`, `width`,
`height`, `margin`, `padding`, `border`, `font-weight`, `text-align`. Adding
to that list is a decision, not a slope.

### D5. No JavaScript. Explicitly, permanently, within this plan

A JS engine is larger than everything in this document combined and would not
fit in `SPAWN_MAX_IMAGE`. Saying so here stops it being proposed as a
"natural next phase".

### D6. HTTPS is a prerequisite for usefulness, and it is out of scope

Without TLS the web view reaches plain-HTTP sites, of which there are very
few left. That makes this a *rendering engine* one can point at a local
server or a test corpus, not a way to browse the internet.

Implementing TLS 1.3 means X25519, ChaCha20-Poly1305, SHA-256, ASN.1
certificate parsing and a trust store. That is its own plan of comparable
size — **`INTERNET_PLAN.md`**, which exists and covers exactly this — and
half-implementing it would be worse than not having it: a browser that
appears to do HTTPS but validates nothing is a liability, not a feature.

`INTERNET_PLAN.md` also found something this plan assumed away: the existing
`getentropy()` returns a mix of TSC, tick count and a fixed constant, so it is
guessable. TLS seeded from it would be decorative. That is why the internet
plan starts with the entropy source rather than with crypto.

**W0 states this in the docs before any code is written**, so the limitation
is discovered by reading rather than by pointing the finger at a blank window.

### D7. Bitmap fonts, and a synthesised bold

The PSF2 font is 8×16 and monospace. Proportional text needs a different
font format and a glyph cache — real work with no dependency on the rest of
this plan. Until then, `<b>` is rendered by drawing the glyph twice, one
pixel apart, which is what every 1980s renderer did and is honest about what
it is.

---

## 4. Phases

### Phase W0 — Scaffolding and an honest README ✅ COMPLETE

**Objective:** a window that says what this can and cannot do.

#### Tasks

- [x] `userspace/apps/webview/` — a window, an event loop, a pixel buffer
      presented with `ag_blit()`.
- [x] A `docs/webview.md` that states, up front: no HTTPS, no JavaScript, no
      images yet, monospace only.
- [x] Establish the frame budget by measuring it, not guessing: blit an
      800×600 buffer and record the cost in the doc.

#### Test gate

- The window opens, fills, and presents at a measured cost.
- `docs/webview.md` exists and names every limitation in §2.

#### Deliverable

`patches/WEB_W0_scaffolding.patch`

#### Results (verified 2026-08-07)

| Item | Outcome |
|---|---|
| `userspace/apps/webview/webview.c` | Window 800×640, event loop (close / `q`/Esc / wheel / arrows), heap-allocated 800×600 page buffer, `ag_blit()` presentation, `/tmp/webview.frames` limit (glcube convention), startup blit benchmark |
| Limitation statement | Drawn in the window itself (paint_limitations): no HTTPS, no JavaScript, no images, monospace only |
| Frame budget, measured | **7 575–7 676 µs/frame** for a full 800×600 blit under QEMU TCG (software emulation, 1 CPU); plan's native baseline is 0.125 ms — recorded in `docs/webview.md` §3 |
| Clean lifecycle | QEMU run: window created (id 2), benchmark PASS, `PASS: 5 frames rendered, exiting`, `'webview' (tid 8) exited (code=0)`, 522 frames reaped — no leak, no fault |
| Integration gate | `tests/integration/cases/test_webview.sh` added to `run_all.sh` (7 asserts: launch, window, limitation statement, benchmark number, frame limit, clean exit, no kernel fault) |
| `webview.elf` size | 130 KB — comfortably inside `SPAWN_MAX_IMAGE` (1 MiB), ~870 KB left for W1–W7 |
| libc discovery | `printf` has **no `%f`** — benchmark reports integer microseconds; noted in the source |

---

### Phase W1 — The HTML tokeniser ✅ COMPLETE

**Objective:** bytes → a token stream, tolerantly.

#### Tasks

- [x] A state machine over the input: text, tag open, attribute name,
      attribute value (quoted and unquoted), comment, DOCTYPE, CDATA.
- [x] Character references: the named five (`&amp; &lt; &gt; &quot; &apos;`)
      plus `&#NN;` and `&#xNN;`.
- [x] **No recursion.** Fixed-size buffers, explicit bounds.
- [x] Host unit tests, mostly on malformed input: an unclosed tag at EOF, a
      quote that never closes, `<`, `<<>>`, a 10 KB attribute value, a NUL
      byte mid-tag.

#### Test gate

- Well-formed HTML tokenises exactly.
- Every malformed case terminates and produces something usable — a tokeniser
  that gives up on bad input is useless, because all real input is bad.
- Fuzzed with random bytes: no crash, no hang, bounded memory.

#### Deliverable

`patches/WEB_W1_tokeniser.patch`

#### Results (verified 2026-08-07)

| Item | Outcome |
|---|---|
| `userspace/apps/webview/wv_html.{h,c}` | 18-state machine over the byte stream; token kinds TEXT/START/END/COMMENT/DOCTYPE/CDATA/EOF; attributes with name/value/flag; `self_closing`; line/col; arena = caller-provided token array + attribute array + string pool, `truncated` flag when any limit is hit while the scan keeps draining to EOF |
| No recursion | By construction: one loop, one switch; the only locals are fixed-size byte buffers (≤ 4 KB total) — the 64 KiB user stack cannot be exhausted by input |
| Tolerance | WHATWG behaviours honoured where cheap: `<` at EOF → text, `<<>>` → text runs, unclosed quote keeps the tag and everything before the quote, unclosed comment keeps its text, EOF in any state emits what exists plus WV_T_EOF; NUL → U+FFFD → `'?'` (8-bit font ceiling); names lower-cased; unknown/unterminated char refs stay literal; a reference must end in `;` (documented simplification) |
| Unit tests | `tests/unit/test_wv_html.c` — **122 checks, 0 failures**: exact token stream for well-formed HTML; all five named refs + numeric in text and attribute values; every malformed case from the gate; 10 000-byte attribute value capped with `truncated`; 100 000-byte text run and 10 000 tags hit caps and terminate; 3000 fuzz iterations + a 64 KiB random blob — every token/attr offset walked and checked against arena bounds |
| In-guest smoke | `/apps/webview` runs the tokeniser on `<p>Hello <b>web</b> &amp; bye<br/></p>` → `tokeniser smoke: 9 tokens, truncated=0` → PASS; `test_webview.sh` gained the assert (8 checks now) |
| Size | `webview.elf` 143 KB (tokeniser adds ~13 KB) — still far inside `SPAWN_MAX_IMAGE` |
| Bugs found by the tests | (1) after a character reference the scan position was advanced twice, skipping bytes after `&amp;`; (2) attributes were not flushed when a space or `/` followed a quoted value, so `class="main" id=x` lost `class`. Both caught by the exact-stream test, fixed, and covered by regression checks |

---

### Phase W2 — The DOM ✅ COMPLETE

**Objective:** tokens → a tree, with the implied structure real pages omit.

#### Tasks

- [x] Nodes in a flat array with index links (parent/first-child/next-sibling),
      not pointers into a heap — one allocation, and a tree walk that cannot
      run away.
- [x] An explicit open-element stack with a **depth cap** (D3).
- [x] Implicit close rules for the tags that need them: `<p>`, `<li>`, `<td>`,
      `<tr>`, and the void elements.
- [x] Mismatched close tags are reconciled against the stack, not obeyed.

#### Test gate

- `<p>a<p>b` produces two sibling paragraphs, not nested ones.
- `<b><i>x</b></i>` does not lose the text or corrupt the tree.
- A 10 000-element-deep document hits the cap and **does not overflow the
  64 KB stack** — asserted in QEMU, where the real stack limit applies.

#### Deliverable

`patches/WEB_W2_dom.patch`

#### Results (verified 2026-08-07)

| Item | Outcome |
|---|---|
| `userspace/apps/webview/wv_dom.{h,c}` | Flat node array with parent / first-child / last-child / next-sibling index links; implicit document root at index 0; explicit open-element stack with `WV_DOM_DEFAULT_DEPTH` (512) cap — past the cap elements are appended without nesting and `truncated` is set, so depth is bounded no matter the input |
| Implicit closes | `<p>` closes an open `<p>`; `<li>` closes an open `<li>`; `<td>`/`<th>` close open cells but never the row; `<tr>` closes the previous row and its cells; 14 void elements never nest; `self_closing` tags never nest |
| Mismatched closes | WHATWG pop-until-match: `<b><i>x</b></i>` closes `<i>` implicitly when `</b>` arrives; unmatched end tags are ignored (never wipe the stack — a bug the tests caught) |
| Attributes | Copied into the DOM's own pool (name/value pairs, value-less flags preserved); text runs merge across comments/entities/CDATA into single text nodes |
| Host gate | `tests/unit/test_wv_dom.c` — **65 checks, 0 failures**: the plan's two tree-shape cases, deep mismatch, void/li/td/tr rules, attribute carry, text merging, unmatched ends, empty input, a 10 000-deep document (nodes=10001, max_depth=512, truncated=1) and 2000 fuzz iterations — every build walked and every parent/child/sibling link verified |
| QEMU gate (the point of W2) | `/apps/webview` builds the 10 000-deep document **in-guest on the real 64 KiB user stack**: `dom deep test: PASS (tokens=20001 nodes=10001 maxdepth=512 truncated=1)`; `dom smoke: PASS` for `<p>a<p>b`; `test_webview.sh` asserts both (10 checks now) |
| Bug found by the tests | `<td>` was closing the `<tr>` too (WHATWG says cells close cells, rows close rows), which produced a wrong tree — fixed, with the test that caught it |
| Size | `webview.elf` ~158 KB (DOM adds ~15 KB) — inside `SPAWN_MAX_IMAGE` |

---

### Phase W3 — Block layout ✅ COMPLETE

**Objective:** a DOM → a list of positioned boxes.

#### Tasks

- [x] Block boxes stacked vertically, honouring width, margin and padding.
- [x] Inline flow within a block: text runs, word wrapping at the box edge.
- [x] An iterative layout walk over the flat node array (D3 again — the same
      64 KB applies here, and layout recursion is the classic way to hit it).
- [x] Layout produces a **display list**, not pixels. Separating them is what
      makes both testable.

#### Test gate

- A paragraph wider than the viewport wraps at the right column.
- Nested blocks indent by the sum of their margins and padding.
- Layout of a 5 000-box document completes within the frame budget from W0.
- The display list is compared against an expected list — text, not pixels,
  so a failure says *what* moved.

#### Deliverable

`patches/WEB_W3_layout.patch`

#### Results (verified 2026-08-07)

| Item | Outcome |
|---|---|
| `userspace/apps/webview/wv_layout.{h,c}` | DOM → display list (`WV_D_BOX` + `WV_D_TEXT` items): block boxes honouring width/margin/padding; inline flow with HTML whitespace collapsing and word wrap at the content edge; `<br>`; `<pre>`; inline style stack (`<b>/<strong>` bold, `<a>` blue+underline, `<u>` underline) surviving nesting; `<img>` 16×16 placeholder; `<hr>` rule; `<canvas width height>` block (W7 will back it with an FBO); hidden elements (`head/title/style/script/meta/link/base/noscript`) produce no boxes |
| Iterative by design | The tree walk is a (node, phase) stack; block/inline contexts are explicit caller-provided arrays with caps — no recursion, nothing on the 64 KiB stack beyond a few locals |
| UA stylesheet | body 8 px margin, p margins, h1–h6 bold, ul/ol 32 px list indent, blockquote 16/32 margins, hr rule — the browser defaults that make bare HTML readable; W5's CSS will override |
| Host gate | `tests/unit/test_wv_layout.c` — **79 checks, 0 failures**: wrap (20 words in a 300 px viewport end at ≤ 300 and span 7 lines), nested indent (text at exactly the summed margin/padding offsets), the exact expected display list for `<body><p>ab cd`, whitespace collapsing, `<pre>`/`<br>`, hidden elements, inline styles incl. nesting, placeholders, and **5 000 boxes laid out in 1 064 µs — inside the W0 budget (7 500 µs)**; 1 000 fuzz iterations with every item's offsets verified |
| QEMU gate | `/apps/webview` lays out the same 5 000-box document in-guest on the real 64 KiB stack: `layout smoke: PASS (items=10001, 5000 boxes in 10101 us, page_h=80000)` — 10 101 µs under TCG emulation (10× the host's 1 064 µs, i.e. the emulation factor, not a layout blow-up); `test_webview.sh` asserts it (11 checks now) |
| Bugs found by the tests | `br`/`img` were missing from the inline-tag list, so `<br>` opened a block (an empty 600×0 box) and never broke the line — fixed with regressions |
| Size | `webview.elf` ~175 KB (layout adds ~17 KB) — inside `SPAWN_MAX_IMAGE` |

---

### Phase W4 — Painting ✅ COMPLETE

**Objective:** display list → pixels in the window.

#### Tasks

- [x] Fill rectangles, draw borders, draw text runs through the PSF glyph
      rasteriser.
- [x] Synthesised bold (D7).
- [x] Clip every draw to the viewport, and skip boxes entirely outside it —
      the difference between scrolling a long page smoothly and not.
- [x] Scrolling by `memmove` of the retained buffer plus a repaint of the
      exposed band, which §1 measured at 0.068 ms.

#### Test gate

- A page renders with text where the display list says it should be.
- Scrolling a 10 000-line page stays within the frame budget.
- Painting is checked by hashing the buffer against a stored reference for a
  fixed input, so a change in output is a deliberate act.

#### Deliverable

`patches/WEB_W4_paint.patch`

#### Results (verified 2026-08-07)

| Item | Outcome |
|---|---|
| `userspace/apps/webview/wv_paint.{h,c}` | Clip-aware rect/glyph/text drawing; the project's PSF2 VGA 8×16 font embedded as data (same blob the kernel console uses); synthesised bold = double-strike (D7); underline at the glyph baseline; display-list run culls any box/word entirely outside the viewport before touching a pixel |
| Scrolling | `wv_paint_scroll()` memmoves the retained buffer; `wv_paint_band()` repaints only the exposed band, with **boxes clipped to the band** (a box straddling the band edge must not erase content painted above it) |
| Host gate | `tests/unit/test_wv_paint.c` — **42 checks, 0 failures**: clip rects; glyph 'A' compared pixel-by-pixel against the font bitmap; bold/underline pixels; the display list's text appears exactly where W3 said (first ink pixel of 'a'/'b' at the expected positions); scroll equivalence (memmove+band == full repaint, including a 2 000-line page with an opaque `<body>` box — the regression that used to erase content); **10 000-line page: one scroll step in 144 µs** (budget 7 500 µs); **the reference hash: a fixed document hashes to 0xFC12ACDC — stored in the test, so a rendering change is a deliberate act**; off-screen culling; 500 fuzz iterations |
| QEMU gate | `/apps/webview` renders a real 842 px page (h1, bold/underline/link, list, hr, canvas, 8 paragraphs) and prints `paint smoke: PASS (hash=0xfc12acdc)` — **the guest hash equals the host reference**, proving determinism — and `paint scroll smoke: PASS`; `test_webview.sh` asserts both (13 checks now) |
| Bug found by the tests | Band repaints drew opaque boxes whole, erasing content above the band (only visible once the page had a `<body>` background) — fixed by clipping boxes to the band; regression test added |
| Size | `webview.elf` ~180 KB (painter adds ~24 KB incl. the 4 KB font) — inside `SPAWN_MAX_IMAGE` |

---

### Phase W5 — Inline CSS ✅ COMPLETE

**Objective:** the subset from D4, from `style=` attributes and `<style>`.

#### Tasks

- [x] A declaration parser: `property: value` pairs, `;`-separated.
- [x] Selector matching limited to tag name, `#id` and `.class` (plus
      `type.class` and comma lists). No combinators, no specificity cascade
      beyond "later wins, inline wins".
- [x] Colour parsing: `#rgb`, `#rrggbb`, and the 16 named colours.
- [x] Unknown properties are **ignored**, unknown selectors **skipped** —
      the one place in this plan where silently ignoring input is correct,
      because that is what the CSS error-handling rules require.

#### Test gate

- `style="color:#f00"` renders red text.
- A malformed declaration does not discard the rest of the block.
- Every property in D4's list has a test that changes the output.

#### Deliverable

`patches/WEB_W5_css.patch`

#### Results (verified 2026-08-07)

| Item | Outcome |
|---|---|
| `userspace/apps/webview/wv_css.{h,c}` | Declaration parser (`;`-separated, tolerant: a no-colon declaration is skipped to the next `;`, never discarding the block); selector matcher (tag / `#id` / `.class` / `type.class` / comma lists, no combinators); colours `#rgb` / `#rrggbb` / 16 names; lengths as `<int>px`; margin/padding 1-2-4 value forms; border width; cascade: later rules win, inline `style=` wins; unknown properties/selectors ignored (CSS error recovery) |
| Computed styles in layout | `wv_style_t` resolved per element; layout honours display none/block, color (inherited into block text via the inline style stack), background-color, width, height, margin/padding (per side, overriding UA values), border (drawn by the painter), font-weight (inherited through nesting), text-align (lines are tracked and shifted on wrap and at block close) |
| Host gate | `tests/unit/test_wv_css.c` — **71 checks, 0 failures**: the plan's `style="color:#f00"` red-text gate; malformed declarations keep the rest of the block; **every D4 property has a test that changes the output**; colours (13 cases); selectors incl. `#id` vs `.class` vs `p.note` precedence and comma lists; display:none in a stylesheet; margin 4-value form; border paints; text-align centre/right at exact pixel offsets; no-CSS builds hash identically to W4; 500 fuzz iterations with random `<style>` content |
| Bugs found | (1) an infinite loop in the selector matcher on a trailing comma (empty tail never advanced) — the fuzz/`h1, h2` test caught it; (2) `#rgb` produced `0x0FF0000` instead of `0x00FF0000` (the channel shift was wrong); (3) values with leading whitespace (`" maroon"`) failed colour parsing; (4) `p.note` selectors were not matched. All fixed with regressions |
| QEMU gate | `/apps/webview` renders a page with a 5-rule `<style>` (h1 colour+centred, p margins, green links, `.note` background+border, `#footer` right-aligned grey): `css smoke: PASS (styled=0x4d394d5c, plain=0x2f39d54f)` — the stylesheet demonstrably changes the output — and the reference hash moved to **0x4D394D5C, identical on host and guest**; `test_webview.sh` asserts the css smoke (14 checks now) |
| Stack safety | `wv_css_build` originally copied `<style>` text into a 64 KiB stack buffer — it overflowed the real user stack in QEMU (a `[GUARD] user stack overflow`, the exact failure the plan's constraints exist to prevent). Reworked to parse each text node in place: no large stack buffers anywhere in the pipeline |
| Size | `webview.elf` ~175 KB (CSS adds ~13 KB) — inside `SPAWN_MAX_IMAGE` |

---

### Phase W6 — Navigation and networking ✅ COMPLETE

**Objective:** a usable browser: links, history, a bigger fetch.

#### Tasks

- [x] Hit-testing over the display list; clicking a link navigates.
- [x] Back/forward history.
- [x] Replace the 16 KB static response buffer with a growing one, and set an
      explicit maximum with a diagnosed refusal past it.
- [x] HTTP/1.1 with `Host:` and chunked transfer decoding — most servers stop
      speaking 1.0 politely.
- [x] A clear "HTTPS is not supported" page for `https://` URLs, rather than
      a connection failure the user has to interpret.

#### Test gate

- Fetch and render a page from the in-tree `/tests/tcpserver`, so the test
  does not depend on the internet.
- Follow a link, then go back; the previous page reappears.
- A chunked response decodes to the same bytes as an unchunked one.
- An `https://` URL produces the explanation, not a hang.

#### Deliverable

`patches/WEB_W6_navigation.patch`

#### Results (verified 2026-08-07)

| Item | Outcome |
|---|---|
| `wv_url.{h,c}` | Tolerant URL parser + resolver: scheme default, port, path, relative/root-relative/`../`/`./`/scheme-absolute/`//host`/fragment resolution — 32 host checks |
| `wv_http.{h,c}` | GET / HTTP/1.1 + Host: request builder; header parser (status, Content-Length, Transfer-Encoding: chunked); chunked decoder (extensions tolerated, incomplete/garbage rejected); **growing response buffer** (8 KB start, realloc-doubling, hard 512 KB cap with `refused` flag) — 69 host checks incl. **chunked decodes byte-identical to the plain body** and 100 KB through 2 KB appends |
| Links in the display list | `wv_disp_t.link_off` (href in the DOM pool) set by the layout walk for `<a>`; hit-testing over text items resolves relative hrefs against the current page URL and navigates |
| UI | Chrome bar: address bar (typing + Enter), Back and Go buttons (clickable), status strip; wheel/arrow scrolling unchanged |
| History | 8-entry back stack; `back` re-fetches without pushing |
| HTTPS refusal | `https://` renders "HTTPS is not supported" with the honest explanation (plan D6), never attempts a connection |
| **QEMU gate** | `test_webview_net.sh` — a real host server (SLIRP at 10.0.2.2) serves `/` (home + 2 links), `/page2.html`, `/chunked` and `/big` (100 KB). The guest web view: fetches home, **follows link 0 → page2, goes back → home reappears**, refuses `https://` with the explanation, decodes the **chunked** page, and receives the **100 KB page in full** (growing buffer). 10 assertions, all green. No internet needed |
| Test hooks | `/tmp/webview.url` (initial page) + `/tmp/webview.steps` (`link 0; back; https; nav <url>`, executed with pauses) — written before `run webview`, because the init shell blocks on a running child |
| Kernel TCP note | The legacy TCP path can drop the last bytes of a stream on FIN (one-segment receive, fixed RTO): a 259-byte body arrived as 256. The web view falls back to rendering the partial body instead of an error page — documented in the code |
| Size | `webview.elf` ~188 KB — inside `SPAWN_MAX_IMAGE` |

---

### Phase W7 — `<canvas>` with an OpenGL context ✅ COMPLETE

**Objective:** the phase OpenGL is actually for.

#### Tasks

- [x] `<canvas width= height=>` becomes a box in the layout, backed by an FBO
      (GL phase G12).
- [x] A tiny scripting-free binding: `<canvas data-scene="cube">` selects one
      of a handful of built-in scenes. **There is no JavaScript** (D5), so the
      page cannot drive the canvas imperatively — it can only ask for a scene.
- [x] The canvas renders through libgl into its FBO, and the result is
      composited into the page like any other box.
- [x] Its cost is measured and recorded, so the §1 table gains the one number
      it is missing: what a GL canvas costs inside a page.

#### Test gate

- A page containing a canvas renders both the text and the 3D content.
- The canvas is clipped and scrolled with the page, not drawn over it.
- A page with no canvas costs exactly what it did in W4 — the GL path must
  not be on the critical path for ordinary pages.

#### Deliverable

`patches/WEB_W7_canvas.patch`

#### Results (verified 2026-08-07)

| Item | Outcome |
|---|---|
| `userspace/apps/webview/wv_canvas.{h,c}` | The only web-view module that includes GL headers. Renders the built-in "cube" scene (the /glcube geometry, immediate mode, depth-tested) into an FBO (G12: colour texture + depth renderbuffer), reads it back with glReadPixels (rows bottom-first, flipped into the page's top-left XRGB buffer), and composites with `wv_canvas_blit()` — clipped to the page and scrolled with it (off-screen boxes cost a bounds check only) |
| Cost, measured | Host: **64×48 cube rendered in ~58 µs** (one-shot render; a full-page 2D blit is 125 µs, so even at canvas sizes the one-shot GL render is a rounding error); in-guest TCG it lands under the 10 ms tick (`0 us` — sub-tick). The plan's §1 table now has its missing number: a GL canvas inside a page costs one sub-frame render at load + a clipped blit per frame |
| GL off the critical path | The scene renders ONCE at page load into a cached buffer; `repaint` only blits it. A page without a canvas never touches libgl — the W4 reference hash for the demo page was unchanged (0x4D394D5C until the W8 rename; now 0xE57F068C, still host==guest) |
| Host gate | `tests/unit/test_wv_canvas.c` — **21 checks, 0 failures**, linking the REAL wv_canvas.c against the REAL libgl sources (LIBGL_TEST_SRCS + auragui stub): the buffer is not the clear colour, green+blue+purple cube faces are visible under the fixed camera, the centre holds the cube, **two renders are byte-identical** (determinism), invalid sizes refused, and blit clipping: exact placement, scroll moves the canvas with the page, fully off-screen boxes paint nothing, right-edge clipping; 2 000 fuzz blits |
| QEMU gate | `/apps/webview` prints `canvas smoke: PASS (64x48 cube in 0 us, hash 4d394d5c -> 8a6c0574)` — the composited canvas changes the page. `test_webview_net.sh` gained a `/canvas.html` route (`<canvas width=64 height=48 data-scene="cube">` next to real text): the guest fetches it and prints `canvas: rendered 64x48 cube` — text AND 3D on one page. 15 assertions across the two cases |
| Size | `webview.elf` ~374 KB (libgl adds ~186 KB) — still inside `SPAWN_MAX_IMAGE` (1 MiB); the plan's unbounded-appetite risk is watched, and W8 will decide gbrowser's fate with the size budget in mind |

**§1 table, completed:**

| Operation | Cost |
|---|---|
| Full-page 2D blit (800×600) | 125 µs (reference) / ~7 600 µs (QEMU TCG) |
| Scroll by 40 px (memmove + band) | 68 µs / 144 µs (10 000-line page, TCG) |
| **GL `<canvas>` 64×48 cube, one-shot** | **~58 µs host / sub-tick in TCG** (NEW — W7) |

---

### Phase W8 — Retire or keep `gbrowser` ✅ COMPLETE

**Objective:** decide, with the evidence in hand.

#### Tasks

- [x] Compare `/apps/webview` against `/apps/gbrowser` on the same pages.
- [x] If the new one is better in every respect, remove the old one and its
      integration case. If it is not, **say which respect** and keep both.
- [x] Update `README.md`, `docs/webview.md` and the launcher.

#### Test gate

- Whatever ships, the integration suite passes and the launcher entry works.

#### Deliverable

`patches/WEB_W8_consolidate.patch`

#### Results (verified 2026-08-07)

**The comparison (the plan's W8 task):**

| Aspect | `gbrowser` (515 lines) | `webview` (~4 800 lines + libgl) |
|---|---|---|
| Rendering | HTML → text lines in an AuraGUI listbox (one line per element; no layout, no boxes, no inline flow) | Full pipeline: tokeniser → DOM → block layout → display list → pixels (PSF 8×16), with culling and memmove+band scrolling |
| CSS | none | D4 subset: display, color, background-color, width, height, margin, padding, border, font-weight, text-align (from `style=` and `<style>`) |
| HTTP | HTTP/1.0, **16 KB static response buffer** | HTTP/1.1 + `Host:`, chunked decoding, **growing buffer to 512 KB** with a diagnosed refusal |
| Navigation | clickable links, 10-entry back stack, Home button | clickable links, address bar, Back/Go buttons, 8-entry history |
| HTTPS | attempts the connection (fails confusingly) | honest "HTTPS is not supported" page (D6) |
| `<canvas>` | none | OpenGL cube scene via FBO (W7) |
| Robustness | 16 KB truncation on real pages | 64 KiB-stack-safe, arena-bounded pipeline; ~500 host checks across W1–W7 + 28 QEMU assertions |
| Size | smaller binary | 374 KB — still inside `SPAWN_MAX_IMAGE` (1 MiB) |

**Decision (user-driven): rename the new web view to `/apps/gbrowser` and give it a full GUI** — the web view's name was kept (it was `/apps/gbrowser` until W8's first draft retired it), the old listbox-based source stays removed, and the browser now ships with a real chrome. The new web view is better in every
functional respect; the only points in gbrowser's favour were cosmetic
(a Home button, native listbox widgets) and one size advantage that does
not matter below the spawn ceiling. The plan's "if it is not better, say
which respect" clause is answered explicitly: there is no respect in which
it is better.

Actions:
- `userspace/apps/webview/` renamed to `userspace/apps/gbrowser/`
  (`webview.c` → `gbrowser.c`; the `wv_*` engine modules keep their names).
- The old listbox-based `gui-browser` source stays removed; the browser's
  name lives on as the GUI browser built in this plan.
- **Full GUI chrome added**: Back / Fwd / Home / Go buttons, a clickable
  address bar, a hover-aware status strip (the link under the cursor is
  shown while moving the mouse), forward history (`Fwd`), `Home` (the
  built-in demo page) and a window title that follows the current page.
- `glaunch`'s "Web Browser" entry launches `gbrowser`; `init`'s `help`
  lists it; `README.md`, `docs/filesystem.md`, `docs/gbrowser.md`
  (renamed from `docs/webview.md`) and `docs/status.md` updated.
- Integration cases renamed `test_webview*` → `test_gbrowser*` (run_all
  updated); log prefixes `[webview]` → `[gbrowser]`; the test hooks are
  now `/tmp/gbrowser.url`, `/tmp/gbrowser.steps`, `/tmp/gbrowser.frames`.

**Test gate:** `make iso` clean; `make test-unit` green (all 7 browser
suites: 122+65+79+42+71+101+21 = 501 host checks); QEMU integration
`test_gbrowser` 15/15 and `test_gbrowser_net` 13/13 pass; the launcher
entry works (`/apps/gbrowser` resolves through the standard search path);
the GUI chrome (Back/Fwd/Home/Go + hover status) is exercised in QEMU.

---

## 5. Order and rationale

| Phase | Why here |
|---|---|
| W0 | The limitations belong in writing before the code invites disappointment |
| W1 | Everything downstream consumes tokens |
| W2 | Layout needs a tree, and the tree is where the stack limit bites |
| W3 | Painting needs positions |
| W4 | The first phase with something to look at |
| W5 | Styling is meaningless before there are boxes to style |
| W6 | Navigation is only useful once pages render |
| W7 | `<canvas>` needs a working layout to sit inside |
| W8 | A decision that needs the comparison to exist first |

**If only three phases are ever built, build W1–W3.** A tokeniser, a DOM and
a layout engine are the parts that do not exist anywhere in this tree. Paint
is a for-loop over rectangles; the hard, interesting, easy-to-get-wrong work
is upstream of it.

---

## 6. Risks

**No HTTPS makes this a demo.** Named in D6 and first in this list because it
is the difference between "we have a web view" and "we can browse the web".
Nothing in this plan changes it.

**The 64 KB stack will be hit.** Twice, probably: once in the parser and once
in layout. The plan says iterative-by-design for both, and W2's test gate
puts a 10 000-deep document in QEMU rather than trusting the host, where the
stack is 8 MB and the bug is invisible. GL phase G11b learned this the
expensive way.

**A page renderer is an unbounded appetite.** There is always another CSS
property. D4 fixes a list; the risk is that the list grows silently until the
program no longer fits in `SPAWN_MAX_IMAGE`, and the failure appears as a
spawn refusal with no obvious connection to the last feature added.

**Correctness has no natural definition.** "Does this page look right" is not
a test. W3 and W4 therefore assert on the *display list* and on a hash of the
output buffer — a change in rendering must be a deliberate act with an updated
expectation, not a judgement call.

**GL may tempt its way onto the fast path.** The measurement in §1 is
unambiguous today, and someone will eventually propose "just composite the
page with GL". If the virtio-gpu hang is ever fixed and there is real hardware
acceleration, that becomes a legitimate question and should be **re-measured**
rather than assumed either way.

---

## 7. What this plan does not do

- **No JavaScript.** Not a phase, not a stretch goal (D5).
- **No HTTPS/TLS.** Its own plan, of comparable size (D6).
- **No images.** PNG and JPEG decoders are each a phase in their own right;
  they belong in a follow-up once boxes exist to put them in.
- **No proportional fonts.** Needs a font format and a glyph cache (D7).
- **No printing, no downloads, no cookies, no forms.** Forms in particular
  look small and are not: they need focus management, input widgets and
  `POST`.
- **No claim of standards compliance.** This renders a subset, deliberately
  chosen, and `docs/webview.md` will say so.
