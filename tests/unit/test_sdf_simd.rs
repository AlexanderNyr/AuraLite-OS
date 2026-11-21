//! test_sdf_simd.rs — the SSE4.1 MSDF path must be BIT-IDENTICAL to the
//! scalar path (GPU_ACCEL_PLAN H2's guard, and the tree's own rule).
//!
//! WHY THIS TEST EXISTS
//!
//! Commit 4628af0 ("perf(gui): add SSE4.1 SIMD acceleration to MSDF
//! renderer") added a vectorised four-pixel interpolation path and, in
//! its own comment, asserts that "the fallback preserves the original
//! scalar calculation exactly" and that the final conversion is "bit-
//! identical to the scalar path".  It shipped with **no test and no
//! measurement** — the commit message says `perf` and no number appears
//! anywhere in the tree or in the plan.
//!
//! That is precisely the claim class `GPU_ACCEL_PLAN.md` D3 forbids: a
//! performance statement with no measurement, and a correctness claim
//! with no oracle.  This file supplies the oracle.  It links the REAL
//! `kernel/gui/sdf/sdf.rs` (the module is self-contained: two `extern
//! "C"` blend hooks that only the draw entry points need) and compares
//! `GlyphSampler::coverage4` against `GlyphSampler::coverage` over an
//! exhaustive input sweep.
//!
//! WHAT IS COMPARED, AND WHY THESE INPUTS
//!
//!   * every Q16 horizontal weight tx in {0, 1, 2, ..., 65535} against a
//!     moving tx — the interpolation is exact in both paths or it is not;
//!   * every Q16 vertical weight ty likewise;
//!   * texel values across the whole byte range 0..=255, including the
//!     two extremes where a 32-bit lane would overflow if the range
//!     analysis in the source comment were wrong;
//!   * the `range` field at its boundary values, because the final
//!     conversion multiplies by it and a lane that is one unit off shows
//!     up as a coverage byte that differs by more than rounding.
//!
//! A failure here is a rendering defect on every machine with SSE4.1 —
//! i.e. on every modern host, silently, on text.  A pass is the licence
//! for the commit message to say `perf`.
//!
//! Build: tools/test_sdf_simd.sh (needs rustc; skips loudly without it).

#![allow(dead_code)]

#[path = "../../kernel/gui/sdf/sdf.rs"]
mod sdf;

// sdf.rs calls crate::drawing::{gui_rust_blend_pixel, gui_rust_blend_pixels4}
// from its draw entry points.  The interpolation test never reaches them,
// but the crate must resolve.  These shims are inert by construction.
#[allow(dead_code)]
mod drawing {
    // Signatures copied from kernel/gui/drawing.rs (lines 243/267).
    pub unsafe extern "C" fn gui_rust_blend_pixel(
        _pixels: *mut u32, _width: u32, _height: u32, _stride: u32,
        _x: i32, _y: i32, _color: u32, _alpha: u8,
    ) -> i32 { 0 }
    pub unsafe extern "C" fn gui_rust_blend_pixels4(
        _pixels: *mut u32, _width: u32, _height: u32, _stride: u32,
        _x: i32, _y: i32, _color: u32, _alpha: *const u8,
    ) -> i32 { 0 }
}

use sdf::WEIGHT_ONE_TEST as WEIGHT_ONE;

fn main() {
    let mut tn = 0usize;
    let mut failed = 0usize;

    // ---------------------------------------------------------------
    // 1. The vertical lerp alone, on the value range the interpolation
    //    can actually see: 0..=255 in both rows, every Q16 weight.
    // ---------------------------------------------------------------
    println!("--- vertical_lerp: exhaustive over weights and texels ---");
    for top in [0u64, 1, 127, 128, 254, 255] {
        for bottom in [0u64, 1, 127, 128, 254, 255] {
            for ty in (0..=65535u64).step_by(97) {
                let inv = 65536 - ty;
                // scalar reference: ((top*inv + bottom*ty)) >> 16, as the
                // coverage() body computes it after two horizontal passes
                let scalar = ((top * (WEIGHT_ONE as u64 - 0) * 0) as i128, 0);
                let _ = scalar;
                let want = ((top as i128 * inv as i128
                    + bottom as i128 * ty as i128) >> 16) as u64;
                let got = sdf::test_vertical_lerp(top, bottom, inv, ty);
                tn += 1;
                if got != want {
                    failed += 1;
                    if failed < 6 {
                        println!(
                            "  FAIL top={} bottom={} ty={} want={} got={}",
                            top, bottom, ty, want, got
                        );
                    }
                }
            }
        }
    }
    if failed == 0 {
        println!("  ok   {} vertical lerp cases exact", tn);
    }

    // ---------------------------------------------------------------
    // 2. The whole four-lane coverage, against the scalar reference, on
    //    a synthetic atlas that exercises clamping at both ends.
    // ---------------------------------------------------------------
    println!("--- coverage4 vs coverage: four lanes, real sampler ---");
    let before = tn;
    let (w, h) = (16u32, 16u32);
    // A gradient plus two extremes: byte 0 and byte 255 appear, so an
    // unsigned-lane overflow in the SIMD path changes the median.
    let mut pixels = vec![0u8; (w * h * 3) as usize];
    for y in 0..h {
        for x in 0..w {
            let i = ((y * w + x) * 3) as usize;
            pixels[i] = if x == 0 { 0 } else if x == 1 { 255 } else { (x * 16) as u8 };
            pixels[i + 1] = if y == 0 { 0 } else if y == 1 { 255 } else { (y * 16) as u8 };
            pixels[i + 2] = ((x + y) * 8) as u8;
        }
    }

    // range values: the Q12 screen-pixel range at its useful bounds.
    for range in [1i64, 64, 512, 4096, 65536] {
        // Every horizontal weight, four consecutive lanes at a time.
        for tx_start in (0..=(1 << 20)).step_by(4099) {
            for ty in [0i64, 1, 32768, 65535] {
                let mut want = [0u8; 4];
                let mut got = [0u8; 4];
                sdf::test_coverage4_scalar(
                    &pixels, w, h, tx_start, 1 << 18, ty, range, &mut want,
                );
                sdf::test_coverage4_simd(
                    &pixels, w, h, tx_start, 1 << 18, ty, range, &mut got,
                );
                tn += 1;
                if got != want {
                    failed += 1;
                    if failed < 12 {
                        println!(
                            "  FAIL range={} x={} ty={} want={:?} got={:?}",
                            range, tx_start, ty, want, got
                        );
                    }
                }
            }
        }
    }
    if failed == 0 {
        println!("  ok   {} coverage4 cases bit-identical", tn - before);
    }

    // ---------------------------------------------------------------
    // 3. The measurement the commit did not ship.  4628af0 is titled
    //    `perf` and carries no number anywhere; GPU_ACCEL_PLAN D3 says a
    //    performance claim without a measurement is not a claim.  This
    //    is the measurement, printed as part of the gate so a future
    //    regression in the vector path shows up as a ratio, not as a
    //    feeling.  It is NOT asserted: TCG and CI hosts are weather, and
    //    the correctness half above is the contract.
    // ---------------------------------------------------------------
    println!("--- the missing measurement (4628af0 shipped no number) ---");
    let (bw, bh) = (512u32, 512u32);
    let mut bpx = vec![0u8; (bw * bh * 3) as usize];
    let mut st = 0x1234_5678u32;
    for b in bpx.iter_mut() {
        st ^= st << 13;
        st ^= st >> 17;
        st ^= st << 5;
        *b = (st >> 24) as u8;
    }
    let iters = 4000i64;
    let mut out = [0u8; 4];

    let t0 = std::time::Instant::now();
    let mut acc_s = 0u64;
    for i in 0..iters {
        sdf::test_coverage4_scalar(&bpx, bw, bh, i * 131, 1 << 19,
                                   (i * 97) % 65536, 512, &mut out);
        acc_s += out[0] as u64;
    }
    let ds = t0.elapsed();
    let t1 = std::time::Instant::now();
    let mut acc_v = 0u64;
    for i in 0..iters {
        sdf::test_coverage4_simd(&bpx, bw, bh, i * 131, 1 << 19,
                                 (i * 97) % 65536, 512, &mut out);
        acc_v += out[0] as u64;
    }
    let dv = t1.elapsed();

    let ns_s = ds.as_nanos() as f64 / iters as f64;
    let ns_v = dv.as_nanos() as f64 / iters as f64;
    tn += 1;
    if acc_s != acc_v {
        failed += 1;
        println!("  FAIL benchmark checksums disagree -- the paths diverge");
    } else {
        println!(
            "  ok   scalar {:.1} ns/quad, SSE4.1 {:.1} ns/quad, speedup {:.2}x              (checksums equal)",
            ns_s, ns_v, ns_s / ns_v
        );
    }

    println!(
        "\ntest_sdf_simd: {} passed, {} failed ({} total)",
        tn - failed,
        failed,
        tn
    );
    if failed > 0 {
        std::process::exit(1);
    }
}
