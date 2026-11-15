# `graphics.c` — 2D Graphics Library

## 1. Overview

`graphics.c` implements a software 2D graphics layer for the Limine framebuffer. Drawing operations target an off-screen back buffer. The contents of that buffer are copied to the visible framebuffer by `gfx_flip()` or partially copied by `gfx_flip_rect()`.

The implementation provides:

- Framebuffer initialization and dimension queries.
- Pixel, rectangle, line, circle, and text drawing.
- Clip-rectangle management.
- RGB blending, per-pixel alpha blending, and translucent rectangle fills.
- Horizontal alpha-span operations used by glyph renderers.
- An optional x86-64 SSE2 path for selected fill, blit, and blend operations.
- Box blur, anti-aliased rounded rectangles, and gradient fills.
- Direct access to rectangular regions of the back buffer.
- Full-framebuffer and rectangular presentation.

The implementation expects a 32-bit framebuffer. `gfx_init()` does not initialize the library when the boot framebuffer is missing or uses a different bits-per-pixel value.

## 2. Pixel, color, and buffer conventions

### 2.1 Coordinates

Coordinates use the framebuffer coordinate system:

- `(0, 0)` is the top-left corner.
- `x` increases to the right.
- `y` increases downward.
- Width and height describe pixel extents, not inclusive end coordinates.

Most drawing functions operate on the back buffer. Their changes do not become visible until a presentation function copies the relevant pixels to the front buffer.

### 2.2 Color formats

Most drawing APIs accept a 24-bit RGB value in the form `0xRRGGBB`. The internal `make_color()` helper extracts the red, green, and blue channels and packs them using the channel shifts reported by the boot framebuffer.

This conversion is important because the framebuffer's channel layout is not assumed to be a fixed byte order.

There are two notable exceptions to the usual color convention:

- `gfx_blit_rgb888()` copies source `uint32_t` pixel values directly. It does not call `make_color()` or convert the source channel layout.
- `gfx_blit_alpha()` accepts source pixels in `0xAARRGGBB` form, extracts their alpha and RGB channels, and converts the RGB value with `make_color()` before writing it.

### 2.3 Front buffer and back buffer

| Buffer | Purpose |
|---|---|
| `front_fb` | Pointer to the mapped boot framebuffer. This is the visible framebuffer memory. |
| `back_fb` | Kernel-allocated off-screen buffer. Drawing functions normally write here. |

`fb_pitch` is the framebuffer row size in bytes. Functions that address the buffers as `uint32_t` pixels use `fb_pitch / 4` as the row stride. This is distinct from `fb_width`: pitch may include row padding.

The back buffer is allocated by `kmalloc()` in `gfx_init()`. It is zero-initialized when allocation succeeds.

### 2.4 Clipping

The library maintains one global clip rectangle:

```text
[clip_x0, clip_x1) × [clip_y0, clip_y1)
```

The right and bottom edges are exclusive. The initial clip rectangle covers the full framebuffer. Drawing functions that use the normal drawing pipeline are restricted by this rectangle.

The clip rectangle is global state, not a per-call parameter. Callers that temporarily change it should restore the previous rectangle when finished.

Some low-level operations deliberately bypass clipping. In particular, `gfx_back_get_rect()` and `gfx_back_put_rect()` work against framebuffer bounds only, while `gfx_flip_rect()` controls which back-buffer region is presented rather than which region may be drawn.

### 2.5 Pixel-write counter

`px_written` is a global 64-bit counter incremented by many drawing functions. `gfx_take_px_counter()` returns the current value and resets it to zero.

The counter is intended as a lightweight rendering statistic. It is not a uniform count of every memory write: functions differ in whether they count transparent pixels, skipped mask pixels, or direct back-buffer copies. The function reference below describes important differences.

## 3. Initialization and global state

### `gfx_init()`

```c
void gfx_init(void);
```

Initializes the graphics library from the framebuffer information provided by the boot layer.

Implementation sequence:

1. Calls `boot_get_framebuffer()`.
2. Returns without initializing if no framebuffer is available or `bpp` is not `32`.
3. Validates nonzero dimensions, signed-coordinate limits, pitch alignment and size, and the RGB channel shifts expected by the 32-bit pixel operations.
4. Checks that the framebuffer byte size fits the address and allocation types used by the target architecture.
5. Maps the physical framebuffer address:
   - On `__i386__`, uses `fb32_map_fb()`.
   - On other configured targets, uses the HHDM offset returned by `boot_get_hhdm_offset()`.
6. Stores framebuffer width, height, pitch, and red/green/blue channel shifts.
7. Resets the clip rectangle to the full framebuffer through `gfx_clip_clear()`.
8. Allocates `fb_pitch * fb_height` bytes for the back buffer.
9. Clears the allocated back buffer to zero when allocation succeeds.

The function returns `void`; it does not report an error code. Invalid framebuffer metadata, unsupported pixel layout, an address/size overflow, failed i386 mapping, or failed back-buffer allocation leaves the drawing system unavailable. Drawing and presentation routines generally check for a null buffer and return without doing work.

`gfx_init()` should be called before using the drawing API. The implementation does not expose a shutdown or reinitialization routine; repeated calls after successful initialization are ignored to avoid leaking the existing back buffer.

## 4. Clip management and rendering statistics

### `gfx_clip_set()`

```c
void gfx_clip_set(int32_t x, int32_t y, uint32_t w, uint32_t h);
```

Sets the global clip rectangle to the requested rectangle, adjusting negative starting coordinates and limiting the right and bottom edges to the framebuffer dimensions.

Parameters:

| Parameter | Description |
|---|---|
| `x`, `y` | Requested top-left coordinate. |
| `w`, `h` | Requested width and height. |

When `x` or `y` is negative, the corresponding origin is moved to zero and the amount outside the framebuffer is subtracted from the requested extent. The right edge is limited to `fb_width`; the bottom edge is limited to `fb_height`.

The stored rectangle uses exclusive right and bottom edges. Calculations use 64-bit intermediates, and the result is clamped to framebuffer bounds. The function does not return the resulting rectangle. Use `gfx_get_clip()` to query it.

### `gfx_clip_clear()`

```c
void gfx_clip_clear(void);
```

Resets the global clip rectangle to the entire framebuffer:

```text
x = 0
 y = 0
width = fb_width
height = fb_height
```

This is also called by `gfx_init()` after framebuffer dimensions are known.

### `gfx_get_clip()`

```c
void gfx_get_clip(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h);
```

Returns the current clip rectangle through output pointers. Every pointer is optional and may be `NULL`.

| Output | Value |
|---|---|
| `x` | Left edge. |
| `y` | Top edge. |
| `w` | `clip_x1 - clip_x0`, or zero if the stored horizontal range is empty or inverted. |
| `h` | `clip_y1 - clip_y0`, or zero if the stored vertical range is empty or inverted. |

This function does not modify the clip state. It is suitable for code that needs to perform its own clipping or culling outside this C file.

### `gfx_take_px_counter()`

```c
uint64_t gfx_take_px_counter(void);
```

Returns the current `px_written` value and resets the counter to zero.

The read and reset are separate ordinary memory operations. The function does not provide atomic synchronization for concurrent rendering threads.

The counter is updated by several drawing paths, including pixel writes, filled rectangles, blits, alpha operations, blur, and gradients. It is not updated by every direct write to `back_fb`; see [Pixel counter behavior](#pixel-counter-behavior).

## 5. Basic pixel and rectangle operations

### `gfx_putpixel()`

```c
void gfx_putpixel(uint32_t x, uint32_t y, color_t color);
```

Writes one pixel to the back buffer.

The function returns without writing when:

- The back buffer is unavailable.
- `x` or `y` is outside the framebuffer.
- The coordinate is outside the current clip rectangle.

The supplied RGB color is converted to the framebuffer's native channel layout with `make_color()`. A successful write increments `px_written` by one.

### `gfx_fill_rect()`

```c
void gfx_fill_rect(uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h,
                   color_t color);
```

Fills a rectangle with one opaque color.

The requested rectangle is intersected with the current clip rectangle. If the resulting region is empty, the function returns without writing. Each visible row is filled through `gfx_memset32()`, which may use the SSE2 implementation on x86-64.

The RGB input is converted to native framebuffer format once before the rows are filled. The counter is increased by the area actually filled:

```text
visible_width × visible_height
```

A zero width or height naturally produces an empty region and no writes.

### `gfx_draw_rect()`

```c
void gfx_draw_rect(uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h,
                   color_t color);
```

Draws the outline of a rectangle. It plots the top and bottom edges, then the left and right edges, using `gfx_putpixel()`.

The function returns immediately when either dimension is zero. Clipping and framebuffer bounds are handled by `gfx_putpixel()` for each edge pixel.

The implementation draws the edges independently. At the corners, the same pixel may be submitted more than once, so the counter can include duplicate successful edge writes.

### `gfx_clear()`

```c
void gfx_clear(color_t color);
```

Fills the full framebuffer dimensions with the supplied color by calling `gfx_fill_rect(0, 0, fb_width, fb_height, color)`.

Because it delegates to `gfx_fill_rect()`, `gfx_clear()` respects the current clip rectangle. It clears only the currently clipped region when a restricted clip rectangle is active; it does not implicitly reset clipping.

## 6. Image and pixel blitting

### `gfx_blit_rgb888()`

```c
uint64_t gfx_blit_rgb888(
    int32_t x,
    int32_t y,
    uint32_t w,
    uint32_t h,
    const uint32_t *src,
    uint32_t src_stride,
    const uint8_t *skip_mask,
    uint32_t mask_stride
);
```

Copies a rectangular region from a source array to the back buffer. The function returns the number of source pixels copied to the destination.

Parameters:

| Parameter | Description |
|---|---|
| `x`, `y` | Destination top-left coordinate. Signed values allow the source rectangle to begin outside the clip area. |
| `w`, `h` | Source rectangle width and height in pixels. |
| `src` | Pointer to 32-bit source pixels. |
| `src_stride` | Number of `uint32_t` elements between source rows. This is measured in pixels, not bytes. |
| `skip_mask` | Optional one-bit-per-pixel mask. A set bit skips the corresponding source pixel. `NULL` disables masking. |
| `mask_stride` | Number of bytes between mask rows. |

The destination rectangle is clipped to the current clip rectangle and framebuffer bounds. The source offsets are adjusted to preserve source-to-destination alignment after clipping. The function rejects a source stride smaller than the requested row width and rejects source or mask offset calculations that cannot fit in `size_t`.

Mask details:

- Bits are stored most-significant-bit first within each byte.
- A bit value of `1` means **skip** the source pixel.
- A bit value of `0` means **copy** the source pixel.
- The mask bit index includes the source horizontal offset, so clipped source regions continue to use the correct mask positions.

The source values are copied directly into the destination. Despite the function name, this implementation does not unpack 24-bit RGB data or convert channel order. The source is a `uint32_t` array, and its pixel representation must already be compatible with the destination framebuffer's native 32-bit layout.

On x86-64, the function uses the SIMD implementation in groups of eight masked pixels or four unmasked pixels, with scalar handling for any remaining pixels and unaligned mask boundaries. Other targets use the scalar loop.

The returned count and `px_written` are incremented only for pixels that are actually copied. Masked pixels are not counted.

### `gfx_blit_alpha()`

```c
void gfx_blit_alpha(int32_t dst_x, int32_t dst_y,
                    uint32_t w, uint32_t h,
                    const uint32_t *src, uint32_t stride);
```

Blits a rectangle from a source image whose pixels use `0xAARRGGBB` format.

Parameters:

| Parameter | Description |
|---|---|
| `dst_x`, `dst_y` | Destination top-left coordinate. |
| `w`, `h` | Source rectangle dimensions. |
| `src` | Pointer to the source image. |
| `stride` | Number of `uint32_t` pixels between source rows. |

The destination is clipped to the current clip rectangle and framebuffer bounds. Source offsets are adjusted when the top or left part of the destination rectangle is clipped. A source stride smaller than `w`, or a source offset calculation that cannot fit in `size_t`, is rejected without writing.

Per-pixel behavior:

- Alpha `0`: the source pixel is skipped and the destination remains unchanged.
- Alpha `255`: the source RGB value is converted with `make_color()` and copied directly.
- Alpha `1..254`: the converted source color is blended over the destination using `gfx_blend_native_u32()`.

The counter is incremented only for source pixels with nonzero alpha. Transparent pixels are not counted.

Unlike `gfx_blit_rgb888()`, this function performs alpha processing and framebuffer channel conversion. The source must use the documented ARGB channel arrangement.

## 7. Lines and circles

### `gfx_draw_line()`

```c
void gfx_draw_line(uint32_t x0, uint32_t y0,
                   uint32_t x1, uint32_t y1,
                   color_t color);
```

Draws a line between two endpoints.

The implementation first clips the line against the framebuffer bounds using the Cohen–Sutherland line-clipping algorithm. It then rasterizes the remaining segment with the integer Bresenham algorithm. Coordinate and intersection calculations use 64-bit intermediates; the multiplication/division helper is safe for the unsigned 32-bit endpoint range accepted by this API.

The line-clipping stage uses the full framebuffer rectangle, not the current clip rectangle. Individual pixels are ultimately written through the normal pixel path, so the active clip rectangle is still respected during rasterization.

### `gfx_draw_circle()`

```c
void gfx_draw_circle(uint32_t cx, uint32_t cy,
                     uint32_t r, color_t color);
```

Draws the outline of a circle centered at `(cx, cy)` with radius `r`.

The rasterizer uses an integer midpoint-circle style algorithm. For every iteration, it plots the eight symmetric points around the circle's center. The generated pixels are passed to `gfx_putpixel()`, so framebuffer bounds and the current clip rectangle are applied to each point.

### `gfx_fill_circle()`

```c
void gfx_fill_circle(uint32_t cx, uint32_t cy,
                    uint32_t r, color_t color);
```

Draws a filled circle by scanning only the intersection of its bounding square with the active clip rectangle and framebuffer. A point is plotted when its squared distance from the center is no greater than the squared radius. Distance calculations use unsigned 64-bit intermediates, avoiding signed 32-bit square overflow for the full `uint32_t` radius range.

Each accepted point is written through `gfx_putpixel()`. The clipped scan range prevents work on off-screen portions of the bounding square. The implementation does not use scanline spans or a dedicated SIMD path.

## 8. Text rendering

Text rendering uses the bitmap font returned by `psf_get_font()`. Characters are treated as single-byte glyph indices; this code does not decode UTF-8 sequences.

### `gfx_draw_string()`

```c
void gfx_draw_string(uint32_t x, uint32_t y,
                     const char *s, color_t color);
```

Draws a string using the active PSF font.

For each character:

1. A newline resets the horizontal position to the original `x` and advances `y` by the font height.
2. Other characters are interpreted as unsigned byte values.
3. If the byte value is greater than or equal to `num_glyphs`, glyph index zero is used.
4. The glyph bitmap is traversed row by row and bit by bit.
5. Set glyph bits are drawn with `gfx_putpixel()`; unset bits remain transparent.
6. The horizontal position advances by the font width.

The font's `width`, `height`, `bytes_per_row`, `bytes_per_glyph`, `num_glyphs`, and `data` fields determine the glyph layout.

If `psf_get_font()` returns `NULL`, the function does nothing. The string pointer itself must be valid and non-`NULL`.

### `gfx_draw_text()`

```c
void gfx_draw_text(uint32_t x, uint32_t y,
                   const char *s, color_t color);
```

Convenience wrapper around `gfx_draw_string()`. It has identical rendering behavior and does not implement a separate text pipeline.

### `gfx_text_width()`

```c
uint32_t gfx_text_width(const char *s);
```

Returns the width of the first line of a string in pixels.

The function adds one glyph width for each byte before the first newline. It does not measure later lines, account for proportional glyph widths, or decode multibyte text.

If a font is available, its `width` field is used. If no font is available, the fallback glyph width is `8`. A `NULL` string returns zero.

### `gfx_draw_text_centered()`

```c
void gfx_draw_text_centered(uint32_t y,
                            const char *s, color_t color);
```

Draws a string horizontally centered in the framebuffer. It obtains the text width through `gfx_text_width()` and computes:

```text
x = (fb_width - text_width) / 2
```

When the measured text width is greater than or equal to the framebuffer width, the starting coordinate is zero. Rendering itself is delegated to `gfx_draw_string()`.

Only the first line is measured for centering. Newline characters are handled by `gfx_draw_string()` after drawing begins.

## 9. Color blending and alpha primitives

### `gfx_blend()`

```c
color_t gfx_blend(color_t a, color_t b, uint8_t t);
```

Interpolates between two `0xRRGGBB` colors channel by channel.

The interpolation formula is:

```text
result = (a * (255 - t) + b * t) / 255
```

The same operation is applied independently to red, green, and blue.

| `t` | Result |
|---:|---|
| `0` | `a` |
| `255` | `b` |
| `1..254` | A proportional interpolation from `a` toward `b` |

The function returns an RGB value. It does not access the framebuffer and does not apply the framebuffer channel shifts.

### `gfx_blend_pixel()`

```c
void gfx_blend_pixel(int32_t x, int32_t y,
                     uint32_t color, uint8_t alpha);
```

Blends one RGB source color over the back-buffer pixel at `(x, y)`.

The function checks the back buffer, framebuffer bounds, and current clip rectangle. Alpha behavior:

- `0`: no operation and no counter increment.
- `255`: direct opaque write after conversion with `make_color()`.
- `1..254`: channel-wise blend over the existing native destination pixel.

A successful nonzero-alpha operation increments `px_written` by one.

### `gfx_blend_pixels4()`

```c
void gfx_blend_pixels4(int32_t x, int32_t y,
                       uint32_t color, const uint8_t alpha[4]);
```

Blends four adjacent horizontal pixels using four independent alpha values and one common RGB source color.

This function is intended for horizontal glyph coverage runs. When all four pixels are inside the framebuffer and clip rectangle, it processes the group through `gfx_blend4_native()`. On x86-64 this selects the SSE2 implementation; other targets use the scalar helper.

If the group is only partially visible, the visible subsection is processed one pixel at a time. In that path, zero-alpha pixels are skipped and full-alpha pixels are copied directly.

The `alpha` pointer must reference at least four bytes; a `NULL` pointer is ignored. The function checks the back buffer and vertical clip range, then limits the horizontal range to the current clip rectangle and framebuffer bounds. The full SIMD path is used only when all four pixels are visible.

Counter behavior: the fully visible path adds four regardless of the individual alpha values. The partial path adds the visible subsection length, including positions whose alpha is zero. The counter therefore represents the processed span width here, not strictly the number of pixels whose color changed.

### `gfx_blend_span()`

```c
void gfx_blend_span(int32_t x, int32_t y,
                    uint32_t count, uint32_t color,
                    const uint8_t *alphas);
```

Blends a horizontal run of `count` pixels using a separate alpha value for each pixel and one common source color.

Parameters:

| Parameter | Description |
|---|---|
| `x`, `y` | Starting destination coordinate. |
| `count` | Number of source alpha entries and destination pixels. |
| `color` | Common RGB source color. |
| `alphas` | Array containing one alpha byte per source pixel. |

The run is clipped against the current clip rectangle and framebuffer horizontal bounds. Rows outside the vertical clip range are ignored.

The visible run is processed in groups of four through `gfx_blend4_native()`, followed by a scalar tail for any remaining pixels. The four-pixel helper handles all alpha values, including zero and full opacity. The scalar tail skips alpha zero and uses a direct write for alpha 255.

The `alphas` pointer must contain at least `count` entries. The counter increases by the visible run length, including pixels whose alpha is zero. It is therefore a count of processed positions rather than a count of modified pixels.

### `gfx_fill_rect_alpha()`

```c
void gfx_fill_rect_alpha(int32_t x, int32_t y,
                         uint32_t w, uint32_t h,
                         uint32_t color, uint8_t alpha);
```

Fills a rectangle by blending one RGB color over the existing back-buffer contents with a constant alpha value.

The destination rectangle is intersected with the current clip rectangle and framebuffer bounds.

Alpha behavior:

- `0`: immediate no-op.
- `255`: opaque fill through `gfx_memset32()`.
- `1..254`: blend the source color into every destination pixel.

For translucent fills, pixels are processed in groups of four through `gfx_fill4_native()`, with a scalar tail when the row width is not divisible by four. The x86-64 implementation uses SSE2 for the four-pixel groups.

The counter increases by the complete visible rectangle area for both the opaque and translucent paths.

### Native blending formula

The internal native-pixel blend operation uses:

```text
dst = (dst * (255 - alpha) + src * alpha) / 255
```

It is applied independently to each of the four bytes in a 32-bit pixel. `gfx_div255()` performs the division using an integer identity that is exact for the range produced by 8-bit channel blending.

The ordinary RGB source conversion performed by `make_color()` does not set the high byte. The implementation nevertheless processes all four bytes in the native blend helpers for uniformity.

## 10. Blur and anti-aliased shapes

### `gfx_box_blur_rect()`

```c
void gfx_box_blur_rect(int32_t x, int32_t y,
                       uint32_t w, uint32_t h,
                       uint32_t radius);
```

Applies a box blur to a rectangular region of the back buffer.

The implementation uses two passes:

1. Horizontal pass: reads the selected back-buffer region and writes blurred pixels to a temporary buffer.
2. Vertical pass: reads the temporary buffer and writes the final result back to the back buffer.

Each pass uses a sliding-window sum for red, green, and blue. This avoids recalculating the complete neighborhood for every output pixel.

The sampling window size is:

```text
2 * radius + 1
```

At the boundaries of the selected region, samples outside the region are clamped to the nearest edge pixel. The blur therefore uses edge replication rather than reading outside the requested rectangle. The alpha/high byte is not blurred; only the RGB channels are reconstructed.

Before processing, the function intersects the requested region with the current clip rectangle and framebuffer dimensions. The radius is limited to at most half of the resulting width and height. The function returns without work when the back buffer is unavailable, the requested dimensions are smaller than two pixels, the radius is zero, the resulting region is too small, or temporary allocation fails.

A temporary buffer of `visible_width * visible_height * sizeof(uint32_t)` bytes is allocated with `kmalloc()` and released with `kfree()`.

The clipping helper rejects empty intersections before the visible dimensions are converted to unsigned values. The temporary allocation size is checked against `SIZE_MAX` before allocation, and sliding-window sums use 64-bit accumulators.

The counter increases by the visible region area after the blur is completed.

### `gfx_fill_round_rect_aa()`

```c
void gfx_fill_round_rect_aa(int32_t x, int32_t y,
                            uint32_t w, uint32_t h,
                            uint32_t r, uint32_t color);
```

Draws a filled anti-aliased rounded rectangle.

The radius is clamped so that it does not exceed half the rectangle's width or height. A radius of zero uses the internal signed-coordinate rectangle fill path.

For a nonzero radius, the rectangle is divided into interior strips and four corner regions:

- The top-middle strip is filled without anti-aliasing.
- The bottom-middle strip is filled without anti-aliasing.
- The central strip is filled without anti-aliasing.
- Each corner is rasterized using per-pixel coverage computed by 4×4 supersampling.

The coverage value ranges from `0` to `255` and is passed to `gfx_aa_pixel()`. Fully covered pixels are written opaquely; partially covered pixels are blended with `gfx_blend_pixel()`.

The normal clip rectangle and framebuffer bounds are applied before writing. The internal signed-coordinate fill path allows negative rectangle origins to be clipped without converting them to large unsigned coordinates. Corner bounding boxes are clipped before supersampling.

## 11. Gradients

### `gfx_gradient_rect_4()`

```c
void gfx_gradient_rect_4(int32_t x, int32_t y,
                         uint32_t w, uint32_t h,
                         uint32_t c_tl, uint32_t c_tr,
                         uint32_t c_bl, uint32_t c_br);
```

Fills a rectangle with a four-corner bilinear RGB gradient.

Corner parameters:

| Parameter | Position |
|---|---|
| `c_tl` | Top-left |
| `c_tr` | Top-right |
| `c_bl` | Bottom-left |
| `c_br` | Bottom-right |

Each input color uses `0xRRGGBB`. For each output pixel, the implementation:

1. Interpolates between the top-left and top-right colors using the horizontal coordinate.
2. Interpolates between the bottom-left and bottom-right colors using the same horizontal coordinate.
3. Interpolates between those two intermediate colors using the vertical coordinate.
4. Converts the resulting RGB value with `make_color()` and writes it to the back buffer.

Interpolation uses 8.8 fixed-point values. The horizontal and vertical interpolation factors range from `0` to `256`, inclusive. A one-pixel dimension uses a factor of zero for that axis.

The requested rectangle is clipped against the current clip rectangle and framebuffer bounds. Interpolation factors are calculated from the original rectangle dimensions and the source offsets introduced by clipping. Clipping therefore limits writes without rescaling the gradient across the remaining visible area. The counter increases by the area written.

### `gfx_gradient_v()`

```c
void gfx_gradient_v(uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h,
                    color_t top, color_t bottom);
```

Fills a rectangle with a vertical gradient between two RGB colors.

The function iterates over the requested rows. For each row it calculates an 8-bit interpolation factor from `0` at the top to `255` at the bottom, obtains the row color through `gfx_blend()`, and fills that row with `gfx_fill_rect()`.

If `h` is zero or the back buffer is unavailable, the function returns. A height of one uses the top color. Horizontal and vertical clipping are applied by `gfx_fill_rect()` for each row.

Unlike `gfx_gradient_rect_4()`, the interpolation factor is based on the original requested height, so clipping rows does not rescale the gradient over the remaining visible rows.

## 12. Direct back-buffer region access

### `gfx_back_get_rect()`

```c
void gfx_back_get_rect(int32_t x, int32_t y,
                       uint32_t w, uint32_t h,
                       uint32_t *dst);
```

Copies a rectangular region from the back buffer into a caller-provided linear array.

The destination array is tightly packed: each output row contains exactly `w` pixels. There is no destination stride parameter.

For each requested source coordinate:

- If it is inside the framebuffer, the raw 32-bit back-buffer value is copied.
- If it is outside the framebuffer, zero is written to the corresponding destination element.

The function checks the back buffer, destination pointer, and nonzero dimensions. It does not apply the current clip rectangle. It reads the back buffer directly and does not increment `px_written`.

### `gfx_back_put_rect()`

```c
void gfx_back_put_rect(int32_t x, int32_t y,
                       uint32_t w, uint32_t h,
                       const uint32_t *src);
```

Copies a tightly packed rectangular source array directly into the back buffer.

The source is interpreted as `w * h` consecutive `uint32_t` pixels, with a row stride of `w`. Pixels outside framebuffer bounds are skipped. The function does not apply the current clip rectangle and does not perform color conversion or alpha blending.

This is a raw native-pixel copy operation. The source values should already match the back buffer's pixel representation.

Unlike most drawing functions, `gfx_back_put_rect()` does not increment `px_written`.

## 13. Presentation and framebuffer control

### `gfx_flip()`

```c
void gfx_flip(void);
```

Presents the entire back buffer.

The function first checks the BSOD presentation lock and verifies that both front and back buffers exist. It then copies `fb_pitch` bytes for every framebuffer row from `back_fb` to `front_fb`.

After the memory copy, it checks `virtio_gpu_available()`. When VirtIO GPU presentation is available, it also calls:

```c
virtio_gpu_present(back_fb, fb_width, fb_height, fb_pitch);
```

The back buffer is not cleared after presentation. Its contents remain available for subsequent drawing or partial updates.

### `gfx_flip_rect()`

```c
void gfx_flip_rect(int32_t x, int32_t y,
                   uint32_t w, uint32_t h);
```

Presents only the specified back-buffer rectangle.

The rectangle is clipped to framebuffer bounds. Each visible row is copied from the corresponding back-buffer range to the front buffer. If VirtIO GPU presentation is available, the same region is passed to `virtio_gpu_present_rect()`.

This function does not use the current drawing clip rectangle. Its coordinates define the region to copy to the visible framebuffer; they do not change or query the drawing clip state.

The function returns without work when presentation has been seized, either framebuffer buffer is unavailable, or the clipped rectangle is empty.

### `gfx_bsod_seize()`

```c
void gfx_bsod_seize(void);
```

Sets the global `bsod_seized` flag. While this flag is set, both `gfx_flip()` and `gfx_flip_rect()` return without presenting the back buffer.

The function does not stop drawing operations or clear either buffer. There is no corresponding release or reset function in this implementation, so the state remains set for the rest of the runtime unless modified elsewhere.

### `gfx_get_width()`

```c
uint32_t gfx_get_width(void);
```

Returns the framebuffer width stored by `gfx_init()`.

### `gfx_get_height()`

```c
uint32_t gfx_get_height(void);
```

Returns the framebuffer height stored by `gfx_init()`.

Both functions return the stored dimensions directly. They do not query the boot framebuffer again.

## 14. Internal helper functions

The following functions are `static` or `static inline` implementation details. They are not intended to be called directly by other modules.

| Helper | Description |
|---|---|
| `make_color(uint32_t rgb)` | Extracts `0xRRGGBB` channels and packs them using the framebuffer's red, green, and blue shifts. It is used by drawing functions that accept ordinary RGB values. |
| `gfx_memset32_sse2(uint32_t *dst, uint32_t v, uint32_t n)` | Fills a 32-bit pixel array using SSE2 stores in groups of four pixels, then writes any remaining pixels with a scalar loop. It is compiled with a function-specific `target("sse2")` attribute. |
| `gfx_memset32(uint32_t *dst, uint32_t v, uint32_t n)` | Selects the SSE2 fill helper on supported x86-64 builds when at least four pixels are requested; otherwise uses a scalar loop. |
| `gfx_blit_rgb888_sse2(...)` | SIMD implementation of `gfx_blit_rgb888()`. It handles masked groups of eight pixels using two four-pixel vector operations, and unmasked groups of four pixels. It uses scalar code for unaligned mask prefixes and trailing pixels. The helper uses SSE2 intrinsics and a `target("sse2")` attribute. |
| `gfx_clip_rect_i64(...)` | Intersects a signed-origin rectangle with the active clip rectangle and framebuffer bounds. Optionally returns offsets into the original source rectangle; used by rectangle fills, blits, blur, rounded rectangles, and gradients. |
| `gfx_fill_rect_i64(...)` | Internal signed-coordinate rectangle fill. Uses `gfx_clip_rect_i64()` before writing and is used where public coordinates are signed. |
| `gfx_putpixel_i64(...)` | Checks signed 64-bit coordinates against framebuffer and clip bounds before forwarding a valid pixel to `gfx_putpixel()`. |
| `gfx_line_outcode(int64_t x, int64_t y, int64_t xmax, int64_t ymax)` | Calculates a Cohen–Sutherland outcode indicating whether a point is left, right, above, or below the framebuffer rectangle. Used by `gfx_draw_line()`. |
| `gfx_muldiv_i64(int64_t a, int64_t b, int64_t c)` | Calculates a signed multiply/divide operation for line-clipping intersections. Its inputs are bounded by the public API's unsigned 32-bit endpoint values. |
| `gfx_div255(uint32_t x)` | Computes integer division by 255 using `(x + 1 + (x >> 8)) >> 8`. The helper is used for exact 8-bit channel blending over the valid product-sum range. |
| `gfx_blend_native_u32(uint32_t d, uint32_t s, uint32_t a)` | Blends one native 32-bit destination pixel with one native source pixel using the supplied alpha. The operation is performed independently on all four bytes. |
| `gfx_blend4_sse2_native(...)` | Blends four destination pixels with four distinct alpha values using SSE2. It unpacks the four 32-bit pixels into 16-bit channels, performs the weighted sums, applies the divide-by-255 adjustment, and packs the result back to 8-bit channels. |
| `gfx_fill4_sse2_native(...)` | Blends one source pixel with a shared alpha over four destination pixels using SSE2. Used by constant-alpha rectangle fills. |
| `gfx_blend4_scalar_native(...)` | Scalar equivalent of the four-pixel, four-alpha blend operation. Calls `gfx_blend_native_u32()` for each pixel. |
| `gfx_fill4_scalar_native(...)` | Scalar equivalent of the four-pixel constant-alpha fill operation. |
| `gfx_blend4_native(...)` | Dispatches a four-pixel, four-alpha blend to SSE2 on x86-64 or to the scalar helper on other targets. |
| `gfx_fill4_native(...)` | Dispatches a four-pixel shared-alpha blend to SSE2 on x86-64 or to the scalar helper on other targets. |
| `gfx_circle_coverage(...)` | Computes a circle's coverage for one pixel using a 4×4 supersampling grid. Returns a value from `0` to `255`. Large squared-distance inputs are scaled before multiplication to keep intermediate values within `uint64_t`. |
| `gfx_aa_pixel(...)` | Applies a coverage value to one pixel. Coverage zero is ignored, full coverage uses `gfx_putpixel()`, and partial coverage uses `gfx_blend_pixel()`. |
| `gfx_lerp8(uint32_t a, uint32_t b, uint32_t t256)` | Performs one 8.8 fixed-point interpolation step. The expected interpolation factor is in the range `0..256`. Used by the four-corner gradient. |

## 15. SIMD implementation

SIMD support is enabled at compile time only when `__x86_64__` is defined:

```c
#if defined(__x86_64__)
#define GFX_HAVE_SIMD 1
#else
#define GFX_HAVE_SIMD 0
#endif
```

On other targets, the corresponding operations use scalar implementations.

The kernel is otherwise compiled with SSE/SSE2 disabled. SIMD helpers are isolated using function-specific target attributes so that the rest of the graphics code does not need to be compiled with those instruction sets enabled.

The current SIMD paths are:

| Operation | SIMD path | Processing width |
|---|---|---:|
| 32-bit fill | `gfx_memset32_sse2()` | 4 pixels per vector store |
| Unmasked `gfx_blit_rgb888()` | `gfx_blit_rgb888_sse2()` | 4 pixels per vector operation |
| Masked `gfx_blit_rgb888()` | `gfx_blit_rgb888_sse2()` | 8 pixels per mask-byte group |
| Four-pixel variable-alpha blend | `gfx_blend4_sse2_native()` | 4 pixels |
| Four-pixel constant-alpha blend | `gfx_fill4_sse2_native()` | 4 pixels |
| `gfx_blend_span()` | Through `gfx_blend4_native()` | 4 pixels per group |
| `gfx_fill_rect_alpha()` | Through `gfx_fill4_native()` | 4 pixels per group |

The SIMD helpers use unaligned loads and stores where appropriate. The scalar tail handles widths that are not multiples of the vector processing width.

There is no runtime CPU feature detection in this file. The SIMD path is selected by the compile-time architecture macro. This is appropriate for the x86-64 target where SSE2 is part of the architecture baseline. All SIMD paths in this file use SSE2; there is no SSE4.1 implementation.

## 16. Pixel counter behavior

The counter measures work as defined by each drawing function. It should not be interpreted as a strict count of unique framebuffer pixels changed.

| Function/path | Counter behavior |
|---|---|
| `gfx_putpixel()` | Adds one for each accepted pixel write. |
| `gfx_fill_rect()` | Adds the visible rectangle area. |
| `gfx_blit_rgb888()` | Adds the number of pixels copied; skipped mask pixels are excluded. |
| `gfx_draw_rect()` | Inherits `gfx_putpixel()` counting. Shared corner pixels may be counted more than once. |
| `gfx_draw_line()` | Inherits `gfx_putpixel()` counting for each rasterized point accepted by the clip checks. |
| Text drawing | Inherits `gfx_putpixel()` counting for set glyph bits only. |
| `gfx_blend_pixel()` | Adds one when alpha is nonzero and the pixel passes all checks. Alpha zero adds nothing. |
| `gfx_blend_pixels4()` | Adds four for a fully visible run, regardless of alpha values; partial runs add their visible length, including zero-alpha positions. |
| `gfx_blend_span()` | Adds the visible span length, including zero-alpha positions. |
| `gfx_fill_rect_alpha()` | Adds the full visible area when alpha is nonzero. |
| `gfx_blit_alpha()` | Adds the number of source pixels with nonzero alpha. |
| `gfx_box_blur_rect()` | Adds the processed region area after the blur. |
| `gfx_fill_round_rect_aa()` | Inherits counts from its opaque fills and per-pixel AA writes. Its count depends on coverage and helper calls. |
| `gfx_gradient_rect_4()` | Adds the visible rectangle area. |
| `gfx_gradient_v()` | Inherits counts from the row-wise `gfx_fill_rect()` calls. |
| `gfx_draw_circle()` / `gfx_fill_circle()` | Inherit counts from `gfx_putpixel()`; repeated symmetric points can be counted more than once. |
| `gfx_back_get_rect()` | Does not increment the counter. |
| `gfx_back_put_rect()` | Does not increment the counter. |
| `gfx_flip()` / `gfx_flip_rect()` | Do not increment the counter. |
| `gfx_clear()` | Inherits `gfx_fill_rect()` counting. |

The counter is reset only when `gfx_take_px_counter()` is called. It is not automatically reset by `gfx_flip()`.

## 17. Important implementation notes

1. **32-bit framebuffer only.** `gfx_init()` rejects any framebuffer whose `bpp` is not 32.
2. **No automatic clearing after presentation.** `gfx_flip()` and `gfx_flip_rect()` copy the back buffer to the front buffer but leave the back buffer unchanged.
3. **Global clipping state.** The clip rectangle is shared by all drawing callers. It is not stored per window or per thread.
4. **Not every operation uses the same clipping path.** Most drawing functions respect the active clip rectangle. Raw back-buffer access and presentation functions have separate bounds behavior.
5. **`gfx_blit_rgb888()` performs a raw 32-bit copy.** It does not convert RGB channel order despite its name.
6. **Text is byte-indexed.** The text renderer does not decode UTF-8; glyph selection is based on individual bytes.
7. **The blur routine needs an intersecting rectangle.** Its current clipping code lacks an empty-intersection check before unsigned dimension calculations.
8. **Rounded rectangles have a negative-origin limitation.** Their interior fill strips convert signed coordinates to unsigned before calling `gfx_fill_rect()`.
9. **Gradient clipping differs between gradient functions.** The four-corner gradient recalculates interpolation across the clipped area, while the vertical gradient retains interpolation based on the original requested height.
10. **SIMD dispatch is compile-time.** The implementation does not perform runtime feature detection, and the RGB blit helper's `sse41` suffix does not match its actual SSE2 target attribute.

## 18. Function index

| Function | Category |
|---|---|
| `gfx_init()` | Initialization |
| `gfx_clip_set()` | Clip management |
| `gfx_clip_clear()` | Clip management |
| `gfx_get_clip()` | Clip management |
| `gfx_take_px_counter()` | Statistics |
| `gfx_putpixel()` | Pixel drawing |
| `gfx_fill_rect()` | Rectangle drawing |
| `gfx_draw_rect()` | Rectangle drawing |
| `gfx_blit_rgb888()` | Blitting |
| `gfx_blit_alpha()` | Alpha blitting |
| `gfx_draw_line()` | Line drawing |
| `gfx_draw_circle()` | Circle drawing |
| `gfx_fill_circle()` | Circle drawing |
| `gfx_draw_string()` | Text |
| `gfx_draw_text()` | Text wrapper |
| `gfx_text_width()` | Text measurement |
| `gfx_draw_text_centered()` | Text layout |
| `gfx_blend()` | Color interpolation |
| `gfx_blend_pixel()` | Pixel blending |
| `gfx_blend_pixels4()` | Four-pixel blending |
| `gfx_blend_span()` | Span blending |
| `gfx_fill_rect_alpha()` | Translucent rectangle fill |
| `gfx_box_blur_rect()` | Blur |
| `gfx_fill_round_rect_aa()` | Anti-aliased shapes |
| `gfx_gradient_rect_4()` | Four-corner gradient |
| `gfx_gradient_v()` | Vertical gradient |
| `gfx_back_get_rect()` | Back-buffer access |
| `gfx_back_put_rect()` | Back-buffer access |
| `gfx_bsod_seize()` | Presentation control |
| `gfx_flip()` | Presentation |
| `gfx_flip_rect()` | Partial presentation |
| `gfx_clear()` | Framebuffer drawing |
| `gfx_get_width()` | Framebuffer information |
| `gfx_get_height()` | Framebuffer information |

---

*This document describes the behavior visible in the supplied `graphics.c` implementation. Public declarations and type definitions should be checked against `drivers/framebuffer/graphics.h` when maintaining the API documentation.*
