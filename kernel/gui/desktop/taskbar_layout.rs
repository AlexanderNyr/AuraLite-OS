//! Pure taskbar and overflow-list geometry, ported from taskbar_layout.h.

pub const TB_START_X: u32 = 4;
pub const TB_START_W: u32 = 90;
pub const TB_GAP: u32 = 4;
pub const TB_BTN_MAX: u32 = 130;
pub const TB_BTN_MIN: u32 = 44;
pub const TB_TRAY_W: u32 = 98;
pub const TB_STRIP_X: u32 = TB_START_X + TB_START_W + 8;

pub const TB_OV_ROW_H: u32 = 18;
pub const TB_OV_PAD: u32 = 6;
pub const TB_OV_W: u32 = 170;
pub const TB_OV_ROWS_MAX: u32 = 12;

#[derive(Copy, Clone, Default)]
pub struct Layout {
    pub x0: u32,
    pub btn_w: u32,
    pub visible: u32,
    pub more_w: u32,
    pub more_x: u32,
    pub end: u32,
}

#[derive(Copy, Clone, Default)]
pub struct Overflow {
    pub x: i32,
    pub y: i32,
    pub w: u32,
    pub h: u32,
    pub rows: u32,
    pub row_h: u32,
    pub hidden: u32,
    pub per_page: u32,
    pub paged: bool,
}

#[inline]
pub fn strip_end(framebuffer_width: u32) -> u32 {
    let right = TB_TRAY_W + 8;
    if framebuffer_width > right {
        framebuffer_width - right
    } else {
        TB_STRIP_X
    }
}

#[inline]
pub fn button_width(framebuffer_width: u32, count: u32) -> u32 {
    let end = strip_end(framebuffer_width);
    if count == 0 {
        return TB_BTN_MAX;
    }
    if end <= TB_STRIP_X {
        return TB_BTN_MIN;
    }
    let span = end - TB_STRIP_X;
    let per = span / count;
    let width = per.saturating_sub(TB_GAP);
    width.clamp(TB_BTN_MIN, TB_BTN_MAX)
}

#[inline]
pub fn capacity(framebuffer_width: u32) -> u32 {
    let end = strip_end(framebuffer_width);
    if end <= TB_STRIP_X {
        return 0;
    }
    (end - TB_STRIP_X + TB_GAP) / (TB_BTN_MIN + TB_GAP)
}

pub fn layout(framebuffer_width: u32, count: u32) -> Layout {
    let end = strip_end(framebuffer_width);
    let width = button_width(framebuffer_width, count);
    let span = end.saturating_sub(TB_STRIP_X);

    let mut result = Layout {
        x0: TB_STRIP_X,
        end,
        btn_w: width,
        more_w: 0,
        more_x: 0,
        visible: 0,
    };

    if count == 0 {
        return result;
    }

    if count.saturating_mul(width + TB_GAP) <= span.saturating_add(TB_GAP) {
        result.visible = count;
        return result;
    }

    let cap = capacity(framebuffer_width);
    if cap == 0 {
        result.more_w = width;
        result.more_x = TB_STRIP_X;
        return result;
    }

    result.visible = cap - 1;
    result.more_w = width;
    result.more_x = TB_STRIP_X + result.visible * (width + TB_GAP);
    result
}

pub fn button_rect(
    framebuffer_width: u32,
    count: u32,
    index: u32,
) -> Option<(u32, u32)> {
    let current = layout(framebuffer_width, count);
    if index >= current.visible {
        return None;
    }
    let x = current.x0 + index * (current.btn_w + TB_GAP);
    if x + current.btn_w > current.end {
        return None;
    }
    Some((x, current.btn_w))
}

pub fn overflow_geometry(
    framebuffer_width: u32,
    framebuffer_height: u32,
    taskbar_height: u32,
    count: u32,
) -> Overflow {
    let current = layout(framebuffer_width, count);
    let hidden = count.saturating_sub(current.visible);
    let room = framebuffer_height.saturating_sub(taskbar_height + TB_OV_PAD + 8);
    let room_rows = (room / TB_OV_ROW_H).min(TB_OV_ROWS_MAX);
    let rows = hidden.min(room_rows);
    let paged = hidden > rows;

    let mut x = current.more_x as i32;
    if x + TB_OV_W as i32 > framebuffer_width as i32 {
        x = framebuffer_width as i32 - TB_OV_W as i32;
    }
    if x < TB_START_X as i32 {
        x = TB_START_X as i32;
    }

    let height = rows * TB_OV_ROW_H + 2 * TB_OV_PAD;
    let mut result = Overflow {
        x,
        y: framebuffer_height as i32 - taskbar_height as i32 - height as i32,
        w: TB_OV_W,
        h: height,
        rows,
        row_h: TB_OV_ROW_H,
        hidden,
        per_page: hidden,
        paged,
    };

    if rows == 0 {
        result.paged = false;
        result.h = 0;
    }
    result
}

/// Hidden-list index for a row; -1 means the paging row or an invalid row.
pub fn overflow_row_entry(geometry: &Overflow, page: u32, row: u32) -> i32 {
    if row >= geometry.rows {
        return -1;
    }
    if geometry.paged {
        if row + 1 >= geometry.rows {
            return -1;
        }
        let per_page = geometry.rows - 1;
        if per_page == 0 || geometry.hidden == 0 {
            return -1;
        }
        let start = page.saturating_mul(per_page) % geometry.hidden;
        let mut index = start + row;
        if index >= geometry.hidden {
            index -= geometry.hidden;
        }
        return index as i32;
    }
    row as i32
}

pub fn overflow_next_page(geometry: &Overflow, page: u32) -> u32 {
    let per_page = geometry.rows.saturating_sub(1).max(1);
    let pages = geometry.hidden.saturating_add(per_page - 1) / per_page;
    if pages == 0 {
        0
    } else {
        (page + 1) % pages
    }
}
