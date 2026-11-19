//! Wire types and constants matching `kernel/gui/gui.h` and
//! `kernel/gui/gui_syscalls.h`.
//!
//! Every struct passed through the GUI syscall ABI uses `#[repr(C)]` and
//! has a compile-time size assertion where its layout is fixed. Keep this
//! file synchronized with the kernel headers.

#![allow(non_upper_case_globals)]

// ===================================================================
// Colour
// ===================================================================

/// 24-bit colour, `0x00RRGGBB`. The top byte is reserved.
pub type Color = u32;

pub const fn rgb(r: u8, g: u8, b: u8) -> Color {
    ((r as u32) << 16) | ((g as u32) << 8) | (b as u32)
}

pub const BLACK: Color = 0x000000;
pub const WHITE: Color = 0xFFFFFF;
pub const RED: Color = 0xC04040;
pub const GREEN: Color = 0x4CAF50;
pub const BLUE: Color = 0x2F60C0;
pub const YELLOW: Color = 0xFFC107;
pub const GRAY: Color = 0x808080;
pub const DARK: Color = 0x303540;
pub const LIGHT: Color = 0xECEDF1;

/// Packs two i32 into one syscall register: `lo` in the low 32 bits,
/// `hi` in the high 32.
#[inline(always)]
pub const fn pack2(lo: i32, hi: i32) -> usize {
    (((hi as u32) as u64) << 32 | ((lo as u32) as u64)) as usize
}

// ===================================================================
// Op codes — MUST match kernel/gui/gui_syscalls.h
// ===================================================================

pub const OP_CREATE: u32 = 1;
pub const OP_DESTROY: u32 = 2;
pub const OP_SHOW: u32 = 3;
pub const OP_HIDE: u32 = 4;
pub const OP_MOVE: u32 = 5;
pub const OP_RESIZE: u32 = 6;
pub const OP_TITLE: u32 = 7;
pub const OP_FOCUS: u32 = 8;
pub const OP_MINIMIZE: u32 = 9;
pub const OP_MAXIMIZE: u32 = 10;
pub const OP_RESTORE: u32 = 11;
pub const OP_SNAP: u32 = 12;
pub const OP_CLEAR: u32 = 13;
pub const OP_FILL_RECT: u32 = 14;
pub const OP_DRAW_RECT: u32 = 15;
pub const OP_DRAW_LINE: u32 = 16;
pub const OP_DRAW_TEXT: u32 = 17;
pub const OP_DRAW_PIXEL: u32 = 18;
pub const OP_INVALIDATE: u32 = 19;
pub const OP_RENDER: u32 = 20;
pub const OP_SET_CURSOR: u32 = 21;
pub const OP_GET_SIZE: u32 = 22;
pub const OP_GET_POS: u32 = 23;
pub const OP_GET_RECT: u32 = 24;
pub const OP_SET_CLIPBOARD: u32 = 25;
pub const OP_GET_CLIPBOARD: u32 = 26;
pub const OP_BLIT: u32 = 27;
pub const OP_BLIT_ALPHA: u32 = 28;
pub const OP_ADD_ICON: u32 = 29;
pub const OP_REMOVE_ICON: u32 = 30;
pub const OP_NOTIFY: u32 = 31;
pub const OP_GET_FLAGS: u32 = 32;
pub const OP_LOWER: u32 = 33;
pub const OP_SET_FLAGS: u32 = 34;
pub const OP_GET_Z: u32 = 35;
pub const OP_SET_CAPTURE: u32 = 36;
pub const OP_GET_CAPTURE: u32 = 37;
pub const OP_GET_SCREEN: u32 = 38;
pub const OP_GET_FOCUSED: u32 = 39;
pub const OP_TOP_WINDOW: u32 = 40;
pub const OP_GET_MOUSE: u32 = 41;
pub const OP_INVAL_RECT: u32 = 42;
pub const OP_GET_PIXEL: u32 = 43;
pub const OP_FONT_INFO: u32 = 44;
pub const OP_SEND_DROP: u32 = 45;
pub const OP_TAKE_DROP: u32 = 46;
pub const OP_SET_PARENT: u32 = 47;
pub const OP_SET_HOLE: u32 = 48;
pub const OP_FRAME_BEGIN: u32 = 49;
pub const OP_FRAME_END: u32 = 50;

// ===================================================================
// Window flags — MUST match kernel/gui/gui.h (AG_WIN_* / GUI_WIN_*)
// ===================================================================

pub const FLAG_RESIZABLE: u32 = 0x001;
pub const FLAG_MOVABLE: u32 = 0x002;
pub const FLAG_HAS_TITLE: u32 = 0x004;
pub const FLAG_HAS_CLOSE: u32 = 0x008;
pub const FLAG_HAS_MINMAX: u32 = 0x010;
pub const FLAG_MODAL: u32 = 0x020;
pub const FLAG_NO_DECOR: u32 = 0x040;
pub const FLAG_ALWAYS_TOP: u32 = 0x080;
pub const FLAG_TOOL_WINDOW: u32 = 0x100;
pub const FLAG_BORDERLESS: u32 = 0x200;
pub const FLAG_PRESENTATION: u32 = 0x800;

// ===================================================================
// Event types — MUST match kernel/gui/gui.h
// ===================================================================

// gui_event_t.type is uint32_t, so the Rust constants use u32 as well.
pub const EVT_NONE: u32 = 0;
pub const EVT_MOUSE_MOVE: u32 = 1;
pub const EVT_MOUSE_DOWN: u32 = 2;
pub const EVT_MOUSE_UP: u32 = 3;
pub const EVT_MOUSE_DBLCLICK: u32 = 4;
pub const EVT_MOUSE_WHEEL: u32 = 5;
pub const EVT_MOUSE_RIGHT_DOWN: u32 = 6;
pub const EVT_MOUSE_RIGHT_UP: u32 = 7;
pub const EVT_MOUSE_MIDDLE_DOWN: u32 = 8;
pub const EVT_MOUSE_MIDDLE_UP: u32 = 9;
pub const EVT_KEY_DOWN: u32 = 10;
pub const EVT_KEY_UP: u32 = 11;
pub const EVT_FOCUS: u32 = 12;
pub const EVT_BLUR: u32 = 13;
pub const EVT_RESIZE: u32 = 14;
pub const EVT_CLOSE_REQ: u32 = 15;
pub const EVT_TIMER: u32 = 16;
pub const EVT_PAINT: u32 = 17;
pub const EVT_CONTEXT_MENU: u32 = 18;
pub const EVT_SNAP_CHANGED: u32 = 19;
pub const EVT_DROP: u32 = 20;
pub const EVT_ICON_CLICK: u32 = 21;

pub const KEY_LEFT: u32 = 0x100;
pub const KEY_RIGHT: u32 = 0x101;
pub const KEY_UP: u32 = 0x102;
pub const KEY_DOWN: u32 = 0x103;
pub const KEY_HOME: u32 = 0x104;
pub const KEY_END: u32 = 0x105;
pub const KEY_DELETE: u32 = 0x109;

pub const MOD_SHIFT: u8 = 1 << 0;
pub const MOD_CTRL: u8 = 1 << 1;
pub const MOD_ALT: u8 = 1 << 2;

// ===================================================================
// Event — exact mirror of kernel gui_event_t
// ===================================================================

/// A GUI event, matching the kernel's `gui_event_t` field-for-field.
///
/// C layout:
/// `uint32_t type; int32_t x, y; uint32_t key; uint8_t buttons;`
/// `uint8_t mods; uint16_t data;`
///
/// Pointer coordinates are client-content-relative. For mouse-wheel events,
/// the signed wheel delta is stored in `key` (reinterpret with `as i32`).
/// `buttons` is the current mouse-button bitmask, not a single button ID.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Event {
    pub kind: u32,
    pub x: i32,
    pub y: i32,
    pub key: u32,
    pub buttons: u8,
    pub mods: u8,
    pub data: u16,
}

const _: () = assert!(core::mem::size_of::<Event>() == 20);

impl Event {
    pub const fn is_mouse(&self) -> bool {
        matches!(
            self.kind,
            EVT_MOUSE_MOVE
                | EVT_MOUSE_DOWN
                | EVT_MOUSE_UP
                | EVT_MOUSE_DBLCLICK
                | EVT_MOUSE_WHEEL
                | EVT_MOUSE_RIGHT_DOWN
                | EVT_MOUSE_RIGHT_UP
                | EVT_MOUSE_MIDDLE_DOWN
                | EVT_MOUSE_MIDDLE_UP
                | EVT_CONTEXT_MENU
        )
    }

    pub const fn is_key(&self) -> bool {
        matches!(self.kind, EVT_KEY_DOWN | EVT_KEY_UP)
    }
}

// ===================================================================
// Enumerated values
// ===================================================================

#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Cursor {
    #[default]
    Arrow = 0,
    IBeam = 1,
    Hand = 2,
    HResize = 3,
    VResize = 4,
    DResize = 5,
    Wait = 6,
    Move = 7,
    Crosshair = 8,
    NotAllowed = 9,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Snap {
    #[default]
    None = 0,
    Left = 1,
    Right = 2,
    Top = 3,
    Bottom = 4,
    Maximized = 5,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct FontMetrics {
    pub width: u32,
    pub height: u32,
    pub ascent: u32,
    pub descent: u32,
    pub num_glyphs: u32,
}

// ===================================================================
// Theme — mirror of gui_theme_t
// ===================================================================

/// Mirror of `gui_theme_t` from `kernel/gui/gui.h`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Theme {
    pub desktop_top: u32,
    pub desktop_bot: u32,
    pub win_bg: u32,
    pub win_content: u32,
    pub title_active: u32,
    pub title_inactive: u32,
    pub title_text: u32,
    pub border: u32,
    pub border_active: u32,
    pub taskbar_bg: u32,
    pub taskbar_border: u32,
    pub taskbar_text: u32,
    pub start_btn_bg: u32,
    pub start_btn_text: u32,
    pub close_bg: u32,
    pub close_bg_hover: u32,
    pub max_bg: u32,
    pub min_bg: u32,
    pub icon_text: u32,
    pub icon_selected: u32,
    pub notif_bg: u32,
    pub notif_border: u32,
    pub notif_text: u32,
    pub shadow_color: u32,
    pub shadow_offset: i32,
    pub taskbar_h: u32,
    pub titlebar_h: u32,
    pub border_w: u32,
    pub resize_grip: u32,
    pub icon_size: u32,
    pub icon_pad: u32,
    pub win_round: u32,
    pub dpi: u32,
}

const _: () = assert!(core::mem::size_of::<Theme>() == 33 * 4);
