#![no_std]
#![no_main]

extern crate rsbr;

use core::{fmt::Write, ptr, slice, str};

use rsbr::fs::File;
use rsbr::gui::{
    self, App, Cursor, Event, Flow, Window,
    EVT_CLOSE_REQ, EVT_KEY_DOWN, EVT_MOUSE_DOWN, EVT_MOUSE_MOVE,
    EVT_MOUSE_WHEEL, EVT_RESIZE,
    FLAG_HAS_CLOSE, FLAG_HAS_MINMAX, FLAG_HAS_TITLE, FLAG_MOVABLE,
    FLAG_RESIZABLE,
    KEY_DELETE, KEY_DOWN, KEY_END, KEY_HOME, KEY_LEFT, KEY_RIGHT, KEY_UP,
    MOD_CTRL,
};

// =============================================================================
// Application configuration
// =============================================================================

const WINDOW_X: i32 = 72;
const WINDOW_Y: i32 = 54;
const WINDOW_WIDTH: i32 = 1120;
const WINDOW_HEIGHT: i32 = 760;

const MAX_PATH_BYTES: usize = 256;
const MAX_RECENT_FILES: usize = 8;
const MAX_DOCUMENT_BYTES: usize = 65_536;

const FONT_WIDTH: i32 = 8;
const LINE_HEIGHT: i32 = 18;
const GUTTER_WIDTH: i32 = 44;

// RSBR currently documents no public growable allocator/vector API. Keep the
// first editor buffer bounded and static rather than inventing an allocator
// interface. The buffer is isolated so it can be replaced with KVec later.
static mut DOCUMENT_BYTES: [u8; MAX_DOCUMENT_BYTES] = [0; MAX_DOCUMENT_BYTES];
static mut LOAD_BYTES: [u8; MAX_DOCUMENT_BYTES] = [0; MAX_DOCUMENT_BYTES];

// =============================================================================
// Theme (shared with GFiles)
// =============================================================================

#[derive(Clone, Copy)]
struct Theme {
    background: u32,
    sidebar: u32,
    header: u32,
    card: u32,
    card_hover: u32,
    card_selected: u32,
    card_border: u32,
    divider: u32,
    text: u32,
    muted: u32,
    accent: u32,
    accent_soft: u32,
    file: u32,
    file_line: u32,
}

impl Theme {
    const LIGHT: Self = Self {
        background: 0x00F5F7FA,
        sidebar: 0x00EDF1F6,
        header: 0x00FFFFFF,
        card: 0x00FFFFFF,
        card_hover: 0x00EAF2FF,
        card_selected: 0x00DCEAFF,
        card_border: 0x00D7DEE8,
        divider: 0x00D7DEE8,
        text: 0x00212B3A,
        muted: 0x006B7788,
        accent: 0x003D78D8,
        accent_soft: 0x00DCEAFF,
        file: 0x00FFFFFF,
        file_line: 0x0096A2B2,
    };
}

const THEME: Theme = Theme::LIGHT;

// =============================================================================
// Layout configuration
// =============================================================================

#[derive(Clone, Copy)]
struct LayoutConfig {
    sidebar_width: i32,
    header_height: i32,
    footer_height: i32,
    main_padding: i32,
    editor_top_padding: i32,
    action_top: i32,
    action_height: i32,
    action_gap: i32,
    recent_heading_y: i32,
    recent_top: i32,
    recent_step: i32,
}

impl LayoutConfig {
    const DEFAULT: Self = Self {
        sidebar_width: 208,
        header_height: 88,
        footer_height: 30,
        main_padding: 20,
        editor_top_padding: 16,
        action_top: 48,
        action_height: 32,
        action_gap: 8,
        recent_heading_y: 224,
        recent_top: 248,
        recent_step: 28,
    };
}

const LAYOUT_CONFIG: LayoutConfig = LayoutConfig::DEFAULT;

// =============================================================================
// Geometry
// =============================================================================

#[derive(Clone, Copy)]
struct Rect {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
}

impl Rect {
    const fn new(x: i32, y: i32, w: i32, h: i32) -> Self {
        Self { x, y, w, h }
    }

    fn right(&self) -> i32 {
        self.x.saturating_add(self.w)
    }

    fn bottom(&self) -> i32 {
        self.y.saturating_add(self.h)
    }

    fn contains(&self, x: i32, y: i32) -> bool {
        x >= self.x && x < self.right() && y >= self.y && y < self.bottom()
    }

    fn is_visible(&self) -> bool {
        self.w > 0 && self.h > 0
    }
}

#[derive(Clone, Copy)]
struct Layout {
    window: Rect,
    sidebar: Rect,
    header: Rect,
    path_box: Rect,
    editor: Rect,
    footer: Rect,
    actions: [Rect; 4],
    recent_area: Rect,
    visible_rows: usize,
    visible_columns: usize,
}

impl Layout {
    fn calculate(width: i32, height: i32, config: LayoutConfig) -> Self {
        let width = width.max(1);
        let height = height.max(1);
        let window = Rect::new(0, 0, width, height);

        let sidebar_width = config.sidebar_width.max(0).min(width);
        let footer_height = config.footer_height.max(0).min(height);
        let header_height = config.header_height.max(0).min(height);
        let sidebar = Rect::new(0, 0, sidebar_width, height);

        let main_x = sidebar.right();
        let main_width = width.saturating_sub(sidebar_width);
        let header = Rect::new(main_x, 0, main_width, header_height);
        let footer_y = height.saturating_sub(footer_height);
        let footer = Rect::new(main_x, footer_y, main_width, footer_height);

        let path_x = main_x + config.main_padding;
        let path_width = (main_width - config.main_padding * 2).max(0);
        let path_box = Rect::new(path_x, 46, path_width, 28);

        let editor_x = main_x + config.main_padding;
        let editor_y = header.bottom() + config.editor_top_padding;
        let editor_width = (main_width - config.main_padding * 2).max(0);
        let editor_height = (footer.y - editor_y - 12).max(0);
        let editor = Rect::new(editor_x, editor_y, editor_width, editor_height);

        let action_x = 16;
        let action_width = (sidebar_width - action_x * 2).max(0);
        let mut actions = [Rect::new(0, 0, 0, 0); 4];
        let mut index = 0;
        while index < actions.len() {
            actions[index] = Rect::new(
                action_x,
                config.action_top + index as i32 * (config.action_height + config.action_gap),
                action_width,
                config.action_height,
            );
            index += 1;
        }

        let recent_area = Rect::new(
            12,
            config.recent_top,
            (sidebar_width - 24).max(0),
            (height - config.recent_top - 44).max(0),
        );

        let text_width = (editor.w - GUTTER_WIDTH - 28).max(FONT_WIDTH);
        let text_height = (editor.h - 24).max(LINE_HEIGHT);
        let visible_columns = (text_width / FONT_WIDTH).max(1) as usize;
        let visible_rows = (text_height / LINE_HEIGHT).max(1) as usize;

        Self {
            window,
            sidebar,
            header,
            path_box,
            editor,
            footer,
            actions,
            recent_area,
            visible_rows,
            visible_columns,
        }
    }

    fn recent_item_rect(&self, index: usize) -> Rect {
        Rect::new(
            self.recent_area.x,
            self.recent_area.y + index as i32 * LAYOUT_CONFIG.recent_step,
            self.recent_area.w,
            LAYOUT_CONFIG.recent_step - 2,
        )
    }
}

// =============================================================================
// Fixed UTF-8 path field
// =============================================================================

#[derive(Clone, Copy)]
struct PathBuffer {
    bytes: [u8; MAX_PATH_BYTES],
    len: usize,
}

impl PathBuffer {
    const fn empty() -> Self {
        Self {
            bytes: [0; MAX_PATH_BYTES],
            len: 0,
        }
    }

    fn set(&mut self, value: &str) -> bool {
        let bytes = value.as_bytes();
        if bytes.is_empty() || bytes.len() >= MAX_PATH_BYTES || bytes.contains(&0) {
            return false;
        }

        self.bytes = [0; MAX_PATH_BYTES];
        self.bytes[..bytes.len()].copy_from_slice(bytes);
        self.len = bytes.len();
        true
    }

    fn clear(&mut self) {
        self.bytes = [0; MAX_PATH_BYTES];
        self.len = 0;
    }

    fn as_str(&self) -> &str {
        str::from_utf8(&self.bytes[..self.len]).unwrap_or("")
    }

    fn insert_ascii(&mut self, cursor: &mut usize, byte: u8) -> bool {
        if self.len + 1 >= MAX_PATH_BYTES || *cursor > self.len {
            return false;
        }

        let mut i = self.len;
        while i > *cursor {
            self.bytes[i] = self.bytes[i - 1];
            i -= 1;
        }
        self.bytes[*cursor] = byte;
        self.len += 1;
        *cursor += 1;
        self.bytes[self.len] = 0;
        true
    }

    fn backspace(&mut self, cursor: &mut usize) -> bool {
        if *cursor == 0 || *cursor > self.len {
            return false;
        }
        let mut i = *cursor - 1;
        while i < self.len - 1 {
            self.bytes[i] = self.bytes[i + 1];
            i += 1;
        }
        self.len -= 1;
        *cursor -= 1;
        self.bytes[self.len] = 0;
        true
    }

    fn delete(&mut self, cursor: usize) -> bool {
        if cursor >= self.len {
            return false;
        }
        let mut i = cursor;
        while i < self.len - 1 {
            self.bytes[i] = self.bytes[i + 1];
            i += 1;
        }
        self.len -= 1;
        self.bytes[self.len] = 0;
        true
    }

    fn basename(&self) -> &str {
        self.as_str().rsplit('/').next().unwrap_or(self.as_str())
    }
}

// =============================================================================
// Recent files
// =============================================================================

struct RecentFiles {
    items: [PathBuffer; MAX_RECENT_FILES],
    count: usize,
}

impl RecentFiles {
    const fn new() -> Self {
        Self {
            items: [PathBuffer::empty(); MAX_RECENT_FILES],
            count: 0,
        }
    }

    fn add(&mut self, path: &PathBuffer) {
        if path.len == 0 {
            return;
        }

        let mut found = None;
        for i in 0..self.count {
            if self.items[i].as_str() == path.as_str() {
                found = Some(i);
                break;
            }
        }

        if let Some(index) = found {
            for i in (1..=index).rev() {
                self.items[i] = self.items[i - 1];
            }
        } else {
            let last = self.count.min(MAX_RECENT_FILES - 1);
            for i in (1..=last).rev() {
                self.items[i] = self.items[i - 1];
            }
            if self.count < MAX_RECENT_FILES {
                self.count += 1;
            }
        }

        self.items[0] = *path;
    }

    fn get(&self, index: usize) -> Option<&PathBuffer> {
        if index < self.count {
            Some(&self.items[index])
        } else {
            None
        }
    }
}

// =============================================================================
// Small stack-backed formatter
// =============================================================================

struct SmallText<const N: usize> {
    bytes: [u8; N],
    len: usize,
}

impl<const N: usize> SmallText<N> {
    const fn new() -> Self {
        Self {
            bytes: [0; N],
            len: 0,
        }
    }

    fn as_str(&self) -> &str {
        str::from_utf8(&self.bytes[..self.len]).unwrap_or("")
    }
}

impl<const N: usize> Write for SmallText<N> {
    fn write_str(&mut self, value: &str) -> core::fmt::Result {
        let available = N.saturating_sub(self.len);
        let mut count = value.len().min(available);
        while count > 0 && !value.is_char_boundary(count) {
            count -= 1;
        }
        self.bytes[self.len..self.len + count].copy_from_slice(&value.as_bytes()[..count]);
        self.len += count;
        Ok(())
    }
}

// =============================================================================
// Editor application state
// =============================================================================

#[derive(Clone, Copy, PartialEq, Eq)]
enum Action {
    New,
    Open,
    Save,
    SaveAs,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum HoverTarget {
    None,
    Action(Action),
    Recent(usize),
    Path,
    Editor,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum FocusTarget {
    Editor,
    Path,
}

struct GEditApp {
    document_len: usize,
    cursor: usize,
    dirty: bool,
    top_line: usize,
    horizontal_column: usize,
    preferred_column: usize,
    preferred_column_valid: bool,

    current_path: PathBuffer,
    path_input: PathBuffer,
    path_cursor: usize,
    focus: FocusTarget,

    recent: RecentFiles,
    hovered: HoverTarget,
    viewport_width: i32,
    viewport_height: i32,
    status: &'static str,
}

impl GEditApp {
    fn new() -> Self {
        unsafe {
            ptr::write_bytes(ptr::addr_of_mut!(DOCUMENT_BYTES).cast::<u8>(), 0, MAX_DOCUMENT_BYTES);
            ptr::write_bytes(ptr::addr_of_mut!(LOAD_BYTES).cast::<u8>(), 0, MAX_DOCUMENT_BYTES);
        }

        let mut path_input = PathBuffer::empty();
        let _ = path_input.set("/tmp/notes.txt");

        Self {
            document_len: 0,
            cursor: 0,
            dirty: false,
            top_line: 0,
            horizontal_column: 0,
            preferred_column: 0,
            preferred_column_valid: false,
            current_path: PathBuffer::empty(),
            path_input,
            path_cursor: 14,
            focus: FocusTarget::Editor,
            recent: RecentFiles::new(),
            hovered: HoverTarget::None,
            viewport_width: WINDOW_WIDTH,
            viewport_height: WINDOW_HEIGHT,
            status: "Ready",
        }
    }

    fn document(&self) -> &[u8] {
        unsafe {
            slice::from_raw_parts(
                ptr::addr_of!(DOCUMENT_BYTES).cast::<u8>(),
                self.document_len,
            )
        }
    }

    fn document_mut(&mut self) -> &mut [u8; MAX_DOCUMENT_BYTES] {
        unsafe { &mut *ptr::addr_of_mut!(DOCUMENT_BYTES) }
    }

    fn layout(&self) -> Layout {
        Layout::calculate(self.viewport_width, self.viewport_height, LAYOUT_CONFIG)
    }

    fn line_count(&self) -> usize {
        1 + self.document().iter().filter(|&&byte| byte == b'\n').count()
    }

    fn line_start(&self, line: usize) -> usize {
        if line == 0 {
            return 0;
        }

        let bytes = self.document();
        let mut current = 0;
        for (index, byte) in bytes.iter().enumerate() {
            if *byte == b'\n' {
                current += 1;
                if current == line {
                    return index + 1;
                }
            }
        }
        self.document_len
    }

    fn line_end(&self, line: usize) -> usize {
        let start = self.line_start(line);
        let bytes = self.document();
        let mut index = start;
        while index < bytes.len() && bytes[index] != b'\n' {
            index += 1;
        }
        index
    }

    fn cursor_line(&self) -> usize {
        self.document()[..self.cursor.min(self.document_len)]
            .iter()
            .filter(|&&byte| byte == b'\n')
            .count()
    }

    fn cursor_column(&self) -> usize {
        let line = self.cursor_line();
        let start = self.line_start(line);
        let end = self.cursor.max(start).min(self.line_end(line));
        str::from_utf8(&self.document()[start..end])
            .map(|text| text.chars().count())
            .unwrap_or(end - start)
    }

    fn position_for_column(&self, line: usize, column: usize) -> usize {
        let start = self.line_start(line);
        let end = self.line_end(line);
        let text = match str::from_utf8(&self.document()[start..end]) {
            Ok(text) => text,
            Err(_) => return end,
        };

        let mut count = 0;
        for (offset, _) in text.char_indices() {
            if count == column {
                return start + offset;
            }
            count += 1;
        }
        end
    }

    fn previous_char_boundary(&self, position: usize) -> usize {
        if position == 0 {
            return 0;
        }
        let bytes = self.document();
        let mut index = position - 1;
        while index > 0 && (bytes[index] & 0xC0) == 0x80 {
            index -= 1;
        }
        index
    }

    fn next_char_boundary(&self, position: usize) -> usize {
        if position >= self.document_len {
            return self.document_len;
        }
        let first = self.document()[position];
        let width = if first < 0x80 { 1 } else if first & 0xE0 == 0xC0 { 2 } else if first & 0xF0 == 0xE0 { 3 } else { 4 };
        (position + width).min(self.document_len)
    }

    fn insert_bytes(&mut self, value: &[u8]) -> bool {
        if value.is_empty() {
            return false;
        }
        if self.document_len.saturating_add(value.len()) > MAX_DOCUMENT_BYTES {
            self.status = "Document limit reached";
            return true;
        }

        let position = self.cursor.min(self.document_len);
        let old_len = self.document_len;
        let new_len = old_len + value.len();
        let buffer = self.document_mut();
        buffer.copy_within(position..old_len, position + value.len());
        buffer[position..position + value.len()].copy_from_slice(value);

        self.document_len = new_len;
        self.cursor = position + value.len();
        self.dirty = true;
        self.preferred_column_valid = false;
        true
    }

    fn delete_range(&mut self, start: usize, end: usize) -> bool {
        if start >= end || end > self.document_len {
            return false;
        }

        let removed = end - start;
        let old_len = self.document_len;
        self.document_mut().copy_within(end..old_len, start);
        self.document_len -= removed;
        self.cursor = start;
        self.dirty = true;
        self.preferred_column_valid = false;
        true
    }

    fn insert_ascii(&mut self, byte: u8) -> bool {
        self.insert_bytes(&[byte])
    }

    fn insert_newline(&mut self) -> bool {
        self.insert_ascii(b'\n')
    }

    fn insert_tab(&mut self) -> bool {
        self.insert_bytes(b"    ")
    }

    fn backspace(&mut self) -> bool {
        if self.cursor == 0 {
            return false;
        }
        let previous = self.previous_char_boundary(self.cursor);
        self.delete_range(previous, self.cursor)
    }

    fn delete_forward(&mut self) -> bool {
        if self.cursor >= self.document_len {
            return false;
        }
        let next = self.next_char_boundary(self.cursor);
        self.delete_range(self.cursor, next)
    }

    fn move_horizontal(&mut self, right: bool) -> bool {
        let next = if right {
            self.next_char_boundary(self.cursor)
        } else {
            self.previous_char_boundary(self.cursor)
        };
        if next == self.cursor {
            return false;
        }
        self.cursor = next;
        self.preferred_column_valid = false;
        true
    }

    fn move_line_edge(&mut self, end: bool) -> bool {
        let line = self.cursor_line();
        let next = if end { self.line_end(line) } else { self.line_start(line) };
        if next == self.cursor {
            return false;
        }
        self.cursor = next;
        self.preferred_column_valid = false;
        true
    }

    fn move_vertical(&mut self, down: bool) -> bool {
        let line = self.cursor_line();
        let count = self.line_count();
        let target_line = if down {
            if line + 1 >= count { return false; }
            line + 1
        } else {
            if line == 0 { return false; }
            line - 1
        };

        if !self.preferred_column_valid {
            self.preferred_column = self.cursor_column();
            self.preferred_column_valid = true;
        }

        let next = self.position_for_column(target_line, self.preferred_column);
        if next == self.cursor {
            return false;
        }
        self.cursor = next;
        true
    }

    fn reset_document(&mut self) {
        self.document_len = 0;
        self.cursor = 0;
        self.dirty = false;
        self.top_line = 0;
        self.horizontal_column = 0;
        self.preferred_column_valid = false;
        self.current_path.clear();
        self.path_input.clear();
        self.path_cursor = 0;
        self.focus = FocusTarget::Editor;
        self.status = "New document";
    }

    fn add_recent(&mut self, path: &PathBuffer) {
        self.recent.add(path);
    }

    
    fn load_path(&mut self, path: &str) -> Result<(), &'static str> {
        rsbr::eprintln!(
            "[gedit][open] begin: path='{}', path_len={}, document_capacity={}",
            path,
            path.len(),
            MAX_DOCUMENT_BYTES
        );

        if path.is_empty() {
            rsbr::eprintln!("[gedit][open][error] empty path");
            return Err("Invalid path");
        }

        if path.as_bytes().contains(&0) {
            rsbr::eprintln!("[gedit][open][error] path contains NUL byte");
            return Err("Invalid path");
        }

        if path.len() >= MAX_PATH_BYTES {
            rsbr::eprintln!(
                "[gedit][open][error] path too long: {} >= {}",
                path.len(),
                MAX_PATH_BYTES
            );
            return Err("Invalid path");
        }

        let mut file = match File::open(path) {
            Ok(file) => {
                rsbr::eprintln!("[gedit][open] File::open succeeded");
                file
            }
            Err(error) => {
                match error.errno() {
                    Some(errno) => rsbr::eprintln!(
                        "[gedit][open][error] File::open failed: path='{}', errno={}",
                        path,
                        errno
                    ),
                    None => rsbr::eprintln!(
                        "[gedit][open][error] File::open failed: path='{}', no errno available",
                        path
                    ),
                }
                return Err("Open failed");
            }
        };

        let staging = unsafe { &mut *ptr::addr_of_mut!(LOAD_BYTES) };
        let mut length = 0usize;
        let mut total_read = 0usize;
        let mut truncated = false;
        let mut chunk = [0u8; 4096];

        loop {
            let read = match file.read(&mut chunk) {
                Ok(read) => read,
                Err(error) => {
                    match error.errno() {
                        Some(errno) => rsbr::eprintln!(
                            "[gedit][open][error] read failed: path='{}', total_read={}, buffered={}, errno={}",
                            path,
                            total_read,
                            length,
                            errno
                        ),
                        None => rsbr::eprintln!(
                            "[gedit][open][error] read failed: path='{}', total_read={}, buffered={}, no errno available",
                            path,
                            total_read,
                            length
                        ),
                    }
                    return Err("Read failed");
                }
            };

            if read == 0 {
                rsbr::eprintln!(
                    "[gedit][open] EOF: total_read={}, buffered={}",
                    total_read,
                    length
                );
                break;
            }

            total_read = total_read.saturating_add(read);

            for &byte in &chunk[..read] {
                if byte == b'\r' {
                    continue;
                }

                if byte == 0 {
                    rsbr::eprintln!(
                        "[gedit][open][error] NUL byte found: file_offset={}, buffered={}",
                        total_read - read,
                        length
                    );
                    return Err("File contains a NUL byte");
                }

                if byte == b'\t' {
                    if length.saturating_add(4) > MAX_DOCUMENT_BYTES {
                        truncated = true;
                        break;
                    }

                    staging[length..length + 4].copy_from_slice(b"    ");
                    length += 4;
                } else if length < MAX_DOCUMENT_BYTES {
                    staging[length] = byte;
                    length += 1;
                } else {
                    truncated = true;
                    break;
                }
            }

            if truncated {
                rsbr::eprintln!(
                    "[gedit][open] document limit reached: total_read={}, buffered={}, capacity={}",
                    total_read,
                    length,
                    MAX_DOCUMENT_BYTES
                );
                break;
            }
        }

        let valid_length = match str::from_utf8(&staging[..length]) {
            Ok(_) => {
                rsbr::eprintln!("[gedit][open] UTF-8 validation succeeded");
                length
            }
            Err(error) if truncated => {
                rsbr::eprintln!(
                    "[gedit][open] truncated UTF-8 tail: valid_up_to={}, error_len={:?}",
                    error.valid_up_to(),
                    error.error_len()
                );
                error.valid_up_to()
            }
            Err(error) => {
                rsbr::eprintln!(
                    "[gedit][open][error] invalid UTF-8: valid_up_to={}, error_len={:?}, buffered={}",
                    error.valid_up_to(),
                    error.error_len(),
                    length
                );
                return Err("File is not valid UTF-8");
            }
        };

        self.document_mut()[..valid_length]
            .copy_from_slice(&staging[..valid_length]);

        self.document_len = valid_length;
        self.cursor = 0;
        self.dirty = false;
        self.top_line = 0;
        self.horizontal_column = 0;
        self.preferred_column_valid = false;

        let mut stored_path = PathBuffer::empty();

        if !stored_path.set(path) {
            rsbr::eprintln!(
                "[gedit][open][error] could not store path after reading: '{}'",
                path
            );
            return Err("Path is too long");
        }

        self.current_path = stored_path;
        self.path_input = stored_path;
        self.path_cursor = self.path_input.len;
        self.focus = FocusTarget::Editor;
        self.add_recent(&stored_path);

        self.status = if truncated {
            "Loaded (document truncated)"
        } else {
            "File loaded"
        };

        rsbr::eprintln!(
            "[gedit][open] success: path='{}', file_bytes={}, document_bytes={}, truncated={}",
            path,
            total_read,
            valid_length,
            truncated
        );

        Ok(())
    }

    fn save_path(&mut self, path: &str) -> Result<(), &'static str> {
        if path.is_empty() || path.as_bytes().contains(&0) || path.len() >= MAX_PATH_BYTES {
            return Err("Invalid path");
        }

        write_document(path, self.document())?;

        let mut stored_path = PathBuffer::empty();
        if !stored_path.set(path) {
            return Err("Path is too long");
        }

        self.current_path = stored_path;
        self.path_input = stored_path;
        self.path_cursor = self.path_input.len;
        self.focus = FocusTarget::Editor;
        self.dirty = false;
        self.add_recent(&stored_path);
        self.status = "File saved";
        Ok(())
    }

    fn open_from_path_field(&mut self) {
        let path = self.path_input;

        rsbr::eprintln!(
            "[gedit][open] requested from path field: '{}'",
            path.as_str()
        );

        match self.load_path(path.as_str()) {
            Ok(()) => {
                rsbr::eprintln!("[gedit][open] path field operation completed");
            }
            Err(error) => {
                rsbr::eprintln!(
                    "[gedit][open][failed] path='{}', reason='{}'",
                    path.as_str(),
                    error
                );
                self.status = error;
            }
        }
    }

    
    fn save_document(&mut self, save_as: bool) {
        let path = if save_as || self.path_input.len > 0 {
            self.path_input
        } else {
            self.current_path
        };

        match self.save_path(path.as_str()) {
            Ok(()) => {}
            Err(error) => self.status = error,
        }
    }


    fn ensure_caret_visible(&mut self, layout: &Layout) {
        let line = self.cursor_line();
        let column = self.cursor_column();

        if line < self.top_line {
            self.top_line = line;
        } else if line >= self.top_line + layout.visible_rows {
            self.top_line = line + 1 - layout.visible_rows;
        }

        if column < self.horizontal_column {
            self.horizontal_column = column;
        } else if column >= self.horizontal_column + layout.visible_columns {
            self.horizontal_column = column + 1 - layout.visible_columns;
        }
    }

    fn position_at_line_column(&self, line: usize, column: usize) -> usize {
        self.position_for_column(line, column)
    }

    fn click_editor(&mut self, x: i32, y: i32, layout: &Layout) {
        self.focus = FocusTarget::Editor;
        let text_x = layout.editor.x + 12 + GUTTER_WIDTH;
        let text_y = layout.editor.y + 12;
        let row = ((y - text_y).max(0) / LINE_HEIGHT) as usize;
        let line = (self.top_line + row).min(self.line_count().saturating_sub(1));
        let column = self.horizontal_column
            + ((x - text_x).max(0) / FONT_WIDTH) as usize;
        self.cursor = self.position_at_line_column(line, column);
        self.preferred_column_valid = false;
        self.status = "Editing";
        self.ensure_caret_visible(layout);
    }

    fn click_path_field(&mut self, x: i32, layout: &Layout) {
        self.focus = FocusTarget::Path;
        let text_x = layout.path_box.x + 10;
        let column = ((x - text_x).max(0) / FONT_WIDTH) as usize;
        self.path_cursor = column.min(self.path_input.len);
    }

    fn path_key(&mut self, key: u32) -> bool {
        match key {
            KEY_LEFT => {
                if self.path_cursor > 0 {
                    self.path_cursor -= 1;
                    return true;
                }
            }
            KEY_RIGHT => {
                if self.path_cursor < self.path_input.len {
                    self.path_cursor += 1;
                    return true;
                }
            }
            KEY_HOME => {
                self.path_cursor = 0;
                return true;
            }
            KEY_END => {
                self.path_cursor = self.path_input.len;
                return true;
            }
            KEY_DELETE => return self.path_input.delete(self.path_cursor),
            0x08 => return self.path_input.backspace(&mut self.path_cursor),
            _ if (0x20..=0x7E).contains(&key) => {
                return self.path_input.insert_ascii(&mut self.path_cursor, key as u8);
            }
            _ => {}
        }
        false
    }

    fn editor_key(&mut self, key: u32) -> bool {
        match key {
            KEY_LEFT => self.move_horizontal(false),
            KEY_RIGHT => self.move_horizontal(true),
            KEY_UP => self.move_vertical(false),
            KEY_DOWN => self.move_vertical(true),
            KEY_HOME => self.move_line_edge(false),
            KEY_END => self.move_line_edge(true),
            KEY_DELETE => self.delete_forward(),
            0x08 => self.backspace(),
            0x0A | 0x0D => self.insert_newline(),
            0x09 => self.insert_tab(),
            _ if (0x20..=0x7E).contains(&key) => self.insert_ascii(key as u8),
            _ => false,
        }
    }

    fn action_at(&self, x: i32, y: i32, layout: &Layout) -> Option<Action> {
        for (index, rect) in layout.actions.iter().enumerate() {
            if rect.contains(x, y) {
                return Some(match index {
                    0 => Action::New,
                    1 => Action::Open,
                    2 => Action::Save,
                    _ => Action::SaveAs,
                });
            }
        }
        None
    }

    fn target_at(&self, x: i32, y: i32, layout: &Layout) -> HoverTarget {
        if let Some(action) = self.action_at(x, y, layout) {
            return HoverTarget::Action(action);
        }

        for index in 0..self.recent.count {
            if layout.recent_item_rect(index).contains(x, y) {
                return HoverTarget::Recent(index);
            }
        }

        if layout.path_box.contains(x, y) {
            return HoverTarget::Path;
        }
        if layout.editor.contains(x, y) {
            return HoverTarget::Editor;
        }
        HoverTarget::None
    }

    fn set_pointer(&self, target: HoverTarget) {
        let cursor = match target {
            HoverTarget::Action(_) | HoverTarget::Recent(_) => Cursor::Hand,
            HoverTarget::Path | HoverTarget::Editor => Cursor::IBeam,
            HoverTarget::None => Cursor::Arrow,
        };
        let _ = gui::set_cursor(cursor);
    }

    fn activate_action(&mut self, action: Action) {
        match action {
            Action::New => self.reset_document(),
            Action::Open => self.open_from_path_field(),
            Action::Save => self.save_document(false),
            Action::SaveAs => self.save_document(true),
        }
    }

    fn open_recent(&mut self, index: usize) {
        let path = match self.recent.get(index) {
            Some(path) => *path,
            None => return,
        };
        if let Err(error) = self.load_path(path.as_str()) {
            self.status = error;
        }
    }

    fn pointer_click(&mut self, x: i32, y: i32) -> Flow {
        let layout = self.layout();

        if let Some(action) = self.action_at(x, y, &layout) {
            self.activate_action(action);
            self.ensure_caret_visible(&layout);
            return Flow::Changed;
        }

        for index in 0..self.recent.count {
            if layout.recent_item_rect(index).contains(x, y) {
                self.open_recent(index);
                self.ensure_caret_visible(&layout);
                return Flow::Changed;
            }
        }

        if layout.path_box.contains(x, y) {
            self.click_path_field(x, &layout);
            return Flow::Changed;
        }

        if layout.editor.contains(x, y) {
            self.click_editor(x, y, &layout);
            self.set_pointer(HoverTarget::Editor);
            return Flow::Changed;
        }

        Flow::Ignore
    }

    fn pointer_move(&mut self, x: i32, y: i32) -> Flow {
        let target = self.target_at(x, y, &self.layout());
        if target == self.hovered {
            return Flow::Ignore;
        }
        self.hovered = target;
        self.set_pointer(target);
        Flow::Changed
    }

    fn scroll(&mut self, x: i32, y: i32, delta: i32) -> Flow {
        if delta == 0 {
            return Flow::Ignore;
        }
        let layout = self.layout();
        if !layout.editor.contains(x, y) {
            return Flow::Ignore;
        }

        let previous = self.top_line;
        let max_top = self.line_count().saturating_sub(layout.visible_rows);
        if delta > 0 {
            self.top_line = self.top_line.saturating_sub(1);
        } else {
            self.top_line = (self.top_line + 1).min(max_top);
        }

        if previous != self.top_line {
            Flow::Changed
        } else {
            Flow::Ignore
        }
    }
}

// =============================================================================
// File writing
// =============================================================================

#[cfg(target_arch = "x86_64")]
fn write_document(path: &str, bytes: &[u8]) -> Result<(), &'static str> {
    use rsbr::fs::flags::{O_CREAT, O_TRUNC, O_WRONLY};

    let mut file = File::open_with(path, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
        .map_err(|_| "Save failed: cannot open file")?;

    file.write_all(bytes)
        .map_err(|_| "Save failed: write error")?;

    file.sync()
        .map_err(|_| "Save failed: sync error")?;

    rsbr::eprintln!(
        "[gedit] File saved: path='{}', size={} bytes",
        path,
        bytes.len()
    );

    Ok(())
}

#[cfg(not(target_arch = "x86_64"))]
fn write_document(_path: &str, _bytes: &[u8]) -> Result<(), &'static str> {
    Err("Saving is not exposed by the current target FS API")
}

// =============================================================================
// RSBR application interface
// =============================================================================

impl App for GEditApp {
    fn update(&mut self, ev: &Event) -> Flow {
        match ev.kind {
            EVT_CLOSE_REQ => Flow::Quit,

            EVT_RESIZE => {
                self.viewport_width = ev.x.max(1);
                self.viewport_height = ev.y.max(1);
                let layout = self.layout();
                self.ensure_caret_visible(&layout);
                Flow::Ignore
            }

            EVT_MOUSE_MOVE => self.pointer_move(ev.x, ev.y),
            EVT_MOUSE_DOWN => self.pointer_click(ev.x, ev.y),
            EVT_MOUSE_WHEEL => self.scroll(ev.x, ev.y, ev.key as i32),

            EVT_KEY_DOWN => {
                if ev.key == 0x1B {
                    return Flow::Quit;
                }

                if ev.mods & MOD_CTRL != 0 {
                    let key = ev.key as u8;
                    match key.to_ascii_lowercase() {
                        b's' => {
                            self.save_document(false);
                            return Flow::Changed;
                        }
                        b'o' => {
                            self.open_from_path_field();
                            return Flow::Changed;
                        }
                        b'n' => {
                            self.reset_document();
                            return Flow::Changed;
                        }
                        _ => return Flow::Ignore,
                    }
                }

                if self.focus == FocusTarget::Path && (ev.key == 0x0A || ev.key == 0x0D) {
                    self.open_from_path_field();
                    return Flow::Changed;
                }

                let changed = match self.focus {
                    FocusTarget::Editor => self.editor_key(ev.key),
                    FocusTarget::Path => self.path_key(ev.key),
                };

                if changed {
                    let layout = self.layout();
                    self.ensure_caret_visible(&layout);
                    Flow::Changed
                } else {
                    Flow::Ignore
                }
            }

            _ => Flow::Ignore,
        }
    }

    fn render(&self, window: &Window) -> rsbr::Result<()> {
        let (width, height) = window.size()?;
        let layout = Layout::calculate(width as i32, height as i32, LAYOUT_CONFIG);
        Renderer::render(window, self, &layout)
    }
}

// =============================================================================
// Renderer
// =============================================================================

struct Renderer;

impl Renderer {
    fn fill_rect(window: &Window, rect: Rect, color: u32) -> rsbr::Result<()> {
        if !rect.is_visible() {
            return Ok(());
        }
        window.fill_rect(rect.x, rect.y, rect.w as u32, rect.h as u32, color)
    }

    fn render(window: &Window, app: &GEditApp, layout: &Layout) -> rsbr::Result<()> {
        window.clear(THEME.background)?;
        Self::draw_sidebar(window, app, layout)?;
        Self::draw_header(window, app, layout)?;
        Self::draw_editor(window, app, layout)?;
        Self::draw_footer(window, app, layout)?;
        Ok(())
    }

    fn draw_sidebar(window: &Window, app: &GEditApp, layout: &Layout) -> rsbr::Result<()> {
        Self::fill_rect(window, layout.sidebar, THEME.sidebar)?;
        Self::fill_rect(
            window,
            Rect::new(layout.sidebar.right(), 0, 1, layout.window.h),
            THEME.divider,
        )?;

        window.draw_text(20, 22, "ACTIONS", THEME.muted)?;

        let labels = ["NEW", "OPEN", "SAVE", "SAVE AS"];
        for index in 0..layout.actions.len() {
            let action = match index {
                0 => Action::New,
                1 => Action::Open,
                2 => Action::Save,
                _ => Action::SaveAs,
            };
            let rect = layout.actions[index];
            let hovered = app.hovered == HoverTarget::Action(action);
            let background = if hovered { THEME.card_hover } else { THEME.card };

            Self::fill_rect(window, rect, background)?;
            window.draw_rect(
                rect.x,
                rect.y,
                rect.w as u32,
                rect.h as u32,
                if hovered { THEME.accent } else { THEME.card_border },
            )?;

            let label = labels[index];
            let text_x = rect.x + ((rect.w - label.len() as i32 * FONT_WIDTH) / 2).max(6);
            window.draw_text(
                text_x,
                rect.y + 9,
                label,
                if hovered { THEME.accent } else { THEME.text },
            )?;
        }

        window.draw_text(20, LAYOUT_CONFIG.recent_heading_y, "RECENT FILES", THEME.muted)?;
        for index in 0..app.recent.count {
            let rect = layout.recent_item_rect(index);
            let path = match app.recent.get(index) {
                Some(path) => path,
                None => continue,
            };
            let selected = app.current_path.as_str() == path.as_str();
            let hovered = app.hovered == HoverTarget::Recent(index);
            if selected || hovered {
                Self::fill_rect(
                    window,
                    rect,
                    if selected { THEME.card_selected } else { THEME.card_hover },
                )?;
            }
            let label = path.basename();
            let label = Self::short_label(label, 20);
            window.draw_text(
                rect.x + 8,
                rect.y + 8,
                label,
                if selected || hovered { THEME.accent } else { THEME.text },
            )?;
        }

        let status_y = (layout.window.h - 24).max(LAYOUT_CONFIG.recent_heading_y + 20);
        let status = Self::short_label(app.status, 22);
        window.draw_text(20, status_y, status, THEME.muted)?;
        Ok(())
    }

    fn draw_header(window: &Window, app: &GEditApp, layout: &Layout) -> rsbr::Result<()> {
        Self::fill_rect(window, layout.header, THEME.header)?;
        Self::fill_rect(
            window,
            Rect::new(layout.header.x, layout.header.bottom() - 1, layout.header.w, 1),
            THEME.divider,
        )?;

        let title = if app.current_path.len == 0 {
            "UNTITLED"
        } else {
            app.current_path.basename()
        };
        window.draw_text(layout.header.x + 20, 17, title, THEME.text)?;

        let path_hovered = app.hovered == HoverTarget::Path;
        let path_focused = app.focus == FocusTarget::Path;
        Self::fill_rect(
            window,
            layout.path_box,
            if path_focused { THEME.accent_soft } else { THEME.sidebar },
        )?;
        window.draw_rect(
            layout.path_box.x,
            layout.path_box.y,
            layout.path_box.w as u32,
            layout.path_box.h as u32,
            if path_focused || path_hovered { THEME.accent } else { THEME.card_border },
        )?;
        window.draw_text(
            layout.path_box.x + 10,
            layout.path_box.y + 9,
            app.path_input.as_str(),
            if path_focused { THEME.text } else { THEME.muted },
        )?;

        if path_focused {
            let caret_x = layout.path_box.x + 10 + app.path_cursor as i32 * FONT_WIDTH;
            window.draw_line(
                caret_x,
                layout.path_box.y + 6,
                caret_x,
                layout.path_box.bottom() - 6,
                THEME.accent,
            )?;
        }

        Ok(())
    }

    fn draw_editor(window: &Window, app: &GEditApp, layout: &Layout) -> rsbr::Result<()> {
        let editor = layout.editor;
        Self::fill_rect(window, editor, THEME.file)?;
        window.draw_rect(
            editor.x,
            editor.y,
            editor.w as u32,
            editor.h as u32,
            if app.focus == FocusTarget::Editor { THEME.accent } else { THEME.card_border },
        )?;

        let bytes = app.document();
        let first_line = app.top_line;
        let line_count = app.line_count();
        let text_x = editor.x + 12 + GUTTER_WIDTH;
        let text_y = editor.y + 12;
        let cursor_line = app.cursor_line();
        let cursor_column = app.cursor_column();

        for visible_row in 0..layout.visible_rows {
            let line = first_line + visible_row;
            if line >= line_count {
                break;
            }

            let start = app.line_start(line);
            let end = app.line_end(line);
            let line_text = str::from_utf8(&bytes[start..end]).unwrap_or("");
            let visible_text = Self::slice_chars(line_text, app.horizontal_column, layout.visible_columns);
            let y = text_y + visible_row as i32 * LINE_HEIGHT;

            let mut number = SmallText::<16>::new();
            let _ = core::write!(&mut number, "{}", line + 1);
            window.draw_text(editor.x + 8, y, number.as_str(), THEME.file_line)?;
            window.draw_text(
                text_x,
                y,
                visible_text,
                if line == cursor_line { THEME.text } else { THEME.muted },
            )?;

            if line == cursor_line && app.focus == FocusTarget::Editor {
                let caret_column = cursor_column.saturating_sub(app.horizontal_column);
                let caret_x = text_x + (caret_column as i32 * FONT_WIDTH);
                window.draw_line(
                    caret_x,
                    y - 1,
                    caret_x,
                    y + LINE_HEIGHT - 4,
                    THEME.accent,
                )?;
            }
        }

        Ok(())
    }

    fn draw_footer(window: &Window, app: &GEditApp, layout: &Layout) -> rsbr::Result<()> {
        if !layout.footer.is_visible() {
            return Ok(());
        }
        Self::fill_rect(
            window,
            Rect::new(layout.footer.x, layout.footer.y, layout.footer.w, 1),
            THEME.divider,
        )?;

        let mut left = SmallText::<64>::new();
        let _ = core::write!(
            &mut left,
            "Ln {}, Col {}{}",
            app.cursor_line() + 1,
            app.cursor_column() + 1,
            if app.dirty { "  *" } else { "" },
        );
        window.draw_text(layout.footer.x + 16, layout.footer.y + 9, left.as_str(), THEME.muted)?;

        let mut right = SmallText::<64>::new();
        let _ = core::write!(&mut right, "UTF-8  |  {} B", app.document_len);
        let right_width = right.as_str().len() as i32 * FONT_WIDTH;
        let right_x = (layout.footer.right() - right_width - 16).max(layout.footer.x + 16);
        window.draw_text(right_x, layout.footer.y + 9, right.as_str(), THEME.muted)?;
        Ok(())
    }

    fn short_label(value: &str, max_chars: usize) -> &str {
        if value.chars().count() <= max_chars {
            return value;
        }
        let mut end = 0;
        for (index, (offset, _)) in value.char_indices().enumerate() {
            if index == max_chars.saturating_sub(3) {
                end = offset;
                break;
            }
        }
        if end == 0 {
            return value;
        }
        // Keep the label bounded without allocating a temporary String.
        &value[..end]
    }

    fn slice_chars(value: &str, start: usize, maximum: usize) -> &str {
        let mut start_byte = value.len();
        let mut end_byte = value.len();
        let mut seen = 0usize;
        let mut taken = 0usize;

        for (offset, ch) in value.char_indices() {
            if seen == start {
                start_byte = offset;
                end_byte = offset;
            }
            if seen >= start && taken < maximum {
                end_byte = offset + ch.len_utf8();
                taken += 1;
            }
            seen += 1;
        }

        if start >= seen {
            return "";
        }
        if taken == 0 {
            return "";
        }
        &value[start_byte..end_byte]
    }
}

// =============================================================================
// Entry point
// =============================================================================

#[no_mangle]
pub extern "C" fn main() -> i32 {
    let window = match Window::create(
        WINDOW_X,
        WINDOW_Y,
        WINDOW_WIDTH as u32,
        WINDOW_HEIGHT as u32,
        Some("gedit.rs"),
        FLAG_HAS_TITLE
            | FLAG_HAS_CLOSE
            | FLAG_HAS_MINMAX
            | FLAG_RESIZABLE
            | FLAG_MOVABLE,
    ) {
        Ok(window) => window,
        Err(_) => return 1,
    };

    if window.show().is_err() {
        return 1;
    }

    let app = GEditApp::new();
    match gui::run(app, window) {
        Ok(()) => 0,
        Err(_) => 1,
    }
}