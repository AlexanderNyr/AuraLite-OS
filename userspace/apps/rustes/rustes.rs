#![no_std]

use rsbr::gui::{
    Window, Event, App, Flow,
    EVT_KEY_DOWN, EVT_MOUSE_DOWN, EVT_MOUSE_MOVE,
    FLAG_HAS_TITLE, FLAG_HAS_CLOSE, FLAG_RESIZABLE,
};

const WIN_W: u32 = 480;
const WIN_H: u32 = 320;
const BTN_W: u32 = 120;
const BTN_H: u32 = 40;
const BTN1_X: i32 = 40;
const BTN1_Y: i32 = 120;
const BTN2_X: i32 = 320;
const BTN2_Y: i32 = 120;

const BG:    u32 = 0x0018202C;
const DIM:   u32 = 0x008090A0;
const TEXT:  u32 = 0x00E8E8F0;
const BTN_BG:     u32 = 0x002F60C0;
const BTN_HOVER:  u32 = 0x003D78E0;
const BTN_ACTIVE: u32 = 0x00204890;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Hit { None, Btn1, Btn2 }

fn hit_test(mx: i32, my: i32) -> Hit {
    let in_rect = |x: i32, y: i32, w: u32, h: u32| {
        mx >= x && mx < x + w as i32 && my >= y && my < y + h as i32
    };
    if in_rect(BTN1_X, BTN1_Y, BTN_W, BTN_H) { Hit::Btn1 }
    else if in_rect(BTN2_X, BTN2_Y, BTN_W, BTN_H) { Hit::Btn2 }
    else { Hit::None }
}

struct RustesApp {
    count: i32,
    hover: Hit,
    pressed: Hit,
}

impl App for RustesApp {
    fn update(&mut self, ev: &Event) -> Flow {
        match ev.kind {
            EVT_MOUSE_DOWN => {
                let h = hit_test(ev.x, ev.y);
                if h != Hit::None {
                    self.pressed = h;
                    self.count += if h == Hit::Btn1 { 1 } else { -1 };
                    Flow::Changed
                } else {
                    Flow::Ignore
                }
            }
            EVT_MOUSE_MOVE => {
                let h = hit_test(ev.x, ev.y);
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

        draw_button(w, BTN1_X, BTN1_Y, "+1", self.hover == Hit::Btn1, self.pressed == Hit::Btn1)?;
        draw_button(w, BTN2_X, BTN2_Y, "-1", self.hover == Hit::Btn2, self.pressed == Hit::Btn2)?;

        let mut buf = [0u8; 16];
        let s = fmt_i32(&mut buf, self.count);
        w.draw_text(WIN_W as i32 / 2 - 8 * s.len() as i32, 220, s, TEXT)?;

        w.draw_text(24, WIN_H as i32 - 36,
                    "Esc or Q to quit.  Window [X] also quits.",
                    DIM)?;
        Ok(())
    }
}

fn draw_button(w: &Window, x: i32, y: i32, label: &str, hover: bool, pressed: bool) -> rsbr::Result<()> {
    let bg = if pressed { BTN_ACTIVE } else if hover { BTN_HOVER } else { BTN_BG };
    w.fill_rect(x, y, BTN_W, BTN_H, bg)?;
    w.draw_rect(x, y, BTN_W, BTN_H, 0x00607090)?;
    let tx = x + (BTN_W as i32 - 8 * label.len() as i32) / 2;
    let ty = y + (BTN_H as i32 - 16) / 2 + 2;
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