#![no_std]

use rsbr::gui::{
    Window, Event, App, Flow,
    EVT_KEY_DOWN, EVT_MOUSE_DOWN, EVT_MOUSE_MOVE, EVT_MOUSE_UP,
    FLAG_HAS_TITLE, FLAG_HAS_CLOSE, FLAG_RESIZABLE,
};

const WIN_W: u32 = 480;
const WIN_H: u32 = 320;

/* Base (unhovered) button geometry.  Hovered/pressed sizes are computed
 * from these; hit tests use the current visual rect, so hovering enlarges
 * the clickable area consistently with what the user sees. */
const BTN_W: u32 = 120;
const BTN_H: u32 = 40;
const BTN1_X: i32 = 40;
const BTN1_Y: i32 = 120;
const BTN2_X: i32 = 320;
const BTN2_Y: i32 = 120;

/* Hover scale: 3/2 = 1.5x.  Integer math so the demo stays `no_std` and
 * free of floating point. */
const HOVER_NUM: u32 = 3;
const HOVER_DEN: u32 = 2;

/* Pressed scale: 7/8 — slightly smaller than base, reads as "pushed in". */
const PRESSED_NUM: u32 = 7;
const PRESSED_DEN: u32 = 8;

const BG:    u32 = 0x0018202C;
const DIM:   u32 = 0x008090A0;
const TEXT:  u32 = 0x00E8E8F0;
const BTN_BORDER: u32 = 0x00607090;
const BTN_BG:     u32 = 0x002F60C0;
const BTN_HOVER:  u32 = 0x003D78E0;
const BTN_ACTIVE: u32 = 0x00204890;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Hit { None, Btn1, Btn2 }

/* The geometry a button currently occupies, given its base rect and its
 * visual state.  Render and hit-test both go through this, so the enlarged
 * hover box can always be clicked and its background always lines up with
 * what the user sees. */
#[derive(Clone, Copy)]
struct Rect { x: i32, y: i32, w: u32, h: u32 }

fn scaled_rect(bx: i32, by: i32, bw: u32, bh: u32,
               num: u32, den: u32) -> Rect {
    let nw = bw * num / den;
    let nh = bh * num / den;
    /* Grow/shrink around the base centre. */
    let cx = bx + (bw as i32) / 2;
    let cy = by + (bh as i32) / 2;
    Rect {
        x: cx - (nw as i32) / 2,
        y: cy - (nh as i32) / 2,
        w: nw,
        h: nh,
    }
}

fn rect_contains(r: &Rect, mx: i32, my: i32) -> bool {
    mx >= r.x && mx < r.x + r.w as i32
        && my >= r.y && my < r.y + r.h as i32
}

fn button_rect(base_x: i32, base_y: i32,
               hover: bool, pressed: bool) -> Rect {
    if pressed {
        scaled_rect(base_x, base_y, BTN_W, BTN_H, PRESSED_NUM, PRESSED_DEN)
    } else if hover {
        scaled_rect(base_x, base_y, BTN_W, BTN_H, HOVER_NUM, HOVER_DEN)
    } else {
        Rect { x: base_x, y: base_y, w: BTN_W, h: BTN_H }
    }
}

fn hit_test(mx: i32, my: i32, hover: Hit) -> Hit {
    /* Hit-test against the CURRENT visual rect of each button.  A hovered
     * button is 1.5x larger, so the enlarged area is clickable too — the
     * user never has to "find" the base rect again after the button grew. */
    let r1 = button_rect(BTN1_X, BTN1_Y, hover == Hit::Btn1, false);
    let r2 = button_rect(BTN2_X, BTN2_Y, hover == Hit::Btn2, false);
    if rect_contains(&r1, mx, my) { Hit::Btn1 }
    else if rect_contains(&r2, mx, my) { Hit::Btn2 }
    else { Hit::None }
}

struct RustesApp {
    count: i32,
    hover: Hit,
    /* Button on which MOUSE_DOWN landed; the click only counts on
     * MOUSE_UP over the same button.  None between events. */
    pressed: Hit,
}

impl App for RustesApp {
    fn update(&mut self, ev: &Event) -> Flow {
        match ev.kind {
            EVT_MOUSE_DOWN => {
                let h = hit_test(ev.x, ev.y, self.hover);
                if h != Hit::None {
                    self.pressed = h;
                    Flow::Changed
                } else {
                    Flow::Ignore
                }
            }
            EVT_MOUSE_UP => {
                if self.pressed == Hit::None {
                    return Flow::Ignore;
                }
                let h = hit_test(ev.x, ev.y, self.hover);
                if h == self.pressed {
                    self.count += if h == Hit::Btn1 { 1 } else { -1 };
                }
                self.pressed = Hit::None;
                Flow::Changed
            }
            EVT_MOUSE_MOVE => {
                /* Hover is computed against the CURRENT visual rect (so
                 * a hovered button keeps its enlarged box while the
                 * cursor is anywhere inside it). */
                let h = hit_test(ev.x, ev.y, self.hover);
                if h != self.hover {
                    self.hover = h;
                    Flow::Changed
                } else {
                    Flow::Ignore
                }
            }
            EVT_KEY_DOWN => {
                if ev.key == 0x1B || ev.key == b'q' as u32 {
                    return Flow::Quit;
                }
                self.count += 1;
                Flow::Changed
            }
            _ => Flow::Ignore,
        }
    }

    fn render(&self, w: &Window) -> rsbr::Result<()> {
        w.clear(BG)?;
        w.draw_text(24, 24, "Hello, AuraLite (Rust edition)", TEXT)?;
        w.draw_text(24, 48, "Click a button to change the counter.", DIM)?;

        let r1 = button_rect(BTN1_X, BTN1_Y,
                             self.hover == Hit::Btn1,
                             self.pressed == Hit::Btn1);
        let r2 = button_rect(BTN2_X, BTN2_Y,
                             self.hover == Hit::Btn2,
                             self.pressed == Hit::Btn2);

        draw_button(w, &r1, "+1",
                    self.hover == Hit::Btn1,
                    self.pressed == Hit::Btn1)?;
        draw_button(w, &r2, "-1",
                    self.hover == Hit::Btn2,
                    self.pressed == Hit::Btn2)?;

        let mut buf = [0u8; 16];
        let s = fmt_i32(&mut buf, self.count);
        w.draw_text(WIN_W as i32 / 2 - 8 * s.len() as i32, 220, s, TEXT)?;

        w.draw_text(24, WIN_H as i32 - 36,
                    "Esc or Q to quit.  Window [X] also quits.",
                    DIM)?;
        Ok(())
    }
}

fn draw_button(w: &Window, r: &Rect, label: &str,
               hover: bool, pressed: bool) -> rsbr::Result<()> {
    let bg = if pressed { BTN_ACTIVE } else if hover { BTN_HOVER } else { BTN_BG };
    w.fill_rect(r.x, r.y, r.w, r.h, bg)?;
    w.draw_rect(r.x, r.y, r.w, r.h, BTN_BORDER)?;

    /* Font metrics: 8 px advance, 16 px cell.  When the button grows, the
     * label grows with it so the hover effect is visible in the text too. */
    let char_w: u32 = 8;
    let line_h: u32 = 16;
    let text_w = label.len() as u32 * char_w;
    /* Slight extra for pressed (feels like it "sinks"). */
    let grow = if pressed { 0 } else { 0 }; // reserved for future tweaks
    let tx = r.x + (r.w as i32 - text_w as i32) / 2;
    let ty = r.y + (r.h as i32 - line_h as i32) / 2 + grow;
    w.draw_text(tx, ty, label, TEXT)?;
    Ok(())
}

fn fmt_i32(buf: &mut [u8; 16], mut n: i32) -> &str {
    let neg = n < 0;
    if neg { n = -n; }
    let mut i = buf.len();
    if n == 0 { i -= 1; buf[i] = b'0'; }
    else {
        while n > 0 && i > 0 { i -= 1; buf[i] = b'0' + (n % 10) as u8; n /= 10; }
    }
    if neg && i > 0 { i -= 1; buf[i] = b'-'; }
    core::str::from_utf8(&buf[i..]).unwrap_or("?")
}

#[no_mangle]
pub extern "C" fn main() -> i32 {
    let w = match Window::create(
        100, 100, WIN_W, WIN_H,
        Some("Buttons demo"),
        FLAG_HAS_TITLE | FLAG_HAS_CLOSE | FLAG_RESIZABLE,
    ) {
        Ok(w) => w,
        Err(e) => { rsbr::eprintln!("create: {}", e); return 1; }
    };
    if w.show().is_err() { return 1; }

    match rsbr::gui::run(RustesApp { count: 0, hover: Hit::None, pressed: Hit::None }, w) {
        Ok(()) => 0,
        Err(e) => { rsbr::eprintln!("run: {}", e); 1 }
    }
}