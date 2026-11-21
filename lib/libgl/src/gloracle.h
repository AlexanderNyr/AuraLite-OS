#ifndef LIBGL_GLORACLE_H
#define LIBGL_GLORACLE_H

/* lib/libgl/src/gloracle.h — GPU_ACCEL_PLAN H6: the frame oracle.
 *
 * The question this file answers is not "does the cube look right" but
 * "is the hardware path the SAME function as the software path".  A scene is
 * rendered, hashed, and the hash is compared against a stored one; the
 * comparison carries an explicit tolerance because two correct rasterizers
 * are not obliged to agree to the last bit.
 *
 * WHY A DEDICATED SCENE RATHER THAN "whatever the demo drew"
 *
 * A hash is only meaningful if its input is reproducible.  A demo's frame
 * depends on the window size, the clock, keyboard state and whatever the
 * compositor did before.  This scene is a pure function of its parameters:
 * fixed geometry, a fixed orthographic projection, no time, no RNG, no
 * allocation.  Render it twice and the bytes are identical; change one vertex
 * and the hash changes.  That last property is what makes the oracle a
 * regression test rather than a checksum — see test_gloracle.c's mutation
 * cases, where a shifted vertex, a swapped colour channel and a flipped row
 * each have to produce a DIFFERENT hash for the oracle to be worth running.
 *
 * WHY SHA-256 AND NOT A CHECKSUM
 *
 * The tree already has ksha256() compiled both into the kernel and into a
 * host unit test (kernel/lib/sha256.{h,c}), so the oracle uses the same
 * implementation on both lanes and a hash is comparable across them.  A
 * lighter checksum would make a "hash differs" report less actionable: with a
 * digest a conflict means "some pixel differs", with a weak checksum it could
 * mean "the checksum collided with itself".
 *
 * THE TOLERANCE MODEL
 *
 * Two correct implementations may differ in exactly two ways, and the model
 * names them:
 *
 *   1. Coverage: a pixel whose centre lies within a rounding error of an edge
 *      may be filled by one and not the other.  The software rasterizer works
 *      in fixed point and the GPU's viewport transform rounds independently,
 *      so edge pixels are legitimately unstable.  GLORACLE_TOLERANCE_PERMILLE
 *      bounds the fraction of such pixels.
 *   2. Value: an interior pixel's colour may differ by one step in the lowest
 *      byte when a blend factor is computed at different precision.
 *
 * What the model does NOT tolerate is a colour band appearing, moving or
 * vanishing: that is a coverage change far larger than an epsilon along an
 * edge, and it shows up as differing pixels well past the tolerance.  The
 * per-channel allowance is therefore 1 (out of 255), and a pixel differing by
 * more counts as fully differing — a half-brightness band cannot sneak in
 * under "small numeric noise".
 *
 * The two numbers below are the plan's stated tolerance, in code, and the
 * checker pins them against the document so the two cannot drift apart.
 */

#include <stdint.h>
#include <stddef.h>

/* Differing-pixel budget, in thousandths of the frame.  5/1000 at 64x64 is
 * two pixels — room for the four edge pixels of a triangle's perimeter to
 * round differently, and not enough room for a band. */
#define GLORACLE_TOLERANCE_PERMILLE 5u

/* Per-channel allowance for a pixel to still count as "agreeing".  1 of 255:
 * the difference between two roundings of the same blend, not a shading
 * difference. */
#define GLORACLE_CHANNEL_EPSILON 1u

/* The oracle's scene is rendered into a fixed-size square so the hash does
 * not depend on a window's dimensions.  64 is the size every existing GL test
 * already uses. */
#define GLORACLE_W 64
#define GLORACLE_H 64

typedef struct {
    uint32_t pixels_differing;   /* outside the per-channel epsilon          */
    uint32_t pixels_total;       /* GLORACLE_W * GLORACLE_H                  */
    uint32_t max_channel_delta;  /* worst per-channel difference seen        */
    uint32_t tolerance_pixels;   /* the budget this verdict used             */
    int      pass;               /* 1 = within tolerance                     */
} gloracle_verdict_t;

/* Render the deterministic scene into a caller-owned ARGB buffer of
 * GLORACLE_W * GLORACLE_H words.  Self-contained: creates its own context,
 * draws, reads the colour buffer back with glReadPixels-correct orientation,
 * destroys the context.  Returns 0 on success, non-zero if the context could
 * not be created (which is a real failure, not a skip).
 *
 * The scene is a lit, depth-tested pair of triangles — the same shape of draw
 * H5 made eligible — with a fixed colour per vertex.  It is drawn through
 * whichever backend is ACTIVE, which is what lets the same function serve
 * both lanes. */
int gloracle_render(uint32_t *out);

/* SHA-256 of a frame, rendered by the standard hex string for logs. */
void gloracle_hash(const uint32_t *pixels, uint8_t out[32]);
void gloracle_hash_hex(const uint32_t *pixels, char out[65]);

/* Compare two frames under the tolerance model above. */
void gloracle_compare(const uint32_t *a, const uint32_t *b,
                      gloracle_verdict_t *v);

#endif /* LIBGL_GLORACLE_H */
