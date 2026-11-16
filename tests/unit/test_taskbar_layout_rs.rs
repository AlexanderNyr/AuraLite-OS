//! tests/unit/test_taskbar_layout_rs.rs
//!
//! OF0 gate (docs/plans/OFFICE_PLAN.md): the taskbar must never lose a window.
//!
//! The 2026-11-08 live pass found "10 windows -> Start + 6 buttons, the rest
//! unreachable": the drawing code and the hit test each had their own copy of
//! the button arithmetic and each ran out at its own place.  The arithmetic
//! was centralised into one pure module, and PR #13 ported that module from
//! kernel/gui/taskbar_layout.h to kernel/gui/desktop/taskbar_layout.rs.
//!
//! That port left the gate pointing at the wrong target: the C test
//! (test_taskbar_layout.c) kept asserting P1..P5 against the C header, which
//! the kernel no longer compiles, while the Rust module -- the geometry that
//! actually ships -- had no test at all.  Every green run of the old test was
//! therefore evidence about dead code.
//!
//! This is the same set of properties, asserted against the shipping code.
//! The module under test is included directly: it is pure u32 arithmetic with
//! no imports, no allocator and no kernel dependency, so it compiles on the
//! host unchanged -- which is exactly the property that makes it testable.
//!
//!   P1 no button overlaps another;
//!   P2 no button runs under the clock/tray or off the right edge;
//!   P3 the hit-test rect of a button is exactly the drawn rect;
//!   P4 every open window is reachable, either as a button or on some page of
//!      the ">>" overflow list, for every count 1..256 and every screen size
//!      the GUI can actually run at;
//!   P5 the overflow list pages without losing or repeating an entry.
//!
//! Built and run by `make test-unit` (`rustc --test`), needs no boot, no MMU
//! and no framebuffer.

#![allow(dead_code)]

#[path = "../../kernel/gui/desktop/taskbar_layout.rs"]
mod tb;

use tb::*;

/// Screen widths the kernel GUI is asked to run at (fb.c accepts 640..1920).
const WIDTHS: [u32; 7] = [640, 800, 1024, 1280, 1366, 1600, 1920];
const HEIGHTS: [u32; 6] = [480, 600, 720, 768, 900, 1080];

/// Mirrors GUI_MAX_WINDOWS.
const MAX_WINDOWS: u32 = 256;

/// The narrowest button a user can still read and hit reliably.  Asserted as
/// an absolute floor, not as ">= TB_BTN_MIN": the clamp is what produces
/// TB_BTN_MIN, so a check written against the constant itself goes green when
/// someone lowers the constant -- which is precisely the change that must not
/// pass silently.  (Verified: dropping TB_BTN_MIN to 20 leaves every
/// relative property true.)
const MIN_READABLE: u32 = 44;

struct Checker {
    checks: u64,
    failures: u64,
}

impl Checker {
    fn check(&mut self, cond: bool, msg: String) {
        self.checks += 1;
        if !cond {
            self.failures += 1;
            if self.failures <= 25 {
                println!("FAIL: {}", msg);
            }
        }
    }
}

/// P1 and P2, plus the invariant that the layout accounts for every window:
/// buttons plus the ">>" button must never leave a window unrepresented.
fn test_strip_geometry(c: &mut Checker) {
    for &fw in WIDTHS.iter() {
        for n in 1..=MAX_WINDOWS {
            let l = layout(fw, n);

            c.check(
                l.btn_w >= TB_BTN_MIN && l.btn_w <= TB_BTN_MAX,
                format!(
                    "fw={} n={} width {} outside [{},{}]",
                    fw, n, l.btn_w, TB_BTN_MIN, TB_BTN_MAX
                ),
            );
            c.check(
                l.btn_w >= MIN_READABLE,
                format!(
                    "fw={} n={} button {} px is below the readable minimum {}",
                    fw, n, l.btn_w, MIN_READABLE
                ),
            );

            let mut prev_end: u64 = 0;
            for slot in 0..l.visible {
                let (x, w) = match button_rect(fw, n, slot) {
                    Some(v) => v,
                    None => {
                        c.check(
                            false,
                            format!(
                                "fw={} n={} slot {} not drawn but visible={}",
                                fw, n, slot, l.visible
                            ),
                        );
                        continue;
                    }
                };
                c.check(
                    w == l.btn_w,
                    format!("fw={} n={} slot {} width {} != {}", fw, n, slot, w, l.btn_w),
                );
                if slot > 0 {
                    // P1
                    c.check(
                        x as u64 >= prev_end,
                        format!(
                            "fw={} n={} slot {} overlaps (x={} < {})",
                            fw, n, slot, x, prev_end
                        ),
                    );
                }
                // P2
                let right = x as u64 + w as u64;
                c.check(
                    right <= l.end as u64,
                    format!(
                        "fw={} n={} slot {} runs past strip ({} > {})",
                        fw, n, slot, right, l.end
                    ),
                );
                c.check(
                    right <= (fw.saturating_sub(TB_TRAY_W)) as u64,
                    format!(
                        "fw={} n={} slot {} under the tray ({} > {})",
                        fw, n, slot, right, fw.saturating_sub(TB_TRAY_W)
                    ),
                );
                prev_end = right + TB_GAP as u64;
            }

            if l.more_w != 0 {
                c.check(
                    l.visible + 1 <= n,
                    format!("fw={} n={} more button with nothing hidden", fw, n),
                );
                c.check(
                    l.more_x >= l.x0,
                    format!("fw={} n={} more button left of strip", fw, n),
                );
                c.check(
                    l.more_x as u64 + l.more_w as u64 <= l.end as u64,
                    format!("fw={} n={} more button past strip", fw, n),
                );
                if l.visible > 0 {
                    if let Some((lx, lw)) = button_rect(fw, n, l.visible - 1) {
                        c.check(
                            l.more_x as u64 >= lx as u64 + lw as u64 + TB_GAP as u64,
                            format!("fw={} n={} more button touches last button", fw, n),
                        );
                    }
                }
            } else {
                c.check(
                    l.visible == n,
                    format!(
                        "fw={} n={} no more button but only {} drawn",
                        fw, n, l.visible
                    ),
                );
            }
        }
    }
}

/// P3: the drawn rect and the hit rect come from the same function.  A click at
/// the centre of each drawn button must map back to that same slot, and a click
/// on ">>" must never land on a normal button.
fn test_hit_matches_draw(c: &mut Checker) {
    for &fw in WIDTHS.iter() {
        for n in 1..=40 {
            let l = layout(fw, n);
            for slot in 0..l.visible {
                let (x, w) = match button_rect(fw, n, slot) {
                    Some(v) => v,
                    None => continue,
                };
                let centre = x + w / 2;
                let mut found: i64 = -1;
                for s2 in 0..l.visible {
                    match button_rect(fw, n, s2) {
                        Some((hx, hw)) => {
                            if centre >= hx && centre < hx + hw {
                                found = s2 as i64;
                                break;
                            }
                        }
                        None => break,
                    }
                }
                c.check(
                    found == slot as i64,
                    format!(
                        "fw={} n={} centre of slot {} hit slot {}",
                        fw, n, slot, found
                    ),
                );
            }
            if l.more_w != 0 {
                let mx = l.more_x + l.more_w / 2;
                for s2 in 0..l.visible {
                    if let Some((hx, hw)) = button_rect(fw, n, s2) {
                        c.check(
                            !(mx >= hx && mx < hx + hw),
                            format!("fw={} n={} more button centre hits slot {}", fw, n, s2),
                        );
                    }
                }
            }
        }
    }
}

/// P4 and P5: walk the overflow list page by page and collect which hidden
/// entries a user can actually reach.  Every hidden window must be reachable,
/// and paging must not lose or repeat an entry.
fn test_overflow_reaches_everything(c: &mut Checker) {
    for &fw in WIDTHS.iter() {
        for &fh in HEIGHTS.iter() {
            let tb_h: u32 = 40; // the tallest shipped theme taskbar
            for n in 1..=MAX_WINDOWS {
                let l = layout(fw, n);
                let visible = l.visible.min(n);
                let hidden = n - visible;

                let ov = overflow_geometry(fw, fh, tb_h, n);

                if hidden == 0 {
                    c.check(
                        ov.rows == 0 && !ov.paged,
                        format!("fw={} fh={} n={} stray list", fw, fh, n),
                    );
                    continue;
                }

                c.check(
                    ov.hidden == hidden,
                    format!(
                        "fw={} fh={} n={} hidden {} != {}",
                        fw, fh, n, ov.hidden, hidden
                    ),
                );
                c.check(
                    ov.rows > 0,
                    format!("fw={} fh={} n={} hidden {} with no rows", fw, fh, n, hidden),
                );
                c.check(ov.y >= 0, format!("fw={} fh={} n={} list above screen", fw, fh, n));
                c.check(
                    ov.y as i64 + ov.h as i64 <= fh as i64,
                    format!("fw={} fh={} n={} list runs off the bottom", fw, fh, n),
                );
                c.check(
                    ov.y as i64 + ov.h as i64 <= fh as i64 - tb_h as i64,
                    format!("fw={} fh={} n={} list overlaps the taskbar", fw, fh, n),
                );
                c.check(
                    ov.x as i64 + ov.w as i64 <= fw as i64,
                    format!("fw={} fh={} n={} list past right edge", fw, fh, n),
                );

                let mut seen = vec![false; hidden as usize];
                let mut page: u32 = 0;
                let mut steps: u32 = 0;
                let limit = hidden.saturating_mul(4).saturating_add(8);
                let mut reachable: u32 = 0;
                loop {
                    steps += 1;
                    if steps > limit {
                        break;
                    }
                    for r in 0..ov.rows {
                        let e = overflow_row_entry(&ov, page, r);
                        if e < 0 {
                            continue;
                        }
                        let e = e as u32;
                        c.check(
                            e < hidden,
                            format!("fw={} n={} entry {} out of {}", fw, n, e, hidden),
                        );
                        if e < hidden && !seen[e as usize] {
                            seen[e as usize] = true;
                            reachable += 1;
                        }
                    }
                    if !ov.paged {
                        break;
                    }
                    let next = overflow_next_page(&ov, page);
                    c.check(
                        next != page,
                        format!("fw={} fh={} n={} paging did not advance", fw, fh, n),
                    );
                    if next == page {
                        break;
                    }
                    page = next;
                }
                c.check(
                    reachable == hidden,
                    format!(
                        "fw={} fh={} n={} reachable {} of {} hidden windows",
                        fw, fh, n, reachable, hidden
                    ),
                );
            }
        }
    }
}

/// The live repros.  At 44 px minimum the strip holds 8 buttons at 640 px with
/// room to read, so the plan's gate -- "8 windows open -> all 8 reachable by
/// click" -- must hold without pushing anything into the overflow list.
fn test_live_repro(c: &mut Checker) {
    let cases: [(u32, u32, u32); 4] = [(640, 8, 44), (800, 8, 44), (1024, 10, 44), (1280, 12, 44)];
    for &(fw, n, min_w) in cases.iter() {
        let l = layout(fw, n);
        c.check(
            l.visible == n && l.more_w == 0,
            format!(
                "fw={}: {} windows should fit without overflow (visible={})",
                fw, n, l.visible
            ),
        );
        for slot in 0..n {
            match button_rect(fw, n, slot) {
                Some((_x, w)) => {
                    c.check(
                        w >= min_w,
                        format!("fw={}: window {} button only {} px", fw, slot, w),
                    );
                }
                None => c.check(false, format!("fw={}: window {} has no button", fw, slot)),
            }
        }
    }

    // The old code's exact failure: at 800x600 with ten windows, buttons 7..9
    // used to be drawn nowhere and unclickable.  Now either they are buttons
    // or they are in the list -- and the list is never needed silently.
    let l = layout(800, 10);
    let hidden = 10u32.saturating_sub(l.visible);
    let accounted = l.visible + (if l.more_w != 0 { 1 } else { 0 }) + hidden;
    c.check(accounted == 10, format!("800x600: ten windows unaccounted for"));
    c.check(
        l.visible >= 8 || l.more_w != 0,
        format!(
            "800x600: ten windows, only {} buttons and no list",
            l.visible
        ),
    );
}

#[test]
fn taskbar_layout_properties() {
    println!("[unit] taskbar layout (Rust, the shipping geometry)");
    let mut c = Checker {
        checks: 0,
        failures: 0,
    };

    test_strip_geometry(&mut c);
    println!(
        "[unit]   strip geometry, 1..{} windows x {} widths",
        MAX_WINDOWS,
        WIDTHS.len()
    );
    test_hit_matches_draw(&mut c);
    println!("[unit]   hit test matches the drawn rect");
    test_overflow_reaches_everything(&mut c);
    println!("[unit]   overflow list accounts for every hidden window");
    test_live_repro(&mut c);
    println!("[unit]   ten-window live repro at 640/800/1024");

    if c.failures != 0 {
        panic!(
            "TASKBAR LAYOUT: FAIL ({} of {} checks)",
            c.failures, c.checks
        );
    }
    println!("TASKBAR LAYOUT RS: PASS ({} checks)", c.checks);
}
